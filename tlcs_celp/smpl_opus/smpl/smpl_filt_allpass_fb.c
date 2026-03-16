#include "smpl_filt_allpass_fb.h"
#include "smpl_typedef.h"
#include "smpl_filt.h"
#include "smpl_codec_util.h"
#include "smpl_tables.h"
#include <string.h>
#include "smpl_defines.h"
#include "silk/debug.h"

void smpl_filt_allpass_fb_ana(const float *x, int x_len, const float* A0, int A0_len, const float *A1, int A1_len, float* yL, float* yH, float* state, int state_len)
{
    TIC(fb_ana)

    smpl_assert((x_len & 1) == 0); // signal should have even length
    smpl_assert(x_len <= SMPL_FRAME_LEN);
    smpl_assert(A0_len == 3);
    smpl_assert(A1_len == 3);
    smpl_assert(state_len == 8);

    float x_tmp1[SMPL_FRAME_LEN];
    float x_tmp2[SMPL_FRAME_LEN];
    for (int i = 0; i < x_len / 2; i++)
    {
        x_tmp1[i] = x[i * 2 + 1];
        x_tmp2[i] = x[i * 2];
    }

    smpl_allpass2(x_tmp1, x_len / 2, A0, A0_len, state,     4, yL);
    smpl_allpass2(x_tmp2, x_len / 2, A1, A1_len, state + 4, 4, yH);

    for (int i = 0; i < x_len / 2; i++)
    {
        float yL_tmp = yL[i];
        yL[i] = (yL[i]  + yH[i]) * 0.5f;
        yH[i] = (yL_tmp - yH[i]) * 0.5f;
    }

    TOC(fb_ana)
}

void smpl_filt_allpass_fb_syn(const float* xL, int xL_len, const float* xH, int xH_len, const float* A0, int A0_len, const float* A1, int A1_len, float* y, float* state, int state_len)
{
    TIC(fb_syn)

    smpl_assert(xL_len <= SMPL_MAX_WB_PACKET_SIZE_IN_SAMPLES);
    smpl_assert(xL_len == xH_len);
    smpl_assert(A0_len == 3);
    smpl_assert(A1_len == 3);
    smpl_assert(state_len == 8);
    smpl_assert(state_len == 8);

    float x_tmp1[SMPL_MAX_WB_PACKET_SIZE_IN_SAMPLES];
    float x_tmp2[SMPL_MAX_WB_PACKET_SIZE_IN_SAMPLES];
    float x_tmp3[SMPL_MAX_WB_PACKET_SIZE_IN_SAMPLES];
    smpl_add_vec(xL, xH, x_tmp1, xL_len);
    smpl_allpass2(x_tmp1, xL_len, A0, A0_len, state,     4, x_tmp2);
    smpl_sub_vec(xL, xH, x_tmp1, xL_len);
    smpl_allpass2(x_tmp1, xL_len, A1, A1_len, state + 4, 4, x_tmp3);
    for (int i = 0; i < xL_len; i++) {
        y[2 * i]     = x_tmp2[i];
        y[2 * i + 1] = x_tmp3[i];
    }

    TOC(fb_syn)
}
