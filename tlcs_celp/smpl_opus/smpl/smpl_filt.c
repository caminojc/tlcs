#include "smpl_filt.h"
#include "smpl_defines.h"
#include "smpl_tables.h"
#include "smpl_codec_util.h"
#include "smpl_detect_arch.h"
#include "smpl_typedef.h"
#include "smpl_errors.h"
#include "silk/debug.h"
#include <string.h>
#include <math.h>
#include <stdio.h>

#if SMPL_USE_NEON
#include <arm_neon.h>
#endif

#define SMPL_MAX_FILTER_INPUT_SAMPLES (16 * 20)

// 1st order MA filter, does not need to be monic
void smpl_filt_ma1(const float *x, int N, const float *coef, int coef_len, float *state, int state_len, float *y)
{
    smpl_assert(coef_len == 2);
    smpl_assert(state_len == 1);
    smpl_assert(N > 0);
    smpl_assert(x != y);

    if (coef[0] == 1.0f)
    {
        smpl_add_scale_vec(x + 1, x, y + 1, N - 1, coef[1]);
    } else {
        smpl_scale_vec(x, y, N, coef[0]);
        smpl_add_scale_vec_inplace(x, y + 1, N - 1, coef[1]);
    }
    y[0] = coef[0] * x[0] + coef[1] * state[0];
    state[0] = x[N - 1];
}

// 2nd order MA filter, does not need to be monic
void smpl_filt_ma2(const float *x, int N, const float *coef, int coef_len, float *state, int state_len, float *y)
{
    smpl_assert(coef_len == 3);
    smpl_assert(state_len == 2);
    smpl_assert(N > 1);
    smpl_assert(x != y);

    if (coef[0] == 1.0f)  // monic
    {
        smpl_add_scale_vec(x + 1,  x, y + 1, N - 1, coef[1]);
    } else {                   // not monic
        smpl_scale_vec(x, y, N, coef[0]);
        smpl_add_scale_vec_inplace(x, y + 1, N - 1, coef[1]);
    }
    smpl_add_scale_vec_inplace(x, y + 2, N - 2, coef[2]);
    y[0] = coef[0] * x[0] + coef[1] * state[0] + coef[2] * state[1];
    y[1] += coef[2] * state[0];
    state[0] = x[N - 1];
    state[1] = x[N - 2];
}

// 3rd order MA function. The state is presumed to sit in the 3 samples before x[0]
void smpl_filt_ma3(const float *x, int N, const float *coef, const int coef_len, float *y)
{
    TIC(ma3)
    smpl_assert(coef_len == 4);
    for (int n = 0; n < N; n++) {
        float res = 0;
        for (int i = 0; i < 4; i++) {
            res += coef[i] * x[n - i];
        }
        y[n] = res;
    }
    TOC(ma3)
}

// 9th order MA function. The state is presumed to sit in the 9 samples before x[0]
void smpl_filt_ma9(const float *x, int N, const float *coef, const int coef_len, float *y)
{
    TIC(ma9)
    smpl_assert(coef_len == 10);
    for (int n = 0; n < N; n++) {
        float res = 0;
        for (int i = 0; i < 10; i++) {
            res += coef[i] * x[n - i];
        }
        y[n] = res;
    }
    TOC(ma9)
}

// 16th order MA function. The state is presumed to sit in the 16 samples before x[0]
void smpl_filt_ma16_monic(const float *x, int N, const float *coef, const int coef_len, float *y)
{
    TIC(ma16_monic)
    smpl_assert(coef_len == 17);
    smpl_assert(coef[0] == 1.0f);
    for (int n = 0; n < N; n++) {
        float res = x[n];
        for (int i = 1; i < 17; i++) {
            res += coef[i] * x[n - i];
        }
        y[n] = res;
    }
    TOC(ma16_monic)
}

// 16th order symmetric MA function. The state is presumed to sit in the 16 samples before x[0]
void smpl_filt_ma16_sym(const float *x, int N, const float *coef, const int coef_len, float *y)
{
    TIC(ma16_sym)
    smpl_assert(coef_len == 17);
    for (int n = 0; n < N; n++) {
        float res = x[n - 8] * coef[8];
        for (int i = 0; i < 8; i++) {
            res += coef[i] * (x[n - i] + x[n - 16 + i]);
        }
        y[n] = res;
    }
    TOC(ma16_sym)
}

// MA function. The state is presumed to sit in the (coef_len - 1) samples before x[0]
void smpl_filt_ma(const float *x, int N, const float *coef, const int coef_len, float *y)
{
    TIC(ma)
    smpl_assert(x != y);
    smpl_assert(coef_len > 1);
    int i;
    if (coef[0] == 1.0f) {
        smpl_add_scale_vec(x, x - 1, y, N, coef[1]);
        i = 2;
    } else {
        smpl_scale_vec(x, y, N, coef[0]);
        i = 1;
    }
    for ( ; i < coef_len; i++) {
        smpl_add_scale_vec_inplace(x - i, y, N, coef[i]);
    }
    TOC(ma)
}

// 1st order AR filter
void smpl_filt_ar1(const float *x, int N, const float *coef, int coef_len, float *state, int state_len, float *y)
{
    smpl_assert(coef_len == 2);
    smpl_assert(state_len == 1);
    smpl_assert(coef[0] == 1.0f);
    smpl_assert(coef[1] > -0.9999f);
    smpl_assert(coef[1] <  0.9999f);

    const float ar1 = -coef[1];
    const float ar1_2 = ar1 * ar1;
    const float ar1_3 = ar1 * ar1_2;
    const float ar1_4 = ar1 * ar1_3;
    const float ar1_5 = ar1 * ar1_4;
    float xtmp0, xtmp1, xtmp2, xtmp3, xtmp4;
    float ytmp = state[0];
    int n = 0;
    for (; n < N - 4; n += 5)
    {
        xtmp0 = x[n + 0];
        xtmp1 = x[n + 1];
        xtmp2 = x[n + 2];
        xtmp3 = x[n + 3];
        xtmp4 = x[n + 4];
        y[n + 4] = xtmp4 + ar1 * xtmp3 + ar1_2 * xtmp2 + ar1_3 * xtmp1 + ar1_4 * xtmp0 + ar1_5 * ytmp;
        y[n + 0] = xtmp0 + ar1 * ytmp;
        y[n + 1] = xtmp1 + ar1 * xtmp0 + ar1_2 * ytmp;
        y[n + 2] = xtmp2 + ar1 * xtmp1 + ar1_2 * xtmp0 + ar1_3 * ytmp;
        y[n + 3] = xtmp3 + ar1 * xtmp2 + ar1_2 * xtmp1 + ar1_3 * xtmp0 + ar1_4 * ytmp;
        ytmp = y[n + 4];
    }
    for (; n < N; n++)
    {
        ytmp = x[n] + ytmp * ar1;
        y[n] = ytmp;
    }
    state[0] = ytmp;
}

// 2nd order AR filter
void smpl_filt_ar2(const float *x, int N, const float *coef, int coef_len, float *state, int state_len, float *y)
{
    smpl_assert(coef_len == 3);
    smpl_assert(state_len == 2);
    smpl_assert(coef[0] == 1.0f);
    smpl_assert(coef[2] > -0.9999f);
    smpl_assert(coef[2] <  0.9999f);

    float ytmp0 = state[1];
    float ytmp1 = state[0];
    const float ar1 = -coef[1];
    const float ar2 = -coef[2];
    const float ar1_2 = ar1 * ar1;
    const float ar1_3 = ar1 * ar1_2;
    const float ar1_4 = ar1 * ar1_3;
    const float imp1 = ar1;
    const float imp2 = ar1_2 + ar2;
    const float imp3 = ar1_3 + 2 * ar1 * ar2;
    const float imp4 = ar1_4 + ar2 * ar2 + 3 * ar1_2 * ar2;
#if SMPL_USE_NEON
    float tmp[8] = {0.0f, 0.0f, 0.0f, 1.0f, imp1, imp2, imp3, imp4};
    float32x4_t imp0000 = vld1q_f32(&tmp[0]);
    float32x4_t imp0001 = vld1q_f32(&tmp[1]);
    float32x4_t imp0012 = vld1q_f32(&tmp[2]);
    float32x4_t imp0123 = vld1q_f32(&tmp[3]);
    float32x4_t imp1234 = vld1q_f32(&tmp[4]);
    float32x4_t ymp1234 = vmulq_n_f32(imp0123, ar2);
    float xtmp0 = x[0];
    float xtmp1 = x[1];
    float xtmp2 = x[2];
    float xtmp3 = x[3];
    int n = 0;
    for (; n < N - 7; n += 4)
    {
        // y[n + 0] =                                              xtmp0 + imp1 * ytmp1 + ymp1 * ytmp0;
        // y[n + 1] =                               xtmp1 + imp1 * xtmp0 + imp2 * ytmp1 + ymp2 * ytmp0;
        // y[n + 2] =                xtmp2 + imp1 * xtmp1 + imp2 * xtmp0 + imp3 * ytmp1 + ymp3 * ytmp0;
        // y[n + 3] = xtmp3 + imp1 * xtmp2 + imp2 * xtmp1 + imp3 * xtmp0 + imp4 * ytmp1 + ymp4 * ytmp0;
        float32x4_t acc4;
        acc4 = vmulq_n_f32(      imp0123, xtmp0);
        acc4 = vmlaq_n_f32(acc4, imp0012, xtmp1);
        acc4 = vmlaq_n_f32(acc4, imp0001, xtmp2);
        acc4 = vmlaq_n_f32(acc4, imp0000, xtmp3);
        acc4 = vmlaq_n_f32(acc4, imp1234, ytmp1);
        acc4 = vmlaq_n_f32(acc4, ymp1234, ytmp0);
        vst1q_f32(&y[n],  acc4);

        // load future input samples, hence the "n < N - 7" in the for-loop
        xtmp0 = x[n + 4];
        xtmp1 = x[n + 5];
        xtmp2 = x[n + 6];
        xtmp3 = x[n + 7];

        ytmp0 = y[n + 2];
        ytmp1 = y[n + 3];
    }
#else
    const float ymp1 = ar2;
    const float ymp2 = ar2 * imp1;
    const float ymp3 = ar2 * imp2;
    const float ymp4 = ar2 * imp3;
    float xtmp0, xtmp1, xtmp2, xtmp3;
    int n = 0;
    for (; n < N - 3; n += 4)
    {
        xtmp0 = x[n + 0];
        xtmp1 = x[n + 1];
        xtmp2 = x[n + 2];
        y[n + 2] = xtmp2 + imp1 * xtmp1 + imp2 * xtmp0 + imp3 * ytmp1 + ymp3 * ytmp0;
        xtmp3 = x[n + 3];
        y[n + 3] = xtmp3 + imp1 * xtmp2 + imp2 * xtmp1 + imp3 * xtmp0 + imp4 * ytmp1 + ymp4 * ytmp0;
        y[n + 0] = xtmp0 + imp1 * ytmp1 + ymp1 * ytmp0;
        y[n + 1] = xtmp1 + imp1 * xtmp0 + imp2 * ytmp1 + ymp2 * ytmp0;
        ytmp0 = y[n + 2];
        ytmp1 = y[n + 3];
    }
#endif
    for (; n < N; n++)
    {
        y[n] = x[n] + ar1 * ytmp1 + ar2 * ytmp0;
        ytmp0 = ytmp1;
        ytmp1 = y[n];
    }
    state[1] = ytmp0;
    state[0] = ytmp1;
}

// 4th-order AR function. The state is presumed to sit in the 4 samples before y[0]
void smpl_filt_ar4(const float *x, int N, const float *coef, float *y)
{
    TIC(ar4)
    smpl_assert(coef[0] == 1.0f);
    float coef2[4];
    coef2[0] = coef[2] - coef[1] * coef[1];
    coef2[1] = coef[3] - coef[1] * coef[2];
    coef2[2] = coef[4] - coef[1] * coef[3];
    coef2[3] =         - coef[1] * coef[4];
    float tmp0;
    float res0 = y[-2];
    float res1 = y[-1];
    int n = 0;
    for ( ; n < N - 1; n += 2) {
        tmp0 = x[n]                      - coef[ 4] * y[n - 4] - coef[ 3] * y[n - 3] - coef[ 2] * res0 - coef[ 1] * res1;
        res1 = x[n + 1] - coef[1] * x[n] - coef2[3] * y[n - 4] - coef2[2] * y[n - 3] - coef2[1] * res0 - coef2[0] * res1;
        res0 = tmp0;
        y[n + 0] = res0;
        y[n + 1] = res1;
    }
    for ( ; n < N; n++) {
        float res = x[n];
        for (int i = 0; i < 4; i++) {
            res -= coef[4 - i] * y[n - 4 + i];
        }
        y[n] = res;
    }
    TOC(ar4)
}

// 16th-order AR function. The state is presumed to sit in the 16 samples before y[0]
void smpl_filt_ar16(const float *x, int N, const float *coef, float *y)
{
    TIC(ar16)
    smpl_assert(coef[0] == 1.0f);
#if SMPL_USE_NEON
    float coef_rev[16];
    smpl_reverse_into(coef + 1, coef_rev, 16);
    float32x4_t coef0  = vld1q_f32(&coef_rev[0]);
    float32x4_t coef4  = vld1q_f32(&coef_rev[4]);
    float32x4_t coef8  = vld1q_f32(&coef_rev[8]);
    float32x4_t coef12 = vld1q_f32(&coef_rev[12]);
    for (int n = 0; n < N; n++) {
        float32x4_t y0  = vld1q_f32(&y[n - 16]);
        float32x4_t partialSums = vmulq_f32(coef0, y0);
        float32x4_t y4  = vld1q_f32(&y[n - 12]);
        partialSums = vmlaq_f32(partialSums, coef4, y4);
        float32x4_t y8  = vld1q_f32(&y[n - 8]);
        partialSums = vmlaq_f32(partialSums, coef8, y8);
        float32x4_t y12 = vld1q_f32(&y[n - 4]);
        partialSums = vmlaq_f32(partialSums, coef12, y12);
#if defined(__aarch64__)
        y[n] = x[n] - vaddvq_f32(partialSums);
#else
        partialSums = vaddq_f32(partialSums, vrev64q_f32(partialSums));
        partialSums = vaddq_f32(partialSums, vcombine_f32(vget_high_f32(partialSums), vget_low_f32(partialSums)));
        y[n] = x[n] - vgetq_lane_f32(partialSums, 0);
#endif
    }
#else
    for (int n = 0; n < N; n++) {
        float res = x[n];
        for (int i = 0; i < 16; i++) {
            res -= coef[16 - i] * y[n - 16 + i];
        }
        y[n] = res;
    }
#endif
    TOC(ar16)
}

// 1st order ARMA filter, does not have to be monic (coef_ma[1] can differ from 1)
void smpl_filt_arma1(const float *x, int N, const float *coef_ma, int coef_ma_len, const float *coef_ar, int coef_ar_len, float *state, int state_len, float *y)
{
    TIC(arma1)
    smpl_assert(coef_ma_len == 2);
    smpl_assert(coef_ar_len == 2);
    smpl_assert(state_len == 2);
    smpl_assert(coef_ar[0] == 1.0f);
    smpl_assert(N > 0);

    float *y_ptr, yBuf[SMPL_MAX_FILTER_INPUT_SAMPLES];
    if (x == y) {
        smpl_assert(N <= SMPL_MAX_FILTER_INPUT_SAMPLES);
        y_ptr = yBuf; // Use temp buffer when input and output are identical
    } else {
        y_ptr = y;
    }

    smpl_filt_ma1(x, N, coef_ma, 2, state, 1, y_ptr);
    smpl_filt_ar1(y_ptr, N, coef_ar, coef_ar_len, state + 1, 1, y);

    TOC(arma1)
}

// 2nd order ARMA filter, does not have to be monic (coef_ma[1] can differ from 1)
void smpl_filt_arma2(const float *x, int N, const float* coef_ma, int coef_ma_len, const float* coef_ar, int coef_ar_len, float *state, int state_len, float *y)
{
    TIC(arma2)
    smpl_assert(coef_ma_len == 3);
    smpl_assert(coef_ar_len == 3);
    smpl_assert(state_len == 4);
    smpl_assert(coef_ar[0] == 1.0f);
    smpl_assert(N > 1);

    float *y_ptr, yBuf[SMPL_MAX_FILTER_INPUT_SAMPLES];
    if (x == y) {
        smpl_assert(N <= SMPL_MAX_FILTER_INPUT_SAMPLES);
        y_ptr = yBuf; // Use temp buffer when input and output are identical
    } else {
        y_ptr = y;
    }

    smpl_filt_ma2(x, N, coef_ma, 3, state, 2, y_ptr);
    smpl_filt_ar2(y_ptr, N, coef_ar, coef_ar_len, state + 2, 2, y);

    TOC(arma2)
}

// 2nd order allpass filter
void smpl_allpass2(const float* x, int N, const float* coef, int coef_len, float* state, int state_len, float* y)
{
    smpl_assert(coef_len == 3);
    smpl_assert(coef[0] == 1.0f);
    smpl_assert(state_len == 4);
    smpl_assert((N & 1) == 0);

    float coef_ma[] = {coef[2], coef[1], 1.0f};
    smpl_filt_ma2(x, N, coef_ma, 3, state, 2, y);
    smpl_filt_ar2(y, N, coef, coef_len, state + 2, 2, y);
}

void smpl_up_2x_fast(const float* x, int x_len, const float* coef, int coef_len, float* state, int state_len, float* y, int y_len)
{
    TIC(up_2x)
    smpl_assert(coef_len == SMPL_UP_2X_STATE_LEN);
    smpl_assert(state_len == SMPL_UP_2X_STATE_LEN);
    smpl_assert(y_len <= SMPL_UP_2X_MAX_LEN);
    smpl_assert(x_len * 2 == y_len);
    smpl_assert((x_len & 1) == 0);
    float statea = state[0];
    float stateb = state[1];
    float ca = coef[0];
    float cb = coef[1];
    float ca2 = ca * ca;
    float cb2 = cb * cb;
    float ca3 = ca + ca2;
    float cb3 = cb + cb2;
    for (int i = 0; i < x_len; i += 2) {
        float x0 = x[i + 0];
        float x1 = x[i + 1];
        float tmpa0 = ca * (x0 - statea);
        float tmpb0 = cb * (x0 - stateb);
        float tmpa1 = ca * x1 - ca3 * x0 + ca2 * statea;
        float tmpb1 = cb * x1 - cb3 * x0 + cb2 * stateb;
        y[2 * i + 0] = statea + tmpa0;
        y[2 * i + 1] = stateb + tmpb0;
        y[2 * i + 2] = x0 + tmpa0 + tmpa1;
        y[2 * i + 3] = x0 + tmpb0 + tmpb1;
        statea = x1 + tmpa1;
        stateb = x1 + tmpb1;
    }
    state[0] = statea;
    state[1] = stateb;
    TOC(up_2x)
}

static inline float dot_prod_inline_32_48(const float a[], const float b[])
{
    float ret = 0.0f;
    for(int i = 0; i < SMPL_FIR_N_32_48; i++){
        ret += a[i] * b[i]; 
    }
    return ret;
}

void smpl_up_32_48(const float* x, int x_len, float* state, int state_len, float* y) {
    TIC(up_32_48)
    smpl_assert(state_len == SMPL_FIR_N_32_48);
    smpl_assert(x_len % 2 == 0);

    float xtmp[SMPL_UP_2X_MAX_LEN + SMPL_FIR_N_32_48 - SMPL_UP_2X_STATE_LEN];
    int extra = SMPL_FIR_N_32_48 - SMPL_UP_2X_STATE_LEN;
    smpl_up_2x_fast(x, x_len, smpl_ap_coefs_32_48, SMPL_AP_LEN_32_48, state, SMPL_UP_2X_STATE_LEN, xtmp + extra, 2 * x_len);
    memcpy(xtmp, state + SMPL_UP_2X_STATE_LEN, extra * sizeof(float));
    memcpy(state + SMPL_UP_2X_STATE_LEN, xtmp + 2 * x_len, extra * sizeof(float));

    for (int i = 0; i < x_len / 2; i++) {
        y[3 * i]     = dot_prod_inline_32_48(xtmp + 4 * i,     smpl_fir_coefs_32_48[0]);
        y[3 * i + 1] = dot_prod_inline_32_48(xtmp + 4 * i + 1, smpl_fir_coefs_32_48[1]);
        y[3 * i + 2] = dot_prod_inline_32_48(xtmp + 4 * i + 2, smpl_fir_coefs_32_48[2]);
    }
    TOC(up_32_48)
}

void smpl_interpol(const float *x, float *y, int N) {
    TIC(interpol)
    smpl_assert(sizeof(smpl_interpol_kernel) == 16 * sizeof(smpl_interpol_kernel[0]));
    for(int n = 0; n < N; n++) {
        float ret = 0.0f;
        for(int i = 0; i < 8; i++){
            ret += (x[n + i] + x[n + 15 - i]) * smpl_interpol_kernel[i];
        }
        y[n] = ret;
    }
    TOC(interpol)
}
