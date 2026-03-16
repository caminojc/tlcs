#include <gtest/gtest.h>

#include "smpl_plc.h"
#include "smpl_defines.h"
#include "smpl_errors.h"
#include "smpl_codec_util.h"


TEST(SmplPLC, UpdateCelpZero)
{
    int num_subframes  = 4;
    int subfrlen       = 80;

    PLC plc;
    memset(&plc, 0, sizeof(PLC));

    LbQuantParams lb_params;
    memset(&lb_params, 0, sizeof(LbQuantParams));

    float A[SMPL_MAX_N_SUBFR][SMPL_LPC_ORDER + 1] {};
    float lsfs[SMPL_MAX_N_SUBFR][SMPL_LPC_ORDER] {};
    float acb_gains[SMPL_MAX_N_SUBFR][SMPL_ACBG_M] {};
    float lags[SMPL_PITCH_NUM_SUBFRAMES] {};

    smpl_plc_update_celp(&plc, &lb_params, &acb_gains[0][0], A[num_subframes - 1], lsfs[num_subframes - 1],
                                   lags, SMPL_PITCH_NUM_SUBFRAMES, num_subframes, subfrlen);
}

TEST(SmplPLC, UpdateCelp)
{
    int num_subframes  = 4;
    int subfrlen       = 80;
    int lags_per_frame = 8;

    PLC plc;
    memset(&plc, 0, sizeof(PLC));

    LbQuantParams lb_params;
    memset(&lb_params, 0, sizeof(LbQuantParams));

    float A[SMPL_MAX_N_SUBFR][SMPL_LPC_ORDER + 1] {};
    float lsfs[SMPL_MAX_N_SUBFR][SMPL_LPC_ORDER] {};
    float acb_gains[SMPL_MAX_N_SUBFR][SMPL_ACBG_M] = {
        { 0.3f,  0.7f},
        {-0.7f,  0.3f},
        { 1.5f,  0.0f},
        { 0.0f,  0.0f},
    };
    float lags[SMPL_PITCH_NUM_SUBFRAMES] {};
    for (int i = 0; i < SMPL_PITCH_NUM_SUBFRAMES; i++) {
        lags[i] = 1.0f;
    }

    smpl_plc_update_celp(&plc, &lb_params, &acb_gains[0][0], A[num_subframes - 1], lsfs[num_subframes - 1],
                                   lags, lags_per_frame, num_subframes, subfrlen);

    for (int i = 0; i < lags_per_frame; i++) {
        EXPECT_EQ(plc.lag_buf[i], 0);
        EXPECT_EQ(plc.lag_buf[i + lags_per_frame], 1);
    }

    for (int i = 0; i < num_subframes-1; i++)
    {
        EXPECT_EQ(plc.acb_buf[i], 0.95f);
    }
    EXPECT_EQ(plc.acb_buf[num_subframes-1], 0.7f);
}

TEST(SmplPLC, UpdateHb)
{
    PLC plc;
    memset(&plc, 0, sizeof(PLC));

    HbQuantParams hb_params;

    float A_hb[SMPL_HB_LPC_ORDER + 1];
    for (int i = 0; i < SMPL_HB_LPC_ORDER + 1; i++) {
        A_hb[i] = (float)i;
    }
    float hb_gain = 1.0f;

    smpl_plc_update_hb(&plc, A_hb, hb_gain);

    for (int i = 0; i < SMPL_HB_LPC_ORDER + 1; i++) {
        EXPECT_EQ(A_hb[i], plc.A_hb[i]);
    }
    EXPECT_EQ(hb_gain, plc.hb_gain);
}

TEST(SmplPLC, ConcealCelp)
{
    int num_subframes = 4;
    int subfrlen = 80;
    int lags_per_frame = 8;

    PLC plc;
    memset(&plc, 0, sizeof(PLC));

    LbQuantParams lb_params;
    memset(&lb_params, 0, sizeof(LbQuantParams));

    float A[SMPL_MAX_N_SUBFR][SMPL_LPC_ORDER + 1]  = {};
    float lsfs[SMPL_MAX_N_SUBFR][SMPL_LPC_ORDER]   = {};
    float acb_gains[SMPL_MAX_N_SUBFR][SMPL_ACBG_M] = {};
    float lags[SMPL_PITCH_NUM_SUBFRAMES]           = {};

    // check PLC gives valid filters when no packet has been received yet
    smpl_plc_conceal_celp(
        &plc,
        &lb_params,
        &acb_gains[0][0],
        &A[0][0],
        &lsfs[0][0],
        num_subframes,
        subfrlen,
        lags);
    for (int num_subframe = 0; num_subframe < num_subframes; num_subframe++) {
        EXPECT_EQ(A[num_subframe][0], 1.0f);
        for (int i = 0; i < SMPL_LPC_ORDER; i++) {
            EXPECT_EQ(plc.A[i+1], 0.0f);
        }
        EXPECT_EQ(A[num_subframe][0], 1.0f);
        for (int i = 0; i < SMPL_HB_LPC_ORDER; i++) {
            EXPECT_EQ(plc.A_hb[i+1], 0.0f);
        }
    }

    plc.A[0] = 1.0f;
    plc.A[1] = 1.0f;
    for (int i = 0; i < SMPL_LPC_ORDER; i++)
    {
        plc.lsf[i] = 1.0f;
    }
    plc.precise_lag = 100.0f;
    for (int i = 0; i < SMPL_MAX_PITCH_LAG / SMPL_MIN_SF_LEN; i++)
    {
        plc.acb_buf[i] = 1.0f;
    }

    // test bandwidth expansion and state copy
    smpl_plc_conceal_celp(
        &plc,
        &lb_params,
        &acb_gains[0][0],
        &A[0][0],
        &lsfs[0][0],
        num_subframes,
        subfrlen,
        lags);

    for (int j = 0; j < SMPL_MAX_N_SUBFR; j++)
    {
        EXPECT_EQ(A[j][0], 1.0f);
        EXPECT_EQ(A[j][1], pow(SMPL_PLC_BWE_UV, (float)(j+1)));
        for (int i = 0; i < SMPL_LPC_ORDER-1; i++)
        {
            EXPECT_EQ(A[j][i + 2], 0);
        }
    }
    for (int j = 0; j < SMPL_MAX_N_SUBFR; j++)
    {
        for (int i = 0; i < SMPL_LPC_ORDER; i++)
        {
            EXPECT_EQ(plc.lsf[i], lsfs[j][i]);
        }
    }

    // test pitch lag increment
    float lags_expected[8] = {
        101.0f,
        102.0f,
        103.0f,
        104.0f,
        105.0f,
        106.0f,
        107.0f,
        108.5f
    };
    for (int i = 0; i < lags_per_frame; i++)
    {
        EXPECT_EQ(lags[i], lags_expected[i / 2]);
    }
    smpl_plc_conceal_celp(
        &plc,
        &lb_params,
        &acb_gains[0][0],
        &A[0][0],
        &lsfs[0][0],
        num_subframes,
        subfrlen,
        lags);
    for (int i = 0; i < lags_per_frame; i++)
    {
        EXPECT_EQ(lags[i], lags_expected[4 + i / 2]);
    }

    // test pitch lag upper limit
    for (int i = 0; i < 50; i++)
    {
        smpl_plc_conceal_celp(
            &plc,
            &lb_params,
            &acb_gains[0][0],
            &A[0][0],
            &lsfs[0][0],
            num_subframes,
            subfrlen,
            lags);
    }
    EXPECT_EQ(plc.precise_lag, SMPL_MAX_PITCH_LAG);
}


TEST(SmplPLC, ConcealHb)
{
    PLC plc;
    int num_hb_subframes = 4;
    float A_hb_expected[SMPL_MAX_N_SUBFR][SMPL_HB_LPC_ORDER + 1];
    for (int i = 0; i < SMPL_HB_LPC_ORDER + 1; i++) {
        plc.A_hb[i] = (float)(i + 1);
    }
    for (int i = 0; i < num_hb_subframes; i++) {
        memcpy(&A_hb_expected[0][0] + i * (SMPL_HB_LPC_ORDER + 1), plc.A_hb, (SMPL_HB_LPC_ORDER + 1) * sizeof(float));
    }
    plc.hb_gain = 1.0f;
    plc.hb_gain_attenuation = 1.0f;
    float hb_gains[SMPL_MAX_HB_SUBFR];
    HbQuantParams hb_params;
    float A_hb[SMPL_MAX_N_SUBFR][SMPL_HB_LPC_ORDER + 1];

    smpl_plc_conceal_hb(&plc, &A_hb[0][0], hb_gains, num_hb_subframes);

    float hb_gains_expected[SMPL_MAX_HB_SUBFR] = { 0.8f, 0.64f,  0.512f, 0.4096f };
    for (int i = 0; i < num_hb_subframes; i++) {
        for (int j = 0; j < SMPL_HB_LPC_ORDER + 1; j++) {
            EXPECT_NEAR(A_hb_expected[i][j], A_hb[i][j], 1e-6);
        }
        EXPECT_NEAR(hb_gains[i], hb_gains_expected[i], 1e-6);
    }
}

void check_fvec_val(const float x[], int len, float val, float eps) {
    for (int i = 0; i < len; i++) {
        EXPECT_NEAR(x[i], val, eps);
    }
}

void check_fvec_zero(const float x[], int len, float eps) {
    check_fvec_val(x, len, 0.0f, eps);
}

// PLC module is responsible for sourcing DTX packets so testing that here
TEST(SmplPLC, ConcealCelpDtx)
{
    PLC plc;
    plc.toc.SID = SMPL_TRUE;
    for (int i = 0; i < SMPL_LPC_ORDER; i++) {
        plc.lsf[i] = 0.1f * i;
    }

    for (auto lowrate : {SMPL_FALSE, SMPL_TRUE}) {
        for (auto framesize : {10, 20}) {
            LbQuantParams lb_params;
            float A[SMPL_MAX_N_SUBFR][SMPL_LPC_ORDER + 1];
            float lsfs[SMPL_MAX_N_SUBFR][SMPL_LPC_ORDER];
            float lags[SMPL_PITCH_NUM_SUBFRAMES];
            float lsf_prev[SMPL_LPC_ORDER];
            memset(lsf_prev, 0, sizeof(float) * SMPL_LPC_ORDER);

            int num_subframes = framesize / (lowrate + 1) / 5;
            int num_lags = framesize * 16 / SMPL_LAG_SUBFRLEN;
            smpl_plc_conceal_celp_dtx(&plc, &lb_params, lsf_prev, &A[0][0], &lsfs[0][0], num_subframes, lags, num_lags);
            for (int i = 0; i < SMPL_LPC_ORDER; i++) {
                EXPECT_EQ(lsfs[num_subframes-1][i], lsf_prev[i]);
            }
            memset(lsf_prev, 0, sizeof(float) * SMPL_LPC_ORDER);

            EXPECT_EQ(lb_params.n_pulses, 0);
            check_fvec_zero(lags, num_lags, 1e-30);
            for (int num_subframe = 0; num_subframe < num_subframes; num_subframe++) {
                EXPECT_EQ(lb_params.fcbg_idx[num_subframe], 0);
                EXPECT_EQ(lb_params.sf_pulses[num_subframe], 0);

                int different = SMPL_FALSE;
                for (int i = 0; i < SMPL_LPC_ORDER; i++) {
                    if (lsf_prev[i] != lsfs[num_subframe][i]) {
                        different = SMPL_TRUE;
                        break;
                    }
                }
                ASSERT_TRUE(different);
            }
        }
    }
}

TEST(SmplPLC, ConcealHbDtx)
{
    PLC plc;
    memset(&plc, 0, sizeof(PLC));
    plc.toc.SID = SMPL_TRUE;
    plc.hb_gain = 0;
    for (auto lowrate : { SMPL_FALSE, SMPL_TRUE }) {
        for (auto framesize : { 10, 20 }) {
            HbQuantParams hb_params;
            float A_hb[SMPL_MAX_N_SUBFR][SMPL_HB_LPC_ORDER + 1];
            int num_subframes = framesize / (lowrate + 1) / 5;
            int num_hb_subframes = framesize * 16 / SMPL_HB_SF_LEN;
            float hb_gains[SMPL_MAX_HB_SUBFR];

            smpl_plc_conceal_hb_dtx(&plc, &A_hb[0][0], hb_gains, num_subframes, num_hb_subframes);
            check_fvec_zero(hb_gains, num_hb_subframes, 1e-30);
            for (int num_subframe = 0; num_subframe < num_subframes; num_subframe++) {
                check_fvec_zero(A_hb[num_subframe], SMPL_HB_LPC_ORDER + 1, 1e-30);
            }
        }
    }
}

TEST(SmplPLC, AcbGainSearch)
{
    float lags[SMPL_PITCH_NUM_SUBFRAMES];
    float acb_gains[SMPL_MAX_N_SUBFR][SMPL_ACBG_M];
    float A[SMPL_MAX_N_SUBFR][SMPL_LPC_ORDER + 1];
    float lsfs[SMPL_MAX_N_SUBFR][SMPL_LPC_ORDER];

    int num_subframes  = 4;
    int subfrlen       = 80;

    int buf_len = SMPL_MAX_PITCH_LAG / subfrlen;
    int lag_buf_len = 2 * SMPL_MAX_PITCH_LAG / SMPL_LAG_SUBFRLEN;
    PLC plc;
    memset(&plc, 0, sizeof(PLC));
    plc.precise_lag = SMPL_MAX_PITCH_LAG - 1.0f;
    plc.voiced = SMPL_TRUE;

    // max at end
    for (size_t i = 0; i < buf_len; i++) {
        plc.acb_buf[i] = 1.0f;
    }
    for (size_t i = 0; i < buf_len; i++) {
        plc.exc_nrg_buf[i] = (float)i;
    }
    plc.acb_buf[buf_len - 1] = 0.0f;
    
    LbQuantParams lb_params;
    memset(&lb_params, 0, sizeof(LbQuantParams));

    smpl_plc_conceal_celp(
        &plc,
        &lb_params,
        &acb_gains[0][0],
        &A[0][0],
        &lsfs[0][0],
        num_subframes,
        subfrlen,
        lags);
    for (int i = 0; i < SMPL_MAX_N_SUBFR; i++) {
        for (int j = 1; j < SMPL_ACBG_M; j++) {
            EXPECT_EQ(acb_gains[i][j], 0);
        }
    }
    EXPECT_LE(acb_gains[SMPL_MAX_N_SUBFR - 1][0], 0.95f);

    // max at start
    plc.precise_lag = 280.0f;
    for (size_t i = 0; i < buf_len; i++) {
        plc.acb_buf[i] = 1.0f;
    }
    for (size_t i = 0; i < buf_len; i++) {
        plc.exc_nrg_buf[buf_len - i - 1] = (float)i;
    }
    plc.acb_buf[0] = 0.0f;

    smpl_plc_conceal_celp(
        &plc,
        &lb_params,
        &acb_gains[0][0],
        &A[0][0],
        &lsfs[0][0],
        num_subframes,
        subfrlen,
        lags);
    for (int i = 0; i < SMPL_MAX_N_SUBFR; i++) {
        for (int j = 1; j < SMPL_ACBG_M; j++) {
            EXPECT_EQ(acb_gains[i][j], 0);
        }
    }
    EXPECT_LE(acb_gains[SMPL_MAX_N_SUBFR - 1][0], 0.5f);

    // resets
    memset(lags, 0, sizeof(lags));
    memset(&acb_gains[0][0], 0, SMPL_MAX_N_SUBFR * SMPL_ACBG_M * sizeof(float));
    smpl_plc_update_celp(&plc, &lb_params, &acb_gains[0][0], A[num_subframes - 1], lsfs[num_subframes - 1],
                               lags, SMPL_PITCH_NUM_SUBFRAMES, num_subframes, subfrlen);
    check_fvec_val(plc.acb_buf, buf_len, SMPL_PLC_ACB_MAX, 1e-6f);
    check_fvec_zero(plc.exc_nrg_buf, buf_len, 1e-6f);
    check_fvec_zero(plc.lag_buf, lag_buf_len, 1e-6f);
}

#define LTP_BUF_LEN 2 * SMPL_MAX_PITCH_LAG + SMPL_LTP_INTERPOL_DELAY

TEST(SmplPLC, LtpBlend)
{
    PLC plc;
    memset(&plc, 0, sizeof(PLC));
    plc.voiced = SMPL_TRUE;
    int lag_buf_len = 2 * SMPL_MAX_PITCH_LAG / SMPL_LAG_SUBFRLEN;
    float ltp[LTP_BUF_LEN];
    memset(ltp, 0, sizeof(ltp));

    // non-fractional case
    for (size_t i = 0; i < lag_buf_len; i++)
    {
        plc.lag_buf[i] = 40.0f;
    }

    float ltp_state[40] = {
	    0.4453157739364415, -0.6227901520109183, 0.2666379327329227,
	    -0.597681949437177, 0.6820479779786313, 0.4148069270541792,
	    -0.015866618690065115, 0.6057754544402378, 0.526291624607647,
	    -0.8996407371862967, 0.6702771363979014, 0.545888149744618,
	    0.34359206334441894, -0.7331201040626658, 0.07599387434566895,
	    -0.9498960107020018, 0.3198850437434473, -0.08656738620357385,
	    -0.12580780291788063, 0.5037136295308964, -0.5930580228972502,
	    -0.36660279136687235, -0.41561394135017515, -0.9585363098466406,
	    0.6340736565294169, 0.07614798000161205, 0.6127989621153822,
	    -0.0332422430938315, -0.52929625071036, -0.7782301412839838,
	    0.32817273761281607, -0.15171142132667104, 0.9306386473001342,
	    -0.8242822717116467, -0.5651445192288762, 0.9723773783106788,
	    0.5658689126794203, 0.21995650315810344, -0.2468904574579487,
	    -0.160931051392061
    };

    for (size_t i = 0; i < 40; i++)
    {
        ltp[LTP_BUF_LEN - i - 1] = ltp_state[i];
    }
    
    float lag = 40.0f;
    float blend_nrg = 3.1100f;
    smpl_plc_blend_ltp(&plc, ltp, lag);
    EXPECT_NEAR(smpl_nrg(ltp, LTP_BUF_LEN), blend_nrg, 1e-1 * blend_nrg);

    // fractional case
    lag = 40.5f;
    blend_nrg = 0.7753f;
    smpl_plc_blend_ltp(&plc, ltp, lag);
    EXPECT_NEAR(smpl_nrg(ltp, LTP_BUF_LEN), blend_nrg, 1e-1 * blend_nrg);
}

TEST(SmplPLC, UpdateCng)
{
    PLC plc;
    memset(&plc, 0, sizeof(PLC));
    int numsubfrs = 4;
    float y[SMPL_FRAME_LEN];
    memset(y, 0, sizeof(y));
    int subfrlen = SMPL_MIN_SF_LEN;
    LbQuantParams lb_params;
    for (int i = 0; i < numsubfrs; i++) {
        lb_params.nrgres[i] = (float)(i+1);
    }
    int active = SMPL_FALSE;
    int swb = SMPL_TRUE;
    float lsfs[MAX_NUM_SUBFR][SMPL_LPC_ORDER];
    memset(lsfs, 0, sizeof(lsfs));

    float* pLsf = &lsfs[0][0];
    for (int i = 0; i < MAX_NUM_SUBFR*SMPL_LPC_ORDER; i++) {
        pLsf[i] = (float)(i + 1);
    }

    float A_hb[MAX_NUM_SUBFR][SMPL_HB_LPC_ORDER + 1];
    float* pA_hb = &A_hb[0][0];
    for (int i = 0; i < MAX_NUM_SUBFR*(SMPL_HB_LPC_ORDER+1); i++) {
        pA_hb[i] = (float)(i + 1);
    }

    float hb_exc_gains[MAX_NUM_SUBFR];
    for (int i = 0; i < numsubfrs; i++) {
        hb_exc_gains[i] = (float)(i + 1);
    }

    lb_params.voiced = SMPL_TRUE;
    smpl_plc_update_cng(&plc, y, &lb_params, y, swb, active, active, numsubfrs, subfrlen, pLsf, pA_hb, hb_exc_gains);
    EXPECT_EQ(plc.cng.num_candidates, 0);
    lb_params.voiced = SMPL_FALSE;
    smpl_plc_update_cng(&plc, y, &lb_params, y, swb, active, active, numsubfrs, subfrlen, pLsf, pA_hb, hb_exc_gains);
    EXPECT_EQ(plc.cng.num_candidates, 1);

    plc.cng.cng_candidates[0].frame_nrg = 100.0f;
    for (int i = 0; i < numsubfrs; i++) {
        y[i * subfrlen] = (float)(i + 1);
    }
    smpl_plc_update_cng(&plc, y, &lb_params, y, swb, active, active, numsubfrs, subfrlen, pLsf, pA_hb, hb_exc_gains);
    EXPECT_EQ(plc.cng.best_candidate_idx, 0);

}

TEST(SmplPLC, AddComfortNoise)
{
    PLC plc;
    memset(&plc, 0, sizeof(PLC));
    plc.cng.cng_candidates->nrg_bands[SMPL_LB] = 0.001f;
    float lsfs[SMPL_LPC_ORDER] = {
        0.06567355f, 0.24073912f, 0.4227076f, 0.6320001f, 0.8119552f, 0.93122226f,
        1.1997647f, 1.3951555f, 1.5471156f, 1.761777f, 1.942499f, 2.1490114f,
        2.3533185f, 2.5546534f, 2.7553172f, 2.9322042f };
    for (int i = 0; i < SMPL_LPC_ORDER; i++) {
        plc.cng.cng_candidates->lsfs_lb[i] = lsfs[i];
    }

    float yBuf[SMPL_FRAME_LEN];
    memset(yBuf, 0, sizeof(yBuf));

    // don't add during DTX
    plc.toc.SID = SMPL_TRUE;
    int lostFlag = SMPL_FLAG_PACKET_LOST;
    smpl_add_comfort_noise(&plc, yBuf, SMPL_FRAME_LEN, lostFlag, SMPL_LB);
    EXPECT_NEAR(smpl_nrg(yBuf, SMPL_FRAME_LEN), 0.0f, 1e-9);
    EXPECT_NEAR(smpl_nrg(plc.cng.lb_delay_buf, SMPL_TOT_POSTFILT_DELAY), 0.0f, 1e-9);

    // don't add during normal playout
    plc.toc.SID = SMPL_FALSE;
    lostFlag = SMPL_FLAG_DECODE_NORMAL;
    smpl_add_comfort_noise(&plc, yBuf, SMPL_FRAME_LEN, lostFlag, SMPL_LB);
    EXPECT_NEAR(smpl_nrg(yBuf, SMPL_FRAME_LEN), 0.0f, 1e-9);
    EXPECT_NEAR(smpl_nrg(plc.cng.lb_delay_buf, SMPL_TOT_POSTFILT_DELAY), 0.0f, 1e-9);

    plc.toc.SID  = SMPL_FALSE;
    lostFlag = SMPL_FLAG_PACKET_LOST;
    smpl_add_comfort_noise(&plc, yBuf, SMPL_FRAME_LEN, lostFlag, SMPL_LB);
    EXPECT_GT(smpl_nrg(yBuf, SMPL_FRAME_LEN), 1e-1f);
    float comf_sig_ratio_dB = 10.0f * log10f(plc.comf_sig_ratio);
    EXPECT_GT(comf_sig_ratio_dB, 10);
}
