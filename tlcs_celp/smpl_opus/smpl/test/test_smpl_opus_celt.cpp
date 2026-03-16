#include <gtest/gtest.h>
#include <opus.h>
#include <array>
#include <vector>
#include "load_testfile.hpp"

TEST(Using_CELT, CeltWithDifferentPacketSizes)
{
#if defined(ENABLE_SMPL)
    opus_global_create();

    const int bitrate = 32000;
    const int complexity = 5;
    int packet_duration = 0;

    for (auto srate : { 16000, 48000 }) {
        auto pcmBuf = load_testfile(srate, 5);
        for (int pTime : { 5, 10, 20, 60, 120 }) { // Try 5, 10, 20, 60 and 120 ms
            std::vector<OpusEncoder*>encoders;
            std::vector<OpusDecoder*>decoders;
            std::array<std::vector<int16_t>, 2> decBufs;
            std::vector<int> using_smpl;
            for (int use_smpl : {1, 0}) {
                encoders.push_back(opus_encoder_create(srate, 1, OPUS_APPLICATION_VOIP, NULL));
                EXPECT_TRUE(opus_encoder_ctl(encoders.back(), OPUS_SET_BITRATE(bitrate)) != OPUS_BAD_ARG);
                EXPECT_TRUE(opus_encoder_ctl(encoders.back(), OPUS_SET_COMPLEXITY(complexity)) != OPUS_BAD_ARG);
                EXPECT_TRUE(opus_encoder_ctl(encoders.back(), OPUS_SET_USING_SMPL(use_smpl)) != OPUS_BAD_ARG);
                EXPECT_TRUE(opus_encoder_ctl(encoders.back(), OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC)) != OPUS_BAD_ARG);
                decoders.push_back(opus_decoder_create(srate, 1, NULL));
                EXPECT_TRUE(opus_decoder_ctl(decoders.back(), OPUS_SET_USING_SMPL(use_smpl)) != OPUS_BAD_ARG);
                using_smpl.push_back(use_smpl);
            }

            const int packet_length = (pTime * srate / 1000);
            int N = pcmBuf.size() / packet_length;
            for (auto& elem : decBufs) {
                elem.reserve(packet_length);
                elem.resize(packet_length);
            }

            for (int n = 1; n < N; n++) {
                unsigned char payloads[2][2048];
                int payload_length;
                for (int codec_no = 0; codec_no < encoders.size(); codec_no++) {
                    payload_length = opus_encode(encoders[codec_no], pcmBuf.data() + n * packet_length, packet_length, (unsigned char*)payloads[codec_no], 1024);
                    EXPECT_TRUE(payload_length > 0 && payload_length < 2048);

                    auto res = opus_decode(decoders[codec_no], (unsigned char*)payloads[codec_no], payload_length, decBufs[codec_no].data(), packet_length, 0);
                    EXPECT_EQ(res, packet_length);
                    if (using_smpl[codec_no])
                        EXPECT_EQ(3, payloads[codec_no][0] >> 6);
                    EXPECT_TRUE(opus_decoder_ctl(decoders[codec_no], OPUS_GET_LAST_PACKET_DURATION(&packet_duration)) != OPUS_BAD_ARG);
                    EXPECT_EQ((packet_duration * 1000) / srate, pTime);
                }
                if (encoders.size() == 2) {
                    std::vector<unsigned char> payload0_without_toc = { payloads[0] + 1, payloads[0] + payload_length - 1 };
                    std::vector<unsigned char> payload1_without_toc = { payloads[1] + 1, payloads[1] + payload_length - 1 };
                    EXPECT_TRUE(payload0_without_toc == payload1_without_toc);
                    EXPECT_TRUE(decBufs[0] == decBufs[1]);
                }
            }
            // Switch out of Music mode to trigger CELT -> silk/mlow switch
            {
                unsigned char payloads[2][2048];
                int payload_length;
                for (int codec_no = 0; codec_no < encoders.size(); codec_no++) {
                    // Switch out of Music mode to trigger prefilling
                    EXPECT_TRUE(opus_encoder_ctl(encoders[codec_no], OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE)) != OPUS_BAD_ARG);
                    payload_length = opus_encode(encoders[codec_no], pcmBuf.data(), packet_length, (unsigned char*)payloads[codec_no], 1024);
                    EXPECT_TRUE(payload_length > 0 && payload_length < 2048);

                    auto res = opus_decode(decoders[codec_no], (unsigned char*)payloads[codec_no], payload_length, decBufs[codec_no].data(), packet_length, 0);
                    EXPECT_EQ(res, packet_length);
                }
            }
            for (int i = 0; i < encoders.size(); i++) {
                opus_encoder_destroy(encoders[i]);
                opus_decoder_destroy(decoders[i]);
            }
        }
    }

    opus_global_free();
#endif
}
