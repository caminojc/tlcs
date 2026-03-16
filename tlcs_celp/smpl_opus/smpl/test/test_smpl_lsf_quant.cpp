 #include <gtest/gtest.h>
#include "smpl_lsf_quant.h"
#include "smpl_errors.h"
#include "memory.h"
#include <math.h>
#include <fenv.h>

#ifdef SMPL_USE_SPEC_LSW_WEIGHT
TEST(SmplLsfQuant, lsf_weights)
{
    float A[SMPL_LPC_ORDER + 1] = { 1.0, -0.61862946, -0.26583862, -0.20011902, -0.034179688, -0.07493591, 0.09850311, 0.03742218, 0.14997864, -0.033355713, 0.031715393, -0.042366028, 0.058502197, 0.022598267, -0.007965088, -0.08963013, 0.014961243 };
    float lsf[SMPL_LPC_ORDER] = { 0.07660317, 0.18848789, 0.3363253, 0.6139758, 0.8351567, 1.0654455, 1.2336082, 1.4109747, 1.6308134, 1.8056872, 2.0119116, 2.184868, 2.3864906, 2.5720065, 2.7548378, 2.9166727 };
    float wlsf[SMPL_LPC_ORDER];
    float expect_wlsf[SMPL_LPC_ORDER] = { 1.0, 0.5629088, 0.17216241, 0.0679816, 0.062169354, 0.06251089, 0.081660435, 0.05669734, 0.060945086, 0.05862698, 0.05907727, 0.06039643, 0.055145934, 0.060657907, 0.06218464, 0.056123998 };    //float expect_wlsf[SMPL_LPC_ORDER] = { 1.0, 0.56087226, 0.17191176, 0.06793004, 0.062114023, 0.06244796, 0.08159084, 0.05663607, 0.060891874, 0.058579717, 0.059018437, 0.06035789, 0.055095512, 0.060587905, 0.062140185, 0.056067128 };
    smpl_lsf_weights(A, lsf, wlsf);
    for (int i = 0; i < SMPL_LPC_ORDER; i++) {
        EXPECT_NEAR(wlsf[i], expect_wlsf[i], 1e-5f);
    }
}
#endif

TEST(SmplLsfQuant, lsf_min_dist)
{
    float lsf[SMPL_LPC_ORDER] = {0.0, 0.0, 0.0, 0.5, 0.5, 1.0, 1.0, 1.5, 1.5, 2.0, 2.0, 2.0, 2.5, 2.5, 3.0, 3.1};
    std::vector<float>min_dist(SMPL_LPC_ORDER+1, 1e-2f);
    SMPL_lsf_min_dist(lsf, min_dist.data());
    for (int i = 1; i < SMPL_LPC_ORDER; i++) {
        EXPECT_TRUE(lsf[i] - lsf[i-1] > 1e-2f);
    }
}

TEST(SmplLsfQuant, LoadCbks)
{
    void* cbks = smpl_load_lsf_CBks();
    EXPECT_TRUE(cbks != nullptr);
    void* cbks2 = smpl_load_lsf_CBks();
    EXPECT_EQ(cbks, cbks2);
}
