#include "tlcs/tlcs.h"
#include "tlcs/tlcs_wav.h"
#include <stdio.h>
#include <stdlib.h>

/*
 * tlcs_dec — decode a TLCS bitstream file to WAV.
 * Usage: tlcs_dec <input.tlcs> <output.wav>
 */

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: tlcs_dec <input.tlcs> <output.wav>\n");
        return 1;
    }

    const char *in_path  = argv[1];
    const char *out_path = argv[2];

    /* Open input bitstream */
    FILE *fp_in = fopen(in_path, "rb");
    if (!fp_in) {
        fprintf(stderr, "Error: cannot open %s\n", in_path);
        return 1;
    }

    /* Read file header */
    uint8_t magic[4];
    uint32_t sr, br;
    if (fread(magic, 1, 4, fp_in) != 4 ||
        magic[0] != 'T' || magic[1] != 'L' || magic[2] != 'C' || magic[3] != 'S') {
        fprintf(stderr, "Error: %s is not a TLCS file\n", in_path);
        fclose(fp_in);
        return 1;
    }
    if (fread(&sr, 4, 1, fp_in) != 1 || fread(&br, 4, 1, fp_in) != 1) {
        fprintf(stderr, "Error: truncated header\n");
        fclose(fp_in);
        return 1;
    }

    fprintf(stderr, "Input: %s, %u Hz, %u bps\n", in_path, sr, br);

    /* Configure codec */
    tlcs_config cfg;
    if (tlcs_config_init(&cfg, (int32_t)sr, (int32_t)br) != TLCS_OK) {
        fprintf(stderr, "Error: unsupported params\n");
        fclose(fp_in);
        return 1;
    }

    /* Init decoder */
    tlcs_decoder dec;
    tlcs_decoder_init(&dec, &cfg);

    /* Open output WAV */
    tlcs_wav wav_out;
    if (tlcs_wav_open_write(&wav_out, out_path, (int32_t)sr) != TLCS_OK) {
        fprintf(stderr, "Error: cannot create %s\n", out_path);
        fclose(fp_in);
        return 1;
    }

    /* Decode frame by frame */
    uint8_t bs_buf[1024];
    int16_t pcm_buf[TLCS_MAX_FRAME_SIZE];
    int32_t total_frames = 0;

    for (;;) {
        uint16_t frame_len = 0;
        if (fread(&frame_len, 2, 1, fp_in) != 1) break;  /* EOF */
        if (frame_len == 0 || frame_len > sizeof(bs_buf)) break;

        if (fread(bs_buf, 1, frame_len, fp_in) != frame_len) {
            fprintf(stderr, "Warning: truncated frame at %d\n", total_frames);
            break;
        }

        tlcs_status st = tlcs_decode(&dec, bs_buf, (int32_t)frame_len, pcm_buf);
        if (st != TLCS_OK) {
            fprintf(stderr, "Decode error at frame %d, using PLC\n", total_frames);
            tlcs_decode_plc(&dec, pcm_buf);
        }

        tlcs_wav_write(&wav_out, pcm_buf, cfg.frame_size);
        total_frames++;
    }

    fclose(fp_in);
    tlcs_wav_close(&wav_out);

    fprintf(stderr, "Decoded %d frames\n", total_frames);
    return 0;
}
