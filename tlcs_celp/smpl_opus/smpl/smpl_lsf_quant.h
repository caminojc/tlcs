#ifndef SMPL_LSF_QUANT_H
#define SMPL_LSF_QUANT_H

#include <stdint.h>
#include "smpl_lsf_tables.h"
#include "smpl_hb_lpc_tables.h"

typedef struct smpl_LSF_CB_st1_ {
    float cbhalf[LSF_CB_CENTROIDS][SMPL_LPC_ORDER];
    float cInv[SMPL_LPC_ORDER][SMPL_LPC_ORDER];
    float bits_cond[LSF_CB_CENTROIDS + 1];
    float Rotcond[2][SMPL_LPC_ORDER][SMPL_LPC_ORDER];  // HR/LR
    float cbCinv[LSF_CB_CENTROIDS][SMPL_LPC_ORDER];
    float we[LSF_CB_CENTROIDS][SMPL_LPC_ORDER][SMPL_LPC_ORDER];
    float bits[LSF_CB_CENTROIDS];
    float wie[LSF_CB_CENTROIDS][SMPL_LPC_ORDER][SMPL_LPC_ORDER];
} smpl_LSF_CB_st1;

typedef struct smpl_LSF_CB_st2_ {
    int numQlvls[SMPL_LPC_ORDER];
    const float* Qlvls[SMPL_LPC_ORDER];
    const float* numBits[SMPL_LPC_ORDER];
    const uint16_t* cmf[SMPL_LPC_ORDER];
} smpl_LSF_CB_st2;

typedef struct smpl_LSF_CBs_ {
    smpl_LSF_CB_st1 st1[2];
    smpl_LSF_CB_st2 st2[2][2][LSF_CB_CENTROIDS + 1]; // uv/v, hi/lo,
    float *QlvlsTable;
    uint16_t *cmfTable;
    float *numBitsTable;
} smpl_LSF_CBs;

extern void* g_smpl_lsf_CBks;

typedef struct LSF_cond_params_ {
    float st1_cbhalf[SMPL_LPC_ORDER];
    float st1_cbCinv[SMPL_LPC_ORDER];
    float st1_we[SMPL_LPC_ORDER][SMPL_LPC_ORDER];
    float st1_wie[SMPL_LPC_ORDER][SMPL_LPC_ORDER];
    float st1_bits;
} LSF_cond_params;

#ifdef __cplusplus
extern "C" {
#endif

void* smpl_load_lsf_CBks(void);
void smpl_free_lsf_CBks(void);
const smpl_LSF_CBs* smpl_get_lsf_CBks(void);

void smpl_lsf_weights_laroia(const float *lsf, float *lsfw);

#ifdef SMPL_USE_SPEC_LSW_WEIGHT
void smpl_lsf_weights(const float A[SMPL_LPC_ORDER + 1], const float lsf[SMPL_LPC_ORDER], float lsfw[SMPL_LPC_ORDER]);
#endif

void smpl_rot_apply_wght(
    const float rot[SMPL_LPC_ORDER][SMPL_LPC_ORDER], 
    const float *lsf, 
    float wrot1[SMPL_LPC_ORDER][SMPL_LPC_ORDER], 
    float wrot2[SMPL_LPC_ORDER][SMPL_LPC_ORDER]);

void smpl_lsf_quant(
    const int surv,
    const float A[SMPL_LPC_ORDER+1],
    float RDw_adj,
    int voiced,
    int lowRate,
    float *qlsf,
    int8_t *qi,
    float* bits_used,
    float* RDbest,
    float wlsf[SMPL_LPC_ORDER]);

void smpl_lsf_quant_cond(
    const int surv,
    const float A[SMPL_LPC_ORDER + 1],
    const float *lsfq_prev,
    float RDw_adj,
    int voiced,
    int lowRate,
    float *qlsf,
    int8_t *qi,
    float* bits,
    float* RDbest,
    float wlsf[SMPL_LPC_ORDER]);

void SMPL_lsf_min_dist(float *lsfs, const float *min_dist);

void smpl_lsf_dequant(
    int8_t *qi,
    const float *lsfq_prev,
    int voiced,
    int lowRate,
    float *lsfq);

#ifdef __cplusplus
}
#endif

#endif
