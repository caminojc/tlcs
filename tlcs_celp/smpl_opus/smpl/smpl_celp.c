#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "smpl_celp.h"
#include <math.h>
#include "smpl_codec_util.h"
#include "smpl_helpers.h"
#include "smpl_filt.h"
#include "smpl_lpc.h"
#include "smpl_tables.h"
#include "smpl_typedef.h"
#include "smpl_nrgres_tables.h"
#include <stdio.h>
#include "silk/debug.h"

void *smpl_create_celp_encoder(void)
{
    CelpEncoder *pSt = (CelpEncoder*)calloc(1,sizeof(CelpEncoder));
    if(!pSt){
        return NULL;
    }
    CelpScratch* pSc = (CelpScratch*)calloc(1, sizeof(CelpScratch));
    smpl_init_celp_encoder((void*)pSt, pSc);
    return (void*)pSt;
}

void smpl_init_celp_encoder(void* st, CelpScratch* scratchMem)
{
    CelpEncoder* pSt = (CelpEncoder*)st;
    smpl_assert(pSt != NULL);
    smpl_assert(scratchMem != NULL);
    pSt->scratchMem = scratchMem;
    CelpScratch* pScratch = pSt->scratchMem;
    smpl_assert(pSt != NULL);
    int nBitsrand = (int)ceilf(log2f((float) (RAND_MAX)));
    int reps = 64 / nBitsrand;
    for (int i = 0; i < SMPL_MAX_SF_LEN; i++) {
        uint64_t tmp = (uint64_t)rand();
        for (int r = 1; r < reps; r++) {
            tmp <<= nBitsrand;
            tmp += (uint64_t)rand();
        }
        pSt->sgntrs[i] = tmp;
    }
    memset(pScratch->imp_lpc_, 0, SMPL_LPC_ORDER * sizeof(float));
    pScratch->imp_lpc = pScratch->imp_lpc_ + SMPL_LPC_ORDER;
    pSt->state_wght = pSt->state_wght_ + SMPL_LPC_ORDER;
    pSt->initialized = SMPL_TRUE;
    smpl_update_celp_params(st, SMPL_MAX_SF_LEN, 1, SMPL_PERC_RESP_LEN, SMPL_FALSE, SMPL_TRUE);
}

void smpl_update_celp_params(
    void* st,
    int fcb_subfrlen,
    int subfr_per_packet,
    int perc_resp_len,
    int ignore_zir,
    int low_rate)
{
    CelpEncoder* pSt = (CelpEncoder*)st;
    smpl_assert(pSt != NULL);
    pSt->acb_state_len = fcb_subfrlen + SMPL_MAXPITCH_LEN + SMPL_LTP_INTERPOL_DELAY;
    smpl_assert(fcb_subfrlen <= SMPL_MAX_SF_LEN);
    smpl_assert(perc_resp_len <= SMPL_MAX_L_RESP);
    smpl_assert(fcb_subfrlen <= pSt->acb_state_len);

    if (perc_resp_len != pSt->perc_resp_len) {
        float scale = 1.0f / (2 * SMPL_PERC_RESP_LEN + 1);
        for (int i = 0; i < perc_resp_len; i++) {
            pSt->hanning_win[i] = sinf(SMPL_PI * (perc_resp_len + i + 1) * scale);
        }
        if (perc_resp_len == 10) {
            pSt->perc_filt_ma = smpl_filt_ma9;
        } else {
            pSt->perc_filt_ma = smpl_filt_ma;
        }
    }

    if (subfr_per_packet != pSt->subfr_per_packet) {
        for (int i = 0; i < SMPL_CELP_MAX_RATES; i++) {
            pSt->prev_acb_idx[i] = -1;
            pSt->prev_fcb_idx[i] = -1;
        }
        pSt->subfr_cnt = 0;
    }

    pSt->fcb_subfrlen = fcb_subfrlen;
    pSt->perc_resp_len = perc_resp_len;
    pSt->ignore_zir = ignore_zir;
    pSt->low_rate = low_rate;
    pSt->subfr_per_packet = subfr_per_packet;
}

void smpl_free_celp_encoder(void* st)
{
    CelpEncoder *pSt = (CelpEncoder*)st;
    if (pSt->scratchMem) {
        free(pSt->scratchMem);
    }
    free(pSt);
}

void* g_smpl_celp_tables = NULL;

void* smpl_create_celp_tables(void)
{
    if (g_smpl_celp_tables) {
        return g_smpl_celp_tables;
    }
    CelpTables* pSt = (CelpTables*)calloc(1, sizeof(CelpTables));
    if (!pSt) {
        smpl_assert(0);
        return NULL;
    }
    float sc = 1.0f / sqrtf(SMPL_NOISE_DCT_ORDER);
    for (int i = 0; i < SMPL_NOISE_DCT_ORDER; i++) {
        float dOmega = (( 0.5f + i) * SMPL_PI) / SMPL_NOISE_DCT_ORDER;
        float omega = 0.0f;
        for (int j = 0; j < SMPL_NOISE_CORR_ORDER+1; j++) {
            pSt->dct_mat_t[j][i] = cosf(omega) * sc;
            omega += dOmega;
        }
    }
    const uint8_t *dcmf_ptr = smpl_acbgains_dcmf_lr;
    uint16_t *cmf_ptr = pSt->acbgains_cmf_lr;
    for (int i = 0; i < SMPL_ACBG_N+1; i++) {
        smpl_dcmf_to_cmf(dcmf_ptr, SMPL_ACBG_N, cmf_ptr);
        dcmf_ptr += SMPL_ACBG_N;
        cmf_ptr  += SMPL_ACBG_N+1;
    }
    dcmf_ptr = smpl_acbgains_dcmf_hr;
    cmf_ptr = pSt->acbgains_cmf_hr;
    for (int i = 0; i < SMPL_ACBG_N+1; i++) {
        smpl_dcmf_to_cmf(dcmf_ptr, SMPL_ACBG_N, cmf_ptr);
        dcmf_ptr += SMPL_ACBG_N;
        cmf_ptr  += SMPL_ACBG_N+1;
    }

    float* acbg_ptr_lr = pSt->acbg_inv_prob_lr;
    float* acbg_ptr_hr = pSt->acbg_inv_prob_hr;
    const uint16_t* cmf_ptr_lr = pSt->acbgains_cmf_lr;
    const uint16_t* cmf_ptr_hr = pSt->acbgains_cmf_hr;
    for (int i = 0; i < SMPL_ACBG_N+1; i++) {
        smpl_cmf_to_bits(cmf_ptr_lr, SMPL_ACBG_N+1, acbg_ptr_lr);
        smpl_cmf_to_bits(cmf_ptr_hr, SMPL_ACBG_N+1, acbg_ptr_hr);
        for (int j = 0; j < SMPL_ACBG_N; j++) {
            acbg_ptr_lr[j] = powf(2.0f, acbg_ptr_lr[j] * SMPL_G_ACB_RD_MU);
            acbg_ptr_hr[j] = powf(2.0f, acbg_ptr_hr[j] * SMPL_G_ACB_RD_MU);
        }
        acbg_ptr_lr += SMPL_ACBG_N;
        acbg_ptr_hr += SMPL_ACBG_N;
        cmf_ptr_lr  += SMPL_ACBG_N+1;
        cmf_ptr_hr  += SMPL_ACBG_N+1;
    }

    smpl_dcmf_to_cmf(smpl_fcbg_v_dcmf, SMPL_FCBG_V_N, pSt->fcbgains_v_cmf);
    smpl_dcmf_to_cmf(smpl_fcbg_v_delta_dcmf, SMPL_FCBG_V_DELTA_N, pSt->fcbgains_v_delta_cmf);

    smpl_cmf_to_bits(pSt->fcbgains_v_cmf, SMPL_FCBG_V_N+1, pSt->fcbg_v_inv_prob);
    for (int i = 0; i < SMPL_FCBG_V_N; i++) {
        pSt->fcbg_v_inv_prob[i] = powf(2.0f, pSt->fcbg_v_inv_prob[i] * SMPL_G_ACB_RD_MU);
    }
    smpl_cmf_to_bits(pSt->fcbgains_v_delta_cmf, SMPL_FCBG_V_DELTA_N+1, pSt->fcbg_v_delta_inv_prob);
    for (int i = 0; i < SMPL_FCBG_V_DELTA_N; i++) {
        pSt->fcbg_v_delta_inv_prob[i] = powf(2.0f, pSt->fcbg_v_delta_inv_prob[i] * SMPL_G_ACB_RD_MU);
    }

    for (int ix = 0; ix < SMPL_FCBG_V_N; ix++) {
        float fcb_gain_db = (float)ix * SMPL_V_GAIN_STEP_DB + SMPL_V_GAIN_MIN_DB;
        pSt->fcbgains_v[ix] = powf(10.0f, 0.05f * fcb_gain_db);
    }
    for (int ix = 0; ix <= SMPL_UV_GAIN_IDX_LEN; ix++) {
        float fcb_gain_db = (float)ix * SMPL_UV_GAIN_STEP_DB + SMPL_UV_GAIN_MIN_DB;
        pSt->fcbgains_uv[ix] = powf(10.0f, 0.05f * fcb_gain_db);
    }

    g_smpl_celp_tables = (void*)pSt;
    return pSt;
}

void smpl_free_celp_tables(void)
{
    if (!g_smpl_celp_tables) {
        return;
    }
    CelpTables* pSt = (CelpTables*)g_smpl_celp_tables;
    free(pSt);

    g_smpl_celp_tables = NULL;
}

const CelpTables* smpl_get_celp_Tbls(void)
{
    if (g_smpl_celp_tables) {
        return ((const CelpTables*)g_smpl_celp_tables);
    }
    return NULL;
}

static inline float smpl_wnrg2(float *C, float *x) {
    return x[0] * (C[0] * x[0] + C[1] * x[1]) +
           x[1] * (C[2] * x[0] + C[3] * x[1]);
}

static int calc_acb_gain(
    CelpEncoder *pSt,
    int L_resp,
    const float acb_basis[],
    const float d_lpc[],
    ACBGparams *acbg_params,
    float d_ltp[]
)
{
    CelpTables* pTbl = (CelpTables*)g_smpl_celp_tables;
    smpl_assert(pTbl != NULL);
    CelpScratch* pScratch = pSt->scratchMem;
    smpl_assert(pScratch != NULL);

    for(int m = 0; m < SMPL_ACBG_M; m++){
        smpl_mult_symtoepl2(pScratch->PhiFlip + SMPL_MAX_SF_LEN - L_resp + 1, L_resp, &acb_basis[m*pSt->fcb_subfrlen], &acbg_params->acb_basis_phi[m*pSt->fcb_subfrlen], pSt->fcb_subfrlen);
        for(int i = 0; i < SMPL_ACBG_M; i++){
            acbg_params->Phi_acb[m*SMPL_ACBG_M + i] = smpl_dot_prod(&acb_basis[i*pSt->fcb_subfrlen], &acbg_params->acb_basis_phi[m*pSt->fcb_subfrlen], pSt->fcb_subfrlen);
        }
        acbg_params->d_acb_lpc[m] = smpl_dot_prod(&acb_basis[m*pSt->fcb_subfrlen], d_lpc, pSt->fcb_subfrlen);
    }

    float best_RD = 1e30f;
    int best_acbg_idx = 0;
    const int transition_idx = pSt->prev_acb_idx[SMPL_CELP_IDX_MAIN] == -1 ? 0 : (pSt->prev_acb_idx[SMPL_CELP_IDX_MAIN] + 1);
    const float* acbg_inv_prob = (pSt->low_rate == SMPL_TRUE ? pTbl->acbg_inv_prob_lr : pTbl->acbg_inv_prob_hr) + transition_idx * SMPL_ACBG_N;
    const int16_t *cb_acbgains = pSt->low_rate == SMPL_TRUE ? smpl_cb_acbgains_lr_Q14 : smpl_cb_acbgains_hr_Q14;
    float acb_gains[SMPL_ACBG_M];
    const float sc_Q14 = 1.0f / ((int)1 << 14);
    for(int n = 0; n < SMPL_ACBG_N; n++){
        for (int m = 0; m < SMPL_ACBG_M; m++) {
            acb_gains[m] = cb_acbgains[n * SMPL_ACBG_M + m] * sc_Q14;
        }
        float werr_out = acbg_params->werr_in + smpl_wnrg2(acbg_params->Phi_acb, acb_gains) -
            2.0f * (acbg_params->d_acb_lpc[0] * acb_gains[0] + acbg_params->d_acb_lpc[1] * acb_gains[1]);  // hardcoded to SMPL_ACBG_M = 2
        float RD = werr_out * acbg_inv_prob[n];
        if(RD < best_RD){
            best_RD = RD;
            best_acbg_idx = n;
        }
    }

    // fcb target signal, hardcoded to SMPL_ACBG_M = 2
    float g = -cb_acbgains[best_acbg_idx * SMPL_ACBG_M] * sc_Q14;
    smpl_add_scale_vec(d_lpc, acbg_params->acb_basis_phi, d_ltp, pSt->fcb_subfrlen, g);
    g = -cb_acbgains[best_acbg_idx * SMPL_ACBG_M + 1] * sc_Q14;
    smpl_add_scale_vec_inplace(&acbg_params->acb_basis_phi[pSt->fcb_subfrlen], d_ltp, pSt->fcb_subfrlen, g);

    return best_acbg_idx;
}

static inline float smpl_wnrg3(float *C, float *x) {
    return x[0] * (C[0] * x[0] + C[1] * x[1] + C[2] * x[2]) +
           x[1] * (C[3] * x[0] + C[4] * x[1] + C[5] * x[2]) +
           x[2] * (C[6] * x[0] + C[7] * x[1] + C[8] * x[2]);
}

static float calc_gains_v(
    CelpEncoder *pSt,
    float fcb_wnrg,
    float gain_from_search,
    const float exc_fcb[],
    const float d_lpc[],
    ACBGparams *acbg_params,
    int rate_idx,
    int16_t acb_idx[SMPL_CELP_MAX_RATES],
    int16_t fcb_idx[SMPL_CELP_MAX_RATES]
)
{
    CelpTables* pTbl = (CelpTables*)g_smpl_celp_tables;
    smpl_assert(pTbl != NULL);

    float fcbgain = SMPL_max(gain_from_search, 0.0f);
    float gain_db = 20.0f * log10f(fcbgain + 1.0e-16f);
    gain_db = SMPL_min(SMPL_max(gain_db, SMPL_V_GAIN_MIN_DB), SMPL_V_GAIN_MAX_DB);
    const int max_gain_idx = (int)roundf((SMPL_V_GAIN_MAX_DB - SMPL_V_GAIN_MIN_DB) / SMPL_V_GAIN_STEP_DB);
    const float g_acb_rd_mu = SMPL_G_ACB_RD_MU;
    if(g_acb_rd_mu > 0.0f) {
        int best_acbg_idx = 0;
        int best_fcbg_idx = 0;

        float acb_fcb[SMPL_ACBG_M];
        for(int i = 0; i < SMPL_ACBG_M; i++){
            acb_fcb[i] = smpl_dot_prod(&acbg_params->acb_basis_phi[i*pSt->fcb_subfrlen], exc_fcb, pSt->fcb_subfrlen);
        }
        float Phi_all[SMPL_ACBG_M+1][SMPL_ACBG_M+1];
        for(int i = 0; i < SMPL_ACBG_M; i++){
            for(int j = 0; j < SMPL_ACBG_M; j++){
                Phi_all[i][j] = acbg_params->Phi_acb[i*SMPL_ACBG_M + j];
            }
        }
        for(int i = 0; i < SMPL_ACBG_M; i++){
            Phi_all[i][SMPL_ACBG_M] = acb_fcb[i];
            Phi_all[SMPL_ACBG_M][i] = acb_fcb[i];
        }
        Phi_all[SMPL_ACBG_M][SMPL_ACBG_M] = fcb_wnrg;
        float dall[SMPL_ACBG_M+1];
        memcpy(dall, acbg_params->d_acb_lpc, SMPL_ACBG_M * sizeof(float));
        dall[SMPL_ACBG_M] = smpl_dot_prod(d_lpc, exc_fcb, pSt->fcb_subfrlen);
        #define N_GAIN_STEPS 2
        int gain_idxs[N_GAIN_STEPS];
        float fcbgains[N_GAIN_STEPS];
        float fcbg_inv_prob[N_GAIN_STEPS];
        int first_gain_idx = SMPL_max((int)floor((gain_db - SMPL_V_GAIN_MIN_DB) / SMPL_V_GAIN_STEP_DB) - (N_GAIN_STEPS-1)/2, 0);
        first_gain_idx = SMPL_min(first_gain_idx, max_gain_idx - 1);
        int offset = (int)floor((SMPL_V_GAIN_MIN_DB - SMPL_V_GAIN_MAX_DB) / SMPL_V_GAIN_STEP_DB);
        for(int i = 0; i < N_GAIN_STEPS; i++){
            gain_idxs[i] = first_gain_idx + i;
            fcbgains[i] = pTbl->fcbgains_v[gain_idxs[i]];
            if(pSt->prev_fcb_idx[rate_idx] == -1) {
                fcbg_inv_prob[i] = pTbl->fcbg_v_inv_prob[gain_idxs[i]];
            }else{
                int delta = pSt->prev_fcb_idx[rate_idx] - gain_idxs[i];
                int cmf_idx = delta - offset;
                fcbg_inv_prob[i] = pTbl->fcbg_v_delta_inv_prob[cmf_idx];
            }
        }
        float best_RD = 1e30f;
        const int   transition_idx = pSt->prev_acb_idx[rate_idx] == -1 ? 0 : (pSt->prev_acb_idx[rate_idx] + 1);
        const int16_t *cb_acbgains = pSt->low_rate == SMPL_TRUE ? smpl_cb_acbgains_lr_Q14 : smpl_cb_acbgains_hr_Q14;
        const float* acbg_inv_prob = (pSt->low_rate == SMPL_TRUE ? pTbl->acbg_inv_prob_lr : pTbl->acbg_inv_prob_hr) + transition_idx * SMPL_ACBG_N;
        const float sc_Q14 = 1.0f / ((int)1 << 14);
        for(int n = 0; n < SMPL_ACBG_N; n++){
            float gains[SMPL_ACBG_M+1];
            for (int m = 0; m < SMPL_ACBG_M; m++) {
                gains[m] = cb_acbgains[n * SMPL_ACBG_M + m] * sc_Q14;
            }
            for(int i = 0; i < N_GAIN_STEPS; i++){
                gains[SMPL_ACBG_M] = fcbgains[i];
                float werr_out = acbg_params->werr_in + smpl_wnrg3(&Phi_all[0][0], gains) -
                    2.0f * (dall[0] * gains[0] + dall[1] * gains[1] + dall[2] * gains[2]);  // hardcoded to SMPL_ACBG_M = 2
                float RD = werr_out * fcbg_inv_prob[i] * acbg_inv_prob[n];
                if(RD < best_RD) {
                    best_RD = RD;
                    best_acbg_idx = n;
                    best_fcbg_idx = gain_idxs[i];
                }
            }
        }
        acb_idx[rate_idx] = best_acbg_idx;
        fcb_idx[rate_idx] = best_fcbg_idx;
    }else{
        fcb_idx[rate_idx] = (int)roundf((gain_db - SMPL_V_GAIN_MIN_DB) / SMPL_V_GAIN_STEP_DB);
    }

    fcb_idx[rate_idx] = SMPL_min(SMPL_max(fcb_idx[rate_idx], 0), max_gain_idx);

    float gain = pTbl->fcbgains_v[fcb_idx[rate_idx]];
    return gain;
}

static inline int16_t quant_gain_uv(float gain_from_search)
{
    float gain_db = 20.0f * log10f(gain_from_search + 1.0e-16f);
    gain_db = SMPL_min(SMPL_max(gain_db, SMPL_UV_GAIN_MIN_DB), SMPL_UV_GAIN_MAX_DB);
    return (int16_t)roundf((gain_db - SMPL_UV_GAIN_MIN_DB) / SMPL_UV_GAIN_STEP_DB);
}

static inline void fcb_synthesize(CelpEncoder *pSt, const int16_t pulses[], int n_pulses, float fcb[])
{
    TIC(fcb_synth)
    memset(fcb, 0, pSt->fcb_subfrlen * sizeof(float));
    for(int n = 0; n < n_pulses; n++){
        int16_t sign = 1.0f + 2 * (pulses[n] >> 15);
        int16_t pos = (pulses[n] * sign) - 1;
        fcb[pos] += sign;
    }
    TOC(fcb_synth)
}

void smpl_celp_encoder(
    void* st,
    float res_lpc[],
    const float predcoef[],
    const float perc_wght_resp[],
    const float lags[],
    const float subfr_importance[SMPL_CELP_MAX_RATES],
    int16_t fcb_pulses_max[SMPL_CELP_MAX_RATES],
    int16_t surv[],
    int16_t pulses[SMPL_CELP_MAX_RATES][SMPL_MAX_PULSES_PER_SF],
    int16_t n_pulses[SMPL_CELP_MAX_RATES],
    int16_t acb_idx[SMPL_CELP_MAX_RATES],
    int16_t gain_idx[SMPL_CELP_MAX_RATES])
{
    CelpEncoder *pSt = (CelpEncoder*)st;
    smpl_assert(pSt != NULL);
    smpl_assert(pSt->initialized);
    CelpScratch* pScratch = pSt->scratchMem;
    smpl_assert(pSt != NULL);
    CelpTables* pTbl = (CelpTables*)g_smpl_celp_tables;
    smpl_assert(pTbl != NULL);
    const int L_resp = pSt->perc_resp_len;
    const int voiced = lags[1] > 0 ? 1 : 0;
    smpl_filt_ar16(perc_wght_resp, L_resp, predcoef, pScratch->imp_lpc);
    smpl_mul_vec_inplace(pSt->hanning_win, pScratch->imp_lpc, L_resp);
    float imp_lpc_rev_[SMPL_MAX_L_RESP + SMPL_MAX_L_RESP - 1];
    float *imp_lpc_rev = imp_lpc_rev_ + SMPL_MAX_L_RESP - 1;
    smpl_reverse_into(pScratch->imp_lpc, imp_lpc_rev, L_resp);
    memset(imp_lpc_rev - (L_resp - 1), 0, (L_resp - 1) * sizeof(float));
    pSt->perc_filt_ma(imp_lpc_rev, L_resp, pScratch->imp_lpc, L_resp, pScratch->Phi);
    smpl_reverse(pScratch->Phi, L_resp);
    memset(&pScratch->Phi[L_resp], 0, (pSt->fcb_subfrlen - L_resp) * sizeof(float));
    memset(pScratch->PhiFlip, 0, sizeof(pScratch->PhiFlip));
    pScratch->PhiFlip[SMPL_MAX_SF_LEN] = pScratch->Phi[0];
    for (int i = 0; i < L_resp + 1; i++) {
        pScratch->PhiFlip[SMPL_MAX_SF_LEN - i] = pScratch->PhiFlip[SMPL_MAX_SF_LEN + i] = pScratch->Phi[i];
    }

    float d_lpc[SMPL_MAX_SF_LEN];
    smpl_mult_symtoepl2(pScratch->PhiFlip + SMPL_MAX_SF_LEN - L_resp + 1, L_resp, res_lpc, d_lpc, pSt->fcb_subfrlen);
    ACBGparams acbg_params;
    float zir_lpc[SMPL_MAX_SF_LEN];
    if (pSt->ignore_zir == SMPL_FALSE) {
        float zir_lpc_tmp_[SMPL_MAX_SF_LEN + SMPL_MAX_L_RESP - 1];
        float *zir_lpc_tmp = zir_lpc_tmp_ + SMPL_MAX_L_RESP - 1;
        float Ht_zir_[SMPL_MAX_L_RESP + SMPL_MAX_L_RESP - 1];
        float *Ht_zir = Ht_zir_ + SMPL_MAX_L_RESP - 1;
        memset(zir_lpc_tmp, 0, L_resp * sizeof(float));
        int state_len = SMPL_max(SMPL_LPC_ORDER, L_resp - 1);
        memcpy(zir_lpc_tmp - state_len, &pSt->state_wght[pSt->fcb_subfrlen - state_len], state_len * sizeof(float));
        smpl_filt_ar16(zir_lpc_tmp, L_resp, predcoef, zir_lpc_tmp);
        pSt->perc_filt_ma(zir_lpc_tmp, L_resp, perc_wght_resp, L_resp, zir_lpc);

        smpl_reverse_into(zir_lpc, zir_lpc_tmp, L_resp);
        memset(zir_lpc_tmp - (L_resp - 1), 0, (L_resp - 1) * sizeof(float));
        pSt->perc_filt_ma(zir_lpc_tmp, L_resp, pScratch->imp_lpc, L_resp, Ht_zir);
        smpl_reverse(Ht_zir, L_resp);
        acbg_params.werr_in = voiced ? smpl_dot_prod(d_lpc, res_lpc, pSt->fcb_subfrlen) + 2.0f * smpl_dot_prod(Ht_zir, res_lpc, L_resp) + smpl_nrg(zir_lpc, L_resp) : 0.0f;
        smpl_add_vec_inplace(Ht_zir, d_lpc, L_resp);
    }
    else {
        memset(zir_lpc, 0, L_resp * sizeof(float));
        acbg_params.werr_in = voiced ? smpl_dot_prod(d_lpc, res_lpc, pSt->fcb_subfrlen) : 0.0f;
    }

    float acb_basis[SMPL_MAX_SF_LEN * SMPL_ACBG_M], acb[SMPL_MAX_SF_LEN], d_ltp[SMPL_MAX_SF_LEN];
    acb_idx[SMPL_CELP_IDX_FEC] = -1;
    acb_idx[SMPL_CELP_IDX_MAIN] = -1;
    if(voiced){
        smpl_syn_ltp_basis(lags, pSt->fcb_subfrlen/SMPL_LAG_SUBFRLEN, pSt->acb_state, pSt->acb_state_len, acb_basis);
        acb_idx[SMPL_CELP_IDX_MAIN] = calc_acb_gain(pSt, L_resp, acb_basis, d_lpc, &acbg_params, d_ltp);
        float acb_gain[SMPL_ACBG_M];
        acb_dequant(pSt->low_rate, acb_idx[SMPL_CELP_IDX_MAIN], acb_gain);
        acb_synthesize(pSt->fcb_subfrlen, acb_basis, acb_gain, acb, 0.0f);
        acb_idx[SMPL_CELP_IDX_FEC] = acb_idx[SMPL_CELP_IDX_MAIN];
    }

    float wtgt_tmp_[SMPL_MAX_SF_LEN + SMPL_MAX_L_RESP + SMPL_MAX_L_RESP - 1];
    float *wtgt_tmp = wtgt_tmp_ + SMPL_MAX_L_RESP - 1;
    float wtgt[SMPL_MAX_SF_LEN + SMPL_MAX_L_RESP];
    memcpy(wtgt_tmp, res_lpc, pSt->fcb_subfrlen * sizeof(float));
    if(voiced){
        smpl_add_scale_vec_inplace(acb, wtgt_tmp, pSt->fcb_subfrlen, -SMPL_RATE_ACB_SCALE);
    }
    memset(&wtgt_tmp[pSt->fcb_subfrlen], 0, L_resp * sizeof(float));
    memset(wtgt_tmp - (L_resp - 1), 0, (L_resp - 1) * sizeof(float));
    pSt->perc_filt_ma(wtgt_tmp, pSt->fcb_subfrlen + L_resp, pScratch->imp_lpc, L_resp, wtgt);
    smpl_add_vec_inplace(zir_lpc, wtgt, L_resp);
    pScratch->nrg_wtgt = smpl_nrg(wtgt, pSt->fcb_subfrlen + L_resp);
    float wnrg_per_pulse[SMPL_CELP_MAX_RATES];
    for (int r = 0; r < SMPL_CELP_MAX_RATES; r++) {
        wnrg_per_pulse[r] = pScratch->nrg_wtgt / (subfr_importance[r] + 1.0e-3f);
    }
    int i_lag = (int)lags[(pSt->fcb_subfrlen/SMPL_LAG_SUBFRLEN)-1];
    float gain_from_search[SMPL_CELP_MAX_RATES], fcb_wnrg[SMPL_CELP_MAX_RATES];
    memset(n_pulses, 0, SMPL_CELP_MAX_RATES * sizeof(int16_t));
    memset(gain_from_search, 0, SMPL_CELP_MAX_RATES * sizeof(float));
    memset(fcb_wnrg, 0, SMPL_CELP_MAX_RATES * sizeof(float));

    if (fcb_pulses_max[SMPL_CELP_IDX_MAIN] > 0) {
        if (fcb_pulses_max[SMPL_CELP_IDX_MAIN] - 1 > 0 && surv[fcb_pulses_max[SMPL_CELP_IDX_MAIN] - 2] == 1 && !pSt->low_rate) {
            smpl_fcb_search(pSt, voiced ? d_ltp : d_lpc, wnrg_per_pulse, fcb_pulses_max,
                pulses, n_pulses, pScratch->wnrg, gain_from_search, fcb_wnrg);
        } else {
            smpl_fcb_search_deldec(pSt, voiced ? d_ltp : d_lpc, SMPL_PITCH_SHARPENING_COEF * pSt->low_rate, i_lag, wnrg_per_pulse, fcb_pulses_max, surv,
                pulses, n_pulses, pScratch->wnrg, gain_from_search, fcb_wnrg);
        }
    }
    float lpc_res_err[SMPL_MAX_SF_LEN], fcbgain = 0.0f;
    gain_idx[SMPL_CELP_IDX_FEC] = -1;
    gain_idx[SMPL_CELP_IDX_MAIN] = -1;
    for (int r = 0; r < SMPL_CELP_MAX_RATES; r++) {
        fcb_synthesize(pSt, pulses[r], n_pulses[r], pScratch->exc_fcb_raw);
        memcpy(pScratch->exc_fcb, pScratch->exc_fcb_raw, pSt->fcb_subfrlen * sizeof(float));
        if (n_pulses[r] > 0) {
            if (voiced) {
                if (pSt->low_rate) {
                    smpl_pitch_sharp(pScratch->exc_fcb, i_lag, pSt->fcb_subfrlen);
                }
                fcbgain = calc_gains_v(pSt, fcb_wnrg[r], gain_from_search[r], pScratch->exc_fcb, d_lpc, &acbg_params, r, acb_idx, gain_idx);
            }
            else {
                gain_idx[r] = quant_gain_uv(gain_from_search[r]);
                fcbgain = pTbl->fcbgains_uv[gain_idx[r]];
            }
            smpl_scale_vec_inplace(pScratch->exc_fcb, pSt->fcb_subfrlen, fcbgain);
        }
    }
    memcpy(pScratch->res_ltp, res_lpc, pSt->fcb_subfrlen * sizeof(float)); // Only used for development plotting
    memcpy(pScratch->exc_lpc, pScratch->exc_fcb, pSt->fcb_subfrlen * sizeof(float));
    if(voiced){
        float acb_gain[SMPL_ACBG_M];
        acb_dequant(pSt->low_rate, acb_idx[SMPL_CELP_IDX_MAIN], acb_gain);
        acb_synthesize(pSt->fcb_subfrlen, acb_basis, acb_gain, acb, 0.0f);
        smpl_add_vec_inplace(acb, pScratch->exc_lpc, pSt->fcb_subfrlen);
        smpl_sub_vec_inplace(acb, pScratch->res_ltp, pSt->fcb_subfrlen);
    }

    // Update adaptive codebook state
    memmove(pSt->acb_state, &pSt->acb_state[pSt->fcb_subfrlen], (pSt->acb_state_len - 2*pSt->fcb_subfrlen) * sizeof(float));
    memcpy(&pSt->acb_state[pSt->acb_state_len - 2*pSt->fcb_subfrlen], pScratch->exc_lpc, pSt->fcb_subfrlen * sizeof(float));

    if (pSt->ignore_zir == SMPL_FALSE) {
        // Update zir state
        smpl_sub_vec(res_lpc, pScratch->exc_lpc, lpc_res_err, pSt->fcb_subfrlen);
        memcpy(pSt->state_wght - SMPL_LPC_ORDER, pSt->state_err_lpc_syn, SMPL_LPC_ORDER * sizeof(float));
        smpl_filt_ar16(lpc_res_err, pSt->fcb_subfrlen, predcoef, pSt->state_wght);
        memcpy(pSt->state_err_lpc_syn, pSt->state_wght + pSt->fcb_subfrlen - SMPL_LPC_ORDER, SMPL_LPC_ORDER * sizeof(float));
    }

    pSt->subfr_cnt++;
    if(pSt->subfr_cnt == pSt->subfr_per_packet){
        for (int r = 0; r < SMPL_CELP_MAX_RATES; r++) {
            pSt->prev_acb_idx[r] = -1;
            pSt->prev_fcb_idx[r] = -1;
        }
        pSt->subfr_cnt = 0;
    }else{
        for(int r = 0; r < SMPL_CELP_MAX_RATES; r++){
            pSt->prev_acb_idx[r] = voiced ? acb_idx[r] : -1;
            pSt->prev_fcb_idx[r] = voiced ? gain_idx[r] : -1;
        }
    }
    pSt->fcbgain = fcbgain;
}

float smpl_get_internals(
    void* st,
    float res_ltp[],
    float exc_fcb_raw[],
    float exc_fcb[],
    float exc_lpc[])
{
    CelpEncoder *pSt = (CelpEncoder*)st;
    smpl_assert(pSt != NULL);
    CelpScratch* pScratch = pSt->scratchMem;
    smpl_assert(pSt != NULL);
    memcpy(res_ltp, pScratch->res_ltp, pSt->fcb_subfrlen * sizeof(float));
    smpl_scale_vec(pScratch->exc_fcb_raw, exc_fcb_raw, pSt->fcb_subfrlen, pSt->fcbgain);
    memcpy(exc_fcb, pScratch->exc_fcb, pSt->fcb_subfrlen * sizeof(float));
    memcpy(exc_lpc, pScratch->exc_lpc, pSt->fcb_subfrlen * sizeof(float));
    return pScratch->wnrg[1] / (pScratch->nrg_wtgt + 1e-12f);
}

void* smpl_create_celp_decoder(void)
{
    CelpDecoder* pSt = (CelpDecoder*)calloc(1, sizeof(CelpDecoder));
    if (!pSt) {
        smpl_assert(0);
        return NULL;
    }
    return (void*)pSt;
}
void smpl_free_celp_decoder(void* st) {
    free(st);
}

void smpl_distribute_fcb_surv(int16_t* numsurv, int max_pulses, int tot_surv) {
    smpl_assert(max_pulses <= 256);
    if (max_pulses <= 1) {
        numsurv[0] = 1;
        return;
    }

    for (int i = 0; i < max_pulses; i++)
    {
        numsurv[i] = 1;
    }
    int sum_surv = max_pulses;
    int extra_surv = tot_surv - max_pulses;
    int extra = SMPL_min(extra_surv / (max_pulses - 1), SMPL_FCB_SRV_MAX - 1);
    for (int i = 0; i < max_pulses - 1; i++)
    {
        numsurv[i] += extra;
    }
    sum_surv += extra * (max_pulses-1);
    // Add extra from top end
    int ix = max_pulses - 2;
    while (sum_surv < tot_surv) {
        if (numsurv[ix] < SMPL_FCB_SRV_MAX) {
            numsurv[ix] += 1;
            sum_surv += 1;
        }
        ix -= 1;
        if (ix < 0) {
            break;
        }
    }
}
