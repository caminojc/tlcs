#include <stdio.h>
#include <string.h>
#include "tlcs/tlcs.h"
#include "tlcs/tlcs_wav.h"
#include "../src/codec/tlcs_qmf.h"

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "Usage: %s <in.wav> <out.wav>\n", argv[0]); return 1; }

    tlcs_wav rd;
    if (tlcs_wav_open_read(&rd, argv[1]) != TLCS_OK) { fprintf(stderr, "Can't open %s\n", argv[1]); return 1; }

    tlcs_wav wr;
    tlcs_wav_open_write(&wr, argv[2], rd.sample_rate);

    float ana_mem[TLCS_QMF_TAPS_MAX] = {0};
    float lb_mem[TLCS_QMF_HALF_MAX] = {0};
    float hb_mem[TLCS_QMF_HALF_MAX] = {0};

    int16_t pcm_in[160], pcm_out[160];
    float input_f[160], lb[80], hb[80], output_f[160];

    int32_t frames = rd.num_samples / 160;
    for (int32_t f = 0; f < frames; f++) {
        int32_t got;
        tlcs_wav_read(&rd, pcm_in, 160, &got);
        if (got < 160) break;

        for (int i = 0; i < 160; i++) input_f[i] = (float)pcm_in[i];

        tlcs_qmf_analyze(ana_mem, input_f, 160, lb, hb);
        tlcs_qmf_synthesize(lb_mem, hb_mem, lb, hb, output_f, 80);

        for (int i = 0; i < 160; i++) {
            float v = output_f[i];
            if (v > 32767.0f) v = 32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            pcm_out[i] = (int16_t)v;
        }
        tlcs_wav_write(&wr, pcm_out, 160);
    }

    tlcs_wav_close(&rd);
    tlcs_wav_close(&wr);
    fprintf(stderr, "QMF roundtrip: %d frames\n", frames);
    return 0;
}
