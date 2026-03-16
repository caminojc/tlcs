/* Internal pitch analysis API. */
#ifndef TLCS_PITCH_H
#define TLCS_PITCH_H

#include "tlcs_config.h"

/* Open-loop pitch estimate via normalised autocorrelation.
 * Returns integer lag with highest correlation. */
int tlcs_pitch_open_loop(const float *residual, int len,
                         int min_lag, int max_lag);

/* Closed-loop pitch search (analysis-by-synthesis).
 * Searches integer lags then refines fractionally (1/3 sample).
 * Returns lag (float) and gain (clamped to [0, 1.2]). */
void tlcs_pitch_closed_loop(const float *target, const float *h,
                            const float *exc_buf, int exc_len,
                            int subframe_size, int min_lag, int max_lag,
                            float *out_lag, float *out_gain);

/* Build adaptive codebook excitation vector with fractional lag.
 * exc_buf: past excitation (length exc_len).
 * lag: float lag (may be fractional).
 * out: output vector (subframe_size samples). */
void tlcs_pitch_build_acb(const float *exc_buf, int exc_len,
                          float lag, int subframe_size, float *out);

/* Convolve x with h, causal, truncated to len samples. */
void tlcs_convolve(const float *x, const float *h, int len, float *out);

#endif /* TLCS_PITCH_H */
