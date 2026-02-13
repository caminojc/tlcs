#ifndef TLCS_PITCH_H
#define TLCS_PITCH_H

#include "tlcs/tlcs_types.h"

/* ── Pitch search parameters ─────────────────────────────────── */

#define TLCS_PITCH_OL_DECIMATE    2    /* decimate factor for OL search */
#define TLCS_PITCH_CL_DELTA       16   /* closed-loop search range ± */
#define TLCS_PITCH_GAIN_BITS      4    /* bits for adaptive gain */
#define TLCS_PITCH_DELTA_BITS     7    /* bits for delta pitch lag index */
#define TLCS_PITCH_DELTA_OFFSET  64    /* signed-to-unsigned offset */
#define TLCS_PITCH_GAIN_LEVELS   (1 << TLCS_PITCH_GAIN_BITS)  /* 16 */
#define TLCS_PITCH_GAIN_MAX       1.5f

/* ── Fractional pitch lag ─────────────────────────────────────── */

#define TLCS_PITCH_FRAC_RES       3    /* 1/3 sample resolution */
#define TLCS_PITCH_FRAC_RANGE   120    /* fractional resolution for lags <= this */
#define TLCS_PITCH_SINC_HALF      4    /* sinc interpolation half-length */
#define TLCS_PITCH_SINC_LEN      (2 * TLCS_PITCH_SINC_HALF + 2) /* 10 taps */

/* ── Excitation buffer management ────────────────────────────── */

/* Excitation buffer layout:
 *   [0 .. MAX_PITCH_LAG-1]  : history (past excitation)
 *   [MAX_PITCH_LAG .. MAX_PITCH_LAG+frame_size-1] : current frame
 *
 * Before each frame, shift buffer left by frame_size.
 * The "current" pointer is at offset MAX_PITCH_LAG.
 */

/* Shift excitation buffer: move recent history into position.
 * Call at the start of each frame. */
void tlcs_exc_buf_shift(float *exc_buf, int32_t frame_size);

/* ── Open-loop pitch detection ───────────────────────────────── */

/* Search for best pitch lag using normalized autocorrelation.
 * signal: the LPC residual for the current frame
 * exc_buf: excitation buffer (contains history + current frame space)
 * n: frame size
 * min_lag, max_lag: search range
 * voicing: output normalized correlation [0, 1]
 * Returns best integer lag. */
int32_t tlcs_pitch_ol_search(const float *exc_buf,
                             int32_t n, int32_t min_lag, int32_t max_lag,
                             float *voicing);

/* ── Closed-loop pitch search ────────────────────────────────── */

/* Refine pitch lag per subframe around open-loop estimate.
 * target: subframe of residual to match
 * exc_buf: excitation buffer (search backward from current position)
 * exc_offset: offset into exc_buf for current subframe start
 * subfr_size: subframe length
 * center_lag: open-loop estimate to refine around
 * delta: search range ±delta
 * gain: output adaptive codebook gain
 * Returns best lag. */
int32_t tlcs_pitch_cl_search(const float *target,
                             const float *exc_buf, int32_t exc_offset,
                             int32_t subfr_size,
                             int32_t center_lag, int32_t delta,
                             int32_t min_lag, int32_t max_lag,
                             float *gain);

/* ── Adaptive gain quantization (scalar — legacy) ──────────────── */

int32_t tlcs_pitch_gain_quantize(float gain);
float   tlcs_pitch_gain_dequantize(int32_t index);

/* ── 2-tap symmetric ACB gain VQ ───────────────────────────────── */
/*
 * 2-tap symmetric pitch predictor:
 *   a(n) = g0 * exc[n-lag] + g1 * (exc[n-lag-1] + exc[n-lag+1])
 *
 * 16-entry VQ codebook (4 bits), same cost as scalar gain.
 * Captures pitch pulse shape that single-tap cannot.
 */
#define TLCS_ACB_VQ_SIZE   16
#define TLCS_ACB_VQ_SIZE_HR 16
#define TLCS_ACB_VQ_TAPS   2

extern const float tlcs_acb_vq[TLCS_ACB_VQ_SIZE][TLCS_ACB_VQ_TAPS];

/* Extract 2 ACB basis vectors: v0 = exc[n-lag], v1 = exc[n-lag-1]+exc[n-lag+1].
 * Works with both integer and fractional lags. */
void tlcs_pitch_get_acb_basis(const float *exc_buf, int32_t exc_offset,
                               int32_t lag_int, int32_t frac,
                               float *v0, float *v1, int32_t subfr_size);

/* Search the 2-tap VQ codebook in the weighted domain.
 * w_v0, w_v1: basis vectors convolved with h_w.
 * Returns VQ index (0..TLCS_ACB_VQ_SIZE-1). */
int32_t tlcs_acb_vq_search(const float *w_target,
                            const float *w_v0, const float *w_v1,
                            int32_t subfr_size);

/* ── Adaptive codebook vector extraction ─────────────────────── */

/* Extract adaptive codebook vector: v[n] = exc_buf[exc_offset + n - lag]
 * for n = 0..subfr_size-1. Handles lag < subfr_size (pitch repetition). */
void tlcs_pitch_get_adaptive_vec(const float *exc_buf, int32_t exc_offset,
                                 int32_t lag, float *vec, int32_t subfr_size);

/* Extract adaptive codebook vector with fractional lag interpolation.
 * lag_int: integer part of lag
 * frac: fractional part (0, 1, or 2 for 0/3, 1/3, 2/3 sample delay)
 * Uses sinc interpolation with Hamming window for fractional delays. */
void tlcs_pitch_get_adaptive_vec_frac(const float *exc_buf, int32_t exc_offset,
                                       int32_t lag_int, int32_t frac,
                                       float *vec, int32_t subfr_size);

/* ── Fractional pitch lag encoding ────────────────────────────── */

/* Encode (lag_int, frac) to 9-bit index.
 * Lags 20-84: fractional, index = (lag-20)*3 + frac
 * Lags 85-300: integer, index = 195 + (lag-85) */
int32_t tlcs_pitch_encode_lag(int32_t lag_int, int32_t frac);

/* Decode 9-bit index to (lag_int, frac). */
void tlcs_pitch_decode_lag(int32_t index, int32_t *lag_int, int32_t *frac);

/* ── Fractional closed-loop pitch search ──────────────────────── */

/* Search integer and fractional lags around center_lag.
 * For lags <= FRAC_RANGE: searches at 1/3 sample resolution.
 * For lags > FRAC_RANGE: searches at integer resolution.
 * target: weighted target signal (in perceptual domain)
 * h_w: weighted synthesis impulse response
 * Returns best lag via (lag_int, frac). */
void tlcs_pitch_cl_search_frac(const float *target, const float *h_w,
                                const float *exc_buf, int32_t exc_offset,
                                int32_t subfr_size,
                                int32_t center_lag, int32_t delta,
                                int32_t min_lag, int32_t max_lag,
                                int32_t *out_lag, int32_t *out_frac,
                                float *out_gain);

#endif /* TLCS_PITCH_H */
