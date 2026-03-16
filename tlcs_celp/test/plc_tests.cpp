// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

// Test file for testing basic PLC behavior of Opus and SMPL codec.

#include <SigProc_FIX.h>

#include <glog/logging.h>
#include <gtest/gtest.h>
#include "base.h"
#include "dsp_utils.h"
#include "vad.h"

#include <fmt/core.h>

#include <random>

#define MINIMUM_OUTPUT_DB -100
#define BURST_LENGTH 120
#define OUTPUT_ENERGY_BUF_SIZE 6
#define CNG_DB_THRESHOLD -50
#define FIRST_PLC_LOWER_BOUND -10
#define FIRST_PLC_UPPER_BOUND 0
#define PLC_60MS_LOWER_BOUND -30
#define PLC_60MS_UPPER_BOUND -10

// Helper function to determine whether given frame was dropped in bursty_loss
// test. The result is <GOOD:120ms> <LOST:120ms> <GOOD:120ms> <LOST:120ms>.
namespace {
bool bursty_frame_lost(
    const uint16_t frame_idx,
    const uint16_t frame_size,
    const uint16_t burst_length) {
  return (((frame_idx) % (2 * burst_length / frame_size)) >=
          (burst_length / frame_size)) &&
      frame_idx;
}
} // namespace

// Demonstrate basic PLC usage
// TODO integrate ViSQOL scores with testing, test energy distribution for
// 20ms frames, and test energy attenuation for bursty loss >120ms
TEST_P(OpusSmplTestDefault, opus_plc_test) {
  if (params_.codec_sample_rate != PCM_RATE_48K) {
    return; // Skipping tests with sampling rate < 48 kHz
  }

  LOG(INFO) << fmt::format("Frame size: {}", params_.frame_size_ms);
  // Adjust pcm_info for current frame size
  input_pcm_info_ =
      build_pcm_info(PCM_RATE_48K, params_.frame_size_ms, DEFAULT_NUM_CHANNELS);

  std::vector<unsigned char> buf_enc(input_pcm_info_.bytes_per_frame);
  std::vector<int16_t> buf_out(input_pcm_info_.samples_per_frame, 0);

  std::default_random_engine generator(0);
  std::uniform_int_distribution<int> distribution(1, 100);

  // Read from PCM buffer and perform encode/decode in a loop.
  for (size_t start = 0;
       start + input_pcm_info_.samples_per_frame < num_input_samples_;
       start += input_pcm_info_.samples_per_frame) {
    int random_number = distribution(generator);
    if (start == 0) {
      // Ensure first frame is encoded/decoded normally
      random_number = 100;
    }

    int encoded_size = opus_encode(
        encoder_,
        input_pcm_.data() + start,
        input_pcm_info_.samples_per_frame,
        buf_enc.data(),
        input_pcm_info_.bytes_per_frame);

    auto vad = opus_get_vad_flag(GetUsingMLow(), buf_enc.data(), encoded_size);

    // Call PLC on 10% of frames
    int decoded_samples = 0;
    if (random_number <= 10) {
      decoded_samples = opus_decode(
          decoder_,
          NULL,
          encoded_size,
          buf_out.data(),
          input_pcm_info_.samples_per_frame,
          0);
    } else {
      decoded_samples = opus_decode(
          decoder_,
          buf_enc.data(),
          encoded_size,
          buf_out.data(),
          input_pcm_info_.samples_per_frame,
          0);
    }
    // Ensure PLC call works as intended
    EXPECT_EQ(decoded_samples, input_pcm_info_.samples_per_frame);

    // PLC energy analysis
    // Run analysis over energy bands of the output pcm data
    auto output_energy =
        calc_energy(buf_out.data(), input_pcm_info_.samples_per_frame);

    // Ensure PLC output contains some background noise energy
    EXPECT_GT(output_energy, MINIMUM_OUTPUT_DB);

    LOG(INFO) << fmt::format(
        "frame no. {}: rng={}, lost={}, vad={}, output_energy={:.4f}",
        (int32_t)start / input_pcm_info_.samples_per_frame,
        random_number,
        random_number <= 10,
        vad,
        output_energy);
  }
  ResetEncoderDecoder();
}

// Demonstrate plc energy attenuation for 120ms bursty loss
TEST_P(OpusSmplTestDefault, opus_bursty_plc_test) {
  if (params_.codec_sample_rate != PCM_RATE_48K) {
    return; // Skipping tests with sampling rate < 48 kHz
  }

  auto frame_size = params_.frame_size_ms;

  if (frame_size == 20 || frame_size == 60) {
    LOG(INFO) << fmt::format("Frame size: {}", frame_size);
    // Adjust pcm_info for current frame size
    input_pcm_info_ =
        build_pcm_info(PCM_RATE_48K, frame_size, DEFAULT_NUM_CHANNELS);

    std::vector<unsigned char> buf_enc(input_pcm_info_.bytes_per_frame);
    std::vector<int16_t> buf_out(input_pcm_info_.samples_per_frame, 0);

    float new_output_energy = 0;
    // Circular buffer of the last 6 output energies
    std::vector<int> prev_output_energies(OUTPUT_ENERGY_BUF_SIZE);
    float first_plc_diff_sum = 0, plc_diff_sum = 0;
    int first_plc_diff_count = 0, plc_diff_count = 0;
    bool frame_lost = false;

    // Read from PCM buffer and perform encode/decode in a loop.
    for (size_t start = 0;
         start + input_pcm_info_.samples_per_frame < num_input_samples_;
         start += input_pcm_info_.samples_per_frame) {
      // Lose packets in 120ms intervals
      auto frame_idx = (start / input_pcm_info_.samples_per_frame);
      frame_lost = bursty_frame_lost(frame_idx, frame_size, BURST_LENGTH);

      int encoded_size = opus_encode(
          encoder_,
          input_pcm_.data() + start,
          input_pcm_info_.samples_per_frame,
          buf_enc.data(),
          input_pcm_info_.bytes_per_frame);

      auto vad =
          opus_get_vad_flag(GetUsingMLow(), buf_enc.data(), encoded_size);

      // Call PLC on 120ms bursts
      int decoded_samples = opus_decode(
          decoder_,
          frame_lost ? NULL : buf_enc.data(),
          encoded_size,
          buf_out.data(),
          input_pcm_info_.samples_per_frame,
          0);
      // Ensure PLC call works as intended
      EXPECT_EQ(decoded_samples, input_pcm_info_.samples_per_frame);

      // PLC energy analysis
      new_output_energy =
          calc_energy(buf_out.data(), input_pcm_info_.samples_per_frame);
      prev_output_energies[frame_idx % OUTPUT_ENERGY_BUF_SIZE] =
          new_output_energy;

      // Ensure PLC output contains some background noise energy
      EXPECT_GT(new_output_energy, MINIMUM_OUTPUT_DB);

      // Check that the first lost frame contains reasonable amount of energy
      // for 20ms frames
      auto prev_output_energy = prev_output_energies
          [(frame_idx + OUTPUT_ENERGY_BUF_SIZE - 1) % OUTPUT_ENERGY_BUF_SIZE];
      if (frame_lost && frame_idx &&
          !bursty_frame_lost(frame_idx - 1, frame_size, BURST_LENGTH) &&
          prev_output_energy >= CNG_DB_THRESHOLD && frame_size == 20u) {
        auto energy_diff = new_output_energy - prev_output_energy;
        first_plc_diff_sum += energy_diff;
        first_plc_diff_count += 1;
      }

      // Check that energy is lost over time (energy decreases after 60ms of
      // PLC)
      auto prev_output_energy_60ms = prev_output_energies
          [(frame_idx + OUTPUT_ENERGY_BUF_SIZE - (60 / frame_size)) %
           OUTPUT_ENERGY_BUF_SIZE];
      if (frame_lost && frame_idx > (60 / frame_size) &&
          bursty_frame_lost(
              frame_idx - (60 / frame_size), frame_size, BURST_LENGTH) &&
          prev_output_energy_60ms >= CNG_DB_THRESHOLD) {
        plc_diff_sum += new_output_energy - prev_output_energy_60ms;
        plc_diff_count += 1;
      }
      LOG(INFO) << fmt::format(
          "frame no. {}: lost={}, vad={}, output_energy={:.4f}, diff={:.4f}",
          (int32_t)start / input_pcm_info_.samples_per_frame,
          frame_lost,
          vad,
          new_output_energy,
          new_output_energy - prev_output_energy);
    }
    // Check that average energy lost in first use of PLC is reasonable for 20ms
    // frames
    if (frame_size == 20u) {
      auto first_plc_diff_avg = first_plc_diff_sum / first_plc_diff_count;
      EXPECT_GE(first_plc_diff_avg, FIRST_PLC_LOWER_BOUND);
      EXPECT_LE(first_plc_diff_avg, FIRST_PLC_UPPER_BOUND);
      LOG(INFO) << fmt::format(
          "Avg first plc DB change {}", first_plc_diff_avg);
    }
    // Check that the energy lost after 60ms of PLC is reasonable for voiced
    // speech
    if (params_.expected_music_count == 0) {
      auto plc_diff_avg = plc_diff_sum / plc_diff_count;
      EXPECT_GE(plc_diff_avg, PLC_60MS_LOWER_BOUND);
      EXPECT_LE(plc_diff_avg, PLC_60MS_UPPER_BOUND);
      LOG(INFO) << fmt::format(
          "Avg plc DB change from 60ms before {}", plc_diff_avg);
    }
    ResetEncoderDecoder();
  }
}
