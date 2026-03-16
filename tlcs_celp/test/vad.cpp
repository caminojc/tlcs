// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include "vad.h"
#include <fmt/core.h>
#include <glog/logging.h>
#include <opus.h>
#include <bitset>
#include <cstdio>

// Taken from opus_interface.cc in WebRTC-m89
// https://fburl.com/code/vrn3dhsq
static int WebRtcOpus_NumSilkFrames(const uint8_t* payload) {
  // For computing the payload length in ms, the sample rate is not important
  // since it cancels out. We use 48 kHz, but any valid sample rate would work.
  int payload_length_ms =
      opus_packet_get_samples_per_frame(payload, 48000) / 48;
  if (payload_length_ms < 10)
    payload_length_ms = 10;

  int silk_frames;
  switch (payload_length_ms) {
    case 10:
    case 20:
      silk_frames = 1;
      break;
    case 40:
      silk_frames = 2;
      break;
    case 60:
      silk_frames = 3;
      break;
    default:
      return 0; // It is actually even an invalid packet.
  }
  return silk_frames;
}

// Taken from opus_interface.cc in WebRTC-m89
// https://fburl.com/code/v49nmf9n
static int WebRtcOpus_PacketHasVoiceActivity(
    const uint8_t* payload,
    size_t payload_length_bytes) {
  if (payload == NULL || payload_length_bytes == 0)
    return 0;

  // In CELT_ONLY mode we can not determine whether there is VAD.
  if (payload[0] & 0x80)
    return -1;

  int silk_frames = WebRtcOpus_NumSilkFrames(payload);
  if (silk_frames == 0)
    return -1;

  const int channels = opus_packet_get_nb_channels(payload);

  // Max number of frames in an Opus packet is 48.
  opus_int16 frame_sizes[48];
  const unsigned char* frame_data[48];

  // Parse packet to get the frames.
  int frames = opus_packet_parse(
      payload,
      static_cast<opus_int32>(payload_length_bytes),
      NULL,
      frame_data,
      frame_sizes,
      NULL);
  if (frames < 0)
    return -1;

  // Iterate over all Opus frames which may contain multiple SILK frames.
  for (int frame = 0; frame < frames; frame++) {
    if (frame_sizes[frame] < 1) {
      continue;
    }
    if (frame_data[frame][0] >> (8 - silk_frames))
      return 1;
    if (channels == 2 &&
        (frame_data[frame][0] << (silk_frames + 1)) >> (8 - silk_frames))
      return 1;
  }

  return 0;
}

// Taken from opus_interface.cc in WebRTC-m89
// https://fburl.com/code/tpgclnar
// Renamed from WebRtcOpus_PacketHasFec
static int opus_get_fec_flag_internal(
    const uint8_t* payload,
    size_t payload_length_bytes) {
  if (payload == NULL || payload_length_bytes == 0)
    return 0;

  // In CELT_ONLY mode, packets should not have FEC.
  if (payload[0] & 0x80)
    return 0;

  int silk_frames = WebRtcOpus_NumSilkFrames(payload);
  if (silk_frames == 0)
    return 0; // Not valid.

  const int channels = opus_packet_get_nb_channels(payload);

  // Max number of frames in an Opus packet is 48.
  opus_int16 frame_sizes[48];
  const unsigned char* frame_data[48];

  // Parse packet to get the frames. But we only care about the first frame,
  // since we can only decode the FEC from the first one.
  if (opus_packet_parse(
          payload,
          static_cast<opus_int32>(payload_length_bytes),
          NULL,
          frame_data,
          frame_sizes,
          NULL) < 0) {
    return 0;
  }

  if (frame_sizes[0] < 1) {
    return 0;
  }

  // A frame starts with the LP layer. The LP layer begins with two to eight
  // header bits.These consist of one VAD bit per SILK frame (up to 3),
  // followed by a single flag indicating the presence of LBRR frames.
  // For a stereo packet, these first flags correspond to the mid channel, and
  // a second set of flags is included for the side channel. Because these are
  // the first symbols decoded by the range coder and because they are coded
  // as binary values with uniform probability, they can be extracted directly
  // from the most significant bits of the first byte of compressed data.
  for (int n = 0; n < channels; n++) {
    // The LBRR bit for channel 1 is on the (|silk_frames| + 1)-th bit, and
    // that of channel 2 is on the |(|silk_frames| + 1) * 2 + 1|-th bit.
    if (frame_data[0][0] & (0x80 >> ((n + 1) * (silk_frames + 1) - 1)))
      return 1;
  }

  return 0;
}

codec_mode opus_packet_get_mode(const unsigned char* data) {
  codec_mode mode;
  if (data[0] & 0x80) {
    mode = CODEC_MODE_MUSIC;
  } else {
    mode = CODEC_MODE_VOICE;
  }
  return mode;
}

// Return True if packet is VAD, False o/w.
bool opus_get_vad_flag(
    bool using_mlow,
    const uint8_t* payload,
    int payload_length_bytes) {
  if (!using_mlow) {
    return (
        WebRtcOpus_PacketHasVoiceActivity(payload, payload_length_bytes) == 1);
  } else {
    return (payload[0] >> 6 == 1) ||
        (payload[0] >> 6 == 3); // Normal frame with VoA or a CELT frame
  }
}

bool opus_get_fec_flag(
    bool using_mlow,
    const uint8_t* payload,
    size_t payload_length_bytes) {
  if (!using_mlow) {
    return opus_get_fec_flag_internal(payload, payload_length_bytes) == 1;
  } else {
    return ((payload[0] >> 1) & 1) && ((payload[0] >> 6) & 1);
  }
}
