/* Internal bitstream packing/unpacking API. */
#ifndef TLCS_BITSTREAM_H
#define TLCS_BITSTREAM_H

#include <stdint.h>
#include "tlcs_config.h"

/* ---- Bit writer ---------------------------------------------------- */
typedef struct {
    uint8_t *buf;
    int      capacity;   /* bytes */
    int      bit_pos;    /* next bit to write */
} TlcsBitWriter;

void tlcs_bw_init(TlcsBitWriter *bw, uint8_t *buf, int capacity);
void tlcs_bw_write(TlcsBitWriter *bw, int value, int num_bits);
int  tlcs_bw_bytes_written(const TlcsBitWriter *bw);

/* ---- Bit reader ---------------------------------------------------- */
typedef struct {
    const uint8_t *buf;
    int            size;     /* bytes */
    int            bit_pos;  /* next bit to read */
} TlcsBitReader;

void tlcs_br_init(TlcsBitReader *br, const uint8_t *buf, int size);
int  tlcs_br_read(TlcsBitReader *br, int num_bits);

/* ---- Frame-level subframe data ------------------------------------- */
typedef struct {
    int pitch_lag_idx;   /* 8 bits */
    int pitch_frac_idx;  /* 2 bits */
    int pitch_gain_idx;  /* 4 bits */
    int fcb_index;       /* 12 bits */
    int gain_index;      /* 6 bits */
} TlcsSubframeData;

typedef struct {
    int lsp_indices[TLCS_LSP_NUM_SPLITS];         /* 4 x 8 = 32 bits */
    TlcsSubframeData sf[TLCS_NUM_SUBFRAMES];      /* 4 x 32 = 128 bits */
} TlcsFrameData;

/* Pack a TlcsFrameData into buf (must be >= TLCS_BYTES_PER_FRAME).
 * Returns bytes written. */
int tlcs_frame_pack(const TlcsFrameData *fd, uint8_t *buf, int buf_size);

/* Unpack buf into TlcsFrameData. Returns 0 on success. */
int tlcs_frame_unpack(const uint8_t *buf, int buf_size, TlcsFrameData *fd);

#endif /* TLCS_BITSTREAM_H */
