#ifndef SMPL_CELP_H
#define SMPL_CELP_H

#include <stdint.h>
#include "smpl_tables.h"
#include "smpl_defines.h"
#include "smpl_structs.h"
#include "smpl_nrgres_tables.h"

typedef struct CelpTables {
    float dct_mat_t[SMPL_NOISE_CORR_ORDER + 1][SMPL_NOISE_DCT_ORDER];
    uint16_t acbgains_cmf_lr[(SMPL_ACBG_N+1)*(SMPL_ACBG_N+1)];
    uint16_t acbgains_cmf_hr[(SMPL_ACBG_N+1)*(SMPL_ACBG_N+1)];
    float    acbg_inv_prob_lr[(SMPL_ACBG_N+1)*(SMPL_ACBG_N)];
    float    acbg_inv_prob_hr[(SMPL_ACBG_N+1)*(SMPL_ACBG_N)];

    uint16_t fcbgains_v_cmf[SMPL_FCBG_V_N+1];
    uint16_t fcbgains_v_delta_cmf[SMPL_FCBG_V_DELTA_N+1];
    float    fcbgains_v[SMPL_FCBG_V_N];
    float    fcbgains_uv[SMPL_UV_GAIN_IDX_LEN + 1];
    float    fcbg_v_inv_prob[SMPL_FCBG_V_N];
    float    fcbg_v_delta_inv_prob[SMPL_FCBG_V_DELTA_N];
} CelpTables;

extern void* g_smpl_celp_tables;

#ifdef __cplusplus
extern "C" {
#endif

void* smpl_create_celp_encoder(void);

void smpl_init_celp_encoder(void* st, CelpScratch* scratchMem);

void smpl_update_celp_params(
    void* st,
    int fcb_subfrlen,
    int subfr_per_packet,
    int perc_resp_len,
    int ignore_zir,
    int low_rate);

void smpl_free_celp_encoder(void* st);

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
    int16_t gain_idx[SMPL_CELP_MAX_RATES]);

// Get internal signals for plotting
float smpl_get_internals(
    void* st, 
    float res_ltp[],
    float exc_fcb_raw[],
    float exc_fcb[],
    float exc_lpc[]);

void smpl_fcb_search_deldec(
    CelpEncoder* pSt,
    const float d[],
    float pitch_sharp,
    int lag,
    float wnrg_per_pulse[SMPL_CELP_MAX_RATES],
    int16_t fcb_pulses_max[SMPL_CELP_MAX_RATES],
    const int16_t surv[SMPL_MAX_PULSES_PER_SF],
    int16_t pulses[SMPL_CELP_MAX_RATES][SMPL_MAX_PULSES_PER_SF],
    int16_t n_pulses[SMPL_CELP_MAX_RATES],
    float wnrg[SMPL_CELP_MAX_RATES],
    float gain_from_search[SMPL_CELP_MAX_RATES],
    float fcb_wnrg[SMPL_CELP_MAX_RATES]);

void smpl_fcb_search(
    CelpEncoder* pSt,
    const float d[],
    float wnrg_per_pulse[SMPL_CELP_MAX_RATES],
    int16_t fcb_pulses_max[SMPL_CELP_MAX_RATES],
    int16_t pulses[SMPL_CELP_MAX_RATES][SMPL_MAX_PULSES_PER_SF],
    int16_t n_pulses[SMPL_CELP_MAX_RATES],
    float wnrg[SMPL_CELP_MAX_RATES],
    float gain_from_search[SMPL_CELP_MAX_RATES],
    float fcb_wnrg[SMPL_CELP_MAX_RATES]);

void* smpl_create_celp_decoder(void);
void smpl_free_celp_decoder(void* st);

void smpl_gen_excitation(
    const int16_t fcb_gain_ix[],
    const int voiced,
    const int num_subfr,
    const int subfr_len,
    const int nPositions,
    const int16_t positions[],
    const int16_t pos_pulses[],
    float exc[]);

void smpl_celp_decode(
    CelpDecoder* pCelpDec,
    const int voiced,
    float acb_gain[SMPL_ACBG_M],
    const float lags[],
    const int num_lags,
    const int subfrlen,
    const int low_rate,
    const float normalized_bitrate,
    float lpc_res[]);

void smpl_celp_gen_noise(
    NoiseGenerator* pCelpDec,
    const float exc_lpc[],
    const int L,
    const int voiced,
    const int num_pulses,
    const float nrgres,
    const int fcbg_idx,
    const float lsf[SMPL_LPC_ORDER],
    const float normalized_bitrate,
    float noise[]);

void smpl_distribute_fcb_surv(int16_t* numsurv, int max_pulses, int tot_surv);

void acb_synthesize(int fcb_subfrlen, float acb_basis[], float acb_g[SMPL_ACBG_M], float acb[], float high_boost);
void smpl_syn_ltp_basis(const float lags[], int n_lags, float state[], int state_len, float acb_basis[]);
void smpl_pitch_sharp(float x[], int lag, int L);

void acb_dequant(int low_rate, int acb_idx, float acb_g[]);
void adjust_acbgains(float acb_g[SMPL_ACBG_M], float high_boost);

void* smpl_create_celp_tables(void);
void smpl_free_celp_tables(void);
const CelpTables* smpl_get_celp_Tbls(void);

#ifdef __cplusplus
}
#endif

#endif
