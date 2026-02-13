/*
 * tlcs_diag — diagnostic tool for TLCS quality decomposition.
 *
 * Usage: tlcs_diag <input.wav> <output_prefix>
 *
 * Outputs:
 *   <prefix>_full_recon.wav   — normal encode/decode reconstruction
 *   <prefix>_pitch_only.wav   — only pitch excitation through 1/A(z) + de-emphasis
 *   <prefix>_cb_only.wav      — only codebook excitation through 1/A(z) + de-emphasis
 *   <prefix>_residual.wav     — raw LPC residual (no synthesis)
 *   <prefix>_lpc_resynth.wav  — resynthesis with unquantized LPC (isolates LSF quant loss)
 *   <prefix>_metrics.csv      — per-frame diagnostic metrics
 */

#include "tlcs/tlcs.h"
#include "tlcs/tlcs_wav.h"
#include "../src/codec/tlcs_diag_encode.h"
#include "../src/codec/tlcs_lpc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* Synthesize excitation through 1/A(z) + de-emphasis, output to int16 */
static void synth_component(const float *exc, const float *a, int32_t order,
                             int32_t n, float *synth_mem, float *deemph_mem,
                             int16_t *pcm_out)
{
    float synth[TLCS_MAX_FRAME_SIZE];
    tlcs_synthesis_filter(a, order, exc, synth, n, synth_mem);
    tlcs_deemph(synth, n, deemph_mem);

    for (int32_t i = 0; i < n; i++) {
        float v = synth[i];
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        pcm_out[i] = (int16_t)v;
    }
}

static char *make_path(const char *prefix, const char *suffix)
{
    size_t len = strlen(prefix) + strlen(suffix) + 1;
    char *path = malloc(len);
    if (!path) return NULL;
    snprintf(path, len, "%s%s", prefix, suffix);
    return path;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: tlcs_diag <input.wav> <output_prefix>\n");
        return 1;
    }

    const char *in_path = argv[1];
    const char *prefix  = argv[2];

    /* Open input WAV */
    tlcs_wav wav_in;
    if (tlcs_wav_open_read(&wav_in, in_path) != TLCS_OK) {
        fprintf(stderr, "Error: cannot open %s (must be 16-bit mono WAV)\n", in_path);
        return 1;
    }

    fprintf(stderr, "Input: %s, %d Hz, %d samples\n",
            in_path, wav_in.sample_rate, wav_in.num_samples);

    /* Configure codec */
    tlcs_config cfg;
    if (tlcs_config_init(&cfg, wav_in.sample_rate, 8000) != TLCS_OK) {
        fprintf(stderr, "Error: unsupported sample rate %d\n", wav_in.sample_rate);
        tlcs_wav_close(&wav_in);
        return 1;
    }

    int32_t n = cfg.frame_size;
    int32_t order = cfg.lpc_order;

    /* Init encoder */
    tlcs_encoder enc;
    tlcs_encoder_init(&enc, &cfg);

    /* Open output WAVs */
    char *path_full    = make_path(prefix, "_full_recon.wav");
    char *path_pitch   = make_path(prefix, "_pitch_only.wav");
    char *path_cb      = make_path(prefix, "_cb_only.wav");
    char *path_res     = make_path(prefix, "_residual.wav");
    char *path_lpc     = make_path(prefix, "_lpc_resynth.wav");
    char *path_csv     = make_path(prefix, "_metrics.csv");

    if (!path_full || !path_pitch || !path_cb || !path_res || !path_lpc || !path_csv) {
        fprintf(stderr, "Error: memory allocation failed\n");
        return 1;
    }

    tlcs_wav wav_full, wav_pitch, wav_cb, wav_res, wav_lpc;
    if (tlcs_wav_open_write(&wav_full, path_full, wav_in.sample_rate) != TLCS_OK ||
        tlcs_wav_open_write(&wav_pitch, path_pitch, wav_in.sample_rate) != TLCS_OK ||
        tlcs_wav_open_write(&wav_cb, path_cb, wav_in.sample_rate) != TLCS_OK ||
        tlcs_wav_open_write(&wav_res, path_res, wav_in.sample_rate) != TLCS_OK ||
        tlcs_wav_open_write(&wav_lpc, path_lpc, wav_in.sample_rate) != TLCS_OK) {
        fprintf(stderr, "Error: cannot create output WAV files\n");
        return 1;
    }

    FILE *fp_csv = fopen(path_csv, "w");
    if (!fp_csv) {
        fprintf(stderr, "Error: cannot create %s\n", path_csv);
        return 1;
    }

    /* CSV header */
    fprintf(fp_csv, "frame,lsf_sd_db,voicing,"
                    "pitch_gain0,pitch_gain1,pitch_gain2,pitch_gain3,"
                    "pitch_gain_uq0,pitch_gain_uq1,pitch_gain_uq2,pitch_gain_uq3,"
                    "pitch_lag0,pitch_lag1,pitch_lag2,pitch_lag3,"
                    "residual_energy,pitch_energy,cb_energy,error_energy,"
                    "pitch_frac,cb_frac,error_frac,"
                    "cb_gain0,cb_gain1,cb_gain2,cb_gain3,"
                    "cb_gain_uq0,cb_gain_uq1,cb_gain_uq2,cb_gain_uq3\n");

    /* Synthesis filter states — one per component stream */
    float full_synth_mem[TLCS_LPC_ORDER_MAX] = {0};
    float full_deemph_mem = 0.0f;

    float pitch_synth_mem[TLCS_LPC_ORDER_MAX] = {0};
    float pitch_deemph_mem = 0.0f;

    float cb_synth_mem[TLCS_LPC_ORDER_MAX] = {0};
    float cb_deemph_mem = 0.0f;

    float lpc_synth_mem[TLCS_LPC_ORDER_MAX] = {0};
    float lpc_deemph_mem = 0.0f;

    /* Process frame by frame */
    int16_t pcm_buf[TLCS_MAX_FRAME_SIZE];
    uint8_t bs_buf[TLCS_MAX_FRAME_BYTES];
    int32_t total_frames = 0;

    for (;;) {
        int32_t samples_read = 0;
        tlcs_wav_read(&wav_in, pcm_buf, cfg.frame_size, &samples_read);
        if (samples_read <= 0) break;

        /* Zero-pad last frame */
        if (samples_read < cfg.frame_size) {
            for (int32_t i = samples_read; i < cfg.frame_size; i++)
                pcm_buf[i] = 0;
        }

        /* Diagnostic encode */
        int32_t bytes_written = 0;
        tlcs_diag_output diag;
        tlcs_status st = tlcs_diag_encode(&enc, pcm_buf, bs_buf, &bytes_written, &diag);
        if (st != TLCS_OK) {
            fprintf(stderr, "Encode error at frame %d\n", total_frames);
            break;
        }

        /* 1. Full reconstruction: full_exc through 1/A_q(z) + de-emphasis */
        int16_t pcm_full[TLCS_MAX_FRAME_SIZE];
        synth_component(diag.full_exc, diag.a_quant, order, n,
                        full_synth_mem, &full_deemph_mem, pcm_full);
        tlcs_wav_write(&wav_full, pcm_full, n);

        /* 2. Pitch only: pitch_exc through 1/A_q(z) + de-emphasis */
        int16_t pcm_pitch[TLCS_MAX_FRAME_SIZE];
        synth_component(diag.pitch_exc, diag.a_quant, order, n,
                        pitch_synth_mem, &pitch_deemph_mem, pcm_pitch);
        tlcs_wav_write(&wav_pitch, pcm_pitch, n);

        /* 3. CB only: cb_exc through 1/A_q(z) + de-emphasis */
        int16_t pcm_cb[TLCS_MAX_FRAME_SIZE];
        synth_component(diag.cb_exc, diag.a_quant, order, n,
                        cb_synth_mem, &cb_deemph_mem, pcm_cb);
        tlcs_wav_write(&wav_cb, pcm_cb, n);

        /* 4. Residual: scale float residual to int16 */
        int16_t pcm_res[TLCS_MAX_FRAME_SIZE];
        for (int32_t i = 0; i < n; i++) {
            float v = diag.residual[i];
            if (v > 32767.0f) v = 32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            pcm_res[i] = (int16_t)v;
        }
        tlcs_wav_write(&wav_res, pcm_res, n);

        /* 5. LPC resynth: full_exc through 1/A_unquant(z) + de-emphasis
         *    (isolates LSF quantization loss) */
        int16_t pcm_lpc[TLCS_MAX_FRAME_SIZE];
        synth_component(diag.full_exc, diag.a_unquant, order, n,
                        lpc_synth_mem, &lpc_deemph_mem, pcm_lpc);
        tlcs_wav_write(&wav_lpc, pcm_lpc, n);

        /* Write CSV row */
        tlcs_diag_frame *m = &diag.metrics;
        float total_e = m->pitch_energy + m->cb_energy + m->error_energy;
        float pitch_frac = (total_e > 0) ? m->pitch_energy / total_e : 0.0f;
        float cb_frac    = (total_e > 0) ? m->cb_energy / total_e : 0.0f;
        float error_frac = (total_e > 0) ? m->error_energy / total_e : 0.0f;

        fprintf(fp_csv, "%d,%.3f,%.3f,"
                        "%.4f,%.4f,%.4f,%.4f,"
                        "%.4f,%.4f,%.4f,%.4f,"
                        "%d,%d,%d,%d,"
                        "%.1f,%.1f,%.1f,%.1f,"
                        "%.4f,%.4f,%.4f,"
                        "%.2f,%.2f,%.2f,%.2f,"
                        "%.2f,%.2f,%.2f,%.2f\n",
                m->frame_num, m->lsf_sd, m->voicing,
                m->pitch_gain[0], m->pitch_gain[1], m->pitch_gain[2], m->pitch_gain[3],
                m->pitch_gain_unquant[0], m->pitch_gain_unquant[1],
                m->pitch_gain_unquant[2], m->pitch_gain_unquant[3],
                m->pitch_lag[0], m->pitch_lag[1], m->pitch_lag[2], m->pitch_lag[3],
                m->residual_energy, m->pitch_energy, m->cb_energy, m->error_energy,
                pitch_frac, cb_frac, error_frac,
                m->cb_gain[0], m->cb_gain[1], m->cb_gain[2], m->cb_gain[3],
                m->cb_gain_unquant[0], m->cb_gain_unquant[1],
                m->cb_gain_unquant[2], m->cb_gain_unquant[3]);

        total_frames++;
    }

    /* Close everything */
    fclose(fp_csv);
    tlcs_wav_close(&wav_full);
    tlcs_wav_close(&wav_pitch);
    tlcs_wav_close(&wav_cb);
    tlcs_wav_close(&wav_res);
    tlcs_wav_close(&wav_lpc);
    tlcs_wav_close(&wav_in);

    free(path_full);
    free(path_pitch);
    free(path_cb);
    free(path_res);
    free(path_lpc);
    free(path_csv);

    fprintf(stderr, "Processed %d frames\n", total_frames);
    fprintf(stderr, "Output files:\n");
    fprintf(stderr, "  %s_full_recon.wav\n", prefix);
    fprintf(stderr, "  %s_pitch_only.wav\n", prefix);
    fprintf(stderr, "  %s_cb_only.wav\n", prefix);
    fprintf(stderr, "  %s_residual.wav\n", prefix);
    fprintf(stderr, "  %s_lpc_resynth.wav\n", prefix);
    fprintf(stderr, "  %s_metrics.csv\n", prefix);

    return 0;
}
