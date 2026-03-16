/*
 * tlcs_quantization.c — Split VQ for LSP, joint gain VQ, scalar pitch gain.
 *
 * LSP split-VQ: 4 splits x 256 entries x 4 dimensions each = 32 bits.
 * Gain VQ: 64 entries of (pitch_gain, cb_gain), 6 bits.
 * Codebooks initialised with linearly-spaced defaults (no training needed).
 */
#include "tlcs_config.h"
#include "tlcs_quantization.h"
#include "tlcs_codebooks.h"

#include <math.h>
#include <string.h>

/* ================================================================== */
/* LSP Split VQ                                                        */
/* ================================================================== */

/*
 * LPC order 16, 4 splits of 4 elements each.
 * Each split has 256 entries (8 bits).
 */
#define LSP_SPLIT_DIM   (TLCS_LPC_ORDER / TLCS_LSP_NUM_SPLITS)  /* 4 */

static float lsp_codebook[TLCS_LSP_NUM_SPLITS][TLCS_LSP_CB_SIZE][LSP_SPLIT_DIM];
static int   lsp_vq_ready = 0;

void tlcs_lsp_vq_init(void)
{
    if (lsp_vq_ready) return;

    /* Load trained codebooks from tlcs_codebooks.h */
    const float *trained[4] = {
        (const float *)tlcs_lsp_cb_split0,
        (const float *)tlcs_lsp_cb_split1,
        (const float *)tlcs_lsp_cb_split2,
        (const float *)tlcs_lsp_cb_split3,
    };
    for (int s = 0; s < TLCS_LSP_NUM_SPLITS; s++) {
        memcpy(lsp_codebook[s], trained[s],
               (size_t)(TLCS_LSP_CB_SIZE * LSP_SPLIT_DIM) * sizeof(float));
    }

    lsp_vq_ready = 1;
}

/* Predictive LSP VQ: quantize (lsp - prev_lsp) using delta codebooks.
 * Consecutive frames have similar LSPs → delta is small → VQ captures it better.
 * Encoder/decoder maintain prev_lsp state. */
static float lsp_vq_prev[TLCS_LPC_ORDER];
static int   lsp_vq_prev_valid = 0;

void tlcs_lsp_vq_reset_pred(void)
{
    lsp_vq_prev_valid = 0;
}

void tlcs_lsp_vq_quantize(const float *lsp, int *indices, float *lsp_q)
{
    if (!lsp_vq_ready) tlcs_lsp_vq_init();

    /* Initialize predictor with uniform LSPs if first frame */
    if (!lsp_vq_prev_valid) {
        for (int i = 0; i < TLCS_LPC_ORDER; i++)
            lsp_vq_prev[i] = 3.14159f * (float)(i + 1) / (float)(TLCS_LPC_ORDER + 1);
        lsp_vq_prev_valid = 1;
    }

    /* Compute delta from prediction */
    float delta[TLCS_LPC_ORDER];
    for (int i = 0; i < TLCS_LPC_ORDER; i++)
        delta[i] = lsp[i] - lsp_vq_prev[i];

    /* Quantize delta using split VQ */
    for (int s = 0; s < TLCS_LSP_NUM_SPLITS; s++) {
        const float *sub = &delta[s * LSP_SPLIT_DIM];
        int best_idx = 0;
        float best_dist = 1e30f;

        for (int i = 0; i < TLCS_LSP_CB_SIZE; i++) {
            float dist = 0.0f;
            for (int d = 0; d < LSP_SPLIT_DIM; d++) {
                float diff = sub[d] - lsp_codebook[s][i][d];
                dist += diff * diff;
            }
            if (dist < best_dist) {
                best_dist = dist;
                best_idx = i;
            }
        }

        indices[s] = best_idx;
        /* Reconstruct: prev + quantized_delta */
        for (int d = 0; d < LSP_SPLIT_DIM; d++) {
            lsp_q[s * LSP_SPLIT_DIM + d] =
                lsp_vq_prev[s * LSP_SPLIT_DIM + d] + lsp_codebook[s][best_idx][d];
        }
    }

    /* Update predictor with quantized output (decoder will do the same) */
    memcpy(lsp_vq_prev, lsp_q, TLCS_LPC_ORDER * sizeof(float));
}

void tlcs_lsp_vq_dequantize(const int *indices, float *lsp_out)
{
    if (!lsp_vq_ready) tlcs_lsp_vq_init();

    if (!lsp_vq_prev_valid) {
        for (int i = 0; i < TLCS_LPC_ORDER; i++)
            lsp_vq_prev[i] = 3.14159f * (float)(i + 1) / (float)(TLCS_LPC_ORDER + 1);
        lsp_vq_prev_valid = 1;
    }

    for (int s = 0; s < TLCS_LSP_NUM_SPLITS; s++) {
        int idx = indices[s];
        if (idx < 0) idx = 0;
        if (idx >= TLCS_LSP_CB_SIZE) idx = TLCS_LSP_CB_SIZE - 1;
        for (int d = 0; d < LSP_SPLIT_DIM; d++) {
            lsp_out[s * LSP_SPLIT_DIM + d] =
                lsp_vq_prev[s * LSP_SPLIT_DIM + d] + lsp_codebook[s][idx][d];
        }
    }

    memcpy(lsp_vq_prev, lsp_out, TLCS_LPC_ORDER * sizeof(float));
}

/* ================================================================== */
/* Joint Gain VQ                                                       */
/* ================================================================== */

/*
 * 64 entries: 8 pitch-gain levels x 8 cb-gain levels.
 * Each entry is (pitch_gain, cb_gain).
 */
static float gain_codebook[TLCS_GAIN_CB_SIZE][2];
static int   gain_vq_ready = 0;

void tlcs_gain_vq_init(void)
{
    if (gain_vq_ready) return;

    /* Load trained gain codebook from tlcs_codebooks.h */
    memcpy(gain_codebook, tlcs_gain_cb,
           (size_t)(TLCS_GAIN_CB_SIZE * 2) * sizeof(float));

    gain_vq_ready = 1;
}

int tlcs_gain_vq_quantize(float pg, float cg, float *q_pg, float *q_cg)
{
    if (!gain_vq_ready) tlcs_gain_vq_init();

    int best_idx = 0;
    float best_dist = 1e30f;

    for (int i = 0; i < TLCS_GAIN_CB_SIZE; i++) {
        float dp = pg - gain_codebook[i][0];
        float dc = cg - gain_codebook[i][1];
        float dist = dp * dp + dc * dc;
        if (dist < best_dist) {
            best_dist = dist;
            best_idx = i;
        }
    }

    *q_pg = gain_codebook[best_idx][0];
    *q_cg = gain_codebook[best_idx][1];
    return best_idx;
}

void tlcs_gain_vq_dequantize(int index, float *pg, float *cg)
{
    if (!gain_vq_ready) tlcs_gain_vq_init();

    if (index < 0) index = 0;
    if (index >= TLCS_GAIN_CB_SIZE) index = TLCS_GAIN_CB_SIZE - 1;

    *pg = gain_codebook[index][0];
    *cg = gain_codebook[index][1];
}

/* ================================================================== */
/* Scalar pitch gain quantizer                                         */
/* ================================================================== */

int tlcs_pitch_gain_quantize(float gain)
{
    int levels = 1 << TLCS_PITCH_GAIN_BITS;  /* 16 */
    int idx = (int)roundf(gain / 1.2f * (float)(levels - 1));
    if (idx < 0) idx = 0;
    if (idx >= levels) idx = levels - 1;
    return idx;
}

float tlcs_pitch_gain_dequantize(int index)
{
    int levels = 1 << TLCS_PITCH_GAIN_BITS;
    if (index < 0) index = 0;
    if (index >= levels) index = levels - 1;
    return 1.2f * (float)index / (float)(levels - 1);
}

/* ================================================================== */
/* FCB Gain — dB-stepped quantizer (SMPL-style)                        */
/* ================================================================== */

int tlcs_fcbgain_quantize(float gain_weighted, int voiced, float *out_gain)
{
    float abs_gain = fabsf(gain_weighted);
    if (abs_gain < 1e-16f) abs_gain = 1e-16f;
    float gain_db = 20.0f * log10f(abs_gain);

    float min_db, step_db;
    int steps;
    if (voiced) {
        min_db = TLCS_V_GAIN_MIN_DB;
        step_db = TLCS_V_GAIN_STEP_DB;
        steps = TLCS_V_GAIN_STEPS;
    } else {
        min_db = TLCS_UV_GAIN_MIN_DB;
        step_db = TLCS_UV_GAIN_STEP_DB;
        steps = TLCS_UV_GAIN_STEPS;
    }

    /* Clamp to range */
    float max_db = min_db + (steps - 1) * step_db;
    if (gain_db < min_db) gain_db = min_db;
    if (gain_db > max_db) gain_db = max_db;

    int idx = (int)roundf((gain_db - min_db) / step_db);
    if (idx < 0) idx = 0;
    if (idx >= steps) idx = steps - 1;

    /* Reconstruct quantized gain (preserve sign) */
    float q_db = min_db + (float)idx * step_db;
    float q_gain = powf(10.0f, 0.05f * q_db);
    if (gain_weighted < 0.0f) q_gain = -q_gain;

    *out_gain = q_gain;
    return idx;
}

float tlcs_fcbgain_dequantize(int index, int voiced)
{
    float min_db, step_db;
    int steps;
    if (voiced) {
        min_db = TLCS_V_GAIN_MIN_DB;
        step_db = TLCS_V_GAIN_STEP_DB;
        steps = TLCS_V_GAIN_STEPS;
    } else {
        min_db = TLCS_UV_GAIN_MIN_DB;
        step_db = TLCS_UV_GAIN_STEP_DB;
        steps = TLCS_UV_GAIN_STEPS;
    }
    if (index < 0) index = 0;
    if (index >= steps) index = steps - 1;
    float q_db = min_db + (float)index * step_db;
    return powf(10.0f, 0.05f * q_db);
}
