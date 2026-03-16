/*
 * tlcs_pitch.c — Pitch estimation and adaptive codebook.
 *
 * Open-loop pitch search via normalised autocorrelation,
 * closed-loop (analysis-by-synthesis) refinement with
 * 1/3-sample fractional interpolation (windowed sinc).
 */
#include "tlcs_config.h"
#include "tlcs_pitch.h"

#include <math.h>
#include <string.h>

/* ================================================================== */
/* Windowed-sinc table for 1/3 fractional pitch                        */
/* ================================================================== */

#define SINC_HALF_LEN  11
#define SINC_TAPS      (2 * SINC_HALF_LEN + 1)  /* 23 */
#define SINC_PHASES    3

static float sinc_table[SINC_PHASES][SINC_TAPS];
static int   sinc_init_done = 0;

static void sinc_table_init(void)
{
    if (sinc_init_done) return;
    const float PI = 3.14159265f;
    for (int phase = 0; phase < SINC_PHASES; phase++) {
        float frac = (float)phase / (float)SINC_PHASES;
        for (int k = -SINC_HALF_LEN; k <= SINC_HALF_LEN; k++) {
            float x = (float)k - frac;
            /* sinc(x) */
            float s;
            if (fabsf(x) < 1e-6f) {
                s = 1.0f;
            } else {
                s = sinf(PI * x) / (PI * x);
            }
            /* Hamming window */
            float w = 0.54f + 0.46f * cosf(PI * x / (float)(SINC_HALF_LEN + 1));
            sinc_table[phase][k + SINC_HALF_LEN] = s * w;
        }
    }
    sinc_init_done = 1;
}

/* ================================================================== */
/* Build ACB vector — integer lag, with pitch-period repetition        */
/* ================================================================== */

static void build_acb_int(const float *exc_buf, int exc_len,
                          int lag, int length, float *out)
{
    for (int i = 0; i < length; i++) {
        int idx = exc_len - lag + i;
        /* Wrap for pitch-period repetition when subframe > lag */
        while (idx < 0) idx += lag;
        if (idx >= exc_len) {
            /* Beyond buffer, wrap using most recent period */
            int wrap = (idx - exc_len) % lag;
            int src = exc_len - lag + wrap;
            out[i] = (src >= 0 && src < exc_len) ? exc_buf[src] : 0.0f;
        } else {
            out[i] = exc_buf[idx];
        }
    }
}

/* ================================================================== */
/* Build ACB vector — fractional lag (sinc interpolation)              */
/* ================================================================== */

static void build_acb_frac(const float *exc_buf, int exc_len,
                           float lag, int length, float *out)
{
    sinc_table_init();

    int int_lag = (int)floorf(lag);
    float frac = lag - (float)int_lag;
    int phase = (int)roundf(frac * SINC_PHASES) % SINC_PHASES;

    /* If phase == 0, no fractional component — use integer path */
    if (phase == 0) {
        build_acb_int(exc_buf, exc_len, int_lag, length, out);
        return;
    }

    const float *coef = sinc_table[phase];

    for (int i = 0; i < length; i++) {
        int pos = exc_len - int_lag + i;
        float val = 0.0f;
        for (int k = -SINC_HALF_LEN; k <= SINC_HALF_LEN; k++) {
            int idx = pos + k;
            if (idx >= 0 && idx < exc_len) {
                val += coef[k + SINC_HALF_LEN] * exc_buf[idx];
            }
        }
        out[i] = val;
    }
}

/* ================================================================== */
/* Truncated causal convolution                                        */
/* ================================================================== */

void tlcs_convolve(const float *x, const float *h, int len, float *out)
{
    for (int i = 0; i < len; i++) {
        float sum = 0.0f;
        for (int k = 0; k <= i; k++) {
            sum += x[k] * h[i - k];
        }
        out[i] = sum;
    }
}

/* ================================================================== */
/* Open-loop pitch search                                              */
/* ================================================================== */

int tlcs_pitch_open_loop(const float *residual, int len,
                         int min_lag, int max_lag)
{
    int best_lag = min_lag;
    float best_corr = -1e30f;

    for (int lag = min_lag; lag <= max_lag && lag < len; lag++) {
        int seg_len = len - lag;
        float xy = 0.0f, xx = 0.0f, yy = 0.0f;
        for (int i = 0; i < seg_len; i++) {
            float s = residual[i];
            float r = residual[i + lag];
            xy += s * r;
            xx += s * s;
            yy += r * r;
        }
        float denom = xx * yy;
        if (denom < 1e-10f) continue;
        float corr = xy / sqrtf(denom + 1e-10f);
        if (corr > best_corr) {
            best_corr = corr;
            best_lag = lag;
        }
    }
    return best_lag;
}

/* ================================================================== */
/* Closed-loop pitch search                                            */
/* ================================================================== */

void tlcs_pitch_closed_loop(const float *target, const float *h,
                            const float *exc_buf, int exc_len,
                            int subframe_size, int min_lag, int max_lag,
                            float *out_lag, float *out_gain)
{
    sinc_table_init();

    float v[TLCS_SUBFRAME_SIZE];
    float fv[TLCS_SUBFRAME_SIZE];

    /* ---- Integer search ---- */
    int best_lag = min_lag;
    float best_score = -1e30f;

    for (int lag = min_lag; lag <= max_lag && lag < exc_len; lag++) {
        build_acb_int(exc_buf, exc_len, lag, subframe_size, v);
        tlcs_convolve(v, h, subframe_size, fv);

        float corr = 0.0f, energy = 0.0f;
        for (int i = 0; i < subframe_size; i++) {
            corr += target[i] * fv[i];
            energy += fv[i] * fv[i];
        }
        energy += 1e-10f;
        float score = corr * corr / energy;
        if (score > best_score) {
            best_score = score;
            best_lag = lag;
        }
    }

    /* ---- Fractional refinement (1/3 sample) ---- */
    float fracs[] = {-2.0f/3.0f, -1.0f/3.0f, 0.0f, 1.0f/3.0f, 2.0f/3.0f};
    float best_frac_lag = (float)best_lag;
    float best_frac_score = -1e30f;

    for (int f = 0; f < 5; f++) {
        float flag = (float)best_lag + fracs[f];
        if (flag < (float)min_lag || flag > (float)max_lag) continue;

        build_acb_frac(exc_buf, exc_len, flag, subframe_size, v);
        tlcs_convolve(v, h, subframe_size, fv);

        float corr = 0.0f, energy = 0.0f;
        for (int i = 0; i < subframe_size; i++) {
            corr += target[i] * fv[i];
            energy += fv[i] * fv[i];
        }
        energy += 1e-10f;
        float score = corr * corr / energy;
        if (score > best_frac_score) {
            best_frac_score = score;
            best_frac_lag = flag;
        }
    }

    /* Compute final gain */
    build_acb_frac(exc_buf, exc_len, best_frac_lag, subframe_size, v);
    tlcs_convolve(v, h, subframe_size, fv);

    float corr = 0.0f, energy = 0.0f;
    for (int i = 0; i < subframe_size; i++) {
        corr += target[i] * fv[i];
        energy += fv[i] * fv[i];
    }
    float gain = corr / (energy + 1e-10f);
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 1.2f) gain = 1.2f;

    *out_lag = best_frac_lag;
    *out_gain = gain;
}

/* ================================================================== */
/* Public: build ACB excitation                                        */
/* ================================================================== */

void tlcs_pitch_build_acb(const float *exc_buf, int exc_len,
                          float lag, int subframe_size, float *out)
{
    sinc_table_init();
    build_acb_frac(exc_buf, exc_len, lag, subframe_size, out);
}

/* ================================================================== */
/* Public: build 2-basis ACB (MLOW-style)                              */
/* basis0 = pitch-delayed signal                                       */
/* basis1 = exc[i-lag-1] + exc[i-lag+1]  (symmetric neighbor sum)      */
/* ================================================================== */

void tlcs_pitch_build_acb_2basis(const float *exc_buf, int exc_len,
                                  float lag, int subframe_size,
                                  float *basis0, float *basis1)
{
    sinc_table_init();

    /* basis0: standard pitch prediction */
    build_acb_frac(exc_buf, exc_len, lag, subframe_size, basis0);

    /* basis1: sum of adjacent pitch-delayed samples (lag-1 and lag+1) */
    float lag_m1 = lag + 1.0f;  /* lag+1 in excitation = one sample earlier */
    float lag_p1 = lag - 1.0f;  /* lag-1 in excitation = one sample later */

    float tmp_m1[TLCS_SUBFRAME_SIZE];
    float tmp_p1[TLCS_SUBFRAME_SIZE];

    if (lag_p1 < 1.0f) {
        /* Edge case: lag too small for lag-1, just use lag+1 doubled */
        build_acb_frac(exc_buf, exc_len, lag_m1, subframe_size, tmp_m1);
        for (int i = 0; i < subframe_size; i++) {
            basis1[i] = 2.0f * tmp_m1[i];
        }
    } else {
        build_acb_frac(exc_buf, exc_len, lag_m1, subframe_size, tmp_m1);
        build_acb_frac(exc_buf, exc_len, lag_p1, subframe_size, tmp_p1);
        for (int i = 0; i < subframe_size; i++) {
            basis1[i] = tmp_m1[i] + tmp_p1[i];
        }
    }
}
