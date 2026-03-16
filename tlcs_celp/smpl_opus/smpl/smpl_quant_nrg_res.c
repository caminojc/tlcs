#include "smpl_defines.h"
#include "smpl_typedef.h"
#include "smpl_nrgres_tables.h"
#include "smpl_structs.h"
#include "smpl_helpers.h"
#include "smpl_quant_nrg_res.h"
#include "smpl_codec_util.h"
#include "silk/define.h"
#include "silk/debug.h"

static void* g_smpl_rgresq_data = NULL;

void* smpl_load_nrgresq_data(void)
{
    if (g_smpl_rgresq_data != NULL) {
        return g_smpl_rgresq_data;
    }
    QuantNrgResData* pQNRD = (QuantNrgResData*)calloc(1, sizeof(QuantNrgResData));
    if (!pQNRD) {
        smpl_assert(0);
        return NULL;
    }

    for (int nsubfr_ix = 0; nsubfr_ix < 3; nsubfr_ix++) {
        for (int fcbg_offset_bin = 0; fcbg_offset_bin < SMPL_FCB_G_OFFSET_CMFS; fcbg_offset_bin++) {
            uint16_t* cmf_ptr = pQNRD->fcbg_offset_cmf[nsubfr_ix][fcbg_offset_bin];
            smpl_dcmf_to_cmf(smpl_fcbg_offset_dcmf[nsubfr_ix][fcbg_offset_bin], SMPL_FCB_G_OFFSET_STEPS, cmf_ptr);
        }
    }

    smpl_dcmf_to_cmf(nrgres_shape_CB_2_dcmf, SMPL_RES_NRG_SHAPE_CB_N_2, pQNRD->nrgres_shape_CB_2_cmf);
    smpl_dcmf_to_cmf(nrgres_shape_CB_4_dcmf, SMPL_RES_NRG_SHAPE_CB_N_4, pQNRD->nrgres_shape_CB_4_cmf);
    smpl_dcmf_to_cmf(smpl_nrgres_gain_1_dcmf, SMPL_RES_NRG_Q_STEPS_1, pQNRD->smpl_nrgres_gain_1_cmf);
    smpl_dcmf_to_cmf(smpl_nrgres_gain_2_dcmf, SMPL_RES_NRG_Q_STEPS_2, pQNRD->smpl_nrgres_gain_2_cmf);
    smpl_dcmf_to_cmf(smpl_nrgres_gain_4_dcmf, SMPL_RES_NRG_Q_STEPS_4, pQNRD->smpl_nrgres_gain_4_cmf);
    g_smpl_rgresq_data = (void*)pQNRD;
    return g_smpl_rgresq_data;
}

void smpl_free_nrgresq_data(void)
{
    if (g_smpl_rgresq_data) {
        free((QuantNrgResData*)g_smpl_rgresq_data);
        g_smpl_rgresq_data = NULL;
    }
}

const QuantNrgResData* smpl_get_nrgres_CBks(void)
{
    if (g_smpl_rgresq_data) {
        return ((const QuantNrgResData*)g_smpl_rgresq_data);
    }
    return NULL;
}

static inline int num_subfr_to_idx(int num_subfr)
{
    if (num_subfr == 1) {
        return 0;
    }
    if (num_subfr == 2) {
        return 1;
    }
    if (num_subfr == 4) {
        return 2;
    }
    smpl_assert(0);
    return -1;
}

int smpl_quant_nrg_res(const float nrgres[], int num_subfr, LbQuantParams* pLbParams)
{
    OPUS_UNUSED const QuantNrgResData* pQNRD = smpl_get_nrgres_CBks();
    smpl_assert(pQNRD != NULL);

    float nrgres_db[SMPL_MAX_N_SUBFR], nrgres_frame_db = 0.0f;
    for (int i = 0; i < num_subfr; i++) {
        nrgres_db[i] = SMPL_min(10.0f * log10f(nrgres[i] + SMPL_RES_NRG_BIAS), SMPL_RES_NRG_MAX_DB);
        nrgres_frame_db += nrgres_db[i];
    }
    int table_ix = num_subfr_to_idx(num_subfr);
    nrgres_frame_db /= num_subfr;
    float scQ14 = 1.0f / ((int)1 << 14);
    pLbParams->nrgres_frame_qi = (int)roundf((nrgres_frame_db - SMPL_RES_NRG_MIN_DB) / (scQ14 * smpl_nrg_step_db_Q14[table_ix]));
    int32_t nrgres_frame_dbq_Q14 = ((int32_t)pLbParams->nrgres_frame_qi * (int32_t)smpl_nrg_step_db_Q14[table_ix]);
    nrgres_frame_dbq_Q14 += (((int32_t)SMPL_RES_NRG_MIN_DB) * (1<<14));
    for (int i = 0; i < num_subfr; i++) {
        nrgres_db[i] -= nrgres_frame_dbq_Q14 * scQ14;
    }
    if (num_subfr == 1) {
        pLbParams->nrgres_dbq_Q14[0] = nrgres_frame_dbq_Q14;
        return 0;
    }
    const int nVecs = num_subfr == 4 ? SMPL_RES_NRG_SHAPE_CB_N_4 : SMPL_RES_NRG_SHAPE_CB_N_2;
    const int16_t* cbPtr = num_subfr == 4 ? nrgres_shape_CB_4_Q10 : nrgres_shape_CB_2_Q10;
    float bestRD = 1e30f;
    int qi = 0;
    float scQ10 = 1.0f / ((int)1 << 10);
    for (int n = 0; n < nVecs; n++) {
        float RD = 0.0f;
        for (int i = 0; i < num_subfr; i++) {
            float d = nrgres_db[i] - (cbPtr[n * num_subfr + i] * scQ10);
            RD += d * d;
        }
        if (RD < bestRD) {
            qi = n;
            bestRD = RD;
        }
    }
    pLbParams->nrgres_shape_qi = qi;
    for (int i = 0; i < num_subfr; i++) {
        int32_t nrgres_q14 = nrgres_frame_dbq_Q14 + (((int32_t)cbPtr[qi * num_subfr + i]) * 16);
        pLbParams->nrgres_dbq_Q14[i] = nrgres_q14;
    }

    return 0;
}

float smpl_decode_resnrg(int32_t nrgres_frame_dbq_Q14, int fcb_subfrlen)
{
#ifdef SMPL_USE_POWF_FAST
    float resnrg = smpl_powf_fast(10.0f, 0.1f * (nrgres_frame_dbq_Q14 / (float)((int)1 << 14))) - SMPL_RES_NRG_BIAS;
#else
    float resnrg = powf(10.0f, 0.1f * (nrgres_frame_dbq_Q14 / (float)((int)1 << 14))) - SMPL_RES_NRG_BIAS;
#endif
    resnrg = SMPL_max(resnrg, 0.0f);
    resnrg *= fcb_subfrlen;
    return resnrg;
}
