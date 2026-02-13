#ifndef TLCS_LSF_VQ_H
#define TLCS_LSF_VQ_H

#include <stdint.h>

/* ── Split-VQ for LSF quantization ──────────────────────────────
 *
 * 4-split VQ: 4 splits × 4 dims.
 * Separate codebooks for LR (128 entries, 7-bit) and VLR (64 entries, 6-bit).
 * Operates on prediction residuals (delta = lsf - lsf_pred).
 */

#define LSF_VQ_NUM_SPLITS  4
#define LSF_VQ_SPLIT_DIM   4
#define LSF_VQ_SPLIT_SIZE  128   /* max entries (LR mode) */
#define LSF_VQ_ORDER       16

/* LR codebooks: 128 entries per split (7-bit mode) */
extern const float lsf_vq_cb0[128][4];
extern const float lsf_vq_cb1[128][4];
extern const float lsf_vq_cb2[128][4];
extern const float lsf_vq_cb3[128][4];

/* VLR codebooks: 64 entries per split (6-bit mode) */
extern const float lsf_vq_cb_vlr0[64][4];
extern const float lsf_vq_cb_vlr1[64][4];
extern const float lsf_vq_cb_vlr2[64][4];
extern const float lsf_vq_cb_vlr3[64][4];

/* Encode: find best VQ indices for prediction delta.
 * delta[order] = lsf - lsf_pred (input)
 * indices[4]   = VQ split indices (output)
 * delta_q[order] = quantized delta (output) */
void tlcs_lsf_vq_encode(const float *delta, int32_t order,
                          int32_t *indices, float *delta_q);
void tlcs_lsf_vq_encode_n(const float *delta, int32_t order,
                            int32_t *indices, float *delta_q,
                            int32_t vq_entries);

/* Decode: reconstruct quantized delta from VQ indices.
 * indices[4]   = VQ split indices (input)
 * delta_q[order] = quantized delta (output) */
void tlcs_lsf_vq_decode(const int32_t *indices, int32_t order,
                          float *delta_q);
void tlcs_lsf_vq_decode_n(const int32_t *indices, int32_t order,
                             float *delta_q, int32_t vq_entries);

#endif /* TLCS_LSF_VQ_H */
