/*
 * tlcs_bitstream.c — Bit packing and unpacking.
 *
 * BitWriter: accumulates bits MSB-first into a byte buffer.
 * BitReader: reads bits MSB-first from a byte buffer.
 *
 * Frame packing (160 bits = 20 bytes):
 *   LSP: 4 x 8 = 32 bits
 *   SF0: full_lag(7) + frac(1) + pgain(3) + fcb_lo(24) + fcb_hi(24) + gain(4) = 63 bits
 *   SF1: delta_lag(4) + frac(1) + pgain(3) + fcb_lo(24) + fcb_hi(24) + gain(4) = 60 bits
 *   Spare: 5 bits (written as zero)
 *   Total: 32 + 63 + 60 + 5 = 160 bits = 20 bytes
 */
#include "tlcs_config.h"
#include "tlcs_bitstream.h"
#include "tlcs.h"

#include <string.h>

/* ================================================================== */
/* Bit Writer                                                          */
/* ================================================================== */

void tlcs_bw_init(TlcsBitWriter *bw, uint8_t *buf, int capacity)
{
    bw->buf = buf;
    bw->capacity = capacity;
    bw->bit_pos = 0;
    memset(buf, 0, capacity);
}

void tlcs_bw_write(TlcsBitWriter *bw, int value, int num_bits)
{
    /* Write bits MSB first */
    for (int i = num_bits - 1; i >= 0; i--) {
        int bit = (value >> i) & 1;
        int byte_idx = bw->bit_pos / 8;
        int bit_idx = 7 - (bw->bit_pos % 8);

        if (byte_idx < bw->capacity) {
            if (bit) {
                bw->buf[byte_idx] |= (uint8_t)(1 << bit_idx);
            }
        }
        bw->bit_pos++;
    }
}

int tlcs_bw_bytes_written(const TlcsBitWriter *bw)
{
    return (bw->bit_pos + 7) / 8;
}

/* ================================================================== */
/* Bit Reader                                                          */
/* ================================================================== */

void tlcs_br_init(TlcsBitReader *br, const uint8_t *buf, int size)
{
    br->buf = buf;
    br->size = size;
    br->bit_pos = 0;
}

int tlcs_br_read(TlcsBitReader *br, int num_bits)
{
    int value = 0;
    for (int i = 0; i < num_bits; i++) {
        int byte_idx = br->bit_pos / 8;
        int bit_idx = 7 - (br->bit_pos % 8);

        value <<= 1;
        if (byte_idx < br->size) {
            if (br->buf[byte_idx] & (1 << bit_idx)) {
                value |= 1;
            }
        }
        br->bit_pos++;
    }
    return value;
}

/* ================================================================== */
/* Frame packing                                                       */
/* ================================================================== */

/*
 * Bit layout per frame (160 bits = 20 bytes):
 *
 *   LSP: 4 splits x 8 bits = 32 bits
 *   SF0: full_lag(7) + frac(1) + pgain(3) + fcb_lo(24) + fcb_hi(24) + gain(4) = 63 bits
 *   SF1: delta_lag(4) + frac(1) + pgain(3) + fcb_lo(24) + fcb_hi(24) + gain(4) = 60 bits
 *   Spare: 5 bits (zero)
 *   Total: 32 + 63 + 60 + 5 = 160 bits = 20 bytes
 */

int tlcs_frame_pack(const TlcsFrameData *fd, uint8_t *buf, int buf_size)
{
    if (buf_size < TLCS_BYTES_PER_FRAME) return TLCS_ERR_ARGS;

    TlcsBitWriter bw;
    tlcs_bw_init(&bw, buf, buf_size);

    /* LSP indices: 4 x 8 bits = 32 */
    for (int s = 0; s < TLCS_LSP_NUM_SPLITS; s++) {
        tlcs_bw_write(&bw, fd->lsp_indices[s], TLCS_LSP_CB_BITS);
    }

    /* SF0: full lag(7) + frac(1) + pgain(3) + fcb_lo(24) + fcb_hi(24) + gain(4) = 63 */
    {
        const TlcsSubframeData *sd = &fd->sf[0];
        tlcs_bw_write(&bw, sd->pitch_lag_idx,  TLCS_PITCH_LAG_BITS);   /* 7 */
        tlcs_bw_write(&bw, sd->pitch_frac_idx, TLCS_PITCH_FRAC_BITS);  /* 1 */
        tlcs_bw_write(&bw, sd->pitch_gain_idx, TLCS_PITCH_GAIN_BITS);  /* 3 */
        tlcs_bw_write(&bw, sd->fcb_index_lo, 24);                      /* 24 */
        tlcs_bw_write(&bw, sd->fcb_index_hi, 24);                      /* 24 */
        tlcs_bw_write(&bw, sd->gain_index,   TLCS_FCB_GAIN_BITS);      /* 4 */
    }

    /* SF1: delta_lag(4) + frac(1) + pgain(3) + fcb_lo(24) + fcb_hi(24) + gain(4) = 60 */
    {
        const TlcsSubframeData *sd = &fd->sf[1];
        tlcs_bw_write(&bw, sd->pitch_lag_idx,  TLCS_PITCH_DELTA_BITS); /* 4 */
        tlcs_bw_write(&bw, sd->pitch_frac_idx, TLCS_PITCH_FRAC_BITS);  /* 1 */
        tlcs_bw_write(&bw, sd->pitch_gain_idx, TLCS_PITCH_GAIN_BITS);  /* 3 */
        tlcs_bw_write(&bw, sd->fcb_index_lo, 24);                      /* 24 */
        tlcs_bw_write(&bw, sd->fcb_index_hi, 24);                      /* 24 */
        tlcs_bw_write(&bw, sd->gain_index,   TLCS_FCB_GAIN_BITS);      /* 4 */
    }

    /* Spare: 5 bits zero */
    tlcs_bw_write(&bw, 0, TLCS_SPARE_BITS);

    return tlcs_bw_bytes_written(&bw);
}

int tlcs_frame_unpack(const uint8_t *buf, int buf_size, TlcsFrameData *fd)
{
    if (buf_size < TLCS_BYTES_PER_FRAME) return TLCS_ERR_STREAM;

    TlcsBitReader br;
    tlcs_br_init(&br, buf, buf_size);

    /* LSP indices */
    for (int s = 0; s < TLCS_LSP_NUM_SPLITS; s++) {
        fd->lsp_indices[s] = tlcs_br_read(&br, TLCS_LSP_CB_BITS);
    }

    /* SF0: full lag(7) + frac(1) + pgain(3) + fcb(48) + gain(4) */
    {
        TlcsSubframeData *sd = &fd->sf[0];
        sd->pitch_lag_idx  = tlcs_br_read(&br, TLCS_PITCH_LAG_BITS);
        sd->pitch_frac_idx = tlcs_br_read(&br, TLCS_PITCH_FRAC_BITS);
        sd->pitch_gain_idx = tlcs_br_read(&br, TLCS_PITCH_GAIN_BITS);
        sd->fcb_index_lo   = tlcs_br_read(&br, 24);
        sd->fcb_index_hi   = tlcs_br_read(&br, 24);
        sd->gain_index     = tlcs_br_read(&br, TLCS_FCB_GAIN_BITS);
    }

    /* SF1: delta_lag(4) + frac(1) + pgain(3) + fcb(48) + gain(4) */
    {
        TlcsSubframeData *sd = &fd->sf[1];
        sd->pitch_lag_idx  = tlcs_br_read(&br, TLCS_PITCH_DELTA_BITS);
        sd->pitch_frac_idx = tlcs_br_read(&br, TLCS_PITCH_FRAC_BITS);
        sd->pitch_gain_idx = tlcs_br_read(&br, TLCS_PITCH_GAIN_BITS);
        sd->fcb_index_lo   = tlcs_br_read(&br, 24);
        sd->fcb_index_hi   = tlcs_br_read(&br, 24);
        sd->gain_index     = tlcs_br_read(&br, TLCS_FCB_GAIN_BITS);
    }

    /* Skip spare bits (don't need to read) */

    return TLCS_OK;
}

/* ================================================================== */
/* Mode-aware frame packing (5k or 8k)                                 */
/* ================================================================== */

/*
 * 5k bit layout (100 bits = 13 bytes):
 *   LSP: 4 splits x 6 bits = 24 bits
 *   SF0: full_lag(7) + frac(1) + pgain(3) + fcb_lo(24) + gain(4) = 39 bits
 *   SF1: delta_lag(4) + frac(1) + pgain(3) + fcb_lo(24) + gain(4) = 36 bits
 *   Spare: 1 bit (zero)
 *   Total: 24 + 39 + 36 + 1 = 100 bits = 13 bytes
 *
 * 8k bit layout (160 bits = 20 bytes): same as original
 */

int tlcs_frame_pack_mode(const TlcsFrameData *fd, uint8_t *buf, int buf_size, int is_5k)
{
    if (!is_5k) return tlcs_frame_pack(fd, buf, buf_size);

    int bytes_needed = TLCS_5K_BYTES_PER_FRAME;
    if (buf_size < bytes_needed) return TLCS_ERR_ARGS;

    TlcsBitWriter bw;
    tlcs_bw_init(&bw, buf, buf_size);

    /* LSP indices: 4 x 6 bits = 24 */
    for (int s = 0; s < TLCS_LSP_NUM_SPLITS; s++) {
        tlcs_bw_write(&bw, fd->lsp_indices[s], TLCS_5K_LSP_CB_BITS);
    }

    /* SF0: full lag(7) + frac(1) + pgain(3) + fcb_lo(24) + gain(4) = 39 */
    {
        const TlcsSubframeData *sd = &fd->sf[0];
        tlcs_bw_write(&bw, sd->pitch_lag_idx,  TLCS_PITCH_LAG_BITS);   /* 7 */
        tlcs_bw_write(&bw, sd->pitch_frac_idx, TLCS_PITCH_FRAC_BITS);  /* 1 */
        tlcs_bw_write(&bw, sd->pitch_gain_idx, TLCS_PITCH_GAIN_BITS);  /* 3 */
        tlcs_bw_write(&bw, sd->fcb_index_lo, 24);                      /* 24 */
        tlcs_bw_write(&bw, sd->gain_index,   TLCS_FCB_GAIN_BITS);      /* 4 */
    }

    /* SF1: delta_lag(4) + frac(1) + pgain(3) + fcb_lo(24) + gain(4) = 36 */
    {
        const TlcsSubframeData *sd = &fd->sf[1];
        tlcs_bw_write(&bw, sd->pitch_lag_idx,  TLCS_PITCH_DELTA_BITS); /* 4 */
        tlcs_bw_write(&bw, sd->pitch_frac_idx, TLCS_PITCH_FRAC_BITS);  /* 1 */
        tlcs_bw_write(&bw, sd->pitch_gain_idx, TLCS_PITCH_GAIN_BITS);  /* 3 */
        tlcs_bw_write(&bw, sd->fcb_index_lo, 24);                      /* 24 */
        tlcs_bw_write(&bw, sd->gain_index,   TLCS_FCB_GAIN_BITS);      /* 4 */
    }

    /* Spare: 1 bit zero */
    tlcs_bw_write(&bw, 0, TLCS_5K_SPARE_BITS);

    return tlcs_bw_bytes_written(&bw);
}

int tlcs_frame_unpack_mode(const uint8_t *buf, int buf_size, TlcsFrameData *fd, int is_5k)
{
    if (!is_5k) return tlcs_frame_unpack(buf, buf_size, fd);

    if (buf_size < TLCS_5K_BYTES_PER_FRAME) return TLCS_ERR_STREAM;

    TlcsBitReader br;
    tlcs_br_init(&br, buf, buf_size);

    /* LSP indices: 4 x 6 bits */
    for (int s = 0; s < TLCS_LSP_NUM_SPLITS; s++) {
        fd->lsp_indices[s] = tlcs_br_read(&br, TLCS_5K_LSP_CB_BITS);
    }

    /* SF0: full lag(7) + frac(1) + pgain(3) + fcb_lo(24) + gain(4) */
    {
        TlcsSubframeData *sd = &fd->sf[0];
        sd->pitch_lag_idx  = tlcs_br_read(&br, TLCS_PITCH_LAG_BITS);
        sd->pitch_frac_idx = tlcs_br_read(&br, TLCS_PITCH_FRAC_BITS);
        sd->pitch_gain_idx = tlcs_br_read(&br, TLCS_PITCH_GAIN_BITS);
        sd->fcb_index_lo   = tlcs_br_read(&br, 24);
        sd->fcb_index_hi   = 0;  /* no hi bits in 5k mode */
        sd->gain_index     = tlcs_br_read(&br, TLCS_FCB_GAIN_BITS);
    }

    /* SF1: delta_lag(4) + frac(1) + pgain(3) + fcb_lo(24) + gain(4) */
    {
        TlcsSubframeData *sd = &fd->sf[1];
        sd->pitch_lag_idx  = tlcs_br_read(&br, TLCS_PITCH_DELTA_BITS);
        sd->pitch_frac_idx = tlcs_br_read(&br, TLCS_PITCH_FRAC_BITS);
        sd->pitch_gain_idx = tlcs_br_read(&br, TLCS_PITCH_GAIN_BITS);
        sd->fcb_index_lo   = tlcs_br_read(&br, 24);
        sd->fcb_index_hi   = 0;  /* no hi bits in 5k mode */
        sd->gain_index     = tlcs_br_read(&br, TLCS_FCB_GAIN_BITS);
    }

    return TLCS_OK;
}
