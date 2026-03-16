// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

// Test file for testing basic encode decode behavior. Also instantiates tests
// with current parameters.
#include <gtest/gtest.h>
#include "base.h"
#include "vad.h"

#include <fmt/core.h>
#include <glog/logging.h>

#define EQUAL_FRAMES_THERSHOLD 0.25

// Convert Mono audio to stereo audio with left and right streams identical
static void mono_to_stereo(
    const std::vector<int16_t>& pcm_mono,
    std::vector<int16_t>& pcm_stereo) {
  pcm_stereo.clear();
  pcm_stereo.reserve(pcm_mono.size() * 2);
  for (size_t idx = 0; idx < pcm_mono.size(); idx++) {
    pcm_stereo.push_back(pcm_mono[idx]);
    pcm_stereo.push_back(pcm_mono[idx]);
  }
}

static bool check_if_equal(const std::vector<int16_t>& pcm_stereo) {
  size_t samples_equal = 0;
  size_t samples_unequal = 0;
  const size_t num_samples = pcm_stereo.size() / 2;
  for (size_t idx = 0; idx < num_samples; idx++) {
    if (pcm_stereo[2 * idx] == pcm_stereo[2 * idx + 1]) {
      samples_equal++;
    } else {
      samples_unequal++;
    }
  }
  return (samples_equal == num_samples);
}

// Demonstrate basic Opus encode/decode test.
TEST_P(OpusSmplTest, basic_stereo_test) {
  if (params_.codec_sample_rate != PCM_RATE_48K) {
    return; // Skipping tests with sampling rate < 48 kHz
  }

  // OPUS cannot decode 120ms stereo frames, skipping test in this case
  if (params_.frame_size_ms == 120) {
    return;
  }

  // Configure the encoder decoder to use 2 channels.
  OpusSmplTest::ReadInputPCM(params_.pcm_file);
  const auto& mono_info = input_pcm_info_;
  CreateEncoderDecoder(mono_info.pcm_rate, mono_info.pcm_rate, 2);

  // Create stereo PCM input
  std::vector<int16_t> input_pcm_stereo;
  mono_to_stereo(input_pcm_, input_pcm_stereo);
  auto stereo_info =
      build_pcm_info(mono_info.pcm_rate, params_.frame_size_ms, 2);

  std::vector<unsigned char> buf_enc(stereo_info.bytes_per_frame);
  std::vector<int16_t> buf_out(
      stereo_info.samples_per_frame * stereo_info.num_channels);

  // Read from PCM buffer and perform encode/decode in a loop.
  int frames_equal = 0;
  for (size_t start = 0;
       start + stereo_info.samples_per_frame < num_input_samples_;
       start += stereo_info.samples_per_frame) {
    int encoded_size = opus_encode(
        encoder_,
        input_pcm_stereo.data() + start * 2, // account for stereo samples
        stereo_info.samples_per_frame,
        buf_enc.data(),
        stereo_info.bytes_per_frame);
    EXPECT_GE(encoded_size, 0);

    int decoded_samples = opus_decode(
        decoder_,
        buf_enc.data(),
        encoded_size,
        buf_out.data(),
        stereo_info.samples_per_frame,
        0);
    EXPECT_EQ(decoded_samples, stereo_info.samples_per_frame);
    auto samples_equal = check_if_equal(buf_out);
    frames_equal += samples_equal;
  }
  // Check if a significant number of frames have identical left / right side
  // channel outputs. Can be adjusted if MLow uses different noise generating
  // methods
  EXPECT_GE(
      frames_equal,
      EQUAL_FRAMES_THERSHOLD *
          (num_input_samples_ / mono_info.samples_per_frame));
  LOG(INFO) << fmt::format(
      "frames_equal {}, total_frames {}",
      frames_equal,
      num_input_samples_ / mono_info.samples_per_frame);

  DestroyEncoderDecoder();
}
