// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include <stddef.h>
#include <stdint.h>

// Enum for voice vs music mode for OPUS and MLow codecs
// OPUS codec mode defines
// Voice mode defined as opus silk_only or hybrid mode
// Music mode is defined as opus celt_only mode
enum codec_mode {
  CODEC_MODE_VOICE = 0,
  CODEC_MODE_MUSIC = 1,
};

bool opus_get_vad_flag(
    bool using_mlow,
    const uint8_t* payload,
    int payload_length_bytes);

bool opus_get_fec_flag(
    bool using_mlow,
    const uint8_t* payload,
    size_t payload_length_bytes);

// Return which mode of encoding the packet is in by reading the toc byte
codec_mode opus_packet_get_mode(const unsigned char* data);
