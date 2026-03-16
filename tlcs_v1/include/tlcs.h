#ifndef TLCS_H
#define TLCS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handles */
typedef struct TlcsEncoder TlcsEncoder;
typedef struct TlcsDecoder TlcsDecoder;

/* Error codes */
#define TLCS_OK          0
#define TLCS_ERR_ARGS   -1
#define TLCS_ERR_ALLOC  -2
#define TLCS_ERR_STREAM -3

/* Lifecycle */
TlcsEncoder* tlcs_encoder_create(int sample_rate, int bitrate_bps);
TlcsDecoder* tlcs_decoder_create(int sample_rate);
void         tlcs_encoder_destroy(TlcsEncoder* enc);
void         tlcs_decoder_destroy(TlcsDecoder* dec);

/* Encode one 20ms frame.
 * pcm: 320 int16 samples @ 16 kHz.
 * buf: output bitstream (must be >= 20 bytes).
 * Returns number of bytes written, or negative on error. */
int tlcs_encode(TlcsEncoder* enc,
                const int16_t* pcm,
                uint8_t* buf,
                int buf_size);

/* Decode one 20ms frame.
 * Returns 320 samples @ 16 kHz, or negative on error. */
int tlcs_decode(TlcsDecoder* dec,
                const uint8_t* buf,
                int buf_size,
                int16_t* pcm);

/* Reset state (for new stream) */
void tlcs_encoder_reset(TlcsEncoder* enc);
void tlcs_decoder_reset(TlcsDecoder* dec);

#ifdef __cplusplus
}
#endif

#endif /* TLCS_H */
