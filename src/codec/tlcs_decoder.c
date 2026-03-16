#include "tlcs/tlcs.h"
#include "tlcs_lpc.h"
#include "tlcs_pitch.h"
#include "tlcs_codebook.h"
#include "tlcs_qmf.h"
#include "tlcs_lsf_vq.h"
#include "tlcs_mdct.h"
#include "tlcs_tcx.h"
#include "tlcs_tune.h"
#include "tlcs_mode.h"
#include "../bitstream/tlcs_bitstream.h"
#include "../entropy/tlcs_range_coder.h"
#include "../entropy/tlcs_ec_models.h"
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── Decoder-side pitch detection (autocorrelation) ───────────── */
static int32_t detect_pitch_autocorr(const float *signal, int32_t n,
                                      int32_t min_lag, int32_t max_lag,
                                      float *voicing_out)
{
    float best_corr = 0.0f;
    int32_t best_lag = 0;

    /* Signal energy */
    float energy = 0.0f;
    for (int32_t i = 0; i < n; i++)
        energy += signal[i] * signal[i];
    if (energy < 1.0f) {
        *voicing_out = 0.0f;
        return 0;
    }

    for (int32_t lag = min_lag; lag <= max_lag && lag < n; lag++) {
        float corr = 0.0f, lag_e = 0.0f;
        for (int32_t i = lag; i < n; i++) {
            corr  += signal[i] * signal[i - lag];
            lag_e += signal[i - lag] * signal[i - lag];
        }
        if (lag_e < 1.0f) continue;
        float norm_corr = corr / sqrtf(energy * lag_e);
        if (norm_corr > best_corr) {
            best_corr = norm_corr;
            best_lag = lag;
        }
    }

    *voicing_out = best_corr;
    return best_lag;
}

/* ── PRNG for noise fill ──────────────────────────────────────── */

static float prng_float(uint32_t *state)
{
    *state = *state * 1664525u + 1013904223u;
    return ((float)(int32_t)*state) / 2147483648.0f;  /* uniform in [-1, 1] */
}

/* ── Decoder-only ACB high-frequency boost ─────────────────────
 *
 * Sharpens pitch pulses by boosting the HF component of 2-tap
 * ACB gains.  Inspired by SMPL's adjust_acbgains().
 * Transforms {g0, g1} to frequency domain, boosts HF, inverts.
 * Only applied for voiced frames in LR/VLR modes.
 */
static void acb_high_boost(float *g0, float *g1, float boost)
{
    float f0 = *g0 + 2.0f * (*g1);   /* LF (all taps sum) */
    float f1 = *g0 - *g1;            /* HF (center − side) */
    float abs_f1 = fabsf(f1);
    float abs_f0 = fabsf(f0);
    float abs_f1_new = abs_f1 + boost;
    if (abs_f1_new > abs_f0) abs_f1_new = abs_f0;
    if (abs_f1 > 1e-12f)
        f1 *= abs_f1_new / abs_f1;
    else
        f1 = (f1 >= 0.0f) ? abs_f1_new : -abs_f1_new;
    *g0 = (f0 + 2.0f * f1) / 3.0f;
    *g1 = (f0 - f1) / 3.0f;
}

/* ── Innovation noise fill ─────────────────────────────────────
 *
 * Fills zero positions in the ACELP innovation (between sparse
 * pulses) with envelope-shaped, HP-filtered noise.  This smooths
 * the harsh spectral gaps caused by algebraic codebook sparsity.
 * Decoder-only: no bitrate cost, but causes minor encoder-decoder
 * excitation drift (acceptable, same as SMPL's approach).
 *
 * Key ideas from SMPL:
 *   - Only fill truly zero positions (preserve pulse signal)
 *   - Shape noise by smoothed excitation envelope
 *   - HP filter to remove LF energy that would boom
 */
static void innovation_noise_fill(float *innovation, int32_t subfr_size,
                                   float acb_gain, uint32_t *seed)
{
    /* Compute RMS of innovation (pulse energy) */
    float sum2 = 0.0f;
    int32_t n_nonzero = 0;
    for (int32_t i = 0; i < subfr_size; i++) {
        if (innovation[i] != 0.0f) {
            sum2 += innovation[i] * innovation[i];
            n_nonzero++;
        }
    }
    if (n_nonzero == 0) return;
    float rms = sqrtf(sum2 / (float)n_nonzero);

    /* Noise gain: stronger for unvoiced, weaker for voiced */
    float noise_gain;
    if (acb_gain > 0.6f)
        noise_gain = 0.06f;   /* strongly voiced: very mild */
    else if (acb_gain > 0.3f)
        noise_gain = 0.15f;   /* transition */
    else
        noise_gain = 0.30f;   /* unvoiced: strong fill */

    float level = noise_gain * rms;

    /* Fill zero positions with HP-filtered noise */
    float prev_noise = 0.0f;
    for (int32_t i = 0; i < subfr_size; i++) {
        float r = prng_float(seed);
        if (innovation[i] == 0.0f) {
            /* Simple 1st-order HP: y = x - 0.5*prev */
            float hp = r - 0.5f * prev_noise;
            prev_noise = r;
            innovation[i] = level * hp;
        } else {
            prev_noise = r;  /* advance PRNG, keep state for HP */
        }
    }
}

/* ── Pre-synthesis excitation tilt filter ───────────────────────
 *
 * Mild low-pass [0.84, 0.16] applied to excitation before LPC
 * synthesis.  Smooths sparse pulse energy and reduces HF harshness.
 * Only used for LR mode (sparse excitation).
 */
static void excitation_tilt(float *excitation, int32_t n, float *mem)
{
    float prev = *mem;
    for (int32_t i = 0; i < n; i++) {
        float e = 0.84f * excitation[i] + 0.16f * prev;
        prev = excitation[i];
        excitation[i] = e;
    }
    *mem = prev;
}

/* ── Harmonic post-filter ────────────────────────────────────────
 *
 * Enhances pitch periodicity by blending synthesized speech with
 * pitch-delayed copies.  Applied per-subframe after LPC synthesis.
 * Uses a simple comb filter: y[n] = (1-s)*x[n] + s*x[n-lag]
 * where s is proportional to voicing strength with high-lag rolloff.
 * A 3-tap averaging around the lag smooths the periodic component.
 */
static void harmonic_postfilter(float *synth, int32_t n,
                                 int32_t subfr_size, int32_t n_subfr,
                                 const int32_t *lags, const float *gains,
                                 float *history, float max_strength)
{

    /* Shift history, append current frame */
    for (int32_t i = 0; i < TLCS_MAX_PITCH_LAG; i++)
        history[i] = history[i + n];
    for (int32_t i = 0; i < n; i++)
        history[TLCS_MAX_PITCH_LAG + i] = synth[i];

    for (int32_t sf = 0; sf < n_subfr; sf++) {
        int32_t offset = sf * subfr_size;
        int32_t lag = lags[sf];
        float g0 = gains[sf];

        if (lag < 20 || g0 < 0.25f) continue;

        /* Strength: proportional to voicing, with high-lag rolloff */
        float strength = 0.40f * g0;
        if (strength > max_strength) strength = max_strength;
        /* Reduce at high lags (less reliable periodicity) */
        if (lag > 40)
            strength *= (1.0f - 0.06f * (float)(lag - 40) / 260.0f);

        /* Compute original energy */
        float orig_e = 0.0f;
        for (int32_t i = 0; i < subfr_size; i++)
            orig_e += synth[offset + i] * synth[offset + i];

        /* Comb filter with 3-tap smoothing around pitch lag */
        for (int32_t i = 0; i < subfr_size; i++) {
            int32_t h_idx = TLCS_MAX_PITCH_LAG + offset + i;
            /* 3-tap average: 0.25 * x[n-lag-1] + 0.50 * x[n-lag] + 0.25 * x[n-lag+1] */
            float periodic = 0.50f * history[h_idx - lag];
            if (h_idx - lag - 1 >= 0)
                periodic += 0.25f * history[h_idx - lag - 1];
            if (h_idx - lag + 1 < TLCS_MAX_PITCH_LAG + n)
                periodic += 0.25f * history[h_idx - lag + 1];
            synth[offset + i] = (1.0f - strength) * synth[offset + i]
                               + strength * periodic;
        }

        /* Gain normalization: preserve energy */
        float new_e = 0.0f;
        for (int32_t i = 0; i < subfr_size; i++)
            new_e += synth[offset + i] * synth[offset + i];
        if (new_e > 1.0f) {
            float gain = sqrtf(orig_e / new_e);
            if (gain > 2.0f) gain = 2.0f;
            for (int32_t i = 0; i < subfr_size; i++)
                synth[offset + i] *= gain;
        }
    }
}

/* ── Bass post-filter (decoder-only) ──────────────────────────────
 *
 * Enhances pitch periodicity in the low-frequency band (< 400 Hz)
 * without distorting the spectral envelope.  This is the AMR-WB
 * style bass post-filter, NOT a formant post-filter.
 *
 * Algorithm:
 *   noise[n]   = LP( s[n] - s[n - T] )          (inter-harmonic noise)
 *   s_out[n]   = s[n] - alpha * 0.5 * noise[n]  (remove fraction)
 *   alpha      = clamp(g0 * 0.5, 0, 0.5)        (voicing-dependent)
 *
 * The LP filter (1st-order IIR, ~400 Hz cutoff at 16 kHz) ensures
 * only low frequencies are affected, preserving sibilants and HF.
 */
static void bass_postfilter(float *synth, int32_t n,
                            int32_t subfr_size,
                            const int32_t *lags, const float *gains,
                            int32_t n_subfr,
                            float *history,   /* [MAX_PITCH_LAG + MAX_FRAME_SIZE] */
                            float *lp_mem)
{
    /* 1st-order LP: y[n] = (1-a)*x[n] + a*y[n-1]
     * a = exp(-2*pi*400/16000) ≈ 0.854  →  -3 dB at ~400 Hz */
    /* 1st-order LP: y[n] = (1-a)*x[n] + a*y[n-1]
     * a = exp(-2*pi*400/16000) ≈ 0.854  →  -3 dB at ~400 Hz */
    const float LP_A = 0.854f;
    const float LP_B = 1.0f - LP_A;   /* 0.146 */
    const float MAX_ALPHA = 0.15f;

    /* Shift history left, append current frame */
    for (int32_t i = 0; i < TLCS_MAX_PITCH_LAG; i++)
        history[i] = history[i + n];
    for (int32_t i = 0; i < n; i++)
        history[TLCS_MAX_PITCH_LAG + i] = synth[i];

    float mem = *lp_mem;

    for (int32_t sf = 0; sf < n_subfr; sf++) {
        int32_t offset = sf * subfr_size;
        int32_t lag = lags[sf];
        float g0 = gains[sf];

        /* Voicing-dependent blend */
        float alpha = 0.0f;
        if (g0 > 0.3f && lag >= 20) {
            alpha = g0 * 0.15f;
            if (alpha > MAX_ALPHA) alpha = MAX_ALPHA;
        }

        for (int32_t i = 0; i < subfr_size; i++) {
            int32_t hist_idx = TLCS_MAX_PITCH_LAG + offset + i;
            float s_cur = history[hist_idx];
            float s_del = history[hist_idx - lag];

            /* LP-filter the inter-harmonic noise estimate */
            float diff = s_cur - s_del;
            float diff_lp = LP_B * diff + LP_A * mem;
            mem = diff_lp;

            /* Subtract fraction of LP-filtered noise */
            synth[offset + i] -= alpha * 0.5f * diff_lp;
        }
    }

    *lp_mem = mem;
}

/* ── Formant post-filter ───────────────────────────────────────────
 *
 * Sharpens formant peaks via H(z) = A(z/γn) / A(z/γd).
 * Applied per-subframe with interpolated LPC, gain-normalized to
 * preserve energy.  Adaptive tilt compensation boosts HF when the
 * postfiltered signal is bass-heavy (k1 > 0).
 */
static void formant_postfilter(float *synth, int32_t n,
                                int32_t subfr_size, int32_t n_subfr,
                                int32_t order,
                                const float *lsf_q,
                                const float *prev_lsf_f,
                                const float *lsf_alpha_tbl,
                                float gamma_n, float gamma_d,
                                float tilt_mu,
                                float *fir_mem,
                                float *iir_mem,
                                float *tilt_mem)
{
    /* Keep original synth for FIR input (avoid in-place aliasing) */
    float synth_orig[TLCS_MAX_FRAME_SIZE];
    memcpy(synth_orig, synth, (size_t)n * sizeof(float));

    for (int32_t sf = 0; sf < n_subfr; sf++) {
        int32_t offset = sf * subfr_size;
        float alpha = lsf_alpha_tbl[sf];

        /* Interpolated LPC for this subframe */
        float lsf_sf[TLCS_LPC_ORDER_MAX];
        for (int32_t i = 0; i < order; i++)
            lsf_sf[i] = (1.0f - alpha) * prev_lsf_f[i] + alpha * lsf_q[i];
        tlcs_lsf_stabilize(lsf_sf, order);
        float a_sf[TLCS_LPC_ORDER_MAX + 1];
        tlcs_lsf_to_lpc(lsf_sf, order, a_sf);

        /* Weighted LPC */
        float a_gn[TLCS_LPC_ORDER_MAX + 1];
        float a_gd[TLCS_LPC_ORDER_MAX + 1];
        tlcs_lpc_weight_coeffs(a_sf, order, gamma_n, a_gn);
        tlcs_lpc_weight_coeffs(a_sf, order, gamma_d, a_gd);

        /* Original subframe energy */
        float orig_e = 0.0f;
        for (int32_t i = 0; i < subfr_size; i++)
            orig_e += synth_orig[offset + i] * synth_orig[offset + i];

        /* FIR: A(z/γn) — analysis filter on original synth */
        float fir_out[TLCS_MAX_SUBFR_SIZE];
        tlcs_analysis_filter(a_gn, order, &synth_orig[offset],
                             fir_out, subfr_size, fir_mem);

        /* IIR: 1/A(z/γd) — synthesis filter */
        float pf_out[TLCS_MAX_SUBFR_SIZE];
        tlcs_synthesis_filter(a_gd, order, fir_out,
                              pf_out, subfr_size, iir_mem);

        /* Adaptive tilt compensation (G.729 / AMR-WB style):
         * Compute first normalized autocorrelation k1 of pf_out.
         * k1 > 0 → bass-heavy → apply high-pass: y = x - mu*k1*x[n-1]
         * k1 < 0 → treble-heavy → apply low-pass: y = x + mu*|k1|*x[n-1]
         * This counteracts the spectral tilt introduced by the formant filter. */
        float r0 = 0.0f, r1 = 0.0f;
        for (int32_t i = 0; i < subfr_size; i++)
            r0 += pf_out[i] * pf_out[i];
        for (int32_t i = 1; i < subfr_size; i++)
            r1 += pf_out[i] * pf_out[i - 1];
        float k1 = (r0 > 1.0f) ? r1 / r0 : 0.0f;
        float tilt_coeff = tilt_mu * k1;

        float prev_s = *tilt_mem;
        for (int32_t i = 0; i < subfr_size; i++) {
            float s = pf_out[i] - tilt_coeff * prev_s;
            prev_s = pf_out[i];
            pf_out[i] = s;
        }
        *tilt_mem = prev_s;

        /* Gain normalization */
        float pf_e = 0.0f;
        for (int32_t i = 0; i < subfr_size; i++)
            pf_e += pf_out[i] * pf_out[i];

        float gain = 1.0f;
        if (pf_e > 1.0f)
            gain = sqrtf(orig_e / pf_e);
        if (gain > 2.0f) gain = 2.0f;

        for (int32_t i = 0; i < subfr_size; i++)
            synth[offset + i] = pf_out[i] * gain;
    }
}

static void init_default_lsf(float *lsf, int32_t order)
{
    for (int32_t i = 0; i < order; i++) {
        lsf[i] = (float)M_PI * (float)(i + 1) / (float)(order + 1);
    }
}

/* ══════════════════════════════════════════════════════════════════
 *  WB Band-Split Decoder
 * ══════════════════════════════════════════════════════════════════ */

static tlcs_status decode_wb_bandsplit(tlcs_decoder *dec,
                                        const uint8_t *bitstream_in,
                                        int32_t bytes_in,
                                        int16_t *pcm_out)
{
    const int32_t n_lb   = TLCS_LB_FRAME;   /* 80 */
    const int32_t order  = TLCS_LB_ORDER;    /* 10 */
    const int32_t subfr  = TLCS_LB_SUBFR;    /* 20 */
    const int32_t n_subfr = TLCS_LB_NSUBFR;  /* 4 */

    /* Configure codebook (must match encoder) */
    tlcs_cb_config cb_cfg;
    tlcs_cb_config_init(&cb_cfg, subfr, TLCS_LB_NUM_PULSES);

    /* Compute expected bitstream size (delta pitch: 2 absolute + 2 delta) */
    int32_t subfr_other_bits_wb = TLCS_PITCH_GAIN_BITS
                                + cb_cfg.num_pulses * (cb_cfg.pos_bits + 1)
                                + TLCS_FCB_GAIN_BITS;
    int32_t pitch_bits_wb = 2 * 9 + (n_subfr - 2) * TLCS_PITCH_DELTA_BITS;
    int32_t total_bits = order * TLCS_LSF_BITS + pitch_bits_wb + n_subfr * subfr_other_bits_wb
                       + TLCS_HB_ORDER * TLCS_HB_LSF_BITS
                       + TLCS_HB_ENERGY_BITS;
    int32_t expected_bytes = (total_bits + 7) / 8;
    if (bytes_in < expected_bytes) return TLCS_ERR_BAD_BITSTREAM;

    /* ── Step 1: Unpack LB parameters ──────────────────── */
    tlcs_bs_reader bsr;
    tlcs_bs_reader_init(&bsr, bitstream_in, bytes_in);

    /* LB LSF indices */
    int16_t lsf_indices[TLCS_LPC_ORDER_MAX];
    for (int32_t i = 0; i < order; i++) {
        uint32_t val;
        tlcs_bs_read(&bsr, &val, TLCS_LSF_BITS);
        lsf_indices[i] = (int16_t)val;
    }

    /* Predictive LSF dequantization */
    float lsf_pred[TLCS_LPC_ORDER_MAX];
    for (int32_t i = 0; i < order; i++)
        lsf_pred[i] = (float)dec->prev_lsf[i] / 5000.0f;

    float lsf[TLCS_LPC_ORDER_MAX];
    tlcs_lsf_dequantize_pred(lsf_indices, lsf_pred, order, lsf);
    tlcs_lsf_stabilize(lsf, order);

    float a[TLCS_LPC_ORDER_MAX + 1];
    tlcs_lsf_to_lpc(lsf, order, a);

    /* ── Step 2: Load excitation buffer ────────────────── */
    float *exc = dec->exc_buf;
    tlcs_exc_buf_shift(exc, n_lb);

    /* ── Step 3: LB per-subframe reconstruction ────────── */
    float lb_excitation[TLCS_MAX_FRAME_SIZE];
    float prev_cb_gain_mag_wb = 0.0f;
    int32_t prev_lag_idx_wb = 0;
    for (int32_t sf = 0; sf < n_subfr; sf++) {
        int32_t sf_offset = sf * subfr;
        int32_t exc_offset = TLCS_MAX_PITCH_LAG + sf_offset;

        /* Pitch lag: absolute or delta */
        int32_t lag_idx_wb;
        if (sf == 0 || sf == n_subfr / 2) {
            uint32_t lag_val;
            tlcs_bs_read(&bsr, &lag_val, 9);
            lag_idx_wb = (int32_t)lag_val;
        } else {
            uint32_t delta_val;
            tlcs_bs_read(&bsr, &delta_val, TLCS_PITCH_DELTA_BITS);
            lag_idx_wb = prev_lag_idx_wb + (int32_t)delta_val - TLCS_PITCH_DELTA_OFFSET;
        }
        prev_lag_idx_wb = lag_idx_wb;
        int32_t lag, lag_frac;
        tlcs_pitch_decode_lag(lag_idx_wb, &lag, &lag_frac);

        /* Pitch gain (ACB VQ index) */
        uint32_t gi_val;
        tlcs_bs_read(&bsr, &gi_val, TLCS_PITCH_GAIN_BITS);
        int32_t vq_idx = (int32_t)gi_val;
        if (vq_idx >= TLCS_ACB_VQ_SIZE) vq_idx = TLCS_ACB_VQ_SIZE - 1;
        float g0 = tlcs_acb_vq[vq_idx][0];
        float g1 = tlcs_acb_vq[vq_idx][1];

        /* Codebook pulses */
        tlcs_cb_entry cb_entry;
        for (int32_t p = 0; p < cb_cfg.num_pulses; p++) {
            uint32_t pos_val = 0, sign_val;
            if (cb_cfg.pos_bits > 0)
                tlcs_bs_read(&bsr, &pos_val, (int32_t)cb_cfg.pos_bits);
            tlcs_bs_read(&bsr, &sign_val, 1);
            cb_entry.pulse_pos[p] = (int32_t)pos_val;
            cb_entry.pulse_sign[p] = sign_val ? 1 : -1;
        }

        /* Fixed CB gain */
        uint32_t fcb_gi;
        tlcs_bs_read(&bsr, &fcb_gi, TLCS_FCB_GAIN_BITS);
        cb_entry.gain_index = (int32_t)fcb_gi;
        cb_entry.gain = tlcs_fcb_gain_dequantize_pred(cb_entry.gain_index, prev_cb_gain_mag_wb);
        prev_cb_gain_mag_wb = fabsf(cb_entry.gain);

        /* Clamp pitch lag to LB range */
        if (lag < TLCS_LB_MIN_LAG) lag = TLCS_LB_MIN_LAG;
        if (lag > TLCS_LB_MAX_LAG) lag = TLCS_LB_MAX_LAG;

        /* 2-tap ACB basis vectors */
        float v0[TLCS_MAX_SUBFR_SIZE], v1[TLCS_MAX_SUBFR_SIZE];
        tlcs_pitch_get_acb_basis(exc, exc_offset, lag, lag_frac,
                                  v0, v1, subfr);

        /* Build innovation */
        float innovation[TLCS_MAX_SUBFR_SIZE];
        tlcs_cb_build_innovation(&cb_entry, &cb_cfg, innovation, subfr);

        /* Pitch sharpening */
        tlcs_cb_pitch_sharpen(innovation, subfr, lag, g0);

        /* Phase dispersion */
        tlcs_cb_phase_disperse(innovation, subfr, g0);

        /* Reconstruct excitation */
        for (int32_t i = 0; i < subfr; i++) {
            float e = g0 * v0[i] + g1 * v1[i] + innovation[i];
            lb_excitation[sf_offset + i] = e;
            exc[exc_offset + i] = e;
        }

        /* Pitch periodicity enhancement */
        if (lag >= 20 && g0 > 0.3f) {
            float blend = g0 * 0.08f;
            if (blend > 0.08f) blend = 0.08f;
            for (int32_t i = 0; i < subfr; i++) {
                float past_exc = exc[exc_offset + i - lag];
                float enhanced = (1.0f - blend) * exc[exc_offset + i]
                               + blend * past_exc;
                lb_excitation[sf_offset + i] = enhanced;
                exc[exc_offset + i] = enhanced;
            }
        }
    }

    /* ── Step 4: Unpack HB parameters ──────────────────── */
    int32_t hb_lsf_idx[TLCS_HB_ORDER];
    for (int32_t i = 0; i < TLCS_HB_ORDER; i++) {
        uint32_t val;
        tlcs_bs_read(&bsr, &val, TLCS_HB_LSF_BITS);
        hb_lsf_idx[i] = (int32_t)val;
    }

    uint32_t energy_idx;
    tlcs_bs_read(&bsr, &energy_idx, TLCS_HB_ENERGY_BITS);

    /* Dequantize HB LSFs */
    float hb_lsf[TLCS_HB_ORDER];
    for (int32_t i = 0; i < TLCS_HB_ORDER; i++)
        hb_lsf[i] = (float)hb_lsf_idx[i] * (float)M_PI / 15.0f;
    tlcs_lsf_stabilize(hb_lsf, TLCS_HB_ORDER);

    float hb_a[TLCS_HB_ORDER + 1];
    tlcs_lsf_to_lpc(hb_lsf, TLCS_HB_ORDER, hb_a);

    /* Dequantize energy ratio: map [0,31] to [-8, 2] */
    float log_ratio = (float)energy_idx * 10.0f / 31.0f - 8.0f;
    float energy_ratio = expf(log_ratio);

    /* ── Step 5: LB synthesis (per-subframe LSF interpolation) ── */
    /* Clamp LB excitation */
    int32_t exc_total = TLCS_MAX_PITCH_LAG + n_lb;
    for (int32_t i = 0; i < exc_total; i++) {
        if (exc[i] > 32767.0f) exc[i] = 32767.0f;
        if (exc[i] < -32768.0f) exc[i] = -32768.0f;
    }

    /* Per-subframe LSF interpolation (must match encoder) */
    static const float lsf_alpha[8] = {0.55f, 0.65f, 0.75f, 0.85f, 0.92f, 0.96f, 1.0f, 1.0f};
    float prev_lsf_f[TLCS_LPC_ORDER_MAX];
    for (int32_t i = 0; i < order; i++)
        prev_lsf_f[i] = (float)dec->prev_lsf[i] / 5000.0f;

    float lb_synth[TLCS_MAX_FRAME_SIZE];
    float syn_mem[TLCS_LPC_ORDER_MAX];
    memcpy(syn_mem, dec->synth_mem, (size_t)order * sizeof(float));

    for (int32_t sf = 0; sf < n_subfr; sf++) {
        float alpha = lsf_alpha[sf < 8 ? sf : 7];
        float lsf_sf[TLCS_LPC_ORDER_MAX];
        for (int32_t i = 0; i < order; i++)
            lsf_sf[i] = (1.0f - alpha) * prev_lsf_f[i] + alpha * lsf[i];
        tlcs_lsf_stabilize(lsf_sf, order);
        float a_sf[TLCS_LPC_ORDER_MAX + 1];
        tlcs_lsf_to_lpc(lsf_sf, order, a_sf);
        tlcs_synthesis_filter(a_sf, order, &lb_excitation[sf * subfr],
                              &lb_synth[sf * subfr], subfr, syn_mem);
    }

    memcpy(dec->synth_mem, syn_mem, (size_t)order * sizeof(float));

    /* De-emphasis on LB */
    float deemph_mem_f = (float)dec->deemph_mem;
    tlcs_deemph(lb_synth, n_lb, &deemph_mem_f);
    dec->deemph_mem = (int16_t)deemph_mem_f;

    /* ── Step 6: HB synthesis ──────────────────────────── */
    /* Spectral folding via sign alternation (AMR-WB style):
     * exc_hb[i] = exc_lb[i] * (-1)^i shifts spectrum by π,
     * mapping LB content into HB frequencies. */
    float hb_exc[TLCS_MAX_FRAME_SIZE];
    for (int32_t i = 0; i < n_lb; i++) {
        float sign = (i & 1) ? -1.0f : 1.0f;
        hb_exc[i] = lb_excitation[i] * sign;
    }

    /* Filter through HB synthesis filter */
    float hb_synth[TLCS_MAX_FRAME_SIZE];
    tlcs_synthesis_filter(hb_a, TLCS_HB_ORDER, hb_exc,
                          hb_synth, n_lb, dec->hb_synth_mem);

    /* Scale HB to target energy based on decoded ratio and LB energy */
    float lb_eng = 0.0f, hb_eng = 0.0f;
    for (int32_t i = 0; i < n_lb; i++) {
        lb_eng += lb_synth[i] * lb_synth[i];
        hb_eng += hb_synth[i] * hb_synth[i];
    }

    float target_hb_eng = lb_eng * energy_ratio;
    float hb_scale = 1.0f;
    if (hb_eng > 1.0f)
        hb_scale = sqrtf(target_hb_eng / hb_eng);
    if (hb_scale > 10.0f) hb_scale = 10.0f;

    for (int32_t i = 0; i < n_lb; i++)
        hb_synth[i] *= hb_scale;

    /* ── Step 7: QMF synthesis (combine LB + HB → fullband) ── */
    float fullband[TLCS_MAX_FRAME_SIZE];
    tlcs_qmf_synthesize(dec->qmf_lb_mem, dec->qmf_hb_mem,
                         lb_synth, hb_synth,
                         fullband, n_lb);

    /* ── Step 8: Output ────────────────────────────────── */
    for (int32_t i = 0; i < 160; i++) {
        float v = fullband[i];
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        pcm_out[i] = (int16_t)v;
    }

    /* Update state (save prev→prev_prev before overwriting) */
    for (int32_t i = 0; i < order; i++) {
        dec->prev_prev_lsf[i] = dec->prev_lsf[i];
        dec->prev_lsf[i] = (int16_t)(lsf[i] * 5000.0f);
    }
    dec->frame_count++;

    return TLCS_OK;
}

/* ══════════════════════════════════════════════════════════════════
 *  Entropy-Coded Low-Rate Decoder
 *  Reads parameters via range coder, synthesis identical to decode_direct.
 * ══════════════════════════════════════════════════════════════════ */

static tlcs_status decode_lowrate_ec(tlcs_decoder *dec,
                                      const uint8_t *bitstream_in,
                                      int32_t bytes_in,
                                      int16_t *pcm_out)
{
    const int32_t n       = dec->cfg.frame_size;
    const int32_t order   = dec->cfg.lpc_order;
    const int32_t subfr   = dec->cfg.subfr_size;
    const int32_t n_subfr = dec->cfg.n_subfr;

    int32_t lsf_bits   = dec->cfg.lsf_bits;
    float   lsf_range  = (lsf_bits <= 5) ? 0.40f : (lsf_bits == 6) ? 0.45f : 0.50f;
    int32_t delta_off   = dec->cfg.pitch_delta_offset;
    int32_t delta_bits  = dec->cfg.pitch_delta_bits;
    int32_t fcb_bits    = dec->cfg.fcb_gain_bits;

    /* Configure codebook (must match encoder) */
    tlcs_cb_config cb_cfg;
    tlcs_cb_config_init(&cb_cfg, subfr, dec->cfg.num_pulses);

    /* Minimum bytes: need at least 4 for range coder init */
    if (bytes_in < 4) return TLCS_ERR_BAD_BITSTREAM;

    /* ── Step 1: Unpack parameters via range coder ──── */
    tlcs_rc_decoder rc;
    tlcs_rc_dec_init(&rc, bitstream_in, bytes_in);

    /* V/UV flag */
    int32_t voiced = tlcs_rc_dec_symbol(&rc, ec_cdf_vuv, EC_N_VUV);

    /* Predictive LSF dequantization */
    float lsf_pred[TLCS_LPC_ORDER_MAX];
    for (int32_t i = 0; i < order; i++)
        lsf_pred[i] = (float)dec->prev_lsf[i] / 5000.0f;

    float lsf[TLCS_LPC_ORDER_MAX];
    if (dec->cfg.use_lsf_vq) {
        /* VQ: decode 4 split indices */
        int32_t vq_size = 1 << lsf_bits;
        int32_t vq_idx[LSF_VQ_NUM_SPLITS];
        for (int32_t s = 0; s < LSF_VQ_NUM_SPLITS; s++)
            vq_idx[s] = tlcs_rc_dec_uniform(&rc, vq_size);
        float delta_q[TLCS_LPC_ORDER_MAX];
        tlcs_lsf_vq_decode_n(vq_idx, order, delta_q, vq_size);
        for (int32_t i = 0; i < order; i++)
            lsf[i] = lsf_pred[i] + delta_q[i];
    } else {
        /* Scalar: decode 16 indices */
        const uint16_t *lsf_cdf = (lsf_bits == 7) ? ec_cdf_lsf_delta_7bit
                                : (lsf_bits == 6) ? ec_cdf_lsf_delta_6bit
                                : ec_cdf_lsf_delta;
        int32_t n_lsf_syms = (1 << lsf_bits);
        int16_t lsf_indices[TLCS_LPC_ORDER_MAX];
        for (int32_t i = 0; i < order; i++)
            lsf_indices[i] = (int16_t)tlcs_rc_dec_symbol(&rc, lsf_cdf, n_lsf_syms);
        tlcs_lsf_dequantize_pred_n(lsf_indices, lsf_pred, order, lsf,
                                    lsf_bits, lsf_range);
    }
    tlcs_lsf_stabilize(lsf, order);

    /* ── Step 2: Load excitation buffer ──────────────── */
    float *exc = dec->exc_buf;
    tlcs_exc_buf_shift(exc, n);

    /* Shift innovation history buffer */
    float *innov_hist = dec->innov_buf;
    memmove(innov_hist, innov_hist + n,
            (size_t)TLCS_MAX_PITCH_LAG * sizeof(float));
    memset(innov_hist + TLCS_MAX_PITCH_LAG, 0, (size_t)n * sizeof(float));

    /* ── Step 3: Per-subframe reconstruction ─────────── */
    float excitation[TLCS_MAX_FRAME_SIZE];
    int32_t dec_lags[TLCS_MAX_SUBFRAMES];
    float dec_gains[TLCS_MAX_SUBFRAMES];
    float prev_cb_gain_mag = 0.0f;
    int32_t prev_lag_idx = 0;
    int32_t half_frame = n_subfr / 2;
    for (int32_t sf = 0; sf < n_subfr; sf++) {
        int32_t sf_offset = sf * subfr;
        int32_t exc_offset = TLCS_MAX_PITCH_LAG + sf_offset;

        /* Pitch lag and ACB: only for voiced frames */
        int32_t lag = TLCS_MIN_PITCH_LAG, lag_frac = 0;
        float g0 = 0.0f, g1 = 0.0f;
        if (voiced) {
            int32_t lag_idx;
            if (sf == 0 || sf == half_frame) {
                lag_idx = tlcs_rc_dec_symbol(&rc, ec_cdf_pitch_abs,
                                              EC_N_PITCH_ABS);
            } else {
                const uint16_t *pd_cdf = (delta_bits == 6)
                                       ? ec_cdf_pitch_delta_6bit : ec_cdf_pitch_delta;
                int32_t n_pd = (1 << delta_bits);
                int32_t sym = tlcs_rc_dec_symbol(&rc, pd_cdf, n_pd);
                lag_idx = prev_lag_idx + sym - delta_off;
            }
            prev_lag_idx = lag_idx;
            tlcs_pitch_decode_lag(lag_idx, &lag, &lag_frac);

            /* Pitch gain (ACB VQ index) */
            int32_t vq_idx = tlcs_rc_dec_symbol(&rc, ec_cdf_acb_vq,
                                                 EC_N_ACB_VQ);
            if (vq_idx >= TLCS_ACB_VQ_SIZE) vq_idx = TLCS_ACB_VQ_SIZE - 1;
            g0 = tlcs_acb_vq[vq_idx][0];
            g1 = tlcs_acb_vq[vq_idx][1];
        }

        /* Codebook pulses */
        tlcs_cb_entry cb_entry;
        for (int32_t p = 0; p < cb_cfg.num_pulses; p++) {
            cb_entry.pulse_pos[p] = tlcs_rc_dec_uniform(&rc,
                                                         cb_cfg.positions_per_track);
            int32_t sign_sym = tlcs_rc_dec_uniform(&rc, 2);
            cb_entry.pulse_sign[p] = sign_sym ? 1 : -1;
        }

        /* Fixed CB gain (select CDF by bit depth) */
        const uint16_t *fcb_cdf = (fcb_bits == 6)
                                ? ec_cdf_fcb_gain_6bit : ec_cdf_fcb_gain;
        int32_t n_fcb = (1 << fcb_bits);
        int32_t fcb_gi = tlcs_rc_dec_symbol(&rc, fcb_cdf, n_fcb);
        cb_entry.gain_index = fcb_gi;
        cb_entry.gain = tlcs_fcb_gain_dequantize_pred_n(fcb_gi, prev_cb_gain_mag,
                                                         fcb_bits);
        prev_cb_gain_mag = fabsf(cb_entry.gain);

        /* Clamp pitch lag */
        if (lag < TLCS_MIN_PITCH_LAG) lag = TLCS_MIN_PITCH_LAG;
        if (lag > TLCS_MAX_PITCH_LAG) lag = TLCS_MAX_PITCH_LAG;
        dec_lags[sf] = lag;
        dec_gains[sf] = g0;

        /* 2-tap ACB basis vectors (voiced only; UV has g0=g1=0) */
        float v0[TLCS_MAX_SUBFR_SIZE], v1[TLCS_MAX_SUBFR_SIZE];
        if (voiced) {
            tlcs_pitch_get_acb_basis(exc, exc_offset, lag, lag_frac,
                                      v0, v1, subfr);
        } else {
            memset(v0, 0, (size_t)subfr * sizeof(float));
            memset(v1, 0, (size_t)subfr * sizeof(float));
        }

        /* Build innovation */
        float innovation[TLCS_MAX_SUBFR_SIZE];
        tlcs_cb_build_innovation(&cb_entry, &cb_cfg, innovation, subfr);

        /* Reconstruct excitation */
        for (int32_t i = 0; i < subfr; i++) {
            float e = g0 * v0[i] + g1 * v1[i] + innovation[i];
            excitation[sf_offset + i] = e;
            exc[exc_offset + i] = e;
        }

        /* Pitch periodicity enhancement (voiced only)
         * Stronger blending for short/medium lags (female pitch range) */
        if (voiced && lag >= 20 && g0 > 0.3f) {
            float blend_coeff = (lag < 120) ? 0.18f : 0.08f;
            float blend_max   = (lag < 120) ? 0.18f : 0.08f;
            float blend = g0 * blend_coeff;
            if (blend > blend_max) blend = blend_max;
            for (int32_t i = 0; i < subfr; i++) {
                float past_exc = exc[exc_offset + i - lag];
                float enhanced = (1.0f - blend) * exc[exc_offset + i]
                               + blend * past_exc;
                excitation[sf_offset + i] = enhanced;
                exc[exc_offset + i] = enhanced;
            }
        }
    }

    /* ── Step 4: Clamp excitation ────────────────────── */
    int32_t exc_total = TLCS_MAX_PITCH_LAG + n;
    for (int32_t i = 0; i < exc_total; i++) {
        if (exc[i] > 32767.0f) exc[i] = 32767.0f;
        if (exc[i] < -32768.0f) exc[i] = -32768.0f;
    }

    /* ── Step 5: Synthesis with per-subframe LSF interpolation ── */
    static const float lsf_alpha_4[4] = {0.75f, 0.90f, 1.0f, 1.0f};

    float prev_lsf_f[TLCS_LPC_ORDER_MAX];
    for (int32_t i = 0; i < order; i++)
        prev_lsf_f[i] = (float)dec->prev_lsf[i] / 5000.0f;

    float synth[TLCS_MAX_FRAME_SIZE];
    float syn_mem[TLCS_LPC_ORDER_MAX];
    memcpy(syn_mem, dec->synth_mem, (size_t)order * sizeof(float));

    for (int32_t sf = 0; sf < n_subfr; sf++) {
        float alpha = lsf_alpha_4[sf < n_subfr ? sf : n_subfr - 1];
        float lsf_sf[TLCS_LPC_ORDER_MAX];
        for (int32_t i = 0; i < order; i++)
            lsf_sf[i] = (1.0f - alpha) * prev_lsf_f[i] + alpha * lsf[i];
        tlcs_lsf_stabilize(lsf_sf, order);
        float a_sf[TLCS_LPC_ORDER_MAX + 1];
        tlcs_lsf_to_lpc(lsf_sf, order, a_sf);
        tlcs_synthesis_filter(a_sf, order, &excitation[sf * subfr],
                              &synth[sf * subfr], subfr, syn_mem);
    }

    memcpy(dec->synth_mem, syn_mem, (size_t)order * sizeof(float));

    /* ── Step 5.5: Harmonic post-filter ──── */
    harmonic_postfilter(synth, n, subfr, n_subfr,
                        dec_lags, dec_gains, dec->harm_pf_hist, 0.30f);

    /* ── Step 6: Formant post-filter (mild, SMPL-style) ── */
    formant_postfilter(synth, n, subfr, n_subfr, order,
                       lsf, prev_lsf_f, lsf_alpha_4,
                       0.90f, 0.95f, 0.35f,
                       dec->pf_fir_mem, dec->pf_synth_mem,
                       &dec->pf_tilt_mem);

    /* ── Step 7: Bass post-filter ─────────────────────── */
    bass_postfilter(synth, n, subfr, dec_lags, dec_gains, n_subfr,
                    dec->pf_history, &dec->pf_lp_mem);

    /* ── Step 8: De-emphasis ─────────────────────────── */
    float deemph_mem_f = (float)dec->deemph_mem;
    tlcs_deemph(synth, n, &deemph_mem_f);
    dec->deemph_mem = (int16_t)deemph_mem_f;

    /* ── Step 9: Output ──────────────────────────────── */
    for (int32_t i = 0; i < n; i++) {
        float v = synth[i];
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        pcm_out[i] = (int16_t)v;
    }

    /* Update state */
    for (int32_t i = 0; i < order; i++) {
        dec->prev_prev_lsf[i] = dec->prev_lsf[i];
        dec->prev_lsf[i] = (int16_t)(lsf[i] * 5000.0f);
    }
    dec->frame_count++;

    return TLCS_OK;
}

/* ══════════════════════════════════════════════════════════════════
 *  Entropy-Coded HR Decoder
 *  Reads parameters via range coder, synthesis identical to decode_direct.
 * ══════════════════════════════════════════════════════════════════ */

static tlcs_status decode_direct_ec(tlcs_decoder *dec,
                                     const uint8_t *bitstream_in,
                                     int32_t bytes_in,
                                     int16_t *pcm_out)
{
    const int32_t n       = dec->cfg.frame_size;
    const int32_t order   = dec->cfg.lpc_order;
    const int32_t subfr   = dec->cfg.subfr_size;
    const int32_t n_subfr = dec->cfg.n_subfr;

    int32_t lsf_bits   = dec->cfg.lsf_bits;
    float   lsf_range  = (lsf_bits <= 5) ? 0.40f : (lsf_bits == 6) ? 0.45f : 0.50f;
    int32_t delta_bits  = dec->cfg.pitch_delta_bits;
    int32_t delta_off   = dec->cfg.pitch_delta_offset;
    int32_t fcb_bits    = dec->cfg.fcb_gain_bits;

    /* Configure codebook (must match encoder) */
    tlcs_cb_config cb_cfg;
    tlcs_cb_config_init(&cb_cfg, subfr, dec->cfg.num_pulses);

    /* Minimum bytes: need at least 4 for range coder init */
    if (bytes_in < 4) return TLCS_ERR_BAD_BITSTREAM;

    /* ── Step 1: Unpack parameters via range coder ──── */
    tlcs_rc_decoder rc;
    tlcs_rc_dec_init(&rc, bitstream_in, bytes_in);

    /* V/UV flag */
    int32_t voiced = tlcs_rc_dec_symbol(&rc, ec_cdf_vuv, EC_N_VUV);

    /* Predictive LSF dequantization */
    float lsf_pred[TLCS_LPC_ORDER_MAX];
    for (int32_t i = 0; i < order; i++)
        lsf_pred[i] = (float)dec->prev_lsf[i] / 5000.0f;

    float lsf[TLCS_LPC_ORDER_MAX];
    if (dec->cfg.use_lsf_vq) {
        int32_t vq_size = 1 << lsf_bits;
        int32_t vq_idx[LSF_VQ_NUM_SPLITS];
        for (int32_t s = 0; s < LSF_VQ_NUM_SPLITS; s++)
            vq_idx[s] = tlcs_rc_dec_uniform(&rc, vq_size);
        float delta_q[TLCS_LPC_ORDER_MAX];
        tlcs_lsf_vq_decode_n(vq_idx, order, delta_q, vq_size);
        for (int32_t i = 0; i < order; i++)
            lsf[i] = lsf_pred[i] + delta_q[i];
    } else {
        const uint16_t *lsf_cdf = (lsf_bits == 7) ? ec_cdf_lsf_delta_7bit
                                 : (lsf_bits == 6) ? ec_cdf_lsf_delta_6bit
                                 : ec_cdf_lsf_delta;
        int32_t n_lsf_syms = (1 << lsf_bits);
        int16_t lsf_indices[TLCS_LPC_ORDER_MAX];
        for (int32_t i = 0; i < order; i++)
            lsf_indices[i] = (int16_t)tlcs_rc_dec_symbol(&rc, lsf_cdf, n_lsf_syms);
        tlcs_lsf_dequantize_pred_n(lsf_indices, lsf_pred, order, lsf,
                                    lsf_bits, lsf_range);
    }
    tlcs_lsf_stabilize(lsf, order);

    /* ── Step 2: Load excitation buffer ──────────────── */
    float *exc = dec->exc_buf;
    tlcs_exc_buf_shift(exc, n);

    /* Shift innovation history buffer */
    float *innov_hist = dec->innov_buf;
    memmove(innov_hist, innov_hist + n,
            (size_t)TLCS_MAX_PITCH_LAG * sizeof(float));
    memset(innov_hist + TLCS_MAX_PITCH_LAG, 0, (size_t)n * sizeof(float));

    /* ── Step 3: Per-subframe reconstruction ─────────── */
    float excitation[TLCS_MAX_FRAME_SIZE];
    int32_t dec_lags[TLCS_MAX_SUBFRAMES];
    float dec_gains[TLCS_MAX_SUBFRAMES];
    float prev_cb_gain_mag = 0.0f;
    int32_t prev_lag_idx = 0;
    int32_t half_frame = n_subfr / 2;
    for (int32_t sf = 0; sf < n_subfr; sf++) {
        int32_t sf_offset = sf * subfr;
        int32_t exc_offset = TLCS_MAX_PITCH_LAG + sf_offset;

        /* Pitch lag and ACB: only for voiced frames */
        int32_t lag = TLCS_MIN_PITCH_LAG, lag_frac = 0;
        float g0 = 0.0f, g1 = 0.0f;
        if (voiced) {
            int32_t lag_idx;
            if (sf == 0 || sf == half_frame) {
                lag_idx = tlcs_rc_dec_symbol(&rc, ec_cdf_pitch_abs,
                                              EC_N_PITCH_ABS);
            } else {
                const uint16_t *pd_cdf = (delta_bits == 7)
                    ? ec_cdf_pitch_delta_7bit
                    : (delta_bits == 6) ? ec_cdf_pitch_delta_6bit
                    : ec_cdf_pitch_delta;
                int32_t n_pd = (1 << delta_bits);
                int32_t sym = tlcs_rc_dec_symbol(&rc, pd_cdf, n_pd);
                lag_idx = prev_lag_idx + sym - delta_off;
            }
            prev_lag_idx = lag_idx;
            tlcs_pitch_decode_lag(lag_idx, &lag, &lag_frac);

            /* Pitch gain (ACB VQ index) */
            int32_t vq_idx = tlcs_rc_dec_symbol(&rc, ec_cdf_acb_vq,
                                                 EC_N_ACB_VQ);
            if (vq_idx >= TLCS_ACB_VQ_SIZE) vq_idx = TLCS_ACB_VQ_SIZE - 1;
            g0 = tlcs_acb_vq[vq_idx][0];
            g1 = tlcs_acb_vq[vq_idx][1];
        }

        /* Codebook pulses */
        tlcs_cb_entry cb_entry;
        for (int32_t p = 0; p < cb_cfg.num_pulses; p++) {
            cb_entry.pulse_pos[p] = tlcs_rc_dec_uniform(&rc,
                                                         cb_cfg.positions_per_track);
            int32_t sign_sym = tlcs_rc_dec_uniform(&rc, 2);
            cb_entry.pulse_sign[p] = sign_sym ? 1 : -1;
        }

        /* Fixed CB gain (select CDF by bit depth) */
        const uint16_t *fcb_cdf = (fcb_bits == 7) ? ec_cdf_fcb_gain_7bit
                                : (fcb_bits == 6) ? ec_cdf_fcb_gain_6bit
                                : ec_cdf_fcb_gain;
        int32_t n_fcb = (1 << fcb_bits);
        int32_t fcb_gi = tlcs_rc_dec_symbol(&rc, fcb_cdf, n_fcb);
        cb_entry.gain_index = fcb_gi;
        cb_entry.gain = tlcs_fcb_gain_dequantize_pred_n(fcb_gi, prev_cb_gain_mag,
                                                         fcb_bits);
        prev_cb_gain_mag = fabsf(cb_entry.gain);

        /* Clamp pitch lag */
        if (lag < TLCS_MIN_PITCH_LAG) lag = TLCS_MIN_PITCH_LAG;
        if (lag > TLCS_MAX_PITCH_LAG) lag = TLCS_MAX_PITCH_LAG;
        dec_lags[sf] = lag;
        dec_gains[sf] = g0;

        /* 2-tap ACB basis vectors (voiced only; UV has g0=g1=0) */
        float v0[TLCS_MAX_SUBFR_SIZE], v1[TLCS_MAX_SUBFR_SIZE];
        if (voiced) {
            tlcs_pitch_get_acb_basis(exc, exc_offset, lag, lag_frac,
                                      v0, v1, subfr);
        } else {
            memset(v0, 0, (size_t)subfr * sizeof(float));
            memset(v1, 0, (size_t)subfr * sizeof(float));
        }

        /* Build innovation */
        float innovation[TLCS_MAX_SUBFR_SIZE];
        tlcs_cb_build_innovation(&cb_entry, &cb_cfg, innovation, subfr);

        /* Pitch sharpening with cross-subframe innovation history */
        int32_t innov_off = TLCS_MAX_PITCH_LAG + sf_offset;
        for (int32_t i = 0; i < subfr; i++)
            innov_hist[innov_off + i] = innovation[i];

        if (g0 >= 0.6f && lag >= 20) {
            float beta;
            if (lag < subfr) {
                beta = g0 * 0.4f;
                if (beta > 0.4f) beta = 0.4f;
            } else if (lag < 100) {
                beta = g0 * 0.20f;
                if (beta > 0.20f) beta = 0.20f;
            } else {
                beta = 0.0f;
            }
            if (beta > 0.0f) {
                for (int32_t i = 0; i < subfr; i++) {
                    int32_t src = innov_off + i - lag;
                    if (src >= 0)
                        innovation[i] += beta * innov_hist[src];
                }
            }
        }

        for (int32_t i = 0; i < subfr; i++)
            innov_hist[innov_off + i] = innovation[i];

        /* Phase dispersion */
        tlcs_cb_phase_disperse(innovation, subfr, g0);

        /* Innovation filtering: reduce inter-harmonic noise in voiced frames */
        if (lag >= 20 && g0 > 0.5f) {
            /* Stronger filtering for short lags (female pitch range)
             * where inter-harmonic noise is more perceptible */
            float alpha_innov = (lag < 80) ? 0.12f * g0 : 0.07f * g0;
            float alpha_max   = (lag < 80) ? 0.16f : 0.10f;
            if (alpha_innov > alpha_max) alpha_innov = alpha_max;
            float prev = dec->innov_filt_mem;
            for (int32_t i = 0; i < subfr; i++) {
                float next = (i + 1 < subfr) ? innovation[i + 1] : 0.0f;
                float filtered = innovation[i]
                               - alpha_innov * (prev + next);
                prev = innovation[i];
                innovation[i] = filtered;
            }
            dec->innov_filt_mem = prev;
        }

        /* Reconstruct excitation */
        for (int32_t i = 0; i < subfr; i++) {
            float e = g0 * v0[i] + g1 * v1[i] + innovation[i];
            excitation[sf_offset + i] = e;
            exc[exc_offset + i] = e;
        }

        /* Pitch periodicity enhancement */
        if (lag >= 20 && g0 > 0.3f) {
            float blend = g0 * 0.08f;
            if (blend > 0.08f) blend = 0.08f;
            for (int32_t i = 0; i < subfr; i++) {
                float past_exc = exc[exc_offset + i - lag];
                float enhanced = (1.0f - blend) * exc[exc_offset + i]
                               + blend * past_exc;
                excitation[sf_offset + i] = enhanced;
                exc[exc_offset + i] = enhanced;
            }
        }
    }

    /* ── Step 4: Clamp excitation ────────────────────── */
    int32_t exc_total = TLCS_MAX_PITCH_LAG + n;
    for (int32_t i = 0; i < exc_total; i++) {
        if (exc[i] > 32767.0f) exc[i] = 32767.0f;
        if (exc[i] < -32768.0f) exc[i] = -32768.0f;
    }

    /* ── Step 5: Synthesis with per-subframe LSF interpolation ── */
    static const float lsf_alpha_8[8] = {0.55f, 0.65f, 0.75f, 0.85f, 0.92f, 0.96f, 1.0f, 1.0f};
    static const float lsf_alpha_4[4] = {0.75f, 0.90f, 1.0f, 1.0f};
    const float *lsf_alpha = (n_subfr <= 4) ? lsf_alpha_4 : lsf_alpha_8;

    float prev_lsf_f[TLCS_LPC_ORDER_MAX];
    for (int32_t i = 0; i < order; i++)
        prev_lsf_f[i] = (float)dec->prev_lsf[i] / 5000.0f;

    float synth[TLCS_MAX_FRAME_SIZE];
    float syn_mem[TLCS_LPC_ORDER_MAX];
    memcpy(syn_mem, dec->synth_mem, (size_t)order * sizeof(float));

    for (int32_t sf = 0; sf < n_subfr; sf++) {
        float alpha = lsf_alpha[sf < n_subfr ? sf : n_subfr - 1];
        float lsf_sf[TLCS_LPC_ORDER_MAX];
        for (int32_t i = 0; i < order; i++)
            lsf_sf[i] = (1.0f - alpha) * prev_lsf_f[i] + alpha * lsf[i];
        tlcs_lsf_stabilize(lsf_sf, order);
        float a_sf[TLCS_LPC_ORDER_MAX + 1];
        tlcs_lsf_to_lpc(lsf_sf, order, a_sf);
        tlcs_synthesis_filter(a_sf, order, &excitation[sf * subfr],
                              &synth[sf * subfr], subfr, syn_mem);
    }

    memcpy(dec->synth_mem, syn_mem, (size_t)order * sizeof(float));

    /* ── Step 5.5: Harmonic post-filter (LR only) ─── */
    if (n_subfr <= 4) {
        harmonic_postfilter(synth, n, subfr, n_subfr,
                            dec_lags, dec_gains, dec->harm_pf_hist, 0.30f);
    }

    /* ── Step 6: Post-filter ─────────────────────────── */
    if (n_subfr <= 4) {
        /* LR: formant postfilter */
        formant_postfilter(synth, n, subfr, n_subfr, order,
                           lsf, prev_lsf_f, lsf_alpha,
                           0.55f, 0.75f, 0.55f,
                           dec->pf_fir_mem, dec->pf_synth_mem,
                           &dec->pf_tilt_mem);
    } else {
        /* HR: adaptive tilt-only post-filter */
        for (int32_t sf = 0; sf < n_subfr; sf++) {
            int32_t offset = sf * subfr;
            float r0 = 0.0f, r1 = 0.0f;
            for (int32_t i = 0; i < subfr; i++)
                r0 += synth[offset + i] * synth[offset + i];
            for (int32_t i = 1; i < subfr; i++)
                r1 += synth[offset + i] * synth[offset + i - 1];
            float k1 = (r0 > 1.0f) ? r1 / r0 : 0.0f;
            float tc = (k1 > 0.0f) ? 0.25f * k1 : 0.0f;
            float prev_s = dec->pf_tilt_mem;
            float orig_e = 0.0f, filt_e = 0.0f;
            float tmp[TLCS_MAX_SUBFR_SIZE];
            for (int32_t i = 0; i < subfr; i++) {
                orig_e += synth[offset + i] * synth[offset + i];
                tmp[i] = synth[offset + i] - tc * prev_s;
                prev_s = synth[offset + i];
                filt_e += tmp[i] * tmp[i];
            }
            dec->pf_tilt_mem = prev_s;
            float gain = (filt_e > 1.0f) ? sqrtf(orig_e / filt_e) : 1.0f;
            if (gain > 2.0f) gain = 2.0f;
            for (int32_t i = 0; i < subfr; i++)
                synth[offset + i] = tmp[i] * gain;
        }
    }

    /* ── Step 7: Bass post-filter ─────────────────────── */
    bass_postfilter(synth, n, subfr, dec_lags, dec_gains, n_subfr,
                    dec->pf_history, &dec->pf_lp_mem);

    /* ── Step 8: De-emphasis ─────────────────────────── */
    float deemph_mem_f = (float)dec->deemph_mem;
    tlcs_deemph(synth, n, &deemph_mem_f);
    dec->deemph_mem = (int16_t)deemph_mem_f;

    /* ── Step 9: Output ──────────────────────────────── */
    for (int32_t i = 0; i < n; i++) {
        float v = synth[i];
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        pcm_out[i] = (int16_t)v;
    }

    /* Update state */
    for (int32_t i = 0; i < order; i++) {
        dec->prev_prev_lsf[i] = dec->prev_lsf[i];
        dec->prev_lsf[i] = (int16_t)(lsf[i] * 5000.0f);
    }
    dec->frame_count++;

    return TLCS_OK;
}

/* ══════════════════════════════════════════════════════════════════
 *  VLR CELP Decoder (2×160, SMPL-style pitch sharpening)
 *  Fixed-width bitstream, aggressive pitch sharpening, LR post-filters.
 * ══════════════════════════════════════════════════════════════════ */

/* decode_vlr_core: shared CELP decode logic for VLR and LR Mode S.
 * If ext_bsr is non-NULL, uses it (reader already past mode bit).
 * If ext_bsr is NULL, creates own reader from bitstream_in. */
static tlcs_status decode_vlr_core(tlcs_decoder *dec,
                                     const uint8_t *bitstream_in,
                                     int32_t bytes_in,
                                     int16_t *pcm_out,
                                     tlcs_bs_reader *ext_bsr)
{
    const int32_t n       = dec->cfg.frame_size;
    const int32_t order   = dec->cfg.lpc_order;
    const int32_t subfr   = dec->cfg.subfr_size;
    const int32_t n_subfr = dec->cfg.n_subfr;

    int32_t lsf_bits   = dec->cfg.lsf_bits;
    float   lsf_range  = (lsf_bits <= 5) ? 0.40f : (lsf_bits == 6) ? 0.45f : 0.50f;
    int32_t delta_bits  = dec->cfg.pitch_delta_bits;
    int32_t delta_off   = dec->cfg.pitch_delta_offset;
    int32_t fcb_bits    = dec->cfg.fcb_gain_bits;

    /* Configure codebook (must match encoder) */
    tlcs_cb_config cb_cfg;
    tlcs_cb_config_init(&cb_cfg, subfr, dec->cfg.num_pulses);

    /* Validate bitstream size */
    int32_t n_abs = 1;  /* VLR: single absolute pitch */
    int32_t subfr_other_bits = TLCS_PITCH_GAIN_BITS
                             + cb_cfg.num_pulses * (cb_cfg.pos_bits + 1)
                             + fcb_bits;
    int32_t pitch_bits = n_abs * 9 + (n_subfr - n_abs) * delta_bits;
    int32_t lsf_total_bits = dec->cfg.use_lsf_vq
                           ? LSF_VQ_NUM_SPLITS * lsf_bits
                           : order * lsf_bits;
    int32_t total_bits = lsf_total_bits + pitch_bits + n_subfr * subfr_other_bits;
    if (ext_bsr) total_bits += 1; /* account for mode bit already read */
    int32_t expected_bytes = (total_bits + 7) / 8;
    if (bytes_in < expected_bytes) return TLCS_ERR_BAD_BITSTREAM;

    /* ── Step 1: Unpack parameters ──────────────────────── */
    tlcs_bs_reader local_bsr;
    tlcs_bs_reader *bsr_ptr;
    if (ext_bsr) {
        bsr_ptr = ext_bsr;
    } else {
        tlcs_bs_reader_init(&local_bsr, bitstream_in, bytes_in);
        bsr_ptr = &local_bsr;
    }
    #define bsr (*bsr_ptr)

    /* Predictive LSF dequantization */
    float lsf_pred[TLCS_LPC_ORDER_MAX];
    for (int32_t i = 0; i < order; i++)
        lsf_pred[i] = (float)dec->prev_lsf[i] / 5000.0f;

    float lsf[TLCS_LPC_ORDER_MAX];
    if (dec->cfg.use_lsf_vq) {
        int32_t vq_size = 1 << lsf_bits;
        int32_t vq_idx[LSF_VQ_NUM_SPLITS];
        for (int32_t s = 0; s < LSF_VQ_NUM_SPLITS; s++) {
            uint32_t val;
            tlcs_bs_read(&bsr, &val, lsf_bits);
            vq_idx[s] = (int32_t)val;
        }
        float delta_q[TLCS_LPC_ORDER_MAX];
        tlcs_lsf_vq_decode_n(vq_idx, order, delta_q, vq_size);
        for (int32_t i = 0; i < order; i++)
            lsf[i] = lsf_pred[i] + delta_q[i];
    } else {
        int16_t lsf_indices[TLCS_LPC_ORDER_MAX];
        for (int32_t i = 0; i < order; i++) {
            uint32_t val;
            tlcs_bs_read(&bsr, &val, lsf_bits);
            lsf_indices[i] = (int16_t)val;
        }
        tlcs_lsf_dequantize_pred_n(lsf_indices, lsf_pred, order, lsf,
                                    lsf_bits, lsf_range);
    }
    tlcs_lsf_stabilize(lsf, order);

    /* ── Step 2: Load excitation buffer ──────────────────── */
    float *exc = dec->exc_buf;
    tlcs_exc_buf_shift(exc, n);

    /* Shift innovation history buffer */
    float *innov_hist = dec->innov_buf;
    memmove(innov_hist, innov_hist + n,
            (size_t)TLCS_MAX_PITCH_LAG * sizeof(float));
    memset(innov_hist + TLCS_MAX_PITCH_LAG, 0, (size_t)n * sizeof(float));

    /* ── Step 3: Per-subframe reconstruction ─────────────── */
    float excitation[TLCS_MAX_FRAME_SIZE];
    int32_t dec_lags[TLCS_MAX_SUBFRAMES];
    float dec_gains[TLCS_MAX_SUBFRAMES];
    float prev_cb_gain_mag = 0.0f;
    int32_t prev_lag_idx = 0;
    for (int32_t sf = 0; sf < n_subfr; sf++) {
        int32_t sf_offset = sf * subfr;
        int32_t exc_offset = TLCS_MAX_PITCH_LAG + sf_offset;

        /* Pitch lag: absolute for sf=0, delta for rest */
        int32_t lag_idx;
        if (sf == 0) {
            uint32_t lag_val;
            tlcs_bs_read(&bsr, &lag_val, 9);
            lag_idx = (int32_t)lag_val;
        } else {
            uint32_t delta_val;
            tlcs_bs_read(&bsr, &delta_val, delta_bits);
            lag_idx = prev_lag_idx + (int32_t)delta_val - delta_off;
        }
        prev_lag_idx = lag_idx;
        int32_t lag, lag_frac;
        tlcs_pitch_decode_lag(lag_idx, &lag, &lag_frac);

        /* Pitch gain (ACB VQ index) */
        uint32_t gi_val;
        tlcs_bs_read(&bsr, &gi_val, TLCS_PITCH_GAIN_BITS);
        int32_t vq_idx = (int32_t)gi_val;
        if (vq_idx >= TLCS_ACB_VQ_SIZE) vq_idx = TLCS_ACB_VQ_SIZE - 1;
        float g0 = tlcs_acb_vq[vq_idx][0];
        float g1 = tlcs_acb_vq[vq_idx][1];

        /* Codebook pulses */
        tlcs_cb_entry cb_entry;
        for (int32_t p = 0; p < cb_cfg.num_pulses; p++) {
            uint32_t pos_val = 0, sign_val;
            if (cb_cfg.pos_bits > 0)
                tlcs_bs_read(&bsr, &pos_val, (int32_t)cb_cfg.pos_bits);
            tlcs_bs_read(&bsr, &sign_val, 1);
            cb_entry.pulse_pos[p] = (int32_t)pos_val;
            cb_entry.pulse_sign[p] = sign_val ? 1 : -1;
        }

        /* Fixed CB gain */
        uint32_t fcb_gi;
        tlcs_bs_read(&bsr, &fcb_gi, fcb_bits);
        cb_entry.gain_index = (int32_t)fcb_gi;
        cb_entry.gain = tlcs_fcb_gain_dequantize_pred_n(cb_entry.gain_index, prev_cb_gain_mag, fcb_bits);
        prev_cb_gain_mag = fabsf(cb_entry.gain);

        /* Clamp pitch lag */
        if (lag < TLCS_MIN_PITCH_LAG) lag = TLCS_MIN_PITCH_LAG;
        if (lag > TLCS_MAX_PITCH_LAG) lag = TLCS_MAX_PITCH_LAG;
        dec_lags[sf] = lag;
        dec_gains[sf] = g0;

        /* 2-tap ACB basis vectors */
        float v0[TLCS_MAX_SUBFR_SIZE], v1[TLCS_MAX_SUBFR_SIZE];
        tlcs_pitch_get_acb_basis(exc, exc_offset, lag, lag_frac,
                                  v0, v1, subfr);

        /* Build innovation */
        float innovation[TLCS_MAX_SUBFR_SIZE];
        tlcs_cb_build_innovation(&cb_entry, &cb_cfg, innovation, subfr);

        /* Decoder noise fill (SMPL-style): fill zero positions with
         * spectrally-shaped noise to smooth sparse pulse excitation.
         *
         * Key ideas from SMPL:
         *   - Voiced: low gain, HP-filtered noise (preserves pitch clarity)
         *   - Unvoiced: higher gain, LP-filtered @ 800 Hz (natural frication)
         *   - Scale by innovation envelope, not just global RMS
         *   - HP filter removes LF energy that would boom
         */
        {
            int voiced = (g0 > 0.3f && lag >= 20) ? 1 : 0;
            float nf_gain = voiced ? TUNE_CELP_NF_VOICED : TUNE_CELP_NF_UNVOICED;

            /* Compute local envelope from non-zero pulses (smoothed) */
            float env[TLCS_MAX_SUBFR_SIZE];
            float innov_energy = 0.0f;
            int32_t nz = 0;
            for (int32_t i = 0; i < subfr; i++) {
                if (innovation[i] != 0.0f) {
                    innov_energy += innovation[i] * innovation[i];
                    nz++;
                }
            }
            if (nz > 0 && nf_gain > 0.001f) {
                float innov_rms = sqrtf(innov_energy / (float)nz);

                /* Generate raw noise */
                float noise[TLCS_MAX_SUBFR_SIZE];
                for (int32_t i = 0; i < subfr; i++)
                    noise[i] = prng_float(&dec->noise_seed);

                /* Shape noise: HP for voiced (remove boom), LP for unvoiced (natural) */
                if (voiced) {
                    /* 1st-order HP: y[n] = x[n] - 0.7*x[n-1] */
                    float prev = 0.0f;
                    for (int32_t i = 0; i < subfr; i++) {
                        float raw = noise[i];
                        noise[i] = raw - 0.7f * prev;
                        prev = raw;
                    }
                } else {
                    /* 1st-order LP @ ~800 Hz: y[n] = 0.85*y[n-1] + 0.15*x[n] */
                    float lp_coeff = 0.85f;
                    float prev_out = 0.0f;
                    for (int32_t i = 0; i < subfr; i++) {
                        prev_out = lp_coeff * prev_out + (1.0f - lp_coeff) * noise[i];
                        noise[i] = prev_out;
                    }
                }

                /* Compute smoothed pulse envelope for amplitude shaping */
                float pulse_env = 0.0f;
                float env_alpha = 0.95f;
                for (int32_t i = 0; i < subfr; i++) {
                    float abs_inn = fabsf(innovation[i]);
                    if (abs_inn > 0.0f)
                        pulse_env = abs_inn;
                    else
                        pulse_env *= env_alpha;  /* decay between pulses */
                    env[i] = pulse_env;
                }

                /* Fill zeros with shaped, envelope-scaled noise */
                for (int32_t i = 0; i < subfr; i++) {
                    if (innovation[i] == 0.0f) {
                        float scale = (env[i] > 0.01f * innov_rms)
                                    ? env[i] : 0.3f * innov_rms;
                        innovation[i] = noise[i] * nf_gain * scale;
                    }
                }
            }
        }

        /* Reconstruct excitation: ACB + FCB */
        for (int32_t i = 0; i < subfr; i++) {
            float e = g0 * v0[i] + g1 * v1[i] + innovation[i];
            excitation[sf_offset + i] = e;
            exc[exc_offset + i] = e;
        }

        /* Pitch sharpening (SMPL-style): sharpen pitch pulses in excitation.
         * SMPL uses 0.9881; we use a tunable coefficient (default 0.85).
         * Applied as a 1-tap comb filter: exc[i] += sharp * g0 * exc[i - lag] */
        if (lag >= 20 && g0 > 0.4f) {
            float sharp = TUNE_CELP_PITCH_SHARP;
            for (int32_t i = 0; i < subfr; i++) {
                int32_t idx = exc_offset + i - lag;
                if (idx >= 0) {
                    float ps = sharp * g0 * exc[idx];
                    exc[exc_offset + i] += ps;
                    excitation[sf_offset + i] += ps;
                }
            }
        }

    }

    /* ── Step 4: Clamp excitation ────────────────────────── */
    int32_t exc_total = TLCS_MAX_PITCH_LAG + n;
    for (int32_t i = 0; i < exc_total; i++) {
        if (exc[i] > 32767.0f) exc[i] = 32767.0f;
        if (exc[i] < -32768.0f) exc[i] = -32768.0f;
    }

    /* ── Step 5: Synthesis with per-subframe LSF interpolation ── */
    static const float lsf_alpha_2[2] = {0.50f, 1.0f};
    static const float lsf_alpha_4[4] = {0.75f, 0.90f, 1.0f, 1.0f};
    const float *lsf_alpha_vlr = (n_subfr <= 2) ? lsf_alpha_2 : lsf_alpha_4;

    float prev_lsf_f[TLCS_LPC_ORDER_MAX];
    for (int32_t i = 0; i < order; i++)
        prev_lsf_f[i] = (float)dec->prev_lsf[i] / 5000.0f;

    float synth[TLCS_MAX_FRAME_SIZE];
    float syn_mem[TLCS_LPC_ORDER_MAX];
    memcpy(syn_mem, dec->synth_mem, (size_t)order * sizeof(float));

    for (int32_t sf = 0; sf < n_subfr; sf++) {
        float alpha = lsf_alpha_vlr[sf < n_subfr ? sf : n_subfr - 1];
        float lsf_sf[TLCS_LPC_ORDER_MAX];
        for (int32_t i = 0; i < order; i++)
            lsf_sf[i] = (1.0f - alpha) * prev_lsf_f[i] + alpha * lsf[i];
        tlcs_lsf_stabilize(lsf_sf, order);
        float a_sf[TLCS_LPC_ORDER_MAX + 1];
        tlcs_lsf_to_lpc(lsf_sf, order, a_sf);
        tlcs_synthesis_filter(a_sf, order, &excitation[sf * subfr],
                              &synth[sf * subfr], subfr, syn_mem);
    }

    memcpy(dec->synth_mem, syn_mem, (size_t)order * sizeof(float));

    /* ── Step 5.5: Harmonic post-filter ──── */
    harmonic_postfilter(synth, n, subfr, n_subfr,
                        dec_lags, dec_gains, dec->harm_pf_hist,
                        TUNE_CELP_HARM_STR);

    /* ── Step 6: Formant post-filter ── */
    formant_postfilter(synth, n, subfr, n_subfr, order,
                       lsf, prev_lsf_f, lsf_alpha_vlr,
                       TUNE_CELP_PF_NUM, TUNE_CELP_PF_DEN, TUNE_CELP_PF_TILT,
                       dec->pf_fir_mem, dec->pf_synth_mem,
                       &dec->pf_tilt_mem);

    /* ── Step 7: Bass post-filter ─────────────────────── */
    bass_postfilter(synth, n, subfr, dec_lags, dec_gains, n_subfr,
                    dec->pf_history, &dec->pf_lp_mem);

    /* ── Step 7.5: HP post-filter (50 Hz HP + 3 kHz shelf) ── */
    {
        /* 2nd-order Butterworth HP at 50 Hz (fs=16 kHz)
         * Removes DC and sub-bass rumble.
         * Coefficients from bilinear transform:
         *   b = [0.9903, -1.9806, 0.9903]
         *   a = [1.0,    -1.9806, 0.9613]
         */
        const float b0 = 0.9903f, b1 = -1.9806f, b2 = 0.9903f;
        const float a1 = -1.9806f, a2 = 0.9613f;
        for (int32_t i = 0; i < n; i++) {
            float x = synth[i];
            float y = b0 * x + b1 * dec->hp50_x1 + b2 * dec->hp50_x2
                              - a1 * dec->hp50_y1 - a2 * dec->hp50_y2;
            dec->hp50_x2 = dec->hp50_x1;
            dec->hp50_x1 = x;
            dec->hp50_y2 = dec->hp50_y1;
            dec->hp50_y1 = y;
            synth[i] = y;
        }

        /* 1st-order high-frequency emphasis at ~3 kHz, ~3 dB presence boost.
         * y[n] = x[n] + alpha * (x[n] - x[n-1])
         * This is a 1st-order differentiator shelf: flat at DC, +alpha at Nyquist,
         * with the 3 dB point around 3 kHz for alpha ≈ 0.25. */
        float shelf_alpha = TUNE_CELP_HP_SHELF;
        for (int32_t i = 0; i < n; i++) {
            float x = synth[i];
            float y = x + shelf_alpha * (x - dec->shelf_prev_x);
            dec->shelf_prev_x = x;
            synth[i] = y;
        }
    }

    /* ── Step 8: De-emphasis ─────────────────────────── */
    float deemph_mem_f = (float)dec->deemph_mem;
    tlcs_deemph(synth, n, &deemph_mem_f);
    dec->deemph_mem = (int16_t)deemph_mem_f;

    /* ── Step 9: Output ──────────────────────────────── */
    for (int32_t i = 0; i < n; i++) {
        float v = synth[i];
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        pcm_out[i] = (int16_t)v;
    }

    /* Update state */
    for (int32_t i = 0; i < order; i++) {
        dec->prev_prev_lsf[i] = dec->prev_lsf[i];
        dec->prev_lsf[i] = (int16_t)(lsf[i] * 5000.0f);
    }
    dec->prev_codec_mode = TLCS_CODEC_MODE_S;
    dec->frame_count++;

    return TLCS_OK;
    #undef bsr
}

/* Thin wrapper: VLR decode without external reader */
static tlcs_status decode_vlr(tlcs_decoder *dec,
                                const uint8_t *bitstream_in,
                                int32_t bytes_in,
                                int16_t *pcm_out)
{
    return decode_vlr_core(dec, bitstream_in, bytes_in, pcm_out, NULL);
}

/* ══════════════════════════════════════════════════════════════════
 *  Generic CELP Decoder (used for both HR and LR modes)
 *  Reads bit widths from dec->cfg.
 * ══════════════════════════════════════════════════════════════════ */

static tlcs_status decode_direct(tlcs_decoder *dec,
                                  const uint8_t *bitstream_in,
                                  int32_t bytes_in,
                                  int16_t *pcm_out)
{
    const int32_t n       = dec->cfg.frame_size;
    const int32_t order   = dec->cfg.lpc_order;
    const int32_t subfr   = dec->cfg.subfr_size;
    const int32_t n_subfr = dec->cfg.n_subfr;

    int32_t lsf_bits   = dec->cfg.lsf_bits;
    float   lsf_range  = (lsf_bits <= 5) ? 0.40f : (lsf_bits == 6) ? 0.45f : 0.50f;
    int32_t delta_bits  = dec->cfg.pitch_delta_bits;
    int32_t delta_off   = dec->cfg.pitch_delta_offset;
    int32_t fcb_bits    = dec->cfg.fcb_gain_bits;

    /* Configure codebook (must match encoder) */
    tlcs_cb_config cb_cfg;
    tlcs_cb_config_init(&cb_cfg, subfr, dec->cfg.num_pulses);

    /* Validate bitstream size.
     * LR (4 subfr): 1 absolute + 3 delta
     * HR (8 subfr): 2 absolute + 6 delta */
    int32_t n_abs = (n_subfr <= 4) ? 1 : 2;
    int32_t subfr_other_bits = TLCS_PITCH_GAIN_BITS
                             + cb_cfg.num_pulses * (cb_cfg.pos_bits + 1)
                             + fcb_bits;
    int32_t pitch_bits = n_abs * 9 + (n_subfr - n_abs) * delta_bits;
    int32_t lsf_total_bits = dec->cfg.use_lsf_vq
                           ? LSF_VQ_NUM_SPLITS * lsf_bits
                           : order * lsf_bits;
    int32_t total_bits = lsf_total_bits + pitch_bits + n_subfr * subfr_other_bits;
    int32_t expected_bytes = (total_bits + 7) / 8;
    if (bytes_in < expected_bytes) return TLCS_ERR_BAD_BITSTREAM;

    /* ── Step 1: Unpack parameters ──────────────────────── */
    tlcs_bs_reader bsr;
    tlcs_bs_reader_init(&bsr, bitstream_in, bytes_in);

    /* Predictive LSF dequantization */
    float lsf_pred[TLCS_LPC_ORDER_MAX];
    for (int32_t i = 0; i < order; i++)
        lsf_pred[i] = (float)dec->prev_lsf[i] / 5000.0f;

    float lsf[TLCS_LPC_ORDER_MAX];
    if (dec->cfg.use_lsf_vq) {
        int32_t vq_size = 1 << lsf_bits;
        int32_t vq_idx[LSF_VQ_NUM_SPLITS];
        for (int32_t s = 0; s < LSF_VQ_NUM_SPLITS; s++) {
            uint32_t val;
            tlcs_bs_read(&bsr, &val, lsf_bits);
            vq_idx[s] = (int32_t)val;
        }
        float delta_q[TLCS_LPC_ORDER_MAX];
        tlcs_lsf_vq_decode_n(vq_idx, order, delta_q, vq_size);
        for (int32_t i = 0; i < order; i++)
            lsf[i] = lsf_pred[i] + delta_q[i];
    } else {
        int16_t lsf_indices[TLCS_LPC_ORDER_MAX];
        for (int32_t i = 0; i < order; i++) {
            uint32_t val;
            tlcs_bs_read(&bsr, &val, lsf_bits);
            lsf_indices[i] = (int16_t)val;
        }
        tlcs_lsf_dequantize_pred_n(lsf_indices, lsf_pred, order, lsf,
                                    lsf_bits, lsf_range);
    }
    tlcs_lsf_stabilize(lsf, order);

    /* ── Step 2: Load excitation buffer ──────────────────── */
    float *exc = dec->exc_buf;
    tlcs_exc_buf_shift(exc, n);

    /* Shift innovation history buffer */
    float *innov_hist = dec->innov_buf;
    memmove(innov_hist, innov_hist + n,
            (size_t)TLCS_MAX_PITCH_LAG * sizeof(float));
    memset(innov_hist + TLCS_MAX_PITCH_LAG, 0, (size_t)n * sizeof(float));

    /* ── Step 3: Per-subframe reconstruction ─────────────── */
    float excitation[TLCS_MAX_FRAME_SIZE];
    int32_t dec_lags[TLCS_MAX_SUBFRAMES];
    float dec_gains[TLCS_MAX_SUBFRAMES];
    float prev_cb_gain_mag = 0.0f;
    int32_t prev_lag_idx = 0;
    for (int32_t sf = 0; sf < n_subfr; sf++) {
        int32_t sf_offset = sf * subfr;
        int32_t exc_offset = TLCS_MAX_PITCH_LAG + sf_offset;

        /* Pitch lag: absolute or delta */
        int32_t lag_idx;
        int is_abs = (n_subfr <= 4) ? (sf == 0)
                                     : (sf == 0 || sf == n_subfr / 2);
        if (is_abs) {
            uint32_t lag_val;
            tlcs_bs_read(&bsr, &lag_val, 9);
            lag_idx = (int32_t)lag_val;
        } else {
            uint32_t delta_val;
            tlcs_bs_read(&bsr, &delta_val, delta_bits);
            lag_idx = prev_lag_idx + (int32_t)delta_val - delta_off;
        }
        prev_lag_idx = lag_idx;
        int32_t lag, lag_frac;
        tlcs_pitch_decode_lag(lag_idx, &lag, &lag_frac);

        /* Pitch gain (ACB VQ index) */
        uint32_t gi_val;
        tlcs_bs_read(&bsr, &gi_val, TLCS_PITCH_GAIN_BITS);
        int32_t vq_idx = (int32_t)gi_val;
        if (vq_idx >= TLCS_ACB_VQ_SIZE) vq_idx = TLCS_ACB_VQ_SIZE - 1;
        float g0 = tlcs_acb_vq[vq_idx][0];
        float g1 = tlcs_acb_vq[vq_idx][1];

        /* Codebook pulses */
        tlcs_cb_entry cb_entry;
        for (int32_t p = 0; p < cb_cfg.num_pulses; p++) {
            uint32_t pos_val = 0, sign_val;
            if (cb_cfg.pos_bits > 0)
                tlcs_bs_read(&bsr, &pos_val, (int32_t)cb_cfg.pos_bits);
            tlcs_bs_read(&bsr, &sign_val, 1);
            cb_entry.pulse_pos[p] = (int32_t)pos_val;
            cb_entry.pulse_sign[p] = sign_val ? 1 : -1;
        }

        /* Fixed CB gain */
        uint32_t fcb_gi;
        tlcs_bs_read(&bsr, &fcb_gi, fcb_bits);
        cb_entry.gain_index = (int32_t)fcb_gi;
        cb_entry.gain = tlcs_fcb_gain_dequantize_pred_n(cb_entry.gain_index, prev_cb_gain_mag, fcb_bits);
        prev_cb_gain_mag = fabsf(cb_entry.gain);

        /* Clamp pitch lag */
        if (lag < TLCS_MIN_PITCH_LAG) lag = TLCS_MIN_PITCH_LAG;
        if (lag > TLCS_MAX_PITCH_LAG) lag = TLCS_MAX_PITCH_LAG;
        dec_lags[sf] = lag;
        dec_gains[sf] = g0;

        /* 2-tap ACB basis vectors */
        float v0[TLCS_MAX_SUBFR_SIZE], v1[TLCS_MAX_SUBFR_SIZE];
        tlcs_pitch_get_acb_basis(exc, exc_offset, lag, lag_frac,
                                  v0, v1, subfr);

        /* Build innovation */
        float innovation[TLCS_MAX_SUBFR_SIZE];
        tlcs_cb_build_innovation(&cb_entry, &cb_cfg, innovation, subfr);

        /* Pitch sharpening — must match encoder exactly.
         * LR/VLR (subfr>=80): SMPL-style forward sharpening at 0.95.
         * HR (subfr<80): moderate cross-subframe sharpening. */
        int32_t innov_off = TLCS_MAX_PITCH_LAG + sf_offset;
        for (int32_t i = 0; i < subfr; i++)
            innov_hist[innov_off + i] = innovation[i];

        if (g0 >= 0.6f && lag >= 20 && subfr < 80) {
            /* HR: existing cross-subframe sharpening */
            float beta;
            if (lag < subfr) {
                beta = g0 * 0.4f;
                if (beta > 0.4f) beta = 0.4f;
            } else if (lag < 100) {
                beta = g0 * 0.20f;
                if (beta > 0.20f) beta = 0.20f;
            } else {
                beta = 0.0f;
            }
            if (beta > 0.0f) {
                for (int32_t i = 0; i < subfr; i++) {
                    int32_t src = innov_off + i - lag;
                    if (src >= 0)
                        innovation[i] += beta * innov_hist[src];
                }
            }
        }

        for (int32_t i = 0; i < subfr; i++)
            innov_hist[innov_off + i] = innovation[i];

        /* Phase dispersion */
        tlcs_cb_phase_disperse(innovation, subfr, g0);

        /* Innovation filtering: AMR-WB style periodicity enhancement.
         * High-pass filter the innovation to reduce inter-harmonic noise
         * in voiced frames. F(z) = -alpha*z + 1 - alpha*z^(-1)
         * alpha = 0.07 * voicing_ratio, capped at 0.10 */
        if (lag >= 20 && g0 > 0.5f) {
            /* Stronger filtering for short lags (female pitch range)
             * where inter-harmonic noise is more perceptible */
            float alpha_innov = (lag < 80) ? 0.12f * g0 : 0.07f * g0;
            float alpha_max   = (lag < 80) ? 0.16f : 0.10f;
            if (alpha_innov > alpha_max) alpha_innov = alpha_max;
            float prev = dec->innov_filt_mem;
            for (int32_t i = 0; i < subfr; i++) {
                float next = (i + 1 < subfr) ? innovation[i + 1] : 0.0f;
                float filtered = innovation[i]
                               - alpha_innov * (prev + next);
                prev = innovation[i];
                innovation[i] = filtered;
            }
            dec->innov_filt_mem = prev;
        }

        /* Reconstruct excitation */
        for (int32_t i = 0; i < subfr; i++) {
            float e = g0 * v0[i] + g1 * v1[i] + innovation[i];
            excitation[sf_offset + i] = e;
            exc[exc_offset + i] = e;
        }

        /* Pitch periodicity enhancement (must match encoder) */
        if (lag >= 20 && g0 > 0.3f) {
            float blend_coeff, blend_max;
            if (subfr >= 80) {
                /* LR: pitch-adaptive blend (matches encoder) */
                blend_coeff = (lag < 120) ? 0.18f : 0.08f;
                blend_max   = (lag < 120) ? 0.18f : 0.08f;
            } else {
                /* HR: fixed blend */
                blend_coeff = 0.08f;
                blend_max   = 0.08f;
            }
            float blend = g0 * blend_coeff;
            if (blend > blend_max) blend = blend_max;
            for (int32_t i = 0; i < subfr; i++) {
                float past_exc = exc[exc_offset + i - lag];
                float enhanced = (1.0f - blend) * exc[exc_offset + i]
                               + blend * past_exc;
                excitation[sf_offset + i] = enhanced;
                exc[exc_offset + i] = enhanced;
            }
        }
    }

    /* ── Step 4: Clamp excitation ────────────────────────── */
    int32_t exc_total = TLCS_MAX_PITCH_LAG + n;
    for (int32_t i = 0; i < exc_total; i++) {
        if (exc[i] > 32767.0f) exc[i] = 32767.0f;
        if (exc[i] < -32768.0f) exc[i] = -32768.0f;
    }

    /* ── Step 5: Synthesis with per-subframe LSF interpolation ── */
    static const float lsf_alpha_8[8] = {0.55f, 0.65f, 0.75f, 0.85f, 0.92f, 0.96f, 1.0f, 1.0f};
    static const float lsf_alpha_4[4] = {0.75f, 0.90f, 1.0f, 1.0f};
    const float *lsf_alpha = (n_subfr <= 4) ? lsf_alpha_4 : lsf_alpha_8;

    float prev_lsf_f[TLCS_LPC_ORDER_MAX];
    for (int32_t i = 0; i < order; i++)
        prev_lsf_f[i] = (float)dec->prev_lsf[i] / 5000.0f;

    float synth[TLCS_MAX_FRAME_SIZE];
    float syn_mem[TLCS_LPC_ORDER_MAX];
    memcpy(syn_mem, dec->synth_mem, (size_t)order * sizeof(float));

    for (int32_t sf = 0; sf < n_subfr; sf++) {
        float alpha = lsf_alpha[sf < n_subfr ? sf : n_subfr - 1];
        float lsf_sf[TLCS_LPC_ORDER_MAX];
        for (int32_t i = 0; i < order; i++)
            lsf_sf[i] = (1.0f - alpha) * prev_lsf_f[i] + alpha * lsf[i];
        tlcs_lsf_stabilize(lsf_sf, order);
        float a_sf[TLCS_LPC_ORDER_MAX + 1];
        tlcs_lsf_to_lpc(lsf_sf, order, a_sf);
        tlcs_synthesis_filter(a_sf, order, &excitation[sf * subfr],
                              &synth[sf * subfr], subfr, syn_mem);
    }

    memcpy(dec->synth_mem, syn_mem, (size_t)order * sizeof(float));

    /* ── Step 5.5: Harmonic post-filter (HR disabled — hurts PESQ) ─── */
    /* harmonic_postfilter(synth, n, subfr, n_subfr,
                        dec_lags, dec_gains, dec->harm_pf_hist, 0.10f); */

    /* ── Step 6: Post-filter ─────────────────────────────── */
    if (n_subfr <= 4) {
        /* LR: formant postfilter */
        formant_postfilter(synth, n, subfr, n_subfr, order,
                           lsf, prev_lsf_f, lsf_alpha,
                           0.55f, 0.75f, 0.55f,
                           dec->pf_fir_mem, dec->pf_synth_mem,
                           &dec->pf_tilt_mem);
    } else {
        /* HR: adaptive tilt-only post-filter (restores HF balance) */
        for (int32_t sf = 0; sf < n_subfr; sf++) {
            int32_t offset = sf * subfr;
            float r0 = 0.0f, r1 = 0.0f;
            for (int32_t i = 0; i < subfr; i++)
                r0 += synth[offset + i] * synth[offset + i];
            for (int32_t i = 1; i < subfr; i++)
                r1 += synth[offset + i] * synth[offset + i - 1];
            float k1 = (r0 > 1.0f) ? r1 / r0 : 0.0f;
            /* Only boost HF when signal is bass-heavy (k1 > 0) */
            float tc = (k1 > 0.0f) ? 0.25f * k1 : 0.0f;
            float prev_s = dec->pf_tilt_mem;
            float orig_e = 0.0f, filt_e = 0.0f;
            float tmp[TLCS_MAX_SUBFR_SIZE];
            for (int32_t i = 0; i < subfr; i++) {
                orig_e += synth[offset + i] * synth[offset + i];
                tmp[i] = synth[offset + i] - tc * prev_s;
                prev_s = synth[offset + i];
                filt_e += tmp[i] * tmp[i];
            }
            dec->pf_tilt_mem = prev_s;
            /* Gain normalization */
            float gain = (filt_e > 1.0f) ? sqrtf(orig_e / filt_e) : 1.0f;
            if (gain > 2.0f) gain = 2.0f;
            for (int32_t i = 0; i < subfr; i++)
                synth[offset + i] = tmp[i] * gain;
        }
    }

    /* ── Step 7: Bass post-filter (decoder-only) ─────────── */
    bass_postfilter(synth, n, subfr, dec_lags, dec_gains, n_subfr,
                    dec->pf_history, &dec->pf_lp_mem);

    /* ── Step 8: De-emphasis ─────────────────────────────── */
    float deemph_mem_f = (float)dec->deemph_mem;
    tlcs_deemph(synth, n, &deemph_mem_f);
    dec->deemph_mem = (int16_t)deemph_mem_f;

    /* ── Step 9: Output ──────────────────────────────────── */
    for (int32_t i = 0; i < n; i++) {
        float v = synth[i];
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        pcm_out[i] = (int16_t)v;
    }

    /* Update state (save prev→prev_prev before overwriting) */
    for (int32_t i = 0; i < order; i++) {
        dec->prev_prev_lsf[i] = dec->prev_lsf[i];
        dec->prev_lsf[i] = (int16_t)(lsf[i] * 5000.0f);
    }
    dec->frame_count++;

    return TLCS_OK;
}

/* ══════════════════════════════════════════════════════════════════
 *  TCX Decoder (Mode T)
 *  Reads spectral parameters, inverse quantizes, IMDCT, LPC synthesis.
 * ══════════════════════════════════════════════════════════════════ */

static tlcs_status decode_tcx(tlcs_decoder *dec,
                                tlcs_bs_reader *bsr,
                                int32_t bytes_in,
                                int16_t *pcm_out)
{
    const int32_t n     = dec->cfg.frame_size;
    const int32_t order = dec->cfg.lpc_order;
    int32_t lsf_bits    = dec->cfg.lsf_bits;

    /* ── Step 1: LSF dequantization (always VQ for TCX) ── */
    float lsf_pred[TLCS_LPC_ORDER_MAX];
    for (int32_t i = 0; i < order; i++)
        lsf_pred[i] = (float)dec->prev_lsf[i] / 5000.0f;

    float lsf[TLCS_LPC_ORDER_MAX];
    {
        int32_t vq_size = 1 << lsf_bits;
        int32_t vq_idx[LSF_VQ_NUM_SPLITS];
        for (int32_t s = 0; s < LSF_VQ_NUM_SPLITS; s++) {
            uint32_t val;
            tlcs_bs_read(bsr, &val, lsf_bits);
            vq_idx[s] = (int32_t)val;
        }
        float delta_q[TLCS_LPC_ORDER_MAX];
        tlcs_lsf_vq_decode_n(vq_idx, order, delta_q, vq_size);
        for (int32_t i = 0; i < order; i++)
            lsf[i] = lsf_pred[i] + delta_q[i];
    }
    tlcs_lsf_stabilize(lsf, order);

    float a_q[TLCS_LPC_ORDER_MAX + 1];
    tlcs_lsf_to_lpc(lsf, order, a_q);

    /* ── Step 2: Compute LPC envelope (needed for per-band CDFs) ── */
    float lpc_env[TLCS_MAX_FRAME_SIZE];
    tlcs_tcx_lpc_envelope(a_q, order, lpc_env, n);

    /* ── Step 3: Read TCX spectral parameters ──────────── */
    tlcs_tcx_params tcx_params;
    memset(&tcx_params, 0, sizeof(tcx_params));
    tcx_params.num_bins = n;

    /* Global gain: 7 bits */
    uint32_t gain_val;
    tlcs_bs_read(bsr, &gain_val, TCX_GAIN_BITS);
    tcx_params.global_gain_idx = (int32_t)gain_val;

    /* Step index: 3 bits */
    {
        uint32_t step_val;
        tlcs_bs_read(bsr, &step_val, TCX_STEP_BITS);
        tcx_params.step_idx = (int32_t)step_val;
    }

    /* Compute header byte offset (must match encoder) */
    int32_t lsf_total_bits = LSF_VQ_NUM_SPLITS * lsf_bits;
    int32_t hdr_bits = 1 + lsf_total_bits + TCX_GAIN_BITS + TCX_STEP_BITS;
    int32_t hdr_bytes = (hdr_bits + 7) / 8;
    int32_t rc_bytes_avail = bytes_in - hdr_bytes;
    if (rc_bytes_avail < 4) rc_bytes_avail = 4;

    /* Compute coded bins (must match encoder) */
    int32_t frame_bytes_budget = (dec->cfg.bitrate / 50 + 7) / 8;
    if (frame_bytes_budget < 18) frame_bytes_budget = 18;
    int32_t rc_bytes_budget = frame_bytes_budget - hdr_bytes;
    if (rc_bytes_budget < 4) rc_bytes_budget = 4;
    int32_t is_lr = (dec->cfg.bitrate < TLCS_LR_BITRATE_THRESHOLD);
    int32_t is_vlr = (dec->cfg.bitrate < TLCS_VLR_BITRATE_THRESHOLD);
    int32_t rc_mult = is_lr ? (is_vlr ? 7 : 8) : 2;
    int32_t num_rc_bins = rc_bytes_budget * rc_mult;
    if (num_rc_bins > n) num_rc_bins = n;

    /* Range-decode spectral bins */
    uint16_t cdf[TCX_SPEC_NSYM + 1];
    tlcs_tcx_compute_cdf(tcx_params.step_idx, cdf);

    tlcs_rc_decoder rc_dec;
    tlcs_rc_dec_init(&rc_dec, (const uint8_t *)bsr->buf + hdr_bytes, rc_bytes_avail);

    memset(tcx_params.quant, 0, (size_t)n * sizeof(int8_t));
    for (int32_t i = 0; i < num_rc_bins; i++) {
        int32_t sym = tlcs_rc_dec_symbol(&rc_dec, cdf, TCX_SPEC_NSYM);
        int32_t q = sym - TCX_QUANT_MAX;
        if (q < -TCX_QUANT_MAX) q = -TCX_QUANT_MAX;
        if (q > TCX_QUANT_MAX) q = TCX_QUANT_MAX;
        tcx_params.quant[i] = (int8_t)q;
    }
    tcx_params.num_coded_bins = num_rc_bins;

    float spec[TLCS_MAX_FRAME_SIZE];
    tlcs_tcx_decode(&tcx_params, lpc_env, spec, &dec->noise_seed);

    /* ── Step 4: Inverse MDCT ──────────────────────── */
    float synth[TLCS_MAX_FRAME_SIZE];
    tlcs_mdct_inverse(spec, synth, dec->mdct_overlap, n);

    /* ── Step 4.5: Formant post-filter ─────────────── */
    {
        float prev_lsf_f[TLCS_LPC_ORDER_MAX];
        float alpha_tbl[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        for (int32_t i = 0; i < order; i++)
            prev_lsf_f[i] = lsf[i];
        float pf_num = (dec->cfg.bitrate < TLCS_LR_BITRATE_THRESHOLD)
                      ? TUNE_PF_NUM_LR : 0.55f;
        float pf_den = (dec->cfg.bitrate < TLCS_LR_BITRATE_THRESHOLD)
                      ? TUNE_PF_DEN_LR : 0.75f;
        float pf_tilt = (dec->cfg.bitrate < TLCS_LR_BITRATE_THRESHOLD)
                       ? TUNE_PF_TILT_LR : 0.55f;
        formant_postfilter(synth, n, 80, n / 80, order,
                           lsf, prev_lsf_f, alpha_tbl,
                           pf_num, pf_den, pf_tilt,
                           dec->pf_fir_mem, dec->pf_synth_mem,
                           &dec->pf_tilt_mem);
    }

    /* ── Step 5: De-emphasis ──────────────────────── */
    float deemph_mem_f = (float)dec->deemph_mem;
    tlcs_deemph(synth, n, &deemph_mem_f);
    dec->deemph_mem = (int16_t)deemph_mem_f;

    /* ── Step 6: Output ───────────────────────────── */
    for (int32_t i = 0; i < n; i++) {
        float v = synth[i];
        if (v > 32767.0f) v = 32767.0f;
        if (v < -32768.0f) v = -32768.0f;
        pcm_out[i] = (int16_t)v;
    }

    /* Update state */
    for (int32_t i = 0; i < order; i++) {
        dec->prev_prev_lsf[i] = dec->prev_lsf[i];
        dec->prev_lsf[i] = (int16_t)(lsf[i] * 5000.0f);
    }
    for (int32_t i = 0; i < order; i++)
        dec->tcx_synth_mem[i] = synth[n - order + i];
    dec->prev_codec_mode = TLCS_CODEC_MODE_T;
    dec->frame_count++;

    return TLCS_OK;
}

/* ══════════════════════════════════════════════════════════════════
 *  HR Hybrid Decoder: reads mode bit, dispatches to CELP or TCX.
 * ══════════════════════════════════════════════════════════════════ */

static tlcs_status decode_hr_hybrid(tlcs_decoder *dec,
                                      const uint8_t *bitstream_in,
                                      int32_t bytes_in,
                                      int16_t *pcm_out)
{
    if (bytes_in < 1) return TLCS_ERR_BAD_BITSTREAM;

    tlcs_bs_reader bsr;
    tlcs_bs_reader_init(&bsr, bitstream_in, bytes_in);

    uint32_t mode_val;
    tlcs_bs_read(&bsr, &mode_val, 1);

    if (mode_val == TLCS_CODEC_MODE_T) {
        return decode_tcx(dec, &bsr, bytes_in, pcm_out);
    }

    /* Mode S (CELP): currently dead code — mode decision always returns T.
     * Fall back to decode_direct (creates its own reader from byte 0). */
    return decode_direct(dec, bitstream_in, bytes_in, pcm_out);
}

/* ══════════════════════════════════════════════════════════════════
 *  LR Hybrid Decoder: reads mode bit, dispatches to CELP or TCX.
 * ══════════════════════════════════════════════════════════════════ */

static tlcs_status decode_lr_hybrid(tlcs_decoder *dec,
                                      const uint8_t *bitstream_in,
                                      int32_t bytes_in,
                                      int16_t *pcm_out)
{
    if (bytes_in < 1) return TLCS_ERR_BAD_BITSTREAM;

    tlcs_bs_reader bsr;
    tlcs_bs_reader_init(&bsr, bitstream_in, bytes_in);

    uint32_t mode_val;
    tlcs_bs_read(&bsr, &mode_val, 1);

    if (mode_val == TLCS_CODEC_MODE_T) {
        return decode_tcx(dec, &bsr, bytes_in, pcm_out);
    }

    /* Mode S (CELP): reuse VLR decode core with reader past mode bit */
    return decode_vlr_core(dec, bitstream_in, bytes_in, pcm_out, &bsr);
}

/* ══════════════════════════════════════════════════════════════════
 *  Public API
 * ══════════════════════════════════════════════════════════════════ */

tlcs_status tlcs_decoder_init(tlcs_decoder *dec, const tlcs_config *cfg)
{
    if (!dec || !cfg) return TLCS_ERR_INVALID_ARG;
    memset(dec, 0, sizeof(*dec));
    dec->cfg = *cfg;

    tlcs_preemph_init();  /* read TLCS_PREEMPH env var */

    float lsf_tmp[TLCS_LPC_ORDER_MAX];
    init_default_lsf(lsf_tmp, cfg->lpc_order);
    for (int32_t i = 0; i < cfg->lpc_order; i++) {
        dec->prev_lsf[i] = (int16_t)(lsf_tmp[i] * 5000.0f);
        dec->prev_prev_lsf[i] = dec->prev_lsf[i];
    }

    return TLCS_OK;
}

tlcs_status tlcs_decode(tlcs_decoder *dec,
                        const uint8_t *bitstream_in,
                        int32_t bytes_in,
                        int16_t *pcm_out)
{
    if (!dec || !bitstream_in || !pcm_out) return TLCS_ERR_INVALID_ARG;

    if (dec->cfg.use_ec) {
        if (dec->cfg.bitrate < TLCS_LR_BITRATE_THRESHOLD)
            return decode_lowrate_ec(dec, bitstream_in, bytes_in, pcm_out);
        return decode_direct_ec(dec, bitstream_in, bytes_in, pcm_out);
    }

    if (dec->cfg.bitrate < TLCS_LR_BITRATE_THRESHOLD)
        return decode_lr_hybrid(dec, bitstream_in, bytes_in, pcm_out);

    return decode_hr_hybrid(dec, bitstream_in, bytes_in, pcm_out);
}

tlcs_status tlcs_decode_plc(tlcs_decoder *dec, int16_t *pcm_out)
{
    if (!dec || !pcm_out) return TLCS_ERR_INVALID_ARG;

    memset(pcm_out, 0, (size_t)dec->cfg.frame_size * sizeof(int16_t));
    dec->plc_loss_count++;
    dec->frame_count++;
    return TLCS_OK;
}
