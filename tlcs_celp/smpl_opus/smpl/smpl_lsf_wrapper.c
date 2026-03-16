#include "silk/SigProc_FIX.h"
#include "smpl_defines.h"
#include "silk/debug.h"

// Need to wrap functions as we want to build remaining code with -Ofast ie fast float arithmetic
// This fails when including Opus headers

#define SCALE1 32768.0f/SMPL_PI
#define QA 16                        // NB!! keep in sync with NLSF2A.c
#define SCALE2_ALT 1.0f/((int)1 << (QA + 1))
void smpl_NLSF2A(float a[], const float nlsf[], const int d)
{
    TIC(LSF2A)
    if (d > SMPL_LPC_ORDER) {
        return;
    }
    opus_int16 NLSF[SMPL_LPC_ORDER];
    for (int i = 0; i < d; i++) {
        NLSF[i] = (opus_int16)roundf(nlsf[i] * SCALE1);
    }
    opus_int32 a32_QA1[SMPL_LPC_ORDER + 1];
    silk_NLSF2A_32(a32_QA1, NLSF, d);

    a[0] = 1.0f;
    for (int i = 0; i < d; i++) {
        a[i + 1] = (float)(-a32_QA1[i] * SCALE2_ALT);
    }
    TOC(LSF2A)
}

void smpl_A2NLSF_16(float nlsf[], const float A[])
{
    opus_int16 lsf_Q15[SMPL_LPC_ORDER];
    opus_int32 a_Q16[SMPL_LPC_ORDER];
    for (int i = 0; i < SMPL_LPC_ORDER; i++)
    {
        a_Q16[i] = (opus_int32)roundf(-A[i + 1] * 65536);
    }
    silk_A2NLSF(lsf_Q15, a_Q16, SMPL_LPC_ORDER);
    for (int i = 0; i < SMPL_LPC_ORDER; i++)
    {
        nlsf[i] = (float)(lsf_Q15[i] / 32768.0f * SMPL_PI);
    }
}
