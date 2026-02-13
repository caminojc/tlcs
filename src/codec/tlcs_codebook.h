#ifndef TLCS_CODEBOOK_H
#define TLCS_CODEBOOK_H

#include "tlcs/tlcs_types.h"

/* ── Algebraic CELP (ACELP) codebook ───────────────────────────── */

#define TLCS_CB_MAX_PULSES      20
#define TLCS_CB_NUM_PULSES      10   /* active pulses per subframe */

/* Fixed codebook gain: 7 bits unsigned (sign absorbed into pulse signs) */
#define TLCS_FCB_GAIN_BITS       7
#define TLCS_FCB_GAIN_LEVELS    (1 << TLCS_FCB_GAIN_BITS)  /* 128 */

/* Perceptual weighting filter parameters */
#define TLCS_GAMMA1              0.96f
#define TLCS_GAMMA2              0.50f

/* ── Codebook configuration ────────────────────────────────────── */

typedef struct {
    int32_t num_pulses;           /* number of ACELP pulses per subframe */
    int32_t positions_per_track;  /* subfr_size / num_pulses */
    int32_t pos_bits;             /* bits per pulse position */
    int32_t subfr_size;           /* subframe length in samples */
} tlcs_cb_config;

/* ── Codebook search result ────────────────────────────────────── */

typedef struct {
    int32_t pulse_pos[TLCS_CB_MAX_PULSES];   /* track-relative indices */
    int32_t pulse_sign[TLCS_CB_MAX_PULSES];  /* +1 or -1 */
    float   gain;                             /* quantized gain */
    int32_t gain_index;                       /* 5-bit packed index */
} tlcs_cb_entry;

/* ── Functions ─────────────────────────────────────────────────── */

/* Initialize codebook config for given subframe size and pulse count.
 * Tracks are interleaved: track k has positions {k, k+N, k+2N, ...}
 * where N = num_pulses. */
void tlcs_cb_config_init(tlcs_cb_config *cfg,
                          int32_t subfr_size, int32_t num_pulses);

/* Compute impulse response of weighted synthesis filter:
 *   H_w(z) = A(z/gamma1) / (A(z) * A(z/gamma2))
 * a_q: quantized LPC coefficients [order+1]
 * order: LPC order
 * h_w: output impulse response [subfr_size] */
void tlcs_cb_impulse_response(const float *a_q, int32_t order,
                               float *h_w, int32_t subfr_size);

/* Same as above but with explicit gamma parameters for adaptive weighting. */
void tlcs_cb_impulse_response_gamma(const float *a_q, int32_t order,
                                     float *h_w, int32_t subfr_size,
                                     float gamma1, float gamma2);

/* Search the algebraic codebook for best pulse configuration.
 * target: innovation target in excitation domain [subfr_size]
 *         (residual minus pitch contribution)
 * h_w: weighted synthesis impulse response [subfr_size]
 * cfg: codebook configuration
 * entry: output best pulse positions, signs, and quantized gain */
void tlcs_cb_search(const float *target, const float *h_w,
                    const tlcs_cb_config *cfg, tlcs_cb_entry *entry);

/* Search with pre-weighted target (already in weighted domain).
 * w_target: pre-weighted innovation target [subfr_size]
 *           (includes ZIR and h_w*residual, minus pitch contribution)
 * h_w: weighted synthesis impulse response [subfr_size]
 * cfg: codebook configuration
 * entry: output best pulse positions, signs, and quantized gain */
void tlcs_cb_search_weighted(const float *w_target, const float *h_w,
                              const tlcs_cb_config *cfg, tlcs_cb_entry *entry);

/* Multi-survivor tree search: explores pulse configurations using
 * tree search with survivor pruning. More thorough than greedy pair
 * search. Same interface as tlcs_cb_search_weighted. */
void tlcs_cb_search_tree(const float *w_target, const float *h_w,
                          const tlcs_cb_config *cfg, tlcs_cb_entry *entry);

/* Build innovation vector from codebook entry.
 * innov[n] = gain * sum_k( sign_k * delta(n - abs_pos_k) )
 * innov: output [subfr_size], zeroed then populated */
void tlcs_cb_build_innovation(const tlcs_cb_entry *entry,
                               const tlcs_cb_config *cfg,
                               float *innov, int32_t subfr_size);

/* Fixed codebook gain quantization (7-bit, signed log-magnitude) */
int32_t tlcs_fcb_gain_quantize(float gain);
float   tlcs_fcb_gain_dequantize(int32_t index);

/* Predictive FCB gain quantization: quantize log(gain/pred) for better
 * resolution when gains are correlated between subframes.
 * pred_gain: magnitude of previous subframe's quantized gain (>0, or 0 for fallback) */
int32_t tlcs_fcb_gain_quantize_pred(float gain, float pred_gain);
float   tlcs_fcb_gain_dequantize_pred(int32_t index, float pred_gain);

/* Parameterized predictive FCB gain quantization (variable bits).
 * Same algorithm as above but with configurable bit depth. */
int32_t tlcs_fcb_gain_quantize_pred_n(float gain, float pred_gain, int32_t bits);
float   tlcs_fcb_gain_dequantize_pred_n(int32_t index, float pred_gain, int32_t bits);

/* Pitch sharpening: add periodicity to innovation vector.
 * innov[n] += beta * innov[n - pitch_lag] for n >= pitch_lag.
 * Helps break chicken-and-egg adaptive codebook quality problem. */
void tlcs_cb_pitch_sharpen(float *innov, int32_t subfr_size,
                            int32_t pitch_lag, float pitch_gain);

/* Phase dispersion: apply short all-pass FIR to innovation vector
 * to spread sparse pulse energy and reduce scratchiness.
 * Must be applied identically in encoder and decoder.
 * innov: innovation vector [subfr_size], modified in-place
 * pitch_gain: ACB main tap gain (controls dispersion strength) */
void tlcs_cb_phase_disperse(float *innov, int32_t subfr_size, float pitch_gain);

/* Innovation spreading: convolve innovation with symmetric 3-tap kernel
 * [alpha, 1, alpha] to reduce sparsity of ACELP pulses.
 * Must be applied identically in encoder and decoder.
 * innov: innovation vector [subfr_size], modified in-place
 * alpha: side-tap weight (e.g. 0.25) */
void tlcs_cb_spread_innovation(float *innov, int32_t subfr_size, float alpha);

/* Spread impulse response: convolve h_w with [alpha, 1, alpha] kernel.
 * Used to make tree search optimal for spread pulses.
 * h_w: impulse response [subfr_size], modified in-place
 * alpha: side-tap weight (e.g. 0.25) */
void tlcs_cb_spread_impulse_response(float *h_w, int32_t subfr_size, float alpha);

/* ── FFT-based perceptual weighting ───────────────────────────── */

/* Compute perceptual AR filter from signal spectrum using
 * Mel-scale masking model (inspired by SMPL's approach).
 * speech: windowed speech frame [frame_size]
 * frame_size: number of speech samples
 * sample_rate: effective sample rate (Hz)
 * b_perc: output perceptual filter coefficients [perc_order+1]
 * perc_order: order of perceptual filter
 * Returns 0 on success, -1 if fallback needed. */
int32_t tlcs_compute_perceptual_filter(const float *speech, int32_t frame_size,
                                        float sample_rate,
                                        float *b_perc, int32_t perc_order);

/* Compute impulse response h_w = B(z) / A(z) where B(z) is the
 * perceptual whitening filter and 1/A(z) is the synthesis filter.
 * a_q: quantized LPC [order+1]
 * b_perc: perceptual filter [perc_order+1]
 * h_w: output impulse response [subfr_size] */
void tlcs_perceptual_impulse_response(const float *a_q, int32_t order,
                                       const float *b_perc, int32_t perc_order,
                                       float *h_w, int32_t subfr_size);

#endif /* TLCS_CODEBOOK_H */
