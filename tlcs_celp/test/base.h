// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include <gtest/gtest.h>
#include <opus.h>

#define PCM_RATE_8K 8000u
#define PCM_RATE_16K 16000u
#define PCM_RATE_24K 24000u
#define PCM_RATE_32K 32000u
#define PCM_RATE_48K 48000u

#define DEFAULT_NUM_CHANNELS 1u
#define BYTES_PER_SAMPLE 2u

#define NO_ENERGY (-240.0)
#define ENERGY_DIFF_TOLERANCE_DB 6

constexpr char PCM_FILE_DTX_48K[] = "/assets/female_48k_dtx.pcm";

typedef struct pcm_info {
  uint32_t pcm_rate; // PCM sampling rate in Hz
  uint32_t samples_per_frame; // No. of samples per frame per channel
  uint32_t bytes_per_frame; // No. of bytes per frame per channel
  uint32_t num_channels; // No. of channels
} pcm_info_t;

pcm_info_t
build_pcm_info(uint16_t pcm_rate, uint16_t frame_ms, uint16_t num_channels);

enum CodecName {
  OPUS,
  MLow,
};

typedef struct test_params {
  CodecName codec_name;
  uint32_t input_sample_rate;
  uint32_t codec_sample_rate;
  uint16_t frame_size_ms;
  std::string pcm_file;
  uint32_t vad_expected_active_count;
  uint32_t vad_expected_silent_count;
  uint32_t expected_voice_count;
  uint32_t expected_music_count;
} test_params;

class OpusSmplTest : public ::testing::TestWithParam<test_params> {
 protected:
  void SetUp();
  void TearDown();
  void ReadInputPCM(const std::string pcm_name);

  void CreateEncoderDecoder(
      uint16_t rate_enc,
      uint16_t rate_dec,
      uint16_t num_channels);
  void DestroyEncoderDecoder();
  void ResetEncoderDecoder();
  bool GetUsingMLow();

  // Details about the input PCM for the codec. This has already
  // been resampled if the test file samping rate doesn't match
  // the input codec sampling rate.
  std::vector<int16_t> input_pcm_;
  size_t num_input_samples_;
  pcm_info_t input_pcm_info_;

  // Test parameters
  test_params params_;

  // Encoder/decoder instance for the test case.
  OpusEncoder* encoder_;
  OpusDecoder* decoder_;
};

class OpusSmplTestDefault : public OpusSmplTest {
 protected:
  void SetUp();
  void TearDown();
};
