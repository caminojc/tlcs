// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

// Test file for testing sample rate related behavoir of OPUS and SMPL codec.
// Also includes tests for helper functions used in sample rate tests.

#include <SigProc_FIX.h>

#include <gtest/gtest.h>
#include "base.h"
#include "dsp_utils.h"

// Max energy different between input & output signal
#define DB_MAX_DIFF 6.0
#define MLOW_BW_EXT_ADDED_ENERGY_DIFF 10.0
#define FREQ_BAND_WIDTH 4000

// Sampling rate test loop; loops over different sampling rates
TEST_P(OpusSmplTest, opus_encode_decode_sampling_rate) {
  // Setup default PCM
  OpusSmplTest::ReadInputPCM(params_.pcm_file);
  auto input_rate_ = input_pcm_info_.pcm_rate;

  // TODO make resampler work with 24k input (and 32k for SMPL codec)
  pcm_info_t sample_pcm_infos[] = {
      build_pcm_info(PCM_RATE_8K, params_.frame_size_ms, DEFAULT_NUM_CHANNELS),
      build_pcm_info(PCM_RATE_16K, params_.frame_size_ms, DEFAULT_NUM_CHANNELS),
      build_pcm_info(
          PCM_RATE_48K, params_.frame_size_ms, DEFAULT_NUM_CHANNELS)};

  // Generate energy frequency buckets of input
  auto spectrum_input = calc_energy_spectrum(
      input_pcm_.data(), num_input_samples_, input_pcm_info_.pcm_rate);
  std::map<int, float> input_buckets_db;
  for (uint16_t bucket = 0; bucket < input_rate_ / 2;
       bucket += FREQ_BAND_WIDTH) {
    auto input_band_db =
        calc_energy_band(spectrum_input, bucket, bucket + FREQ_BAND_WIDTH);
    input_buckets_db[bucket] = input_band_db.value_or(NO_ENERGY);
  }

  std::map<int, float> resampled_buckets_db;

  // Go until input_pcm rate such that populate all buckets even the
  // ones which won't have energy after resampling.
  for (uint16_t bucket = 0; bucket < input_pcm_info_.pcm_rate / 2;
       bucket += FREQ_BAND_WIDTH) {
    auto db =
        calc_energy_band(spectrum_input, bucket, bucket + FREQ_BAND_WIDTH);
    resampled_buckets_db[bucket] = db.value_or(NO_ENERGY);
  }

  for (pcm_info_t output_pcm_info : sample_pcm_infos) {
    uint16_t output_rate_ = output_pcm_info.pcm_rate;
    CreateEncoderDecoder(
        input_pcm_info_.pcm_rate, output_rate_, DEFAULT_NUM_CHANNELS);

    uint32_t total_frames =
        input_pcm_.size() / input_pcm_info_.samples_per_frame;
    size_t output_samples_ = total_frames * output_pcm_info.samples_per_frame;

    // Setup intermediate and output buffers
    std::vector<unsigned char> buf_enc(input_pcm_info_.bytes_per_frame);
    std::vector<int16_t> output_pcm_(output_samples_);

    size_t input_start;
    size_t output_start;
    for (uint16_t frame_idx = 0; frame_idx < total_frames; frame_idx++) {
      input_start = frame_idx * input_pcm_info_.samples_per_frame;
      output_start = frame_idx * output_pcm_info.samples_per_frame;
      int encoded_size = opus_encode(
          encoder_,
          input_pcm_.data() + input_start,
          input_pcm_info_.samples_per_frame,
          buf_enc.data(),
          input_pcm_info_.bytes_per_frame);
      EXPECT_GE(encoded_size, 0);

      int decoded_samples = opus_decode(
          decoder_,
          buf_enc.data(),
          encoded_size,
          output_pcm_.data() + output_start,
          output_pcm_info.samples_per_frame,
          0);
      EXPECT_EQ(decoded_samples, output_pcm_info.samples_per_frame);
    }

    // Run analysis over energy bands of the output pcm data
    auto spectrum_output =
        calc_energy_spectrum(output_pcm_.data(), output_samples_, output_rate_);
    // Input audio file has energy up until roughly 19kHz
    uint32_t max_input_frequency =
        std::min(input_pcm_info_.pcm_rate / 2, 19000u);
    for (uint16_t bucket = 0; bucket < output_pcm_info.pcm_rate / 2;
         bucket += FREQ_BAND_WIDTH) {
      auto output_band_db =
          calc_energy_band(spectrum_output, bucket, bucket + FREQ_BAND_WIDTH)
              .value_or(NO_ENERGY);
      float input_band_db = input_buckets_db[bucket];
      float resampled_db = resampled_buckets_db[bucket];
      std::cout << "Bucket " << bucket << ": Input dB = " << input_band_db
                << ", Resampled dB = " << resampled_db
                << ", Output dB = " << output_band_db << std::endl;
      // MLow bandwidth extension synthesizes signal upto 48KHz based on the
      // available signal of upto 32KHz only. This happens in the decoder. As
      // a result we can expect to see some more energy in the energy bins after
      // 16K (32KHz/2).
      if (bucket < max_input_frequency) {
        auto threshold = (GetUsingMLow() && bucket >= 16000)
            ? MLOW_BW_EXT_ADDED_ENERGY_DIFF
            : DB_MAX_DIFF;
        EXPECT_NEAR(output_band_db, resampled_db, threshold);
      } else if (bucket >= max_input_frequency + 2000) {
        EXPECT_LE(output_band_db, -85);
      }
    }

    DestroyEncoderDecoder();
  }
}

// Test to demo silk resampler usage.
TEST_P(OpusSmplTestDefault, silk_resampler_test) {
  auto output_pcm_info =
      build_pcm_info(PCM_RATE_16K, params_.frame_size_ms, DEFAULT_NUM_CHANNELS);

  silk_resampler_state_struct resampler = {};
  silk_resampler_init(
      &resampler, input_pcm_info_.pcm_rate, output_pcm_info.pcm_rate, 1);

  std::vector<int16_t> buf_out(output_pcm_info.samples_per_frame, 0);

  for (size_t start = 0;
       start + input_pcm_info_.samples_per_frame < num_input_samples_;
       start += input_pcm_info_.samples_per_frame) {
    auto status = silk_resampler(
        &resampler,
        buf_out.data(),
        input_pcm_.data() + start,
        input_pcm_info_.samples_per_frame);
    EXPECT_EQ(0, status);
  }
}

// Test to ensure that the energy spectrum is implemented correctly.
TEST(opus_smpl_tests, test_energy_spectrum) {
  // We use a SINE wave at 800Hz and ensure that the we have maximum
  // energy in the 780 to 820 Hz region.
  const size_t kNumSamples = 32000; // 2 sec audio signal at 16kHz
  const unsigned kSignalHz = 800;
  std::vector<int16_t> pcm_buf(kNumSamples, 0);
  const unsigned factor = PCM_RATE_16K / kSignalHz;

  for (int i = 0; i < kNumSamples; i++) {
    pcm_buf[i] = MAX_PCM_VAL * sin(2 * PI * (i % factor) / factor);
  }

  // Uncomment to dump SINE wave PCM to a file.
  /*std::ofstream pcm_out(
      "tests/opus_integration_tests/assets/generated.pcm",
      std::ofstream::binary);
  EXPECT_TRUE(pcm_out.is_open()) << "Error opening output file!";
  pcm_out.write((char*)pcm_buf, kNumSamples * 2);
  pcm_out.close();
  */

  // We should have a value of 0dB at 800Hz but the output spectrum
  // doesn't have the 800Hz bucket itself and the energy is spread
  // around it. So we look for energy from [780, 820]Hz and it should
  // have a sharp drop outside these.
  auto spectrum =
      calc_energy_spectrum(pcm_buf.data(), kNumSamples, PCM_RATE_16K);
  for (auto it = spectrum.begin(); it != spectrum.end(); it++) {
    if (it->first > 780 && it->first < 820) {
      EXPECT_GE(it->second, -15.0); // Keeping bound little loose.
    } else if (it->first > 100 && it->first < 1500) {
      // Also restrict the checks to avoid other harmonics.
      EXPECT_LE(it->second, -20.0);
    }
  }
}

// Test to compute energy in different bands
TEST_P(OpusSmplTestDefault, energy_bands) {
  if (params_.codec_sample_rate != PCM_RATE_48K) {
    return; // Only 48kHz is supported for now.
  }
  // Calculate energy spectrum and dump it.
  auto spectrum = calc_energy_spectrum(
      input_pcm_.data(), num_input_samples_, input_pcm_info_.pcm_rate);

  // Given input audio file has energy till roughly 19kHz. The exact energy
  // values were taken from spectrum dump.
  EXPECT_GE(calc_energy_band(spectrum, 0, 4000), -60.0);
  EXPECT_GE(calc_energy_band(spectrum, 4000, 16000), -68.0);
  EXPECT_GE(calc_energy_band(spectrum, 16000, 22000), -80.0);
  EXPECT_LE(calc_energy_band(spectrum, 22000, 24000), -80.0);
}
