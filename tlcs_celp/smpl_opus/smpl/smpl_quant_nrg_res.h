#ifndef SMPL_QUANT_RES_NRG_H
#define SMPL_QUANT_RES_NRG_H

#include "stdint.h"
#include "smpl_nrgres_tables.h"

typedef struct QuantNrgResData {
    uint16_t fcbg_offset_cmf[3][SMPL_FCB_G_OFFSET_CMFS][SMPL_FCB_G_OFFSET_STEPS+1];
    uint16_t nrgres_shape_CB_4_cmf[SMPL_RES_NRG_SHAPE_CB_N_4+1];
    uint16_t nrgres_shape_CB_2_cmf[SMPL_RES_NRG_SHAPE_CB_N_4+1];
    uint16_t smpl_nrgres_gain_1_cmf[SMPL_RES_NRG_Q_STEPS_1+1];
    uint16_t smpl_nrgres_gain_2_cmf[SMPL_RES_NRG_Q_STEPS_2+1];
    uint16_t smpl_nrgres_gain_4_cmf[SMPL_RES_NRG_Q_STEPS_4+1];
} QuantNrgResData;

#ifdef __cplusplus
extern "C" {
#endif
    void* smpl_load_nrgresq_data(void);
    void smpl_free_nrgresq_data(void);
    const QuantNrgResData* smpl_get_nrgres_CBks(void);
    int smpl_quant_nrg_res(const float nrgres[], int num_subfr, LbQuantParams* pLbParams);
    float smpl_decode_resnrg(int32_t nrgres_frame_dbq_Q14, int fcb_subfrlen);
#ifdef __cplusplus
}
#endif

#endif
