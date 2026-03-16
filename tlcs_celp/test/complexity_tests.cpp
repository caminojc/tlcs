// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

// Test file for testing complexity of encode/decode functions.

#include <fmt/core.h>
#include <glog/logging.h>
#include <gtest/gtest.h>
#include <unordered_map>
#include <vector>

#include "base.h"
#include "stop_watch.h"

#define ENCODER_COMPLEXITY_THRESHOLD 0.95
#define DECODER_COMPLEXITY_THRESHOLD 0.90
#define STRICT_COMPLEXITY_THRESHOLD 1.1
#define REPEAT_COUNT 10
#define MAX_COMPLEXITY 10

// Test complexity of encode/decode
TEST_P(OpusSmplTestDefault, opus_complexity_test) {
  if (params_.codec_sample_rate != PCM_RATE_48K) {
    return; // Skipping tests with sampling rate < 48 kHz
  }

  std::vector<unsigned char> buf_enc(input_pcm_info_.bytes_per_frame);
  std::vector<int> complexities{1, 10};
  std::map<int, float> encode_us;

  // Loop over all complexities multiple times for consistency
  for (auto& complexity : complexities) {
    opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(complexity));

    // Opus internally switches from SILK to CELT when the input signal has
    // music content and the complexity is too high. That sort of messes up
    // these complexity tests as we don't want to compare SILK vs CELT.
    // So we force Opus to pick the right codec by explicitly setting content
    // type.
    if (params_.expected_music_count > 0) {
      opus_encoder_ctl(encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    } else {
      opus_encoder_ctl(encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    }

    StopWatch encode_timer;
    for (int n = 0; n < REPEAT_COUNT; n++) {
      encode_timer.start();
      // Read from PCM buffer and perform encode/decode in a loop.
      for (size_t start = 0;
           start + input_pcm_info_.samples_per_frame < num_input_samples_;
           start += input_pcm_info_.samples_per_frame) {
        auto enc_size = opus_encode(
            encoder_,
            input_pcm_.data() + start,
            input_pcm_info_.samples_per_frame,
            buf_enc.data(),
            input_pcm_info_.bytes_per_frame);
      }
      encode_timer.stop();
    }
    encode_us[complexity] = encode_timer.avg_lap_time().count();

    // Wipe out all encoder/decoder state.
    ResetEncoderDecoder();
  }
  // Ensure encode time increases with complexity values 1, 10
  EXPECT_GE(encode_us[10], encode_us[1] * STRICT_COMPLEXITY_THRESHOLD);
}

// Testing variable complexity of encode/decode across bitrates
TEST_P(OpusSmplTestDefault, opus_detailed_complexity_test) {
  if (params_.codec_sample_rate != PCM_RATE_48K) {
    return; // Skipping tests with sampling rate < 48 kHz
  }

  std::vector<unsigned char> buf_enc(input_pcm_info_.bytes_per_frame);
  std::vector<int> complexities{1, 10};

  const int16_t test_bitrates[] = {
      6000, 8000, 10000, 13000, 16000, 20000, 25000};

  // Map from bitrate -> complexity -> encode_time
  std::map<int, std::map<int, float>> timings;
  for (auto bitrate : test_bitrates) {
    for (auto complexity : complexities) {
      opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(bitrate));
      opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(complexity));

      // Opus internally switches from SILK to CELT when the input signal has
      // music content and the complexity is too high. That sort of messes up
      // these complexity tests as we don't want to compare SILK vs CELT.
      // So we force Opus to pick the right codec by explicitly setting
      // content type.
      if (params_.expected_music_count > 0) {
        opus_encoder_ctl(encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
      } else {
        opus_encoder_ctl(encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
      }

      // Loop over all complexities multiple times for consistency.
      StopWatch encode_timer;
      for (int n = 0; n < REPEAT_COUNT; n++) {
        // Read from PCM buffer and perform encode/decode in a loop.
        encode_timer.start();
        for (size_t start = 0;
             start + input_pcm_info_.samples_per_frame < num_input_samples_;
             start += input_pcm_info_.samples_per_frame) {
          int ret = opus_encode(
              encoder_,
              input_pcm_.data() + start,
              input_pcm_info_.samples_per_frame,
              buf_enc.data(),
              input_pcm_info_.bytes_per_frame);
        }
        encode_timer.stop();
      }
      timings[bitrate][complexity] = encode_timer.avg_lap_time().count();
      ResetEncoderDecoder();
    }
  }

  for (auto bitrate : test_bitrates) {
    // Ensure complexity increases for values 1, 10
    EXPECT_GE(
        timings[bitrate][MAX_COMPLEXITY],
        timings[bitrate][1] * STRICT_COMPLEXITY_THRESHOLD);
  }
}
