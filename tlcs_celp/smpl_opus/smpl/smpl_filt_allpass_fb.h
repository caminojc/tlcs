#ifndef SMPL_FILT_ALLPASSS_FB_H
#define SMPL_FILT_ALLPASSS_FB_H

#ifdef __cplusplus
extern "C" {
#endif

void smpl_filt_allpass_fb_ana(const float *x, int x_len, const float* A0, int A0_len, const float *A1, int A1_len, float* yL, float* yH, float* state, int state_len);
void smpl_filt_allpass_fb_syn(const float* xL, int xL_len, const float* xH, int xH_len, const float* A0, int A0_len, const float* A1, int A1_len, float* y, float* state, int state_len);

#ifdef __cplusplus
}
#endif

#endif
