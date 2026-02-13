#include "tlcs/tlcs_types.h"
#include "tlcs/tlcs_wav.h"
#include <string.h>

/* WAV format chunk IDs */
#define RIFF_ID 0x46464952U  /* "RIFF" */
#define WAVE_ID 0x45564157U  /* "WAVE" */
#define FMT_ID  0x20746D66U  /* "fmt " */
#define DATA_ID 0x61746164U  /* "data" */

#define WAV_PCM_FORMAT 1

/* Read a little-endian uint32 from file. */
static int read_u32_le(FILE *fp, uint32_t *val)
{
    uint8_t b[4];
    if (fread(b, 1, 4, fp) != 4) return 0;
    *val = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return 1;
}

/* Read a little-endian uint16 from file. */
static int read_u16_le(FILE *fp, uint16_t *val)
{
    uint8_t b[2];
    if (fread(b, 1, 2, fp) != 2) return 0;
    *val = (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
    return 1;
}

/* Write a little-endian uint32 to file. */
static int write_u32_le(FILE *fp, uint32_t val)
{
    uint8_t b[4];
    b[0] = (uint8_t)(val);
    b[1] = (uint8_t)(val >> 8);
    b[2] = (uint8_t)(val >> 16);
    b[3] = (uint8_t)(val >> 24);
    return fwrite(b, 1, 4, fp) == 4;
}

/* Write a little-endian uint16 to file. */
static int write_u16_le(FILE *fp, uint16_t val)
{
    uint8_t b[2];
    b[0] = (uint8_t)(val);
    b[1] = (uint8_t)(val >> 8);
    return fwrite(b, 1, 2, fp) == 2;
}

tlcs_status tlcs_wav_open_read(tlcs_wav *wav, const char *path)
{
    if (!wav || !path) return TLCS_ERR_INVALID_ARG;
    memset(wav, 0, sizeof(*wav));

    wav->fp = fopen(path, "rb");
    if (!wav->fp) return TLCS_ERR_IO;

    uint32_t chunk_id, chunk_size, format;
    if (!read_u32_le(wav->fp, &chunk_id) || chunk_id != RIFF_ID) goto fail;
    if (!read_u32_le(wav->fp, &chunk_size)) goto fail;
    if (!read_u32_le(wav->fp, &format) || format != WAVE_ID) goto fail;

    /* Find fmt chunk */
    uint32_t sub_id, sub_size;
    int found_fmt = 0, found_data = 0;

    while (!found_data) {
        if (!read_u32_le(wav->fp, &sub_id)) goto fail;
        if (!read_u32_le(wav->fp, &sub_size)) goto fail;

        if (sub_id == FMT_ID) {
            uint16_t audio_format, num_channels, bits_per_sample, block_align;
            uint32_t sample_rate, byte_rate;

            if (!read_u16_le(wav->fp, &audio_format)) goto fail;
            if (audio_format != WAV_PCM_FORMAT) goto fail;

            if (!read_u16_le(wav->fp, &num_channels)) goto fail;
            if (num_channels != 1) goto fail;  /* mono only */

            if (!read_u32_le(wav->fp, &sample_rate)) goto fail;
            if (!read_u32_le(wav->fp, &byte_rate)) goto fail;
            (void)byte_rate;
            if (!read_u16_le(wav->fp, &block_align)) goto fail;
            (void)block_align;
            if (!read_u16_le(wav->fp, &bits_per_sample)) goto fail;
            if (bits_per_sample != 16) goto fail;

            wav->sample_rate = (int32_t)sample_rate;

            /* Skip any extra fmt bytes */
            long extra = (long)sub_size - 16;
            if (extra > 0) fseek(wav->fp, extra, SEEK_CUR);
            found_fmt = 1;

        } else if (sub_id == DATA_ID) {
            if (!found_fmt) goto fail;
            wav->num_samples = (int32_t)(sub_size / 2);  /* 16-bit = 2 bytes/sample */
            wav->data_offset = (int32_t)ftell(wav->fp);
            found_data = 1;

        } else {
            /* Skip unknown chunk */
            fseek(wav->fp, (long)sub_size, SEEK_CUR);
        }
    }

    return TLCS_OK;

fail:
    if (wav->fp) { fclose(wav->fp); wav->fp = NULL; }
    return TLCS_ERR_IO;
}

tlcs_status tlcs_wav_read(tlcs_wav *wav, int16_t *buf, int32_t max_samples, int32_t *samples_read)
{
    if (!wav || !wav->fp || !buf || !samples_read) return TLCS_ERR_INVALID_ARG;

    size_t n = fread(buf, sizeof(int16_t), (size_t)max_samples, wav->fp);
    *samples_read = (int32_t)n;

    /* WAV is always little-endian. On big-endian platforms we'd need to swap.
     * For now, assume little-endian host (x86, ARM). */
    return TLCS_OK;
}

tlcs_status tlcs_wav_open_write(tlcs_wav *wav, const char *path, int32_t sample_rate)
{
    if (!wav || !path) return TLCS_ERR_INVALID_ARG;
    memset(wav, 0, sizeof(*wav));

    wav->fp = fopen(path, "wb");
    if (!wav->fp) return TLCS_ERR_IO;

    wav->sample_rate = sample_rate;
    wav->num_samples = 0;

    /* Write placeholder header — finalized on close */
    uint32_t data_size = 0;
    uint32_t file_size = 36 + data_size;
    uint32_t byte_rate = (uint32_t)sample_rate * 2;

    if (!write_u32_le(wav->fp, RIFF_ID)) goto fail;
    if (!write_u32_le(wav->fp, file_size)) goto fail;
    if (!write_u32_le(wav->fp, WAVE_ID)) goto fail;

    /* fmt chunk */
    if (!write_u32_le(wav->fp, FMT_ID)) goto fail;
    if (!write_u32_le(wav->fp, 16)) goto fail;           /* chunk size */
    if (!write_u16_le(wav->fp, WAV_PCM_FORMAT)) goto fail;
    if (!write_u16_le(wav->fp, 1)) goto fail;             /* mono */
    if (!write_u32_le(wav->fp, (uint32_t)sample_rate)) goto fail;
    if (!write_u32_le(wav->fp, byte_rate)) goto fail;
    if (!write_u16_le(wav->fp, 2)) goto fail;             /* block align */
    if (!write_u16_le(wav->fp, 16)) goto fail;            /* bits per sample */

    /* data chunk header */
    if (!write_u32_le(wav->fp, DATA_ID)) goto fail;
    if (!write_u32_le(wav->fp, data_size)) goto fail;     /* placeholder */

    wav->data_offset = (int32_t)ftell(wav->fp);
    return TLCS_OK;

fail:
    if (wav->fp) { fclose(wav->fp); wav->fp = NULL; }
    return TLCS_ERR_IO;
}

tlcs_status tlcs_wav_write(tlcs_wav *wav, const int16_t *buf, int32_t num_samples)
{
    if (!wav || !wav->fp || !buf) return TLCS_ERR_INVALID_ARG;

    size_t written = fwrite(buf, sizeof(int16_t), (size_t)num_samples, wav->fp);
    wav->num_samples += (int32_t)written;

    return ((int32_t)written == num_samples) ? TLCS_OK : TLCS_ERR_IO;
}

tlcs_status tlcs_wav_close(tlcs_wav *wav)
{
    if (!wav) return TLCS_ERR_INVALID_ARG;
    if (!wav->fp) return TLCS_OK;

    /* If this was a writer, patch the header with final sizes */
    if (wav->data_offset > 0 && wav->num_samples > 0) {
        uint32_t data_size = (uint32_t)wav->num_samples * 2;
        uint32_t file_size = 36 + data_size;

        /* Patch RIFF chunk size (offset 4) */
        fseek(wav->fp, 4, SEEK_SET);
        write_u32_le(wav->fp, file_size);

        /* Patch data chunk size (offset 40) */
        fseek(wav->fp, 40, SEEK_SET);
        write_u32_le(wav->fp, data_size);
    }

    fclose(wav->fp);
    wav->fp = NULL;
    return TLCS_OK;
}
