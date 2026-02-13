#include "tlcs/tlcs.h"
#include "tlcs/tlcs_wav.h"
#include <stdio.h>
#include <stdlib.h>

/*
 * tlcs_enc — encode a WAV file to TLCS bitstream file.
 * Usage: tlcs_enc <input.wav> <output.tlcs> [bitrate]
 *
 * Milestone 0: passthrough (raw PCM wrapped in frame packets).
 */

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: tlcs_enc <input.wav> <output.tlcs> [bitrate]\n");
        return 1;
    }

    const char *in_path  = argv[1];
    const char *out_path = argv[2];
    int32_t bitrate = (argc >= 4) ? atoi(argv[3]) : 8000;

    /* Open input WAV */
    tlcs_wav wav_in;
    if (tlcs_wav_open_read(&wav_in, in_path) != TLCS_OK) {
        fprintf(stderr, "Error: cannot open %s (must be 16-bit mono WAV)\n", in_path);
        return 1;
    }

    fprintf(stderr, "Input: %s, %d Hz, %d samples\n",
            in_path, wav_in.sample_rate, wav_in.num_samples);

    /* Configure codec */
    tlcs_config cfg;
    if (tlcs_config_init(&cfg, wav_in.sample_rate, bitrate) != TLCS_OK) {
        fprintf(stderr, "Error: unsupported sample rate %d or bitrate %d\n",
                wav_in.sample_rate, bitrate);
        tlcs_wav_close(&wav_in);
        return 1;
    }

    /* Init encoder */
    tlcs_encoder enc;
    tlcs_encoder_init(&enc, &cfg);

    /* Open output file */
    FILE *fp_out = fopen(out_path, "wb");
    if (!fp_out) {
        fprintf(stderr, "Error: cannot create %s\n", out_path);
        tlcs_wav_close(&wav_in);
        return 1;
    }

    /* Write a minimal file header: magic + sample_rate + bitrate */
    const uint8_t magic[4] = {'T', 'L', 'C', 'S'};
    fwrite(magic, 1, 4, fp_out);
    uint32_t sr = (uint32_t)wav_in.sample_rate;
    uint32_t br = (uint32_t)bitrate;
    fwrite(&sr, 4, 1, fp_out);
    fwrite(&br, 4, 1, fp_out);

    /* Encode frame by frame */
    int16_t pcm_buf[TLCS_MAX_FRAME_SIZE];
    uint8_t bs_buf[1024];  /* generous for passthrough */
    int32_t total_frames = 0;
    int32_t total_bytes  = 12;  /* header */

    for (;;) {
        int32_t samples_read = 0;
        tlcs_wav_read(&wav_in, pcm_buf, cfg.frame_size, &samples_read);
        if (samples_read <= 0) break;

        /* Zero-pad last frame if short */
        if (samples_read < cfg.frame_size) {
            for (int32_t i = samples_read; i < cfg.frame_size; i++)
                pcm_buf[i] = 0;
        }

        int32_t bytes_written = 0;
        tlcs_status st = tlcs_encode(&enc, pcm_buf, bs_buf, &bytes_written);
        if (st != TLCS_OK) {
            fprintf(stderr, "Encode error at frame %d\n", total_frames);
            break;
        }

        /* Write frame: [2-byte length][payload] */
        uint16_t frame_len = (uint16_t)bytes_written;
        fwrite(&frame_len, 2, 1, fp_out);
        fwrite(bs_buf, 1, (size_t)bytes_written, fp_out);

        total_bytes += 2 + bytes_written;
        total_frames++;
    }

    fclose(fp_out);
    tlcs_wav_close(&wav_in);

    fprintf(stderr, "Encoded %d frames, %d bytes total\n", total_frames, total_bytes);
    return 0;
}
