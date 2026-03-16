#ifndef TLCS_LPC_H
#define TLCS_LPC_H

#include "tlcs/tlcs_types.h"

/* Pre-emphasis coefficient (runtime-tunable via TLCS_PREEMPH env var) */
extern float tlcs_preemph_coeff_;
#define TLCS_PREEMPH_COEFF  tlcs_preemph_coeff_

void tlcs_preemph_init(void);

/* LSF quantization bits per coefficient */
#define TLCS_LSF_BITS       7

/* LSF minimum spacing in radians (~50 Hz at 16 kHz) */
#define TLCS_LSF_MIN_GAP    0.02f

/* Grid resolution for LSF root finding */
#define TLCS_LSF_GRID       512

/* ── Pre-emphasis / De-emphasis ──────────────────────────────── */

void tlcs_preemph(const int16_t *in, float *out, int32_t n, float *mem);
void tlcs_deemph(float *buf, int32_t n, float *mem);

/* ── Windowing ───────────────────────────────────────────────── */

void tlcs_hamming_window(const float *in, float *out, int32_t n);

/* ── Autocorrelation ─────────────────────────────────────────── */

void tlcs_autocorrelation(const float *windowed, int32_t n,
                          float *r, int32_t order);

/* ── Levinson-Durbin ─────────────────────────────────────────── */

/* Returns prediction gain = r[0] / error.
 * a[0..order] output (a[0]=1.0).
 * k_coeff may be NULL if reflection coefficients not needed. */
float tlcs_levinson(const float *r, int32_t order, float *a, float *k_coeff);

/* ── LPC ↔ LSF conversion ───────────────────────────────────── */

/* a[0..order] → lsf[0..order-1] in radians (0, pi).
 * Returns 0 on success, -1 if roots not found. */
int tlcs_lpc_to_lsf(const float *a, int32_t order, float *lsf);

/* lsf[0..order-1] → a[0..order] (a[0] set to 1.0). */
void tlcs_lsf_to_lpc(const float *lsf, int32_t order, float *a);

/* Enforce minimum LSF spacing and boundary constraints. */
void tlcs_lsf_stabilize(float *lsf, int32_t order);

/* Interpolate LSFs: out = (1-alpha)*prev + alpha*cur */
void tlcs_lsf_interpolate(const float *prev, const float *cur,
                          float *out, int32_t order, float alpha);

/* ── Scalar LSF quantization (placeholder for M1) ───────────── */

void tlcs_lsf_quantize(const float *lsf, int32_t order, int16_t *indices);
void tlcs_lsf_dequantize(const int16_t *indices, int32_t order, float *lsf);

/* Predictive LSF quantization: encode error from prediction (prev frame) */
void tlcs_lsf_quantize_pred(const float *lsf, const float *pred,
                              int32_t order, int16_t *indices);
void tlcs_lsf_dequantize_pred(const int16_t *indices, const float *pred,
                                int32_t order, float *lsf);

/* Parameterized predictive LSF quantization (variable bits/range) */
void tlcs_lsf_quantize_pred_n(const float *lsf, const float *pred,
                                int32_t order, int16_t *indices,
                                int32_t bits, float range);
void tlcs_lsf_dequantize_pred_n(const int16_t *indices, const float *pred,
                                  int32_t order, float *lsf,
                                  int32_t bits, float range);

/* ── Burg LPC analysis ──────────────────────────────────────── */

/* Modified Burg method: computes LPC coefficients directly from signal.
 * Minimizes both forward and backward prediction error.
 * a[0..order] output (a[0]=1.0). Returns prediction gain.
 * No windowing needed — operates on raw signal. */
float tlcs_burg(const float *x, int32_t n, int32_t order, float *a);

/* ── Bandwidth expansion ─────────────────────────────────────── */

/* Compute a_gamma[k] = a[k] * gamma^k for perceptual weighting.
 * Used to form A(z/gamma) from A(z). */
void tlcs_lpc_weight_coeffs(const float *a, int32_t order,
                             float gamma, float *a_gamma);

/* ── Filters ─────────────────────────────────────────────────── */

/* Analysis filter A(z): residual[n] = in[n] + sum(a[k]*in[n-k])
 * mem[0..order-1] is past input samples (read-only, caller manages). */
void tlcs_analysis_filter(const float *a, int32_t order,
                          const float *in, float *residual,
                          int32_t n, float *mem);

/* Synthesis filter 1/A(z): out[n] = exc[n] - sum(a[k]*out[n-k])
 * mem[0..order-1] is past output samples (updated in-place). */
void tlcs_synthesis_filter(const float *a, int32_t order,
                           const float *exc, float *out,
                           int32_t n, float *mem);

#endif /* TLCS_LPC_H */
