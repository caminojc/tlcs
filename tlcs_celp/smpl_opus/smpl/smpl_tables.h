#ifndef SMPL_TABLES_H
#define SMPL_TABLES_H

#include <stdint.h>
#include "smpl_defines.h" 

#ifdef __cplusplus
extern "C" {
#endif
#define SMPL_G_ACB_RD_MU 0.014999999664723873f
#define SMPL_ACBG_N 16
#define SMPL_ACBG_M 2
#define SMPL_V_GAIN_MAX_DB 0.0f
#define SMPL_V_GAIN_MIN_DB -100.0f
#define SMPL_V_GAIN_Q_STEP_DB 3.0f
extern const int16_t smpl_cb_acbgains_lr_Q14[SMPL_ACBG_N*SMPL_ACBG_M];
extern const uint8_t smpl_acbgains_dcmf_lr[(SMPL_ACBG_N+1)*(SMPL_ACBG_N)];
extern const int16_t smpl_cb_acbgains_hr_Q14[SMPL_ACBG_N*SMPL_ACBG_M];
extern const uint8_t smpl_acbgains_dcmf_hr[(SMPL_ACBG_N+1)*(SMPL_ACBG_N)];
#define SMPL_FCBG_V_N 34
extern const uint8_t smpl_fcbg_v_dcmf[SMPL_FCBG_V_N];
#define SMPL_FCBG_V_DELTA_N 67
extern const uint8_t smpl_fcbg_v_delta_dcmf[SMPL_FCBG_V_DELTA_N];
#define SMPL_LTP_INTERPOL_DELAY 8
extern const float smpl_interpol_kernel[2*SMPL_LTP_INTERPOL_DELAY];
extern const float smpl_plc_inject_coef[2];
extern const float smpl_cng_emph_coef[2];
extern const uint16_t smpl_lsf_interp_cmf[3];
#define SMPL_HP_A_LEN 3
//extern const float smpl_hp_a2[SMPL_HP_A_LEN];
//extern const float smpl_hp_b2[SMPL_HP_A_LEN];
#define SMPL_FILTERBANK_A_LEN 3
extern const float smpl_filterbankL_coef[SMPL_FILTERBANK_A_LEN];
extern const float smpl_filterbankH_coef[SMPL_FILTERBANK_A_LEN];
#define SMPL_AP_LEN_32_48 2
extern const float smpl_ap_coefs_32_48[SMPL_AP_LEN_32_48];
#define SMPL_FIR_M_32_48 3
#define SMPL_FIR_N_32_48 12
extern const float smpl_fir_coefs_32_48[SMPL_FIR_M_32_48][SMPL_FIR_N_32_48];
#define SMPL_HB_WGHT_LEN 4
extern const float smpl_hb_wght_coef[SMPL_HB_WGHT_LEN];
#define SMPL_HB_POST_LEN 2
extern const float smpl_hb_post_coef[SMPL_HB_POST_LEN];
#define SMPL_LB_WGHT_LEN 10
#define SMPL_GEN_LOG_PWR 0.2f
extern const float smpl_lb_wght_coef[SMPL_LB_WGHT_LEN];
extern const float smpl_perc_emph_pitch;
extern const float smpl_perc_emph_v[ 2];
extern const float smpl_perc_emph_uv[2];

extern const float smpl_lsf_interpol_1;
extern const float smpl_lsf_interpol_2[2][2];
extern const float smpl_lsf_interpol_4[2][4];

extern const float smpl_lsf_interpol_dtx_1;
extern const float smpl_lsf_interpol_dtx_2[2];
extern const float smpl_lsf_interpol_dtx_4[4];

extern const float smpl_post_tilt_coefs[2][2];

extern const float smpl_uv_pulse_shaping_coefs[2][2][2];

#define SMPL_RESNRG_UPD_FACTOR_DTX 0.2f

extern const float smpl_low_rate_thr[2][4];
extern const uint8_t smpl_max_pulses_per_frame[2][3];
extern const int smpl_fcb_tot_surv_20ms_max[2];
extern const float smpl_vuv_weights[6];
extern const uint16_t smpl_vuv_cmfs[3][3];

extern const float smpl_rate_control_model_comp5[4][2][8];

extern const uint16_t smpl_rate_control_thrs_comp5[4][2];

extern const float smpl_plc_cng_init[SMPL_LPC_ORDER];
#ifdef __cplusplus
}
#endif

#endif
