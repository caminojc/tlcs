// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include "base.h"
#include <SigProc_FIX.h>
#include <glog/logging.h>
#include <cstdlib>
#include <fstream>

pcm_info_t
build_pcm_info(uint16_t pcm_rate, uint16_t frame_ms, uint16_t num_channels) {
  pcm_info_t pcm_info;
  pcm_info.pcm_rate = pcm_rate;
  pcm_info.samples_per_frame = frame_ms * pcm_rate / 1000;
  pcm_info.bytes_per_frame =
      pcm_info.samples_per_frame * num_channels * BYTES_PER_SAMPLE;
  pcm_info.num_channels = num_channels;
  return pcm_info;
}

static const std::string ASSET_DIR = "test";

void OpusSmplTest::SetUp() {
  // Setup logging
  FLAGS_alsologtostderr = 1;
  params_ = GetParam();
  google::InitGoogleLogging("IntegrationTest");
}

void OpusSmplTest::TearDown() {
  // Stop logging
  google::ShutdownGoogleLogging();
}

// NOTE: Currently we are only using single 48kHz input file. We can generate
// the SetUp() method further in future if we have other input files. One option
// can be to pass input file path as one of the args for parameterized test.

void OpusSmplTest::ReadInputPCM(const std::string file_name) {
  // --------- Setup Input PCM file --------
  // Open input PCM file and read it.
  auto file_path = "test" + file_name;
  std::ifstream pcm_in(file_path, std::ifstream::binary);
  if (!pcm_in) {
    FAIL() << "Failed to open file: '" << file_path << "'\n";
  }

  auto input_file_pcm_info = build_pcm_info(
      params_.input_sample_rate, params_.frame_size_ms, DEFAULT_NUM_CHANNELS);

  input_pcm_info_ = build_pcm_info(
      params_.codec_sample_rate, params_.frame_size_ms, DEFAULT_NUM_CHANNELS);

  // Get length of file, read all of it in one go.
  pcm_in.seekg(0, pcm_in.end);
  auto num_input_file_samples = (size_t)pcm_in.tellg() / 2;
  pcm_in.seekg(0, pcm_in.beg);
  std::vector<int16_t> input_file_pcm(num_input_file_samples);

  pcm_in.read((char*)input_file_pcm.data(), num_input_file_samples * 2);
  // Resample if needed
  if (params_.codec_sample_rate != params_.input_sample_rate) {
    input_pcm_.resize(
        input_pcm_info_.samples_per_frame * num_input_file_samples /
        input_file_pcm_info.samples_per_frame);

    silk_resampler_state_struct resampler = {};
    silk_resampler_init(
        &resampler, params_.input_sample_rate, input_pcm_info_.pcm_rate, 1);
    size_t buf_start = 0;
    for (size_t start = 0;
         start + input_file_pcm_info.samples_per_frame < num_input_file_samples;
         start += input_file_pcm_info.samples_per_frame) {
      auto status = silk_resampler(
          &resampler,
          input_pcm_.data() + buf_start,
          input_file_pcm.data() + start,
          input_file_pcm_info.samples_per_frame);
      EXPECT_EQ(0, status);
      buf_start += input_pcm_info_.samples_per_frame;
    }
    // Initial resize can cause the size to be bigger because the input
    // no. of samples may not be aligned with the frame size.
    input_pcm_.resize(buf_start);
  } else {
    input_pcm_.assign(input_file_pcm.begin(), input_file_pcm.end());
  }
  num_input_samples_ = input_pcm_.size();
}

void OpusSmplTest::CreateEncoderDecoder(
    uint16_t rate_enc,
    uint16_t rate_dec,
    uint16_t num_channels) {
  // --------- Setup codec instance ---------
  opus_global_create();
  int error;
  int using_MLow;
  encoder_ = opus_encoder_create(
      rate_enc, num_channels, OPUS_APPLICATION_VOIP, &error);
  ASSERT_EQ(OPUS_OK, error);
  opus_encoder_ctl(encoder_, OPUS_SET_USING_SMPL(GetUsingMLow()));
  opus_encoder_ctl(encoder_, OPUS_GET_USING_SMPL(&using_MLow));
  ASSERT_EQ(GetUsingMLow(), using_MLow);
  decoder_ = opus_decoder_create(rate_dec, num_channels, &error);
  ASSERT_EQ(OPUS_OK, error);
  opus_decoder_ctl(decoder_, OPUS_SET_USING_SMPL(GetUsingMLow()));
  opus_decoder_ctl(decoder_, OPUS_GET_USING_SMPL(&using_MLow));
  ASSERT_EQ(GetUsingMLow(), using_MLow);
}

bool OpusSmplTest::GetUsingMLow() {
  return params_.codec_name == CodecName::MLow;
}

void OpusSmplTest::DestroyEncoderDecoder() {
  opus_decoder_destroy(decoder_);
  opus_encoder_destroy(encoder_);
  opus_global_free();
}

void OpusSmplTest::ResetEncoderDecoder() {
  opus_encoder_ctl(encoder_, OPUS_RESET_STATE);
  opus_decoder_ctl(decoder_, OPUS_RESET_STATE);
}

// The Default Encode Decode sets up the encoder/decoder in addition to the PCM
// file
void OpusSmplTestDefault::SetUp() {
  OpusSmplTest::SetUp();
  // Setup Input PCM file
  OpusSmplTest::ReadInputPCM(params_.pcm_file);
  OpusSmplTest::CreateEncoderDecoder(
      params_.codec_sample_rate,
      params_.codec_sample_rate,
      DEFAULT_NUM_CHANNELS);
}

void OpusSmplTestDefault::TearDown() {
  OpusSmplTest::DestroyEncoderDecoder();
  OpusSmplTest::TearDown();
}
