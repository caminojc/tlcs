#include <gtest/gtest.h>
#include "smpl_api.h"
#include "smpl_postfilter.h"

TEST(SmplPostfilters, HpVoiced)
{
    EXPECT_EQ(smpl_CreateCodec(), 0);

    HpPst hp_postfilt;
    smpl_hp_postfilter_init(&hp_postfilt);
    float x[SMPL_FRAME_LEN], y[SMPL_FRAME_LEN];
    memset(x, 0, SMPL_FRAME_LEN * sizeof(float));
    x[0] = 1.0f;
    float lag = 88.0f;// Test voiced Impulse Responce
    float expect_impz[] = {1.000000, 0.995000, 0.990025, 0.985075, 0.980150, 0.975249, 0.970372, 0.965521, 0.960693, 0.955890};
    smpl_hp_postfilter(&hp_postfilt, x, SMPL_FRAME_LEN, &lag, 1, y);
    EXPECT_EQ(hp_postfilt.lag_old, lag);
    for (int i = 0; i < SMPL_FRAME_LEN; i++) {
        EXPECT_EQ(hp_postfilt.x_old[i], x[i]);
    }
    for (int i = 0; i < SMPL_ARR_LEN(expect_impz); i++) {
        EXPECT_NEAR(expect_impz[i], x[i], 1e-5f);
    }

    smpl_FreeCodec();
}

TEST(SmplPostfilters, HpUnvoiced)
{
    EXPECT_EQ(smpl_CreateCodec(), 0);

    HpPst hp_postfilt;
    smpl_hp_postfilter_init(&hp_postfilt);
    float x[SMPL_FRAME_LEN];
    memset(x, 0, SMPL_FRAME_LEN * sizeof(float));
    x[0] = 1.0f;
    float lag = 0.0f;// Test unvoiced Impulse Responce
    float expect_impz[] = { 0.986442f, -0.026937f, -0.026579f, -0.026220f, -0.025860f, -0.025501f, -0.025141f, -0.024782f, -0.024422f, -0.024062f };
    smpl_hp_postfilter(&hp_postfilt, x, SMPL_FRAME_LEN, &lag, 1, x);
    for (int i = 0; i < SMPL_ARR_LEN(expect_impz); i++) {
        EXPECT_NEAR(expect_impz[i], x[i], 1e-5f);
    }

    smpl_FreeCodec();
}

TEST(SmplPostfilters, HpTransition)
{
    EXPECT_EQ(smpl_CreateCodec(), 0);

    HpPst hp_postfilt;
    smpl_hp_postfilter_init(&hp_postfilt);
    float x[SMPL_FRAME_LEN];
    memset(x, 0, SMPL_FRAME_LEN * sizeof(float));
    x[0] = 1.0f;
    float lag = 44.0f;// Test transition
    float expect_impz[] = { 0.950307f, -0.098880f, -0.097441f, -0.095193f, -0.092233f, -0.088658f, -0.084561f, -0.080030f, -0.075150f, -0.070002f };
    smpl_hp_postfilter(&hp_postfilt, x, SMPL_FRAME_LEN, &lag, 1, x);
    lag = 250.0f;
    memset(x, 0, SMPL_FRAME_LEN * sizeof(float));
    x[0] = 1.0f;
    smpl_hp_postfilter(&hp_postfilt, x, SMPL_FRAME_LEN, &lag, 1, x);
    for (int i = 0; i < SMPL_ARR_LEN(expect_impz); i++) {
        EXPECT_NEAR(expect_impz[i], x[i], 1e-5f);
    }

    smpl_FreeCodec();
}
