#include "tlcs_bitstream.h"

/* ── Writer ────────────────────────────────────────────────────── */

void tlcs_bs_writer_init(tlcs_bs_writer *w, uint8_t *buf, int32_t buf_size)
{
    w->buf        = buf;
    w->buf_size   = buf_size;
    w->byte_pos   = 0;
    w->bit_acc    = 0;
    w->bits_in_acc= 0;
}

tlcs_status tlcs_bs_write(tlcs_bs_writer *w, uint32_t val, int32_t n_bits)
{
    if (n_bits < 1 || n_bits > 25) return TLCS_ERR_INVALID_ARG;

    /* Check capacity: total bits used + new bits must fit in buffer */
    int32_t total_bits_used = w->byte_pos * 8 + w->bits_in_acc + n_bits;
    if (total_bits_used > w->buf_size * 8) return TLCS_ERR_BUFFER_TOO_SMALL;

    /* Mask to n_bits */
    uint32_t mask = (1U << n_bits) - 1U;
    val &= mask;

    w->bit_acc    = (w->bit_acc << n_bits) | val;
    w->bits_in_acc += n_bits;

    /* Flush complete bytes from accumulator */
    while (w->bits_in_acc >= 8) {
        if (w->byte_pos >= w->buf_size) return TLCS_ERR_BUFFER_TOO_SMALL;
        w->bits_in_acc -= 8;
        w->buf[w->byte_pos++] = (uint8_t)(w->bit_acc >> w->bits_in_acc);
    }

    return TLCS_OK;
}

int32_t tlcs_bs_writer_flush(tlcs_bs_writer *w)
{
    if (w->bits_in_acc > 0) {
        if (w->byte_pos < w->buf_size) {
            /* Left-align remaining bits in the last byte */
            w->buf[w->byte_pos++] = (uint8_t)(w->bit_acc << (8 - w->bits_in_acc));
        }
        w->bits_in_acc = 0;
    }
    return w->byte_pos;
}

/* ── Reader ────────────────────────────────────────────────────── */

void tlcs_bs_reader_init(tlcs_bs_reader *r, const uint8_t *buf, int32_t buf_size)
{
    r->buf        = buf;
    r->buf_size   = buf_size;
    r->byte_pos   = 0;
    r->bit_acc    = 0;
    r->bits_in_acc= 0;
}

tlcs_status tlcs_bs_read(tlcs_bs_reader *r, uint32_t *val, int32_t n_bits)
{
    if (n_bits < 1 || n_bits > 25) return TLCS_ERR_INVALID_ARG;

    /* Refill accumulator from bytes as needed */
    while (r->bits_in_acc < n_bits) {
        if (r->byte_pos >= r->buf_size) return TLCS_ERR_BAD_BITSTREAM;
        r->bit_acc = (r->bit_acc << 8) | r->buf[r->byte_pos++];
        r->bits_in_acc += 8;
    }

    r->bits_in_acc -= n_bits;
    *val = (r->bit_acc >> r->bits_in_acc) & ((1U << n_bits) - 1U);

    return TLCS_OK;
}

int32_t tlcs_bs_reader_remaining(const tlcs_bs_reader *r)
{
    return r->bits_in_acc + (r->buf_size - r->byte_pos) * 8;
}
