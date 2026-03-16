/*
 * tlcs_bitstream.c — Bit packing and unpacking.
 *
 * BitWriter: accumulates bits MSB-first into a byte buffer.
 * BitReader: reads bits MSB-first from a byte buffer.
 * Frame packing: LSP(32) + 4 * [pitch(14) + FCB(12) + gain(6)] = 160 bits = 20 bytes.
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
 *   Per subframe (x4):
 *     pitch_lag_idx:  8 bits
 *     pitch_frac_idx: 2 bits
 *     pitch_gain_idx: 4 bits
 *     fcb_index:     12 bits
 *     gain_index:     6 bits
 *     subtotal:      32 bits
 *   Total: 32 + 4*32 = 160 bits = 20 bytes
 */

int tlcs_frame_pack(const TlcsFrameData *fd, uint8_t *buf, int buf_size)
{
    if (buf_size < TLCS_BYTES_PER_FRAME) return TLCS_ERR_ARGS;

    TlcsBitWriter bw;
    tlcs_bw_init(&bw, buf, buf_size);

    /* LSP indices: 4 x 8 bits */
    for (int s = 0; s < TLCS_LSP_NUM_SPLITS; s++) {
        tlcs_bw_write(&bw, fd->lsp_indices[s], TLCS_LSP_CB_BITS);
    }

    /* Subframes */
    for (int sf = 0; sf < TLCS_NUM_SUBFRAMES; sf++) {
        const TlcsSubframeData *sd = &fd->sf[sf];
        tlcs_bw_write(&bw, sd->pitch_lag_idx,  TLCS_PITCH_LAG_BITS);
        tlcs_bw_write(&bw, sd->pitch_frac_idx, TLCS_PITCH_FRAC_BITS);
        tlcs_bw_write(&bw, sd->pitch_gain_idx, TLCS_PITCH_GAIN_BITS);
        tlcs_bw_write(&bw, sd->fcb_index,      TLCS_ACB_BITS_PER_SUB);
        tlcs_bw_write(&bw, sd->gain_index,     TLCS_GAIN_BITS_PER_SUB);
    }

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

    /* Subframes */
    for (int sf = 0; sf < TLCS_NUM_SUBFRAMES; sf++) {
        TlcsSubframeData *sd = &fd->sf[sf];
        sd->pitch_lag_idx  = tlcs_br_read(&br, TLCS_PITCH_LAG_BITS);
        sd->pitch_frac_idx = tlcs_br_read(&br, TLCS_PITCH_FRAC_BITS);
        sd->pitch_gain_idx = tlcs_br_read(&br, TLCS_PITCH_GAIN_BITS);
        sd->fcb_index      = tlcs_br_read(&br, TLCS_ACB_BITS_PER_SUB);
        sd->gain_index     = tlcs_br_read(&br, TLCS_GAIN_BITS_PER_SUB);
    }

    return TLCS_OK;
}
