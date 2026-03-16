#include "smpl_lsf_tables.h"
#include "smpl_typedef.h"
#include "smpl_codec_util.h"
#include "smpl_lsf_quant.h"
#include "smpl_helpers.h"
#include <stdlib.h>
#include <math.h>

void* g_smpl_lsf_CBks = NULL;

void* smpl_load_lsf_CBks(void)
{
    if (g_smpl_lsf_CBks) {
        return g_smpl_lsf_CBks;
    }
    smpl_LSF_CBs* pSt = (smpl_LSF_CBs*)calloc(1, sizeof(smpl_LSF_CBs));
    smpl_assert(pSt);
    if(!pSt){
        return NULL;
    }
    pSt->numBitsTable = (float*)calloc(LSF_ST2_ALL_QLVLS_LEN, sizeof(float));
    smpl_assert(pSt->numBitsTable);
    if(!pSt->numBitsTable){
        return NULL;
    }
    pSt->cmfTable = (uint16_t*)calloc(LSF_ST2_ALL_QLVL_CMFS_LEN, sizeof(uint16_t));
    smpl_assert(pSt->cmfTable);
    if(!pSt->cmfTable){
        return NULL;
    }
    pSt->QlvlsTable = (float*)calloc(LSF_ST2_ALL_QLVLS_LEN, sizeof(float));
    smpl_assert(pSt->QlvlsTable);
    if(!pSt->QlvlsTable){
        return NULL;
    }

    // Load Stage1 data
    for (int voiced = SMPL_FALSE; voiced <= SMPL_TRUE; voiced++) {
        const uint16_t *LSF_cinv_16 = voiced == SMPL_FALSE ? smpl_LSF_cinv_uv_16 : smpl_LSF_cinv_v_16;
        float lsf_cinv_min   = voiced == SMPL_FALSE ? LSF_CINV_UV_MIN   : LSF_CINV_V_MIN;
        float lsf_cinv_scale = voiced == SMPL_FALSE ? LSF_CINV_UV_SCALE : LSF_CINV_V_SCALE;
        const uint16_t (*LSF_cb_16)[SMPL_LPC_ORDER] = voiced == SMPL_FALSE ? smpl_LSF_cb_uv_16 : smpl_LSF_cb_v_16;
        float lsf_cb_min   = voiced == SMPL_FALSE ? LSF_CB_UV_MIN   : LSF_CB_V_MIN;
        float lsf_cb_scale = voiced == SMPL_FALSE ? LSF_CB_UV_SCALE : LSF_CB_V_SCALE;
        const uint8_t (*LSF_Rot_8)[SMPL_LPC_ORDER][SMPL_LPC_ORDER] = voiced == SMPL_FALSE ? smpl_LSF_Rot_uv_8 : smpl_LSF_Rot_v_8;
        float lsf_rot_min   = voiced == SMPL_FALSE ? LSF_ROT_UV_MIN   : LSF_ROT_V_MIN;
        float lsf_rot_scale = voiced == SMPL_FALSE ? LSF_ROT_UV_SCALE : LSF_ROT_V_SCALE;
        const float* smpl_LSF_mean = voiced == SMPL_FALSE ? smpl_LSF_mean_uv : smpl_LSF_mean_v;
        smpl_LSF_CB_st1 *pst1 = &pSt->st1[voiced];
        for (int i = 0; i < SMPL_LPC_ORDER; i++) {
            for (int j = 0; j <= i; j++) {
                pst1->cInv[i][j] = pst1->cInv[j][i] = lsf_cinv_min + lsf_cinv_scale * *LSF_cinv_16++;
            }
        }
        for (int c = 0; c < LSF_CB_CENTROIDS; c++) {
            float LSF_cb[SMPL_LPC_ORDER];
            for (int i = 0; i < SMPL_LPC_ORDER; i++) {
                LSF_cb[i] = lsf_cb_min + LSF_cb_16[c][i] * lsf_cb_scale + smpl_LSF_mean[i];
                pst1->cbhalf[c][i] = LSF_cb[i] * 0.5f;
            }
            smpl_matrix_mult_transp_16(&pst1->cInv[0][0], LSF_cb, pst1->cbCinv[c], SMPL_LPC_ORDER, SMPL_LPC_ORDER);

            float rot[SMPL_LPC_ORDER][SMPL_LPC_ORDER];
            smpl_unpack8(LSF_Rot_8[c][0], rot[0], SMPL_LPC_ORDER * SMPL_LPC_ORDER, lsf_rot_scale, lsf_rot_min);
            smpl_rot_apply_wght(rot, LSF_cb, pst1->we[c], pst1->wie[c]);
        }
    }
    
    smpl_unpack8(smpl_LSF_rot_cond_uv_8[0][0], pSt->st1[0].Rotcond[0][0], SMPL_LPC_ORDER * SMPL_LPC_ORDER, LSF_ROT_COND_UV_SCALE, LSF_ROT_COND_UV_MIN);
    smpl_unpack8(smpl_LSF_rot_cond_uv_8[1][0], pSt->st1[0].Rotcond[1][0], SMPL_LPC_ORDER * SMPL_LPC_ORDER, LSF_ROT_COND_UV_SCALE, LSF_ROT_COND_UV_MIN);
    smpl_unpack8( smpl_LSF_rot_cond_v_8[0][0], pSt->st1[1].Rotcond[0][0], SMPL_LPC_ORDER * SMPL_LPC_ORDER, LSF_ROT_COND_V_SCALE,  LSF_ROT_COND_V_MIN);
    smpl_unpack8( smpl_LSF_rot_cond_v_8[1][0], pSt->st1[1].Rotcond[1][0], SMPL_LPC_ORDER * SMPL_LPC_ORDER, LSF_ROT_COND_V_SCALE,  LSF_ROT_COND_V_MIN);

    smpl_cmf_to_bits(smpl_LSF_CMF_uv,      LSF_CB_CENTROIDS + 1, pSt->st1[0].bits);
    smpl_cmf_to_bits(smpl_LSF_CMF_v,       LSF_CB_CENTROIDS + 1, pSt->st1[1].bits);
    smpl_cmf_to_bits(smpl_LSF_CMF_cond_uv, LSF_CB_CENTROIDS + 2, pSt->st1[0].bits_cond);
    smpl_cmf_to_bits(smpl_LSF_CMF_cond_v,  LSF_CB_CENTROIDS + 2, pSt->st1[1].bits_cond);

    // Load Stage2 data
    const uint8_t* QlvlsRomPtr8 = smpl_LSF_St2_all_qlvls_8;
    // const uint16_t* QlvlsRomPtr8 = smpl_LSF_St2_all_qlvls_16;
    float* QlvlsRomPtr = pSt->QlvlsTable;
    const uint8_t* QlvlsDCmfRomPtr = smpl_LSF_St2_all_qlvl_dcmfs;
    uint16_t* QlvlsCmfRomPtr = pSt->cmfTable;
    float *numBitsPtr = pSt->numBitsTable;
    for (int voiced = SMPL_FALSE; voiced <= SMPL_TRUE; voiced++) {
        for (int lowRate = SMPL_FALSE; lowRate <= SMPL_TRUE; lowRate++) {
            float qstep = smpl_LSF_qstep[voiced][lowRate];
            for (int c = 0; c < LSF_CB_CENTROIDS+1; c++) {
                smpl_LSF_CB_st2 *pst2 = &pSt->st2[voiced][lowRate][c];
                if (c == LSF_CB_CENTROIDS) {
                    qstep *= LSF_QSTEP_COND_MULT;
                }
                for (int i = 0; i < SMPL_LPC_ORDER; i++) {
                    int min_qi = smpl_LSF_St2_min_qi[voiced][lowRate][c][i];
                    int max_qi = smpl_LSF_St2_max_qi[voiced][lowRate][c][i];
                    int numQlvls = max_qi - min_qi + 1;
                    pst2->numQlvls[i] = numQlvls;
                    pst2->Qlvls[i] = QlvlsRomPtr;
                    for (int lvl = 0; lvl < numQlvls; lvl++) {
                        *QlvlsRomPtr++ = (LSF_ST2_ALL_QLVLS_MIN + LSF_ST2_ALL_QLVLS_SCALE * *QlvlsRomPtr8++ + lvl + min_qi) * qstep;
                    }
                    smpl_dcmf_to_cmf(QlvlsDCmfRomPtr, numQlvls, QlvlsCmfRomPtr);
                    pst2->cmf[i] = QlvlsCmfRomPtr;
                    pst2->numBits[i] = numBitsPtr;
                    smpl_cmf_to_bits(QlvlsCmfRomPtr, numQlvls + 1, numBitsPtr);
                    numBitsPtr      += numQlvls;
                    QlvlsDCmfRomPtr += numQlvls;
                    QlvlsCmfRomPtr  += numQlvls + 1;
                }
            }
        }
    }
    smpl_assert(QlvlsRomPtr8 - smpl_LSF_St2_all_qlvls_8 == LSF_ST2_ALL_QLVLS_LEN);
    // smpl_assert(QlvlsRomPtr8 - smpl_LSF_St2_all_qlvls_16 == LSF_ST2_ALL_QLVLS_LEN);
    smpl_assert(QlvlsRomPtr - pSt->QlvlsTable == LSF_ST2_ALL_QLVLS_LEN);
    smpl_assert(QlvlsDCmfRomPtr - smpl_LSF_St2_all_qlvl_dcmfs == LSF_ST2_ALL_QLVLS_LEN);
    smpl_assert(QlvlsCmfRomPtr - pSt->cmfTable == LSF_ST2_ALL_QLVL_CMFS_LEN);
    smpl_assert(numBitsPtr - pSt->numBitsTable == LSF_ST2_ALL_QLVLS_LEN);

    g_smpl_lsf_CBks = (void*)pSt;
    return g_smpl_lsf_CBks;
}

void smpl_free_lsf_CBks(void)
{
    smpl_LSF_CBs *pSt = (smpl_LSF_CBs*)g_smpl_lsf_CBks;
    if (pSt) {
        if (pSt->numBitsTable) {
            free(pSt->numBitsTable);
            pSt->numBitsTable = NULL;
        }
        if (pSt->cmfTable) {
            free(pSt->cmfTable);
            pSt->cmfTable = NULL;
        }
        if (pSt->QlvlsTable) {
            free(pSt->QlvlsTable);
            pSt->QlvlsTable = NULL;
        }
        free(pSt);
        g_smpl_lsf_CBks = NULL;
    }
}

const smpl_LSF_CBs* smpl_get_lsf_CBks(void)
{
    if (g_smpl_lsf_CBks) {
        return ((const smpl_LSF_CBs*)g_smpl_lsf_CBks);
    }
    return NULL;
}
