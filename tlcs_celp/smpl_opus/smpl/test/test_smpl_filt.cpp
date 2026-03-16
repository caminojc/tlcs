#include <gtest/gtest.h>
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

TEST(SmplFiltTest, FiltMa1)
{
    srand(9769769); // Avoid flakiness
    auto x = random_vector(320);
    std::vector<float>y0(320);
    std::vector<float>y1(320);
    float coef[] = {1.0f, 0.934f};
    StopWatch timerC;
    for (auto N = 2; N < x.size(); N++) {
        timerC.start();
        float state[] = {0.0f};
        for (auto i = 0; i < NLOOPS; i++) {
            state[0] = 0.0f;
            smpl_filt_ma1(x.data(), N, coef, 2, state, 1, y0.data());
        }
        timerC.stop();

        // test state
        state[0] = 0.0f;
        int N2 = N >> 1;
        smpl_filt_ma1(x.data(), N2, coef, 2, state, 1, y1.data());
        smpl_filt_ma1(x.data() + N2, N - N2, coef, 2, state, 1, y1.data() + N2);
        for (int n = 0; n < N; n++) {
            EXPECT_NEAR(y0[n], y1[n], 1e-5);
        }
    }
    printf("%25s  %5.2f ns\n", "FiltMa1", timerC.avg_lap_time_ns() / NLOOPS);
}

TEST(SmplFiltTest, FiltMa2)
{
    srand(9769769); // Avoid flakiness
    auto x = random_vector(320);
    std::vector<float>y0(320);
    std::vector<float>y1(320);
    float coef[] = {1.0f, 1.234f, 0.934f};
    StopWatch timerC;
    for (auto N = 4; N < x.size(); N++) {
        timerC.start();
        float state[] = {0.0f, 0.0f};
        for (auto i = 0; i < NLOOPS; i++) {
            state[0] = 0.0f;
            state[1] = 0.0f;
            smpl_filt_ma2(x.data(), N, coef, 3, state, 2, y0.data());
        }
        timerC.stop();

        // test state
        state[0] = 0.0f;
        state[1] = 0.0f;
        int N2 = N >> 1;
        smpl_filt_ma2(x.data(), N2, coef, 3, state, 2, y1.data());
        smpl_filt_ma2(x.data() + N2, N - N2, coef, 3, state, 2, y1.data() + N2);
        for (int n = 0; n < N; n++) {
            EXPECT_NEAR(y0[n], y1[n], 1e-5);
        }
    }
    printf("%25s  %5.2f ns\n", "FiltMa2", timerC.avg_lap_time_ns() / NLOOPS);
}

TEST(SmplFiltTest, FiltMa3)
{
    srand(9769769); // Avoid flakiness
    int order = 3;
    auto x = random_vector(320 + order);
    std::vector<float>y0(320);
    std::vector<float>y1(320);
    auto coef = random_vector(order + 1);
    StopWatch timerC;
    for (auto N = 0; N < x.size() - order; N++) {
        timerC.start();
        for (auto i = 0; i < NLOOPS; i++) {
            smpl_filt_ma3(x.data() + order, N, coef.data(), order + 1, y0.data());
        }
        timerC.stop();

        smpl_filt_ma(x.data() + order, N, coef.data(), order + 1, y1.data());
        for (int n = 0; n < N; n++) {
            EXPECT_NEAR(y0[n], y1[n], 1e-5);
        }
    }
    printf("%25s  %5.2f ns\n", "FiltMa3", timerC.avg_lap_time_ns() / NLOOPS);
}

TEST(SmplFiltTest, FiltMa9)
{
    srand(9769769); // Avoid flakiness
    int order = 9;
    auto x = random_vector(320 + order);
    std::vector<float>y0(320);
    std::vector<float>y1(320);
    auto coef = random_vector(order + 1);
    StopWatch timerC;
    for (auto N = 0; N < x.size() - order; N++) {
        timerC.start();
        for (auto i = 0; i < NLOOPS; i++) {
            smpl_filt_ma9(x.data() + order, N, coef.data(), order + 1, y0.data());
        }
        timerC.stop();

        smpl_filt_ma(x.data() + order, N, coef.data(), order + 1, y1.data());
        for (int n = 0; n < N; n++) {
            EXPECT_NEAR(y0[n], y1[n], 1e-5);
        }
    }
    printf("%25s  %5.2f ns\n", "FiltMa9", timerC.avg_lap_time_ns() / NLOOPS);
}

TEST(SmplFiltTest, FiltMa16_monic)
{
    srand(9769769); // Avoid flakiness
    int order = 16;
    auto x = random_vector(320 + order);
    std::vector<float>y0(320);
    std::vector<float>y1(320);
    auto coef = random_vector(order + 1);
    coef[0] = 1.0f;
    StopWatch timerC;
    for (auto N = 0; N < x.size() - order; N++) {
        timerC.start();
        for (auto i = 0; i < NLOOPS; i++) {
            smpl_filt_ma16_monic(x.data() + order, N, coef.data(), order + 1, y0.data());
        }
        timerC.stop();

        smpl_filt_ma(x.data() + order, N, coef.data(), order + 1, y1.data());
        for (int n = 0; n < N; n++) {
            EXPECT_NEAR(y0[n], y1[n], 1e-5);
        }
    }
    printf("%25s  %5.2f ns\n", "FiltMa16_monic", timerC.avg_lap_time_ns() / NLOOPS);
}

TEST(SmplFiltTest, FiltMa)
{
    srand(9769769); // Avoid flakiness
    auto max_order = 32;
    auto x = random_vector(320 + max_order);
    auto y = random_vector(320);
    auto coef = random_vector(max_order + 1);
    // for (auto order = 1; order <= max_order; order++) {
    for (auto order = 1; order <= max_order; order *= 2) {
        StopWatch timerC;
        for (auto N = 0; N < x.size() - max_order; N++) {
            timerC.start();
            for (auto i = 0; i < NLOOPS; i++) {
                smpl_filt_ma(x.data() + order, N, coef.data(), order + 1, y.data());
            }
            timerC.stop();
        }
        printf("          FiltMA order = %2d  %5.2f ns\n", order, timerC.avg_lap_time_ns() / NLOOPS);
    }
    coef[0] = 1.0f;
    for (auto order = 1; order <= max_order; order *= 2) {
        StopWatch timerC;
        for (auto N = 0; N < x.size() - max_order; N++) {
            timerC.start();
            for (auto i = 0; i < NLOOPS; i++) {
                smpl_filt_ma(x.data() + order, N, coef.data(), order + 1, y.data());
            }
            timerC.stop();
        }
        printf("    FiltMA monic order = %2d  %5.2f ns\n", order, timerC.avg_lap_time_ns() / NLOOPS);
    }
}

TEST(SmplFiltTest, FiltAr1)
{
    srand(9769769); // Avoid flakiness
    auto x = random_vector(320);
    std::vector<float>y0(320);
    std::vector<float>y1(320);
    float coef[] = {1.0f, 0.934f};
    StopWatch timerC;
    for (auto N = 0; N < x.size(); N++) {
        timerC.start();
        float state[] = {0.0f};
        for (auto i = 0; i < NLOOPS; i++) {
            state[0] = 0.0f;
            smpl_filt_ar1(x.data(), N, coef, 2, state, 1, y0.data());
        }
        timerC.stop();

        // test state
        state[0] = 0.0f;
        int N2 = N >> 1;
        smpl_filt_ar1(x.data(), N2, coef, 2, state, 1, y1.data());
        smpl_filt_ar1(x.data() + N2, N - N2, coef, 2, state, 1, y1.data() + N2);
        for (int n = 0; n < N; n++) {
            EXPECT_NEAR(y0[n], y1[n], 1e-5);
        }

        // test x == y
        state[0] = 0.0f;
        smpl_filt_ar1(x.data(), N, coef, 2, state, 1, y0.data());
        auto x2 = x; // copy
        state[0] = 0.0f;
        smpl_filt_ar1(x2.data(), N, coef, 2, state, 1, x2.data());
        for (int n = 0; n < N; n++) {
            EXPECT_NEAR(y0[n], x2[n], 1e-6);
        }
    }
    printf("%25s  %5.2f ns\n", "FiltAr1", timerC.avg_lap_time_ns() / NLOOPS);
}

TEST(SmplFiltTest, FiltAr2)
{
    srand(9769769); // Avoid flakiness
    auto x = random_vector(320);
    std::vector<float>y0(320);
    std::vector<float>y1(320);
    float coef[] = {1.0f, 1.234f, 0.934f};
    StopWatch timerC;
    for (auto N = 0; N < x.size(); N++) {
        timerC.start();
        float state[] = {0.0f, 0.0f};
        for (auto i = 0; i < NLOOPS; i++) {
            state[0] = 0.0f;
            state[1] = 0.0f;
            smpl_filt_ar2(x.data(), N, coef, 3, state, 2, y0.data());
        }
        timerC.stop();

        // test state
        state[0] = 0.0f;
        state[1] = 0.0f;
        int N2 = N >> 1;
        smpl_filt_ar2(x.data(), N2, coef, 3, state, 2, y1.data());
        smpl_filt_ar2(x.data() + N2, N - N2, coef, 3, state, 2, y1.data() + N2);
        for (int n = 0; n < N; n++) {
            EXPECT_NEAR(y0[n], y1[n], 1e-5);
        }

        // test x == y
        state[0] = 0.0f;
        state[1] = 0.0f;
        smpl_filt_ar2(x.data(), N, coef, 3, state, 2, y0.data());
        auto x2 = x; // copy
        state[0] = 0.0f;
        state[1] = 0.0f;
        smpl_filt_ar2(x2.data(), N, coef, 3, state, 2, x2.data());
        for (int n = 0; n < N; n++) {
            EXPECT_FLOAT_EQ(y0[n], x2[n]);
        }
    }
    printf("%25s  %5.2f ns\n", "FiltAr2", timerC.avg_lap_time_ns() / NLOOPS);
}

TEST(SmplFiltTest, FiltAr4)
{
    srand(9769769); // Avoid flakiness
    auto x = random_vector(320);
    auto y = random_vector(320 + 4);
    float coef[] = {1.0f, 1.234f, 0.934f, 0.1f, 0.1f};
    StopWatch timerC;
    for (auto N = 0; N < x.size(); N++) {
        timerC.start();
        for (auto i = 0; i < NLOOPS; i++) {
            smpl_filt_ar4(x.data(), N, coef, y.data() + 4);
        }
        timerC.stop();
    }
    printf("%25s  %5.2f ns\n", "FiltAr4", timerC.avg_lap_time_ns() / NLOOPS);
}

TEST(SmplFiltTest, FiltAr16)
{
    srand(9769769); // Avoid flakiness
    auto x = random_vector(320);
    auto y = random_vector(320 + 16);
    float coef[17] = {1.0000, -0.2490, 0.3680, -0.6909, -0.1432, -0.5223, -0.2506, -0.2776, 0.0093, 0.3631, 0.3004, 0.5567, 0.1098, 0.6656, -0.6711, 0.1512, -0.7183};
    StopWatch timerC;
    for (auto N = 0; N < x.size(); N++) {
        timerC.start();
        for (auto i = 0; i < NLOOPS; i++) {
            smpl_filt_ar16(x.data(), N, coef, y.data() + 16);
        }
        timerC.stop();
    }
    printf("%25s  %5.2f ns\n", "FiltAr16", timerC.avg_lap_time_ns() / NLOOPS);
}

TEST(SmplFiltTest, FiltArma1)
{
    srand(9769769); // Avoid flakiness
    auto x = random_vector(320);
    std::vector<float>y(320);
    float coefma[] = {1.0f, 0.934f};
    float coefma2[] = {coefma[0] * 2, coefma[1] * 2};
    float coefar[] = {1.0f, 0.934f};
    float coefar2[] = {1.0f, 0.765f};
    StopWatch timer0, timer1;
    for (auto N = 2; N < x.size(); N++) {   
        timer0.start();
        for (auto i = 0; i < NLOOPS; i++) {
            float state[2] = {0.0f};
            smpl_filt_arma1(x.data(), N, coefma, 2, coefar, 2, state, 2, y.data());
        }
        timer0.stop();
        for (int n = 0; n < N; n++) {
            EXPECT_NEAR(y[n], x[n], 1e-5);
        }
        // test state
        int N2 = N >> 1;
        float state[2] = {0.0f};
        smpl_filt_arma1(x.data()     , N2,     coefma, 2, coefar, 2, state, 2, y.data());
        smpl_filt_arma1(x.data() + N2, N - N2, coefma, 2, coefar, 2, state, 2, y.data() + N2);
        for (int n = 0; n < N; n++) {
            EXPECT_NEAR(y[n], x[n], 1e-5);
        }
 
        timer1.start();
        for (auto i = 0; i < NLOOPS; i++) {
            float tmp_state[2] = {0.0f};
            smpl_filt_arma1(x.data(), N, coefma2, 2, coefar, 2, tmp_state, 2, y.data());
        }
        timer1.stop();
        for (int n = 0; n < N; n++) {
            EXPECT_NEAR(y[n], 2 * x[n], 1e-5);
        }
        // test state
        N2 = N >> 1;
        state[0] = 0.0f;
        state[1] = 0.0f;
        smpl_filt_arma1(x.data()     , N2,     coefma2, 2, coefar, 2, state, 2, y.data());
        smpl_filt_arma1(x.data() + N2, N - N2, coefma2, 2, coefar, 2, state, 2, y.data() + N2);
        for (int n = 0; n < N; n++) {
            EXPECT_NEAR(y[n], 2 * x[n], 1e-5);
        }

        // test x == y
        state[0] = 0.0f;
        state[1] = 0.0f;
        smpl_filt_arma1(x.data(), N, coefma2, 2, coefar2, 2, state, 2, y.data());
        state[0] = 0.0f;
        state[1] = 0.0f;
        auto x2 = x; // copy
        smpl_filt_arma1(x2.data(), N, coefma2, 2, coefar2, 2, state, 2, x2.data());
        for (int n = 0; n < N; n++) {
            EXPECT_NEAR(y[n], x2[n], 1e-6);
        }
    }
    printf("%25s  %5.2f ns\n", "FiltArma1 (monic)", timer0.avg_lap_time_ns() / NLOOPS);
    printf("%25s  %5.2f ns\n", "FiltArma1",         timer1.avg_lap_time_ns() / NLOOPS);
}

TEST(SmplFiltTest, FiltArma2)
{
    srand(9769769); // Avoid flakiness
    auto x = random_vector(320);
    std::vector<float>y(320);
    float coefma[] = {1.0f, -1.23f, 0.834f};
    float coefma2[] = {coefma[0] * 2, coefma[1] * 2, coefma[2] * 2};
    float coefar[] = {1.0f, -1.23f, 0.834f};
    float coefar2[] = {1.0f, 0.65f, 0.654f};
    StopWatch timer0, timer1;
    for (auto N = 4; N < x.size(); N++) {   
        timer0.start();
        for (auto i = 0; i < NLOOPS; i++) {
            float state[4] = {0.0f};
            smpl_filt_arma2(x.data(), N, coefma, 3, coefar, 3, state, 4, y.data());
        }
        timer0.stop();
        for (int n = 0; n < N; n++) {
            EXPECT_NEAR(y[n], x[n], 1e-5);
        }
        // test state
        int N2 = N >> 1;
        float state[4] = {0.0f};
        smpl_filt_arma2(x.data()     , N2,     coefma, 3, coefar, 3, state, 4, y.data());
        smpl_filt_arma2(x.data() + N2, N - N2, coefma, 3, coefar, 3, state, 4, y.data() + N2);
        for (int n = 0; n < N; n++) {
            EXPECT_NEAR(y[n], x[n], 1e-4);
        }
 
        timer1.start();
        for (auto i = 0; i < NLOOPS; i++) {
            state[0] = 0.0f;
            state[1] = 0.0f;
            state[2] = 0.0f;
            state[3] = 0.0f;
            smpl_filt_arma2(x.data(), N, coefma2, 3, coefar, 3, state, 4, y.data());
        }
        timer1.stop();
        for (int n = 0; n < N; n++) {
            EXPECT_NEAR(y[n], 2 * x[n], 1e-4);
        }
        // test state
        N2 = N >> 1;
        state[0] = 0.0f;
        state[1] = 0.0f;
        state[2] = 0.0f;
        state[3] = 0.0f;
        smpl_filt_arma2(x.data()     , N2,     coefma2, 3, coefar, 3, state, 4, y.data());
        smpl_filt_arma2(x.data() + N2, N - N2, coefma2, 3, coefar, 3, state, 4, y.data() + N2);
        for (int n = 0; n < N; n++) {
            EXPECT_NEAR(y[n], 2 * x[n], 1e-4);
        }

        // test x == y
        state[0] = 0.0f;
        state[1] = 0.0f;
        state[2] = 0.0f;
        state[3] = 0.0f;
        smpl_filt_arma2(x.data(), N, coefma2, 3, coefar2, 3, state, 4, y.data());
        state[0] = 0.0f;
        state[1] = 0.0f;
        state[2] = 0.0f;
        state[3] = 0.0f;
        auto x2 = x; // copy
        smpl_filt_arma2(x2.data(), N, coefma2, 3, coefar2, 3, state, 4, x2.data());
        for (int n = 0; n < N; n++) {
            EXPECT_NEAR(y[n], x2[n], 1e-6);
        }
    }

    printf("%25s  %5.2f ns\n", "FiltArma2 (monic)", timer0.avg_lap_time_ns() / NLOOPS);
    printf("%25s  %5.2f ns\n", "FiltArma2",         timer1.avg_lap_time_ns() / NLOOPS);
}


TEST(SmplFiltTest, interpol)
{
    auto x = random_vector(320 + 16);
    StopWatch timerC, timerI;
    for (auto N = 0; N < x.size() - 16; N++) {
        auto y1 = x;  // copy
        auto y2 = x;  // copy
        timerC.start();
        for (auto i = 0; i < NLOOPS; i++)
            smpl_interpol(x.data(), y1.data(), N);
        timerC.stop();
        timerI.start();
        for (auto i = 0; i < NLOOPS; i++)
            smpl_filt_ma(x.data() + 15, N, smpl_interpol_kernel, 16, y2.data());
        timerI.stop();
        for (int n = 0; n < N; n++) {
            EXPECT_NEAR(y1[n], y2[n], 1e-5);
        }
    }
    printf("    interpol: %5.2f ns  using filt_ma: %5.2f ns \n", timerC.avg_lap_time_ns() / NLOOPS, timerI.avg_lap_time_ns() / NLOOPS);
}

TEST(SmplFiltTest, FiltAr16Ma16)
{
    srand(9769769); // Avoid flakiness
    auto x = random_vector(320 + 16);

    // Check smpl_filt_ar16 is inverse of smpl_filt_ma16_monic
    float coef[17] = { 1.0000, -0.2490, 0.3680, -0.6909, -0.1432, -0.5223, -0.2506, -0.2776, 0.0093, 0.3631, 0.3004, 0.5567, 0.1098, 0.6656, -0.6711, 0.1512, -0.7183 };
    auto y1 = std::vector<float>(320);
    auto y2 = std::vector<float>(320 + 16, 0.0f);
    memset(x.data(), 0, 16 * sizeof(float));
    smpl_filt_ma16_monic(x.data() + 16, 320, coef, 17, y1.data());
    smpl_filt_ar16(y1.data(), 320, coef, y2.data() + 16);
    for (int i = 0; i < 320; i++) {
        EXPECT_NEAR(x[16 + i], y2[16 + i], 1e-5f);
    }
}
