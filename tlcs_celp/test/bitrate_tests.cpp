// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

// Test file for testing bitrate related behavoir of OPUS and SMPL codec.

#include <SigProc_FIX.h>

#include <gtest/gtest.h>
#include "base.h"
#include "dsp_utils.h"
#include "vad.h"

#include <numeric>

// Bitrate check thresholds
#define BPS_MEAN_THRESHOLD 0.2
#define BPS_STD_THRESHOLD 0.35
#define MAX_FRAME_SIZE_THRESHOLD 2
#define MOVING_BPS_MEAN_THRESHOLD 0.3
#define LOW_BPS_ADJUSTMENT_FACTOR 1.5

// Test to ensure that codec respects target bitrate for active speech segments.
TEST_P(OpusSmplTestDefault, bitrate_speech_segments) {
  if (params_.frame_size_ms < 20 || params_.codec_sample_rate != PCM_RATE_48K) {
    return; // Skipping tests with frame size < 20ms or sampling rate < 48 kHz
  }

  // Music files don't have VAD signal as it will use CELT under the hood.
  const bool is_music = params_.expected_music_count > 0;
  input_pcm_info_ =
      build_pcm_info(PCM_RATE_48K, params_.frame_size_ms, DEFAULT_NUM_CHANNELS);
  int16_t test_bitrates[] = {6000, 8000, 10000, 13000, 16000, 20000, 25000};
  const uint16_t frames_per_window = 2000 / params_.frame_size_ms;

  for (int16_t bitrate : test_bitrates) {
    opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(bitrate));

    std::vector<int> last_encoded_sizes(frames_per_window);
    std::vector<unsigned char> buf_enc(input_pcm_info_.bytes_per_frame);
    std::vector<int16_t> buf_out(input_pcm_info_.samples_per_frame);

    int32_t sum = 0, sum_sq = 0, max_frame_size = 0;
    uint32_t speech_cnt = 0;
    float bps_adjustment_factor = 1;
    // Both Opus/MLow have large deviation from the set target bitrate
    // when operating at lower bitrates. So we give more slack there.
    if (bitrate <= 8000) {
      bps_adjustment_factor = LOW_BPS_ADJUSTMENT_FACTOR;
    }
    // Read from PCM buffer and perform encode/decode in a loop.
    float in_energy = 0;
    float out_energy = 0;
    int n_frames = 0;
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

      if (is_music ||
          opus_get_vad_flag(GetUsingMLow(), buf_enc.data(), encoded_size)) {
        last_encoded_sizes[speech_cnt % frames_per_window] = encoded_size;

        speech_cnt++;
        sum += encoded_size;
        sum_sq += pow(encoded_size, 2);
        max_frame_size = std::max(max_frame_size, encoded_size);

        // Update moving average with last N frame sizes
        if (speech_cnt >= frames_per_window) {
          double moving_sum = std::accumulate(
              last_encoded_sizes.begin(), last_encoded_sizes.end(), 0);
          auto moving_bps_mean =
              (moving_sum / frames_per_window) * 8000 / params_.frame_size_ms;

          EXPECT_NEAR(
              moving_bps_mean,
              bitrate,
              bps_adjustment_factor * MOVING_BPS_MEAN_THRESHOLD * bitrate);
        }
      }
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

    // Calculate mean and standard deviation, scaled to be in bps
    // VAR[X] = E[(X - E[X]) ^ 2] = E[X ^ 2] - E[X] ^ 2
    // Using the second formula for variance allow us to calculate
    // expectation cumulatively, rather than keeping a list of the
    // past encoded sizes
    double mean = sum / speech_cnt;
    double mean_sq = sum_sq / speech_cnt;
    double std = pow(mean_sq - pow(mean, 2), 0.5);
    auto bps_mean = mean * 8000 / params_.frame_size_ms;
    auto bps_std = std * 8000 / params_.frame_size_ms;

    EXPECT_NEAR(bps_mean, bitrate, BPS_MEAN_THRESHOLD * bitrate);
    EXPECT_LE(bps_std, bps_adjustment_factor * BPS_STD_THRESHOLD * bitrate);
    EXPECT_LE(
        max_frame_size,
        bps_adjustment_factor * MAX_FRAME_SIZE_THRESHOLD * mean);

    ResetEncoderDecoder();
  }
}

// Computes avg bitrate over 2sec moving window. Changes the target bitrate
// halfway through and checks that the codec adjusts to new bitrate quickly.
// All the testing is done over active speech segments.
TEST_P(OpusSmplTestDefault, dynamic_bitrate_speech_segments) {
  if (params_.frame_size_ms < 20 || params_.codec_sample_rate != PCM_RATE_48K) {
    return; // Skipping tests with frame size < 20ms or ampling rate < 48 kHz
  }

  // Music files don't have VAD signal as it will use CELT under the hood.
  const bool is_music = params_.expected_music_count > 0;
  int16_t test_bitrates[] = {6000, 8000, 10000, 13000, 16000, 20000, 25000};
  input_pcm_info_ =
      build_pcm_info(PCM_RATE_48K, params_.frame_size_ms, DEFAULT_NUM_CHANNELS);
  const int16_t frames_per_window = 2000 / params_.frame_size_ms;

  // It takes ~0.5s for OPUS to fully adjust to the target bitrate, we will
  // only check the moving average after this adjustment period
  int16_t frames_dynamic_adjustment = 500 / params_.frame_size_ms;

  for (auto bitrate_start : test_bitrates) {
    for (auto bitrate_end : test_bitrates) {
      opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(bitrate_start));
      int16_t cur_bitrate = bitrate_start;

      float bps_adjustment_factor = 1;
      if (cur_bitrate == 6000) {
        bps_adjustment_factor = LOW_BPS_ADJUSTMENT_FACTOR;
      }

      std::vector<int> last_encoded_sizes(frames_per_window);
      std::vector<unsigned char> buf_enc(input_pcm_info_.bytes_per_frame);

      int32_t sum = 0;
      uint32_t speech_cnt = 0, total_cnt = 0;
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
        EXPECT_GT(encoded_size, 0);

        if (is_music ||
            opus_get_vad_flag(GetUsingMLow(), buf_enc.data(), encoded_size)) {
          last_encoded_sizes[speech_cnt % frames_per_window] = encoded_size;

          speech_cnt++;
          sum += encoded_size;

          // Test moving average with last N frame sizes
          if (speech_cnt >= (frames_per_window + frames_dynamic_adjustment)) {
            double moving_sum = std::accumulate(
                last_encoded_sizes.begin(), last_encoded_sizes.end(), 0);
            auto moving_bps_mean =
                (moving_sum / frames_per_window) * 8000 / params_.frame_size_ms;

            EXPECT_NEAR(
                moving_bps_mean,
                cur_bitrate,
                bps_adjustment_factor * MOVING_BPS_MEAN_THRESHOLD *
                    cur_bitrate);
          }
        }

        total_cnt++;
        // Change bitrate halfway through encoding
        if (total_cnt ==
            num_input_samples_ / input_pcm_info_.samples_per_frame / 2) {
          double mean = sum / speech_cnt;
          auto bps_mean = mean * 8000 / params_.frame_size_ms;
          EXPECT_NEAR(
              bps_mean,
              cur_bitrate,
              bps_adjustment_factor * BPS_MEAN_THRESHOLD * cur_bitrate);

          // Reset mean tracking values & update bitrate
          sum = 0;
          speech_cnt = 0;
          opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(bitrate_end));
          cur_bitrate = bitrate_end;
          if (cur_bitrate == 6000) {
            bps_adjustment_factor = LOW_BPS_ADJUSTMENT_FACTOR;
          }
        }
      }

      // Calculate second half bitrate mean.
      double mean = sum / speech_cnt;
      auto bps_mean = mean * 8000 / params_.frame_size_ms;

      EXPECT_NEAR(
          bps_mean,
          cur_bitrate,
          bps_adjustment_factor * BPS_MEAN_THRESHOLD * cur_bitrate);
      ResetEncoderDecoder();
    }
  }
}
