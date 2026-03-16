// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

// Test file for testing basic PLC behavior of Opus and SMPL codec.

#include <SigProc_FIX.h>

#include <glog/logging.h>
#include <gtest/gtest.h>
#include "base.h"
#include "dsp_utils.h"
#include "vad.h"

#include <fmt/core.h>

#include <algorithm>
#include <limits>
#include <random>

#define FEC_MISSED_THRESHOLD 25
#define FEC_DB_THRESHOLD 3
#define FEC_FREQUENT_DB_THRESHOLD 8

// Check that inband FEC is encoded for most speech frames and that inband
// FEC encoding has decent energy as compared to normal encoding.
TEST_P(OpusSmplTestDefault, opus_fec_test) {
  // OPUS does not have FEC for 10ms frames, so test skipped in that case
  if (params_.frame_size_ms == 10 && params_.codec_name == CodecName::OPUS) {
    return; // Skipping FEC test for Opus with 10ms frames
  }

  // TODO(T158167025): For some reason FEC is not being encoded for most frames
  // when the content is music. This is kind of a gray area as we are trying to
  // encode music using SILK (based on the bitrate setting). Also the VAD
  // detection will likely not be accurate for music.
  if (params_.expected_music_count > 0) {
    return; // Skipping FEC testing for music file
  }

  // Test typical and frequent FEC decode scenarios
  std::vector<std::pair<int, int>> fec_frequencies = {
      {3, FEC_DB_THRESHOLD}, {2, FEC_FREQUENT_DB_THRESHOLD}};
  for (const auto& [fec_freq, fec_thresh] : fec_frequencies) {
    // Create separate decoder for FEC frames
    int error;
    OpusDecoder* decoder_fec = opus_decoder_create(
        params_.codec_sample_rate, DEFAULT_NUM_CHANNELS, &error);
    ASSERT_EQ(OPUS_OK, error);
    opus_decoder_ctl(decoder_fec, OPUS_SET_USING_SMPL(GetUsingMLow()));

    std::vector<unsigned char> buf_enc(input_pcm_info_.bytes_per_frame);
    std::vector<unsigned char> prev_buf_enc(input_pcm_info_.bytes_per_frame);
    std::vector<int16_t> buf_out(input_pcm_info_.samples_per_frame, 0);
    std::vector<int16_t> buf_out_fec(input_pcm_info_.samples_per_frame, 0);

    // Enable FEC, the high packet loss and bitrate ensures FEC is encoded
    opus_encoder_ctl(encoder_, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(encoder_, OPUS_SET_PACKET_LOSS_PERC(10));
    opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(20000));

    int encoded_size = 0, prev_encoded_size = 0;
    bool vad = false, prev_vad = false, has_fec = false, prev_has_fec = false;
    int speech_cnt = 0;
    int fec_cnt = 0;
    int vad_fec_cnt = 0;
    int decode_vad_fec_cnt = 0;
    float sum_diff_1v1f = 0;
    // Read from PCM buffer and perform encode/decode in a loop.
    // Decode will be delayed 1 frame from encode
    for (size_t start = 0;
         start + input_pcm_info_.samples_per_frame < num_input_samples_;
         start += input_pcm_info_.samples_per_frame) {
      prev_buf_enc = buf_enc;
      prev_encoded_size = encoded_size;
      prev_vad = vad;
      prev_has_fec = has_fec;

      auto frame_cnt = start / input_pcm_info_.samples_per_frame;
      encoded_size = opus_encode(
          encoder_,
          input_pcm_.data() + start,
          input_pcm_info_.samples_per_frame,
          buf_enc.data(),
          input_pcm_info_.bytes_per_frame);

      vad = opus_get_vad_flag(GetUsingMLow(), buf_enc.data(), encoded_size);
      has_fec = opus_get_fec_flag(GetUsingMLow(), buf_enc.data(), encoded_size);
      speech_cnt += vad;
      fec_cnt += has_fec;
      vad_fec_cnt += vad && has_fec;

      // Begin decode once two frames have been encoded
      if (frame_cnt >= 1) {
        bool decode_fec = ((frame_cnt % fec_freq) == 0) && (frame_cnt != 1);
        int decoded_samples = 0;
        int decoded_samples_fec = 0;

        // Decode without FEC for energy comparisons
        decoded_samples = opus_decode(
            decoder_,
            prev_buf_enc.data(),
            prev_encoded_size,
            buf_out.data(),
            input_pcm_info_.samples_per_frame,
            0);
        // Either decode the FEC from the most recent frame or the main audio
        // from the previous frame Eg. if we have frame_idx = 10 and
        // prev_frame_idx = 9, then frame 10 has the FEC from frame 9, so we
        // will always get audio data from frame 9 in this process
        // If FEC is not included in the given frame, PLC is decoded instead
        if (decode_fec) {
          decoded_samples_fec = opus_decode(
              decoder_fec,
              buf_enc.data(),
              encoded_size,
              buf_out_fec.data(),
              input_pcm_info_.samples_per_frame,
              1);
        } else {
          decoded_samples_fec = opus_decode(
              decoder_fec,
              prev_buf_enc.data(),
              prev_encoded_size,
              buf_out_fec.data(),
              input_pcm_info_.samples_per_frame,
              0);
        }

        // Ensure decode works for FEC & non-FEC frames
        EXPECT_EQ(decoded_samples, input_pcm_info_.samples_per_frame);
        EXPECT_EQ(decoded_samples_fec, input_pcm_info_.samples_per_frame);

        auto output_energy =
            calc_energy(buf_out.data(), input_pcm_info_.samples_per_frame);
        auto output_energy_fec =
            calc_energy(buf_out_fec.data(), input_pcm_info_.samples_per_frame);
        // Measure energy difference between FEC encoding and non-fec encoding
        // of a frame.
        if (prev_vad && has_fec) {
          auto energy_diff = output_energy_fec - output_energy;
          sum_diff_1v1f += std::abs(energy_diff);
          decode_vad_fec_cnt += 1;
        }
      }
    }
    // Ensure FEC is encoded for most speech frames
    EXPECT_GE(fec_cnt, speech_cnt - FEC_MISSED_THRESHOLD);
    // Ensure FEC energy difference is below threshold
    EXPECT_LE(sum_diff_1v1f / decode_vad_fec_cnt, fec_thresh);

    // Destroy FEC encoder, reset other encoders
    opus_decoder_destroy(decoder_fec);
    ResetEncoderDecoder();
  }
}
