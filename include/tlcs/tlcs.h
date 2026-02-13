#ifndef TLCS_H
#define TLCS_H

#include "tlcs_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Configuration ─────────────────────────────────────────────── */

/* Initialize config with defaults for a given sample rate and bitrate. */
tlcs_status tlcs_config_init(tlcs_config *cfg, int32_t sample_rate, int32_t bitrate);

/* ── Encoder ───────────────────────────────────────────────────── */

/* Initialize encoder state. Zeroes all memory. */
tlcs_status tlcs_encoder_init(tlcs_encoder *enc, const tlcs_config *cfg);

/* Encode one frame of PCM.
 * pcm_in:       input samples (cfg.frame_size samples, int16)
 * bitstream_out: output buffer (must be >= TLCS_MAX_FRAME_BYTES)
 * bytes_written: actual bytes written to bitstream_out
 */
tlcs_status tlcs_encode(tlcs_encoder *enc,
                        const int16_t *pcm_in,
                        uint8_t *bitstream_out,
                        int32_t *bytes_written);

/* ── Decoder ───────────────────────────────────────────────────── */

/* Initialize decoder state. Zeroes all memory. */
tlcs_status tlcs_decoder_init(tlcs_decoder *dec, const tlcs_config *cfg);

/* Decode one frame from bitstream.
 * bitstream_in: encoded frame bytes
 * bytes_in:     length of bitstream_in
 * pcm_out:      output buffer (must hold cfg.frame_size samples)
 */
tlcs_status tlcs_decode(tlcs_decoder *dec,
                        const uint8_t *bitstream_in,
                        int32_t bytes_in,
                        int16_t *pcm_out);

/* Decode with packet loss (PLC). Call when no bitstream available. */
tlcs_status tlcs_decode_plc(tlcs_decoder *dec, int16_t *pcm_out);

#ifdef __cplusplus
}
#endif

#endif /* TLCS_H */
