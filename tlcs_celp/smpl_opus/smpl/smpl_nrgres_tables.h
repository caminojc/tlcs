#ifndef SMPL_NRGRES_TABLES_H
#define SMPL_NRGRES_TABLES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
#define SMPL_RES_NRG_MIN_DB -85
#define SMPL_RES_NRG_MAX_DB 0
#define SMPL_RES_NRG_BIAS 3.1622776e-9f
extern const int16_t smpl_nrg_step_db_Q14[3];
#define SMPL_RES_NRG_SHAPE_CB_N_4 98
extern const int16_t nrgres_shape_CB_4_Q10[SMPL_RES_NRG_SHAPE_CB_N_4 * 4];
extern const uint8_t nrgres_shape_CB_4_dcmf[SMPL_RES_NRG_SHAPE_CB_N_4];
#define SMPL_RES_NRG_SHAPE_CB_N_2 22
extern const int16_t nrgres_shape_CB_2_Q10[SMPL_RES_NRG_SHAPE_CB_N_2 * 2];
extern const uint8_t nrgres_shape_CB_2_dcmf[SMPL_RES_NRG_SHAPE_CB_N_2];
#define SMPL_RES_NRG_Q_STEPS_1 86
#define SMPL_RES_NRG_Q_STEPS_2 71
#define SMPL_RES_NRG_Q_STEPS_4 84
extern const uint8_t smpl_nrgres_gain_1_dcmf[SMPL_RES_NRG_Q_STEPS_1];
extern const uint8_t smpl_nrgres_gain_2_dcmf[SMPL_RES_NRG_Q_STEPS_2];
extern const uint8_t smpl_nrgres_gain_4_dcmf[SMPL_RES_NRG_Q_STEPS_4];
#define SMPL_UV_FCBG_MIN_DB -90
#define SMPL_UV_FCBG_MAX_DB 0
#define SMPL_UV_GAIN_Q_STEP_DB 1
#define SMPL_UV_GAIN_IDX_LEN (SMPL_UV_FCBG_MAX_DB - SMPL_UV_FCBG_MIN_DB) / SMPL_UV_GAIN_Q_STEP_DB
#define SMPL_N_PULSES_STEP 10
#define SMPL_FCB_G_OFFSET_STEPS (SMPL_UV_FCBG_MAX_DB - SMPL_RES_NRG_MIN_DB) - (SMPL_UV_FCBG_MIN_DB - SMPL_RES_NRG_MAX_DB) + 1
#define SMPL_FCB_G_OFFSET_CMFS 4
extern const uint8_t smpl_fcbg_offset_dcmf[3][SMPL_FCB_G_OFFSET_CMFS][SMPL_FCB_G_OFFSET_STEPS];
#ifdef __cplusplus
}
#endif

#endif
