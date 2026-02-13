#include "tlcs_lsf_vq.h"
#include <float.h>

/* LR codebook pointers (128 entries) */
static const float (*const lsf_vq_cb_lr[4])[4] = {
    lsf_vq_cb0, lsf_vq_cb1, lsf_vq_cb2, lsf_vq_cb3
};

/* VLR codebook pointers (64 entries) */
static const float (*const lsf_vq_cb_vlr[4])[4] = {
    lsf_vq_cb_vlr0, lsf_vq_cb_vlr1, lsf_vq_cb_vlr2, lsf_vq_cb_vlr3
};

void tlcs_lsf_vq_encode_n(const float *delta, int32_t order,
                            int32_t *indices, float *delta_q,
                            int32_t vq_entries)
{
    (void)order;  /* always 16 */

    /* Select codebook set based on target size */
    const float (*const *cb_set)[4] = (vq_entries <= 64) ? lsf_vq_cb_vlr
                                                          : lsf_vq_cb_lr;
    int32_t max_entries = (vq_entries <= 64) ? 64 : 128;
    if (vq_entries > max_entries) vq_entries = max_entries;

    for (int32_t s = 0; s < LSF_VQ_NUM_SPLITS; s++) {
        const float *d = &delta[s * LSF_VQ_SPLIT_DIM];
        const float (*cb)[4] = cb_set[s];
        float best_dist = FLT_MAX;
        int32_t best_idx = 0;

        for (int32_t i = 0; i < vq_entries; i++) {
            float dist = 0.0f;
            for (int32_t j = 0; j < LSF_VQ_SPLIT_DIM; j++) {
                float e = d[j] - cb[i][j];
                dist += e * e;
            }
            if (dist < best_dist) {
                best_dist = dist;
                best_idx = i;
            }
        }

        indices[s] = best_idx;
        for (int32_t j = 0; j < LSF_VQ_SPLIT_DIM; j++)
            delta_q[s * LSF_VQ_SPLIT_DIM + j] = cb[best_idx][j];
    }
}

void tlcs_lsf_vq_encode(const float *delta, int32_t order,
                          int32_t *indices, float *delta_q)
{
    tlcs_lsf_vq_encode_n(delta, order, indices, delta_q, LSF_VQ_SPLIT_SIZE);
}

void tlcs_lsf_vq_decode_n(const int32_t *indices, int32_t order,
                             float *delta_q, int32_t vq_entries)
{
    (void)order;  /* always 16 */

    const float (*const *cb_set)[4] = (vq_entries <= 64) ? lsf_vq_cb_vlr
                                                          : lsf_vq_cb_lr;
    int32_t max_entries = (vq_entries <= 64) ? 64 : 128;
    if (vq_entries > max_entries) vq_entries = max_entries;

    for (int32_t s = 0; s < LSF_VQ_NUM_SPLITS; s++) {
        const float (*cb)[4] = cb_set[s];
        int32_t idx = indices[s];
        if (idx < 0) idx = 0;
        if (idx >= vq_entries) idx = vq_entries - 1;
        for (int32_t j = 0; j < LSF_VQ_SPLIT_DIM; j++)
            delta_q[s * LSF_VQ_SPLIT_DIM + j] = cb[idx][j];
    }
}

void tlcs_lsf_vq_decode(const int32_t *indices, int32_t order,
                          float *delta_q)
{
    tlcs_lsf_vq_decode_n(indices, order, delta_q, LSF_VQ_SPLIT_SIZE);
}
