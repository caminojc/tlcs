#include "tlcs_range_coder.h"

/* ══════════════════════════════════════════════════════════════════
 *  Subbotin-style range coder (no carry propagation needed).
 *
 *  The key insight: by renormalizing when range < TOP (2^24), and
 *  checking if the top byte of low is the same in both low and
 *  low+range, we can emit bytes without any carry ambiguity.
 *
 *  When top bytes of low and low+range differ, we check if the
 *  interval spans a byte boundary. If it does, we must renormalize
 *  differently to avoid carries.
 *
 *  Reference: Subbotin 1999, used in PPMd, 7zip, etc.
 * ══════════════════════════════════════════════════════════════════ */

/* ── Encoder ──────────────────────────────────────────────────── */

static void rc_enc_output_byte(tlcs_rc_encoder *rc, uint8_t byte)
{
    if (rc->pos < rc->buf_size)
        rc->buf[rc->pos] = byte;
    rc->pos++;
}

void tlcs_rc_enc_init(tlcs_rc_encoder *rc, uint8_t *buf, int32_t size)
{
    rc->buf      = buf;
    rc->buf_size = size;
    rc->pos      = 0;
    rc->low      = 0;
    rc->range    = 0xFFFFFFFFu;
}

static void rc_enc_normalize(tlcs_rc_encoder *rc)
{
    while ((rc->low ^ (rc->low + rc->range)) < TLCS_RC_TOP
           || rc->range < TLCS_RC_BOT) {
        if (rc->range < TLCS_RC_BOT && (rc->low ^ (rc->low + rc->range)) >= TLCS_RC_TOP) {
            /* Range is small but straddles a TOP boundary.
             * Shrink range to fit within the current TOP block. */
            rc->range = (uint32_t)(-(int32_t)rc->low) & (TLCS_RC_TOP - 1);
        }
        rc_enc_output_byte(rc, (uint8_t)(rc->low >> 24));
        rc->low <<= 8;
        rc->range <<= 8;
    }
}

void tlcs_rc_enc_encode(tlcs_rc_encoder *rc,
                        uint32_t cum_freq, uint32_t freq, uint32_t total)
{
    rc->range /= total;
    rc->low   += cum_freq * rc->range;
    rc->range *= freq;
    rc_enc_normalize(rc);
}

int32_t tlcs_rc_enc_flush(tlcs_rc_encoder *rc)
{
    /* Output enough bytes to uniquely identify the final interval */
    for (int i = 0; i < 4; i++) {
        rc_enc_output_byte(rc, (uint8_t)(rc->low >> 24));
        rc->low <<= 8;
    }
    return rc->pos;
}

void tlcs_rc_enc_symbol(tlcs_rc_encoder *rc, int32_t symbol,
                        const uint16_t *cdf, int32_t n_symbols)
{
    if (symbol < 0) symbol = 0;
    if (symbol >= n_symbols) symbol = n_symbols - 1;
    uint32_t cum  = cdf[symbol];
    uint32_t freq = cdf[symbol + 1] - cdf[symbol];
    uint32_t total = cdf[n_symbols];
    if (freq == 0) freq = 1;
    tlcs_rc_enc_encode(rc, cum, freq, total);
}

void tlcs_rc_enc_uniform(tlcs_rc_encoder *rc, int32_t symbol, int32_t n)
{
    if (n <= 1) return;
    if (symbol < 0) symbol = 0;
    if (symbol >= n) symbol = n - 1;
    tlcs_rc_enc_encode(rc, (uint32_t)symbol, 1, (uint32_t)n);
}

/* ── Decoder ──────────────────────────────────────────────────── */

static uint8_t rc_dec_read_byte(tlcs_rc_decoder *rc)
{
    if (rc->pos < rc->buf_size)
        return rc->buf[rc->pos++];
    return 0;
}

void tlcs_rc_dec_init(tlcs_rc_decoder *rc, const uint8_t *buf, int32_t size)
{
    rc->buf      = buf;
    rc->buf_size = size;
    rc->pos      = 0;
    rc->low      = 0;
    rc->range    = 0xFFFFFFFFu;
    rc->code     = 0;

    /* Read initial 4 bytes into code */
    for (int i = 0; i < 4; i++)
        rc->code = (rc->code << 8) | rc_dec_read_byte(rc);
}

static void rc_dec_normalize(tlcs_rc_decoder *rc)
{
    while ((rc->low ^ (rc->low + rc->range)) < TLCS_RC_TOP
           || rc->range < TLCS_RC_BOT) {
        if (rc->range < TLCS_RC_BOT && (rc->low ^ (rc->low + rc->range)) >= TLCS_RC_TOP) {
            rc->range = (uint32_t)(-(int32_t)rc->low) & (TLCS_RC_TOP - 1);
        }
        rc->code = (rc->code << 8) | rc_dec_read_byte(rc);
        rc->low <<= 8;
        rc->range <<= 8;
    }
}

static void rc_dec_decode(tlcs_rc_decoder *rc,
                          uint32_t cum_freq, uint32_t freq, uint32_t total)
{
    rc->range /= total;
    rc->low   += cum_freq * rc->range;
    rc->range *= freq;
    rc_dec_normalize(rc);
}

int32_t tlcs_rc_dec_symbol(tlcs_rc_decoder *rc,
                           const uint16_t *cdf, int32_t n_symbols)
{
    uint32_t total = cdf[n_symbols];
    uint32_t r = rc->range / total;
    uint32_t val = (rc->code - rc->low) / r;
    if (val >= total) val = total - 1;

    /* Linear search */
    int32_t sym = 0;
    while (sym < n_symbols - 1 && cdf[sym + 1] <= val)
        sym++;

    uint32_t cum  = cdf[sym];
    uint32_t freq = cdf[sym + 1] - cdf[sym];
    if (freq == 0) freq = 1;
    rc_dec_decode(rc, cum, freq, total);
    return sym;
}

int32_t tlcs_rc_dec_uniform(tlcs_rc_decoder *rc, int32_t n)
{
    if (n <= 1) return 0;
    uint32_t total = (uint32_t)n;
    uint32_t r = rc->range / total;
    uint32_t val = (rc->code - rc->low) / r;
    if (val >= total) val = total - 1;
    int32_t sym = (int32_t)val;

    rc_dec_decode(rc, (uint32_t)sym, 1, total);
    return sym;
}
