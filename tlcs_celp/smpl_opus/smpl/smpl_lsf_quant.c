#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include "smpl_codec_util.h"
#include "smpl_lpc.h"
#include "smpl_lsf_wrapper.h"
#include "smpl_lsf_quant.h"
#include "smpl_lsf_tables.h"
#include "smpl_typedef.h"
#include <assert.h>
#include <string.h>
#include <float.h>

void smpl_lsf_weights_laroia(const float *lsf, float *lsfw)
{
    const float min_dist = 1e-3f;
    float inv_delta[SMPL_LPC_ORDER + 1];
    inv_delta[0] = 1.0f / SMPL_max(lsf[0], min_dist);
    for(int i = 1; i < SMPL_LPC_ORDER; i++) {
        inv_delta[i] = 1.0f / SMPL_max(lsf[i] - lsf[i - 1], min_dist);
    }
    inv_delta[SMPL_LPC_ORDER] = 1.0f / SMPL_max(SMPL_PI - lsf[SMPL_LPC_ORDER - 1], min_dist);
    for(int i = 0; i < SMPL_LPC_ORDER; i++) {
        lsfw[i] = inv_delta[i] + inv_delta[i + 1];
    }
}

void smpl_rot_apply_wght(const float rot[SMPL_LPC_ORDER][SMPL_LPC_ORDER], const float *lsf,
    float wrot1[SMPL_LPC_ORDER][SMPL_LPC_ORDER], float wrot2[SMPL_LPC_ORDER][SMPL_LPC_ORDER])
{
    float lsfw[SMPL_LPC_ORDER], lsfwInv[SMPL_LPC_ORDER];
    smpl_lsf_weights_laroia(lsf, lsfw);
    smpl_sqrt_vec(lsfw, SMPL_LPC_ORDER);
    for (int i = 0; i < SMPL_LPC_ORDER; i++) {
        lsfwInv[i] = 1.0f / lsfw[i];
    }
    for (int i = 0; i < SMPL_LPC_ORDER; i++) {
        for (int j = 0; j < SMPL_LPC_ORDER; j++) {
            wrot1[i][j] = rot[i][j] * lsfwInv[j];
            wrot2[j][i] = rot[i][j] * lsfw[j];
        }
    }
}

static inline int VQ_temp(
    const float lsf[SMPL_LPC_ORDER],
    const float cbhalf[][SMPL_LPC_ORDER],
    const float cbCinv[][SMPL_LPC_ORDER],
    const LSF_cond_params* cond_params,
    int surv,
    int idxs[LSF_CB_CENTROIDS+1])
{
    float tmp[SMPL_LPC_ORDER];
    float err[LSF_CB_CENTROIDS + 1];
    for(int s = 0; s < LSF_CB_CENTROIDS; s++){
        smpl_sub_vec(cbhalf[s], lsf, tmp, SMPL_LPC_ORDER);
        err[s] = -smpl_dot_prod(tmp, cbCinv[s], SMPL_LPC_ORDER);
    }
    int cb_centroids = LSF_CB_CENTROIDS;
    if (cond_params) {
        smpl_sub_vec(cond_params->st1_cbhalf, lsf, tmp, SMPL_LPC_ORDER);
        err[LSF_CB_CENTROIDS] = -smpl_dot_prod(tmp, cond_params->st1_cbCinv, SMPL_LPC_ORDER);
        cb_centroids++;
    }
    smpl_get_maxi_K(err, idxs, cb_centroids, surv);
    return surv;
}

#define SMPL_sign(a) ((a) > 0.0f ? 1 : (a == 0.0f ? 0 : -1))

static void smpl_lsf_quant_core(
    const smpl_LSF_CB_st1* pSt1,
    const smpl_LSF_CB_st2* pSt2,
    int surv,
    const float A[SMPL_LPC_ORDER + 1],
    float RDw_adj,
    int voiced,
    int lowRate,
    const LSF_cond_params *cond_params,
    float *qlsf_out,
    int8_t *qi,
    float *bits_used,
    float *RDbest,
    float wlsf[SMPL_LPC_ORDER])
{
    float lsf[SMPL_LPC_ORDER];
    smpl_A2NLSF_16(lsf, A);

 #ifdef SMPL_USE_SPEC_LSW_WEIGHT
    smpl_lsf_weights(A, lsf, wlsf);
#else
    smpl_lsf_weights_laroia(lsf, wlsf);
#endif

    float qstep = smpl_LSF_qstep[voiced][lowRate];
    float qstep_cond = qstep * LSF_QSTEP_COND_MULT;
    int qim1[LSF_CB_CENTROIDS + 1];
    VQ_temp(lsf, pSt1->cbhalf, pSt1->cbCinv, cond_params, surv, qim1);
    float RD_best = FLT_MAX;
    for (int s1 = 0; s1 < surv; s1++) {
        int qi1 = qim1[s1];
        float lsfq1[SMPL_LPC_ORDER];
        if (qi1 == LSF_CB_CENTROIDS) {
            assert(cond_params != NULL);
            smpl_scale_vec(cond_params->st1_cbhalf, lsfq1, SMPL_LPC_ORDER, 2.0f);
        }else{
            smpl_scale_vec(pSt1->cbhalf[qi1], lsfq1, SMPL_LPC_ORDER, 2.0f);
        }
        float qerr[SMPL_LPC_ORDER];
        {
            float qerr_[SMPL_LPC_ORDER];
            smpl_sub_vec(lsf, lsfq1, qerr_, SMPL_LPC_ORDER);
            const float *wie_ptr = (qi1 == LSF_CB_CENTROIDS) ? &cond_params->st1_wie[0][0] : &pSt1->wie[qi1][0][0];
            smpl_matrix_mult_transp_16(wie_ptr, qerr_, qerr, SMPL_LPC_ORDER, SMPL_LPC_ORDER);
        }
        int8_t qi2[SMPL_LPC_ORDER];
        float inv_qstep = 1.0f / (qi1 < LSF_CB_CENTROIDS ? qstep : qstep_cond);
        smpl_scale_vec_inplace(qerr, SMPL_LPC_ORDER, inv_qstep);
        float bits = (cond_params == NULL) ? pSt1->bits[qi1] : pSt1->bits_cond[qi1];
        int alt[SMPL_LPC_ORDER];
        float abs_qerr[SMPL_LPC_ORDER], qres[SMPL_LPC_ORDER], lsfq[SMPL_LPC_ORDER];
        for (int i = 0; i < SMPL_LPC_ORDER; i++) {
            int qi2_ = roundf(qerr[i]);
            int min_qi = smpl_LSF_St2_min_qi[voiced][lowRate][qi1][i];
            int max_qi = smpl_LSF_St2_max_qi[voiced][lowRate][qi1][i];
            qi2_ = SMPL_min(qi2_, max_qi);
            qi2_ = SMPL_max(qi2_, min_qi);
            qerr[i] -= qi2_;
            alt[i] = SMPL_sign(qerr[i]);
            if ((qi2_ == max_qi && alt[i] > 0) ||
                (qi2_ == min_qi && alt[i] < 0))
            {
                abs_qerr[i] = -1.0f;
            }
            else {
                abs_qerr[i] = SMPL_abs(qerr[i]);
            }
            qi2_ -= min_qi;
            bits += pSt2[qi1].numBits[i][qi2_];
            qres[i] = pSt2[qi1].Qlvls[i][qi2_];
            qi2[i] = (int8_t)qi2_;
        }
        int i_alt[SMPL_LPC_ORDER];
        smpl_get_maxi_K(abs_qerr, i_alt, SMPL_LPC_ORDER, surv);
        const float (*we_ptr)[SMPL_LPC_ORDER][SMPL_LPC_ORDER] = (qi1 == LSF_CB_CENTROIDS) ? &cond_params->st1_we : &pSt1->we[qi1];
        smpl_matrix_mult_transp_16(&(*we_ptr)[0][0], qres, lsfq, SMPL_LPC_ORDER, SMPL_LPC_ORDER);
        smpl_add_vec_inplace(lsfq1, lsfq, SMPL_LPC_ORDER);
        int surv2_ = surv - s1;
        int ind_chgd = -1;
        float bits_ = bits;
        float lsfq_[SMPL_LPC_ORDER];
        memcpy(lsfq_, lsfq, sizeof(lsfq));
        for (int s2 = 0; s2 < surv2_; s2++) {
            SMPL_lsf_min_dist(lsfq, voiced ? smpl_LSF_min_dist_v : smpl_LSF_min_dist_uv);
            float werr = smpl_werr(lsf, lsfq, wlsf);
            float RD = 0.5f * SMPL_LPC_ORDER * log2f(werr) * RDw_adj + bits;
            if (RD < RD_best) {
                RD_best = RD;
                qi[0] = (int8_t)qi1;
                memcpy(qi + 1, qi2, sizeof(int8_t) * SMPL_LPC_ORDER);
                *bits_used = bits;
                memcpy(qlsf_out, lsfq, sizeof(float) * SMPL_LPC_ORDER);
            }
            if (s2 == (surv2_ - 1) || abs_qerr[i_alt[s2]] < 0.25f) {
                break;
            }
            if (s2 > 0) {
                qi2[ind_chgd] -= alt[ind_chgd];
            }
            ind_chgd = i_alt[s2];
            int qi2_old = qi2[ind_chgd];
            qi2[ind_chgd] += alt[ind_chgd];
            int qi2_new = qi2[ind_chgd];
            float qlvls_diff = pSt2[qi1].Qlvls[ind_chgd][qi2_new] - pSt2[qi1].Qlvls[ind_chgd][qi2_old];
            smpl_add_scale_vec(lsfq_, (*we_ptr)[ind_chgd], lsfq, SMPL_LPC_ORDER, qlvls_diff);
            bits = bits_ + pSt2[qi1].numBits[ind_chgd][qi2_new] - pSt2[qi1].numBits[ind_chgd][qi2_old];
        }
     }
     *RDbest = RD_best;
}

void smpl_lsf_quant(
    const int surv,
    const float A[SMPL_LPC_ORDER + 1],
    float RDw_adj,
    int voiced,
    int lowRate,
    float *qlsf,
    int8_t *qi,
    float* bits,
    float* RDbest,
    float wlsf[SMPL_LPC_ORDER])
{
    smpl_assert(voiced == SMPL_TRUE || voiced == SMPL_FALSE);
    smpl_assert(lowRate == SMPL_TRUE || lowRate == SMPL_FALSE);
    smpl_assert(surv <= SMPL_LPC_ORDER);
    const smpl_LSF_CBs* pCb = smpl_get_lsf_CBks();
    smpl_assert(pCb != NULL);
    smpl_lsf_quant_core(&pCb->st1[voiced], &pCb->st2[voiced][lowRate][0], surv, A, RDw_adj, voiced, lowRate, NULL, qlsf, qi, bits, RDbest, wlsf);
}

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
    float wlsf[SMPL_LPC_ORDER])
{
    smpl_assert(voiced == SMPL_TRUE || voiced == SMPL_FALSE);
    smpl_assert(lowRate == SMPL_TRUE || lowRate == SMPL_FALSE);
    smpl_assert(surv <= SMPL_LPC_ORDER);
    const smpl_LSF_CBs* pCb = smpl_get_lsf_CBks();
    smpl_assert(pCb != NULL);
    const smpl_LSF_CB_st1* pSt1 = &pCb->st1[voiced];
    LSF_cond_params cond_params;
    const float* cb_lsfs_mean = voiced ? smpl_LSF_mean_v : smpl_LSF_mean_uv;
    float lsfq_prev_[SMPL_LPC_ORDER];
    for (int i = 0; i < SMPL_LPC_ORDER; i++) {
        lsfq_prev_[i] = lsfq_prev[i] + smpl_LSF_reg_cond[voiced] * (cb_lsfs_mean[i] - lsfq_prev[i]);
        cond_params.st1_cbhalf[i] = 0.5f * lsfq_prev_[i];
    }
    smpl_matrix_mult_transp_16(&pSt1->cInv[0][0], lsfq_prev_, cond_params.st1_cbCinv, SMPL_LPC_ORDER, SMPL_LPC_ORDER);
    smpl_rot_apply_wght(pSt1->Rotcond[lowRate], lsfq_prev_, cond_params.st1_we, cond_params.st1_wie);
    smpl_lsf_quant_core(pSt1, &pCb->st2[voiced][lowRate][0], surv, A, RDw_adj, voiced, lowRate, &cond_params, qlsf, qi, bits, RDbest, wlsf);
}

void smpl_lsf_dequant(
    int8_t *qi,
    const float *lsfq_prev,
    int voiced,
    int lowRate,
    float *lsfq)
{
    smpl_assert(voiced == SMPL_TRUE || voiced == SMPL_FALSE);
    smpl_assert(lowRate == SMPL_TRUE || lowRate == SMPL_FALSE);
    const smpl_LSF_CBs* pCb = smpl_get_lsf_CBks();
    smpl_assert(pCb != NULL);
    smpl_assert(qi[0] >= 0 && qi[0] <= LSF_CB_CENTROIDS);
    smpl_assert(!(qi[0] == LSF_CB_CENTROIDS && lsfq_prev == NULL));
    float qres[SMPL_LPC_ORDER];
    const smpl_LSF_CB_st2* pSt2 = &pCb->st2[voiced][lowRate][qi[0]];
    for (int i = 0; i < SMPL_LPC_ORDER; i++) {
        smpl_assert(qi[i + 1] >= 0 && qi[i + 1] < pSt2->numQlvls[i]);
        qres[i] = pSt2->Qlvls[i][qi[i + 1]];
    }
    float lsfq1[SMPL_LPC_ORDER], lsfq2[SMPL_LPC_ORDER];
    const smpl_LSF_CB_st1* pSt1 = &pCb->st1[voiced];
    if (qi[0] == LSF_CB_CENTROIDS) {
        const float* cb_lsfs_mean = voiced ? smpl_LSF_mean_v : smpl_LSF_mean_uv;
        for (int i = 0; i < SMPL_LPC_ORDER; i++) {
            lsfq1[i] = lsfq_prev[i] + smpl_LSF_reg_cond[voiced] * (cb_lsfs_mean[i] - lsfq_prev[i]);
        }
        float lsfw[SMPL_LPC_ORDER];
        smpl_lsf_weights_laroia(lsfq1, lsfw);
        smpl_sqrt_vec(lsfw, SMPL_LPC_ORDER);
        smpl_matrix_mult_transp_16(&pSt1->Rotcond[lowRate][0][0], qres, lsfq2, SMPL_LPC_ORDER, SMPL_LPC_ORDER);
        for (int i = 0; i < SMPL_LPC_ORDER; i++) {
            lsfq2[i] /= lsfw[i];
        }
    }
    else {
        smpl_scale_vec(pSt1->cbhalf[qi[0]], lsfq1, SMPL_LPC_ORDER, 2.0f);
        smpl_matrix_mult_transp_16(&pSt1->we[qi[0]][0][0], qres, lsfq2, SMPL_LPC_ORDER, SMPL_LPC_ORDER);
    }
    smpl_add_vec(lsfq1, lsfq2, lsfq, SMPL_LPC_ORDER);
    SMPL_lsf_min_dist(lsfq, voiced ? smpl_LSF_min_dist_v : smpl_LSF_min_dist_uv);

}

static float find_min(const float *vec, int len, int* min_ix)
{
    float min = vec[0];
    *min_ix = 0;
    for (int i = 1; i < len; i++) {
        if (vec[i] < min) {
            min = vec[i];
            *min_ix = i;
        }
    }
    return min;
}

void SMPL_lsf_min_dist(float *lsfs, const float *min_dist)
{
    float dlsfs[SMPL_LPC_ORDER + 1];
    dlsfs[0] = (lsfs[0] - 0.0f) - min_dist[0];
    for (int i = 1; i < SMPL_LPC_ORDER; i++) {
        dlsfs[i] = (lsfs[i] - lsfs[i - 1]) - min_dist[i];
    }
    dlsfs[SMPL_LPC_ORDER] = (SMPL_PI - lsfs[SMPL_LPC_ORDER - 1]) - min_dist[SMPL_LPC_ORDER];
    int min_ix = 0;
    float dm = find_min(dlsfs, SMPL_LPC_ORDER+1, &min_ix);
    if (dm > 0.0f) {
        return;
    }
    for (int k = 0; k < 1000; k++) {
        float delta = k * 1.0e-6f - dm;
        dlsfs[min_ix] += delta;
        if (min_ix == 0) {
            dlsfs[1] -= delta;
        }
        else if (min_ix == SMPL_LPC_ORDER) {
            dlsfs[SMPL_LPC_ORDER - 1] -= delta;
        }
        else {
            delta *= 0.5f;
            dlsfs[min_ix - 1] -= delta;
            dlsfs[min_ix + 1] -= delta;
        }
        dm = find_min(dlsfs, SMPL_LPC_ORDER+1, &min_ix);
        if (dm >= 0.0f) {
            lsfs[0] = dlsfs[0] + min_dist[0];
            for (int i = 1; i < SMPL_LPC_ORDER; i++) {
                lsfs[i] = lsfs[i-1] + (dlsfs[i] + min_dist[i]);
            }
            return;
        }
    }
    smpl_assert(0);
}

#ifdef SMPL_USE_SPEC_LSW_WEIGHT
typedef struct smpl_cmplx {
    float re;
    float im;
} smpl_cmplx;

static inline void mult_cmplx(smpl_cmplx* a, smpl_cmplx* b, smpl_cmplx* c)
{
    float re = (a->re * b->re) - (a->im * b->im);
    float im = (a->re * b->im) + (a->im * b->re);
    c->re = re;
    c->im = im;
}

static inline float abs2(smpl_cmplx* a)
{
    return a->re * a->re + a->im * a->im;
}

void smpl_lsf_weights(const float A[SMPL_LPC_ORDER + 1], const float lsf[SMPL_LPC_ORDER], float lsfw[SMPL_LPC_ORDER])
{
    for (int i = 0; i < SMPL_LPC_ORDER; i++) {
        struct smpl_cmplx e = { cosf(lsf[i]), sinf(lsf[i]) };
        struct smpl_cmplx acc = { 1.0f, 0.0f };
        struct smpl_cmplx e_ = { e.re, e.im };
        for (int j = 1; j < SMPL_LPC_ORDER; j++) {
            acc.re += e_.re * A[j];
            acc.im -= e_.im * A[j];
            mult_cmplx(&e, &e_, &e_);
            // Alternative e_ = { cos((j+1)*lsf[i]), sin((j+1)*lsf[i]) }. Profile what is faster
        }
        acc.re += e_.re * A[SMPL_LPC_ORDER];
        acc.im -= e_.im * A[SMPL_LPC_ORDER];
        lsfw[i] = abs2(&acc);
    }
    int dummy;
    float min_lsfw = find_min(lsfw, SMPL_LPC_ORDER, &dummy);
    float scale = 1.0f / min_lsfw;
    for (int i = 0; i < SMPL_LPC_ORDER; i++) {
        lsfw[i] = 1.0f / sqrtf(lsfw[i] * scale);
    }
}
#endif
