#include "smpl_lpc.h"
#include "smpl_typedef.h"
#include "smpl_structs.h"
#include "smpl_defines.h"
#include "smpl_lsf_wrapper.h"
#include "smpl_codec_util.h"
#include "pffft.h"
#include "smpl_errors.h"
#include "silk/debug.h"
#include <string.h>
#include <math.h>

static inline void smpl_ac2rc_dbl(const double corr[], int order, double reg, float rc[])
{
    smpl_assert(order > 0);
    smpl_assert(order - 1 <= SMPL_MAX_SF_LEN);
    double C0[SMPL_MAX_SF_LEN], C1[SMPL_MAX_SF_LEN];
    memcpy(C0, corr, (order + 1) * sizeof(double));
    C0[0] *= (1.0f + reg);
    memcpy(C1, C0, (order + 1) * sizeof(double));
    memset(rc, 0, order * sizeof(float));
    for (int k = 0; k < order; k++) {
        // reflection coeficients
        if (C0[k + 1] > C1[0]) {
            rc[k] = -1.0f;
            break;
        }
        if (C0[k + 1] < -C1[0]) {
            rc[k] = 1.0f;
            break;
        }
        if (C1[0] == 0.0) {
            break;
        }
        double rc_tmp = -C0[k + 1] / C1[0];
        rc[k] = (float)rc_tmp;
        // Update correlations
        for (int n = 0; n < (order - k); n++) {
            double Ctmp1 = C0[n + k + 1];
            double Ctmp2 = C1[n];
            C0[n + k + 1] = Ctmp1 + Ctmp2 * rc_tmp;
            C1[n] = Ctmp2 + Ctmp1 * rc_tmp;
        }
    }
}

void smpl_ac2rc(const float corr[], int order, float reg, float rc[])
{
    smpl_assert(order > 0);
    smpl_assert(order - 1 <= SMPL_MAX_SF_LEN);

    double corr_dbl[SMPL_MAX_SF_LEN];
    for (int i = 0; i < (order + 1); i++) {
        corr_dbl[i] = (double)corr[i];
    }
    smpl_ac2rc_dbl(corr_dbl, order, reg, rc);
}

static inline void brute_dct(const LPCTables* pSt, const double F2[SMPL_LPC_NFFT/2+1], double R[], int order)
{
    double F2sum = 0.0, F2_dif[SMPL_LPC_NFFT/4], F2_sumsum[SMPL_LPC_NFFT/4], F2_sumdif[SMPL_LPC_NFFT/4];    
    for(int n = 0; n < SMPL_LPC_NFFT/4; n++) {
        F2sum       += F2[n] +                    F2[SMPL_LPC_NFFT/4+n];
        F2_dif[n]    = F2[n] - F2[SMPL_LPC_NFFT/2-n];
        F2_sumsum[n] = F2[n] + F2[SMPL_LPC_NFFT/2-n] + F2[SMPL_LPC_NFFT/4+n] + F2[SMPL_LPC_NFFT/4-n];
        F2_sumdif[n] = F2[n] + F2[SMPL_LPC_NFFT/2-n] - F2[SMPL_LPC_NFFT/4+n] - F2[SMPL_LPC_NFFT/4-n];
    }
    F2_dif[0] *= 0.5;
    R[0] = (2.0 * F2sum - F2[0] + F2[SMPL_LPC_NFFT/2]) / SMPL_LPC_NFFT;

    for(int j = 0; j < order/2; j++){        
        double Rtmp = 0.0;
        const double *pCdif = pSt->Cdif[j];
        for(int k = 0; k < SMPL_LPC_NFFT/4; k++){
            Rtmp += pCdif[k] * F2_dif[k];
        }
        R[(1 + j*2)] = Rtmp;
    }
    for(int j = 0; j < order/4; j++){        
        double Rtmp = 0.0;
        const double *pCsumdiff = pSt->Csumdiff[j];
        for(int k = 0; k < SMPL_LPC_NFFT/4; k++){
            Rtmp += pCsumdiff[k] * F2_sumdif[k];
        }
        R[(2 + j*4)] = Rtmp;
    }
    for(int j = 0; j < order/4; j++){        
        double Rtmp = 0.0;
        const double *pCsumsum = pSt->Csumsum[j];        
        for(int k = 0; k < SMPL_LPC_NFFT/4; k++){
            Rtmp += pCsumsum[k] * F2_sumsum[k];
        }
        R[(4 + j*4)] = Rtmp;
    }        
}

#define MALLOC_PERCW_NFFT_ALIGNMENT 64

void smpl_lpc(const float x[], int x_len, float reg, float A[], double R[], int order, float F2[])
{
    TIC(lpc)
    smpl_assert(order == 4 || order == 16);
    smpl_assert(x_len <= SMPL_LPC_NFFT);
    smpl_assert(g_smpl_lpc_tables != NULL);
    const LPCTables* pSt = (LPCTables*)g_smpl_lpc_tables;
    char F_mem[SMPL_LPC_NFFT * sizeof(float) + MALLOC_PERCW_NFFT_ALIGNMENT];
    float* F = (float*)(((size_t)F_mem + MALLOC_PERCW_NFFT_ALIGNMENT) & (~((size_t)(MALLOC_PERCW_NFFT_ALIGNMENT - 1))));
    {
        char xBuf_mem[SMPL_LPC_NFFT * sizeof(float) + MALLOC_PERCW_NFFT_ALIGNMENT];
        float* xBuf = (float*)(((size_t)xBuf_mem + MALLOC_PERCW_NFFT_ALIGNMENT) & (~((size_t)(MALLOC_PERCW_NFFT_ALIGNMENT - 1))));
        memcpy(xBuf, x, x_len * sizeof(float));
        memset(&xBuf[x_len], 0, (SMPL_LPC_NFFT - x_len) * sizeof(float));

        pffft_transform_ordered(pSt->pffft_setup, xBuf, F, NULL, PFFFT_FORWARD);
        F2[0] = F[0] * F[0];
        F2[SMPL_LPC_NFFT/2] = F[1] * F[1];
        for(int i = 1; i < SMPL_LPC_NFFT/2; i++){
            F2[i] = F[2*i]*F[2*i] + F[2*i+1]*F[2*i+1];
        }
    }
    double F2double[SMPL_LPC_NFFT/2+1];
    for(int i = 0; i <= SMPL_LPC_NFFT/2; i++){
        F2double[i] = (double)F2[i];
    }
    brute_dct(pSt, F2double, R, order);
    if(A != NULL){
        float rc[SMPL_LPC_ORDER];
        smpl_ac2rc_dbl(R, order, reg, rc);
        smpl_rc2a(rc, order, A);
    }
    TOC(lpc)
}

void smpl_rc2a(const float rc[], int order, float A[])
{
    memset(&A[1], 0, order * sizeof(float));
    A[0] = 1.0f;
    for(int k = 0; k < order; k++){
        const float rc_tmp = rc[k];
        for(int n = 0; n < (k+1)/2; n++){
            const float tmp1 = A[n+1];
            const float tmp2 = A[k-n];
            A[n+1] = tmp1 + tmp2 * rc_tmp;
            A[k-n] = tmp2 + tmp1 * rc_tmp;
        }
        A[k+1] = rc_tmp;
    }
}

void smpl_bwe_expand(float A[], const int lpc_order, const float bwe)
{
    if (bwe <= 0.0f) {
        memset(A + 1, 0.0f, lpc_order * sizeof(float));
        return;
    }
    float c = bwe;
    for (int i = 1; i < lpc_order + 1; i++) {
        A[i] *= c;
        c *= bwe;
    }
}

void smpl_lpc_interpol(const float lsf[], float prev_lsf[], const float lsf_interpol[], int lpc_order, int num_subfr, float A[], float lsfs[])
{
    smpl_assert(lpc_order <= SMPL_LPC_ORDER);
    if (prev_lsf[lpc_order - 1] == 0.0f) { // Handle resets
        memcpy(prev_lsf, lsf, lpc_order * sizeof(float));
    }
    float ilsf[SMPL_LPC_ORDER];
    float prev_lsf_interpol = -1.0f;
    for (int j = 0; j < num_subfr; j++) {
        if (lsf_interpol[j] == prev_lsf_interpol) {
            smpl_assert(j > 0);
            memcpy(&A[j * (lpc_order + 1)], &A[(j-1) * (lpc_order + 1)], (lpc_order+1) * sizeof(float));
        }
        else {
            if (lsf_interpol[j] == 1.0f) {
                memcpy(ilsf, lsf, lpc_order * sizeof(float));
            }
            else {
                smpl_scale_vec(prev_lsf, ilsf, lpc_order, 1.0f - lsf_interpol[j]);
                smpl_add_scale_vec_inplace(lsf, ilsf, lpc_order, lsf_interpol[j]);
            }
            smpl_NLSF2A_stabilize(&A[j * (lpc_order + 1)], ilsf, lpc_order);
        }        
        prev_lsf_interpol = lsf_interpol[j];
        if(lsfs){
            memcpy(&lsfs[j * lpc_order], ilsf, lpc_order * sizeof(float));
        }
    }
    memcpy(prev_lsf, ilsf, lpc_order * sizeof(float));
}

void smpl_lpc_stabilize(float A[], const int lpc_order)
{
    TIC(stabilize)
    if (smpl_lpc_is_stable(A, lpc_order) == SMPL_TRUE) {
        TOC(stabilize)
        return;
    }
    int iter = 0;
    do {
        smpl_bwe_expand(A, lpc_order, 1.0f - ++iter * 0.001f);
    } while (smpl_lpc_is_stable(A, lpc_order) == SMPL_FALSE);
    TOC(stabilize)
}

void smpl_NLSF2A_stabilize(float a[], const float nlsf[], const int d)
{
    smpl_NLSF2A(a, nlsf, d);
    smpl_lpc_stabilize(a, d);
}

#define MAX_RC_STABLE  0.9995f
int smpl_lpc_is_stable(const float A[], const int lpc_order)
{
    smpl_assert(A[0] == 1.0f);
    smpl_assert(lpc_order <= SMPL_LPC_ORDER);
    if (A[lpc_order] * A[lpc_order] > MAX_RC_STABLE) {
        return SMPL_FALSE;
    }
    double a0[SMPL_LPC_ORDER];
    for (int i = 0; i < lpc_order; i++) {
        a0[i] = (double)A[i + 1];
    }
    double a1[SMPL_LPC_ORDER];
    for (int m = lpc_order - 1; ; ) {
        double den = 1.0 - a0[m] * a0[m];
        if (den == 0) {
            return SMPL_FALSE;
        }
        double inv_den = 1.0 / den;
        for (int k = 0; k < m; k++) {
            a1[k] = (a0[k] - a0[m] * a0[m - k - 1]) * inv_den;
        }
        if (a1[m - 1] * a1[m - 1] > MAX_RC_STABLE) {
            return SMPL_FALSE;
        }
        if (--m == 0) {
            return SMPL_TRUE;
        }
        // same code again, just reverse a0 and a1
        den = 1.0 - a1[m] * a1[m];
        if (den == 0) {
            return SMPL_FALSE;
        }
        inv_den = 1.0 / den;
        for (int k = 0; k < m; k++) {
            a0[k] = (a1[k] - a1[m] * a1[m - k - 1]) * inv_den;
        }
        if (a0[m - 1] * a0[m - 1] > MAX_RC_STABLE) {
            return SMPL_FALSE;
        }
        if (--m == 0) {
            return SMPL_TRUE;
        }
    }
}

// Gauss-Newton iterations
void smpl_spec_fact2(float c[3], float A[3])
{
    smpl_assert(c[0] >= 0.0f);
    c[0] += 1e-30f;
    float inv_c0 = 1.0f / c[0];
    float r2 = c[2] * inv_c0;
    float r1 = c[1] / (c[0] * (1 + r2));
    // for k = 1:2
        // da0_2_dr = -2 / v[1] * [r1 r2]
        // R = [2r1 2r2; 1 + r2 r1; 0 1] + v * da0_2_dr
        // e = v[1] / c[1] * c - v
        // dr = R \ e
        // r1 += dr[1]
        // r2 += dr[2]
        // v = [1 + r1^2 + r2^2, r1 + r1 * r2, r2]
    // end
    // [1, r1, r2] .* sqrt(c[1] / v[1])
    for (int iter = 0; iter < 2; iter++) {
        float v0 = 1.0f + r1 * r1 + r2 * r2;
        float v1 = r1 + r1 * r2;
        float s = -2.0f / v0;
        float da0 = s * r1;
        float da1 = s * r2;
        s = v0 * inv_c0;
        float e1 = s * c[1] - v1;
        float e2 = s * c[2] - r2;
        float R0 = 2.0f * r1 + v0 * da0;
        float R3 = 2.0f * r2 + v0 * da1;
        float RR00 = R0 * R0;
        float RR01 = R0 * R3;
        float RR11 = R3 * R3;
        float R1 = 1.0f + r2 + v1 * da0;
        float R4 =        r1 + v1 * da1;
        RR00 += R1 * R1;
        RR01 += R1 * R4;
        RR11 += R4 * R4;
        float Re0 = R1 * e1;
        float Re1 = R4 * e1;
        float R2 =        r2 * da0;
        float R5 = 1.0f + r2 * da1;
        RR00 += R2 * R2;
        RR01 += R2 * R5;
        RR11 += R5 * R5;
        Re0  += R2 * e2;
        Re1  += R5 * e2;
        s = RR00 * RR11 - RR01 * RR01;
        if (s < 1e-4f) break;
        s = 1.0f / s;
        r1 += ( RR11 * Re0 - RR01 * Re1) * s;
        r2 += (-RR01 * Re0 + RR00 * Re1) * s;
    }
    float sc = sqrtf(c[0] / (1.0f + r1 * r1 + r2 * r2));
    A[0] = sc;
    A[1] = sc * r1;
    A[2] = sc * r2;
}
