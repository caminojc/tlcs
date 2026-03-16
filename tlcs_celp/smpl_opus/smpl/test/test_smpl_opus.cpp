#include <gtest/gtest.h>
#include <opus.h>
#include <array>
#include <vector>
#include <numeric>
#include "smpl_codec_util.h"
#include "smpl_defines.h"
#include "smpl_param_coding.h"
#include "load_testfile.hpp"
#include <math.h>

static inline void update_ix(size_t& enc_ix, size_t& dec_ix)
{
    dec_ix = enc_ix;
    enc_ix = (enc_ix == 0) ? 1 : 0;
}

static bool opus_get_vad_flag(const uint8_t* payload) {
    return (payload[0] >> 6 == 1) || (payload[0] >> 6 == 3); // Normal frame with VoA or a CELT frame
}

static void makeStereo(std::vector<int16_t> &pcmBuf)
{
    std::vector<int16_t> stereoBuf;
    for (const auto& elem : pcmBuf) {
        stereoBuf.push_back(elem);
        stereoBuf.push_back(0);
    }
    pcmBuf = stereoBuf;
}

float get_nrg_db(const std::vector<opus_int16>& x) {
    float nrg = 0.0f;
    for (int i = 0; i < x.size(); i++) {
        nrg += x[i] * x[i];
    }
    nrg /= powf(2.0f, 30.0f);
    nrg /= x.size();

    return 10 * log10(nrg);
}

TEST(SmplOpus, DecodeFirstPacket)
{
#if defined(ENABLE_SMPL)
    opus_global_create();
    int srate = 16000;
    const int bitrate = 25000;
    const int complexity = 5;
    const int use_smpl = 1;
    int encode_channels = 1;
    int use_inband_fec = 1;
    int pTime = 20;

    auto pcmBuf = load_testfile(srate);

    OpusEncoder* encoder = opus_encoder_create(srate, encode_channels, OPUS_APPLICATION_VOIP, NULL);
    EXPECT_TRUE(opus_encoder_ctl(encoder, OPUS_SET_BITRATE(bitrate)) != OPUS_BAD_ARG);
    EXPECT_TRUE(opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(complexity)) != OPUS_BAD_ARG);
    EXPECT_TRUE(opus_encoder_ctl(encoder, OPUS_SET_USING_SMPL(use_smpl)) != OPUS_BAD_ARG);
    EXPECT_TRUE(opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(use_inband_fec)) != OPUS_BAD_ARG);
    EXPECT_TRUE(opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(25)) != OPUS_BAD_ARG);

    const int packet_length = (pTime * srate / 1000);
    int N = pcmBuf.size() / (packet_length * encode_channels);
    std::vector<uint8_t> payload(1024);
    std::vector<int16_t>decBuf(packet_length);

    auto res = opus_encode(encoder, pcmBuf.data(), packet_length, (unsigned char*)payload.data(), payload.size());
    EXPECT_TRUE(res > 0 && res < 1024);

    for (int decode_fec : {0, 1}) {
        OpusDecoder* decoder = opus_decoder_create(srate, 1, NULL);
        EXPECT_TRUE(opus_decoder_ctl(decoder, OPUS_SET_USING_SMPL(use_smpl)) != OPUS_BAD_ARG);

        res = opus_decode(decoder, decode_fec ? (unsigned char*)payload.data() : nullptr, payload.size(), decBuf.data(), packet_length, decode_fec);
        EXPECT_EQ(res, packet_length);
        EXPECT_LT(get_nrg_db(decBuf), -95.0f);

        opus_decoder_destroy(decoder);
    }

    opus_encoder_destroy(encoder);

    opus_global_free();
#endif
}

TEST(SmplOpus, InbandFEC)
{
#if defined(ENABLE_SMPL)
    opus_global_create();

	// One Encoder. Set to high bitrate and high loss. Easier to test as main and redundant will be identical
    for (int decode_channels : {1, 2}) {
        for (int encode_channels : {1, 2}) {
            for (int srate : {16000, 48000}) {
                auto pcmBuf = load_testfile(srate);
                if (encode_channels == 2) {
                    makeStereo(pcmBuf);
                }
                const int bitrate = 25000;
                const int complexity = 5;
                const int use_smpl = 1;
                for (int use_inband_fec : {0, 1}) {
                    for (int pTime : {20, 60, 120}) { // Try 20 60 and 120 ms
                        OpusEncoder* encoder = opus_encoder_create(srate, encode_channels, OPUS_APPLICATION_VOIP, NULL);
                        EXPECT_TRUE(opus_encoder_ctl(encoder, OPUS_SET_BITRATE(bitrate)) != OPUS_BAD_ARG);
                        EXPECT_TRUE(opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(complexity)) != OPUS_BAD_ARG);
                        EXPECT_TRUE(opus_encoder_ctl(encoder, OPUS_SET_USING_SMPL(use_smpl)) != OPUS_BAD_ARG);
                        EXPECT_TRUE(opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(use_inband_fec)) != OPUS_BAD_ARG);
                        EXPECT_TRUE(opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(25)) != OPUS_BAD_ARG);

                        // Create two decoders one uses PLC the other tries to decode FEC
                        std::vector<OpusDecoder*>decoders;
                        for (int i = 0; i < 2; i++) {
                            decoders.push_back(opus_decoder_create(srate, decode_channels, NULL));
                            EXPECT_TRUE(opus_decoder_ctl(decoders.back(), OPUS_SET_USING_SMPL(use_smpl)) != OPUS_BAD_ARG);
                        }
                        EXPECT_EQ(decoders.size(), 2);

                        const int packet_length = (pTime * srate / 1000);
                        int N = pcmBuf.size() / (packet_length * encode_channels);
                        std::array<std::vector<uint8_t>, 2> payloads;
                        for (auto& elem : payloads) {
                            elem.reserve(1024);
                        }
                        std::vector<int16_t>decBuf1(packet_length * decode_channels);
                        std::vector<int16_t>decBuf2(packet_length * decode_channels);
                        size_t decode_ix = 1;
                        size_t encode_ix = 0;
                        payloads[encode_ix].resize(1024);

                        auto res = opus_encode(encoder, pcmBuf.data(), packet_length, (unsigned char*)payloads[encode_ix].data(), payloads[encode_ix].size());
                        EXPECT_TRUE(res > 0 && res < 1024);
                        payloads[encode_ix].resize(res);
                        update_ix(encode_ix, decode_ix);
                        int vad_cnt = 0;
                        for (int n = 1; n < N; n++) {
                            payloads[encode_ix].resize(1024);
                            res = opus_encode(encoder, pcmBuf.data() + n * packet_length * encode_channels, packet_length, (unsigned char*)payloads[encode_ix].data(), payloads[encode_ix].size());
                            EXPECT_TRUE(res > 0 && res < 1024);
                            payloads[encode_ix].resize(res);

                            bool vad_flag = opus_get_vad_flag((unsigned char*)payloads[decode_ix].data());
                            vad_cnt = vad_flag ? vad_cnt + 1 : 0;
                            if (vad_cnt > 10) {
                                if (use_inband_fec) {
                                    // Decoder 1 does Normal Decoding
                                    res = opus_decode(decoders[0], (unsigned char*)payloads[decode_ix].data(), payloads[decode_ix].size(), decBuf1.data(), packet_length, 0);
                                    EXPECT_EQ(res, packet_length);
                                }
                                else {
                                    // Decoder 1 does PLC
                                    res = opus_decode(decoders[0], nullptr, 0, decBuf1.data(), packet_length, 0);
                                    EXPECT_EQ(res, packet_length);
                                }
                                // Decoder 2 tries to decode inband fec.
                                res = opus_decode(decoders[1], (unsigned char*)payloads[encode_ix].data(), payloads[encode_ix].size(), decBuf2.data(), packet_length, 1);
                                EXPECT_EQ(res, packet_length);
                                if (use_inband_fec || pTime == 20) { // Seems like PLC dosnt give the same if called 3x 20 ms vs one 60 ms call
                                    // Test Output is identical
                                    EXPECT_EQ(decBuf1, decBuf2);
                                }
                                break;
                            }
                            else {
                                res = opus_decode(decoders[0], (unsigned char*)payloads[decode_ix].data(), payloads[decode_ix].size(), decBuf1.data(), packet_length, 0);
                                EXPECT_EQ(res, packet_length);
                                res = opus_decode(decoders[1], (unsigned char*)payloads[decode_ix].data(), payloads[decode_ix].size(), decBuf2.data(), packet_length, 0);
                                EXPECT_EQ(res, packet_length);
                            }
                            update_ix(encode_ix, decode_ix);
                        }
                        opus_encoder_destroy(encoder);
                        for (int i = 0; i < 2; i++) {
                            opus_decoder_destroy(decoders[i]);
                        }
                    }
                }
            }
        }
    }

    opus_global_free();
#endif
}

static std::vector<uint8_t> random_payload()
{
    int L = (int)round((rand() / (float)RAND_MAX) * 256 + 2.0f);
    std::vector<uint8_t> payload(L);
    for (int i = 0; i < L; i++) {
        payload.push_back((uint8_t)(rand() & 0xFF));
    }
    EXPECT_TRUE(payload.size() >= 2);
    return payload;
}

static void CorruptPayloadsTest(bool corrupt_toc)
{
#if defined(ENABLE_SMPL)
    opus_global_create();

    for (int srate : {16000, 48000}) {
        auto pcmBuf = load_testfile(srate);

        const int complexity = 5;
        const int use_smpl = 1;
        const int use_inband_fec = 0; // ToDo Loop Over enable + disabeling

        for (int bitrate : {5000, 32000}) {
            for (int pTime : {20, 60, 120}) { // Try 20 60 and 120 ms
                OpusEncoder* encoder = opus_encoder_create(srate, 1, OPUS_APPLICATION_VOIP, NULL);
                EXPECT_TRUE(opus_encoder_ctl(encoder, OPUS_SET_BITRATE(bitrate)) != OPUS_BAD_ARG);
                EXPECT_TRUE(opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(complexity)) != OPUS_BAD_ARG);
                EXPECT_TRUE(opus_encoder_ctl(encoder, OPUS_SET_USING_SMPL(use_smpl)) != OPUS_BAD_ARG);
                EXPECT_TRUE(opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(use_inband_fec)) != OPUS_BAD_ARG);
                EXPECT_TRUE(opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(25)) != OPUS_BAD_ARG);

                OpusDecoder* decoder = opus_decoder_create(srate, 1, NULL);
                EXPECT_TRUE(opus_decoder_ctl(decoder, OPUS_SET_USING_SMPL(use_smpl)) != OPUS_BAD_ARG);

                const int packet_length = (pTime * srate / 1000);
                int N = pcmBuf.size() / packet_length;
                std::vector<int16_t>decBuf(packet_length);

                uint8_t toc_byte;
                for (int n = 0; n < N; n++) {
                    std::vector<uint8_t> payload(1024);
                    auto res = opus_encode(encoder, pcmBuf.data() + n * packet_length, packet_length, (unsigned char*)payload.data(), payload.size());
                    EXPECT_TRUE(res > 0 && res < 1024);
                    payload.resize(res);
                    toc_byte = payload[0];
                    res = opus_decode(decoder, (unsigned char*)payload.data(), payload.size(), decBuf.data(), packet_length, 0);
                    EXPECT_EQ(res, packet_length);
                }
                for (int n = 0; n < 10; n++) {
                    auto payload = random_payload();
                    EXPECT_TRUE(payload.size() > 0);
                    if (!corrupt_toc) {
                        payload[0] = toc_byte;
                        auto res = opus_decode(decoder, (unsigned char*)payload.data(), payload.size(), decBuf.data(), packet_length, 0);
                        EXPECT_EQ(res, packet_length);
                    }
                    else {
                        auto res = opus_decode(decoder, (unsigned char*)payload.data(), payload.size(), decBuf.data(), packet_length, 0);
                        EXPECT_TRUE(res > 0);
                    }
                }
                opus_encoder_destroy(encoder);
                opus_decoder_destroy(decoder);
            }
        }
    }
    opus_global_free();
#endif
}

TEST(SmplOpus, CorruptPayloadsWoTOC)
{
    CorruptPayloadsTest(false);
}

TEST(SmplOpus, CorruptPayloadsWTOC)
{
    CorruptPayloadsTest(true);
}

TEST(SmplOpus, SecondaryEncoder)
{
#if defined(ENABLE_SMPL)
    opus_global_create();

    for (int srate : {16000, 48000}) {
        for (int pTime : {20, 60, 120}) {
            std::vector<OpusEncoder*>encoders;
            int32_t value = 0;
            for (int i = 0; i < 2; i++) {
                encoders.push_back(opus_encoder_create(srate, 1, OPUS_APPLICATION_VOIP, NULL));
                EXPECT_TRUE(opus_encoder_ctl(encoders.back(), OPUS_SET_USING_SMPL(1)) != OPUS_BAD_ARG);
                EXPECT_TRUE(opus_encoder_ctl(encoders.back(), OPUS_SET_COMPLEXITY(5)) != OPUS_BAD_ARG);
                EXPECT_TRUE(opus_encoder_ctl(encoders.back(), OPUS_SET_SECONDARY_COMPLEXITY(5)) != OPUS_BAD_ARG);
            }
            EXPECT_EQ(encoders.size(), 2);
            EXPECT_TRUE(opus_encoder_ctl(encoders.front(), OPUS_SET_BITRATE(20000)) != OPUS_BAD_ARG);

            EXPECT_TRUE(opus_encoder_ctl(encoders.back(), OPUS_SET_BITRATE(20000)) != OPUS_BAD_ARG);
            EXPECT_TRUE(opus_encoder_ctl(encoders.back(), OPUS_SET_SECONDARY_BITRATE(20000)) != OPUS_BAD_ARG);

            auto pcmBuf = load_testfile(srate); 
            int packet_length = pTime * (srate / 1000);
            int N = pcmBuf.size() / packet_length;
            int vad_cnt = 0;
            for (int n = 0; n < N; n++) {
                std::array<std::vector<uint8_t>, 2> payload = { std::vector<uint8_t>(1024), std::vector<uint8_t>(1024) };
                std::array<std::vector<uint8_t>, 2> payload2 = { std::vector<uint8_t>(1024), std::vector<uint8_t>(1024) };
                for (int i = 0; i < 2; i++) {
                    payload[i].resize(1024);
                    auto res = opus_encode(encoders[i], pcmBuf.data() + n * packet_length, packet_length, (unsigned char*)payload[i].data(), payload[i].size());
                    EXPECT_TRUE(res > 0 && res < 1024);
                    payload[i].resize(res);

                    int32_t secondary_bitrate = 0;
                    EXPECT_TRUE(opus_encoder_ctl(encoders[i], OPUS_GET_SECONDARY_BITRATE(&secondary_bitrate)) != OPUS_BAD_ARG);
                    payload2[i].resize(1024);
                    res = opus_encode_secondary(encoders[i], (unsigned char*)payload2[i].data(), payload2[i].size());
                    payload2[i].resize(res);
                    if (secondary_bitrate > 0) {
                        EXPECT_TRUE(res > 0 && res < 1024);
                        ASSERT_EQ(payload2[i], payload[0]);
                    }
                    else {
                        EXPECT_EQ(payload2[i].size(), (size_t)0);
                    }
                }
                EXPECT_EQ(payload[0], payload[1]);

                bool vad_flag = opus_get_vad_flag((unsigned char*)payload[0].data());
                vad_cnt = vad_flag ? vad_cnt + 1 : 0;
                if (vad_cnt > 10) {
                    break;
                }
            }
            for (auto& enc : encoders) {
                opus_encoder_destroy(enc);
            }
        }
    }    
    opus_global_free();
#endif
}

TEST(SmplOpus, VariableHP)
{
#if defined(ENABLE_SMPL)
    opus_global_create();
    const int enc_1_hp = SMPL_ENC_HP_FCORNER_3DB_HZ; // Default value
    for (int enc_2_hp : {SMPL_ENC_HP_FCORNER_3DB_HZ, 2*SMPL_ENC_HP_FCORNER_3DB_HZ, 0}){
        for (int srate : {16000, 48000}) {
            auto pcmBuf = load_testfile(srate);
            // Create two encoders
            std::vector<OpusEncoder*>encoders;
            int32_t value = 0;
            for (int i = 0; i < 2; i++) {
                encoders.push_back(opus_encoder_create(srate, 1, OPUS_APPLICATION_VOIP, NULL));
                EXPECT_TRUE(opus_encoder_ctl(encoders.back(), OPUS_SET_BITRATE(32000)) != OPUS_BAD_ARG);
                EXPECT_TRUE(opus_encoder_ctl(encoders.back(), OPUS_SET_COMPLEXITY(5)) != OPUS_BAD_ARG);
                EXPECT_TRUE(opus_encoder_ctl(encoders.back(), OPUS_SET_USING_SMPL(1)) != OPUS_BAD_ARG);
                EXPECT_TRUE(opus_encoder_ctl(encoders.back(), OPUS_SET_ENC_HP_CUTOFF(70870)) == OPUS_BAD_ARG); // Invalid
                EXPECT_TRUE(opus_encoder_ctl(encoders.back(), OPUS_SET_ENC_HP_CUTOFF(70870)) == OPUS_BAD_ARG); // Invalid
                EXPECT_EQ(enc_1_hp, SMPL_ENC_HP_FCORNER_3DB_HZ);
            }
            EXPECT_EQ(encoders.size(), 2);
            // Set HP for 2. encoder
            EXPECT_TRUE(opus_encoder_ctl(encoders.back(), OPUS_SET_ENC_HP_CUTOFF(enc_2_hp)) != OPUS_BAD_ARG);
            EXPECT_TRUE(opus_encoder_ctl(encoders.back(), OPUS_GET_ENC_HP_CUTOFF(&value)) != OPUS_BAD_ARG);
            EXPECT_EQ(value, enc_2_hp);

            // Create two decoders
            std::vector<OpusDecoder*>decoders;
            for (int i = 0; i < 2; i++) {
                decoders.push_back(opus_decoder_create(srate, 1, NULL));
                EXPECT_TRUE(opus_decoder_ctl(decoders.back(), OPUS_SET_USING_SMPL(1)) != OPUS_BAD_ARG);
            }
            EXPECT_EQ(decoders.size(), 2);

            int packet_length = srate / 50;
            int N = pcmBuf.size() / packet_length;
            int vad_cnt = 0;
            for (int n = 0; n < N; n++) {
                std::array<std::vector<uint8_t>, 2> payload = { std::vector<uint8_t>(1024), std::vector<uint8_t>(1024) };
                std::array<std::vector<int16_t>, 2> decBuf = { std::vector<int16_t>(packet_length), std::vector<int16_t>(packet_length) };
                for (int i = 0; i < 2; i++) {
                    payload[i].resize(1024);
                    auto res = opus_encode(encoders[i], pcmBuf.data() + n * packet_length, packet_length, (unsigned char*)payload[i].data(), payload[i].size());
                    EXPECT_TRUE(res > 0 && res < 1024);
                    payload[i].resize(res);
                    res = opus_decode(decoders[i], (unsigned char*)payload[i].data(), payload[i].size(), decBuf[i].data(), packet_length, 0);
                    EXPECT_EQ(res, packet_length);
                }
                bool vad_flag = opus_get_vad_flag((unsigned char*)payload[0].data());
                vad_cnt = vad_flag ? vad_cnt + 1 : 0;
                if (vad_cnt > 10) {
                    if (enc_1_hp == enc_2_hp) {
                        EXPECT_EQ(payload[0], payload[1]);
                        EXPECT_EQ(decBuf[0], decBuf[1]);
                    }
                    else {
                        // With different HP filters encoder and decoder should give something different
                        EXPECT_NE(payload[0], payload[1]);
                        EXPECT_NE(decBuf[0], decBuf[1]);
                    }
                    break;
                }
            }
            for (auto& enc : encoders) {
                opus_encoder_destroy(enc);
            }
            for (auto& dec : decoders) {
                opus_decoder_destroy(dec);
            }
        }
    }
    opus_global_free();
#endif
}

TEST(SmplOpus, PlcCng)
{
#if defined(ENABLE_SMPL)
    opus_global_create();
    int pTime = 20;
    int bitrate = 25000;
    int complexity = 5;
    int use_smpl = 1;

    for (int srate : {16000, 48000}) {
        for (int pTime : {20, 60, 120}) {
            auto pcmBuf = load_testfile(srate, 100, true);
            OpusEncoder* encoder = opus_encoder_create(srate, 1, OPUS_APPLICATION_VOIP, NULL);
            EXPECT_TRUE(opus_encoder_ctl(encoder, OPUS_SET_BITRATE(bitrate)) != OPUS_BAD_ARG);
            EXPECT_TRUE(opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(complexity)) != OPUS_BAD_ARG);
            EXPECT_TRUE(opus_encoder_ctl(encoder, OPUS_SET_USING_SMPL(use_smpl)) != OPUS_BAD_ARG);
            OpusDecoder* decoder = opus_decoder_create(srate, 1, NULL);
            EXPECT_TRUE(opus_decoder_ctl(decoder, OPUS_SET_USING_SMPL(use_smpl)) != OPUS_BAD_ARG);

            const int packet_length = (pTime * srate / 1000);
            int N = pcmBuf.size() / packet_length;
            std::vector<int16_t>decBuf(packet_length);

            // Before any encoding/decoding, confirm that PLC produces a zero signal
            auto res = opus_decode(decoder, nullptr, 0, decBuf.data(), packet_length, 0);
            EXPECT_EQ(res, packet_length);
            for (auto i = decBuf.begin(); i < decBuf.end(); ++i) {
                EXPECT_TRUE(*i == 0);
            }

            // Start with activity and then have long lost period. Verify signal goes to a very low noise level
            std::vector<uint8_t> payload = std::vector<uint8_t>(1024);
            EXPECT_EQ(payload.size(), 1024);
            int vad_flag = false;
            size_t first_active = 0;
            while (!vad_flag)
            {
                payload.resize(1024);
                auto res = opus_encode(encoder, pcmBuf.data() + first_active * packet_length, packet_length, (unsigned char*)payload.data(), payload.size());
                EXPECT_TRUE(res > 0 && res < 1024);
                payload.resize(res);
                vad_flag = opus_get_vad_flag((unsigned char*)payload.data());
                first_active += 1;
            }

            opus_encoder_ctl(encoder, OPUS_RESET_STATE);
            opus_decoder_ctl(decoder, OPUS_RESET_STATE);

            payload.resize(1024);
            res = opus_encode(encoder, pcmBuf.data() + first_active * packet_length, packet_length, (unsigned char*)payload.data(), payload.size());
            EXPECT_TRUE(res > 0 && res < 1024);
            payload.resize(res);
            res = opus_decode(decoder, (unsigned char*)payload.data(), payload.size(), decBuf.data(), packet_length, 0);
            EXPECT_EQ(res, packet_length);

            int loss_length = 20;
            for (size_t n = 0; n < loss_length; n++) {
                auto res = opus_decode(decoder, nullptr, 0, decBuf.data(), packet_length, 0);
                EXPECT_EQ(res, packet_length);
            }
            EXPECT_GT(get_nrg_db(decBuf), -95.0f);

            // Check that after next long loss there is some CNG level after encountering no speech activity
            for (size_t n = first_active + 1; n < N; n++)
            {
                payload.resize(1024);
                auto res = opus_encode(encoder, pcmBuf.data() + n * packet_length, packet_length, (unsigned char*)payload.data(), payload.size());
                EXPECT_TRUE(res > 0 && res < 1024);
                payload.resize(res);
                res = opus_decode(decoder, (unsigned char*)payload.data(), payload.size(), decBuf.data(), packet_length, 0);
                vad_flag = opus_get_vad_flag((unsigned char*)payload.data());
                EXPECT_EQ(res, packet_length);
            }
            for (size_t n = 0; n < loss_length; n++) {
                auto res = opus_decode(decoder, nullptr, 0, decBuf.data(), packet_length, 0);
                EXPECT_EQ(res, packet_length);
            }
            EXPECT_GT(get_nrg_db(decBuf), -95.0f);

            opus_encoder_destroy(encoder);
            opus_decoder_destroy(decoder);
        }
    }
    opus_global_free();
#endif
}

TEST(SmplOpus, LpcPostfilter)
{
#if defined(ENABLE_SMPL)
    opus_global_create();
    int srate = 16000;
    int pTime = 20;
    int complexity = 5;
    int use_smpl = 1;

    auto pcmBuf = load_testfile(srate, 100, true);
    for (int bitrate : {6000, 25000 }){
        OpusEncoder* encoder = opus_encoder_create(srate, 1, OPUS_APPLICATION_VOIP, NULL);
        EXPECT_TRUE(opus_encoder_ctl(encoder, OPUS_SET_BITRATE(bitrate)) != OPUS_BAD_ARG);
        EXPECT_TRUE(opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(complexity)) != OPUS_BAD_ARG);
        EXPECT_TRUE(opus_encoder_ctl(encoder, OPUS_SET_USING_SMPL(use_smpl)) != OPUS_BAD_ARG);

        // Create two decoders
        int32_t value = 0;
        std::vector<OpusDecoder*>decoders;
        for (int i = 0; i < 2; i++) {
            decoders.push_back(opus_decoder_create(srate, 1, NULL));
            EXPECT_TRUE(opus_decoder_ctl(decoders.back(), OPUS_SET_USING_SMPL(use_smpl)) != OPUS_BAD_ARG);
            EXPECT_TRUE(opus_decoder_ctl(decoders.back(), OPUS_GET_USE_LPC_POSTFILTER(&value)) != OPUS_BAD_ARG);
            EXPECT_EQ(value, 0);
        }
        EXPECT_EQ(decoders.size(), 2);
        // Enable LPC postfilter on second decoder
        EXPECT_TRUE(opus_decoder_ctl(decoders.back(), OPUS_SET_USE_LPC_POSTFILTER(1)) != OPUS_BAD_ARG);
        EXPECT_TRUE(opus_decoder_ctl(decoders.back(), OPUS_GET_USE_LPC_POSTFILTER(&value)) != OPUS_BAD_ARG);
        EXPECT_EQ(value, 1);

        int packet_length = srate / 50;
        int N = pcmBuf.size() / packet_length;
        int vad_cnt = 0;
        std::array<float, 2>totEnergy = {0.0f};
        for (int n = 0; n < N; n++) {
            std::vector<uint8_t> payload = std::vector<uint8_t>(1024);
            std::array<std::vector<int16_t>, 2> decBuf = { std::vector<int16_t>(packet_length), std::vector<int16_t>(packet_length) };
            payload.resize(1024);
            auto res = opus_encode(encoder, pcmBuf.data() + n * packet_length, packet_length, (unsigned char*)payload.data(), payload.size());
            EXPECT_TRUE(res > 0 && res < 1024);
            payload.resize(res);
            for (auto i = 0; i < decoders.size(); i++) {
                res = opus_decode(decoders[i], (unsigned char*)payload.data(), payload.size(), decBuf[i].data(), packet_length, 0);
                EXPECT_EQ(res, packet_length);
                totEnergy[i] += get_nrg_db(decBuf[i]);
            }
        }
    #ifdef SMPL_USE_LPC_POSTFILTER
        EXPECT_GT(totEnergy[0], totEnergy[1]); // LPC Postfilter reduces energy
    #else
        EXPECT_EQ(totEnergy[0], totEnergy[1]);
    #endif

        opus_encoder_destroy(encoder);
        for (auto i = 0; i < decoders.size(); i++) {
            opus_decoder_destroy(decoders[i]);
        }
    }
    opus_global_free();
#endif
}

TEST(SmplOpus, NotAllocated)
{
    OpusEncoder* encoder = opus_encoder_create(16000, 1, OPUS_APPLICATION_VOIP, NULL);
    EXPECT_TRUE(opus_encoder_ctl(encoder, OPUS_SET_USING_SMPL(1)) != OPUS_BAD_ARG);
    OpusDecoder* decoder = (opus_decoder_create(16000, 1, NULL));
    EXPECT_TRUE(opus_decoder_ctl(decoder, OPUS_SET_USING_SMPL(1)) != OPUS_BAD_ARG);

    int packet_length = 960;
    std::vector<int16_t> pcmBuf(packet_length);
    std::vector<uint8_t> payload = std::vector<uint8_t>(1024);
    payload.resize(1024);
    EXPECT_TRUE(opus_encode(encoder, pcmBuf.data(), packet_length, (unsigned char*)payload.data(), payload.size()) != OPUS_OK);
    EXPECT_TRUE(opus_decode(decoder, payload.data(), 0, pcmBuf.data(), packet_length, 0) != OPUS_OK);

    opus_encoder_destroy(encoder);
    opus_decoder_destroy(decoder);
}
