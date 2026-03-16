#ifndef SMPL_GET_SIGNAL_MODE_H
#define SMPL_GET_SIGNAL_MODE_H

#include "smpl_structs.h"

#ifdef __cplusplus
extern "C" {
#endif

float smpl_get_signal_mode(
    float pitchcorr, 
    float *lags,
    float avg_lag,
    float harm_strength,
    int lags_len, 
    float* F2, 
    int F2_len,
    float sp_act_prob,
    VUV_Mode *vuv_mode);

#ifdef __cplusplus
}
#endif

#endif
