#include "tlcs/tlcs.h"
#include "tlcs_lpc.h"
#include "tlcs_pitch.h"
#include "tlcs_codebook.h"
#include "tlcs_qmf.h"
#include "tlcs_lsf_vq.h"
#include "../bitstream/tlcs_bitstream.h"
#include "../entropy/tlcs_range_coder.h"
#include "../entropy/tlcs_ec_models.h"
#include "tlcs_mdct.h"
#include "tlcs_tcx.h"
#include "tlcs_tune.h"
#include "tlcs_mode.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void init_default_lsf(float *lsf, int32_t order)
{
    for (int32_t i = 0; i < order; i++) {
        lsf[i] = (float)M_PI * (float)(i + 1) / (float)(order + 1);
    }
}

/* ══════════════════════════════════════════════════════════════════
 *  CELP core: encode one frame of float speech at sample_rate/2
 *  Used for both NB direct and WB low-band encoding.
 * ══════════════════════════════════════════════════════════════════ */
static void celp_encode_core(tlcs_encoder *enc,
                              const float *speech,     /* pre-emphasized speech [n] */
                              int32_t n, int32_t order,
                              int32_t subfr, int32_t n_subfr,
                              int32_t min_lag, int32_t max_lag,
                              int32_t num_pulses,
                              float gamma1, float gamma2,
                              /* outputs */
                              int16_t *lsf_indices,
                              int32_t *pitch_lags, int32_t *pitch_fracs,
                              int32_t *pitch_gain_indices,
                              tlcs_cb_entry *cb_entries,
                              float *a_q_out)         /* quantized LPC for caller */
{
    /* ── Update speech history FIRST (for extended LPC window) ── */
    memmove(enc->speech_buf,
            enc->speech_buf + n,
            (size_t)TLCS_MAX_PITCH_LAG * sizeof(float));
    memcpy(enc->speech_buf + TLCS_MAX_PITCH_LAG,
           speech, (size_t)n * sizeof(float));

    /* ── LPC analysis with SMPL-style asymmetric window ──── */
    /* Sine-rise / flat / cosine-fall window with past context.
     * Window must fit within win_prev + n samples (no look-ahead). */
    int32_t win_prev = n / 5;           /* 64 past samples (4ms) */
    int32_t win_ones_init = n * 3 / 8;  /* 120 target flat samples */
    int32_t win1_len = win_prev + n - win_ones_init;  /* 264 rising sine */
    int32_t win3_len = n / 10;          /* 32 falling cosine (2ms) */
    /* Adjust flat to fit: total = win_prev + n */
    int32_t win_ones = (win_prev + n) - win1_len - win3_len;  /* 88 flat */
    int32_t ana_len = win1_len + win_ones + win3_len;

    float extended[TLCS_MAX_PITCH_LAG + TLCS_MAX_FRAME_SIZE];
    memcpy(extended, &enc->speech_buf[TLCS_MAX_PITCH_LAG - win_prev],
           (size_t)ana_len * sizeof(float));

    float windowed[TLCS_MAX_PITCH_LAG + TLCS_MAX_FRAME_SIZE];
    /* Apply sine-rise / flat / cosine-fall */
    for (int32_t i = 0; i < win1_len; i++) {
        float w = sinf((float)(i + 1) / (float)(win1_len + 1) * (float)M_PI * 0.5f);
        windowed[i] = extended[i] * w;
    }
    for (int32_t i = 0; i < win_ones; i++) {
        windowed[win1_len + i] = extended[win1_len + i];
    }
    for (int32_t i = 0; i < win3_len; i++) {
        float w = cosf((float)(i + 1) / (float)(win3_len + 1) * (float)M_PI * 0.5f);
        windowed[win1_len + win_ones + i] = extended[win1_len + win_ones + i] * w;
    }

    float r[TLCS_LPC_ORDER_MAX + 1];
    tlcs_autocorrelation(windowed, ana_len, r, order);

    float a[TLCS_LPC_ORDER_MAX + 1];
    tlcs_levinson(r, order, a, NULL);

    /* LR mode: extra bandwidth expansion to reduce harmonic tracking.
     * Moves LPC poles toward origin, widening formant bandwidths.
     * This helps coarse quantization avoid spectral envelope artifacts. */
    if (subfr >= 80) {
        float g = 1.0f;
        for (int32_t k = 1; k <= order; k++) {
            g *= 0.996f;
            a[k] *= g;
        }
    }

    float lsf[TLCS_LPC_ORDER_MAX];
    int lsf_ok = tlcs_lpc_to_lsf(a, order, lsf);
    if (lsf_ok != 0) {
        for (int32_t i = 0; i < order; i++)
            lsf[i] = (float)enc->prev_lsf[i] / 5000.0f;
    }
    tlcs_lsf_stabilize(lsf, order);

    /* Predictive LSF quantization (parameterized by config) */
    int32_t lsf_bits = enc->cfg.lsf_bits;
    float lsf_range = (lsf_bits <= 5) ? 0.40f : (lsf_bits == 6) ? 0.45f : 0.50f;

    float lsf_pred[TLCS_LPC_ORDER_MAX];
    for (int32_t i = 0; i < order; i++)
        lsf_pred[i] = (float)enc->prev_lsf[i] / 5000.0f;

    /* Optional: dump raw LSF data for VQ training (env TLCS_DUMP_LSF=path) */
    {
        static FILE *dump_fp = NULL;
        static int dump_checked = 0;
        if (!dump_checked) {
            const char *p = getenv("TLCS_DUMP_LSF");
            if (p) dump_fp = fopen(p, "ab");
            dump_checked = 1;
        }
        if (dump_fp) {
            /* Write: [16 raw lsf] [16 lsf_pred] [16 delta] = 48 floats per frame */
            fwrite(lsf, sizeof(float), (size_t)order, dump_fp);
            fwrite(lsf_pred, sizeof(float), (size_t)order, dump_fp);
            float delta[TLCS_LPC_ORDER_MAX];
            for (int32_t i = 0; i < order; i++)
                delta[i] = lsf[i] - lsf_pred[i];
            fwrite(delta, sizeof(float), (size_t)order, dump_fp);
        }
    }

    float lsf_q[TLCS_LPC_ORDER_MAX];
    if (enc->cfg.use_lsf_vq) {
        /* Split-VQ path: quantize prediction delta */
        float delta_raw[TLCS_LPC_ORDER_MAX];
        for (int32_t i = 0; i < order; i++)
            delta_raw[i] = lsf[i] - lsf_pred[i];
        int32_t vq_idx[LSF_VQ_NUM_SPLITS];
        float delta_q[TLCS_LPC_ORDER_MAX];
        tlcs_lsf_vq_encode_n(delta_raw, order, vq_idx, delta_q,
                               1 << enc->cfg.lsf_bits);
        /* Store VQ indices in lsf_indices[0..3] */
        for (int32_t i = 0; i < LSF_VQ_NUM_SPLITS; i++)
            lsf_indices[i] = (int16_t)vq_idx[i];
        for (int32_t i = 0; i < order; i++)
            lsf_q[i] = lsf_pred[i] + delta_q[i];
    } else {
        /* Scalar predictive quantization path */
        tlcs_lsf_quantize_pred_n(lsf, lsf_pred, order, lsf_indices,
                                  lsf_bits, lsf_range);
        tlcs_lsf_dequantize_pred_n(lsf_indices, lsf_pred, order, lsf_q,
                                    lsf_bits, lsf_range);
    }
    tlcs_lsf_stabilize(lsf_q, order);

    float a_q[TLCS_LPC_ORDER_MAX + 1];
    tlcs_lsf_to_lpc(lsf_q, order, a_q);
    memcpy(a_q_out, a_q, (size_t)(order + 1) * sizeof(float));

    /* ── LSF interpolation ──────────────── */
    static const float lsf_alpha_8[8] = {0.55f, 0.65f, 0.75f, 0.85f, 0.92f, 0.96f, 1.0f, 1.0f};
    static const float lsf_alpha_4[4] = {0.75f, 0.90f, 1.0f, 1.0f};
    static const float lsf_alpha_2[2] = {0.50f, 1.0f};
    const float *lsf_alpha = (n_subfr <= 2) ? lsf_alpha_2
                            : (n_subfr <= 4) ? lsf_alpha_4 : lsf_alpha_8;
    float prev_lsf_f[TLCS_LPC_ORDER_MAX];
    for (int32_t i = 0; i < order; i++)
        prev_lsf_f[i] = lsf_pred[i];

    /* ── Per-subframe LPC residual ────────────────────── */
    float residual[TLCS_MAX_FRAME_SIZE];
    float ana_mem[TLCS_LPC_ORDER_MAX];
    memcpy(ana_mem, enc->synth_mem, (size_t)order * sizeof(float));

    for (int32_t sf_pre = 0; sf_pre < n_subfr; sf_pre++) {
        float alpha = lsf_alpha[sf_pre < n_subfr ? sf_pre : n_subfr - 1];
        float lsf_sf_pre[TLCS_LPC_ORDER_MAX];
        for (int32_t i = 0; i < order; i++)
            lsf_sf_pre[i] = (1.0f - alpha) * prev_lsf_f[i] + alpha * lsf_q[i];
        tlcs_lsf_stabilize(lsf_sf_pre, order);
        float a_sf_pre[TLCS_LPC_ORDER_MAX + 1];
        tlcs_lsf_to_lpc(lsf_sf_pre, order, a_sf_pre);
        tlcs_analysis_filter(a_sf_pre, order, &speech[sf_pre * subfr],
                             &residual[sf_pre * subfr], subfr, ana_mem);
    }

    /* ── Excitation buffer ──────────────────────────────── */
    float *exc = enc->exc_buf;
    tlcs_exc_buf_shift(exc, n);

    /* Shift innovation history buffer (same layout as exc_buf) */
    float *innov_hist = enc->innov_buf;
    memmove(innov_hist, innov_hist + n,
            (size_t)TLCS_MAX_PITCH_LAG * sizeof(float));
    memset(innov_hist + TLCS_MAX_PITCH_LAG, 0, (size_t)n * sizeof(float));

    /* Pre-fill excitation with residual for better ACB search.
     * Only for HR (subfr<=40) where lag >= subfr always holds,
     * avoiding enc/dec mismatch that destroys female voice quality. */
    if (subfr <= 40) {
        for (int32_t i = 0; i < n; i++)
            exc[TLCS_MAX_PITCH_LAG + i] = residual[i];
    }

    /* ── Open-loop pitch ────────────────────────────────── */
    int32_t ol_lag, ol_lag2;
    if (n_subfr <= 4) {
        /* VLR/LR: single OL search over full frame for robust estimate */
        float voicing1 = 0.0f;
        ol_lag = tlcs_pitch_ol_search(enc->speech_buf, n,
                                       min_lag, max_lag, &voicing1);
        ol_lag2 = ol_lag;
        (void)voicing1;
    } else {
        /* HR: two OL searches, one per half-frame */
        int32_t half = n / 2;
        float voicing1 = 0.0f;
        ol_lag = tlcs_pitch_ol_search(enc->speech_buf, half,
                                       min_lag, max_lag, &voicing1);
        float voicing2 = 0.0f;
        ol_lag2 = tlcs_pitch_ol_search(enc->speech_buf + half, half,
                                        min_lag, max_lag, &voicing2);
        (void)voicing1; (void)voicing2;
    }

    float ag1 = gamma1;
    float ag2 = gamma2;

    /* ── Codebook setup ────────────────────────────────── */
    tlcs_cb_config cb_cfg;
    tlcs_cb_config_init(&cb_cfg, subfr, num_pulses);

    float hw_synth_mem[TLCS_LPC_ORDER_MAX];
    float hw_wgt_mem[TLCS_LPC_ORDER_MAX];
    memcpy(hw_synth_mem, enc->wgt_synth_mem, (size_t)order * sizeof(float));
    memcpy(hw_wgt_mem, enc->wgt_mem, (size_t)order * sizeof(float));

    /* ── FFT perceptual filter for VLR/LR (SMPL-style Mel masking) ── */
    float b_perc[TLCS_MAX_SUBFR_SIZE];
    int32_t perc_order = order;  /* match LPC order for memory consistency */
    int use_perc = 0;
    if (1) {
        int32_t rc = tlcs_compute_perceptual_filter(
            enc->speech_buf + TLCS_MAX_PITCH_LAG, n,
            (float)(enc->cfg.sample_rate), b_perc, perc_order);
        if (rc == 0) use_perc = 1;
    }

    /* ── Per-subframe analysis-by-synthesis ─────────────── */
    float prev_cb_gain_mag = 0.0f;
    for (int32_t sf = 0; sf < n_subfr; sf++) {
        int32_t sf_offset = sf * subfr;
        int32_t exc_offset = TLCS_MAX_PITCH_LAG + sf_offset;
        float *target = &residual[sf_offset];

        /* Per-subframe LPC from interpolated LSF */
        float a_sf[TLCS_LPC_ORDER_MAX + 1];
        float a_g1[TLCS_LPC_ORDER_MAX + 1];
        float a_g2[TLCS_LPC_ORDER_MAX + 1];
        {
            float alpha = lsf_alpha[sf < n_subfr ? sf : n_subfr - 1];
            float lsf_sf[TLCS_LPC_ORDER_MAX];
            for (int32_t i = 0; i < order; i++)
                lsf_sf[i] = (1.0f - alpha) * prev_lsf_f[i] + alpha * lsf_q[i];
            tlcs_lsf_stabilize(lsf_sf, order);
            tlcs_lsf_to_lpc(lsf_sf, order, a_sf);
            tlcs_lpc_weight_coeffs(a_sf, order, ag1, a_g1);
            tlcs_lpc_weight_coeffs(a_sf, order, ag2, a_g2);
        }
        float h_w[TLCS_MAX_SUBFR_SIZE];
        if (use_perc) {
            /* FFT perceptual filter: h_w = B_perc(z) / A(z) */
            tlcs_perceptual_impulse_response(a_sf, order,
                                              b_perc, perc_order,
                                              h_w, subfr);
        } else {
            tlcs_cb_impulse_response_gamma(a_sf, order, h_w, subfr, ag1, ag2);
        }

        float synth_mem_save[TLCS_LPC_ORDER_MAX];
        memcpy(synth_mem_save, hw_synth_mem,
               (size_t)order * sizeof(float));

        /* ZIR of weighting filter */
        float zir_synth[TLCS_MAX_SUBFR_SIZE];
        float zero_input[TLCS_MAX_SUBFR_SIZE];
        memset(zero_input, 0, (size_t)subfr * sizeof(float));
        float sm_copy[TLCS_LPC_ORDER_MAX];
        memcpy(sm_copy, hw_synth_mem, (size_t)order * sizeof(float));
        tlcs_synthesis_filter(a_sf, order, zero_input,
                              zir_synth, subfr, sm_copy);

        float zir[TLCS_MAX_SUBFR_SIZE];
        if (use_perc) {
            /* ZIR for perceptual filter: apply B_perc(z) FIR to ZIR of 1/A(z)
             * Use synth_mem_save for past synthesis output at subframe boundary */
            for (int32_t j = 0; j < subfr; j++) {
                zir[j] = zir_synth[j]; /* b_perc[0] = 1.0 */
                int32_t kmax = (j < perc_order) ? j : perc_order;
                for (int32_t k = 1; k <= kmax; k++) {
                    float past;
                    if (j - k >= 0)
                        past = zir_synth[j - k];
                    else if (k - 1 - j < order)
                        past = synth_mem_save[k - 1 - j];
                    else
                        past = 0.0f;
                    zir[j] += b_perc[k] * past;
                }
            }
        } else {
            float zir_fir[TLCS_MAX_SUBFR_SIZE];
            for (int32_t j = 0; j < subfr; j++) {
                zir_fir[j] = zir_synth[j];
                for (int32_t k = 1; k <= order; k++) {
                    float past;
                    if (j - k >= 0)
                        past = zir_synth[j - k];
                    else
                        past = synth_mem_save[k - 1 - j];
                    zir_fir[j] += a_g1[k] * past;
                }
            }
            float wm_copy[TLCS_LPC_ORDER_MAX];
            memcpy(wm_copy, hw_wgt_mem, (size_t)order * sizeof(float));
            tlcs_synthesis_filter(a_g2, order, zir_fir,
                                  zir, subfr, wm_copy);
        }

        /* Weighted target = h_w * residual + ZIR */
        float w_target[TLCS_MAX_SUBFR_SIZE];
        for (int32_t j = 0; j < subfr; j++) {
            w_target[j] = zir[j];
            for (int32_t k = 0; k <= j; k++)
                w_target[j] += target[k] * h_w[j - k];
        }

        /* Fractional closed-loop pitch search */
        int32_t cl_lag, cl_frac;
        float pitch_gain;
        tlcs_pitch_cl_search_frac(w_target, h_w, exc, exc_offset,
                                   subfr, ol_lag, TLCS_PITCH_CL_DELTA,
                                   min_lag, max_lag,
                                   &cl_lag, &cl_frac, &pitch_gain);

        /* 2-tap ACB basis vectors */
        float v0[TLCS_MAX_SUBFR_SIZE], v1[TLCS_MAX_SUBFR_SIZE];
        tlcs_pitch_get_acb_basis(exc, exc_offset, cl_lag, cl_frac,
                                  v0, v1, subfr);

        /* Convolve basis vectors with h_w */
        float w_v0[TLCS_MAX_SUBFR_SIZE], w_v1[TLCS_MAX_SUBFR_SIZE];
        for (int32_t j = 0; j < subfr; j++) {
            w_v0[j] = 0.0f;
            w_v1[j] = 0.0f;
            for (int32_t k = 0; k <= j; k++) {
                w_v0[j] += v0[k] * h_w[j - k];
                w_v1[j] += v1[k] * h_w[j - k];
            }
        }

        /* Initial ACB VQ search for codebook search target */
        int32_t gi = tlcs_acb_vq_search(w_target, w_v0, w_v1, subfr);
        float g0 = tlcs_acb_vq[gi][0];
        float g1 = tlcs_acb_vq[gi][1];

        /* Innovation target */
        float w_innov[TLCS_MAX_SUBFR_SIZE];
            for (int32_t i = 0; i < subfr; i++)
                w_innov[i] = w_target[i] - g0 * w_v0[i] - g1 * w_v1[i];

            /* Pitch sharpening of innovation target (SMPL-style):
             * For voiced frames, add a fraction of the pitch-delayed innovation
             * to bias the codebook search toward pitch-harmonic positions. */
            if (cl_lag >= 20 && g0 > 0.4f && cl_lag < subfr) {
                float sharp = 0.3f * g0;  /* proportional to voicing */
                if (sharp > 0.35f) sharp = 0.35f;
                for (int32_t i = cl_lag; i < subfr; i++)
                    w_innov[i] += sharp * w_innov[i - cl_lag];
            }

            /* Algebraic codebook search with pitch-adaptive tilt */
            float h_w_tilt[TLCS_MAX_SUBFR_SIZE];
            float *h_search = h_w_tilt;
            {
                float w_innov_tilt[TLCS_MAX_SUBFR_SIZE];
                float tilt;
                if (subfr >= 80) {
                    /* LR mode: mild tilt to improve pitch tracking */
                    tilt = -0.15f;
                    if (g0 < 0.3f) tilt = 0.0f;  /* unvoiced: no tilt */
                } else {
                    tilt = -0.3f;
                    if (cl_lag < 60)       tilt = -0.10f;
                    else if (cl_lag < 90)  tilt = -0.20f;
                }
                float *h_base = h_w;
                w_innov_tilt[0] = w_innov[0];
                h_w_tilt[0] = h_base[0];
                for (int32_t i = 1; i < subfr; i++) {
                    w_innov_tilt[i] = w_innov[i] + tilt * w_innov[i - 1];
                    h_w_tilt[i] = h_base[i] + tilt * h_base[i - 1];
                }

                tlcs_cb_search_tree(w_innov_tilt, h_search, &cb_cfg, &cb_entries[sf]);
            }

            /* Joint gain optimization: 2-tap ACB VQ × CB gain */
            {
                float c_unit[TLCS_MAX_SUBFR_SIZE];
                memset(c_unit, 0, (size_t)subfr * sizeof(float));
                for (int32_t p = 0; p < cb_cfg.num_pulses; p++) {
                    int32_t abs_pos = p + cb_entries[sf].pulse_pos[p] * cb_cfg.num_pulses;
                    if (abs_pos < subfr)
                        c_unit[abs_pos] = (float)cb_entries[sf].pulse_sign[p];
                }

                float *h_gain = h_w;
                float w_cb[TLCS_MAX_SUBFR_SIZE];
                for (int32_t j = 0; j < subfr; j++) {
                    w_cb[j] = 0.0f;
                    for (int32_t k = 0; k <= j; k++)
                        w_cb[j] += c_unit[k] * h_gain[j - k];
                }

                /* Precompute inner products for 2-tap ACB + CB */
                float R00 = 0, R11 = 0, R01 = 0, Rcc = 0;
                float R0c = 0, R1c = 0;
                float tv0 = 0, tv1 = 0, tc = 0;
                for (int32_t j = 0; j < subfr; j++) {
                    R00 += w_v0[j] * w_v0[j];
                    R11 += w_v1[j] * w_v1[j];
                    R01 += w_v0[j] * w_v1[j];
                    Rcc += w_cb[j] * w_cb[j];
                    R0c += w_v0[j] * w_cb[j];
                    R1c += w_v1[j] * w_cb[j];
                    tv0 += w_target[j] * w_v0[j];
                    tv1 += w_target[j] * w_v1[j];
                    tc  += w_target[j] * w_cb[j];
                }

                /* Determine pulse sign orientation: check if optimal gc < 0 */
                if (Rcc > 1e-6f) {
                    float gc_test = (tc - g0 * R0c - g1 * R1c) / Rcc;
                    if (gc_test < 0.0f) {
                        tc = -tc;
                        R0c = -R0c;
                        R1c = -R1c;
                        for (int32_t p = 0; p < cb_cfg.num_pulses; p++)
                            cb_entries[sf].pulse_sign[p] = -cb_entries[sf].pulse_sign[p];
                    }
                }

                /* Initial CB gain estimate for search range */
                float gc_opt = 0.0f;
                if (Rcc > 1e-6f)
                    gc_opt = (tc - g0 * R0c - g1 * R1c) / Rcc;
                if (gc_opt < 0.0f) gc_opt = 0.0f;
                int32_t fcb_bits = enc->cfg.fcb_gain_bits;
                int32_t fcb_levels = 1 << fcb_bits;
                int32_t gci_center = tlcs_fcb_gain_quantize_pred_n(gc_opt, prev_cb_gain_mag, fcb_bits);

                int32_t gci_lo = gci_center - 5;
                int32_t gci_hi = gci_center + 5;
                if (gci_lo < 0) gci_lo = 0;
                if (gci_hi >= fcb_levels) gci_hi = fcb_levels - 1;

                /* Search all VQ entries × CB gain levels */
                float best_dist = 1e30f;
                int32_t best_gi = gi;
                int32_t best_gci = gci_center;

                for (int32_t vqi = 0; vqi < TLCS_ACB_VQ_SIZE; vqi++) {
                    float vg0 = tlcs_acb_vq[vqi][0];
                    float vg1 = tlcs_acb_vq[vqi][1];
                    for (int32_t gci = gci_lo; gci <= gci_hi; gci++) {
                        float gc = tlcs_fcb_gain_dequantize_pred_n(gci, prev_cb_gain_mag, fcb_bits);
                        float d = -2.0f*vg0*tv0 - 2.0f*vg1*tv1 - 2.0f*gc*tc
                                + vg0*vg0*R00 + vg1*vg1*R11 + gc*gc*Rcc
                                + 2.0f*vg0*vg1*R01 + 2.0f*vg0*gc*R0c + 2.0f*vg1*gc*R1c;
                        if (d < best_dist) {
                            best_dist = d;
                            best_gi = vqi;
                            best_gci = gci;
                        }
                    }
                }

                gi = best_gi;
                g0 = tlcs_acb_vq[gi][0];
                g1 = tlcs_acb_vq[gi][1];
                cb_entries[sf].gain_index = best_gci;
                cb_entries[sf].gain = tlcs_fcb_gain_dequantize_pred_n(best_gci, prev_cb_gain_mag, fcb_bits);
            }

        prev_cb_gain_mag = cb_entries[sf].gain;

        /* Build quantized innovation */
        float innovation[TLCS_MAX_SUBFR_SIZE];
        tlcs_cb_build_innovation(&cb_entries[sf], &cb_cfg, innovation, subfr);

        /* Pitch sharpening — must match decoder exactly.
         * LR/VLR (subfr>=80): SMPL-style forward sharpening at beta=0.95
         * for voiced frames. Applied within-subframe only.
         * HR (subfr<80): moderate cross-subframe sharpening. */
        float gq = g0;
        int32_t innov_off = TLCS_MAX_PITCH_LAG + sf_offset;
        for (int32_t i = 0; i < subfr; i++)
            innov_hist[innov_off + i] = innovation[i];

        if (cl_lag >= 20 && subfr < 80 && gq >= 0.6f) {
            /* HR: existing cross-subframe sharpening */
            float beta;
            if (cl_lag < subfr) {
                beta = gq * 0.4f;
                if (beta > 0.4f) beta = 0.4f;
            } else if (cl_lag < 100) {
                beta = gq * 0.20f;
                if (beta > 0.20f) beta = 0.20f;
            } else {
                beta = 0.0f;
            }
            if (beta > 0.0f) {
                for (int32_t i = 0; i < subfr; i++) {
                    int32_t src = innov_off + i - cl_lag;
                    if (src >= 0)
                        innovation[i] += beta * innov_hist[src];
                }
            }
        }

        /* Store sharpened innovation in history */
        for (int32_t i = 0; i < subfr; i++)
            innov_hist[innov_off + i] = innovation[i];

        /* Phase dispersion: spread sparse pulse energy.
         * Disabled for LR mode — hurts male voice PESQ (-0.14). */
        if (subfr < 80)
            tlcs_cb_phase_disperse(innovation, subfr, gq);

        /* Update excitation buffer with 2-tap ACB */
        for (int32_t i = 0; i < subfr; i++)
            exc[exc_offset + i] = g0 * v0[i] + g1 * v1[i] + innovation[i];

        /* Pitch periodicity enhancement: blend with past excitation
         * to improve periodicity for subsequent ACB predictions.
         * Must match decoder's enhancement to keep excitation in sync. */
        if (cl_lag >= 20 && g0 > 0.3f) {
            float blend_coeff, blend_max;
            if (subfr >= 80) {
                /* LR mode: match decoder's pitch-adaptive blend */
                blend_coeff = (cl_lag < 120) ? 0.18f : 0.08f;
                blend_max   = (cl_lag < 120) ? 0.18f : 0.08f;
            } else {
                /* HR mode: fixed blend */
                blend_coeff = 0.08f;
                blend_max   = 0.08f;
            }
            float blend = g0 * blend_coeff;
            if (blend > blend_max) blend = blend_max;
            for (int32_t i = 0; i < subfr; i++) {
                float past_exc = exc[exc_offset + i - cl_lag];
                exc[exc_offset + i] = (1.0f - blend) * exc[exc_offset + i]
                                    + blend * past_exc;
            }
        }

        /* Update H_w state */
        float err[TLCS_MAX_SUBFR_SIZE];
        for (int32_t i = 0; i < subfr; i++)
            err[i] = target[i] - exc[exc_offset + i];

        float err_synth[TLCS_MAX_SUBFR_SIZE];
        tlcs_synthesis_filter(a_sf, order, err, err_synth,
                              subfr, hw_synth_mem);

        if (!use_perc) {
            /* γ₁/γ₂ cascade: update wgt_mem (only for standard weighting) */
            float err_fir[TLCS_MAX_SUBFR_SIZE];
            for (int32_t j = 0; j < subfr; j++) {
                err_fir[j] = err_synth[j];
                for (int32_t k = 1; k <= order; k++) {
                    float past;
                    if (j - k >= 0)
                        past = err_synth[j - k];
                    else
                        past = synth_mem_save[k - 1 - j];
                    err_fir[j] += a_g1[k] * past;
                }
            }
            float err_wgt[TLCS_MAX_SUBFR_SIZE];
            tlcs_synthesis_filter(a_g2, order, err_fir, err_wgt,
                                  subfr, hw_wgt_mem);
        }
        /* FFT masking: B_perc is FIR — no separate state needed.
         * hw_synth_mem (1/A(z) state) already updated above. */

        pitch_lags[sf] = cl_lag;
        pitch_fracs[sf] = cl_frac;
        pitch_gain_indices[sf] = gi;
        /* Use second-half OL pitch at half-frame boundary */
        if (sf == n_subfr / 2 - 1)
            ol_lag = ol_lag2;
        else
            ol_lag = cl_lag;
    }

    /* Save state (all float precision) */
    memcpy(enc->synth_mem, ana_mem, (size_t)order * sizeof(float));
    memcpy(enc->wgt_synth_mem, hw_synth_mem, (size_t)order * sizeof(float));
    memcpy(enc->wgt_mem, hw_wgt_mem, (size_t)order * sizeof(float));

    int32_t exc_total = TLCS_MAX_PITCH_LAG + n;
    for (int32_t i = 0; i < exc_total; i++) {
        if (exc[i] > 32767.0f) exc[i] = 32767.0f;
        if (exc[i] < -32768.0f) exc[i] = -32768.0f;
    }

    /* Update prev LSFs (save prev→prev_prev before overwriting) */
    float lsf_q_final[TLCS_LPC_ORDER_MAX];
    if (enc->cfg.use_lsf_vq) {
        /* VQ: re-decode from indices to get exact quantized LSFs */
        int32_t vq_idx_final[LSF_VQ_NUM_SPLITS];
        for (int32_t i = 0; i < LSF_VQ_NUM_SPLITS; i++)
            vq_idx_final[i] = (int32_t)lsf_indices[i];
        float delta_q_final[TLCS_LPC_ORDER_MAX];
        tlcs_lsf_vq_decode_n(vq_idx_final, order, delta_q_final,
                               1 << enc->cfg.lsf_bits);
        for (int32_t i = 0; i < order; i++)
            lsf_q_final[i] = lsf_pred[i] + delta_q_final[i];
    } else {
        tlcs_lsf_dequantize_pred_n(lsf_indices, lsf_pred, order, lsf_q_final,
                                    lsf_bits, lsf_range);
    }
    tlcs_lsf_stabilize(lsf_q_final, order);
    for (int32_t i = 0; i < order; i++) {
        enc->prev_prev_lsf[i] = enc->prev_lsf[i];
        enc->prev_lsf[i] = (int16_t)(lsf_q_final[i] * 5000.0f);
    }
    enc->prev_pitch_lag = (int16_t)pitch_lags[n_subfr - 1];
}

/* ══════════════════════════════════════════════════════════════════
 *  Low-Rate 16kHz CELP Encoder (~9.6 kbps)
 *  4 subframes × 80 samples, LPC order 16, 2 ACELP pulses
 * ══════════════════════════════════════════════════════════════════ */

static tlcs_status encode_lowrate(tlcs_encoder *enc,
                                    const int16_t *pcm_in,
                                    uint8_t *bitstream_out,
                                    int32_t *bytes_written)
{
    const int32_t n       = enc->cfg.frame_size;       /* 320 */
    const int32_t order   = enc->cfg.lpc_order;        /* 10 */
    const int32_t subfr   = enc->cfg.subfr_size;       /* 80 */
    const int32_t n_subfr = enc->cfg.n_subfr;          /* 4 */

    /* ── Pre-emphasis ────────────────────────────────── */
    float speech[TLCS_MAX_FRAME_SIZE];
    float preemph_mem_f = (float)enc->preemph_mem;
    for (int32_t i = 0; i < n; i++) {
        float s = (float)pcm_in[i] - TLCS_PREEMPH_COEFF * preemph_mem_f;
        preemph_mem_f = (float)pcm_in[i];
        speech[i] = s;
    }
    enc->preemph_mem = (int16_t)preemph_mem_f;

    /* ── CELP core ───────────────────────────────────── */
    int16_t lsf_indices[TLCS_LPC_ORDER_MAX];
    int32_t pitch_lags[TLCS_MAX_SUBFRAMES];
    int32_t pitch_fracs[TLCS_MAX_SUBFRAMES];
    int32_t pitch_gain_indices[TLCS_MAX_SUBFRAMES];
    tlcs_cb_entry cb_entries[TLCS_MAX_SUBFRAMES];
    float a_q[TLCS_LPC_ORDER_MAX + 1];

    celp_encode_core(enc, speech, n, order, subfr, n_subfr,
                     TLCS_MIN_PITCH_LAG, TLCS_MAX_PITCH_LAG,
                     enc->cfg.num_pulses, TUNE_CELP_GAMMA1, TUNE_CELP_GAMMA2,
                     lsf_indices, pitch_lags, pitch_fracs,
                     pitch_gain_indices, cb_entries, a_q);

    /* ── Pack bitstream with LR bit widths ──────────── */
    tlcs_cb_config cb_cfg;
    tlcs_cb_config_init(&cb_cfg, subfr, enc->cfg.num_pulses);

    tlcs_bs_writer bsw;
    tlcs_bs_writer_init(&bsw, bitstream_out, TLCS_MAX_FRAME_BYTES);

    int32_t lsf_bits   = enc->cfg.lsf_bits;
    int32_t delta_bits  = enc->cfg.pitch_delta_bits;
    int32_t delta_off   = enc->cfg.pitch_delta_offset;
    int32_t fcb_bits    = enc->cfg.fcb_gain_bits;

    /* LSF indices */
    if (enc->cfg.use_lsf_vq) {
        /* VQ: 4 split indices × lsf_bits bits */
        int32_t vq_mask = (1 << lsf_bits) - 1;
        for (int32_t s = 0; s < LSF_VQ_NUM_SPLITS; s++)
            tlcs_bs_write(&bsw, (uint32_t)(lsf_indices[s] & vq_mask),
                          lsf_bits);
    } else {
        for (int32_t i = 0; i < order; i++)
            tlcs_bs_write(&bsw, (uint32_t)(lsf_indices[i] & ((1 << lsf_bits) - 1)),
                          lsf_bits);
    }

    /* Per-subframe data */
    int32_t prev_lag_idx = 0;
    for (int32_t sf = 0; sf < n_subfr; sf++) {
        int32_t lag_idx = tlcs_pitch_encode_lag(pitch_lags[sf],
                                                 pitch_fracs[sf]);
        if (sf == 0) {
            /* Absolute lag: 9 bits */
            tlcs_bs_write(&bsw, (uint32_t)lag_idx, 9);
        } else {
            /* Delta lag */
            int32_t delta = lag_idx - prev_lag_idx;
            if (delta < -delta_off) delta = -delta_off;
            if (delta > delta_off - 1) delta = delta_off - 1;
            tlcs_bs_write(&bsw, (uint32_t)(delta + delta_off), delta_bits);
        }
        prev_lag_idx = lag_idx;

        tlcs_bs_write(&bsw, (uint32_t)pitch_gain_indices[sf],
                      TLCS_PITCH_GAIN_BITS);
        for (int32_t p = 0; p < cb_cfg.num_pulses; p++) {
            if (cb_cfg.pos_bits > 0)
                tlcs_bs_write(&bsw,
                              (uint32_t)cb_entries[sf].pulse_pos[p],
                              (int32_t)cb_cfg.pos_bits);
            tlcs_bs_write(&bsw,
                          (cb_entries[sf].pulse_sign[p] > 0) ? 1u : 0u, 1);
        }
        tlcs_bs_write(&bsw,
                      (uint32_t)cb_entries[sf].gain_index,
                      fcb_bits);
    }

    *bytes_written = tlcs_bs_writer_flush(&bsw);
    enc->frame_count++;

    return TLCS_OK;
}

/* ══════════════════════════════════════════════════════════════════
 *  Entropy-Coded Low-Rate 16kHz CELP Encoder
 *  Same parameters as encode_lowrate() but packed with range coder.
 * ══════════════════════════════════════════════════════════════════ */

static tlcs_status encode_lowrate_ec(tlcs_encoder *enc,
                                      const int16_t *pcm_in,
                                      uint8_t *bitstream_out,
                                      int32_t *bytes_written)
{
    const int32_t n       = enc->cfg.frame_size;       /* 320 */
    const int32_t order   = enc->cfg.lpc_order;        /* 16 */
    const int32_t subfr   = enc->cfg.subfr_size;       /* 80 */
    const int32_t n_subfr = enc->cfg.n_subfr;          /* 4 */

    /* ── Pre-emphasis ────────────────────────────────── */
    float speech[TLCS_MAX_FRAME_SIZE];
    float preemph_mem_f = (float)enc->preemph_mem;
    for (int32_t i = 0; i < n; i++) {
        float s = (float)pcm_in[i] - TLCS_PREEMPH_COEFF * preemph_mem_f;
        preemph_mem_f = (float)pcm_in[i];
        speech[i] = s;
    }
    enc->preemph_mem = (int16_t)preemph_mem_f;

    /* ── CELP core (identical to encode_lowrate) ────── */
    int16_t lsf_indices[TLCS_LPC_ORDER_MAX];
    int32_t pitch_lags[TLCS_MAX_SUBFRAMES];
    int32_t pitch_fracs[TLCS_MAX_SUBFRAMES];
    int32_t pitch_gain_indices[TLCS_MAX_SUBFRAMES];
    tlcs_cb_entry cb_entries[TLCS_MAX_SUBFRAMES];
    float a_q[TLCS_LPC_ORDER_MAX + 1];

    celp_encode_core(enc, speech, n, order, subfr, n_subfr,
                     TLCS_MIN_PITCH_LAG, TLCS_MAX_PITCH_LAG,
                     enc->cfg.num_pulses, TUNE_CELP_GAMMA1, TUNE_CELP_GAMMA2,
                     lsf_indices, pitch_lags, pitch_fracs,
                     pitch_gain_indices, cb_entries, a_q);

    /* ── Optional: dump parameter indices for CDF training ──── */
    {
        static FILE *ec_dump_fp = NULL;
        static int ec_dump_checked = 0;
        if (!ec_dump_checked) {
            const char *p = getenv("TLCS_DUMP_EC");
            if (p) ec_dump_fp = fopen(p, "ab");
            ec_dump_checked = 1;
        }
        if (ec_dump_fp) {
            /* Format per frame: [16 lsf_idx] [4 lag_idx] [4 acb_vq] [4×2 pulse_pos]
               [4×2 pulse_sign] [4 fcb_gain] = 16+4+4+8+8+4 = 44 int16 */
            int16_t buf[64];
            int bpos = 0;
            int32_t lsf_mask = (1 << enc->cfg.lsf_bits) - 1;
            for (int32_t i = 0; i < order; i++)
                buf[bpos++] = (int16_t)(lsf_indices[i] & lsf_mask);
            for (int32_t sf = 0; sf < n_subfr; sf++) {
                int32_t lag_idx = tlcs_pitch_encode_lag(pitch_lags[sf], pitch_fracs[sf]);
                buf[bpos++] = (int16_t)lag_idx;
            }
            for (int32_t sf = 0; sf < n_subfr; sf++)
                buf[bpos++] = (int16_t)pitch_gain_indices[sf];
            tlcs_cb_config tmp_cb;
            tlcs_cb_config_init(&tmp_cb, subfr, enc->cfg.num_pulses);
            for (int32_t sf = 0; sf < n_subfr; sf++) {
                for (int32_t p = 0; p < tmp_cb.num_pulses; p++)
                    buf[bpos++] = (int16_t)cb_entries[sf].pulse_pos[p];
            }
            for (int32_t sf = 0; sf < n_subfr; sf++) {
                for (int32_t p = 0; p < tmp_cb.num_pulses; p++)
                    buf[bpos++] = (int16_t)((cb_entries[sf].pulse_sign[p] > 0) ? 1 : 0);
            }
            for (int32_t sf = 0; sf < n_subfr; sf++)
                buf[bpos++] = (int16_t)cb_entries[sf].gain_index;
            fwrite(buf, sizeof(int16_t), (size_t)bpos, ec_dump_fp);
        }
    }

    /* ── V/UV detection based on average ACB gain ──── */
    float avg_g0 = 0.0f;
    for (int32_t sf = 0; sf < n_subfr; sf++)
        avg_g0 += tlcs_acb_vq[pitch_gain_indices[sf]][0];
    avg_g0 /= (float)n_subfr;
    int32_t voiced = (avg_g0 >= 0.25f) ? 1 : 0;


    /* ── Pack with range coder ──────────────────────── */
    tlcs_cb_config cb_cfg;
    tlcs_cb_config_init(&cb_cfg, subfr, enc->cfg.num_pulses);

    int32_t delta_off = enc->cfg.pitch_delta_offset;

    tlcs_rc_encoder rc;
    tlcs_rc_enc_init(&rc, bitstream_out, TLCS_MAX_FRAME_BYTES);

    /* V/UV flag */
    tlcs_rc_enc_symbol(&rc, voiced, ec_cdf_vuv, EC_N_VUV);

    /* LSF indices */
    if (enc->cfg.use_lsf_vq) {
        /* VQ: 4 split indices, uniform CDF */
        int32_t vq_size = 1 << enc->cfg.lsf_bits;
        for (int32_t s = 0; s < LSF_VQ_NUM_SPLITS; s++)
            tlcs_rc_enc_uniform(&rc, (int32_t)lsf_indices[s],
                                vq_size);
    } else {
        /* Scalar: 16 indices via Laplacian model */
        const uint16_t *lsf_cdf = (enc->cfg.lsf_bits == 7) ? ec_cdf_lsf_delta_7bit
                                : (enc->cfg.lsf_bits == 6) ? ec_cdf_lsf_delta_6bit
                                : ec_cdf_lsf_delta;
        int32_t n_lsf_syms = (1 << enc->cfg.lsf_bits);
        for (int32_t i = 0; i < order; i++) {
            int32_t sym = lsf_indices[i] & (n_lsf_syms - 1);
            tlcs_rc_enc_symbol(&rc, sym, lsf_cdf, n_lsf_syms);
        }
    }

    /* Per-subframe data */
    int32_t prev_lag_idx = 0;
    int32_t half_frame = n_subfr / 2;
    for (int32_t sf = 0; sf < n_subfr; sf++) {
        if (voiced) {
            /* Voiced: encode pitch lag and ACB VQ */
            int32_t lag_idx = tlcs_pitch_encode_lag(pitch_lags[sf],
                                                     pitch_fracs[sf]);
            if (sf == 0 || sf == half_frame) {
                /* Absolute lag at half-frame boundaries */
                tlcs_rc_enc_symbol(&rc, lag_idx, ec_cdf_pitch_abs, EC_N_PITCH_ABS);
            } else {
                int32_t delta = lag_idx - prev_lag_idx;
                if (delta < -delta_off) delta = -delta_off;
                if (delta > delta_off - 1) delta = delta_off - 1;
                int32_t sym = delta + delta_off;
                {
                    const uint16_t *pd_cdf = (enc->cfg.pitch_delta_bits == 6)
                                           ? ec_cdf_pitch_delta_6bit : ec_cdf_pitch_delta;
                    int32_t n_pd = (1 << enc->cfg.pitch_delta_bits);
                    tlcs_rc_enc_symbol(&rc, sym, pd_cdf, n_pd);
                }
            }
            prev_lag_idx = lag_idx;

            /* ACB VQ index */
            tlcs_rc_enc_symbol(&rc, pitch_gain_indices[sf],
                               ec_cdf_acb_vq, EC_N_ACB_VQ);
        }
        /* Unvoiced: skip pitch and ACB — saves ~33 bits */

        /* Pulse positions and signs */
        for (int32_t p = 0; p < cb_cfg.num_pulses; p++) {
            tlcs_rc_enc_uniform(&rc, cb_entries[sf].pulse_pos[p],
                                cb_cfg.positions_per_track);
            int32_t sign_sym = (cb_entries[sf].pulse_sign[p] > 0) ? 1 : 0;
            tlcs_rc_enc_uniform(&rc, sign_sym, 2);
        }

        /* FCB gain (select CDF by bit depth) */
        {
            const uint16_t *fcb_cdf = (enc->cfg.fcb_gain_bits == 6)
                                    ? ec_cdf_fcb_gain_6bit : ec_cdf_fcb_gain;
            int32_t n_fcb = (1 << enc->cfg.fcb_gain_bits);
            tlcs_rc_enc_symbol(&rc, cb_entries[sf].gain_index,
                               fcb_cdf, n_fcb);
        }
    }

    *bytes_written = tlcs_rc_enc_flush(&rc);
    enc->frame_count++;

    return TLCS_OK;
}

/* ══════════════════════════════════════════════════════════════════
 *  Direct 16kHz CELP Encoder (no band split)
 * ══════════════════════════════════════════════════════════════════ */

static tlcs_status encode_direct(tlcs_encoder *enc,
                                  const int16_t *pcm_in,
                                  uint8_t *bitstream_out,
                                  int32_t *bytes_written)
{
    const int32_t n     = enc->cfg.frame_size;     /* 320 */
    const int32_t order = enc->cfg.lpc_order;      /* 16 */
    const int32_t subfr = enc->cfg.subfr_size;     /* 40 */
    const int32_t n_subfr = enc->cfg.n_subfr;      /* 8 */

    /* ── Pre-emphasis ────────────────────────────────── */
    float speech[TLCS_MAX_FRAME_SIZE];
    float preemph_mem_f = (float)enc->preemph_mem;
    for (int32_t i = 0; i < n; i++) {
        float s = (float)pcm_in[i] - TLCS_PREEMPH_COEFF * preemph_mem_f;
        preemph_mem_f = (float)pcm_in[i];
        speech[i] = s;
    }
    enc->preemph_mem = (int16_t)preemph_mem_f;

    /* ── CELP core ───────────────────────────────────── */
    int16_t lsf_indices[TLCS_LPC_ORDER_MAX];
    int32_t pitch_lags[TLCS_MAX_SUBFRAMES];
    int32_t pitch_fracs[TLCS_MAX_SUBFRAMES];
    int32_t pitch_gain_indices[TLCS_MAX_SUBFRAMES];
    tlcs_cb_entry cb_entries[TLCS_MAX_SUBFRAMES];
    float a_q[TLCS_LPC_ORDER_MAX + 1];

    celp_encode_core(enc, speech, n, order, subfr, n_subfr,
                     TLCS_MIN_PITCH_LAG, TLCS_MAX_PITCH_LAG,
                     enc->cfg.num_pulses, 0.96f, 0.50f,
                     lsf_indices, pitch_lags, pitch_fracs,
                     pitch_gain_indices, cb_entries, a_q);

    /* ── Pack bitstream ──────────────────────────────── */
    tlcs_cb_config cb_cfg;
    tlcs_cb_config_init(&cb_cfg, subfr, enc->cfg.num_pulses);

    int32_t lsf_bits   = enc->cfg.lsf_bits;
    int32_t delta_bits  = enc->cfg.pitch_delta_bits;
    int32_t delta_off   = enc->cfg.pitch_delta_offset;
    int32_t fcb_bits    = enc->cfg.fcb_gain_bits;

    tlcs_bs_writer bsw;
    tlcs_bs_writer_init(&bsw, bitstream_out, TLCS_MAX_FRAME_BYTES);

    /* LSF indices */
    if (enc->cfg.use_lsf_vq) {
        int32_t vq_mask = (1 << lsf_bits) - 1;
        for (int32_t s = 0; s < LSF_VQ_NUM_SPLITS; s++)
            tlcs_bs_write(&bsw, (uint32_t)(lsf_indices[s] & vq_mask),
                          lsf_bits);
    } else {
        for (int32_t i = 0; i < order; i++)
            tlcs_bs_write(&bsw, (uint32_t)(lsf_indices[i] & ((1 << lsf_bits) - 1)),
                          lsf_bits);
    }

    /* Per-subframe data (delta pitch coding: absolute at half-frame starts) */
    int32_t prev_lag_idx = 0;
    for (int32_t sf = 0; sf < n_subfr; sf++) {
        int32_t lag_idx = tlcs_pitch_encode_lag(pitch_lags[sf],
                                                 pitch_fracs[sf]);
        if (sf == 0 || sf == n_subfr / 2) {
            /* Absolute lag: 9 bits */
            tlcs_bs_write(&bsw, (uint32_t)lag_idx, 9);
        } else {
            /* Delta lag */
            int32_t delta = lag_idx - prev_lag_idx;
            if (delta < -delta_off) delta = -delta_off;
            if (delta > delta_off - 1) delta = delta_off - 1;
            tlcs_bs_write(&bsw, (uint32_t)(delta + delta_off), delta_bits);
        }
        prev_lag_idx = lag_idx;

        tlcs_bs_write(&bsw, (uint32_t)pitch_gain_indices[sf],
                      TLCS_PITCH_GAIN_BITS);
        for (int32_t p = 0; p < cb_cfg.num_pulses; p++) {
            if (cb_cfg.pos_bits > 0)
                tlcs_bs_write(&bsw,
                              (uint32_t)cb_entries[sf].pulse_pos[p],
                              (int32_t)cb_cfg.pos_bits);
            tlcs_bs_write(&bsw,
                          (cb_entries[sf].pulse_sign[p] > 0) ? 1u : 0u, 1);
        }
        tlcs_bs_write(&bsw,
                      (uint32_t)cb_entries[sf].gain_index,
                      fcb_bits);
    }

    *bytes_written = tlcs_bs_writer_flush(&bsw);
    enc->frame_count++;

    return TLCS_OK;
}

/* ══════════════════════════════════════════════════════════════════
 *  Entropy-Coded HR 16kHz CELP Encoder
 *  Same parameters as encode_direct() but packed with range coder.
 * ══════════════════════════════════════════════════════════════════ */

static tlcs_status encode_direct_ec(tlcs_encoder *enc,
                                     const int16_t *pcm_in,
                                     uint8_t *bitstream_out,
                                     int32_t *bytes_written)
{
    const int32_t n       = enc->cfg.frame_size;     /* 320 */
    const int32_t order   = enc->cfg.lpc_order;      /* 16 */
    const int32_t subfr   = enc->cfg.subfr_size;     /* 40 */
    const int32_t n_subfr = enc->cfg.n_subfr;        /* 8 */

    /* ── Pre-emphasis ────────────────────────────────── */
    float speech[TLCS_MAX_FRAME_SIZE];
    float preemph_mem_f = (float)enc->preemph_mem;
    for (int32_t i = 0; i < n; i++) {
        float s = (float)pcm_in[i] - TLCS_PREEMPH_COEFF * preemph_mem_f;
        preemph_mem_f = (float)pcm_in[i];
        speech[i] = s;
    }
    enc->preemph_mem = (int16_t)preemph_mem_f;

    /* ── CELP core ───────────────────────────────────── */
    int16_t lsf_indices[TLCS_LPC_ORDER_MAX];
    int32_t pitch_lags[TLCS_MAX_SUBFRAMES];
    int32_t pitch_fracs[TLCS_MAX_SUBFRAMES];
    int32_t pitch_gain_indices[TLCS_MAX_SUBFRAMES];
    tlcs_cb_entry cb_entries[TLCS_MAX_SUBFRAMES];
    float a_q[TLCS_LPC_ORDER_MAX + 1];

    celp_encode_core(enc, speech, n, order, subfr, n_subfr,
                     TLCS_MIN_PITCH_LAG, TLCS_MAX_PITCH_LAG,
                     enc->cfg.num_pulses, 0.96f, 0.50f,
                     lsf_indices, pitch_lags, pitch_fracs,
                     pitch_gain_indices, cb_entries, a_q);

    /* ── V/UV detection based on average ACB gain ──── */
    float avg_g0 = 0.0f;
    for (int32_t sf = 0; sf < n_subfr; sf++)
        avg_g0 += tlcs_acb_vq[pitch_gain_indices[sf]][0];
    avg_g0 /= (float)n_subfr;
    int32_t voiced = (avg_g0 >= 0.25f) ? 1 : 0;

    /* ── Pack with range coder ──────────────────────── */
    tlcs_cb_config cb_cfg;
    tlcs_cb_config_init(&cb_cfg, subfr, enc->cfg.num_pulses);

    int32_t lsf_bits  = enc->cfg.lsf_bits;
    int32_t delta_off  = enc->cfg.pitch_delta_offset;
    int32_t delta_bits = enc->cfg.pitch_delta_bits;
    int32_t fcb_bits   = enc->cfg.fcb_gain_bits;

    tlcs_rc_encoder rc;
    tlcs_rc_enc_init(&rc, bitstream_out, TLCS_MAX_FRAME_BYTES);

    /* V/UV flag */
    tlcs_rc_enc_symbol(&rc, voiced, ec_cdf_vuv, EC_N_VUV);

    /* LSF indices */
    if (enc->cfg.use_lsf_vq) {
        int32_t vq_size = 1 << lsf_bits;
        for (int32_t s = 0; s < LSF_VQ_NUM_SPLITS; s++)
            tlcs_rc_enc_uniform(&rc, (int32_t)lsf_indices[s],
                                vq_size);
    } else {
        const uint16_t *lsf_cdf = (lsf_bits == 7) ? ec_cdf_lsf_delta_7bit
                                 : (lsf_bits == 6) ? ec_cdf_lsf_delta_6bit
                                 : ec_cdf_lsf_delta;
        int32_t n_lsf_syms = (1 << lsf_bits);
        for (int32_t i = 0; i < order; i++) {
            int32_t sym = lsf_indices[i] & (n_lsf_syms - 1);
            tlcs_rc_enc_symbol(&rc, sym, lsf_cdf, n_lsf_syms);
        }
    }

    /* Per-subframe data */
    int32_t prev_lag_idx = 0;
    int32_t half_frame = n_subfr / 2;
    for (int32_t sf = 0; sf < n_subfr; sf++) {
        if (voiced) {
            int32_t lag_idx = tlcs_pitch_encode_lag(pitch_lags[sf],
                                                     pitch_fracs[sf]);
            if (sf == 0 || sf == half_frame) {
                /* Absolute lag at half-frame boundaries */
                tlcs_rc_enc_symbol(&rc, lag_idx, ec_cdf_pitch_abs,
                                   EC_N_PITCH_ABS);
            } else {
                int32_t delta = lag_idx - prev_lag_idx;
                if (delta < -delta_off) delta = -delta_off;
                if (delta > delta_off - 1) delta = delta_off - 1;
                int32_t sym = delta + delta_off;
                const uint16_t *pd_cdf = (delta_bits == 7)
                    ? ec_cdf_pitch_delta_7bit
                    : (delta_bits == 6) ? ec_cdf_pitch_delta_6bit
                    : ec_cdf_pitch_delta;
                int32_t n_pd = (1 << delta_bits);
                tlcs_rc_enc_symbol(&rc, sym, pd_cdf, n_pd);
            }
            prev_lag_idx = lag_idx;

            /* ACB VQ index */
            tlcs_rc_enc_symbol(&rc, pitch_gain_indices[sf],
                               ec_cdf_acb_vq, EC_N_ACB_VQ);
        }
        /* Unvoiced: skip pitch and ACB */

        /* Pulse positions and signs */
        for (int32_t p = 0; p < cb_cfg.num_pulses; p++) {
            tlcs_rc_enc_uniform(&rc, cb_entries[sf].pulse_pos[p],
                                cb_cfg.positions_per_track);
            int32_t sign_sym = (cb_entries[sf].pulse_sign[p] > 0) ? 1 : 0;
            tlcs_rc_enc_uniform(&rc, sign_sym, 2);
        }

        /* FCB gain (select CDF by bit depth) */
        {
            const uint16_t *fcb_cdf = (fcb_bits == 7) ? ec_cdf_fcb_gain_7bit
                                    : (fcb_bits == 6) ? ec_cdf_fcb_gain_6bit
                                    : ec_cdf_fcb_gain;
            int32_t n_fcb = (1 << fcb_bits);
            tlcs_rc_enc_symbol(&rc, cb_entries[sf].gain_index,
                               fcb_cdf, n_fcb);
        }
    }

    *bytes_written = tlcs_rc_enc_flush(&rc);
    enc->frame_count++;

    return TLCS_OK;
}

/* ══════════════════════════════════════════════════════════════════
 *  WB Band-Split Encoder (16kHz → LB CELP + HB envelope)
 * ══════════════════════════════════════════════════════════════════ */

static tlcs_status encode_wb_bandsplit(tlcs_encoder *enc,
                                        const int16_t *pcm_in,
                                        uint8_t *bitstream_out,
                                        int32_t *bytes_written)
{
    const int32_t n_full = 160;   /* full-band frame */
    const int32_t n_lb   = TLCS_LB_FRAME;   /* 80 */
    const int32_t order  = TLCS_LB_ORDER;    /* 10 */
    const int32_t subfr  = TLCS_LB_SUBFR;    /* 20 */
    const int32_t n_subfr = TLCS_LB_NSUBFR;  /* 4 */

    /* ── Step 1: Convert to float and QMF split ────────── */
    float input_f[TLCS_MAX_FRAME_SIZE];
    for (int32_t i = 0; i < n_full; i++)
        input_f[i] = (float)pcm_in[i];

    float lb[TLCS_MAX_FRAME_SIZE / 2];
    float hb[TLCS_MAX_FRAME_SIZE / 2];
    tlcs_qmf_analyze(enc->qmf_ana_mem, input_f, n_full, lb, hb);

    /* ── Step 2: Pre-emphasis on LB ────────────────────── */
    float speech[TLCS_MAX_FRAME_SIZE];
    float preemph_mem_f = (float)enc->preemph_mem;
    for (int32_t i = 0; i < n_lb; i++) {
        float s = lb[i] - TLCS_PREEMPH_COEFF * preemph_mem_f;
        preemph_mem_f = lb[i];
        speech[i] = s;
    }
    enc->preemph_mem = (int16_t)preemph_mem_f;

    /* ── Step 3: LB CELP encoding ──────────────────────── */
    int16_t lsf_indices[TLCS_LPC_ORDER_MAX];
    int32_t pitch_lags[TLCS_SUBFRAMES];
    int32_t pitch_fracs[TLCS_SUBFRAMES];
    int32_t pitch_gain_indices[TLCS_SUBFRAMES];
    tlcs_cb_entry cb_entries[TLCS_SUBFRAMES];
    float a_q[TLCS_LPC_ORDER_MAX + 1];

    celp_encode_core(enc, speech, n_lb, order, subfr, n_subfr,
                     TLCS_LB_MIN_LAG, TLCS_LB_MAX_LAG,
                     TLCS_LB_NUM_PULSES, 0.96f, 0.50f,
                     lsf_indices, pitch_lags, pitch_fracs,
                     pitch_gain_indices, cb_entries, a_q);

    /* ── Step 4: HB envelope encoding ──────────────────── */
    /* HB LPC analysis for spectral envelope */
    float hb_windowed[TLCS_MAX_FRAME_SIZE / 2];
    tlcs_hamming_window(hb, hb_windowed, n_lb);

    float hb_r[TLCS_HB_ORDER + 1];
    tlcs_autocorrelation(hb_windowed, n_lb, hb_r, TLCS_HB_ORDER);

    float hb_a[TLCS_HB_ORDER + 1];
    tlcs_levinson(hb_r, TLCS_HB_ORDER, hb_a, NULL);

    float hb_lsf[TLCS_HB_ORDER];
    if (tlcs_lpc_to_lsf(hb_a, TLCS_HB_ORDER, hb_lsf) != 0) {
        /* Fallback to previous or default */
        init_default_lsf(hb_lsf, TLCS_HB_ORDER);
    }
    tlcs_lsf_stabilize(hb_lsf, TLCS_HB_ORDER);

    /* Quantize HB LSFs: 4 bits each (16 levels in [0, pi]) */
    int32_t hb_lsf_idx[TLCS_HB_ORDER];
    for (int32_t i = 0; i < TLCS_HB_ORDER; i++) {
        int32_t idx = (int32_t)(hb_lsf[i] * 15.0f / (float)M_PI + 0.5f);
        if (idx < 0) idx = 0;
        if (idx > 15) idx = 15;
        hb_lsf_idx[i] = idx;
    }

    /* Compute HB/LB energy ratio and quantize */
    float lb_energy = 0.0f, hb_energy = 0.0f;
    for (int32_t i = 0; i < n_lb; i++) {
        lb_energy += lb[i] * lb[i];
        hb_energy += hb[i] * hb[i];
    }
    float log_ratio = logf((hb_energy + 1.0f) / (lb_energy + 1.0f));
    /* Map [-8, 2] to [0, 31] */
    int32_t energy_idx = (int32_t)((log_ratio + 8.0f) * 31.0f / 10.0f + 0.5f);
    if (energy_idx < 0) energy_idx = 0;
    if (energy_idx > 31) energy_idx = 31;

    /* ── Step 5: Pack bitstream ────────────────────────── */
    tlcs_cb_config cb_cfg;
    tlcs_cb_config_init(&cb_cfg, subfr, TLCS_LB_NUM_PULSES);

    tlcs_bs_writer bsw;
    tlcs_bs_writer_init(&bsw, bitstream_out, TLCS_MAX_FRAME_BYTES);

    /* LB LSF indices */
    for (int32_t i = 0; i < order; i++)
        tlcs_bs_write(&bsw, (uint32_t)(lsf_indices[i] & ((1 << TLCS_LSF_BITS) - 1)),
                      TLCS_LSF_BITS);

    /* LB per-subframe data (delta pitch coding) */
    int32_t prev_lag_idx_wb = 0;
    for (int32_t sf = 0; sf < n_subfr; sf++) {
        /* Pitch lag: absolute or delta */
        int32_t lag_idx = tlcs_pitch_encode_lag(pitch_lags[sf],
                                                 pitch_fracs[sf]);
        if (sf == 0 || sf == n_subfr / 2) {
            tlcs_bs_write(&bsw, (uint32_t)lag_idx, 9);
        } else {
            int32_t delta = lag_idx - prev_lag_idx_wb;
            if (delta < -TLCS_PITCH_DELTA_OFFSET) delta = -TLCS_PITCH_DELTA_OFFSET;
            if (delta > TLCS_PITCH_DELTA_OFFSET - 1) delta = TLCS_PITCH_DELTA_OFFSET - 1;
            tlcs_bs_write(&bsw, (uint32_t)(delta + TLCS_PITCH_DELTA_OFFSET),
                          TLCS_PITCH_DELTA_BITS);
        }
        prev_lag_idx_wb = lag_idx;

        /* Pitch gain: 4 bits */
        tlcs_bs_write(&bsw, (uint32_t)pitch_gain_indices[sf],
                      TLCS_PITCH_GAIN_BITS);

        /* Codebook pulses */
        for (int32_t p = 0; p < cb_cfg.num_pulses; p++) {
            if (cb_cfg.pos_bits > 0)
                tlcs_bs_write(&bsw,
                              (uint32_t)cb_entries[sf].pulse_pos[p],
                              (int32_t)cb_cfg.pos_bits);
            tlcs_bs_write(&bsw,
                          (cb_entries[sf].pulse_sign[p] > 0) ? 1u : 0u, 1);
        }

        /* Fixed codebook gain: 7 bits */
        tlcs_bs_write(&bsw,
                      (uint32_t)cb_entries[sf].gain_index,
                      TLCS_FCB_GAIN_BITS);
    }

    /* HB LSF indices: 4 bits each */
    for (int32_t i = 0; i < TLCS_HB_ORDER; i++)
        tlcs_bs_write(&bsw, (uint32_t)hb_lsf_idx[i], TLCS_HB_LSF_BITS);

    /* HB energy ratio: 5 bits */
    tlcs_bs_write(&bsw, (uint32_t)energy_idx, TLCS_HB_ENERGY_BITS);

    *bytes_written = tlcs_bs_writer_flush(&bsw);
    enc->frame_count++;

    return TLCS_OK;
}

/* ══════════════════════════════════════════════════════════════════
 *  TCX Encoder (Mode T)
 *  Pre-emphasis -> LPC -> MDCT -> spectral quantization -> range coding.
 *  Called with bsw positioned after mode bit.
 * ══════════════════════════════════════════════════════════════════ */

static tlcs_status encode_tcx(tlcs_encoder *enc,
                               const int16_t *pcm_in,
                               tlcs_bs_writer *bsw,
                               uint8_t *bitstream_out,
                               int32_t *bytes_written)
{
    const int32_t n     = enc->cfg.frame_size;
    const int32_t order = enc->cfg.lpc_order;

    /* ── Pre-emphasis ────────────────────────────────── */
    float speech[TLCS_MAX_FRAME_SIZE];
    float preemph_mem_f = (float)enc->preemph_mem;
    for (int32_t i = 0; i < n; i++) {
        float s = (float)pcm_in[i] - TLCS_PREEMPH_COEFF * preemph_mem_f;
        preemph_mem_f = (float)pcm_in[i];
        speech[i] = s;
    }
    enc->preemph_mem = (int16_t)preemph_mem_f;

    /* ── Speech history update (for LPC window) ────── */
    memmove(enc->speech_buf,
            enc->speech_buf + n,
            (size_t)TLCS_MAX_PITCH_LAG * sizeof(float));
    memcpy(enc->speech_buf + TLCS_MAX_PITCH_LAG,
           speech, (size_t)n * sizeof(float));

    /* ── LPC analysis with asymmetric window ───────── */
    int32_t win_prev = n / 5;
    int32_t win_ones_init = n * 3 / 8;
    int32_t win1_len = win_prev + n - win_ones_init;
    int32_t win3_len = n / 10;
    int32_t win_ones = (win_prev + n) - win1_len - win3_len;
    int32_t ana_len = win1_len + win_ones + win3_len;

    float extended[TLCS_MAX_PITCH_LAG + TLCS_MAX_FRAME_SIZE];
    memcpy(extended, &enc->speech_buf[TLCS_MAX_PITCH_LAG - win_prev],
           (size_t)ana_len * sizeof(float));

    float windowed[TLCS_MAX_PITCH_LAG + TLCS_MAX_FRAME_SIZE];
    for (int32_t i = 0; i < win1_len; i++) {
        float w = sinf((float)(i + 1) / (float)(win1_len + 1) * (float)M_PI * 0.5f);
        windowed[i] = extended[i] * w;
    }
    for (int32_t i = 0; i < win_ones; i++)
        windowed[win1_len + i] = extended[win1_len + i];
    for (int32_t i = 0; i < win3_len; i++) {
        float w = cosf((float)(i + 1) / (float)(win3_len + 1) * (float)M_PI * 0.5f);
        windowed[win1_len + win_ones + i] = extended[win1_len + win_ones + i] * w;
    }

    float r[TLCS_LPC_ORDER_MAX + 1];
    tlcs_autocorrelation(windowed, ana_len, r, order);

    float a[TLCS_LPC_ORDER_MAX + 1];
    tlcs_levinson(r, order, a, NULL);

    /* Bandwidth expansion for LR/VLR — minimal for TCX (sharper LPC) */
    if (enc->cfg.subfr_size >= 80) {
        float g = 1.0f;
        for (int32_t k = 1; k <= order; k++) {
            g *= 0.996f;
            a[k] *= g;
        }
    }

    float lsf[TLCS_LPC_ORDER_MAX];
    int lsf_ok = tlcs_lpc_to_lsf(a, order, lsf);
    if (lsf_ok != 0) {
        for (int32_t i = 0; i < order; i++)
            lsf[i] = (float)enc->prev_lsf[i] / 5000.0f;
    }
    tlcs_lsf_stabilize(lsf, order);

    /* ── LSF VQ quantization (always VQ for TCX) ───── */
    float lsf_pred[TLCS_LPC_ORDER_MAX];
    for (int32_t i = 0; i < order; i++)
        lsf_pred[i] = (float)enc->prev_lsf[i] / 5000.0f;

    int32_t lsf_bits = enc->cfg.lsf_bits;
    int32_t vq_size = 1 << lsf_bits;
    int32_t vq_idx[LSF_VQ_NUM_SPLITS];
    float delta_raw[TLCS_LPC_ORDER_MAX];
    for (int32_t i = 0; i < order; i++)
        delta_raw[i] = lsf[i] - lsf_pred[i];
    float delta_q[TLCS_LPC_ORDER_MAX];
    tlcs_lsf_vq_encode_n(delta_raw, order, vq_idx, delta_q, vq_size);

    float lsf_q[TLCS_LPC_ORDER_MAX];
    for (int32_t i = 0; i < order; i++)
        lsf_q[i] = lsf_pred[i] + delta_q[i];
    tlcs_lsf_stabilize(lsf_q, order);

    float a_q[TLCS_LPC_ORDER_MAX + 1];
    tlcs_lsf_to_lpc(lsf_q, order, a_q);

    /* Write LSF VQ indices to bitstream */
    int32_t vq_mask = (1 << lsf_bits) - 1;
    for (int32_t s = 0; s < LSF_VQ_NUM_SPLITS; s++)
        tlcs_bs_write(bsw, (uint32_t)(vq_idx[s] & vq_mask), lsf_bits);

    /* ── Forward MDCT ──────────────────────────────── */
    float mdct_spec[TLCS_MAX_FRAME_SIZE];
    tlcs_mdct_forward(enc->mdct_overlap, speech, mdct_spec, n);
    /* Save current speech for next frame's MDCT overlap */
    memcpy(enc->mdct_overlap, speech, (size_t)n * sizeof(float));

    /* ── Compute LPC spectral envelope ─────────────── */
    float lpc_env[TLCS_MAX_FRAME_SIZE];
    tlcs_tcx_lpc_envelope(a_q, order, lpc_env, n);

    /* ── Rate control ──────────────────────────────── */
    int32_t lsf_total_bits = LSF_VQ_NUM_SPLITS * lsf_bits;
    /* Header: mode(1) + LSF VQ + global_gain(7) + step(3) — no band gains */
    int32_t hdr_bits = 1 + lsf_total_bits + TCX_GAIN_BITS + TCX_STEP_BITS;
    int32_t hdr_bytes = (hdr_bits + 7) / 8;

    int32_t frame_bytes_budget = (enc->cfg.bitrate / 50 + 7) / 8;
    if (frame_bytes_budget < 18) frame_bytes_budget = 18;
    int32_t rc_bytes_budget = frame_bytes_budget - hdr_bytes;
    if (rc_bytes_budget < 4) rc_bytes_budget = 4;

    int32_t is_lr = (enc->cfg.bitrate < TLCS_LR_BITRATE_THRESHOLD);
    int32_t is_vlr = (enc->cfg.bitrate < TLCS_VLR_BITRATE_THRESHOLD);
    int32_t rc_mult = is_lr ? (is_vlr ? 7 : 8) : 2;
    int32_t num_rc_bins = rc_bytes_budget * rc_mult;
    if (num_rc_bins > n) num_rc_bins = n;

    float best_dist = 1e30f;
    int32_t best_step = 0;
    tlcs_tcx_params best_params;
    memset(&best_params, 0, sizeof(best_params));

    for (int32_t si = 0; si < TCX_STEP_LEVELS; si++) {
        tlcs_tcx_params trial;
        memset(&trial, 0, sizeof(trial));
        float dist = tlcs_tcx_encode(mdct_spec, lpc_env, n, si, &trial);

        /* Trial range-encode with global CDF to check budget fit */
        uint16_t trial_cdf[TCX_SPEC_NSYM + 1];
        tlcs_tcx_compute_cdf(si, trial_cdf);

        uint8_t trial_buf[TLCS_MAX_FRAME_BYTES];
        tlcs_rc_encoder trial_rc;
        tlcs_rc_enc_init(&trial_rc, trial_buf, (int32_t)sizeof(trial_buf));

        for (int32_t i = 0; i < num_rc_bins; i++) {
            int32_t sym = trial.quant[i] + TCX_QUANT_MAX;
            if (sym < 0) sym = 0;
            if (sym >= TCX_SPEC_NSYM) sym = TCX_SPEC_NSYM - 1;
            tlcs_rc_enc_symbol(&trial_rc, sym, trial_cdf, TCX_SPEC_NSYM);
        }
        int32_t rc_bytes_used = tlcs_rc_enc_flush(&trial_rc);

        if (rc_bytes_used > rc_bytes_budget) continue;

        if (dist < best_dist) {
            best_dist = dist;
            best_step = si;
            memcpy(&best_params, &trial, sizeof(trial));
        }
    }

    /* If no step fit, use coarsest */
    if (best_dist >= 1e30f) {
        best_step = TCX_STEP_LEVELS - 1;
        tlcs_tcx_encode(mdct_spec, lpc_env, n, best_step, &best_params);
    }
    best_params.num_coded_bins = num_rc_bins;

    /* ── Write header: gain + step (no band gains) ── */
    tlcs_bs_write(bsw, (uint32_t)best_params.global_gain_idx, TCX_GAIN_BITS);
    tlcs_bs_write(bsw, (uint32_t)best_params.step_idx, TCX_STEP_BITS);

    int32_t header_bytes_written = tlcs_bs_writer_flush(bsw);

    /* ── Range-encode spectral bins ────────────────── */
    uint16_t cdf[TCX_SPEC_NSYM + 1];
    tlcs_tcx_compute_cdf(best_step, cdf);

    tlcs_rc_encoder rc;
    tlcs_rc_enc_init(&rc, bitstream_out + header_bytes_written,
                     TLCS_MAX_FRAME_BYTES - header_bytes_written);

    for (int32_t i = 0; i < num_rc_bins; i++) {
        int32_t sym = best_params.quant[i] + TCX_QUANT_MAX;
        if (sym < 0) sym = 0;
        if (sym >= TCX_SPEC_NSYM) sym = TCX_SPEC_NSYM - 1;
        tlcs_rc_enc_symbol(&rc, sym, cdf, TCX_SPEC_NSYM);
    }
    int32_t rc_bytes = tlcs_rc_enc_flush(&rc);
    *bytes_written = header_bytes_written + rc_bytes;

    float recon_spec[TLCS_MAX_FRAME_SIZE];
    tlcs_tcx_decode(&best_params, lpc_env, recon_spec, &enc->recon_noise_seed);

    float recon_time[TLCS_MAX_FRAME_SIZE];
    tlcs_mdct_inverse(recon_spec, recon_time, enc->imdct_recon_overlap, n);

    /* Update synth_mem with reconstruction tail */
    for (int32_t i = 0; i < order; i++)
        enc->synth_mem[i] = recon_time[n - order + i];

    /* ── Update prev LSFs ──────────────────────────── */
    float lsf_q_final[TLCS_LPC_ORDER_MAX];
    {
        int32_t vq_idx_final[LSF_VQ_NUM_SPLITS];
        for (int32_t i = 0; i < LSF_VQ_NUM_SPLITS; i++)
            vq_idx_final[i] = vq_idx[i];
        float delta_q_final[TLCS_LPC_ORDER_MAX];
        tlcs_lsf_vq_decode_n(vq_idx_final, order, delta_q_final, vq_size);
        for (int32_t i = 0; i < order; i++)
            lsf_q_final[i] = lsf_pred[i] + delta_q_final[i];
    }
    tlcs_lsf_stabilize(lsf_q_final, order);
    for (int32_t i = 0; i < order; i++) {
        enc->prev_prev_lsf[i] = enc->prev_lsf[i];
        enc->prev_lsf[i] = (int16_t)(lsf_q_final[i] * 5000.0f);
    }

    enc->prev_codec_mode = TLCS_CODEC_MODE_T;
    enc->frame_count++;
    return TLCS_OK;
}

/* ══════════════════════════════════════════════════════════════════
 *  HR Hybrid Encoder: mode decision + Mode T (TCX) or Mode S (CELP).
 * ══════════════════════════════════════════════════════════════════ */

static tlcs_status encode_hr_hybrid(tlcs_encoder *enc,
                                     const int16_t *pcm_in,
                                     uint8_t *bitstream_out,
                                     int32_t *bytes_written)
{
    const int32_t n = enc->cfg.frame_size;

    /* Pre-emphasize for mode decision (temporary, not saved to state) */
    float speech_tmp[TLCS_MAX_FRAME_SIZE];
    float pe_mem = (float)enc->preemph_mem;
    for (int32_t i = 0; i < n; i++) {
        speech_tmp[i] = (float)pcm_in[i] - TLCS_PREEMPH_COEFF * pe_mem;
        pe_mem = (float)pcm_in[i];
    }

    int32_t mode = tlcs_mode_decide(speech_tmp, n, enc->cfg.bitrate,
                                     enc->prev_codec_mode,
                                     &enc->mode_hold_count);

    tlcs_bs_writer bsw;
    tlcs_bs_writer_init(&bsw, bitstream_out, TLCS_MAX_FRAME_BYTES);
    tlcs_bs_write(&bsw, (uint32_t)mode, 1);

    if (mode == TLCS_CODEC_MODE_T) {
        return encode_tcx(enc, pcm_in, &bsw, bitstream_out, bytes_written);
    }

    /* Mode S (CELP): currently dead code — mode decision always returns T.
     * Fall back to encode_direct which creates its own bitstream. */
    return encode_direct(enc, pcm_in, bitstream_out, bytes_written);
}

/* ══════════════════════════════════════════════════════════════════
 *  LR Hybrid Encoder: mode decision + Mode T (TCX) or Mode S (CELP).
 *  Per SMPL-NEXT Tier B: adaptive S+T at 5-12 kbps.
 * ══════════════════════════════════════════════════════════════════ */

static tlcs_status encode_lr_hybrid(tlcs_encoder *enc,
                                     const int16_t *pcm_in,
                                     uint8_t *bitstream_out,
                                     int32_t *bytes_written)
{
    const int32_t n = enc->cfg.frame_size;

    /* Pre-emphasize for mode decision (temporary) */
    float speech_tmp[TLCS_MAX_FRAME_SIZE];
    float pe_mem = (float)enc->preemph_mem;
    for (int32_t i = 0; i < n; i++) {
        speech_tmp[i] = (float)pcm_in[i] - TLCS_PREEMPH_COEFF * pe_mem;
        pe_mem = (float)pcm_in[i];
    }

    int32_t mode = tlcs_mode_decide(speech_tmp, n, enc->cfg.bitrate,
                                     enc->prev_codec_mode,
                                     &enc->mode_hold_count);

    tlcs_bs_writer bsw;
    tlcs_bs_writer_init(&bsw, bitstream_out, TLCS_MAX_FRAME_BYTES);
    tlcs_bs_write(&bsw, (uint32_t)mode, 1);

    if (mode == TLCS_CODEC_MODE_T) {
        return encode_tcx(enc, pcm_in, &bsw, bitstream_out, bytes_written);
    }

    /* Mode S (CELP): encode using CELP core, bitstream continues after mode bit */
    {
        const int32_t n       = enc->cfg.frame_size;
        const int32_t order   = enc->cfg.lpc_order;
        const int32_t subfr   = enc->cfg.subfr_size;
        const int32_t n_subfr = enc->cfg.n_subfr;

        /* Pre-emphasis */
        float speech[TLCS_MAX_FRAME_SIZE];
        float preemph_mem_f = (float)enc->preemph_mem;
        for (int32_t i = 0; i < n; i++) {
            float s = (float)pcm_in[i] - TLCS_PREEMPH_COEFF * preemph_mem_f;
            preemph_mem_f = (float)pcm_in[i];
            speech[i] = s;
        }
        enc->preemph_mem = (int16_t)preemph_mem_f;

        /* CELP core */
        int16_t lsf_indices[TLCS_LPC_ORDER_MAX];
        int32_t pitch_lags[TLCS_MAX_SUBFRAMES];
        int32_t pitch_fracs[TLCS_MAX_SUBFRAMES];
        int32_t pitch_gain_indices[TLCS_MAX_SUBFRAMES];
        tlcs_cb_entry cb_entries[TLCS_MAX_SUBFRAMES];
        float a_q[TLCS_LPC_ORDER_MAX + 1];

        celp_encode_core(enc, speech, n, order, subfr, n_subfr,
                         TLCS_MIN_PITCH_LAG, TLCS_MAX_PITCH_LAG,
                         enc->cfg.num_pulses, TUNE_CELP_GAMMA1, TUNE_CELP_GAMMA2,
                         lsf_indices, pitch_lags, pitch_fracs,
                         pitch_gain_indices, cb_entries, a_q);

        /* Pack into bitstream (bsw already has mode bit written) */
        tlcs_cb_config cb_cfg;
        tlcs_cb_config_init(&cb_cfg, subfr, enc->cfg.num_pulses);

        int32_t lsf_bits   = enc->cfg.lsf_bits;
        int32_t delta_bits  = enc->cfg.pitch_delta_bits;
        int32_t delta_off   = enc->cfg.pitch_delta_offset;
        int32_t fcb_bits    = enc->cfg.fcb_gain_bits;

        /* LSF indices (VQ) */
        if (enc->cfg.use_lsf_vq) {
            int32_t vq_mask = (1 << lsf_bits) - 1;
            for (int32_t s = 0; s < LSF_VQ_NUM_SPLITS; s++)
                tlcs_bs_write(&bsw, (uint32_t)(lsf_indices[s] & vq_mask), lsf_bits);
        } else {
            for (int32_t i = 0; i < order; i++)
                tlcs_bs_write(&bsw, (uint32_t)(lsf_indices[i] & ((1 << lsf_bits) - 1)), lsf_bits);
        }

        /* Per-subframe data */
        int32_t prev_lag_idx = 0;
        for (int32_t sf = 0; sf < n_subfr; sf++) {
            int32_t lag_idx = tlcs_pitch_encode_lag(pitch_lags[sf], pitch_fracs[sf]);
            if (sf == 0) {
                tlcs_bs_write(&bsw, (uint32_t)lag_idx, 9);
            } else {
                int32_t delta = lag_idx - prev_lag_idx;
                if (delta < -delta_off) delta = -delta_off;
                if (delta > delta_off - 1) delta = delta_off - 1;
                tlcs_bs_write(&bsw, (uint32_t)(delta + delta_off), delta_bits);
            }
            prev_lag_idx = lag_idx;

            tlcs_bs_write(&bsw, (uint32_t)pitch_gain_indices[sf], TLCS_PITCH_GAIN_BITS);
            for (int32_t p = 0; p < cb_cfg.num_pulses; p++) {
                if (cb_cfg.pos_bits > 0)
                    tlcs_bs_write(&bsw, (uint32_t)cb_entries[sf].pulse_pos[p], (int32_t)cb_cfg.pos_bits);
                tlcs_bs_write(&bsw, (cb_entries[sf].pulse_sign[p] > 0) ? 1u : 0u, 1);
            }
            tlcs_bs_write(&bsw, (uint32_t)cb_entries[sf].gain_index, fcb_bits);
        }

        *bytes_written = tlcs_bs_writer_flush(&bsw);
        enc->frame_count++;
        return TLCS_OK;
    }
}

/* ══════════════════════════════════════════════════════════════════
 *  Public API
 * ══════════════════════════════════════════════════════════════════ */

tlcs_status tlcs_encoder_init(tlcs_encoder *enc, const tlcs_config *cfg)
{
    if (!enc || !cfg) return TLCS_ERR_INVALID_ARG;
    memset(enc, 0, sizeof(*enc));
    enc->cfg = *cfg;

    tlcs_preemph_init();  /* read TLCS_PREEMPH env var */

    float lsf_tmp[TLCS_LPC_ORDER_MAX];
    init_default_lsf(lsf_tmp, cfg->lpc_order);
    for (int32_t i = 0; i < cfg->lpc_order; i++) {
        enc->prev_lsf[i] = (int16_t)(lsf_tmp[i] * 5000.0f);
        enc->prev_prev_lsf[i] = enc->prev_lsf[i];
    }

    return TLCS_OK;
}

tlcs_status tlcs_encode(tlcs_encoder *enc,
                        const int16_t *pcm_in,
                        uint8_t *bitstream_out,
                        int32_t *bytes_written)
{
    if (!enc || !pcm_in || !bitstream_out || !bytes_written)
        return TLCS_ERR_INVALID_ARG;

    if (enc->cfg.bitrate < TLCS_LR_BITRATE_THRESHOLD) {
        if (enc->cfg.use_ec)
            return encode_lowrate_ec(enc, pcm_in, bitstream_out, bytes_written);
        return encode_lr_hybrid(enc, pcm_in, bitstream_out, bytes_written);
    }

    if (enc->cfg.use_ec)
        return encode_direct_ec(enc, pcm_in, bitstream_out, bytes_written);
    return encode_hr_hybrid(enc, pcm_in, bitstream_out, bytes_written);
}
