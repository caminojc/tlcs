#include <math.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include "smpl_postfilter.h"
#include "smpl_codec_util.h"
#include "smpl_defines.h"
#include "smpl_filt.h"
#include "smpl_typedef.h"

#define SMPL_HARM_POSTF_LP_FILT_RES         2500  // higher means more filters
#define SMPL_HARM_POSTF_NUM_LP_FILT         ((SMPL_HARM_POSTF_LP_FILT_RES / 80) - SMPL_HARM_POSTF_LP_FILT_RES / SMPL_MAX_PITCH_LAG + 1)
static inline int lag_to_filt_ix(int lag) {
    smpl_assert(lag != 0);
    return SMPL_HARM_POSTF_LP_FILT_RES / SMPL_max(lag + 30, 80) - SMPL_HARM_POSTF_LP_FILT_RES / SMPL_MAX_PITCH_LAG;
}

static inline void harm_postfilter_core(float LPcoefs[], const float x[], int future_samples, int lag, float diff[], float y_harm[], const int L, float fb_strength, int *prev_did_filter)
{
    const HarmPostFiltTables *pSt = (HarmPostFiltTables*)g_smpl_harm_postfilt_tables;
    smpl_assert(pSt);

    float xy = 0.0f;
    if (lag > 0) {
        int lookforward = L + lag - future_samples;
        if (lookforward > 0) {
            int L_2nd = SMPL_max(L - lookforward, 0);
            smpl_add_vec(x - lag, x + lag, y_harm, L_2nd);
            smpl_add_vec(x + L_2nd - lag, x + L_2nd, y_harm + L_2nd, L - L_2nd);
        } else {
            smpl_add_vec(x - lag, x + lag, y_harm, L);
        }
        xy = smpl_dot_prod(x, y_harm, L);
    }
    if (lag > 0 && xy > 0.0f) {
        float xx =         smpl_nrg(x, L);
        float yy = 0.25f * smpl_nrg(y_harm, L);    // 0.25 because y_harm is 2x larger than it would be if taking the average of x-lag and x+lag
        float strength = 0.5f  * xy / SMPL_max(yy, xx);  // denominator cannot be zero because xy > 0
        float high_lag_reduction = 1.0f - SMPL_HARM_POSTF_REDUCTION_FAC * ((float)(lag - SMPL_MIN_PITCH_LAG) / (SMPL_MAX_PITCH_LAG - SMPL_MIN_PITCH_LAG));
        strength *= high_lag_reduction * SMPL_HARM_POSTF_STRENGTH;
        smpl_scale_vec_inplace(y_harm, L, 0.5f * strength);  // 0.5 because y_harm is 2x larger than it would be if taking the average of x-lag and x+lag
        smpl_add_scale_vec(y_harm, x, diff, L, -strength);

        smpl_scale_vec(pSt->LP_Filters[lag_to_filt_ix(lag)], LPcoefs, 2 * SMPL_HARM_POSTF_FB_DELAY + 1, fb_strength);
        smpl_filt_ma16_sym(diff, L, LPcoefs, 17, y_harm);
        smpl_add_vec_inplace(x - SMPL_HARM_POSTF_FB_DELAY, y_harm, L);
        *prev_did_filter = 1;
    } else {
        memset(diff, 0, SMPL_HARM_POSTF_LAG_SUBFR_LEN * sizeof(float));
        if (*prev_did_filter) {
            // add zero-input response to y_harm
            smpl_filt_ma16_sym(diff, 2 * SMPL_HARM_POSTF_FB_DELAY, LPcoefs, 17, y_harm);
            smpl_add_vec_inplace(x - SMPL_HARM_POSTF_FB_DELAY, y_harm, 2 * SMPL_HARM_POSTF_FB_DELAY);
            memcpy(y_harm + 2 * SMPL_HARM_POSTF_FB_DELAY, x + SMPL_HARM_POSTF_FB_DELAY, (L - 2 * SMPL_HARM_POSTF_FB_DELAY) * sizeof(float));
        } else {
            memcpy(y_harm, x - SMPL_HARM_POSTF_FB_DELAY, L * sizeof(float));
        }
        *prev_did_filter = 0;
    }
}

void smpl_harm_postfilter(void *st, float x[], const int x_len, float lags[], int nLags, float normalized_bitrate)
{
    smpl_assert(st != NULL);
    smpl_assert(x_len == nLags * SMPL_HARM_POSTF_LAG_SUBFR_LEN);
    smpl_assert(x_len <= SMPL_FRAME_LEN * SMPL_MAX_FRAMES_PER_PACKET);
    HarmPst *pHarmPst = (HarmPst *)st;

    float diff_[SMPL_FRAME_LEN + 2 * SMPL_HARM_POSTF_FB_DELAY];
    float *diff = diff_ + 2 * SMPL_HARM_POSTF_FB_DELAY;

    int lag = pHarmPst->prev_lag;
    memcpy(&pHarmPst->StateComb[SMPL_MAX_PITCH_LAG + SMPL_HARM_POSTF_DELAY], x, x_len * sizeof(float));

    float fb_strength = 1.0f - SMPL_HARM_POSTF_FB_STRENGTH * normalized_bitrate;
    int offset1 = 0;

    // process one frame at a time, to keep diff_ short
    int lag_ctr = 0;
    while (lag_ctr < nLags) {
        int offset2 = 0;
        memcpy(diff - 16, pHarmPst->state1, 16 * sizeof(float));
        int lag_ctr_end = SMPL_min(lag_ctr + SMPL_PITCH_NUM_SUBFRAMES, nLags);
        for( ; lag_ctr < lag_ctr_end; lag_ctr++) {
            harm_postfilter_core(pHarmPst->LPcoefs, pHarmPst->StateComb + SMPL_MAX_PITCH_LAG + offset1, SMPL_HARM_POSTF_DELAY + x_len - offset1, 
                lag, diff + offset2, x + offset1, SMPL_HARM_POSTF_LAG_SUBFR_LEN, fb_strength, &pHarmPst->prev_did_filter);
            offset1 += SMPL_HARM_POSTF_LAG_SUBFR_LEN;
            offset2 += SMPL_HARM_POSTF_LAG_SUBFR_LEN;
            lag = (int)roundf(lags[lag_ctr]);
        }
        memcpy(pHarmPst->state1, diff + offset2 - 16, 16 * sizeof(float));
    }

    pHarmPst->prev_lag = lag;
    memmove(pHarmPst->StateComb, pHarmPst->StateComb + x_len, (SMPL_MAX_PITCH_LAG + SMPL_HARM_POSTF_DELAY) * sizeof(float));
}
