#include "smpl_get_signal_mode.h"
#include "smpl_defines.h"
#include "smpl_typedef.h"
#include "smpl_tables.h"
#include "smpl_structs.h"
#include "smpl_codec_util.h"
#include <math.h>

static inline float smpl_inv_sigmoid(float x) {
    smpl_assert(x > 0.0f);
    smpl_assert(x < 1.0f);
    return -logf((1.0f / x) - 1.0f);
}

float smpl_get_signal_mode(
    float pitchcorr,
    float *lags,
    float avg_lag,
    float harm_strength,
    int lags_len,
    float* F2,
    int F2_len,
    float sp_act_prob,
    VUV_Mode *vuv_mode)
{
    float corr_strength = smpl_inv_sigmoid(0.1f + 0.75f * SMPL_min(SMPL_max(pitchcorr, 0.0f), 1.0f)); // -1.4 ... 1.4

    smpl_assert(sp_act_prob >= 0 && sp_act_prob <= 1);
    float vad_strength = 0.04f * (1 - 1.04f / (sp_act_prob + 0.04f));  // -1 ... 0

    // tilt
    #define TRANSITION_IX  (SMPL_F_LEN / 3)
    smpl_assert(F2_len == SMPL_F_LEN);
    int i = 2;
    float nrg_lo = 0.0f;
    for (; i < TRANSITION_IX; i++) {
        float tmp = F2[i] * (i + 3);
        nrg_lo += tmp * (TRANSITION_IX - i);
    }
    float nrg_hi = 0.0f;
    for (; i < SMPL_F_LEN; i++) {
        float tmp = F2[i] * (i + 3);
        nrg_hi += tmp * (i - TRANSITION_IX);
    }
    if (vad_strength < -0.1f) {
        float smth_coef = -0.5f * vad_strength;
        vuv_mode->nrg_lo_bgn += smth_coef * (nrg_lo - vuv_mode->nrg_lo_bgn);
        vuv_mode->nrg_hi_bgn += smth_coef * (nrg_hi - vuv_mode->nrg_hi_bgn);
    }
    float tilt_strength = (SMPL_max(nrg_lo - vuv_mode->nrg_lo_bgn, 0.0f) - SMPL_max(nrg_hi - vuv_mode->nrg_hi_bgn, 0.0f)) / 
                     (nrg_lo + nrg_hi + 1e-9f);  // -1.0 ... 1.0
    tilt_strength *= tilt_strength * tilt_strength; // make less binary
    float lag_strength = -smpl_sigmoid(0.25f * (38.0f - avg_lag));

    // weighted average
    float voicing_strength = (smpl_vuv_weights[0] * corr_strength +
                              smpl_vuv_weights[1] * vad_strength +
                              smpl_vuv_weights[2] * tilt_strength +
                              smpl_vuv_weights[3] * harm_strength +
                              smpl_vuv_weights[4] * lag_strength) / smpl_sum_vec(smpl_vuv_weights, 5) + SMPL_VUV_BIAS;

    // hysteresis
    if (vuv_mode->last_lag_prev > 0.0f) {
        float tmp = log2f(lags[0] / (vuv_mode->last_lag_prev));
        if (tmp > 0.0f) {
            tmp *= 0.5f;
        }
        vuv_mode->voicing_prev /= 0.4f + tmp * tmp;
    }
    voicing_strength += (vuv_mode->voicing_prev) * SMPL_VUV_HYST;
    vuv_mode->voicing_prev = tanhf(3.0f * voicing_strength);
    vuv_mode->last_lag_prev = lags[lags_len-1];
    
    return voicing_strength;
}
