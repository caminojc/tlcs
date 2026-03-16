#include <gtest/gtest.h>
#include <math.h>
#include <stdio.h>
#include "smpl_api.h"
#include "smpl_param_coding.h"
#include "AudioFile.h"

#include "smpl_hb_lpc_tables.h"

#define MAX_PACKET 1500

static void setDefaultEncControl(smpl_EncControlStruct *encCtrl) {
    memset(encCtrl, 0, sizeof(smpl_EncControlStruct));
    encCtrl->API_sampleRate = 16000;
    encCtrl->nChannelsAPI = 1;
    encCtrl->nChannelsInternal = 1;
    encCtrl->internalSampleRate = 16000;
    encCtrl->minInternalSampleRate = 16000;
    encCtrl->maxInternalSampleRate = 48000;
    encCtrl->bitRate = 9600;
    encCtrl->complexity = 5;
    encCtrl->payloadSize_ms = 20;
    encCtrl->maxBits = MAX_PACKET-1;
    encCtrl->useDTX = 0;
}

static void setDefaultDecControl(smpl_DecControlStruct *decCtrl) {
    memset(decCtrl, 0, sizeof(smpl_DecControlStruct));
    decCtrl->API_sampleRate = 16000;
    decCtrl->nChannelsAPI = 1;
}

TEST(SmplApiTest, EncApiHandlesInvalidArguments)
{
    void *enc = NULL;
    smpl_EncControlStruct encCtrl;
    int enc_size;
    unsigned char encoded_bytes[MAX_PACKET];
    short packet_samples[320];
    ec_enc ent_enc;
    unsigned char toc_byte;
    opus_int32 encoded_no_bytes;
    ASSERT_EQ(SMPL_NO_ERROR, smpl_CreateCodec());
    smpl_Get_Encoder_Size(&enc_size);
    enc = malloc(enc_size);
    setDefaultEncControl(&encCtrl);
    encCtrl.payloadSize_ms = 27;
    ec_enc_init(&ent_enc, encoded_bytes, MAX_PACKET - 1);
    ASSERT_EQ(SMPL_ENC_PACKET_SIZE_NOT_SUPPORTED, smpl_Encode(enc, &encCtrl, packet_samples, 320, &ent_enc, &toc_byte, &encoded_no_bytes, 0, 1));
    encCtrl.payloadSize_ms = 20;
    ASSERT_EQ(SMPL_ENC_INPUT_INVALID_NO_OF_SAMPLES, smpl_Encode(enc, &encCtrl, packet_samples, 316, &ent_enc, &toc_byte, &encoded_no_bytes, 0, 1));
    free(enc);
    smpl_FreeCodec();
}

#define TEST_CHANGING_BITRATE -1

TEST(SmplApiTest, EncDecWithoutErrors)
{
    void *enc = NULL;
    void *dec = NULL;
    unsigned int max_no_seconds = 1;
    unsigned int fs = 16000;
    ASSERT_EQ(SMPL_NO_ERROR, smpl_CreateCodec());
    /* Read input audio file. */
    AudioFile<int16_t> wavReader;
    std::string wavfile(__FILE__);
    std::string wavfile_relpath("/../../../../test_signal_wb.wav");
    std::string cppfilename("test_smpl_enc_api.cpp");
    int pos = wavfile.find(cppfilename, 0);
    wavfile.replace(pos, wavfile_relpath.length(), wavfile_relpath);
    wavReader.load(wavfile);
    assert(wavReader.isMono()); // mono only for now.
    assert(fs == wavReader.getSampleRate());
    int tot_samples = SMPL_min(wavReader.getNumSamplesPerChannel(), max_no_seconds * fs);
    opus_int16 *pcm_buf = wavReader.samples.front().data();

    for (auto bitrate : {4200, 7000, 9600, 18000, 32000, TEST_CHANGING_BITRATE}) {
        for (auto packet_ms : {10, 20, 60, 120}) {
            for (auto lostFlag : { SMPL_FALSE, SMPL_TRUE }) {
                for (auto useDTX : { SMPL_FALSE, SMPL_TRUE }) {
                    smpl_EncControlStruct encCtrl;
                    smpl_DecControlStruct decCtrl;
                    int enc_size, dec_size;
                    smpl_Get_Encoder_Size(&enc_size);
                    enc = malloc(enc_size);
                    smpl_Get_Decoder_Size(&dec_size);
                    dec = malloc(dec_size);
                    setDefaultEncControl(&encCtrl);
                    encCtrl.payloadSize_ms = packet_ms;
                    encCtrl.useDTX = useDTX;
                    setDefaultDecControl(&decCtrl);
                    ASSERT_EQ(SMPL_NO_ERROR, smpl_InitEncoder(enc, &encCtrl));
                    ASSERT_EQ(SMPL_NO_ERROR, smpl_InitDecoder(dec));
                    int packet_samples = packet_ms * (fs / 1000);
                    int packets = SMPL_min(tot_samples/packet_samples, (max_no_seconds*1000)/packet_ms);
                    for (int packet = 0; packet<packets; packet++)  {
                        unsigned char encoded_bytes[MAX_PACKET];
                        opus_int16 samples_out[SMPL_MAX_FB_PACKET_SIZE_IN_SAMPLES];
                        opus_int32 encoded_no_bytes;
                        opus_int32 n_samples_out;
                        unsigned char *toc_byte = encoded_bytes;
                        ec_enc ent_enc;
                        ec_dec ent_dec;
                        if (bitrate != TEST_CHANGING_BITRATE) {
                            encCtrl.bitRate = bitrate;
                            encCtrl.mainBitRate = encCtrl.bitRate;
                        } else {
                            encCtrl.bitRate = 5000 * ((packet%3) + 1);
                            encCtrl.mainBitRate = encCtrl.bitRate;
                        }
                        // Encode
                        ec_enc_init(&ent_enc, encoded_bytes + 1, MAX_PACKET - 1);
                        ASSERT_EQ(SMPL_NO_ERROR, smpl_Encode(enc, &encCtrl, pcm_buf + packet * packet_samples, packet_samples/encCtrl.nChannelsAPI, &ent_enc, toc_byte, &encoded_no_bytes, 0, 1));
                        if (!useDTX) {
                            ASSERT_GT(encoded_no_bytes, 0);
                            ASSERT_EQ(encoded_no_bytes, 1 + ((ec_tell(&ent_enc) + 7) >> 3)); // TOC byte + ent coding
                        }
                        else {
                            ASSERT_EQ(encoded_no_bytes == 0 || (encoded_no_bytes == 1 + ((ec_tell(&ent_enc) + 7) >> 3)), true);
                        }
                        ec_enc_done(&ent_enc);
                        // Decode
                        ec_dec_init(&ent_dec, encoded_bytes + 1, encoded_no_bytes > 0 ? encoded_no_bytes -1 : 0);
                        int modLostFlag = encoded_no_bytes == 0 ? SMPL_FLAG_PACKET_LOST : ((packet > 1) * lostFlag);
                        n_samples_out = modLostFlag == 1 ? packet_samples : SMPL_ARR_LEN(samples_out);
                        ASSERT_EQ(SMPL_NO_ERROR, smpl_Decode(dec, &decCtrl, modLostFlag, 1, &ent_dec, toc_byte[0], samples_out, &n_samples_out));
                        ASSERT_EQ(packet_samples, n_samples_out*decCtrl.nChannelsAPI);
                        ASSERT_EQ(packet_ms, decCtrl.payloadSize_ms);
                        ASSERT_EQ(fs, decCtrl.internalSampleRate);
                    }
                    free(enc);
                    free(dec);
                }
            }
        }
    }
    smpl_FreeCodec();
}

TEST(SmplApiTest, EncDecWithStereo)
{
    void* enc = NULL;
    void* dec = NULL;
    unsigned int max_no_seconds = 1;
    unsigned int fs = 16000;
    ASSERT_EQ(SMPL_NO_ERROR, smpl_CreateCodec());
    /* Read input audio file. */
    AudioFile<int16_t> wavReader;
    std::string wavfile(__FILE__);
    std::string wavfile_relpath("/../../../../test_signal_wb.wav");
    std::string cppfilename("test_smpl_enc_api.cpp");
    int pos = wavfile.find(cppfilename, 0);
    wavfile.replace(pos, wavfile_relpath.length(), wavfile_relpath);
    wavReader.load(wavfile);
    assert(wavReader.isMono()); // Only mono file - will create stereo signal from it
    assert(fs == wavReader.getSampleRate());
    int channels = 2;
    int phase_shift_samples = 2*fs; // right channel shifted by 2s
    int tot_samples = SMPL_min(wavReader.getNumSamplesPerChannel() - phase_shift_samples, max_no_seconds * fs);
    opus_int16* pcm_buf = new opus_int16[wavReader.getNumSamplesPerChannel() * channels];
    // Create a stereo signal
    for (int i = 0; i < tot_samples; i++) {
        pcm_buf[2*i]     = wavReader.samples[0][i];
        pcm_buf[2*i + 1] = wavReader.samples[0][i + phase_shift_samples];
    }
    tot_samples *= channels;

    for (auto bitrate : { 7000, 18000, 32000 }) {
        for (auto packet_ms : { 10, 20, 60, 120 }) {
            for (auto stereoEncodeInternal : { SMPL_FALSE, SMPL_TRUE }) {
                for (auto stereoDecodeApi : { SMPL_FALSE, SMPL_TRUE }) {
                    smpl_EncControlStruct encCtrl;
                    smpl_DecControlStruct decCtrl;
                    int enc_size, dec_size;
                    smpl_Get_Encoder_Size(&enc_size);
                    enc = malloc(enc_size);
                    smpl_Get_Decoder_Size(&dec_size);
                    dec = malloc(dec_size);
                    setDefaultEncControl(&encCtrl);
                    encCtrl.payloadSize_ms = packet_ms;
                    encCtrl.nChannelsAPI = 2;
                    setDefaultDecControl(&decCtrl);
                    decCtrl.nChannelsAPI = (stereoDecodeApi == false) ? 1 : 2;
                    ASSERT_EQ(SMPL_NO_ERROR, smpl_InitEncoder(enc, &encCtrl));
                    ASSERT_EQ(SMPL_NO_ERROR, smpl_InitDecoder(dec));
                    int packet_samples = packet_ms * (fs / 1000) * channels;
                    int packets = SMPL_min(tot_samples / packet_samples, (max_no_seconds * 1000) / packet_ms);
                    for (int packet = 0; packet < packets; packet++) {
                        unsigned char encoded_bytes[MAX_PACKET];
                        opus_int16 samples_out[SMPL_MAX_FB_PACKET_SIZE_IN_SAMPLES];
                        opus_int32 encoded_no_bytes;
                        opus_int32 n_samples_out;
                        unsigned char* toc_byte = encoded_bytes;
                        ec_enc ent_enc;
                        ec_dec ent_dec;
                        // Encode
                        ec_enc_init(&ent_enc, encoded_bytes + 1, MAX_PACKET - 1);
                        encCtrl.bitRate = bitrate;
                        encCtrl.nChannelsInternal = (stereoEncodeInternal == false) ? 1 : 2;
                        ASSERT_EQ(SMPL_NO_ERROR, smpl_Encode(enc, &encCtrl, pcm_buf + packet * packet_samples, packet_samples/encCtrl.nChannelsAPI, &ent_enc, toc_byte, &encoded_no_bytes, 0, 1));
                        ASSERT_GT(encoded_no_bytes, 0);
                        ASSERT_EQ(encoded_no_bytes, 1 + ((ec_tell(&ent_enc) + 7) >> 3)); // TOC byte + ent coding
                        ec_enc_done(&ent_enc);
                        // Decode
                        ec_dec_init(&ent_dec, encoded_bytes + 1, encoded_no_bytes - 1);
                        n_samples_out = SMPL_ARR_LEN(samples_out);
                        ASSERT_EQ(SMPL_NO_ERROR, smpl_Decode(dec, &decCtrl, 0, 1, &ent_dec, toc_byte[0], samples_out, &n_samples_out));
                        ASSERT_EQ(packet_ms * 16, n_samples_out);
                        ASSERT_EQ(packet_ms, decCtrl.payloadSize_ms);
                        ASSERT_EQ(fs, decCtrl.internalSampleRate);
                        ASSERT_EQ(encCtrl.nChannelsInternal, decCtrl.nChannelsInternal);
                    }
                    free(enc);
                    free(dec);
                }
            }
        }
    }
    smpl_FreeCodec();
    delete[] pcm_buf;
}

TEST(SmplApiTest, HbCrash)
{
    void* enc = NULL;
    void* dec = NULL;
    unsigned int max_no_seconds = 100;
    unsigned int fs = 16000;
    ASSERT_EQ(SMPL_NO_ERROR, smpl_CreateCodec());
    /* Read input audio file. */
    AudioFile<int16_t> wavReader;
    std::string wavfile(__FILE__);
    std::string wavfile_relpath("/../../../../test_signal_wb.wav");
    std::string cppfilename("test_smpl_enc_api.cpp");
    int pos = wavfile.find(cppfilename, 0);
    wavfile.replace(pos, wavfile_relpath.length(), wavfile_relpath);
    wavReader.load(wavfile);
    assert(wavReader.isMono()); // mono only for now.
    assert(fs == wavReader.getSampleRate());
    int tot_samples = SMPL_min(wavReader.getNumSamplesPerChannel(), max_no_seconds * fs);
    opus_int16* pcm_buf = wavReader.samples.front().data();

    fs = 48000;

    int bitrate = 32000;
    int packet_ms = 60;
    smpl_EncControlStruct encCtrl;
    int enc_size;
    smpl_Get_Encoder_Size(&enc_size);
    enc = malloc(enc_size);
    setDefaultEncControl(&encCtrl);
    encCtrl.API_sampleRate = fs;
    encCtrl.payloadSize_ms = 60;
    encCtrl.nChannelsAPI = 1;
    encCtrl.useDTX = 1;
    ASSERT_EQ(SMPL_NO_ERROR, smpl_InitEncoder(enc, &encCtrl));
    int packet_samples = packet_ms * (fs / 1000);
    int packets = SMPL_min(tot_samples / packet_samples, (max_no_seconds * 1000) / packet_ms);

    for (int packet = 0; packet < packets; packet++) {
        unsigned char encoded_bytes[MAX_PACKET];
        opus_int16 samples_out[SMPL_MAX_FB_PACKET_SIZE_IN_SAMPLES];
        opus_int32 encoded_no_bytes;
        opus_int32 n_samples_out;
        unsigned char* toc_byte = encoded_bytes;
        ec_enc ent_enc;
        // Encode
        ec_enc_init(&ent_enc, encoded_bytes + 1, MAX_PACKET - 1);
        encCtrl.bitRate = bitrate;
        encCtrl.nChannelsInternal = 1;
        ASSERT_EQ(SMPL_NO_ERROR, smpl_Encode(enc, &encCtrl, pcm_buf + packet * packet_samples, packet_samples / encCtrl.nChannelsAPI, &ent_enc, toc_byte, &encoded_no_bytes, 0, 1));
        ec_enc_done(&ent_enc);

        if (encoded_no_bytes > 1) {
            smpl_TOC toc;
            smpl_decode_toc(*toc_byte, &toc);
            if (toc.SID) {
                // Change bitrate to low rate
                bitrate = 6000;
                ((smpl_encoder*)enc)->smpl_core_encoder_state->hb_state->prev_hb_lpc_ix = hb_lpc_vq_sizes[0][toc.low_rate] - 1;
            }
        }
        ec_enc_done(&ent_enc);
    }
    free(enc);
    smpl_FreeCodec();
}