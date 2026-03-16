/*
 * tlcs_encoder.c — Main CELP encoder.
 *
 * Signal flow per frame:
 * 1. HP filter (2nd-order Butterworth) + pre-emphasis
 * 2. LPC analysis -> LSP -> quantize
 * 3. Per subframe:
 *    a. Interpolate LSP -> LPC
 *    b. Open/closed-loop pitch search -> ACB
 *    c. Algebraic codebook search
 *    d. Gain quantization
 *    e. Update excitation memory
 * 4. Pack bitstream (20 bytes)
 */
#include "tlcs_config.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#include "tlcs.h"
#include "tlcs_lpc.h"
#include "tlcs_pitch.h"
#include "tlcs_algebraic_cb.h"
#include "tlcs_quantization.h"
#include "tlcs_bitstream.h"

/* ================================================================== */
/* Encoder struct (opaque)                                             */
/* ================================================================== */

struct TlcsEncoder {
    int sample_rate;
    int bitrate;

    /* HP filter state (2nd-order Butterworth, direct-form II transposed) */
    float hp_b0, hp_b1, hp_b2;
    float hp_a1, hp_a2;
    float hp_z1, hp_z2;

    /* Pre-emphasis state */
    float preemph_prev;

    /* LPC / LSP state */
    float prev_lsp[TLCS_LPC_ORDER];

    /* Excitation buffer (large enough for max pitch lag + frame) */
    float exc_buf[TLCS_PITCH_MAX_LAG + TLCS_FRAME_SIZE + 160];
    int   exc_len;

    /* Synthesis filter memory */
    float synth_mem[TLCS_LPC_ORDER];
};

/* ================================================================== */
/* HP filter coefficient computation                                   */
/* ================================================================== */

static void hp_filter_init(TlcsEncoder *enc, float cutoff_hz, int sr)
{
    float wc = 2.0f * 3.14159265f * cutoff_hz / (float)sr;
    float k = tanf(wc * 0.5f);
    float k2 = k * k;
    float sq2 = 1.41421356f;
    float norm = 1.0f / (1.0f + sq2 * k + k2);

    enc->hp_b0 = norm;
    enc->hp_b1 = -2.0f * norm;
    enc->hp_b2 = norm;
    enc->hp_a1 = 2.0f * (k2 - 1.0f) * norm;
    enc->hp_a2 = (1.0f - sq2 * k + k2) * norm;
    enc->hp_z1 = 0.0f;
    enc->hp_z2 = 0.0f;
}

static void hp_filter_process(TlcsEncoder *enc, const float *in, float *out, int len)
{
    for (int i = 0; i < len; i++) {
        float x = in[i];
        float y = enc->hp_b0 * x + enc->hp_z1;
        enc->hp_z1 = enc->hp_b1 * x - enc->hp_a1 * y + enc->hp_z2;
        enc->hp_z2 = enc->hp_b2 * x - enc->hp_a2 * y;
        out[i] = y;
    }
}

/* ================================================================== */
/* Pre-emphasis: y[n] = x[n] - coeff * x[n-1]                         */
/* ================================================================== */

static void preemph_process(TlcsEncoder *enc, float *buf, int len)
{
    float prev = enc->preemph_prev;
    for (int i = 0; i < len; i++) {
        float x = buf[i];
        buf[i] = x - TLCS_PREEMPH_COEFF * prev;
        prev = x;
    }
    enc->preemph_prev = prev;
}

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

TlcsEncoder* tlcs_encoder_create(int sample_rate, int bitrate_bps)
{
    if (sample_rate != TLCS_SAMPLE_RATE) return NULL;

    TlcsEncoder *enc = (TlcsEncoder*)calloc(1, sizeof(TlcsEncoder));
    if (!enc) return NULL;

    enc->sample_rate = sample_rate;
    enc->bitrate = bitrate_bps;

    /* Init HP filter (20 Hz cutoff) */
    hp_filter_init(enc, 20.0f, sample_rate);

    /* Init LSP to uniform spacing */
    const float PI = 3.14159265f;
    for (int i = 0; i < TLCS_LPC_ORDER; i++) {
        enc->prev_lsp[i] = PI * (float)(i + 1) / (float)(TLCS_LPC_ORDER + 1);
    }

    enc->exc_len = TLCS_PITCH_MAX_LAG + TLCS_FRAME_SIZE + 160;

    /* Init VQ codebooks */
    tlcs_lsp_vq_init();
    tlcs_gain_vq_init();

    return enc;
}

void tlcs_encoder_destroy(TlcsEncoder *enc)
{
    free(enc);
}

void tlcs_encoder_reset(TlcsEncoder *enc)
{
    if (!enc) return;

    enc->hp_z1 = 0.0f;
    enc->hp_z2 = 0.0f;
    enc->preemph_prev = 0.0f;

    const float PI = 3.14159265f;
    for (int i = 0; i < TLCS_LPC_ORDER; i++) {
        enc->prev_lsp[i] = PI * (float)(i + 1) / (float)(TLCS_LPC_ORDER + 1);
    }

    memset(enc->exc_buf, 0, sizeof(enc->exc_buf));
    memset(enc->synth_mem, 0, sizeof(enc->synth_mem));
}

/* ================================================================== */
/* Encode one frame                                                    */
/* ================================================================== */

int tlcs_encode(TlcsEncoder *enc, const int16_t *pcm,
                uint8_t *buf, int buf_size)
{
    if (!enc || !pcm || !buf) return TLCS_ERR_ARGS;
    if (buf_size < TLCS_BYTES_PER_FRAME) return TLCS_ERR_ARGS;

    const int N = TLCS_FRAME_SIZE;
    const int Nsub = TLCS_SUBFRAME_SIZE;
    const int P = TLCS_LPC_ORDER;

    /* Convert PCM to float */
    float frame[TLCS_FRAME_SIZE];
    for (int i = 0; i < N; i++) {
        frame[i] = (float)pcm[i] / 32768.0f;
    }

    /* ---- 1. HP filter ---- */
    float hp_out[TLCS_FRAME_SIZE];
    hp_filter_process(enc, frame, hp_out, N);

    /* ---- 2. Pre-emphasis ---- */
    float pe_frame[TLCS_FRAME_SIZE];
    memcpy(pe_frame, hp_out, N * sizeof(float));
    preemph_process(enc, pe_frame, N);

    /* ---- 3. LPC analysis ---- */
    float lpc[TLCS_LPC_ORDER + 1];
    float lpc_gain;
    tlcs_lpc_analysis(pe_frame, N, P, lpc, &lpc_gain);

    /* ---- 4. LPC -> LSP -> quantize ---- */
    float lsp_curr[TLCS_LPC_ORDER];
    tlcs_lpc_to_lsp(lpc, P, lsp_curr);
    tlcs_lsp_stabilize(lsp_curr, P, 0.05f);

    int lsp_indices[TLCS_LSP_NUM_SPLITS];
    float lsp_q[TLCS_LPC_ORDER];
    tlcs_lsp_vq_quantize(lsp_curr, lsp_indices, lsp_q);
    tlcs_lsp_stabilize(lsp_q, P, 0.05f);

    /* ---- 5. Open-loop pitch estimate ---- */
    /* Compute residual for OL pitch */
    float residual[TLCS_FRAME_SIZE];
    for (int n = 0; n < N; n++) {
        float val = 0.0f;
        for (int k = 0; k <= P; k++) {
            int idx = n - k;
            if (idx >= 0) val += lpc[k] * pe_frame[idx];
        }
        residual[n] = val;
    }
    int ol_pitch = tlcs_pitch_open_loop(residual, N,
                                        TLCS_PITCH_MIN_LAG, TLCS_PITCH_MAX_LAG);

    /* ---- 6. Subframe processing ---- */
    TlcsFrameData fd;
    memset(&fd, 0, sizeof(fd));
    for (int s = 0; s < TLCS_LSP_NUM_SPLITS; s++) {
        fd.lsp_indices[s] = lsp_indices[s];
    }

    float synth_mem[TLCS_LPC_ORDER];
    memcpy(synth_mem, enc->synth_mem, P * sizeof(float));

    for (int sf = 0; sf < TLCS_NUM_SUBFRAMES; sf++) {
        int sf_start = sf * Nsub;
        const float *target_speech = &pe_frame[sf_start];

        /* Interpolate LSP for this subframe */
        float alpha = (float)(sf + 1) / (float)TLCS_NUM_SUBFRAMES;
        float lsp_interp[TLCS_LPC_ORDER];
        tlcs_lsp_interpolate(enc->prev_lsp, lsp_q, alpha, P, lsp_interp);
        tlcs_lsp_stabilize(lsp_interp, P, 0.05f);

        float lpc_sub[TLCS_LPC_ORDER + 1];
        tlcs_lsp_to_lpc(lsp_interp, P, lpc_sub);

        /* ---- Perceptual weighting filter W(z) = A(z/g1) / A(z/g2) ---- */
        /* Compute weighted LPC coefficients for numerator and denominator */
        float lpc_wnum[TLCS_LPC_ORDER + 1]; /* A(z/gamma1) */
        float lpc_wden[TLCS_LPC_ORDER + 1]; /* A(z/gamma2) */
        lpc_wnum[0] = 1.0f;
        lpc_wden[0] = 1.0f;
        {
            float g1k = TLCS_PERC_GAMMA1;
            float g2k = TLCS_PERC_GAMMA2;
            for (int k = 1; k <= P; k++) {
                lpc_wnum[k] = lpc_sub[k] * g1k;
                lpc_wden[k] = lpc_sub[k] * g2k;
                g1k *= TLCS_PERC_GAMMA1;
                g2k *= TLCS_PERC_GAMMA2;
            }
        }

        /* Impulse response of W(z)/A(z) = A(z/g1) / (A(z) * A(z/g2))
         * For analysis-by-synthesis, we need h = impulse response of
         * W(z) * 1/A(z) which shapes the synthesis through the
         * perceptual weighting filter.
         * Compute as: impulse response of 1/A(z) filtered through W(z).
         */
        /* First: impulse response of 1/A(z) */
        float h_synth[TLCS_SUBFRAME_SIZE];
        tlcs_lpc_impulse_response(lpc_sub, P, Nsub, h_synth);

        /* Apply W(z) = A(z/g1) / A(z/g2) to h_synth to get weighted IR */
        float h[TLCS_SUBFRAME_SIZE];
        {
            float wnum_mem[TLCS_LPC_ORDER];
            float wden_mem[TLCS_LPC_ORDER];
            memset(wnum_mem, 0, sizeof(wnum_mem));
            memset(wden_mem, 0, sizeof(wden_mem));

            for (int i = 0; i < Nsub; i++) {
                /* FIR part: A(z/g1) */
                float val = h_synth[i];
                for (int k = 0; k < P; k++) {
                    val += lpc_wnum[k + 1] * wnum_mem[k];
                }
                /* Shift FIR memory */
                for (int k = P - 1; k > 0; k--) wnum_mem[k] = wnum_mem[k - 1];
                wnum_mem[0] = h_synth[i];

                /* IIR part: 1/A(z/g2) */
                float out_val = val;
                for (int k = 0; k < P; k++) {
                    out_val -= lpc_wden[k + 1] * wden_mem[k];
                }
                for (int k = P - 1; k > 0; k--) wden_mem[k] = wden_mem[k - 1];
                wden_mem[0] = out_val;

                h[i] = out_val;
            }
        }

        /* Zero-state response (ringing from previous subframe) */
        float zsr[TLCS_SUBFRAME_SIZE];
        tlcs_lpc_zero_state_response(lpc_sub, P, synth_mem, Nsub, zsr);

        /* Target = speech - ringing */
        float target_unweighted[TLCS_SUBFRAME_SIZE];
        for (int i = 0; i < Nsub; i++) {
            target_unweighted[i] = target_speech[i] - zsr[i];
        }

        /* Apply perceptual weighting W(z) to target */
        float target[TLCS_SUBFRAME_SIZE];
        {
            float wnum_mem[TLCS_LPC_ORDER];
            float wden_mem[TLCS_LPC_ORDER];
            memset(wnum_mem, 0, sizeof(wnum_mem));
            memset(wden_mem, 0, sizeof(wden_mem));

            for (int i = 0; i < Nsub; i++) {
                /* FIR: A(z/g1) */
                float val = target_unweighted[i];
                for (int k = 0; k < P; k++) {
                    val += lpc_wnum[k + 1] * wnum_mem[k];
                }
                for (int k = P - 1; k > 0; k--) wnum_mem[k] = wnum_mem[k - 1];
                wnum_mem[0] = target_unweighted[i];

                /* IIR: 1/A(z/g2) */
                float out_val = val;
                for (int k = 0; k < P; k++) {
                    out_val -= lpc_wden[k + 1] * wden_mem[k];
                }
                for (int k = P - 1; k > 0; k--) wden_mem[k] = wden_mem[k - 1];
                wden_mem[0] = out_val;

                target[i] = out_val;
            }
        }

        /* ---- Adaptive codebook (pitch) search ---- */
        int search_min = ol_pitch - 10;
        int search_max = ol_pitch + 10;
        if (search_min < TLCS_PITCH_MIN_LAG) search_min = TLCS_PITCH_MIN_LAG;
        if (search_max > TLCS_PITCH_MAX_LAG) search_max = TLCS_PITCH_MAX_LAG;

        float pitch_lag, pitch_gain;
        tlcs_pitch_closed_loop(target, h, enc->exc_buf, enc->exc_len,
                               Nsub, search_min, search_max,
                               &pitch_lag, &pitch_gain);

        /* Build ACB excitation */
        float acb_exc[TLCS_SUBFRAME_SIZE];
        tlcs_pitch_build_acb(enc->exc_buf, enc->exc_len,
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

        /* Remove ACB contribution from target */
        float acb_filtered[TLCS_SUBFRAME_SIZE];
        tlcs_convolve(acb_exc, h, Nsub, acb_filtered);

        float target2[TLCS_SUBFRAME_SIZE];
        for (int i = 0; i < Nsub; i++) {
            target2[i] = target[i] - pitch_gain * acb_filtered[i];
        }

        /* ---- Algebraic codebook search (in weighted domain) ---- */
        int fcb_index;
        float cb_gain_weighted;
        float fcb_exc[TLCS_SUBFRAME_SIZE];
        tlcs_acb_search(target2, h, Nsub, &fcb_index, &cb_gain_weighted, fcb_exc);

        /* Recompute gains in UNWEIGHTED domain for the decoder.
         * The FCB search found optimal pulse positions in weighted domain,
         * but the decoder synthesizes through 1/A(z) without weighting.
         * Compute: gain = <unweighted_target, H_synth*c> / <H_synth*c, H_synth*c> */
        float cb_gain;
        {
            float h_synth_only[TLCS_SUBFRAME_SIZE];
            tlcs_lpc_impulse_response(lpc_sub, P, Nsub, h_synth_only);

            /* Unweighted target = speech - zero-state response */
            float acb_synth[TLCS_SUBFRAME_SIZE];
            tlcs_convolve(acb_exc, h_synth_only, Nsub, acb_synth);
            float tgt_uw[TLCS_SUBFRAME_SIZE];
            for (int i = 0; i < Nsub; i++) {
                tgt_uw[i] = target_unweighted[i] - pitch_gain * acb_synth[i];
            }

            float fcb_synth[TLCS_SUBFRAME_SIZE];
            tlcs_convolve(fcb_exc, h_synth_only, Nsub, fcb_synth);
            float num = 0, den = 0;
            for (int i = 0; i < Nsub; i++) {
                num += tgt_uw[i] * fcb_synth[i];
                den += fcb_synth[i] * fcb_synth[i];
            }
            cb_gain = num / (den + 1e-10f);
        }

        /* Also recompute pitch gain in unweighted domain */
        {
            float h_synth_only[TLCS_SUBFRAME_SIZE];
            tlcs_lpc_impulse_response(lpc_sub, P, Nsub, h_synth_only);
            float acb_synth[TLCS_SUBFRAME_SIZE];
            tlcs_convolve(acb_exc, h_synth_only, Nsub, acb_synth);
            float num = 0, den = 0;
            for (int i = 0; i < Nsub; i++) {
                num += target_unweighted[i] * acb_synth[i];
                den += acb_synth[i] * acb_synth[i];
            }
            pitch_gain = num / (den + 1e-10f);
            if (pitch_gain < 0.0f) pitch_gain = 0.0f;
            if (pitch_gain > 1.2f) pitch_gain = 1.2f;
        }

        /* ---- Gain quantization ---- */
        float q_pg, q_cg;
        int gain_idx = tlcs_gain_vq_quantize(pitch_gain, cb_gain, &q_pg, &q_cg);

        /* Gains quantized */

        /* ---- Update excitation buffer ---- */
        float total_exc[TLCS_SUBFRAME_SIZE];
        for (int i = 0; i < Nsub; i++) {
            total_exc[i] = q_pg * acb_exc[i] + q_cg * fcb_exc[i];
        }

        int n_buf = enc->exc_len;
        memmove(enc->exc_buf, enc->exc_buf + Nsub,
                (n_buf - Nsub) * sizeof(float));
        memcpy(enc->exc_buf + n_buf - Nsub, total_exc, Nsub * sizeof(float));

        /* Update synthesis filter state */
        float synth_out[TLCS_SUBFRAME_SIZE];
        tlcs_lpc_synthesis(total_exc, Nsub, lpc_sub, P, synth_mem, synth_out);

        /* ---- Encode pitch lag ---- */
        int int_lag = (int)roundf(pitch_lag);
        float frac_part = pitch_lag - floorf(pitch_lag);
        int lag_index = int_lag - TLCS_PITCH_MIN_LAG;
        if (lag_index < 0) lag_index = 0;
        if (lag_index > TLCS_PITCH_MAX_LAG - TLCS_PITCH_MIN_LAG)
            lag_index = TLCS_PITCH_MAX_LAG - TLCS_PITCH_MIN_LAG;

        int frac_index = (int)roundf(frac_part * 3.0f) % 3;

        int pg_idx = tlcs_pitch_gain_quantize(q_pg);

        /* Pack subframe data */
        TlcsSubframeData *sd = &fd.sf[sf];
        sd->pitch_lag_idx  = lag_index;
        sd->pitch_frac_idx = frac_index;
        sd->pitch_gain_idx = pg_idx;
        sd->fcb_index      = fcb_index;
        sd->gain_index     = gain_idx;

        /* Update OL pitch for next subframe */
        ol_pitch = int_lag;
    }

    /* Save state */
    memcpy(enc->prev_lsp, lsp_q, P * sizeof(float));
    memcpy(enc->synth_mem, synth_mem, P * sizeof(float));

    /* Pack bitstream */
    return tlcs_frame_pack(&fd, buf, buf_size);
}

/* Temporary debug function */
void tlcs_debug_lpc(const float *a, int order) {
    float sum = 0;
    for (int i = 1; i <= order; i++) sum += fabsf(a[i]);
    printf("  LPC sum|a|=%.3f stable=%s\n", (double)sum, sum < 1.0 ? "maybe" : "NO");
}
