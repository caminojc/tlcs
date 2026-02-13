#include "tlcs/tlcs.h"
#include "tlcs/tlcs_wav.h"
#include "../src/codec/tlcs_qmf.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: qmf_test <input.wav> <output.wav>\n");
        return 1;
    }

    tlcs_wav reader;
    if (tlcs_wav_open_read(&reader, argv[1]) != TLCS_OK) {
        fprintf(stderr, "Failed to open %s\n", argv[1]);
        return 1;
    }

    int32_t n_samples = reader.num_samples;
    int32_t n_frames = n_samples / 160;

    tlcs_wav writer;
    if (tlcs_wav_open_write(&writer, argv[2], 16000) != TLCS_OK) {
        fprintf(stderr, "Failed to open %s\n", argv[2]);
        tlcs_wav_close(&reader);
        return 1;
    }

    float ana_mem[24] = {0};
    float lb_mem[12] = {0};
    float hb_mem[12] = {0};

    int16_t pcm_in[160];
    int16_t pcm_out[160];

    double total_sig = 0, total_noise = 0;
    int32_t total_samples = 0;

    for (int32_t f = 0; f < n_frames; f++) {
        int32_t nread = 0;
        tlcs_wav_read(&reader, pcm_in, 160, &nread);

        float in_f[160];
        for (int i = 0; i < 160; i++)
            in_f[i] = (float)pcm_in[i];

        float lb[80], hb[80];
        tlcs_qmf_analyze(ana_mem, in_f, 160, lb, hb);

        float out_f[160];
        tlcs_qmf_synthesize(lb_mem, hb_mem, lb, hb, out_f, 80);

        /* Print first frame's first 20 samples for diagnosis */
        if (f == 100) {
            fprintf(stderr, "Frame %d samples (in -> out):\n", f);
            for (int i = 0; i < 20; i++)
                fprintf(stderr, "  [%d] %.1f -> %.1f (err=%.1f)\n",
                        i, in_f[i], out_f[i], in_f[i] - out_f[i]);
            fprintf(stderr, "LB[0..5]: ");
            for (int i = 0; i < 6; i++)
                fprintf(stderr, "%.1f ", lb[i]);
            fprintf(stderr, "\nHB[0..5]: ");
            for (int i = 0; i < 6; i++)
                fprintf(stderr, "%.1f ", hb[i]);
            fprintf(stderr, "\n");
        }

        for (int i = 0; i < 160; i++) {
            float v = out_f[i];
            if (v > 32767.0f) v = 32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            pcm_out[i] = (int16_t)v;

            double d = (double)pcm_in[i] - (double)pcm_out[i];
            total_sig += (double)pcm_in[i] * (double)pcm_in[i];
            total_noise += d * d;
        }
        total_samples += 160;

        tlcs_wav_write(&writer, pcm_out, 160);
    }

    double snr = 10.0 * log10(total_sig / total_noise);
    fprintf(stderr, "QMF roundtrip: %d frames (%d samples), SNR = %.1f dB\n",
            n_frames, total_samples, snr);

    /* Try with time-aligned comparison (skip first 24 output samples) */
    tlcs_wav_close(&reader);
    tlcs_wav_close(&writer);
    return 0;
}
