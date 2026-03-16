/* Internal algebraic (fixed) codebook API — ISPP design.
 * 8 pulses, 8 tracks, 160-sample subframe.
 * 48-bit index split into lo (24 bits) and hi (24 bits). */
#ifndef TLCS_ALGEBRAIC_CB_H
#define TLCS_ALGEBRAIC_CB_H

#include "tlcs_config.h"

/* Search: find best 8-pulse ISPP excitation.
 * target: residual target after ACB removal (subframe_size).
 * h:      impulse response of 1/A(z) (subframe_size).
 * out_index_lo: lower 24 bits of packed 48-bit index.
 * out_index_hi: upper 24 bits of packed 48-bit index.
 * out_gain:  optimal codebook gain (weighted-domain).
 * out_exc:   excitation vector (subframe_size). */
void tlcs_acb_search(const float *target, const float *h,
                     int subframe_size,
                     int *out_index_lo, int *out_index_hi,
                     float *out_gain, float *out_exc);

/* Decode: reconstruct excitation from packed index.
 * out_exc: excitation vector (subframe_size, zeroed then filled). */
void tlcs_acb_decode(int index_lo, int index_hi,
                     int subframe_size, float *out_exc);

#endif /* TLCS_ALGEBRAIC_CB_H */
