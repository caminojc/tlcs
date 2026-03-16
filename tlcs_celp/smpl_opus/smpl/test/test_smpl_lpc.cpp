#include <gtest/gtest.h>
#include "smpl_lpc.h"
#include "smpl_defines.h"
#include "smpl_errors.h"
#include "memory.h"
#include "stop_watch.h"
#include <math.h>
#include <fenv.h>

TEST(SmplLPC, ZeroInput)
{
    EXPECT_TRUE(smpl_create_lpc_tables() != nullptr);

    float x[SMPL_LPC_NFFT / 2], F2[SMPL_LPC_NFFT / 2 + 1], A[SMPL_LPC_ORDER + 1];
    double R[SMPL_LPC_ORDER + 1];
    memset(x, 0, sizeof(x));
    smpl_lpc(x, SMPL_LPC_NFFT/2, 0.0001f, A, R, SMPL_LPC_ORDER, F2);
    smpl_lpc(x, SMPL_LPC_NFFT/2, 0.0001f, A, R, SMPL_HB_LPC_ORDER, F2);

    smpl_free_lpc_tables();
}

TEST(SmplLPC, IsStable)
{
    // julia A = rc2a(Float32[-0.990308, 0.9598116, -0.30836773, 0.4134152, -0.19884486, 0.30501714, 0.1257538, 0.091157734, -0.100199886, 0.08663391, -0.06282103, 0.072409086, 0.003585337, 0.11098788, 0.042927843, 0.002855442])
    float A_stable[SMPL_LPC_ORDER + 1] = { 1.0f, -2.4795735f, 2.6683617f, -2.1639912f, 1.704892f, -1.1356078f, 0.730818f, -0.75420743f, 0.851308f, -0.717573f, 0.56512207f, -0.4010401f, 0.27149433f, -0.16333361f, 0.0119546605f, 0.035847213f, 0.002855442f };
    EXPECT_EQ(smpl_lpc_is_stable(A_stable, SMPL_LPC_ORDER), SMPL_TRUE);
    float A_tmp[SMPL_LPC_ORDER + 1];
    memcpy(A_tmp, A_stable, sizeof(A_stable));
    smpl_lpc_stabilize(A_stable, SMPL_LPC_ORDER);
    for (int i = 0; i < SMPL_LPC_ORDER + 1; i++) {
        EXPECT_EQ(A_tmp[i], A_stable[i]);
    }

    // julia A = rc2a(Float32[-1.1, 0.9598116, -0.30836773, 0.4134152, -0.19884486, 0.30501714, 0.1257538, 0.091157734, -0.100199886, 0.08663391, -0.06282103, 0.072409086, 0.003585337, 0.11098788, 0.042927843, 0.002855442])
    float A_unstable[SMPL_LPC_ORDER + 1] = { 1.0f, -2.518166f, 2.6896322f, -2.189343f, 1.7187061f, -1.1500429f, 0.73287326f, -0.764314f, 0.85914904f, -0.72546345f, 0.56988746f, -0.40598556f, 0.27268684f, -0.16676809f, 0.0103587145f, 0.035737015f, 0.002855442f };
    EXPECT_EQ(smpl_lpc_is_stable(A_unstable, SMPL_LPC_ORDER), SMPL_FALSE);
    smpl_lpc_stabilize(A_unstable, SMPL_LPC_ORDER);
    EXPECT_EQ(smpl_lpc_is_stable(A_unstable, SMPL_LPC_ORDER), SMPL_TRUE);
}

static float max_abs(float x[], int L)
{
    float maxAbs = 0.0f;
    for (int i = 0; i < L; i++) {
        maxAbs = SMPL_max(SMPL_abs(x[i]), maxAbs);
    }
    return maxAbs;
}

TEST(SmplLPC, IsStableExtensive)
{
    srand(9769769); // Avoid flakiness
    float rc[SMPL_LPC_ORDER];
    float A[SMPL_LPC_ORDER + 1];
    int cnt = 0;
    while(1) {
        for (int i = 0; i < SMPL_LPC_ORDER; i++) {
            rc[i] = ((rand() / (float)RAND_MAX) -0.5f) * 2.0f * 2.5f; // Generate some potentially unstable filters
        }
        if (max_abs(rc, SMPL_LPC_ORDER) > 2.0f) {
            smpl_rc2a(rc, SMPL_LPC_ORDER, A);
            EXPECT_TRUE(smpl_lpc_is_stable(A, SMPL_LPC_ORDER) == SMPL_FALSE);
            cnt++;
        }
        if (cnt == 1000) {
            break;
        }
    }
    for (cnt = 0; cnt < 1000; cnt++) {
        for (int i = 0; i < SMPL_LPC_ORDER; i++) {
            rc[i] = ((rand() / (float)RAND_MAX) -0.5f) * 2.0f * 0.7f; // Generate some stable filters
        }
        smpl_rc2a(rc, SMPL_LPC_ORDER, A);
        EXPECT_TRUE(smpl_lpc_is_stable(A, SMPL_LPC_ORDER) == SMPL_TRUE);
    }
}

#define NLOOPS  1000

TEST(SmplLPC, LpcInterpol)
{
    int num_subfr = SMPL_MAX_N_SUBFR;
    float lsf[SMPL_LPC_ORDER] = { 0.0735259, 0.29285735, 0.43110102, 0.6383648, 0.8231502, 0.97898674, 1.2236058, 1.3776716, 1.5576458, 1.7235053, 1.9339328, 2.1402185, 2.364583, 2.5487657, 2.75664, 2.9335337 };
    float prev_lsf[SMPL_LPC_ORDER] = { 0.06523302, 0.21730773, 0.4164494, 0.62378794, 0.8068097, 0.9125941, 1.1952778, 1.4006382, 1.570373, 1.7476214, 1.9602344, 2.1680555, 2.3626451, 2.5562344, 2.747417, 2.927669 };
    float lsf_interpol[SMPL_MAX_N_SUBFR] = { 0.55, 0.88, 1.0, 1.0 };
    float A[SMPL_MAX_N_SUBFR][SMPL_LPC_ORDER + 1];
    float lsfs[SMPL_MAX_N_SUBFR][SMPL_LPC_ORDER];
    float expectA[SMPL_MAX_N_SUBFR][SMPL_LPC_ORDER + 1] = {
        {1.0, -0.96606445, 0.1184082, 0.00048828125, -0.078125, 0.022949219, 0.07470703, -0.12939453, -0.023925781, 0.020019531, 0.08105469, -0.0009765625, 0.0068359375, -0.07299805, -0.021728516, -0.046875, 0.06616211},
        {1.0, -0.95874023, 0.17749023, -0.010009766, -0.1315918, 0.04296875, 0.084228516, -0.109375, -0.010009766, 0.005126953, 0.046142578, 0.0041503906, 0.014160156, -0.075683594, -0.012939453, -0.076416016, 0.06713867},
        {1.0, -0.9555664, 0.19873047, -0.013427734, -0.15014648, 0.05126953, 0.08691406, -0.1027832, -0.005126953, 0.00024414062, 0.033935547, 0.0056152344, 0.016113281, -0.07763672, -0.009765625, -0.08691406, 0.06713867},
        {1.0, -0.9555664, 0.19873047, -0.013427734, -0.15014648, 0.05126953, 0.08691406, -0.1027832, -0.005126953, 0.00024414062, 0.033935547, 0.0056152344, 0.016113281, -0.07763672, -0.009765625, -0.08691406, 0.06713867}
    };
    float expect_prev_lsf[SMPL_LPC_ORDER] = { 0.0735259, 0.29285735, 0.43110102, 0.6383648, 0.8231502, 0.97898674, 1.2236058, 1.3776716, 1.5576458, 1.7235053, 1.9339328, 2.1402185, 2.364583, 2.5487657, 2.75664, 2.9335337 };
    smpl_lpc_interpol(lsf, prev_lsf, lsf_interpol, SMPL_LPC_ORDER, num_subfr, &A[0][0], &lsfs[0][0]);
    for (int j = 0; j < num_subfr; j++) {
        for (int i = 0; i < SMPL_LPC_ORDER + 1; i++) {
            EXPECT_NEAR(A[j][i], expectA[j][i], 1e-3);
        }
    }
    for (int i = 0; i < SMPL_LPC_ORDER; i++) {
        EXPECT_NEAR(prev_lsf[i], expect_prev_lsf[i], 1e-3);
    }

    StopWatch timerC;
    timerC.start();
    for (auto i = 0; i < NLOOPS; i++) {
        smpl_lpc_interpol(lsf, prev_lsf, lsf_interpol, SMPL_LPC_ORDER, num_subfr, &A[0][0], &lsfs[0][0]);
    }
    timerC.stop();
    printf("%25s  %5.2f ns\n", "LPC_interpol", timerC.avg_lap_time_ns() / NLOOPS);

}

TEST(SmplCelp, spec_fact2)
{
    {
        float c[3], A[3];
        memset(c, 0, 3 * sizeof(float));
        smpl_spec_fact2(c, A);
        for (int i = 0; i < 3; i++) {
            EXPECT_NEAR(A[i], 0.0f, 1e-12f);
        }
    }
    {
        float c[3] = { 0.016502444f, -0.009673091f, 0.0014022322f }, A[3], expect_A[3] = { 0.09133345f, -0.089095846f, 0.014918986 };
        smpl_spec_fact2(c, A);
        for (int i = 0; i < 3; i++) {
            EXPECT_NEAR(A[i], expect_A[i], 1e-3f);
        }
    }
    {
        float c[3] = { 0.04108915f, 0.0067040417f, 0.02027708f }, A[3], expect_A[3] = { 0.15456566f, 0.0234432f, 0.1290311f };
        smpl_spec_fact2(c, A);
        for (int i = 0; i < 3; i++) {
            EXPECT_NEAR(A[i], expect_A[i], 1e-3f);
        }
    }
}