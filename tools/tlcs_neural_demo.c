/* TLC Neural Mode Demo
 *
 * Encodes speech to LPC+pitch features (DSP), then decodes using
 * the neural excitation generator instead of algebraic codebook.
 *
 * Usage: tlcs_neural_demo <input.wav> <output.wav> [weights.bin]
 */
#include "tlcs/tlcs.h"
#include "tlcs/tlcs_wav.h"
#include "../src/codec/tlcs_lpc.h"
#include "../src/codec/tlcs_pitch.h"
#include "../src/codec/tlcs_neural.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define FRAME_SIZE 160   /* 10ms at 16kHz */
#define LPC_ORDER  16

static void lpc_synthesis(const float *a, int order,
                           const float *exc, float *out,
                           int n, float *mem)
{
    for (int i = 0; i < n; i++) {
        float s = exc[i];
        for (int k = 1; k <= order; k++) {
            int idx = i - k;
            float past = (idx >= 0) ? out[idx] : mem[order + idx];
            s -= a[k] * past;
        }
        out[i] = s;
    }
    /* Update memory */
    for (int k = 0; k < order; k++)
        mem[k] = out[n - order + k];
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <input.wav> <output.wav> [weights.bin]\n", argv[0]);
        return 1;
    }

    const char *in_path = argv[1];
    const char *out_path = argv[2];
    const char *weights = (argc > 3) ? argv[3] : "models/tlc_neural.bin";

    /* Read input WAV */
    tlcs_wav wav_in;
    if (tlcs_wav_open_read(&wav_in, in_path) != TLCS_OK) {
        fprintf(stderr, "Error: cannot open %s\n", in_path);
        return 1;
    }
    int32_t n_samples = wav_in.num_samples;
    int32_t sample_rate = wav_in.sample_rate;
    int16_t *pcm_in = (int16_t *)malloc((size_t)n_samples * sizeof(int16_t));
    int32_t samples_read;
    tlcs_wav_read(&wav_in, pcm_in, n_samples, &samples_read);
    tlcs_wav_close(&wav_in);
    fprintf(stderr, "Input: %s, %d Hz, %d samples\n", in_path, sample_rate, n_samples);

    /* Init neural model */
    tlcs_neural_state neural;
    if (tlcs_neural_init(&neural, weights) != 0) {
        fprintf(stderr, "Error: cannot load neural weights from %s\n", weights);
        free(pcm_in);
        return 1;
    }

    /* Process frame by frame */
    int32_t n_frames = n_samples / FRAME_SIZE;
    int16_t *pcm_out = (int16_t *)calloc((size_t)n_samples, sizeof(int16_t));
    float synth_mem[LPC_ORDER];
    memset(synth_mem, 0, sizeof(synth_mem));
    float deemph_mem = 0.0f;

    for (int32_t f = 0; f < n_frames; f++) {
        int32_t offset = f * FRAME_SIZE;
        float frame[FRAME_SIZE];

        /* Pre-emphasis */
        for (int i = 0; i < FRAME_SIZE; i++) {
            float s = (float)pcm_in[offset + i] / 32768.0f;
            float prev = (offset + i > 0) ? (float)pcm_in[offset + i - 1] / 32768.0f : 0.0f;
            frame[i] = s - 0.60f * prev;
        }

        /* LPC analysis */
        float r[LPC_ORDER + 1];
        tlcs_autocorrelation(frame, FRAME_SIZE, r, LPC_ORDER);
        float a[LPC_ORDER + 1];
        tlcs_levinson(r, LPC_ORDER, a, NULL);

        /* Pitch detection */
        float voicing = 0.0f;
        int32_t lag = tlcs_pitch_ol_search(frame, FRAME_SIZE, 20, 300, &voicing);

        /* Neural excitation generation */
        float exc[FRAME_SIZE];
        tlcs_neural_generate(&neural, a, lag, voicing, exc, FRAME_SIZE);

        /* Scale excitation by residual energy estimate */
        float frame_energy = 0.0f;
        for (int i = 0; i < FRAME_SIZE; i++)
            frame_energy += frame[i] * frame[i];
        float rms = sqrtf(frame_energy / FRAME_SIZE);
        for (int i = 0; i < FRAME_SIZE; i++)
            exc[i] *= rms * 4.0f;  /* scale factor (tunable) */

        /* LPC synthesis */
        float synth[FRAME_SIZE];
        lpc_synthesis(a, LPC_ORDER, exc, synth, FRAME_SIZE, synth_mem);

        /* De-emphasis */
        for (int i = 0; i < FRAME_SIZE; i++) {
            synth[i] = synth[i] + 0.60f * deemph_mem;
            deemph_mem = synth[i];
        }

        /* Convert to PCM */
        for (int i = 0; i < FRAME_SIZE; i++) {
            float v = synth[i] * 32768.0f;
            if (v > 32767.0f) v = 32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            pcm_out[offset + i] = (int16_t)v;
        }
    }

    /* Write output WAV */
    tlcs_wav wav_out;
    tlcs_wav_open_write(&wav_out, out_path, sample_rate);
    tlcs_wav_write(&wav_out, pcm_out, n_frames * FRAME_SIZE);
    tlcs_wav_close(&wav_out);
    fprintf(stderr, "Output: %s (%d frames, neural mode)\n", out_path, n_frames);

    tlcs_neural_free(&neural);
    free(pcm_in);
    free(pcm_out);
    return 0;
}
