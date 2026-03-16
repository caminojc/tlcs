#include <gtest/gtest.h>
#include "smpl_detect_arch.h"
#include "smpl_filt.h"
#include "smpl_tables.h"
#include "smpl_errors.h"
#include "stop_watch.h"
#include "memory.h"
#include "smpl_codec_util.h"


#define NLOOPS  500

static inline std::vector<float> random_vector(int L)
{
    std::vector<float>y(L);
    for (int i = 0; i < L; i++) {
        y[i] = (((float)rand()) + (RAND_MAX / 2.0f)) / RAND_MAX;
    }
    return y;
}


static void smpl_filt_ar16_ref(const float *x, int N, const float *coef, float *y)
{
    for (int n = 0; n < N; n++) {
        float res = x[n];
        for (int i = 0; i < 16; i++) {
            res -= coef[16 - i] * y[n - 16 + i];
        }
        y[n] = res;
    }
}

TEST(SmplFiltTestARM, FiltAr16Neon)
{
    int N = 80;
    srand(9769769); // Avoid flakiness
    auto x = random_vector(N);
    auto y = random_vector(N + 16);
    float coef[17] = {1.0000, -0.2490, 0.3680, -0.6909, -0.1432, -0.5223, -0.2506, -0.2776, 0.0093, 0.3631, 0.3004, 0.5567, 0.1098, 0.6656, -0.6711, 0.1512, -0.7183};
    StopWatch timerC;
    timerC.start();
    for (auto i = 0; i < NLOOPS; i++) {
        smpl_filt_ar16(x.data(), N, coef, y.data() + 16);
    }
    timerC.stop();

    // test correctness
    for (auto i = 0; i < NLOOPS; i++) {
        smpl_filt_ar16_ref(x.data(), N, coef, y.data() + 16);
    }
    auto y2 = y; // copy
    for (auto i = 0; i < NLOOPS; i++) {
        smpl_filt_ar16(x.data(), N, coef, y.data() + 16);
    }
    for (int n = 0; n < N; n++) {
        EXPECT_NEAR(y[n], y2[n], 1e-4);
    }
    printf("%25s  %5.2f ns (%s)\n", "FiltAr16Neon", timerC.avg_lap_time_ns() / NLOOPS, arch_string);
}


void smpl_filt_ar2_ref(const float *x, int N, const float *coef, int coef_len, float *state, int state_len, float *y)
{
    const float ar1 = -coef[1];
    const float ar2 = -coef[2];
    float ytmp0 = state[1];
    float ytmp1 = state[0];
    for (int n = 0; n < N; n++)
    {
        y[n] = x[n] + ar1 * ytmp1 + ar2 * ytmp0;
        ytmp0 = ytmp1;
        ytmp1 = y[n];
    }
    state[1] = ytmp0;
    state[0] = ytmp1;
}

TEST(SmplFiltTestARM, FiltAr2Neon)
{
    srand(9769769); // Avoid flakiness
    int N = 80;
    auto x = random_vector(N);
    std::vector<float>y0(N);
    std::vector<float>y1(N);
    float coef[] = {1.0f, 1.234f, 0.934f};
    StopWatch timerC;
    float state[2];
    timerC.start();
    for (auto i = 0; i < NLOOPS; i++) {
        state[0] = 0.0f;
        state[1] = 0.0f;
        smpl_filt_ar2(x.data(), N, coef, 3, state, 2, y0.data());
    }
    timerC.stop();

    // test state
    state[0] = 0.0f;
    state[1] = 0.0f;
    int N2 = (N >> 1) + 1;
    smpl_filt_ar2(x.data(), N2, coef, 3, state, 2, y1.data());
    smpl_filt_ar2(x.data() + N2, N - N2, coef, 3, state, 2, y1.data() + N2);
    for (int n = 0; n < N; n++) {
        EXPECT_NEAR(y0[n], y1[n], 1e-5);
    }

    state[0] = 0.0f;
    state[1] = 0.0f;
    smpl_filt_ar2_ref(x.data(), N, coef, 3, state, 2, y0.data());
    state[0] = 0.0f;
    state[1] = 0.0f;
    smpl_filt_ar2(x.data(), N, coef, 3, state, 2, y1.data());
    for (int n = 0; n < N; n++) {
        EXPECT_NEAR(y0[n], y1[n], 1e-5);
    }
    printf("%25s  %5.2f ns (%s)\n", "FiltAr2Neon", timerC.avg_lap_time_ns() / NLOOPS, arch_string);
}
