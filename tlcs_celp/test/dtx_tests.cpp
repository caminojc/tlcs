// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

// Test file for testing DTX behavior of Opus and SMPL codec.

#include <SigProc_FIX.h>

#include <glog/logging.h>
#include <gtest/gtest.h>
#include "base.h"
#include "vad.h"

#include <fmt/core.h>

// DTX constant defines
#define DTX_MAX_FRAME_SIZE 2

// Test to ensure basic functionallity of DTX
TEST_P(OpusSmplTest, opus_dtx) {
  if (params_.codec_sample_rate != PCM_RATE_48K) {
    return; // Skipping tests with sampling rate < 48 kHz
  }

  // Setup Input PCM file
  OpusSmplTest::ReadInputPCM(PCM_FILE_DTX_48K);
  OpusSmplTest::CreateEncoderDecoder(
      input_pcm_info_.pcm_rate, input_pcm_info_.pcm_rate, DEFAULT_NUM_CHANNELS);

  // Set encoder to commonly used settings in production
  // Complexity & bitrate settings ensure that SILK VAD/DTX decision is used
  opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(10000));
  opus_encoder_ctl(encoder_, OPUS_SET_DTX(1));
  opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(5));

  uint16_t frame_sizes[2] = {20u, 60u};

  for (auto frame_size : frame_sizes) {
    // Adjust pcm_info for current frame size
    input_pcm_info_ =
        build_pcm_info(PCM_RATE_48K, frame_size, DEFAULT_NUM_CHANNELS);

    std::vector<unsigned char> buf_enc(input_pcm_info_.bytes_per_frame);

    // DTX logic counters
    uint32_t frames_before_dtx = 0, frames_between_dtx = 0, in_dtx = 0,
             dtx_period_count = 0;

    // We expect there to be 400ms of silence between each DTX-CNG frame. If
    // We expect DTX-CNG frames to be sent every 400ms. If the frame size does
    // not divide 400ms evenly, then round down (eg. encode 360ms of 1-byte
    // frames for 60ms frames)
    auto frames_between_dtx_threshold = 400u / frame_size;
    // We expect there to be 200ms of fully encoded silence before DTX 1-byte
    // frames are encoded. A higher threshold is used in case of slight
    // differences between VAD and DTX decision logic
    auto frames_before_dtx_threshold_lower = (std::max)(60u / frame_size, 1u);
    auto frames_before_dtx_threshold_upper = 260u / frame_size;

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

      // Checking VAD flag
      auto vad =
          opus_get_vad_flag(GetUsingMLow(), buf_enc.data(), encoded_size);
      int32_t opus_dtx_state;
      opus_encoder_ctl(encoder_, OPUS_GET_IN_DTX(&opus_dtx_state));

      if (vad) {
        // Check frame is not a DTX frame
        EXPECT_GT(encoded_size, DTX_MAX_FRAME_SIZE);
        in_dtx = 0;
        frames_between_dtx = 0;
        frames_before_dtx = 0;
      } else {
        if (opus_dtx_state) {
          if (encoded_size <= DTX_MAX_FRAME_SIZE) {
            frames_between_dtx += 1;
            // Check that 1-byte frames haven't been repeating for too long
            EXPECT_LE(frames_between_dtx, frames_between_dtx_threshold);
          } else {
            // Separate different checks based on whether DTX mode has started
            // or not
            if (in_dtx) {
              // Check that the expected number of 1-byte frames between DTX is
              // correct
              EXPECT_EQ(frames_between_dtx, frames_between_dtx_threshold);
              frames_between_dtx = 0;
            } else {
              // Check that there is at least either 1 fully encoded frame of
              // silence or 60ms silence total before DTX starts
              EXPECT_GE(frames_before_dtx, frames_before_dtx_threshold_lower);
              frames_before_dtx = 0;
              in_dtx = 1;
              dtx_period_count += 1;
            }
          }
        } else {
          frames_before_dtx += 1;
          // Check that non-DTX silence frames haven't been repeating for much
          // longer than 200ms
          EXPECT_LE(frames_before_dtx, frames_before_dtx_threshold_upper);
          // Check that no DTX frames are encoded outside of DTX mode
          EXPECT_GT(encoded_size, DTX_MAX_FRAME_SIZE);
        }
      }
    }
    // female_48k_dtx.pcm has 3 silence periods long enough to trigger DTX
    EXPECT_GE(dtx_period_count, 3);
    ResetEncoderDecoder();
  }

  OpusSmplTest::DestroyEncoderDecoder();
}

// Test to ensure dynamic functionality of DTX
TEST_P(OpusSmplTest, opus_dtx_dynamic) {
  if (params_.codec_sample_rate != PCM_RATE_48K) {
    return; // Skipping tests with sampling rate < 48 kHz
  }

  // Setup Input PCM file
  OpusSmplTest::ReadInputPCM(PCM_FILE_DTX_48K);
  OpusSmplTest::CreateEncoderDecoder(
      input_pcm_info_.pcm_rate, input_pcm_info_.pcm_rate, DEFAULT_NUM_CHANNELS);

  // Set encoder to commonly used settings in production
  // Complexity & bitrate settings ensure that SILK VAD/DTX decision is used
  opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(10000));
  opus_encoder_ctl(encoder_, OPUS_SET_DTX(1));
  opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(5));

  auto frame_size = params_.frame_size_ms;

  if (frame_size <= 60) {
    // Adjust pcm_info for current frame size
    input_pcm_info_ =
        build_pcm_info(PCM_RATE_48K, frame_size, DEFAULT_NUM_CHANNELS);

    std::vector<unsigned char> buf_enc(input_pcm_info_.bytes_per_frame);

    // DTX logic counters
    uint32_t frames_before_dtx = 0, frames_between_dtx = 0, in_dtx = 0,
             dtx_period_count = 0;

    // We expect there to be 400ms of silence between each DTX-CNG frame. If
    // We expect DTX-CNG frames to be sent every 400ms. If the frame size does
    // not divide 400ms evenly, then round down (eg. encode 360ms of 1-byte
    // frames for 60ms frames)
    auto frames_between_dtx_threshold = 400u / frame_size;
    // We expect there to be 200ms of fully encoded silence before DTX 1-byte
    // frames are encoded. A higher threshold is used in case of slight
    // differences between VAD and DTX decision logic
    auto frames_before_dtx_threshold_lower = (std::max)(60u / frame_size, 1u);
    auto frames_before_dtx_threshold_upper = 260u / frame_size;

    // Read from PCM buffer and perform encode/decode in a loop.
    for (size_t start = 0;
         start + input_pcm_info_.samples_per_frame < num_input_samples_;
         start += input_pcm_info_.samples_per_frame) {
      // Disable DTX at ~0.42s, and enable DTX at ~8.4s
      if (((int)((start / input_pcm_info_.samples_per_frame))) ==
          ((int)(420 / frame_size))) {
        opus_encoder_ctl(encoder_, OPUS_SET_DTX(0));
      }
      if (((int)((start / input_pcm_info_.samples_per_frame))) ==
          ((int)(8400 / frame_size))) {
        opus_encoder_ctl(encoder_, OPUS_SET_DTX(1));
      }

      int encoded_size = opus_encode(
          encoder_,
          input_pcm_.data() + start,
          input_pcm_info_.samples_per_frame,
          buf_enc.data(),
          input_pcm_info_.bytes_per_frame);

      // Checking VAD flag
      auto vad =
          opus_get_vad_flag(GetUsingMLow(), buf_enc.data(), encoded_size);
      int32_t opus_dtx_state, opus_dtx_enabled;
      opus_encoder_ctl(encoder_, OPUS_GET_IN_DTX(&opus_dtx_state));
      opus_encoder_ctl(encoder_, OPUS_GET_DTX(&opus_dtx_enabled));

      // When DTX is not enabled, ensure all frames are encoded fully
      if (vad || (!opus_dtx_enabled)) {
        // Check frame is not a DTX frame
        EXPECT_GT(encoded_size, DTX_MAX_FRAME_SIZE);
        // Check DTX is not enabled
        EXPECT_EQ(opus_dtx_state, 0);
        in_dtx = 0;
      } else {
        if (opus_dtx_state) {
          if (!in_dtx) {
            in_dtx = 1;
            dtx_period_count += 1;
          }
        } else {
          // Check that no DTX frames are encoded outside of DTX mode
          EXPECT_GT(encoded_size, DTX_MAX_FRAME_SIZE);
        }
      }

      LOG(INFO) << fmt::format(
          "frame no. {}: encoded_size={}, vad={}, DTX state={}",
          (int32_t)start / input_pcm_info_.samples_per_frame,
          encoded_size,
          vad,
          opus_dtx_state);
    }
    // female_48k_dtx.pcm has 3 silence periods long enough to trigger DTX
    // Dynamic DTX enable/disable will turn DTX off for one of these periods
    EXPECT_GE(dtx_period_count, 2);
    ResetEncoderDecoder();
  }

  OpusSmplTest::DestroyEncoderDecoder();
}
