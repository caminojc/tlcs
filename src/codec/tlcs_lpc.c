#include "tlcs_lpc.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Runtime-tunable pre-emphasis coefficient (default 0.60, SCOREQ-optimized) */
float tlcs_preemph_coeff_ = 0.60f;

void tlcs_preemph_init(void)
{
    const char *v = getenv("TLCS_PREEMPH");
    if (v) tlcs_preemph_coeff_ = (float)atof(v);
}

/* ══════════════════════════════════════════════════════════════════
 *  Pre-emphasis / De-emphasis
 * ══════════════════════════════════════════════════════════════════ */

void tlcs_preemph(const int16_t *in, float *out, int32_t n, float *mem)
{
    float prev = *mem;
    for (int32_t i = 0; i < n; i++) {
        float s = (float)in[i];
        out[i] = s - TLCS_PREEMPH_COEFF * prev;
        prev = s;
    }
    *mem = (float)in[n - 1];
}

void tlcs_deemph(float *buf, int32_t n, float *mem)
{
    float prev = *mem;
    for (int32_t i = 0; i < n; i++) {
        buf[i] = buf[i] + TLCS_PREEMPH_COEFF * prev;
        prev = buf[i];
    }
    *mem = prev;
}

/* ══════════════════════════════════════════════════════════════════
 *  Hamming Window
 * ══════════════════════════════════════════════════════════════════ */

void tlcs_hamming_window(const float *in, float *out, int32_t n)
{
    float inv_nm1 = 1.0f / (float)(n - 1);
    for (int32_t i = 0; i < n; i++) {
        float w = 0.54f - 0.46f * cosf(2.0f * (float)M_PI * (float)i * inv_nm1);
        out[i] = in[i] * w;
    }
}

/* ══════════════════════════════════════════════════════════════════
 *  Autocorrelation
 * ══════════════════════════════════════════════════════════════════ */

void tlcs_autocorrelation(const float *windowed, int32_t n,
                          float *r, int32_t order)
{
    for (int32_t k = 0; k <= order; k++) {
        float sum = 0.0f;
        for (int32_t i = k; i < n; i++) {
            sum += windowed[i] * windowed[i - k];
        }
        r[k] = sum;
    }

    /* Bandwidth expansion (lag windowing) for robustness.
     * Prevents ill-conditioned autocorrelation matrices. */
    if (r[0] > 0.0f) {
        for (int32_t k = 1; k <= order; k++) {
            float bw = 60.0f * 2.0f * (float)M_PI / 16000.0f;
            float decay = expf(-0.5f * (float)(k * k) * bw * bw);
            r[k] *= decay;
        }
        /* White noise correction: boost r[0] to regularize.
         * Prevents LPC from tracking narrow spectral peaks (harmonics). */
        r[0] *= 1.0001f;
    }
}

/* ══════════════════════════════════════════════════════════════════
 *  Levinson-Durbin Recursion
 * ══════════════════════════════════════════════════════════════════ */

float tlcs_levinson(const float *r, int32_t order, float *a, float *k_coeff)
{
    float err = r[0];
    if (err < 1.0f) err = 1.0f;  /* silence guard */

    a[0] = 1.0f;
    for (int32_t i = 1; i <= order; i++) a[i] = 0.0f;

    /* Support up to order 40 (for perceptual filter at order 31+) */
    float a_prev[42];

    for (int32_t i = 1; i <= order; i++) {
        /* Compute reflection coefficient */
        float sum = 0.0f;
        for (int32_t j = 1; j < i; j++) {
            sum += a[j] * r[i - j];
        }
        float ki = -(r[i] + sum) / err;

        /* Clamp for stability */
        if (ki > 0.9999f)  ki = 0.9999f;
        if (ki < -0.9999f) ki = -0.9999f;

        if (k_coeff) k_coeff[i - 1] = ki;

        /* Update coefficients */
        memcpy(a_prev, a, (size_t)(i + 1) * sizeof(float));
        for (int32_t j = 1; j < i; j++) {
            a[j] = a_prev[j] + ki * a_prev[i - j];
        }
        a[i] = ki;

        /* Update prediction error */
        err *= (1.0f - ki * ki);
        if (err < 1.0f) err = 1.0f;
    }

    return r[0] / err;
}

/* ══════════════════════════════════════════════════════════════════
 *  Modified Burg LPC Analysis
 *
 *  Computes LPC coefficients by minimizing both forward and backward
 *  prediction error. More accurate than autocorrelation+Levinson for
 *  short data segments and narrow formant bandwidths (e.g. female voices).
 * ══════════════════════════════════════════════════════════════════ */

float tlcs_burg(const float *x, int32_t n, int32_t order, float *a)
{
    /* Forward and backward prediction errors */
    float f[TLCS_MAX_PITCH_LAG + TLCS_MAX_FRAME_SIZE];
    float b[TLCS_MAX_PITCH_LAG + TLCS_MAX_FRAME_SIZE];

    /* Initialize */
    float pwr = 0.0f;
    for (int32_t i = 0; i < n; i++) {
        f[i] = x[i];
        b[i] = x[i];
        pwr += x[i] * x[i];
    }
    if (pwr < 1.0f) pwr = 1.0f;

    a[0] = 1.0f;
    for (int32_t i = 1; i <= order; i++) a[i] = 0.0f;

    float err = pwr;
    float a_prev[TLCS_LPC_ORDER_MAX + 1];

    for (int32_t m = 0; m < order; m++) {
        /* Compute reflection coefficient */
        float num = 0.0f;
        float den = 0.0f;
        for (int32_t i = m + 1; i < n; i++) {
            num += f[i] * b[i - 1];
            den += f[i] * f[i] + b[i - 1] * b[i - 1];
        }
        if (den < 1e-10f) break;
        float k = -2.0f * num / den;

        /* Clamp for stability */
        if (k > 0.998f)  k = 0.998f;
        if (k < -0.998f) k = -0.998f;

        /* Update LPC coefficients */
        memcpy(a_prev, a, (size_t)(m + 2) * sizeof(float));
        for (int32_t j = 1; j <= m; j++)
            a[j] = a_prev[j] + k * a_prev[m + 1 - j];
        a[m + 1] = k;

        /* Update prediction errors */
        for (int32_t i = n - 1; i >= m + 1; i--) {
            float f_new = f[i] + k * b[i - 1];
            float b_new = b[i - 1] + k * f[i];
            f[i] = f_new;
            b[i] = b_new;
        }

        /* Update error estimate */
        err *= (1.0f - k * k);
        if (err < 1.0f) err = 1.0f;
    }

    return pwr / err;
}

/* ══════════════════════════════════════════════════════════════════
 *  LPC → LSF Conversion
 *
 *  Method: Form symmetric (P) and antisymmetric (Q) polynomials,
 *  factor out known roots, evaluate via Chebyshev-like formula,
 *  find zeros by grid search + bisection.
 * ══════════════════════════════════════════════════════════════════ */

/*
 * Evaluate the "deconvolved" symmetric polynomial on the unit circle.
 *
 * Given symmetric polynomial coefficients pp[0..2m] where pp[k] = pp[2m-k],
 * evaluate at z = e^{jω} using:
 *   f(ω) = pp[m] + 2 * Σ_{k=1}^{m} pp[m-k] * cos(kω)
 *
 * This is the real envelope (modulo a phase factor e^{-jmω}).
 */
static float eval_sym_poly(const float *pp, int32_t m, float omega)
{
    float val = pp[m];
    for (int32_t k = 1; k <= m; k++) {
        val += 2.0f * pp[m - k] * cosf((float)k * omega);
    }
    return val;
}

/*
 * Find roots of a symmetric polynomial in (0, π) via grid search + bisection.
 * Returns number of roots found. Roots written to roots[].
 */
static int32_t find_roots(const float *pp, int32_t m,
                          float *roots, int32_t max_roots)
{
    int32_t nr = 0;
    float step = (float)M_PI / (float)TLCS_LSF_GRID;
    float prev_val = eval_sym_poly(pp, m, 1e-4f);

    for (int32_t i = 1; i <= TLCS_LSF_GRID && nr < max_roots; i++) {
        float omega = (float)i * step;
        if (omega > (float)M_PI - 1e-4f) omega = (float)M_PI - 1e-4f;
        float val = eval_sym_poly(pp, m, omega);

        if (prev_val * val <= 0.0f) {
            /* Sign change — bisect */
            float lo = (float)(i - 1) * step;
            if (lo < 1e-4f) lo = 1e-4f;
            float hi = omega;
            float lo_val = prev_val;

            for (int32_t iter = 0; iter < 20; iter++) {
                float mid = 0.5f * (lo + hi);
                float mid_val = eval_sym_poly(pp, m, mid);
                if (lo_val * mid_val <= 0.0f) {
                    hi = mid;
                } else {
                    lo = mid;
                    lo_val = mid_val;
                }
            }
            roots[nr++] = 0.5f * (lo + hi);
        }
        prev_val = val;
    }
    return nr;
}

int tlcs_lpc_to_lsf(const float *a, int32_t order, float *lsf)
{
    if (order < 2 || order > TLCS_LPC_ORDER_MAX || (order % 2 != 0))
        return -1;

    int32_t m = order / 2;

    /* Step 1: Form P and Q polynomials (degree order+1).
     *   P[k] coefficient of z^{-k} in A(z) + z^{-(p+1)} A(z^{-1})
     *   Q[k] coefficient of z^{-k} in A(z) - z^{-(p+1)} A(z^{-1})
     *
     *   P[0] = 1, P[order+1] = 1
     *   Q[0] = 1, Q[order+1] = -1
     *   P[k] = a[k] + a[order+1-k]  for k = 1..order
     *   Q[k] = a[k] - a[order+1-k]  for k = 1..order
     *
     *   BUT a[order+1] doesn't exist in A(z). We handle the boundary:
     *   For k=1: a[order+1-1] = a[order] ✓
     *   For k=order: a[order+1-order] = a[1] ✓
     *   All terms reference a[1..order], which exist.
     */
    float p_full[TLCS_LPC_ORDER_MAX + 2];
    float q_full[TLCS_LPC_ORDER_MAX + 2];

    p_full[0] = 1.0f;
    q_full[0] = 1.0f;
    for (int32_t k = 1; k <= order; k++) {
        p_full[k] = a[k] + a[order + 1 - k];
        q_full[k] = a[k] - a[order + 1 - k];
    }
    p_full[order + 1] =  1.0f;
    q_full[order + 1] = -1.0f;

    /* Step 2: Factor out known roots.
     *   P(z) is palindromic of odd degree → root at z = -1
     *   Divide P by (1 + z^{-1}): P'[k] = P[k] - P'[k-1]
     *
     *   Q(z) is antipalindromic of odd degree → root at z = +1
     *   Divide Q by (1 - z^{-1}): Q'[k] = Q[k] + Q'[k-1]
     *
     *   Result: P' and Q' are symmetric of degree 2m = order.
     */
    float pp[TLCS_LPC_ORDER_MAX + 1];
    float qp[TLCS_LPC_ORDER_MAX + 1];

    pp[0] = p_full[0];
    for (int32_t k = 1; k <= order; k++) {
        pp[k] = p_full[k] - pp[k - 1];
    }

    qp[0] = q_full[0];
    for (int32_t k = 1; k <= order; k++) {
        qp[k] = q_full[k] + qp[k - 1];
    }

    /* Step 3: Find m roots of P' and m roots of Q' in (0, π). */
    float p_roots[TLCS_LPC_ORDER_MAX / 2];
    float q_roots[TLCS_LPC_ORDER_MAX / 2];

    int32_t np = find_roots(pp, m, p_roots, m);
    int32_t nq = find_roots(qp, m, q_roots, m);

    if (np != m || nq != m) return -1;

    /* Step 4: Merge roots in ascending order.
     * P' and Q' roots interleave by the stability property.
     * We tag each root with its source for LSF→LPC reconstruction. */
    int32_t pi = 0, qi = 0;
    for (int32_t i = 0; i < order; i++) {
        if (qi >= m || (pi < m && p_roots[pi] <= q_roots[qi])) {
            lsf[i] = p_roots[pi++];
        } else {
            lsf[i] = q_roots[qi++];
        }
    }

    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 *  LSF → LPC Conversion
 *
 *  Reconstruct A(z) from LSFs by building the two factor polynomials
 *  and combining: A(z) = [(1+z^{-1})F1(z) + (1-z^{-1})F2(z)] / 2
 * ══════════════════════════════════════════════════════════════════ */

/*
 * Build polynomial from second-order root factors.
 * Each root ω gives factor (1 - 2cos(ω)z^{-1} + z^{-2}).
 * Result: poly[0..2*n_roots], with poly[0] = 1.
 */
static void build_poly_from_roots(const float *roots, int32_t n_roots,
                                  float *poly)
{
    memset(poly, 0, (size_t)(2 * n_roots + 1) * sizeof(float));
    poly[0] = 1.0f;

    for (int32_t i = 0; i < n_roots; i++) {
        float c = -2.0f * cosf(roots[i]);

        /* Convolve poly[0..2i] with [1, c, 1] → poly[0..2(i+1)]
         * Work backwards to avoid overwriting. */
        int32_t cur_deg = 2 * i;
        for (int32_t j = cur_deg + 2; j >= 0; j--) {
            float v = 0.0f;
            if (j <= cur_deg)              v += poly[j];       /* ×1 */
            if (j >= 1 && j - 1 <= cur_deg) v += c * poly[j-1]; /* ×c */
            if (j >= 2 && j - 2 <= cur_deg) v += poly[j-2];     /* ×1 */
            poly[j] = v;
        }
    }
}

void tlcs_lsf_to_lpc(const float *lsf, int32_t order, float *a)
{
    int32_t m = order / 2;

    /* Split alternating LSFs into two groups */
    float r1[TLCS_LPC_ORDER_MAX / 2];
    float r2[TLCS_LPC_ORDER_MAX / 2];
    for (int32_t i = 0; i < m; i++) {
        r1[i] = lsf[2 * i];
        r2[i] = lsf[2 * i + 1];
    }

    /* Build factor polynomials F1 and F2 (degree 2m = order) */
    float f1[TLCS_LPC_ORDER_MAX + 1];
    float f2[TLCS_LPC_ORDER_MAX + 1];
    build_poly_from_roots(r1, m, f1);
    build_poly_from_roots(r2, m, f2);

    /* P(z) = (1 + z^{-1}) F1(z) → degree order+1
     * Q(z) = (1 - z^{-1}) F2(z) → degree order+1
     * A(z) = (P(z) + Q(z)) / 2 → degree order */
    float p_poly[TLCS_LPC_ORDER_MAX + 2];
    float q_poly[TLCS_LPC_ORDER_MAX + 2];

    for (int32_t k = 0; k <= order + 1; k++) {
        float f1k = (k <= order) ? f1[k] : 0.0f;
        float f1k1 = (k >= 1 && k - 1 <= order) ? f1[k - 1] : 0.0f;
        float f2k = (k <= order) ? f2[k] : 0.0f;
        float f2k1 = (k >= 1 && k - 1 <= order) ? f2[k - 1] : 0.0f;

        p_poly[k] = f1k + f1k1;   /* (1 + z^{-1}) F1 */
        q_poly[k] = f2k - f2k1;   /* (1 - z^{-1}) F2 */
    }

    /* A = (P + Q) / 2 */
    for (int32_t k = 0; k <= order; k++) {
        a[k] = 0.5f * (p_poly[k] + q_poly[k]);
    }

    /* Verify a[0] ≈ 1. If not, try swapping F1↔F2. */
    if (fabsf(a[0] - 1.0f) > 0.01f) {
        /* Swap: P = (1+z^{-1})F2, Q = (1-z^{-1})F1 */
        for (int32_t k = 0; k <= order + 1; k++) {
            float f1k = (k <= order) ? f1[k] : 0.0f;
            float f1k1 = (k >= 1 && k - 1 <= order) ? f1[k - 1] : 0.0f;
            float f2k = (k <= order) ? f2[k] : 0.0f;
            float f2k1 = (k >= 1 && k - 1 <= order) ? f2[k - 1] : 0.0f;

            p_poly[k] = f2k + f2k1;
            q_poly[k] = f1k - f1k1;
        }
        for (int32_t k = 0; k <= order; k++) {
            a[k] = 0.5f * (p_poly[k] + q_poly[k]);
        }
    }
}

/* ══════════════════════════════════════════════════════════════════
 *  LSF Stability
 * ══════════════════════════════════════════════════════════════════ */

void tlcs_lsf_stabilize(float *lsf, int32_t order)
{
    /* Clamp to valid range */
    for (int32_t i = 0; i < order; i++) {
        if (lsf[i] < TLCS_LSF_MIN_GAP)
            lsf[i] = TLCS_LSF_MIN_GAP;
        if (lsf[i] > (float)M_PI - TLCS_LSF_MIN_GAP)
            lsf[i] = (float)M_PI - TLCS_LSF_MIN_GAP;
    }

    /* Enforce minimum spacing (bubble-sort style, iterate until stable) */
    for (int32_t pass = 0; pass < 3; pass++) {
        int changed = 0;
        for (int32_t i = 0; i < order - 1; i++) {
            float gap = lsf[i + 1] - lsf[i];
            if (gap < TLCS_LSF_MIN_GAP) {
                float mid = 0.5f * (lsf[i] + lsf[i + 1]);
                lsf[i]     = mid - 0.5f * TLCS_LSF_MIN_GAP;
                lsf[i + 1] = mid + 0.5f * TLCS_LSF_MIN_GAP;
                changed = 1;
            }
        }
        if (!changed) break;
    }

    /* Final boundary clamp */
    if (lsf[0] < TLCS_LSF_MIN_GAP) lsf[0] = TLCS_LSF_MIN_GAP;
    if (lsf[order - 1] > (float)M_PI - TLCS_LSF_MIN_GAP)
        lsf[order - 1] = (float)M_PI - TLCS_LSF_MIN_GAP;
}

/* ══════════════════════════════════════════════════════════════════
 *  LSF Interpolation
 * ══════════════════════════════════════════════════════════════════ */

void tlcs_lsf_interpolate(const float *prev, const float *cur,
                          float *out, int32_t order, float alpha)
{
    for (int32_t i = 0; i < order; i++) {
        out[i] = (1.0f - alpha) * prev[i] + alpha * cur[i];
    }
}

/* ══════════════════════════════════════════════════════════════════
 *  Predictive LSF Quantization
 *
 *  Each LSF is differentially quantized: encode the error between
 *  the current LSF and a prediction (previous frame's quantized LSF).
 *  7 bits per coefficient, range ±LSF_DIFF_RANGE.
 *  Step = 2*range/128 ≈ 0.0125 rad (vs absolute's 0.0245 rad).
 * ══════════════════════════════════════════════════════════════════ */

#define LSF_QUANT_BITS  TLCS_LSF_BITS
#define LSF_QUANT_LEVELS (1 << LSF_QUANT_BITS)
#define LSF_DIFF_RANGE  1.0f   /* ±1.0 rad max prediction error */

void tlcs_lsf_quantize(const float *lsf, int32_t order, int16_t *indices)
{
    float step = (float)M_PI / (float)LSF_QUANT_LEVELS;
    for (int32_t i = 0; i < order; i++) {
        int32_t idx = (int32_t)(lsf[i] / step + 0.5f);
        if (idx < 1) idx = 1;
        if (idx >= LSF_QUANT_LEVELS - 1) idx = LSF_QUANT_LEVELS - 2;
        indices[i] = (int16_t)idx;
    }
}

void tlcs_lsf_dequantize(const int16_t *indices, int32_t order, float *lsf)
{
    float step = (float)M_PI / (float)LSF_QUANT_LEVELS;
    for (int32_t i = 0; i < order; i++) {
        lsf[i] = (float)indices[i] * step;
    }
}

void tlcs_lsf_quantize_pred(const float *lsf, const float *pred,
                              int32_t order, int16_t *indices)
{
    float step = 2.0f * LSF_DIFF_RANGE / (float)LSF_QUANT_LEVELS;
    for (int32_t i = 0; i < order; i++) {
        float err = lsf[i] - pred[i];
        /* Clip to quantization range */
        if (err < -LSF_DIFF_RANGE) err = -LSF_DIFF_RANGE;
        if (err >  LSF_DIFF_RANGE) err =  LSF_DIFF_RANGE;
        /* Map [-range, +range] → [0, 127] */
        int32_t idx = (int32_t)((err + LSF_DIFF_RANGE) / step + 0.5f);
        if (idx < 0) idx = 0;
        if (idx >= LSF_QUANT_LEVELS) idx = LSF_QUANT_LEVELS - 1;
        indices[i] = (int16_t)idx;
    }
}

void tlcs_lsf_dequantize_pred(const int16_t *indices, const float *pred,
                                int32_t order, float *lsf)
{
    float step = 2.0f * LSF_DIFF_RANGE / (float)LSF_QUANT_LEVELS;
    for (int32_t i = 0; i < order; i++) {
        float err = (float)indices[i] * step - LSF_DIFF_RANGE;
        lsf[i] = pred[i] + err;
    }
}

/* ══════════════════════════════════════════════════════════════════
 *  Parameterized Predictive LSF Quantization
 *
 *  Same algorithm as above but with configurable bit depth and range.
 *  Used for low-rate mode (5 bits, ±0.8 rad).
 * ══════════════════════════════════════════════════════════════════ */

void tlcs_lsf_quantize_pred_n(const float *lsf, const float *pred,
                                int32_t order, int16_t *indices,
                                int32_t bits, float range)
{
    int32_t levels = 1 << bits;
    float step = 2.0f * range / (float)levels;
    for (int32_t i = 0; i < order; i++) {
        float err = lsf[i] - pred[i];
        if (err < -range) err = -range;
        if (err >  range) err =  range;
        int32_t idx = (int32_t)((err + range) / step + 0.5f);
        if (idx < 0) idx = 0;
        if (idx >= levels) idx = levels - 1;
        indices[i] = (int16_t)idx;
    }
}

void tlcs_lsf_dequantize_pred_n(const int16_t *indices, const float *pred,
                                  int32_t order, float *lsf,
                                  int32_t bits, float range)
{
    int32_t levels = 1 << bits;
    float step = 2.0f * range / (float)levels;
    for (int32_t i = 0; i < order; i++) {
        float err = (float)indices[i] * step - range;
        lsf[i] = pred[i] + err;
    }
}

/* ══════════════════════════════════════════════════════════════════
 *  Analysis Filter A(z): computes LPC residual
 *
 *  residual[n] = in[n] + a[1]*in[n-1] + ... + a[order]*in[n-order]
 *
 *  mem[0..order-1]: past input samples (mem[0] = in[-1], etc.)
 *  Updated on return to reflect last 'order' input samples.
 * ══════════════════════════════════════════════════════════════════ */

void tlcs_analysis_filter(const float *a, int32_t order,
                          const float *in, float *residual,
                          int32_t n, float *mem)
{
    for (int32_t i = 0; i < n; i++) {
        float sum = in[i];
        for (int32_t k = 1; k <= order; k++) {
            float past;
            if (i - k >= 0) {
                past = in[i - k];
            } else {
                past = mem[k - 1 - i];  /* mem[0]=in[-1], mem[1]=in[-2], ... */
            }
            sum += a[k] * past;
        }
        residual[i] = sum;
    }

    /* Update memory: last 'order' samples of in[] */
    if (n >= order) {
        for (int32_t k = 0; k < order; k++) {
            mem[k] = in[n - 1 - k];
        }
    } else {
        /* Shift existing memory and prepend new samples */
        float tmp[TLCS_LPC_ORDER_MAX];
        memcpy(tmp, mem, (size_t)order * sizeof(float));
        for (int32_t k = 0; k < order; k++) {
            if (k < n) {
                mem[k] = in[n - 1 - k];
            } else {
                mem[k] = tmp[k - n];
            }
        }
    }
}

/* ══════════════════════════════════════════════════════════════════
 *  Synthesis Filter 1/A(z): reconstructs speech from excitation
 *
 *  out[n] = exc[n] - a[1]*out[n-1] - ... - a[order]*out[n-order]
 *
 *  mem[0..order-1]: past output samples (mem[0] = out[-1], etc.)
 *  Updated on return.
 * ══════════════════════════════════════════════════════════════════ */

void tlcs_synthesis_filter(const float *a, int32_t order,
                           const float *exc, float *out,
                           int32_t n, float *mem)
{
    for (int32_t i = 0; i < n; i++) {
        float sum = exc[i];
        for (int32_t k = 1; k <= order; k++) {
            float past;
            if (i - k >= 0) {
                past = out[i - k];
            } else {
                past = mem[k - 1 - i];  /* mem[0]=out[-1], mem[1]=out[-2], ... */
            }
            sum -= a[k] * past;
        }
        out[i] = sum;
    }

    /* Update memory */
    if (n >= order) {
        for (int32_t k = 0; k < order; k++) {
            mem[k] = out[n - 1 - k];
        }
    } else {
        float tmp[TLCS_LPC_ORDER_MAX];
        memcpy(tmp, mem, (size_t)order * sizeof(float));
        for (int32_t k = 0; k < order; k++) {
            if (k < n) {
                mem[k] = out[n - 1 - k];
            } else {
                mem[k] = tmp[k - n];
            }
        }
    }
}

/* ══════════════════════════════════════════════════════════════════
 *  Bandwidth Expansion: A(z) → A(z/γ)
 *
 *  a_gamma[k] = a[k] * γ^k
 *  Used for perceptual weighting filter W(z) = A(z/γ₁)/A(z/γ₂).
 * ══════════════════════════════════════════════════════════════════ */

void tlcs_lpc_weight_coeffs(const float *a, int32_t order,
                             float gamma, float *a_gamma)
{
    float g = 1.0f;
    a_gamma[0] = a[0];  /* 1.0 */
    for (int32_t k = 1; k <= order; k++) {
        g *= gamma;
        a_gamma[k] = a[k] * g;
    }
}
