#include <stdlib.h>
#include "smpl_filt.h"
#include "smpl_structs.h"

#ifndef SMPL_PLC_H
#define SMPL_PLC_H

#ifdef __cplusplus
extern "C" {
#endif

void smpl_plc_reset(PLC* plc, const smpl_TOC* toc);
void smpl_plc_update_celp(
    PLC* plc,
    const LbQuantParams* lb_params,
    const float* acb_gains,
    const float* A,
    const float* lsf,
    const float* lags,
    int lags_per_frame,
    int num_subframes,
    int fcb_subfrlen);

void smpl_plc_update_hb(PLC* plc, const float* A_hb, float hb_gain);

void smpl_plc_conceal_celp(
    PLC *plc,
    LbQuantParams *lb_params,
    float* acb_gains,
    float* A,
    float* lsfs,
    int num_subframes,
    int subfrlen,
    float* lags);

void smpl_plc_conceal_celp_dtx(
    PLC* plc,
    LbQuantParams* lb_params,
    float* lsf_prev,
    float* A,
    float* lsfs,
    int num_subframes,
    float* lags,
    int lags_per_frame);
void smpl_plc_conceal_hb_dtx(const PLC* plc, float* A_hb, float* hb_gains, int num_subframes, int num_hb_subframes);

void smpl_plc_conceal_hb(PLC* plc, float* A_hb, float* hb_gains, int num_hb_subframes);

void smpl_plc_decay_exc(PLC* plc, float* res_lpc, int subfrlen, int reset, int voiced);
void smpl_plc_update_nrg(PLC* plc, float res_nrg, int subfrlen);
void smpl_plc_blend_ltp(const PLC* plc, float* acb_state, float lag);

void smpl_plc_update_cng(
    PLC* plc,
    const float* y,
    const LbQuantParams* lb_params,
    const float* y_hb,
    int swb,
    int coded_as_active_voice,
    int vad,
    int num_fcb_subframes,
    int subfrlen,
    const float* lsfs,
    const float* A_hb,
    const float* hb_exc_gains);

void smpl_add_comfort_noise(PLC* plc, float* y, int y_len, int lostFlag, int band);
void smpl_update_loss_info(PLC* plc, int lostFlag, int packet_len_ms);
void smpl_plc_adapt_lsf(const PLC* plc, float* lsf_prev, int lpc_order);

void smpl_plc_init(PLC* plc);
void smpl_update_recovery_info(PLC* plc, const LbQuantParams* lb_params);
void smpl_plc_bwe_recover(PLC* plc, const LbQuantParams* lb_params, float* A, int num_subframes, int framelen_ms);

#ifdef __cplusplus
}
#endif

#endif
