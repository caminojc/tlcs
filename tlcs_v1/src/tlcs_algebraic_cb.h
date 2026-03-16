/* Internal algebraic (fixed) codebook API — ISPP design.
 * 8 pulses (8k) or 4 pulses (5k), 8 tracks, 160-sample subframe.
 * Index split into lo (24 bits) and hi (24 bits). */
#ifndef TLCS_ALGEBRAIC_CB_H
#define TLCS_ALGEBRAIC_CB_H

#include "tlcs_config.h"

/* Search: find best N-pulse ISPP excitation.
 * num_pulses: number of pulses (8 for 8k, 4 for 5k).
 * target: residual target after ACB removal (subframe_size).
 * h:      impulse response of 1/A(z) (subframe_size).
 * out_index_lo: lower 24 bits of packed index.
 * out_index_hi: upper 24 bits of packed index (0 for 4 pulses).
 * out_gain:  optimal codebook gain (weighted-domain).
 * out_exc:   excitation vector (subframe_size). */
void tlcs_acb_search_n(const float *target, const float *h,
                       int subframe_size, int num_pulses,
                       int *out_index_lo, int *out_index_hi,
                       float *out_gain, float *out_exc);

/* Decode: reconstruct excitation from packed index.
 * num_pulses: number of pulses (8 for 8k, 4 for 5k).
 * out_exc: excitation vector (subframe_size, zeroed then filled). */
void tlcs_acb_decode_n(int index_lo, int index_hi,
                       int subframe_size, int num_pulses, float *out_exc);

/* Legacy wrappers (8 pulses) */
static inline void tlcs_acb_search(const float *target, const float *h,
                                   int subframe_size,
                                   int *out_index_lo, int *out_index_hi,
                                   float *out_gain, float *out_exc)
{
    tlcs_acb_search_n(target, h, subframe_size, TLCS_ACB_NUM_PULSES,
                      out_index_lo, out_index_hi, out_gain, out_exc);
}

static inline void tlcs_acb_decode(int index_lo, int index_hi,
                                   int subframe_size, float *out_exc)
{
    tlcs_acb_decode_n(index_lo, index_hi, subframe_size,
                      TLCS_ACB_NUM_PULSES, out_exc);
}

#endif /* TLCS_ALGEBRAIC_CB_H */
