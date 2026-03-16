#include <gtest/gtest.h>
#include "AudioFile.h"
#include "smpl_defines.h"

static std::vector<opus_int16> load_testfile(int srate, unsigned int max_no_seconds = 10, bool noisy = false)
{
    AudioFile<int16_t> wavReader;
    std::string wavfile(__FILE__);
    std::string wavfile_relpath;
    EXPECT_TRUE(srate == 16000 || srate == 48000);
    if (srate == 16000) {
        if (noisy) {
            wavfile_relpath = "/../../../../test_signal_16_noisy.wav";
        } else {
            wavfile_relpath = "/../../../../test_signal_wb.wav";
        }
    }
    else {
        if (noisy) {
            wavfile_relpath = "/../../../../test_signal_48_noisy.wav";
        } else {
            wavfile_relpath = "/../../../../test_signal_vctk.wav";
        }
    }
    std::string hppfilename("load_testfile.hpp");
    int pos = wavfile.find(hppfilename, 0);
    wavfile.replace(pos, wavfile_relpath.length(), wavfile_relpath);
    wavReader.load(wavfile);
    EXPECT_TRUE(wavReader.isMono()); // Only mono file - will create stereo signal from it
    EXPECT_TRUE(srate == wavReader.getSampleRate());
    int tot_samples = SMPL_min(wavReader.getNumSamplesPerChannel(), max_no_seconds * srate);
    std::vector<opus_int16> pcm_buf;
    pcm_buf.reserve(tot_samples);
    for (int i = 0; i < tot_samples; i++) {
        pcm_buf.push_back(wavReader.samples[0][i]);
    }
    return pcm_buf;
}
