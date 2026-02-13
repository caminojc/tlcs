#ifndef TLCS_RANGE_CODER_H
#define TLCS_RANGE_CODER_H

#include <stdint.h>

/* ── Byte-aligned range coder ───────────────────────────────────
 *
 * Simple Subbotin-style range coder.
 * Uses 32-bit range and low, byte-aligned renormalization.
 * No carry propagation needed (uses top byte check).
 */

#define TLCS_RC_TOP    (1u << 24)
#define TLCS_RC_BOT    (1u << 16)

/* ── Encoder ──────────────────────────────────────────────────── */

typedef struct {
    uint8_t *buf;
    int32_t  buf_size;
    int32_t  pos;
    uint32_t low;
    uint32_t range;
} tlcs_rc_encoder;

void    tlcs_rc_enc_init(tlcs_rc_encoder *rc, uint8_t *buf, int32_t size);
void    tlcs_rc_enc_encode(tlcs_rc_encoder *rc,
                           uint32_t cum_freq, uint32_t freq, uint32_t total);
int32_t tlcs_rc_enc_flush(tlcs_rc_encoder *rc);

void    tlcs_rc_enc_symbol(tlcs_rc_encoder *rc, int32_t symbol,
                           const uint16_t *cdf, int32_t n_symbols);
void    tlcs_rc_enc_uniform(tlcs_rc_encoder *rc, int32_t symbol, int32_t n);

/* ── Decoder ──────────────────────────────────────────────────── */

typedef struct {
    const uint8_t *buf;
    int32_t  buf_size;
    int32_t  pos;
    uint32_t low;
    uint32_t range;
    uint32_t code;
} tlcs_rc_decoder;

void     tlcs_rc_dec_init(tlcs_rc_decoder *rc, const uint8_t *buf, int32_t size);
int32_t  tlcs_rc_dec_symbol(tlcs_rc_decoder *rc,
                            const uint16_t *cdf, int32_t n_symbols);
int32_t  tlcs_rc_dec_uniform(tlcs_rc_decoder *rc, int32_t n);

#endif /* TLCS_RANGE_CODER_H */
