// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

// Instantiate the test suite

#include <gtest/gtest.h>
#include <vector>
#include "base.h"

using test_file_info = struct test_file_info {
  std::string file_name; // name of input file
  uint32_t pcm_rate; // sampling rate of input file
  uint32_t active_count; // active frame count with 60ms frames
  uint32_t silent_count; // silent frame count with 60ms frames
  uint32_t expected_voice_count; // expected voice frame count with 60ms frames
  uint32_t expected_music_count; // expected music frame count with 60ms frames
};

std::vector<test_file_info> file_info{
    // The frame counts here correspond to 60ms frames.
    // Use vad_tests.cpp to get active/silent frame counts for Opus.
    // Use voice_music_detection_tests.cpp for voice/music counts for Opus.
    test_file_info{"/assets/female_48k.pcm", PCM_RATE_48K, 131, 33, 166, 0},
    test_file_info{"/assets/female_48k_2.pcm", PCM_RATE_48K, 120, 14, 135, 0},
    test_file_info{"/assets/female_48k_3.pcm", PCM_RATE_48K, 135, 37, 150, 0},
    test_file_info{"/assets/female_48k_4.pcm", PCM_RATE_48K, 150, 22, 172, 0},
    test_file_info{"/assets/male_48k_1.pcm", PCM_RATE_48K, 129, 43, 172, 0},
    test_file_info{"/assets/male_48k_2.pcm", PCM_RATE_48K, 146, 26, 172, 0},
    test_file_info{"/assets/male_48k_3.pcm", PCM_RATE_48K, 126, 46, 172, 0},
    test_file_info{"/assets/male_48k_4.pcm", PCM_RATE_48K, 146, 21, 172, 0},
    test_file_info{"/assets/music_48k_1.pcm", PCM_RATE_48K, 199, 0, 11, 188}};
std::vector<test_params> params;

static std::string StringifyTestParams(
    const testing::TestParamInfo<test_params>& info) {
  std::string pcm = info.param.pcm_file;
  pcm = pcm.substr(pcm.find_last_of("/\\") + 1);
  pcm = pcm.substr(0, pcm.find("."));
  const auto codec_name =
      info.param.codec_name == CodecName::OPUS ? "Opus" : "MLow";
  return std::string(codec_name) + "_Frame_" +
      std::to_string(info.param.frame_size_ms) + "_SampleRate_" +
      std::to_string(info.param.codec_sample_rate) + "_" + pcm;
}

// We link test_setup.cpp for each test file separately and some test
// failes may/may not have a specific test suite.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(OpusSmplTestDefault);
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(OpusSmplTest);

INSTANTIATE_TEST_SUITE_P(
    BasicTests,
    OpusSmplTest,
    testing::ValuesIn(params),
    StringifyTestParams);

INSTANTIATE_TEST_SUITE_P(
    BasicTests,
    OpusSmplTestDefault,
    testing::ValuesIn(params),
    StringifyTestParams);

int main(int argc, char** argv) {
  // codecsToTest contains just Opus for now. Add MLow when ready.
  std::vector<CodecName> codecsToTest{CodecName::OPUS, CodecName::MLow};
  std::vector<uint16_t> frameSizesMs{10, 20, 40, 60, 120};
  std::vector<uint16_t> sampleRates{PCM_RATE_8K, PCM_RATE_16K, PCM_RATE_48K};

  for (const auto& codec : codecsToTest) {
    for (const auto& info : file_info) {
      for (const auto& frame_size_ms : frameSizesMs) {
        for (const auto& sample_rate : sampleRates) {
          if (codec == CodecName::MLow && frame_size_ms == 40) {
            // MLow doesn't support 40ms frames.
            continue;
          }

          // all other tests are for all frame sizes
          params.push_back(test_params{
              codec,
              info.pcm_rate, // sampling rate of input file
              sample_rate, // sampling rate at which codec should be tested
              frame_size_ms,
              info.file_name,
              info.active_count,
              info.silent_count,
              info.expected_voice_count,
              info.expected_music_count});
        }
      }
    }
  }

  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
