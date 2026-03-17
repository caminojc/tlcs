/* TLC Neural Excitation — Mode N
 *
 * Lightweight neural speech synthesis:
 *   LPC analysis (DSP) → Neural excitation (tiny GRU) → LPC synthesis (DSP)
 *
 * Like Lyra but 10x lighter:
 *   - Lyra: ~2M params, full vocoder, needs GPU/TPU
 *   - TLC Neural: ~200K params, LPC does 90% of work, runs on phone CPU
 *
 * The model generates excitation samples conditioned on:
 *   - LPC coefficients (spectral envelope)
 *   - Pitch lag (fundamental frequency)
 *   - Voicing strength (voiced/unvoiced mix)
 *
 * Model weights loaded from a binary file at init.
 * All inference is fixed-point friendly (Tanh activations, bounded output).
 */
#ifndef TLCS_NEURAL_H
#define TLCS_NEURAL_H

#include "tlcs/tlcs_types.h"

/* Model dimensions */
#define TN_COND_DIM     19      /* LPC(17) + pitch(1) + voicing(1) */
#define TN_COND_FC      64      /* conditioning FC output */
#define TN_GRU_HIDDEN   192     /* GRU hidden size */
#define TN_OUT_FC       64      /* output FC hidden */

/* Runtime state */
typedef struct {
    /* GRU hidden state */
    float gru_h[TN_GRU_HIDDEN];
    /* Previous output sample */
    float prev_sample;
    /* Model weights (loaded from file) */
    float *weights;
    int32_t weights_size;
    int32_t initialized;
} tlcs_neural_state;

/* Initialize neural mode. Loads weights from file.
 * Returns 0 on success, -1 on failure. */
int32_t tlcs_neural_init(tlcs_neural_state *state, const char *weights_path);

/* Free neural state */
void tlcs_neural_free(tlcs_neural_state *state);

/* Generate one frame of excitation samples.
 * Inputs:
 *   lpc[LPC_ORDER+1] — LPC coefficients for this frame
 *   pitch_lag — pitch period in samples (0 = unvoiced)
 *   voicing — voicing strength [0, 1]
 * Output:
 *   exc_out[frame_size] — excitation samples
 */
void tlcs_neural_generate(tlcs_neural_state *state,
                           const float *lpc, int32_t pitch_lag, float voicing,
                           float *exc_out, int32_t frame_size);

#endif /* TLCS_NEURAL_H */
