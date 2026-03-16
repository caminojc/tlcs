/*
 * tlcs_demo.c — CLI demo: encode + decode a 16-bit PCM WAV file.
 *
 * Reads a WAV file (16-bit mono PCM, 16 kHz), encodes frame-by-frame,
 * decodes frame-by-frame, writes output WAV, and prints SNR + bitrate.
 *
 * Usage: ./tlcs_demo input.wav output.wav [bitrate]
 *
 * No library dependencies beyond libc and libm.
 */
#include "tlcs_config.h"
#include "tlcs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

/* ================================================================== */
/* Minimal WAV reader/writer (16-bit PCM only)                         */
/* ================================================================== */

typedef struct {
    int      sample_rate;
    int      num_channels;
    int      bits_per_sample;
    int32_t  num_samples;
    int16_t *data;
} WavFile;

static int wav_read(const char *path, WavFile *wav)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open '%s'\n", path);
        return -1;
    }

    /* RIFF header */
    char riff[4];
    uint32_t file_size;
    char wave[4];
    if (fread(riff, 1, 4, f) != 4 || memcmp(riff, "RIFF", 4) != 0) goto fail;
    if (fread(&file_size, 4, 1, f) != 1) goto fail;
    if (fread(wave, 1, 4, f) != 4 || memcmp(wave, "WAVE", 4) != 0) goto fail;

    /* Find fmt chunk */
    uint16_t audio_format = 0;
    uint16_t num_channels = 0;
    uint32_t sample_rate = 0;
    uint16_t bits_per_sample = 0;
    int found_fmt = 0, found_data = 0;
    uint32_t data_size = 0;

    while (!found_data) {
        char chunk_id[4];
        uint32_t chunk_size;
        if (fread(chunk_id, 1, 4, f) != 4) break;
        if (fread(&chunk_size, 4, 1, f) != 1) break;

        if (memcmp(chunk_id, "fmt ", 4) == 0) {
            if (chunk_size < 16) goto fail;
            if (fread(&audio_format, 2, 1, f) != 1) goto fail;
            if (fread(&num_channels, 2, 1, f) != 1) goto fail;
            if (fread(&sample_rate, 4, 1, f) != 1) goto fail;
            /* Skip byte rate + block align */
            uint32_t byte_rate;
            uint16_t block_align;
            if (fread(&byte_rate, 4, 1, f) != 1) goto fail;
            if (fread(&block_align, 2, 1, f) != 1) goto fail;
            if (fread(&bits_per_sample, 2, 1, f) != 1) goto fail;
            /* Skip extra format bytes */
            if (chunk_size > 16) {
                fseek(f, chunk_size - 16, SEEK_CUR);
            }
            found_fmt = 1;
        } else if (memcmp(chunk_id, "data", 4) == 0) {
            data_size = chunk_size;
            found_data = 1;
        } else {
            /* Skip unknown chunk */
            fseek(f, chunk_size, SEEK_CUR);
        }
    }

    if (!found_fmt || !found_data) goto fail;
    if (audio_format != 1) {
        fprintf(stderr, "Error: only PCM WAV supported (got format %d)\n", audio_format);
        goto fail;
    }
    if (bits_per_sample != 16) {
        fprintf(stderr, "Error: only 16-bit WAV supported (got %d)\n", bits_per_sample);
        goto fail;
    }

    wav->sample_rate = (int)sample_rate;
    wav->num_channels = (int)num_channels;
    wav->bits_per_sample = (int)bits_per_sample;
    wav->num_samples = (int32_t)(data_size / (num_channels * 2));

    wav->data = (int16_t*)malloc(data_size);
    if (!wav->data) goto fail;

    size_t read = fread(wav->data, 1, data_size, f);
    if ((uint32_t)read < data_size) {
        /* Partial read is OK, adjust num_samples */
        wav->num_samples = (int32_t)(read / (num_channels * 2));
    }

    fclose(f);
    return 0;

fail:
    fclose(f);
    return -1;
}

static int wav_write(const char *path, const int16_t *data, int num_samples,
                     int sample_rate, int num_channels)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "Error: cannot create '%s'\n", path);
        return -1;
    }

    uint32_t data_size = (uint32_t)(num_samples * num_channels * 2);
    uint32_t file_size = 36 + data_size;
    uint32_t byte_rate = (uint32_t)(sample_rate * num_channels * 2);
    uint16_t block_align = (uint16_t)(num_channels * 2);

    /* RIFF header */
    fwrite("RIFF", 1, 4, f);
    fwrite(&file_size, 4, 1, f);
    fwrite("WAVE", 1, 4, f);

    /* fmt chunk */
    fwrite("fmt ", 1, 4, f);
    uint32_t fmt_size = 16;
    fwrite(&fmt_size, 4, 1, f);
    uint16_t audio_format = 1;
    uint16_t nc = (uint16_t)num_channels;
    uint32_t sr = (uint32_t)sample_rate;
    uint16_t bps = 16;
    fwrite(&audio_format, 2, 1, f);
    fwrite(&nc, 2, 1, f);
    fwrite(&sr, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&block_align, 2, 1, f);
    fwrite(&bps, 2, 1, f);

    /* data chunk */
    fwrite("data", 1, 4, f);
    fwrite(&data_size, 4, 1, f);
    fwrite(data, 2, (size_t)(num_samples * num_channels), f);

    fclose(f);
    return 0;
}

static void wav_free(WavFile *wav)
{
    free(wav->data);
    wav->data = NULL;
}

/* ================================================================== */
/* Main                                                                */
/* ================================================================== */

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s input.wav output.wav [bitrate]\n", argv[0]);
        fprintf(stderr, "  Default bitrate: %d bps\n", TLCS_BITRATE_BPS);
        return 1;
    }

    const char *input_path = argv[1];
    const char *output_path = argv[2];
    int bitrate = TLCS_BITRATE_BPS;
    if (argc >= 4) {
        bitrate = atoi(argv[3]);
        if (bitrate <= 0) bitrate = TLCS_BITRATE_BPS;
    }

    /* ---- Read input WAV ---- */
    WavFile wav;
    memset(&wav, 0, sizeof(wav));
    if (wav_read(input_path, &wav) != 0) {
        return 1;
    }

    printf("Input:  %s\n", input_path);
    printf("  Sample rate:  %d Hz\n", wav.sample_rate);
    printf("  Channels:     %d\n", wav.num_channels);
    printf("  Samples:      %d\n", (int)wav.num_samples);
    printf("  Duration:     %.2f s\n", (float)wav.num_samples / (float)wav.sample_rate);

    if (wav.sample_rate != TLCS_SAMPLE_RATE) {
        fprintf(stderr, "Error: codec requires %d Hz, got %d Hz\n",
                TLCS_SAMPLE_RATE, wav.sample_rate);
        wav_free(&wav);
        return 1;
    }

    /* Extract mono channel */
    int total_samples = (int)wav.num_samples;
    int16_t *mono = wav.data;
    int16_t *mono_alloc = NULL;
    if (wav.num_channels > 1) {
        mono_alloc = (int16_t*)malloc(total_samples * sizeof(int16_t));
        if (!mono_alloc) { wav_free(&wav); return 1; }
        for (int i = 0; i < total_samples; i++) {
            mono_alloc[i] = wav.data[i * wav.num_channels];
        }
        mono = mono_alloc;
    }

    /* Pad to frame boundary */
    int N = TLCS_FRAME_SIZE;
    int pad = (N - (total_samples % N)) % N;
    int padded_samples = total_samples + pad;
    int16_t *padded = (int16_t*)calloc(padded_samples, sizeof(int16_t));
    if (!padded) { free(mono_alloc); wav_free(&wav); return 1; }
    memcpy(padded, mono, total_samples * sizeof(int16_t));

    int num_frames = padded_samples / N;

    /* ---- Create encoder and decoder ---- */
    TlcsEncoder *enc = tlcs_encoder_create(TLCS_SAMPLE_RATE, bitrate);
    TlcsDecoder *dec = tlcs_decoder_create(TLCS_SAMPLE_RATE);
    if (!enc || !dec) {
        fprintf(stderr, "Error: failed to create codec\n");
        free(padded); free(mono_alloc); wav_free(&wav);
        return 1;
    }

    /* ---- Encode + decode frame by frame ---- */
    int16_t *output = (int16_t*)calloc(padded_samples, sizeof(int16_t));
    if (!output) {
        tlcs_encoder_destroy(enc); tlcs_decoder_destroy(dec);
        free(padded); free(mono_alloc); wav_free(&wav);
        return 1;
    }

    int is_5k = TLCS_IS_5K(bitrate);
    int bytes_per_frame = is_5k ? TLCS_5K_BYTES_PER_FRAME : TLCS_BYTES_PER_FRAME;
    uint8_t frame_buf[TLCS_BYTES_PER_FRAME + 4]; /* large enough for both modes */
    int total_bytes = 0;

    printf("\nEncoding %d frames (%d bytes/frame, %s mode)...\n",
           num_frames, bytes_per_frame, is_5k ? "5k" : "8k");

    for (int i = 0; i < num_frames; i++) {
        const int16_t *in_frame = &padded[i * N];
        int16_t *out_frame = &output[i * N];

        /* Encode */
        int enc_bytes = tlcs_encode(enc, in_frame, frame_buf, sizeof(frame_buf));
        if (enc_bytes < 0) {
            fprintf(stderr, "Encode error at frame %d: %d\n", i, enc_bytes);
            break;
        }
        total_bytes += enc_bytes;

        /* Decode */
        int dec_samples = tlcs_decode(dec, frame_buf, enc_bytes, out_frame);
        if (dec_samples < 0) {
            fprintf(stderr, "Decode error at frame %d: %d\n", i, dec_samples);
            break;
        }

        /* Progress */
        if ((i + 1) % 50 == 0 || i == num_frames - 1) {
            printf("  Frame %d/%d\r", i + 1, num_frames);
            fflush(stdout);
        }
    }
    printf("\n");

    /* ---- Write output WAV ---- */
    if (wav_write(output_path, output, total_samples, TLCS_SAMPLE_RATE, 1) != 0) {
        fprintf(stderr, "Error: failed to write output\n");
    }

    /* ---- Compute SNR ---- */
    double sig_power = 0.0;
    double noise_power = 0.0;
    for (int i = 0; i < total_samples; i++) {
        double s = (double)padded[i];
        double n = (double)(padded[i] - output[i]);
        sig_power += s * s;
        noise_power += n * n;
    }
    double snr_db = -999.0;
    if (noise_power > 0.0) {
        snr_db = 10.0 * log10(sig_power / noise_power);
    }

    float actual_bitrate = (float)total_bytes * 8.0f /
                           ((float)total_samples / (float)TLCS_SAMPLE_RATE);

    printf("\nOutput: %s\n", output_path);
    printf("  Frames:       %d\n", num_frames);
    printf("  Total bytes:  %d\n", total_bytes);
    printf("  Bitrate:      %.1f bps\n", actual_bitrate);
    printf("  SNR:          %.2f dB\n", snr_db);

    /* ---- Cleanup ---- */
    tlcs_encoder_destroy(enc);
    tlcs_decoder_destroy(dec);
    free(output);
    free(padded);
    free(mono_alloc);
    wav_free(&wav);

    return 0;
}
