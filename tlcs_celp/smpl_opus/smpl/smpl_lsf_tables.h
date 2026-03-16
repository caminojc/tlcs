#ifndef SMPL_LSF_TABLES_H
#define SMPL_LSF_TABLES_H

#include <stdint.h> 
#include "smpl_defines.h" 

#ifdef __cplusplus
extern "C" {
#endif
#define LSF_CB_CENTROIDS 16
#define LSF_CB_V_MIN -0.24721986f
#define LSF_CB_V_SCALE 7.226229e-6f
extern const uint16_t smpl_LSF_cb_v_16[LSF_CB_CENTROIDS][SMPL_LPC_ORDER];
#define LSF_CB_UV_MIN -0.5873778f
#define LSF_CB_UV_SCALE 1.3145164e-5f
extern const uint16_t smpl_LSF_cb_uv_16[LSF_CB_CENTROIDS][SMPL_LPC_ORDER];
#define LSF_CINV_V_MIN -2.778548e-5f
#define LSF_CINV_V_SCALE 1.2180106e-9f
extern const uint16_t smpl_LSF_cinv_v_16[SMPL_LPC_ORDER * (SMPL_LPC_ORDER + 1) / 2];
#define LSF_CINV_UV_MIN -3.5960955e-5f
#define LSF_CINV_UV_SCALE 1.8589316e-9f
extern const uint16_t smpl_LSF_cinv_uv_16[SMPL_LPC_ORDER * (SMPL_LPC_ORDER + 1) / 2];
#define LSF_ROT_COND_V_MIN -0.8248211f
#define LSF_ROT_COND_V_SCALE 0.0064186584f
extern const uint8_t smpl_LSF_rot_cond_v_8[2][SMPL_LPC_ORDER][SMPL_LPC_ORDER]; // HR/LR
#define LSF_ROT_COND_UV_MIN -0.67291605f
#define LSF_ROT_COND_UV_SCALE 0.0052386564f
extern const uint8_t smpl_LSF_rot_cond_uv_8[2][SMPL_LPC_ORDER][SMPL_LPC_ORDER]; // HR/LR
#define LSF_ROT_V_MIN -0.8455929f
#define LSF_ROT_V_SCALE 0.0069253775f
extern const uint8_t smpl_LSF_Rot_v_8[LSF_CB_CENTROIDS][SMPL_LPC_ORDER][SMPL_LPC_ORDER];
#define LSF_ROT_UV_MIN -0.9124832f
#define LSF_ROT_UV_SCALE 0.006554049f
extern const uint8_t smpl_LSF_Rot_uv_8[LSF_CB_CENTROIDS][SMPL_LPC_ORDER][SMPL_LPC_ORDER];
#define LSF_QSTEP_COND_MULT 0.9f
extern const float smpl_LSF_reg_cond[2]; // uv/v
extern const float smpl_LSF_mean_v[SMPL_LPC_ORDER];
extern const float smpl_LSF_mean_uv[SMPL_LPC_ORDER];
extern const uint16_t smpl_LSF_CMF_v[LSF_CB_CENTROIDS+1];
extern const uint16_t smpl_LSF_CMF_uv[LSF_CB_CENTROIDS+1];
extern const uint16_t smpl_LSF_CMF_cond_v[LSF_CB_CENTROIDS+2];
extern const uint16_t smpl_LSF_CMF_cond_uv[LSF_CB_CENTROIDS+2];
extern const float smpl_LSF_min_dist_v[SMPL_LPC_ORDER+1];
extern const float smpl_LSF_min_dist_uv[SMPL_LPC_ORDER+1];
extern const int8_t smpl_LSF_St2_min_qi[2][2][LSF_CB_CENTROIDS+1][SMPL_LPC_ORDER]; // uv/v, HR/LR
extern const int8_t smpl_LSF_St2_max_qi[2][2][LSF_CB_CENTROIDS+1][SMPL_LPC_ORDER]; // uv/v, HR/LR
extern const float smpl_LSF_qstep[2][2]; // uv/v, HR/LR
#define LSF_ST2_ALL_QLVLS_LEN 9593
#define LSF_ST2_ALL_QLVL_CMFS_LEN 10681
#define LSF_ST2_ALL_QLVLS_MIN -0.45f
#define LSF_ST2_ALL_QLVLS_SCALE 0.0034478905f
extern const uint8_t smpl_LSF_St2_all_qlvls_8[LSF_ST2_ALL_QLVLS_LEN]; 
extern const uint8_t smpl_LSF_St2_all_qlvl_dcmfs[LSF_ST2_ALL_QLVLS_LEN]; 
#ifdef __cplusplus
}
#endif

#endif
