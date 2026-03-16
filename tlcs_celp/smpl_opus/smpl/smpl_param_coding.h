#ifndef SMPL_PARAM_CODING_H
#define SMPL_PARAM_CODING_H

#include "smpl_defines.h"
#include "stdint.h"
#include "smpl_structs.h"

#ifdef __cplusplus
extern "C" {
#endif
    void* smpl_create_param_encoder(void);
    void smpl_init_param_encoder(void* pSt);
    unsigned char smpl_encode_toc(const smpl_TOC *TOC);
    void smpl_encode_lb_params(
        void* pSt,
        void* pEcCtx,
        const LbQuantParams* pLbParams,
        int framelen,
        int num_subfr,
        int coded_as_active_voice,
        int cond_coding,
        int low_rate,
        int frame_num,
        int prev_voiced,
        int SID);
    void smpl_free_param_encoder(void *pSt);
    void smpl_encode_hb_params(void* pSt, void* pEcCtx, void* pHe, const HbQuantParams* pHbParams, int frame_length_16, int voiced, const int cond_coding, int low_rate);
    float smpl_encode_lags(const PITCH_data* pPitchData, void* pEcCtx, int blocksegs_ix, const int laginds[], int prev_lagblk, int prev_lagidx, int mode);

    void* smpl_create_param_decoder(void);
    void smpl_decode_toc(unsigned char, smpl_TOC* toc);
    int smpl_decode_lb_params(
        void* pSt,
        void* pEcCtx,
        int framelen,
        int num_subfr,
        int coded_as_active_voice,
        int *cond_coding,
        int low_rate,
        int frame_num,
        int SID,
        LbQuantParams* pLbParams);
    void smpl_free_param_decoder(void* pSt);
    int smpl_decode_hb_params(void* pSt, void* pEcCtx, int frame_length_16, int voiced, int cond_coding, int low_rate, HbQuantParams* pHbParams);
    int smpl_check_end_result(void* pEcCtx);

#ifdef __cplusplus
}
#endif

#endif
