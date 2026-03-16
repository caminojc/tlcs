#ifndef SMPL_FILT_H
#define SMPL_FILT_H

#ifdef __cplusplus
extern "C" {
#endif

void smpl_filt_ma1(const float *x, int N, const float *coef, int coef_len, float *state, int state_len, float *y);
void smpl_filt_ma2(const float *x, int N, const float *coef, int coef_len, float *state, int state_len, float *y);
void smpl_filt_ma3(const float *x, int N, const float *coef, const int coef_len, float *y);
void smpl_filt_ma9(const float *x, int N, const float *coef, const int coef_len, float *y);
void smpl_filt_ma16_monic(const float *x, int N, const float *coef, const int coef_len, float *y);
void smpl_filt_ma16_sym(const float *x, int N, const float *coef, const int coef_len, float *y);
void smpl_filt_ma(const float *x, int N, const float *coef, const int coef_len, float *y);
void smpl_filt_ar1(const float *x, int N, const float *coef, int coef_len, float *state, int state_len, float *y);
void smpl_filt_ar2(const float *x, int N, const float *coef, int coef_len, float *state, int state_len, float *y);
void smpl_filt_ar4(const float *x, int N, const float *coef, float *y);
void smpl_filt_ar16(const float *x, int N, const float *coef, float *y);
void smpl_filt_arma1(const float *x, int x_len, const float* coef_ma, int coef_ma_len, const float* coef_ar, int coef_ar_len, float *state, int state_len, float *y);
void smpl_filt_arma2(const float *x, int x_len, const float* coef_ma, int coef_ma_len, const float* coef_ar, int coef_ar_len, float *state, int state_len, float *y);
void smpl_allpass2(const float* x, int N, const float* coef, int coef_len, float* state, int state_len, float* y);
void smpl_up_2x_fast(const float* x, int x_len, const float* coef, int coef_len, float* state, int state_len, float* y, int y_len);
void smpl_up_32_48(const float* x, int x_len, float* state, int state_len, float* y);
void smpl_interpol(const float *x, float *y, int N);

#ifdef __cplusplus
}
#endif

#endif  // SMPL_FILT_H
