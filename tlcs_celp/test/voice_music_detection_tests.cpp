// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

// Test file for voice and music detection

#include <gtest/gtest.h>
#include "base.h"
#include "vad.h"

#include <fmt/core.h>
#include <glog/logging.h>

// Demonstrate basic voice / music detection
TEST_P(OpusSmplTestDefault, voice_music_test) {
  if (params_.codec_sample_rate != PCM_RATE_48K) {
    return; // Skipping tests with sampling rate < 48 kHz
  }

  // Voice / music detection tests are highly dependent on frame size
  if (params_.frame_size_ms == 60) {
    std::vector<unsigned char> buf_enc(input_pcm_info_.bytes_per_frame);
    std::vector<int16_t> buf_out(input_pcm_info_.samples_per_frame);

    // High bitrate is needed to allow CELT mode
    opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(25000));
    uint32_t voice_cnt = 0, music_cnt = 0;

    // Read from PCM buffer and perform encode/decode in a loop.
    for (size_t start = 0;
         start + input_pcm_info_.samples_per_frame < num_input_samples_;
         start += input_pcm_info_.samples_per_frame) {
      int encoded_size = opus_encode(
          encoder_,
          input_pcm_.data() + start,
          input_pcm_info_.samples_per_frame,
          buf_enc.data(),
          input_pcm_info_.bytes_per_frame);
      EXPECT_GE(encoded_size, 0);
      auto mode = opus_packet_get_mode(buf_enc.data());

      // At high bitrates (but not extremely high) OPUS will encode music using
      // CELT, and voice using SILK/HYBRID modes
      if (mode == CODEC_MODE_MUSIC) {
        music_cnt++;
      } else {
        voice_cnt++;
      }
      int decoded_samples = opus_decode(
          decoder_,
          buf_enc.data(),
          encoded_size,
          buf_out.data(),
          input_pcm_info_.samples_per_frame,
          0);
      EXPECT_EQ(decoded_samples, input_pcm_info_.samples_per_frame);
    }
    LOG(INFO) << fmt::format(
        "voice frames: {}, music frames: {}", voice_cnt, music_cnt);
    EXPECT_GE(voice_cnt, params_.expected_voice_count);
    EXPECT_GE(music_cnt, params_.expected_music_count);
  }
}
