/* TLC Neural Excitation — C inference runtime.
 *
 * Implements a tiny GRU-based excitation generator.
 * Architecture matches the PyTorch training script:
 *   cond_net: FC(19→64) + Tanh + FC(64→64) + Tanh
 *   gru: GRU(65→192)  (64 cond + 1 prev sample)
 *   out: FC(192→64) + Tanh + FC(64→1) + Tanh
 *
 * Weights loaded from a flat binary file exported by PyTorch.
 * All operations are float32 (int8 quantization planned).
 */
#include "tlcs_neural.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Weight layout in binary file ──────────────────────────────────
 * Stored as flat float32 arrays in this order:
 *   cond_fc1_w: (64, 19)
 *   cond_fc1_b: (64,)
 *   cond_fc2_w: (64, 64)
 *   cond_fc2_b: (64,)
 *   gru_ih_w:   (3*192, 65)    — input-to-hidden (z,r,n gates)
 *   gru_ih_b:   (3*192,)
 *   gru_hh_w:   (3*192, 192)   — hidden-to-hidden
 *   gru_hh_b:   (3*192,)
 *   out_fc1_w:  (64, 192)
 *   out_fc1_b:  (64,)
 *   out_fc2_w:  (1, 64)
 *   out_fc2_b:  (1,)
 */

/* Weight offsets (computed from dimensions) */
#define W_COND1_W    0
#define W_COND1_B    (W_COND1_W + 64 * TN_COND_DIM)
#define W_COND2_W    (W_COND1_B + 64)
#define W_COND2_B    (W_COND2_W + 64 * 64)
#define W_GRU_IH_W   (W_COND2_B + 64)
#define W_GRU_IH_B   (W_GRU_IH_W + 3 * TN_GRU_HIDDEN * 65)
#define W_GRU_HH_W   (W_GRU_IH_B + 3 * TN_GRU_HIDDEN)
#define W_GRU_HH_B   (W_GRU_HH_W + 3 * TN_GRU_HIDDEN * TN_GRU_HIDDEN)
#define W_OUT1_W      (W_GRU_HH_B + 3 * TN_GRU_HIDDEN)
#define W_OUT1_B      (W_OUT1_W + TN_OUT_FC * TN_GRU_HIDDEN)
#define W_OUT2_W      (W_OUT1_B + TN_OUT_FC)
#define W_OUT2_B      (W_OUT2_W + 1 * TN_OUT_FC)
#define W_TOTAL       (W_OUT2_B + 1)

static float tanhf_fast(float x)
{
    if (x > 4.0f) return 1.0f;
    if (x < -4.0f) return -1.0f;
    float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

static float sigmoidf(float x)
{
    return 1.0f / (1.0f + expf(-x));
}

/* FC layer: out = tanh(W @ in + b) */
static void fc_tanh(const float *W, const float *b,
                     const float *in, float *out,
                     int32_t out_dim, int32_t in_dim)
{
    for (int32_t i = 0; i < out_dim; i++) {
        float sum = b[i];
        for (int32_t j = 0; j < in_dim; j++)
            sum += W[i * in_dim + j] * in[j];
        out[i] = tanhf_fast(sum);
    }
}

/* GRU step: h' = GRU(input, h) */
static void gru_step(const float *ih_w, const float *ih_b,
                      const float *hh_w, const float *hh_b,
                      const float *input, float *h,
                      int32_t hidden, int32_t input_dim)
{
    float z[TN_GRU_HIDDEN], r[TN_GRU_HIDDEN], n[TN_GRU_HIDDEN];

    /* Compute gates: z, r, n */
    for (int32_t i = 0; i < hidden; i++) {
        float zi = ih_b[i], ri = ih_b[hidden + i], ni = ih_b[2*hidden + i];
        float zh = hh_b[i], rh = hh_b[hidden + i], nh = hh_b[2*hidden + i];

        /* Input contribution */
        for (int32_t j = 0; j < input_dim; j++) {
            zi += ih_w[i * input_dim + j] * input[j];
            ri += ih_w[(hidden + i) * input_dim + j] * input[j];
            ni += ih_w[(2*hidden + i) * input_dim + j] * input[j];
        }
        /* Hidden contribution */
        for (int32_t j = 0; j < hidden; j++) {
            zi += hh_w[i * hidden + j] * h[j];
            ri += hh_w[(hidden + i) * hidden + j] * h[j];
            rh += hh_w[(2*hidden + i) * hidden + j] * h[j];
        }

        z[i] = sigmoidf(zi + zh);
        r[i] = sigmoidf(ri);
        n[i] = tanhf_fast(ni + r[i] * rh);
    }

    /* Update hidden state */
    for (int32_t i = 0; i < hidden; i++)
        h[i] = (1.0f - z[i]) * n[i] + z[i] * h[i];
}

int32_t tlcs_neural_init(tlcs_neural_state *state, const char *weights_path)
{
    memset(state, 0, sizeof(*state));

    FILE *f = fopen(weights_path, "rb");
    if (!f) {
        fprintf(stderr, "Neural: cannot open weights: %s\n", weights_path);
        return -1;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    int32_t expected = W_TOTAL * (int32_t)sizeof(float);
    if (size < expected) {
        fprintf(stderr, "Neural: weights file too small (%ld < %d)\n", size, expected);
        fclose(f);
        return -1;
    }

    state->weights = (float *)malloc((size_t)expected);
    if (!state->weights) { fclose(f); return -1; }

    fread(state->weights, sizeof(float), W_TOTAL, f);
    fclose(f);

    state->weights_size = W_TOTAL;
    state->initialized = 1;
    state->prev_sample = 0.0f;
    memset(state->gru_h, 0, sizeof(state->gru_h));

    fprintf(stderr, "Neural: loaded %d weights (%.0f KB)\n",
            W_TOTAL, (float)expected / 1024.0f);
    return 0;
}

void tlcs_neural_free(tlcs_neural_state *state)
{
    if (state->weights) free(state->weights);
    memset(state, 0, sizeof(*state));
}

void tlcs_neural_generate(tlcs_neural_state *state,
                           const float *lpc, int32_t pitch_lag, float voicing,
                           float *exc_out, int32_t frame_size)
{
    if (!state->initialized) {
        memset(exc_out, 0, (size_t)frame_size * sizeof(float));
        return;
    }

    const float *w = state->weights;

    /* Build conditioning vector */
    float cond[TN_COND_DIM];
    for (int32_t i = 0; i <= LPC_ORDER; i++) cond[i] = lpc[i];
    cond[LPC_ORDER + 1] = (float)pitch_lag / 300.0f;
    cond[LPC_ORDER + 2] = voicing;

    /* Conditioning network (frame-rate, compute once) */
    float c1[64], c2[64];
    fc_tanh(&w[W_COND1_W], &w[W_COND1_B], cond, c1, 64, TN_COND_DIM);
    fc_tanh(&w[W_COND2_W], &w[W_COND2_B], c1, c2, 64, 64);

    /* Sample-rate loop */
    for (int32_t i = 0; i < frame_size; i++) {
        /* GRU input: [conditioning(64), prev_sample(1)] = 65 */
        float gru_input[65];
        memcpy(gru_input, c2, 64 * sizeof(float));
        gru_input[64] = state->prev_sample;

        /* GRU step */
        gru_step(&w[W_GRU_IH_W], &w[W_GRU_IH_B],
                 &w[W_GRU_HH_W], &w[W_GRU_HH_B],
                 gru_input, state->gru_h,
                 TN_GRU_HIDDEN, 65);

        /* Output layers */
        float o1[64];
        fc_tanh(&w[W_OUT1_W], &w[W_OUT1_B], state->gru_h, o1, 64, TN_GRU_HIDDEN);

        float sample = w[W_OUT2_B];
        for (int32_t j = 0; j < 64; j++)
            sample += w[W_OUT2_W + j] * o1[j];
        sample = tanhf_fast(sample);

        exc_out[i] = sample;
        state->prev_sample = sample;
    }
}
