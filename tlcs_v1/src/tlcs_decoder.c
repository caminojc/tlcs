/*
 * tlcs_decoder.c — Main CELP decoder.
 *
 * Signal flow per frame:
 * 1. Unpack bitstream
 * 2. Dequantize LSPs
 * 3. Per subframe (2 x 160 samples):
 *    a. Interpolate LSP -> LPC
 *    b. Dequantize gains
 *    c. Build adaptive codebook excitation (pitch)
 *    d. Build fixed codebook excitation (algebraic, 8 pulses)
 *    e. Combine: exc = gp * acb + gc * fcb
 *    f. Synthesis filter: 1/A(z)
 * 4. Harmonic postfilter -> formant postfilter
 * 5. De-emphasis
 *
 * Pitch decoding:
 *   SF0: full_lag = pitch_lag_idx + PITCH_MIN_LAG, frac = idx * 0.5
 *   SF1: delta decoding: lag = sf0_lag + (pitch_lag_idx - 8), frac = idx * 0.5
 */
#include "tlcs_config.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "tlcs.h"
#include "tlcs_lpc.h"
#include "tlcs_pitch.h"
#include "tlcs_algebraic_cb.h"
#include "tlcs_quantization.h"
#include "tlcs_postfilter.h"
#include "tlcs_bitstream.h"

/* ================================================================== */
/* Decoder struct (opaque)                                             */
/* ================================================================== */

struct TlcsDecoder {
    int sample_rate;

    /* De-emphasis state: y[n] = x[n] + coeff * y[n-1] */
    float deemph_prev;

    /* LPC / LSP state */
    float prev_lsp[TLCS_LPC_ORDER];

    /* Excitation buffer */
    float exc_buf[TLCS_PITCH_MAX_LAG + TLCS_FRAME_SIZE + 160];
    int   exc_len;

    /* Synthesis filter memory */
    float synth_mem[TLCS_LPC_ORDER];

    /* Postfilter */
    TlcsPostfilterState pf;
};

/* ================================================================== */
/* De-emphasis: y[n] = x[n] + coeff * y[n-1]                          */
/* ================================================================== */

static void deemph_process(TlcsDecoder *dec, float *buf, int len)
{
    float prev = dec->deemph_prev;
    for (int i = 0; i < len; i++) {
        buf[i] = buf[i] + TLCS_PREEMPH_COEFF * prev;
        prev = buf[i];
    }
    dec->deemph_prev = prev;
}

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

TlcsDecoder* tlcs_decoder_create(int sample_rate)
{
    if (sample_rate != TLCS_SAMPLE_RATE) return NULL;

    TlcsDecoder *dec = (TlcsDecoder*)calloc(1, sizeof(TlcsDecoder));
    if (!dec) return NULL;

    dec->sample_rate = sample_rate;

    /* Init LSP to uniform spacing */
    const float PI = 3.14159265f;
    for (int i = 0; i < TLCS_LPC_ORDER; i++) {
        dec->prev_lsp[i] = PI * (float)(i + 1) / (float)(TLCS_LPC_ORDER + 1);
    }

    dec->exc_len = TLCS_PITCH_MAX_LAG + TLCS_FRAME_SIZE + 160;

    /* Init VQ codebooks (must match encoder) */
    tlcs_lsp_vq_init();
    tlcs_gain_vq_init();

    /* Init postfilter */
    tlcs_postfilter_init(&dec->pf);

    return dec;
}

void tlcs_decoder_destroy(TlcsDecoder *dec)
{
    free(dec);
}

void tlcs_decoder_reset(TlcsDecoder *dec)
{
    if (!dec) return;

    dec->deemph_prev = 0.0f;

    const float PI = 3.14159265f;
    for (int i = 0; i < TLCS_LPC_ORDER; i++) {
        dec->prev_lsp[i] = PI * (float)(i + 1) / (float)(TLCS_LPC_ORDER + 1);
    }

    memset(dec->exc_buf, 0, sizeof(dec->exc_buf));
    memset(dec->synth_mem, 0, sizeof(dec->synth_mem));
    tlcs_postfilter_init(&dec->pf);
}

/* ================================================================== */
/* Decode one frame                                                    */
/* ================================================================== */

int tlcs_decode(TlcsDecoder *dec, const uint8_t *buf, int buf_size,
                int16_t *pcm)
{
    if (!dec || !buf || !pcm) return TLCS_ERR_ARGS;
    if (buf_size < TLCS_BYTES_PER_FRAME) return TLCS_ERR_STREAM;

    const int N = TLCS_FRAME_SIZE;
    const int Nsub = TLCS_SUBFRAME_SIZE;
    const int P = TLCS_LPC_ORDER;

    /* ---- 1. Unpack bitstream ---- */
    TlcsFrameData fd;
    int ret = tlcs_frame_unpack(buf, buf_size, &fd);
    if (ret != TLCS_OK) return ret;

    /* ---- 2. Dequantize LSPs ---- */
    float lsp_q[TLCS_LPC_ORDER];
    tlcs_lsp_vq_dequantize(fd.lsp_indices, lsp_q);
    tlcs_lsp_stabilize(lsp_q, P, 0.05f);

    /* ---- 3. Subframe synthesis ---- */
    float output[TLCS_FRAME_SIZE];
    float synth_mem[TLCS_LPC_ORDER];
    memcpy(synth_mem, dec->synth_mem, P * sizeof(float));

    int sf0_int_lag = 0;  /* saved for SF1 delta decoding */

    for (int sf = 0; sf < TLCS_NUM_SUBFRAMES; sf++) {
        const TlcsSubframeData *sd = &fd.sf[sf];

        /* Interpolate LSP */
        float alpha = (float)(sf + 1) / (float)TLCS_NUM_SUBFRAMES;
        float lsp_interp[TLCS_LPC_ORDER];
        tlcs_lsp_interpolate(dec->prev_lsp, lsp_q, alpha, P, lsp_interp);
        tlcs_lsp_stabilize(lsp_interp, P, 0.05f);

        float lpc_sub[TLCS_LPC_ORDER + 1];
        tlcs_lsp_to_lpc(lsp_interp, P, lpc_sub);

        /* Decode pitch lag */
        int int_lag;
        if (sf == 0) {
            /* SF0: full lag decoding */
            int_lag = sd->pitch_lag_idx + TLCS_PITCH_MIN_LAG;
            sf0_int_lag = int_lag;
        } else {
            /* SF1: delta lag decoding: lag = sf0_lag + (index - 8) */
            int delta = sd->pitch_lag_idx - 8;
            int_lag = sf0_int_lag + delta;
            /* Clamp to valid range */
            if (int_lag < TLCS_PITCH_MIN_LAG) int_lag = TLCS_PITCH_MIN_LAG;
            if (int_lag > TLCS_PITCH_MAX_LAG) int_lag = TLCS_PITCH_MAX_LAG;
        }

        float frac = (float)sd->pitch_frac_idx * 0.5f;  /* 1-bit: 0 or 0.5 */
        float pitch_lag = (float)int_lag + frac;

        /* Dequantize gains — dB-stepped for FCB, scalar for pitch */
        float q_pg = tlcs_pitch_gain_dequantize(sd->pitch_gain_idx);
        float q_cg = tlcs_fcbgain_dequantize(sd->gain_index, 1 /*voiced*/);

        /* Build adaptive codebook excitation */
        float *acb_exc = (float *)malloc((size_t)Nsub * sizeof(float));
        tlcs_pitch_build_acb(dec->exc_buf, dec->exc_len,
                             pitch_lag, Nsub, acb_exc);

        /* Pitch sharpening: reinforce periodicity */
        {
            int ilag = (int)roundf(pitch_lag);
            if (ilag > 0 && ilag < Nsub) {
                for (int i = ilag; i < Nsub; i++) {
                    acb_exc[i] += TLCS_PITCH_SHARPENING_COEF * acb_exc[i - ilag];
                }
            }
        }

        /* Build fixed codebook excitation (8 pulses, split index) */
        float *fcb_exc = (float *)malloc((size_t)Nsub * sizeof(float));
        tlcs_acb_decode(sd->fcb_index_lo, sd->fcb_index_hi, Nsub, fcb_exc);

        /* Combine excitations */
        float *total_exc = (float *)malloc((size_t)Nsub * sizeof(float));
        for (int i = 0; i < Nsub; i++) {
            total_exc[i] = q_pg * acb_exc[i] + q_cg * fcb_exc[i];
        }

        /* ---- Shaped noise fill ---- */
        {
            float exc_energy = 0.0f;
            for (int i = 0; i < Nsub; i++) {
                exc_energy += total_exc[i] * total_exc[i];
            }
            exc_energy /= (float)Nsub;

            float noise_gain = (q_pg > 0.5f) ? TLCS_NOISE_V_GAIN : TLCS_NOISE_UV_GAIN;
            float noise_scale = noise_gain * (1.0f - q_pg * 0.7f);
            if (noise_scale < 0.0f) noise_scale = 0.0f;

            float *noise_buf = (float *)malloc((size_t)Nsub * sizeof(float));
            static unsigned int noise_seed = 12345;
            float noise_energy = 0.0f;
            for (int i = 0; i < Nsub; i++) {
                noise_seed = noise_seed * 1664525u + 1013904223u;
                float white = ((float)(int)(noise_seed >> 1) / (float)0x3FFFFFFF) - 1.0f;

                float shaped = white;
                if (i >= 1) shaped -= 0.5f * lpc_sub[1] * noise_buf[i - 1];
                if (i >= 2) shaped -= 0.3f * lpc_sub[2] * noise_buf[i - 2];
                noise_buf[i] = shaped;
                noise_energy += shaped * shaped;
            }

            if (noise_energy > 1e-10f && exc_energy > 1e-10f) {
                float norm = sqrtf(exc_energy / (noise_energy / (float)Nsub));
                for (int i = 0; i < Nsub; i++) {
                    total_exc[i] += noise_scale * norm * noise_buf[i];
                }
            }
            free(noise_buf);
        }

        /* Update excitation buffer */
        int n_buf = dec->exc_len;
        memmove(dec->exc_buf, dec->exc_buf + Nsub,
                (n_buf - Nsub) * sizeof(float));
        memcpy(dec->exc_buf + n_buf - Nsub, total_exc, Nsub * sizeof(float));

        /* Synthesis filter */
        float *speech = (float *)malloc((size_t)Nsub * sizeof(float));
        tlcs_lpc_synthesis(total_exc, Nsub, lpc_sub, P, synth_mem, speech);

        /* Harmonic postfilter */
        float *harm_out = (float *)malloc((size_t)Nsub * sizeof(float));
        tlcs_harmonic_postfilter(&dec->pf, speech, Nsub,
                                 (int)roundf(pitch_lag), harm_out);

        /* Formant postfilter */
        float *pf_out = (float *)malloc((size_t)Nsub * sizeof(float));
        tlcs_formant_postfilter(&dec->pf, harm_out, Nsub,
                                lpc_sub, P, pf_out);

        memcpy(&output[sf * Nsub], pf_out, Nsub * sizeof(float));

        free(acb_exc);
        free(fcb_exc);
        free(total_exc);
        free(speech);
        free(harm_out);
        free(pf_out);
    }

    /* ---- 4. De-emphasis ---- */
    deemph_process(dec, output, N);

    /* ---- 5. Convert to int16 ---- */
    for (int i = 0; i < N; i++) {
        float s = output[i] * 32768.0f;
        if (s > 32767.0f) s = 32767.0f;
        if (s < -32768.0f) s = -32768.0f;
        pcm[i] = (int16_t)s;
    }

    /* Save state */
    memcpy(dec->prev_lsp, lsp_q, P * sizeof(float));
    memcpy(dec->synth_mem, synth_mem, P * sizeof(float));

    return N;
}
