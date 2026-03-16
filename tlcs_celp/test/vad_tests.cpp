// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

// Test file for testing VAD related behavoir of OPUS and SMPL codec.

#include <SigProc_FIX.h>

#include <gtest/gtest.h>
#include "base.h"
#include "vad.h"

#include <fmt/core.h>
#include <glog/logging.h>

#define DIFF_MAX 5.0

// Test to ensure basic functionallity of VAD
TEST_P(OpusSmplTestDefault, basic_opus_vad2) {
  if (params_.codec_sample_rate != PCM_RATE_48K) {
    return; // Skipping tests with sampling rate < 48 kHz
  }
  // VAD flag is only set for SILK/Hybrid mode in Opus. So to get
  // accurate VAD flag we explicitly set signal type upfront.
  const bool is_music = params_.expected_music_count > 0;
  if (is_music) {
    opus_encoder_ctl(encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
  } else {
    opus_encoder_ctl(encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
  }

  // VAD tests are highly dependent on frame size
  if (params_.frame_size_ms == 60) {
    input_pcm_info_ = build_pcm_info(
        PCM_RATE_48K, params_.frame_size_ms, DEFAULT_NUM_CHANNELS);
    std::vector<unsigned char> buf_enc(input_pcm_info_.bytes_per_frame);

    uint32_t speech_cnt = 0, silence_cnt = 0;
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

      if (!GetUsingMLow() && !is_music) {
        // Make sure it is not in CELT mode as Opus VAD only works in
        // SILK/Hybrid mode.
        EXPECT_LE(buf_enc[0] >> 3, 15);
      }

      // Checking VAD flag
      auto vad =
          opus_get_vad_flag(GetUsingMLow(), buf_enc.data(), encoded_size);
      // Fake count all Opus frames as speech frames in CELT mode.
      if (vad || (!GetUsingMLow() && is_music)) {
        speech_cnt++;
      } else {
        silence_cnt++;
      }
      LOG(INFO) << fmt::format(
          "Frame: {}, vad: {}",
          (int32_t)start / input_pcm_info_.samples_per_frame,
          vad);
    }
    LOG(INFO) << fmt::format(
        "speech frames {}, silence frames {}", speech_cnt, silence_cnt);
    EXPECT_NEAR(speech_cnt, params_.vad_expected_active_count, DIFF_MAX);
    EXPECT_NEAR(silence_cnt, params_.vad_expected_silent_count, DIFF_MAX);
  }
}
