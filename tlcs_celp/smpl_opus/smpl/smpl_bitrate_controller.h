#ifndef SMPL_BITRATE_CONTROLLER_H
#define SMPL_BITRATE_CONTROLLER_H

#include "smpl_structs.h"

#ifdef __cplusplus
extern "C" {
#endif

void bitrate_controller(
    BitrateController* rateCtrl,
    const smpl_EncControlStruct* enc_status,
    const smpl_dtx_status* dtx,
    int coded_as_active_voice,
    float voice_activity_prob,
    float nonflatness,
    float voicing_strength,
    int voiced,
    float wnrg,
    float wnrg_next,
    int low_rate,
    int framelen,
    int subfrlen,
    int16_t max_pulses_per_subfr[SMPL_CELP_MAX_RATES],
    float subfr_importance[SMPL_CELP_MAX_RATES]);

void bitrate_controller_init(BitrateController* rateCtrl);

void bitrate_controller_update_scale(
    BitrateController* rateCtrl,
    smpl_EncControlStruct* enc_status,
    int frame_ms,
    int frames_per_packet,
    const float bits_used[SMPL_CELP_MAX_RATES],
    int coded_as_active_voice);

float smpl_get_nonflatness(const float res_lpc[], int L, float wlsf[SMPL_LPC_ORDER], float state[SMPL_NON_FLAT_STATE_LEN]);

float smpl_get_hr_nonflat_thres(int bitrate, float sp_act_prob);

#ifdef __cplusplus
}
#endif

#endif
