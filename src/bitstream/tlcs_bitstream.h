#ifndef TLCS_BITSTREAM_H
#define TLCS_BITSTREAM_H

#include "tlcs/tlcs_types.h"

/* ── Bitstream writer ──────────────────────────────────────────── */

typedef struct {
    uint8_t *buf;        /* output buffer */
    int32_t  buf_size;   /* total buffer capacity in bytes */
    int32_t  byte_pos;   /* current byte position */
    uint32_t bit_acc;    /* bit accumulator */
    int32_t  bits_in_acc;/* bits currently in accumulator (0–31) */
} tlcs_bs_writer;

/* Initialize writer to pack bits into buf[0..buf_size-1]. */
void tlcs_bs_writer_init(tlcs_bs_writer *w, uint8_t *buf, int32_t buf_size);

/* Write n_bits (1–25) from val (MSB-first packing). */
tlcs_status tlcs_bs_write(tlcs_bs_writer *w, uint32_t val, int32_t n_bits);

/* Flush remaining bits (zero-padded to byte boundary).
 * Returns total bytes written. */
int32_t tlcs_bs_writer_flush(tlcs_bs_writer *w);

/* ── Bitstream reader ──────────────────────────────────────────── */

typedef struct {
    const uint8_t *buf;
    int32_t  buf_size;
    int32_t  byte_pos;
    uint32_t bit_acc;
    int32_t  bits_in_acc;
} tlcs_bs_reader;

/* Initialize reader from buf[0..buf_size-1]. */
void tlcs_bs_reader_init(tlcs_bs_reader *r, const uint8_t *buf, int32_t buf_size);

/* Read n_bits (1–25) into *val (MSB-first). */
tlcs_status tlcs_bs_read(tlcs_bs_reader *r, uint32_t *val, int32_t n_bits);

/* Return number of bits remaining. */
int32_t tlcs_bs_reader_remaining(const tlcs_bs_reader *r);

#endif /* TLCS_BITSTREAM_H */
