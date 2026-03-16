/* Internal algebraic (fixed) codebook API — ISPP design. */
#ifndef TLCS_ALGEBRAIC_CB_H
#define TLCS_ALGEBRAIC_CB_H

#include "tlcs_config.h"

/* Search: find best 2-pulse ISPP excitation.
 * target: residual target after ACB removal (subframe_size).
 * h:      impulse response of 1/A(z) (subframe_size).
 * out_index: packed index (12 bits for 2 pulses).
 * out_gain:  optimal codebook gain.
 * out_exc:   excitation vector (subframe_size). */
void tlcs_acb_search(const float *target, const float *h,
                     int subframe_size,
                     int *out_index, float *out_gain, float *out_exc);

/* Decode: reconstruct excitation from packed index.
 * out_exc: excitation vector (subframe_size, zeroed then filled). */
void tlcs_acb_decode(int index, int subframe_size, float *out_exc);

#endif /* TLCS_ALGEBRAIC_CB_H */
