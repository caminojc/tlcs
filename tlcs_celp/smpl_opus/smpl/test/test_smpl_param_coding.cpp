#include <gtest/gtest.h>
#include "smpl_api.h"
#include "smpl_quant_nrg_res.h"
#include "smpl_param_coding.h"
#include "smpl_entropy_wrapper.h"
#include "smpl_errors.h"
#include "memory.h"
#include "smpl_pitch.h"
#include "smpl_bandwidth_extension.h"

TEST(SmplParamCoding, TOC)
{
    smpl_TOC enc_toc;
    unsigned char toc_byte = 0;
    for (auto SID_VoA : {0, 1, 2}) {
        enc_toc.SID = SID_VoA > 1 ? SMPL_TRUE : SMPL_FALSE;
        enc_toc.VAD = ((SID_VoA & 1) == 1) ? SMPL_TRUE : SMPL_FALSE;
        for (auto fs_Hz : {16000, 32000}) {
            enc_toc.fs_Hz = fs_Hz;
            for (auto coded_as_active_voice : {SMPL_FALSE, SMPL_TRUE}) {
                enc_toc.coded_as_active_voice = coded_as_active_voice;
                if (enc_toc.VAD && !coded_as_active_voice) // Not allowed combination
                    break;
                for (auto FEC : {SMPL_FALSE, SMPL_TRUE}) {
                    enc_toc.FEC = FEC;
                    if (((!coded_as_active_voice) && FEC)||(enc_toc.SID && FEC)||(!enc_toc.VAD && FEC)) // Not allowed combination
                        break;
                    for (auto low_rate : {SMPL_FALSE, SMPL_TRUE}) {
                        enc_toc.low_rate = low_rate; 
                        for (auto stereo : {SMPL_FALSE, SMPL_TRUE}) {
                            enc_toc.stereo = stereo;
                            if ((stereo && FEC)) // Not allowed combination
                                break;
                            for (auto packet_len_ms : {10, 20, 60, 120}) {
                                enc_toc.packet_len_ms = packet_len_ms;
                                toc_byte = smpl_encode_toc(&enc_toc);
                                smpl_TOC dec_toc;
                                smpl_decode_toc(toc_byte, &dec_toc);
                                EXPECT_EQ(enc_toc.SID, dec_toc.SID);
                                EXPECT_EQ(coded_as_active_voice, dec_toc.coded_as_active_voice);
                                EXPECT_EQ(enc_toc.VAD, dec_toc.VAD);
                                EXPECT_EQ(low_rate, dec_toc.low_rate);
                                EXPECT_EQ(fs_Hz, dec_toc.fs_Hz);
                                EXPECT_EQ(packet_len_ms, dec_toc.packet_len_ms);
                                EXPECT_EQ(FEC, dec_toc.FEC);
                                EXPECT_EQ(stereo, dec_toc.stereo);
                            }
                        }
                    }
                }
            }
        }
    }
}

TEST(SmplParamCoding, LbParametersAllZero)
{
    EXPECT_EQ(smpl_CreateCodec(), 0);

    void* ecEnc = smpl_create_ec_encoder(128);
    EXPECT_TRUE(ecEnc != NULL);
    void* ecEncCtx = smpl_ec_get_srange(ecEnc);
    EXPECT_TRUE(ecEncCtx != NULL);
    void* parmEnc = smpl_create_param_encoder();
    EXPECT_TRUE(parmEnc != NULL);
    void* parmDec = smpl_create_param_decoder();
    EXPECT_TRUE(parmDec != NULL);

    int SID = SMPL_FALSE;
    int coded_as_active_voice = SMPL_FALSE;
    int low_rate = SMPL_FALSE;
    int numsubfr = 4;
    int framelen = 320;
    LbQuantParams lb_params;
    memset(&lb_params, 0, sizeof(LbQuantParams));
    float resnrg[SMPL_MAX_N_SUBFR];
    memset(resnrg, 0, SMPL_MAX_N_SUBFR * sizeof(float));
    EXPECT_EQ(smpl_quant_nrg_res(resnrg, numsubfr, &lb_params), 0);
    smpl_encode_lb_params(parmEnc, ecEncCtx, &lb_params, framelen, numsubfr, coded_as_active_voice, SMPL_FALSE, SMPL_FALSE, 0, SMPL_FALSE, low_rate);
    unsigned char payload[128];
    int num_bytes = smpl_ec_enc_done(ecEnc, payload, sizeof(payload));
    void* ecDec = smpl_create_ec_decoder(payload, num_bytes);
    EXPECT_TRUE(ecDec != NULL);
    void* ecDecCtx = smpl_ec_get_srange(ecDec);
    EXPECT_TRUE(ecDecCtx != NULL);

    LbQuantParams lb_params_dec;
    int cond_coding = SMPL_FALSE;
    EXPECT_EQ(smpl_decode_lb_params(parmDec, ecDecCtx, framelen, numsubfr, coded_as_active_voice, &cond_coding, low_rate, 0, SMPL_FALSE, &lb_params_dec), 0);
    EXPECT_EQ(lb_params.voiced, lb_params_dec.voiced);
    for (int i = 0; i < SMPL_LPC_ORDER + 1; i++) {
        EXPECT_EQ(lb_params.lsf_idx[i], lb_params_dec.lsf_idx[i]);
    }
    smpl_free_param_encoder(parmEnc);
    smpl_free_param_decoder(parmDec);
    smpl_free(ecEnc);
    smpl_free(ecDec);

    smpl_FreeCodec();
}

#if 0 // Uses hardcoded indices - needs to be updated, Thi stes twas mainly useful during development
TEST(SmplParamCoding, CelpParametersVoiced)
{
    EXPECT_EQ(smpl_CreateCodec(), 0);

    void* ecEnc = smpl_create_ec_encoder(128);
    EXPECT_TRUE(ecEnc != NULL);
    void* ecEncCtx = smpl_ec_get_srange(ecEnc);
    EXPECT_TRUE(ecEncCtx != NULL);
    void* parmEnc = smpl_create_param_encoder();
    EXPECT_TRUE(parmEnc != NULL);
    void* parmDec = smpl_create_param_decoder();
    EXPECT_TRUE(parmDec != NULL);

    int SID = SMPL_FALSE;
    int coded_as_active_voice = SMPL_TRUE;
    int low_rate = SMPL_FALSE;
    int numsubfr = 4;
    int framelen = 320;
    LbQuantParams lb_params;
    memset(&lb_params, 0, sizeof(lb_params));
    float resnrg[SMPL_MAX_N_SUBFR];
    memset(resnrg, 0, SMPL_MAX_N_SUBFR * sizeof(float));

    lb_params.voiced = SMPL_TRUE;
    int8_t lsf_idx[SMPL_LPC_ORDER + 1] = { 24, 1, 3, 3, 3, 4, 2, 4, 3, 5, 6, 5, 7, 8, 8, 12, 10 };
    memcpy(lb_params.lsf_idx, lsf_idx, (SMPL_LPC_ORDER + 1) * sizeof(int8_t));
    lb_params.lsf_interpol_idx = 0;
    lb_params.pulses[32] = 1; lb_params.pulses[50] = -1; lb_params.pulses[72] = -1; lb_params.pulses[77] = -1;
    lb_params.pulses[80 + 15] = -1; lb_params.pulses[80 + 34] = 1; lb_params.pulses[80 + 47] = 1; lb_params.pulses[80 + 68] = 1; lb_params.pulses[80 + 72] = 1;
    lb_params.pulses[160 + 37] = -1; lb_params.pulses[160 + 43] = -1; lb_params.pulses[160 + 50] = 1; lb_params.pulses[160 + 64] = -1; lb_params.pulses[160 + 67] = 1;
    lb_params.pulses[240 + 13] = 1; lb_params.pulses[240 + 53] = -1; lb_params.pulses[240 + 57] = -1; lb_params.pulses[240 + 66] = -1; lb_params.pulses[240 + 70] = -1;
    int16_t acbg_idx[] = { 2, 1, 2, 11 };
    memcpy(lb_params.acbg_idx, acbg_idx, numsubfr * sizeof(int16_t));
    int16_t fcbg_idx[] = { 1, 3, 5, 7 };
    memcpy(lb_params.fcbg_idx, fcbg_idx, numsubfr * sizeof(int16_t));
    int laginds[] = { 6, 6, 4, 4, 10, 63, 63, 63 };
    memcpy(lb_params.laginds, laginds, SMPL_PITCH_NUM_SUBFRAMES * sizeof(int));
    for (int i = 0; i < NUM_BLOCKSEGS; i++) {
        if (smpl_pitch_blocksegs2idx[i] == 17) {
            lb_params.blocksegs_ix = i;
            break;
        }
    }

    for(int frame = 0; frame < 2; frame ++){
        EXPECT_EQ(smpl_encode_lb_params(parmEnc, ecEncCtx, &lb_params, framelen, numsubfr, coded_as_active_voice, frame == 0 ? SMPL_FALSE : SMPL_TRUE, low_rate, frame, lb_params.voiced), 0);
    }
    unsigned char payload[128];
    int num_bytes = smpl_ec_enc_done(ecEnc, payload, sizeof(payload));

    void* ecDec = smpl_create_ec_decoder(payload, num_bytes);
    EXPECT_TRUE(ecDec != NULL);
    void* ecDecCtx = smpl_ec_get_srange(ecDec);
    EXPECT_TRUE(ecDecCtx != NULL);
    LbQuantParams lb_params_dec;
    for (int frame = 0; frame < 2; frame++) {
        EXPECT_EQ(smpl_decode_lb_params(parmDec, ecDecCtx, framelen, numsubfr, coded_as_active_voice, frame == 0 ? SMPL_FALSE : SMPL_TRUE, low_rate, frame, &lb_params_dec), 0);
        EXPECT_EQ(lb_params.voiced, lb_params_dec.voiced);
        for (int i = 0; i < SMPL_LPC_ORDER + 1; i++) {
            EXPECT_EQ(lb_params.lsf_idx[i], lb_params_dec.lsf_idx[i]);
        }
        for (int i = 0; i < SMPL_FRAME_LEN; i++) {
            EXPECT_EQ(lb_params.pulses[i], lb_params_dec.pulses[i]);
        }
        for (int i = 0; i < numsubfr; i++) {
            EXPECT_EQ(lb_params.acbg_idx[i], lb_params_dec.acbg_idx[i]);
            EXPECT_EQ(lb_params.fcbg_idx[i], lb_params_dec.fcbg_idx[i]);
        }
        EXPECT_EQ(lb_params.blocksegs_ix, lb_params_dec.blocksegs_ix);
        for (int i = 0; i < SMPL_PITCH_NUM_SUBFRAMES; i++) {
            EXPECT_EQ(lb_params.laginds[i], lb_params_dec.laginds[i]);
        }
    }
    smpl_free_param_encoder(parmEnc);
    smpl_free_param_decoder(parmDec);
    smpl_free(ecEnc);
    smpl_free(ecDec);

    smpl_FreeCodec();
}
#endif

TEST(SmplParamCoding, CelpParametersUnoiced)
{
    EXPECT_EQ(smpl_CreateCodec(), 0);

    void* ecEnc = smpl_create_ec_encoder(128);
    EXPECT_TRUE(ecEnc != NULL);
    void* ecEncCtx = smpl_ec_get_srange(ecEnc);
    EXPECT_TRUE(ecEncCtx != NULL);
    void* parmEnc = smpl_create_param_encoder();
    EXPECT_TRUE(parmEnc != NULL);
    void* parmDec = smpl_create_param_decoder();
    EXPECT_TRUE(parmDec != NULL);

    int SID = SMPL_FALSE;
    int coded_as_active_voice = SMPL_TRUE;
    int low_rate = SMPL_FALSE;
    int numsubfr = 4;
    int framelen = 320;
    LbQuantParams lb_params;
    memset(&lb_params, 0, sizeof(lb_params));
    float resnrg[SMPL_MAX_N_SUBFR] = { 0.0005292276f, 0.0004428682f, 0.00024908743f, 0.00015453692f };

    lb_params.voiced = SMPL_FALSE;
    int8_t lsf_idx[SMPL_LPC_ORDER + 1] = { 11, 3, 2, 3, 1, 2, 2, 2, 3, 2, 2, 4, 3, 4, 5, 6, 7 };
    memcpy(lb_params.lsf_idx, lsf_idx, (SMPL_LPC_ORDER + 1) * sizeof(int8_t));
    lb_params.lsf_interpol_idx = 0;
    lb_params.pulses[32] = 1; lb_params.pulses[50] = -1; lb_params.pulses[72] = -1; lb_params.pulses[77] = -1;
    lb_params.pulses[80 + 15] = -1; lb_params.pulses[80 + 34] = 1; lb_params.pulses[80 + 47] = 1; lb_params.pulses[80 + 68] = 1; lb_params.pulses[80 + 72] = 1;
    lb_params.pulses[160 + 37] = -1; lb_params.pulses[160 + 43] = -1; lb_params.pulses[160 + 50] = 1; lb_params.pulses[160 + 64] = -1; lb_params.pulses[160 + 67] = 1;
    lb_params.pulses[240 + 13] = 1; lb_params.pulses[240 + 53] = -1; lb_params.pulses[240 + 57] = -1; lb_params.pulses[240 + 66] = -1; lb_params.pulses[240 + 70] = -1;
    int16_t acbg_idx[] = { 2, 1, 2, 11 };
    memcpy(lb_params.acbg_idx, acbg_idx, numsubfr * sizeof(int16_t));
    int16_t fcbg_idx[] = { 1, 3, 5, 7 };
    memcpy(lb_params.fcbg_idx, fcbg_idx, numsubfr * sizeof(int16_t));
    float res_nrg[] = { 0.0005292276f, 0.0004428682f, 0.00024908743f, 0.00015453692f };

    EXPECT_EQ(smpl_quant_nrg_res(resnrg, numsubfr, &lb_params), 0);
    for (int frame = 0; frame < 2; frame++) {
        smpl_encode_lb_params(parmEnc, ecEncCtx, &lb_params, framelen, numsubfr, coded_as_active_voice, frame == 0 ? SMPL_FALSE : SMPL_TRUE, low_rate, frame, lb_params.voiced, low_rate);
    }
    unsigned char payload[128];
    int num_bytes = smpl_ec_enc_done(ecEnc, payload, sizeof(payload));
    void* ecDec = smpl_create_ec_decoder(payload, num_bytes);
    EXPECT_TRUE(ecDec != NULL);
    void* ecDecCtx = smpl_ec_get_srange(ecDec);
    EXPECT_TRUE(ecDecCtx != NULL);
    LbQuantParams lb_params_dec;
    for (int frame = 0; frame < 2; frame++) {
        int cond_coding = (frame > 0);
        EXPECT_EQ(smpl_decode_lb_params(parmDec, ecDecCtx, framelen, numsubfr, coded_as_active_voice, &cond_coding, low_rate, frame, SMPL_FALSE, &lb_params_dec), 0);
        EXPECT_EQ(lb_params.voiced, lb_params_dec.voiced);
        for (int i = 0; i < SMPL_LPC_ORDER + 1; i++) {
            EXPECT_EQ(lb_params.lsf_idx[i], lb_params_dec.lsf_idx[i]);
        }
        memset(lb_params_dec.pulses, 0, sizeof(lb_params_dec.pulses));
        for (int num_pos = 0; num_pos < lb_params_dec.nPositions; num_pos++) {
            lb_params_dec.pulses[lb_params_dec.positions[num_pos]] = lb_params_dec.pos_pulses[num_pos];
        }
        for (int i = 0; i < SMPL_FRAME_LEN; i++) {
            EXPECT_EQ(lb_params.pulses[i], lb_params_dec.pulses[i]);
        }
        EXPECT_EQ(lb_params.nrgres_frame_qi, lb_params_dec.nrgres_frame_qi);
        EXPECT_EQ(lb_params.nrgres_shape_qi, lb_params_dec.nrgres_shape_qi);
        for (int i = 0; i < numsubfr; i++) {
            EXPECT_EQ(lb_params.nrgres_dbq_Q14[i], lb_params_dec.nrgres_dbq_Q14[i]);
        }
        for (int i = 0; i < numsubfr; i++) {
            EXPECT_EQ(lb_params.fcbg_idx[i], lb_params_dec.fcbg_idx[i]);
        }
    }
    smpl_free_param_encoder(parmEnc);
    smpl_free_param_decoder(parmDec);
    smpl_free(ecEnc);
    smpl_free(ecDec);

    smpl_FreeCodec();
}

TEST(SmplParamCoding, HbParametersAllZero)
{
    EXPECT_EQ(smpl_CreateCodec(), 0);

    void* ecEnc = smpl_create_ec_encoder(128);
    EXPECT_TRUE(ecEnc != NULL);
    void* ecEncCtx = smpl_ec_get_srange(ecEnc);
    EXPECT_TRUE(ecEncCtx != NULL);
    void* parmEnc = smpl_create_param_encoder();
    EXPECT_TRUE(parmEnc != NULL);
    void* parmDec = smpl_create_param_decoder();
    EXPECT_TRUE(parmDec != NULL);
    void* pHe = smpl_create_hb_encoder();
    EXPECT_TRUE(pHe != NULL);
    EXPECT_TRUE(smpl_load_hb_lsf_CBks != NULL);
    EXPECT_TRUE(smpl_load_hb_gain_CBks != NULL);

    int voiced = SMPL_TRUE;
    int low_rate = SMPL_FALSE;
    int frame_length_16 = 20 * 16;

    HbQuantParams hb_params;
    memset(&hb_params, 0, sizeof(hb_params));
    smpl_encode_hb_params(parmEnc, ecEncCtx, pHe, &hb_params, frame_length_16, voiced, SMPL_FALSE, low_rate);

    unsigned char payload[128];
    int num_bytes = smpl_ec_enc_done(ecEnc, payload, sizeof(payload));
    void* ecDec = smpl_create_ec_decoder(payload, num_bytes);
    EXPECT_TRUE(ecDec != NULL);
    void* ecDecCtx = smpl_ec_get_srange(ecDec);
    EXPECT_TRUE(ecDecCtx != NULL);

    HbQuantParams hb_params_dec;
    smpl_decode_hb_params(parmDec, ecDecCtx, frame_length_16, voiced, SMPL_FALSE, low_rate, &hb_params_dec);
    EXPECT_EQ(hb_params.lsf_idx, hb_params_dec.lsf_idx);
    EXPECT_EQ(hb_params.gain_qi, hb_params_dec.gain_qi);

    smpl_free_param_encoder(parmEnc);
    smpl_free_param_decoder(parmDec);
    smpl_free_hb_encoder(pHe);
    smpl_free(ecEnc);
    smpl_free(ecDec);
    smpl_free_hb_lsf_CBks();
    smpl_free_hb_gain_CBks();

    smpl_FreeCodec();
}

TEST(SmplParamCoding, HbParameters)
{
    EXPECT_EQ(smpl_CreateCodec(), 0);

    void* ecEnc = smpl_create_ec_encoder(128);
    EXPECT_TRUE(ecEnc != NULL);
    void* ecEncCtx = smpl_ec_get_srange(ecEnc);
    EXPECT_TRUE(ecEncCtx != NULL);
    void* parmEnc = smpl_create_param_encoder();
    EXPECT_TRUE(parmEnc != NULL);
    void* parmDec = smpl_create_param_decoder();
    EXPECT_TRUE(parmDec != NULL);
    void* pHe = smpl_create_hb_encoder();
    EXPECT_TRUE(pHe != NULL);
    EXPECT_TRUE(smpl_load_hb_lsf_CBks != NULL);
    EXPECT_TRUE(smpl_load_hb_gain_CBks != NULL);

    int voiced = SMPL_TRUE;
    int low_rate = SMPL_TRUE;
    int frame_length_16 = 20 * 16;

    HbQuantParams hb_params;
    hb_params.lsf_idx = 10;
    hb_params.gain_qi = 10;

    for (int frame = 0; frame < 2; frame++) {
        smpl_encode_hb_params(parmEnc, ecEncCtx, pHe, &hb_params, frame_length_16, voiced, frame == 0 ? SMPL_FALSE : SMPL_TRUE, low_rate);
    }

    unsigned char payload[128];
    int num_bytes = smpl_ec_enc_done(ecEnc, payload, sizeof(payload));
    void* ecDec = smpl_create_ec_decoder(payload, num_bytes);
    EXPECT_TRUE(ecDec != NULL);
    void* ecDecCtx = smpl_ec_get_srange(ecDec);
    EXPECT_TRUE(ecDecCtx != NULL);
    HbQuantParams hb_params_dec;
    for (int frame = 0; frame < 2; frame++) {
        smpl_decode_hb_params(parmDec, ecDecCtx, frame_length_16, voiced, frame == 0 ? SMPL_FALSE : SMPL_TRUE, low_rate, &hb_params_dec);
        EXPECT_EQ(hb_params.lsf_idx, hb_params_dec.lsf_idx);
        EXPECT_EQ(hb_params.gain_qi, hb_params_dec.gain_qi);
    }

    smpl_free_param_encoder(parmEnc);
    smpl_free_param_decoder(parmDec);
    smpl_free_hb_encoder(pHe);
    smpl_free(ecEnc);
    smpl_free(ecDec);
    smpl_free_hb_lsf_CBks();
    smpl_free_hb_gain_CBks();

    smpl_FreeCodec();
}
