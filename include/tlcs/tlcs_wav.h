#ifndef TLCS_WAV_H
#define TLCS_WAV_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Minimal WAV I/O — 16-bit PCM mono only. */

typedef struct {
    FILE    *fp;
    int32_t  sample_rate;
    int32_t  num_samples;    /* total samples (for reader); written so far (for writer) */
    int32_t  data_offset;    /* byte offset where PCM data begins */
} tlcs_wav;

/* Open a WAV file for reading. Validates format (must be 16-bit PCM mono). */
tlcs_status tlcs_wav_open_read(tlcs_wav *wav, const char *path);

/* Read up to max_samples into buf. Returns actual samples read via *samples_read. */
tlcs_status tlcs_wav_read(tlcs_wav *wav, int16_t *buf, int32_t max_samples, int32_t *samples_read);

/* Create a new WAV file for writing. */
tlcs_status tlcs_wav_open_write(tlcs_wav *wav, const char *path, int32_t sample_rate);

/* Write samples to WAV file. */
tlcs_status tlcs_wav_write(tlcs_wav *wav, const int16_t *buf, int32_t num_samples);

/* Close WAV file. For writers, this finalizes the header with correct sizes. */
tlcs_status tlcs_wav_close(tlcs_wav *wav);

#ifdef __cplusplus
}
#endif

#endif /* TLCS_WAV_H */
