#include "tlcs_pitch.h"
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ══════════════════════════════════════════════════════════════════
 *  Excitation Buffer Management
 * ══════════════════════════════════════════════════════════════════ */

void tlcs_exc_buf_shift(float *exc_buf, int32_t frame_size)
{
    /* Shift buffer left by frame_size:
     * The last MAX_PITCH_LAG samples become the new history. */
    memmove(exc_buf,
            exc_buf + frame_size,
            (size_t)TLCS_MAX_PITCH_LAG * sizeof(float));
    /* Zero the current frame region */
    memset(exc_buf + TLCS_MAX_PITCH_LAG, 0,
           (size_t)frame_size * sizeof(float));
}

/* ══════════════════════════════════════════════════════════════════
 *  Open-Loop Pitch Detection
 *
 *  Normalized autocorrelation on the excitation buffer.
 *  For efficiency, decimate by 2 for the coarse search, then refine.
 * ══════════════════════════════════════════════════════════════════ */

int32_t tlcs_pitch_ol_search(const float *exc_buf,
                             int32_t n, int32_t min_lag, int32_t max_lag,
                             float *voicing)
{
    /* The current frame starts at exc_buf[MAX_PITCH_LAG].
     * For lag L, the reference is exc_buf[MAX_PITCH_LAG - L .. MAX_PITCH_LAG - L + n - 1].
     * Since history occupies [0..MAX_PITCH_LAG-1], lag L references
     * samples starting at index MAX_PITCH_LAG - L, which is valid for L <= MAX_PITCH_LAG. */

    const float *cur = exc_buf + TLCS_MAX_PITCH_LAG;

    /* Compute energy of current frame (for normalization) */
    float e_cur = 0.0f;
    for (int32_t i = 0; i < n; i++) {
        e_cur += cur[i] * cur[i];
    }
    if (e_cur < 1.0f) e_cur = 1.0f;

    /* ── Phase 1: Coarse search (decimated by 2) ──────────────── */
    float best_score = -1.0f;
    int32_t best_lag = min_lag;

    for (int32_t lag = min_lag; lag <= max_lag; lag += TLCS_PITCH_OL_DECIMATE) {
        const float *ref = cur - lag;

        float corr = 0.0f;
        float e_ref = 0.0f;
        for (int32_t i = 0; i < n; i++) {
            corr  += cur[i] * ref[i];
            e_ref += ref[i] * ref[i];
        }
        if (e_ref < 1.0f) e_ref = 1.0f;

        float score = corr / sqrtf(e_cur * e_ref);

        /* Short-lag bias: gently prefer shorter lags (helps female voice
         * pitch tracking). Factor scaled from SMPL's 4kHz bias to 16kHz. */
        score *= (1.0f - (float)lag / 16384.0f);

        if (score > best_score) {
            best_score = score;
            best_lag = lag;
        }
    }

    /* ── Phase 2: Refine at full resolution around best coarse lag ─ */
    int32_t search_lo = best_lag - TLCS_PITCH_OL_DECIMATE;
    int32_t search_hi = best_lag + TLCS_PITCH_OL_DECIMATE;
    if (search_lo < min_lag) search_lo = min_lag;
    if (search_hi > max_lag) search_hi = max_lag;

    for (int32_t lag = search_lo; lag <= search_hi; lag++) {
        const float *ref = cur - lag;

        float corr = 0.0f;
        float e_ref = 0.0f;
        for (int32_t i = 0; i < n; i++) {
            corr  += cur[i] * ref[i];
            e_ref += ref[i] * ref[i];
        }
        if (e_ref < 1.0f) e_ref = 1.0f;

        float score = corr / sqrtf(e_cur * e_ref);
        score *= (1.0f - (float)lag / 16384.0f);

        if (score > best_score) {
            best_score = score;
            best_lag = lag;
        }
    }

    /* Sub-harmonic check: prefer shorter lag if it has good correlation.
     * Prevents pitch doubling, especially important for female voices. */
    if (best_score > 0.4f && best_lag > 2 * min_lag) {
        for (int32_t div = 2; div <= 3; div++) {
            int32_t sub_lag = best_lag / div;
            if (sub_lag < min_lag) continue;
            /* Check ±1 around the sub-harmonic */
            for (int32_t d = -1; d <= 1; d++) {
                int32_t test_lag = sub_lag + d;
                if (test_lag < min_lag || test_lag > max_lag) continue;
                const float *ref = cur - test_lag;
                float corr = 0.0f, e_ref = 0.0f;
                for (int32_t i = 0; i < n; i++) {
                    corr  += cur[i] * ref[i];
                    e_ref += ref[i] * ref[i];
                }
                if (e_ref < 1.0f) e_ref = 1.0f;
                float score = corr / sqrtf(e_cur * e_ref);
                /* Accept sub-harmonic if correlation is close to the best */
                if (score > 0.85f * best_score) {
                    best_score = score;
                    best_lag = test_lag;
                }
            }
        }
    }

    if (voicing) *voicing = (best_score > 0.0f) ? best_score : 0.0f;
    return best_lag;
}

/* ══════════════════════════════════════════════════════════════════
 *  Adaptive Codebook Vector Extraction
 *
 *  For lag >= subfr_size: straightforward copy from past.
 *  For lag < subfr_size: pitch repetition (copy cyclically).
 * ══════════════════════════════════════════════════════════════════ */

void tlcs_pitch_get_adaptive_vec(const float *exc_buf, int32_t exc_offset,
                                 int32_t lag, float *vec, int32_t subfr_size)
{
    for (int32_t i = 0; i < subfr_size; i++) {
        int32_t src_idx = exc_offset + i - lag;
        while (src_idx < 0) src_idx += lag;
        vec[i] = exc_buf[src_idx];
    }
}

/* ══════════════════════════════════════════════════════════════════
 *  Closed-Loop Pitch Search
 *
 *  Search integer lags around the open-loop estimate.
 *  Criterion: maximize correlation² / energy (equivalent to
 *  minimizing MSE with optimal gain).
 * ══════════════════════════════════════════════════════════════════ */

int32_t tlcs_pitch_cl_search(const float *target,
                             const float *exc_buf, int32_t exc_offset,
                             int32_t subfr_size,
                             int32_t center_lag, int32_t delta,
                             int32_t min_lag, int32_t max_lag,
                             float *gain)
{
    int32_t lo = center_lag - delta;
    int32_t hi = center_lag + delta;
    if (lo < min_lag) lo = min_lag;
    if (hi > max_lag) hi = max_lag;

    float best_score = -1e30f;
    int32_t best_lag = center_lag;
    float best_corr = 0.0f;
    float best_energy = 1.0f;

    float vec[TLCS_MAX_SUBFR_SIZE];

    for (int32_t lag = lo; lag <= hi; lag++) {
        /* Extract adaptive codebook vector */
        tlcs_pitch_get_adaptive_vec(exc_buf, exc_offset, lag, vec, subfr_size);

        /* Correlation with target and energy */
        float corr = 0.0f;
        float energy = 0.0f;
        for (int32_t i = 0; i < subfr_size; i++) {
            corr   += target[i] * vec[i];
            energy += vec[i] * vec[i];
        }
        if (energy < 1.0f) energy = 1.0f;

        /* Score: corr² / energy (proportional to SNR improvement) */
        float score = (corr * corr) / energy;

        if (score > best_score) {
            best_score = score;
            best_lag = lag;
            best_corr = corr;
            best_energy = energy;
        }
    }

    /* Optimal gain = correlation / energy */
    float g = best_corr / best_energy;

    /* Clamp gain */
    if (g < 0.0f) g = 0.0f;
    if (g > TLCS_PITCH_GAIN_MAX) g = TLCS_PITCH_GAIN_MAX;

    if (gain) *gain = g;
    return best_lag;
}

/* ══════════════════════════════════════════════════════════════════
 *  Adaptive Gain Quantization
 *
 *  16 levels (4 bits), uniform from 0 to PITCH_GAIN_MAX.
 * ══════════════════════════════════════════════════════════════════ */

int32_t tlcs_pitch_gain_quantize(float gain)
{
    if (gain < 0.0f) gain = 0.0f;
    if (gain > TLCS_PITCH_GAIN_MAX) gain = TLCS_PITCH_GAIN_MAX;

    float step = TLCS_PITCH_GAIN_MAX / (float)(TLCS_PITCH_GAIN_LEVELS - 1);
    int32_t idx = (int32_t)(gain / step + 0.5f);
    if (idx < 0) idx = 0;
    if (idx >= TLCS_PITCH_GAIN_LEVELS) idx = TLCS_PITCH_GAIN_LEVELS - 1;
    return idx;
}

float tlcs_pitch_gain_dequantize(int32_t index)
{
    if (index < 0) index = 0;
    if (index >= TLCS_PITCH_GAIN_LEVELS) index = TLCS_PITCH_GAIN_LEVELS - 1;

    float step = TLCS_PITCH_GAIN_MAX / (float)(TLCS_PITCH_GAIN_LEVELS - 1);
    return (float)index * step;
}

/* ══════════════════════════════════════════════════════════════════
 *  Fractional Pitch Lag — Sinc Interpolation
 *
 *  For lags 20-84: 1/3 sample resolution using windowed sinc
 *  interpolation (10-tap FIR, Hamming window).
 *  For lags 85-300: integer resolution (no interpolation).
 * ══════════════════════════════════════════════════════════════════ */

/* Compute one interpolated sample from exc_buf at position (idx - frac/3).
 * idx: integer index into exc_buf
 * frac: fractional part (0, 1, or 2 → 0/3, 1/3, 2/3 sample delay)
 * For frac=0, returns exc_buf[idx] directly. */
static float sinc_interp_sample(const float *exc_buf, int32_t idx, int32_t frac)
{
    if (frac == 0) return exc_buf[idx];

    /* Fractional delay: d = frac / FRAC_RES */
    float d = (float)frac / (float)TLCS_PITCH_FRAC_RES;

    /* 10-tap windowed sinc: taps at k = -4..5 */
    float sum = 0.0f;
    float norm = 0.0f;
    for (int32_t k = -TLCS_PITCH_SINC_HALF; k <= TLCS_PITCH_SINC_HALF + 1; k++) {
        float x = (float)k + d;
        /* Sinc */
        float s;
        if (fabsf(x) < 1e-6f)
            s = 1.0f;
        else
            s = sinf((float)M_PI * x) / ((float)M_PI * x);
        /* Hamming window: centered on d, half-length 5 */
        float w = 0.54f + 0.46f * cosf((float)M_PI * x / 5.0f);
        float h = s * w;
        sum += exc_buf[idx + k] * h;
        norm += h;
    }

    /* Normalize to preserve DC gain (windowed sinc taps sum ≈ 1.03) */
    if (fabsf(norm) > 1e-6f)
        sum /= norm;

    return sum;
}

void tlcs_pitch_get_adaptive_vec_frac(const float *exc_buf, int32_t exc_offset,
                                       int32_t lag_int, int32_t frac,
                                       float *vec, int32_t subfr_size)
{
    if (frac == 0) {
        /* No interpolation needed — use integer extraction */
        tlcs_pitch_get_adaptive_vec(exc_buf, exc_offset, lag_int, vec, subfr_size);
        return;
    }

    for (int32_t i = 0; i < subfr_size; i++) {
        int32_t src_idx = exc_offset + i - lag_int;
        while (src_idx < 0) src_idx += lag_int;
        vec[i] = sinc_interp_sample(exc_buf, src_idx, frac);
    }
}

/* ══════════════════════════════════════════════════════════════════
 *  Fractional Pitch Lag Encoding/Decoding
 *
 *  9-bit encoding scheme:
 *    Lags 20-84 (fractional): index = (lag-20)*3 + frac  [0..194]
 *    Lags 85-300 (integer):   index = 195 + (lag-85)     [195..410]
 *  Total: 411 values, fits in 9 bits (512 max).
 * ══════════════════════════════════════════════════════════════════ */

int32_t tlcs_pitch_encode_lag(int32_t lag_int, int32_t frac)
{
    if (lag_int <= TLCS_PITCH_FRAC_RANGE) {
        return (lag_int - TLCS_MIN_PITCH_LAG) * TLCS_PITCH_FRAC_RES + frac;
    } else {
        return (TLCS_PITCH_FRAC_RANGE - TLCS_MIN_PITCH_LAG + 1)
               * TLCS_PITCH_FRAC_RES
               + (lag_int - TLCS_PITCH_FRAC_RANGE - 1);
    }
}

void tlcs_pitch_decode_lag(int32_t index, int32_t *lag_int, int32_t *frac)
{
    int32_t frac_count = (TLCS_PITCH_FRAC_RANGE - TLCS_MIN_PITCH_LAG + 1)
                         * TLCS_PITCH_FRAC_RES;  /* 195 */
    if (index < frac_count) {
        *lag_int = TLCS_MIN_PITCH_LAG + index / TLCS_PITCH_FRAC_RES;
        *frac = index % TLCS_PITCH_FRAC_RES;
    } else {
        *lag_int = TLCS_PITCH_FRAC_RANGE + 1 + (index - frac_count);
        *frac = 0;
    }
}

/* ══════════════════════════════════════════════════════════════════
 *  Fractional Closed-Loop Pitch Search
 *
 *  Searches at 1/3 sample resolution for lags ≤ FRAC_RANGE,
 *  integer resolution otherwise. Uses weighted-domain criterion.
 * ══════════════════════════════════════════════════════════════════ */

void tlcs_pitch_cl_search_frac(const float *w_target, const float *h_w,
                                const float *exc_buf, int32_t exc_offset,
                                int32_t subfr_size,
                                int32_t center_lag, int32_t delta,
                                int32_t min_lag, int32_t max_lag,
                                int32_t *out_lag, int32_t *out_frac,
                                float *out_gain)
{
    int32_t lo = center_lag - delta;
    int32_t hi = center_lag + delta;
    if (lo < min_lag) lo = min_lag;
    if (hi > max_lag) hi = max_lag;

    float best_score = -1e30f;
    int32_t best_lag = center_lag;
    int32_t best_frac = 0;
    float best_corr = 0.0f;
    float best_energy = 1.0f;

    float vec[TLCS_MAX_SUBFR_SIZE];
    float w_vec[TLCS_MAX_SUBFR_SIZE];

    for (int32_t lag = lo; lag <= hi; lag++) {
        /* Determine fractional candidates for this integer lag */
        int32_t frac_lo = 0;
        int32_t frac_hi = 0;
        if (lag <= TLCS_PITCH_FRAC_RANGE && lag >= min_lag) {
            frac_hi = TLCS_PITCH_FRAC_RES - 1;  /* 0, 1, 2 */
        }

        for (int32_t frac = frac_lo; frac <= frac_hi; frac++) {
            /* Extract adaptive codebook vector with fractional lag */
            tlcs_pitch_get_adaptive_vec_frac(exc_buf, exc_offset,
                                              lag, frac, vec, subfr_size);

            /* Filter through h_w (zero-state convolution) */
            for (int32_t j = 0; j < subfr_size; j++) {
                w_vec[j] = 0.0f;
                for (int32_t k = 0; k <= j; k++)
                    w_vec[j] += vec[k] * h_w[j - k];
            }

            /* Correlation and energy in weighted domain */
            float corr = 0.0f, energy = 0.0f;
            for (int32_t i = 0; i < subfr_size; i++) {
                corr   += w_target[i] * w_vec[i];
                energy += w_vec[i] * w_vec[i];
            }
            if (energy < 1.0f) energy = 1.0f;
            float score = (corr * corr) / energy;

            if (score > best_score) {
                best_score = score;
                best_lag = lag;
                best_frac = frac;
                best_corr = corr;
                best_energy = energy;
            }
        }
    }

    *out_lag = best_lag;
    *out_frac = best_frac;

    /* Optimal gain = correlation / energy */
    float g = best_corr / best_energy;
    if (g < 0.0f) g = 0.0f;
    if (g > TLCS_PITCH_GAIN_MAX) g = TLCS_PITCH_GAIN_MAX;
    *out_gain = g;
}

/* ══════════════════════════════════════════════════════════════════
 *  2-Tap Symmetric ACB Gain VQ
 *
 *  16-entry codebook for (g0, g1) pair where:
 *    a(n) = g0 * exc[n-lag] + g1 * (exc[n-lag-1] + exc[n-lag+1])
 *
 *  Derived from SMPL high-rate codebook (smpl_cb_acbgains_hr_Q14).
 * ══════════════════════════════════════════════════════════════════ */

const float tlcs_acb_vq[TLCS_ACB_VQ_SIZE][TLCS_ACB_VQ_TAPS] = {
    { 0.00f,  0.00f},   /*  0: zero (unvoiced) */
    { 0.15f,  0.00f},   /*  1: very weak */
    { 0.30f,  0.00f},   /*  2: weak */
    { 0.40f,  0.08f},   /*  3: weak + shape */
    { 0.50f,  0.00f},   /*  4: moderate */
    { 0.55f,  0.10f},   /*  5: moderate + shape */
    { 0.65f,  0.00f},   /*  6: medium-low */
    { 0.70f,  0.08f},   /*  7: medium + shape */
    { 0.78f,  0.00f},   /*  8: medium-high */
    { 0.85f,  0.10f},   /*  9: strong + shape */
    { 0.90f,  0.00f},   /* 10: strong */
    { 0.95f,  0.08f},   /* 11: near-unity + shape */
    { 1.00f,  0.00f},   /* 12: unity */
    { 1.00f, -0.10f},   /* 13: unity - shape */
    { 1.15f,  0.05f},   /* 14: above unity */
    { 1.35f,  0.00f},   /* 15: high */
};

/* ── Extract 2 ACB basis vectors ──────────────────────────────── */

void tlcs_pitch_get_acb_basis(const float *exc_buf, int32_t exc_offset,
                               int32_t lag_int, int32_t frac,
                               float *v0, float *v1, int32_t subfr_size)
{
    /* v0: main vector at (lag_int, frac) */
    tlcs_pitch_get_adaptive_vec_frac(exc_buf, exc_offset,
                                      lag_int, frac, v0, subfr_size);

    /* v1: neighbor sum = exc[n-lag-1] + exc[n-lag+1] (integer neighbors) */
    float v_minus[TLCS_MAX_SUBFR_SIZE];
    float v_plus[TLCS_MAX_SUBFR_SIZE];
    tlcs_pitch_get_adaptive_vec(exc_buf, exc_offset, lag_int + 1,
                                 v_minus, subfr_size);
    if (lag_int > 1) {
        tlcs_pitch_get_adaptive_vec(exc_buf, exc_offset, lag_int - 1,
                                     v_plus, subfr_size);
    } else {
        memset(v_plus, 0, (size_t)subfr_size * sizeof(float));
    }

    for (int32_t i = 0; i < subfr_size; i++)
        v1[i] = v_minus[i] + v_plus[i];
}

/* ── Search 2-tap VQ codebook ─────────────────────────────────── */

int32_t tlcs_acb_vq_search(const float *w_target,
                            const float *w_v0, const float *w_v1,
                            int32_t subfr_size)
{
    /* Precompute inner products */
    float R00 = 0, R11 = 0, R01 = 0, t0 = 0, t1 = 0, tt = 0;
    for (int32_t i = 0; i < subfr_size; i++) {
        R00 += w_v0[i] * w_v0[i];
        R11 += w_v1[i] * w_v1[i];
        R01 += w_v0[i] * w_v1[i];
        t0  += w_target[i] * w_v0[i];
        t1  += w_target[i] * w_v1[i];
        tt  += w_target[i] * w_target[i];
    }

    float best_dist = tt;   /* distortion with zero gains */
    int32_t best_idx = 0;   /* entry 0 = {0,0} — default to no pitch */

    for (int32_t idx = 0; idx < TLCS_ACB_VQ_SIZE; idx++) {
        float g0 = tlcs_acb_vq[idx][0];
        float g1 = tlcs_acb_vq[idx][1];

        float dist = tt
                   - 2.0f * g0 * t0 - 2.0f * g1 * t1
                   + g0 * g0 * R00 + g1 * g1 * R11
                   + 2.0f * g0 * g1 * R01;

        if (dist < best_dist) {
            best_dist = dist;
            best_idx = idx;
        }
    }

    return best_idx;
}
