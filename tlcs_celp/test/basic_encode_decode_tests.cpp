// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

// Test file for testing basic encode decode behavior. Also instantiates tests
// with current parameters.

#include <SigProc_FIX.h>
#include <gtest/gtest.h>
#include <random>
#include "base.h"
#include "dsp_utils.h"

// Helper utilities.
namespace {
void dump_energy_spectrum(const std::map<int, float>& spectrum) {
  for (auto it = spectrum.begin(); it != spectrum.end(); it++) {
    printf("%5d\t%f\n", it->first, it->second);
  }
}
} // namespace

// Demonstrate basic Opus encode/decode test.
TEST_P(OpusSmplTestDefault, basic_opus_encode_decode) {
  std::vector<unsigned char> buf_enc(input_pcm_info_.bytes_per_frame);
  std::vector<int16_t> buf_out(input_pcm_info_.samples_per_frame);

  float in_energy = 0;
  float out_energy = 0;
  int n_frames = 0;
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

    int decoded_samples = opus_decode(
        decoder_,
        buf_enc.data(),
        encoded_size,
        buf_out.data(),
        input_pcm_info_.samples_per_frame,
        0);
    EXPECT_EQ(decoded_samples, input_pcm_info_.samples_per_frame);

    in_energy += calc_energy(
        input_pcm_.data() + start, input_pcm_info_.samples_per_frame);
    out_energy += calc_energy(buf_out.data(), decoded_samples);
    n_frames++;
  }
  EXPECT_NEAR(
      in_energy / n_frames, out_energy / n_frames, ENERGY_DIFF_TOLERANCE_DB);
}

// Basic encode/decode with random input PCM vector.
TEST_P(OpusSmplTestDefault, basic_encode_decode_random_input) {
  std::random_device rd;
  std::mt19937 mt(rd());
  std::vector<int16_t> random_input(input_pcm_info_.samples_per_frame);

  std::vector<unsigned char> buf_enc(input_pcm_info_.bytes_per_frame);
  std::vector<int16_t> buf_out(input_pcm_info_.samples_per_frame);

  float in_energy = 0;
  float out_energy = 0;
  int n_frames = 0;
  // Run a loop over 1000 random frames
  for (; n_frames < 1000; n_frames++) {
    for (int i = 0; i < input_pcm_info_.samples_per_frame; i++) {
      random_input[i] =
          std::uniform_int_distribution<int16_t>(INT16_MIN, INT16_MAX)(mt);
    }
    int encoded_size = opus_encode(
        encoder_,
        random_input.data(),
        input_pcm_info_.samples_per_frame,
        buf_enc.data(),
        input_pcm_info_.bytes_per_frame);
    EXPECT_GE(encoded_size, 0);

    int decoded_samples = opus_decode(
        decoder_,
        buf_enc.data(),
        encoded_size,
        buf_out.data(),
        input_pcm_info_.samples_per_frame,
        0);
    EXPECT_EQ(decoded_samples, input_pcm_info_.samples_per_frame);

    in_energy +=
        calc_energy(random_input.data(), input_pcm_info_.samples_per_frame);
    out_energy += calc_energy(buf_out.data(), decoded_samples);
  }
  EXPECT_NEAR(
      in_energy / n_frames, out_energy / n_frames, ENERGY_DIFF_TOLERANCE_DB);
}
