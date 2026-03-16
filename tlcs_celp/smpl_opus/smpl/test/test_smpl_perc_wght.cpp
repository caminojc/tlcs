#include <gtest/gtest.h>
#include "smpl_perc_wght.h"
#include "smpl_lpc.h"
#include "smpl_errors.h"
#include "memory.h"
#include <math.h>
#include <fenv.h>
#include "smpl_lpc.h"

#define FS_KHZ 16
#define WINNEXT 2 * FS_KHZ
#define WINNEXT_LONG 4 * FS_KHZ
#define WINONES_PERC 10 * FS_KHZ
#define WINPREV_PERC 12 * FS_KHZ

TEST(SmplPercWght, ZeroInput)
{
    void* pPM = smpl_create_perc_model_tables();
    smpl_create_lpc_windows();
    EXPECT_TRUE(pPM != NULL);

    for (int framelen : {10 * FS_KHZ, 20 * FS_KHZ}) {

        float buf[512 + 64];
        memset(buf, 0, (512+64)*sizeof(float));
        float x[80];
        memset(x, 0, sizeof(x));
        float R[32];
        memset(R, 0, sizeof(R));
        smpl_perc_model(buf, x, SMPL_ARR_LEN(x), framelen/FS_KHZ, SMPL_FALSE, R, SMPL_ARR_LEN(R));
        smpl_perc_model(buf, x, SMPL_ARR_LEN(x), framelen/FS_KHZ, SMPL_TRUE, R, SMPL_ARR_LEN(R));
    }
    smpl_free_perc_model_tables();
    smpl_free_lpc_windows();
}
    