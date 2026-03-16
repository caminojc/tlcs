#include "smpl_defines.h"
#include "smpl_typedef.h"
#include "smpl_param_coding.h"
#include "smpl_pitch.h"
#include "smpl_pulse_coding.h"
#include "smpl_entropy_wrapper.h"
#include "smpl_lsf_quant.h"
#include "smpl_tables.h"
#include "smpl_nrgres_tables.h"
#include "smpl_lsf_tables.h"
#include "smpl_hb_gain_tables.h"
#include "smpl_hb_lpc_tables.h"
#include "smpl_bandwidth_extension.h"
#include "smpl_quant_nrg_res.h"
#include "smpl_celp.h"
#include "silk/debug.h"

void *smpl_create_param_encoder(void)
{
    ParamsEncoder* pPE = (ParamsEncoder*)calloc(1, sizeof(ParamsEncoder));
    if (!pPE) {
        smpl_assert(0);
        return NULL;
    }
    smpl_init_param_encoder((void*)pPE);
    return (void*)pPE;
}

void smpl_init_param_encoder(void* pSt)
{
    ParamsEncoder* pPE = (ParamsEncoder*)pSt;
    pPE->prev_acb_idx = -1;
    pPE->prev_fcb_idx = -1;
    pPE->prev_lagblk = -1;
    pPE->prev_lagidx = -1;
}

void smpl_free_param_encoder(void *pSt)
{
    if (!pSt) {
        return;
    }
    ParamsEncoder* pPE = (ParamsEncoder*)pSt;
    free(pPE);
}

unsigned char smpl_encode_toc(const smpl_TOC* toc)
{
//  TOC, at the moment 8 bits
//
//  For SMPL
//           +--1--+--2--+--3--+--4--+--5--+--6--+--7--+--8--+
//           |  SID/VoA  | Fs  |  FrameSz  | Rate| FEC | Ste |
//           +-----+-----+-----+-----+-----+-----+-----+-----+
//
// SID/VoA - 00: Normal frame(s) sent while there is no voice activity i.e. when DTX is off
//           01: Normal frame(s) with some voice activity
//           10: SID frame(s) (no voice activity)
//           11: CELT used => Should never hit this while in this part of the code
// Fs      -  0: 16kHz mode without any bits for 32/48kHz mode
//            1: 32/48kHz mode. Decoder can decide to get either 32 or 48kHz
// FrameSz - 00:  10ms SMPL frame
//           01:  20ms SMPL frame
//           10:  60ms SMPL frame
//           11: 120ms SMPL frame
// Rate       0: Low Rate mode of SMPL
//            1: High rate mode of SMPL
// FEC     -  0: Packed does not contain FEC frame
//            1: If VoA == 1 Packet contains FEC frame (not yet used)
//               If VoA == 0 Packet does not contain FEC but frame(s) are encoded as if they may contain voiced (hang-over period)
// Ste     -  0: Frame is mono
//            1: Frame contains stereo information
    unsigned char byte = 0;

    // Validate that combinations are allowed
    smpl_assert(!(toc->SID && toc->FEC));
    smpl_assert(!(toc->VAD && !toc->coded_as_active_voice));
    smpl_assert(!(toc->FEC && !toc->VAD));

    // Encode if it's a SID frame
    byte += (toc->SID << 7);
    // Encode voice activity
    smpl_assert(!(toc->SID && toc->VAD));
    byte += (toc->VAD << 6);
    // Encode sampling frequency: 16000 / 32000 / 48000
    smpl_assert(toc->fs_Hz == 16000 || toc->fs_Hz == 32000 || toc->fs_Hz == 48000);
    byte += (toc->fs_Hz == 16000) ? 0 : (1<<5);
    // Encode frame length
    switch(toc->packet_len_ms) {
        case 10:
            byte += (0 << 3);
        break;
        case 20:
            byte += (1 << 3);
        break;
        case 60:
            byte += (2 << 3);
        break;
        case 120:
            byte += (3 << 3);
        break;
        default:
            smpl_assert(0);
        break;
    }
    // Encode low rate mode
    smpl_assert(toc->low_rate == SMPL_TRUE || toc->low_rate == SMPL_FALSE);
    byte += (toc->low_rate << 2);
    // Encode if FEC is present or coded as voiced during inactive voiced (where FEC is never used)
    byte += ((toc->FEC == 1) || ((toc->VAD == 0) && (toc->coded_as_active_voice == 1))) ? 1 << 1 : 0;
    // Encode if stereo is used)
    byte += (toc->stereo);

    return byte;
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

static inline void encode_lb_unvoiced(ec_ctx* pECenc, const LbQuantParams* pLbParams, const int16_t nPulses[], int num_subfr) {
    // Code resnrg
    const QuantNrgResData* pQNRD = smpl_get_nrgres_CBks();
    smpl_assert(pQNRD != NULL);

    if (num_subfr == 1) {
        smpl_assert(pLbParams->nrgres_frame_qi >= 0 && pLbParams->nrgres_frame_qi < SMPL_RES_NRG_Q_STEPS_1);
        ec_encode(pECenc, pQNRD->smpl_nrgres_gain_1_cmf[pLbParams->nrgres_frame_qi], pQNRD->smpl_nrgres_gain_1_cmf[pLbParams->nrgres_frame_qi + 1], pQNRD->smpl_nrgres_gain_1_cmf[SMPL_RES_NRG_Q_STEPS_1]);
    }
    else if (num_subfr == 2) {
        smpl_assert(pLbParams->nrgres_frame_qi >= 0 && pLbParams->nrgres_frame_qi < SMPL_RES_NRG_Q_STEPS_2);
        ec_encode(pECenc, pQNRD->smpl_nrgres_gain_2_cmf[pLbParams->nrgres_frame_qi], pQNRD->smpl_nrgres_gain_2_cmf[pLbParams->nrgres_frame_qi + 1], pQNRD->smpl_nrgres_gain_2_cmf[SMPL_RES_NRG_Q_STEPS_2]);
        smpl_assert(pLbParams->nrgres_shape_qi >= 0 && pLbParams->nrgres_shape_qi < SMPL_RES_NRG_SHAPE_CB_N_2);
        ec_encode(pECenc, pQNRD->nrgres_shape_CB_2_cmf[pLbParams->nrgres_shape_qi], pQNRD->nrgres_shape_CB_2_cmf[pLbParams->nrgres_shape_qi + 1], pQNRD->nrgres_shape_CB_2_cmf[SMPL_RES_NRG_SHAPE_CB_N_2]);
    }
    else {
        smpl_assert(num_subfr == 4);
        smpl_assert(pLbParams->nrgres_frame_qi >= 0 && pLbParams->nrgres_frame_qi < SMPL_RES_NRG_Q_STEPS_4);
        ec_encode(pECenc, pQNRD->smpl_nrgres_gain_4_cmf[pLbParams->nrgres_frame_qi], pQNRD->smpl_nrgres_gain_4_cmf[pLbParams->nrgres_frame_qi + 1], pQNRD->smpl_nrgres_gain_4_cmf[SMPL_RES_NRG_Q_STEPS_4]);
        smpl_assert(pLbParams->nrgres_shape_qi >= 0 && pLbParams->nrgres_shape_qi < SMPL_RES_NRG_SHAPE_CB_N_4);
        ec_encode(pECenc, pQNRD->nrgres_shape_CB_4_cmf[pLbParams->nrgres_shape_qi], pQNRD->nrgres_shape_CB_4_cmf[pLbParams->nrgres_shape_qi + 1], pQNRD->nrgres_shape_CB_4_cmf[SMPL_RES_NRG_SHAPE_CB_N_4]);
    }
    // Code fcbgain
    int table_ix = num_subfr_to_idx(num_subfr);
    for (int i = 0; i < num_subfr; i++) {
        if (nPulses[i] > 0) {
            int nrgres_dbq = (pLbParams->nrgres_dbq_Q14[i] + ((int)1 << 13)) >> 14;
            nrgres_dbq = SMPL_min(SMPL_max(nrgres_dbq, SMPL_RES_NRG_MIN_DB), SMPL_RES_NRG_MAX_DB);
            int min_offset = 0 - nrgres_dbq;
            smpl_assert(min_offset >= 0);
            int max_offset = SMPL_UV_GAIN_IDX_LEN - nrgres_dbq;
            int cmfLen = max_offset - min_offset + 2;
            int cmfIx = SMPL_min(nPulses[i] / SMPL_N_PULSES_STEP, SMPL_FCB_G_OFFSET_CMFS - 1);
            const uint16_t* pCMF = &pQNRD->fcbg_offset_cmf[table_ix][cmfIx][min_offset];
            ec_encode(pECenc, pCMF[pLbParams->fcbg_idx[i]] - pCMF[0], pCMF[pLbParams->fcbg_idx[i] +1] - pCMF[0], pCMF[cmfLen-1] - pCMF[0]);
        }
    }
}

static float ec_encode_wrap(void* pECenc, uint16_t _fl, uint16_t _fh, uint16_t _ft)
{
    if (pECenc) {
        ec_encode((ec_enc*)pECenc, _fl, _fh, _ft);
        return 0.0f;
    }
    else {
        float num = (float)(_fh - _fl);
        float den = (float)(_ft);
        return -log2f(num / den);
    }
}

// Encode Lags
float smpl_encode_lags(const PITCH_data* pPitchData, void* pEcCtx, int blocksegs_ix, const int laginds[], int prev_lagblk, int prev_lagidx, int mode)
{
    float n_bits = 0.0f;
    int ix_julia = pPitchData->blocksegs2idx[blocksegs_ix];

    const int blocksize = SMPL_PITCHBLOCK_MS * SMPL_PITCH_FS_KHZ * 2;
    const PITCH_blocksegs* pBlocksegs = &pPitchData->blocksegs[blocksegs_ix];
    if (prev_lagblk < 0) {
        const uint16_t* pCMF = pPitchData->blockseg_idx_CMF;
        n_bits += ec_encode_wrap(pEcCtx, pCMF[ix_julia - 1], pCMF[ix_julia], pCMF[pPitchData->num_blocksegs]);
    }
    else {
        smpl_assert(pPitchData->framelen_ms == 20); // Cant have multiframe packets with 10 ms frames
        const uint16_t* pCMF = pPitchData->block_transition_CMF[prev_lagblk];
        n_bits += ec_encode_wrap(pEcCtx, pCMF[pBlocksegs->blocks[0]], pCMF[pBlocksegs->blocks[0] + 1], pCMF[PITCH_NUM_BLOCKS]);
        int start_ix = smpl_pitch_firstblock_range[pBlocksegs->blocks[0]][0];
        int cmf_len = smpl_pitch_firstblock_range[pBlocksegs->blocks[0]][1] - smpl_pitch_firstblock_range[pBlocksegs->blocks[0]][0] + 1;
        pCMF = &pPitchData->blockseg_idx_CMF[start_ix];
        n_bits += ec_encode_wrap(pEcCtx, pCMF[ix_julia - start_ix - 1] - pCMF[0], pCMF[ix_julia - start_ix] - pCMF[0], pCMF[cmf_len] - pCMF[0]);
    }
    int blk = pBlocksegs->blocks[0];
    int delta_blk = blk - prev_lagblk;
    int start_seg = 0;
    int laginds_ix = 0;
    if (!((prev_lagblk > -1) && (-1 <= delta_blk && delta_blk <= 2))) {
        // encode first lag with uniform CMF
        int idx_mod = laginds[laginds_ix] - blk * blocksize;
        if (pEcCtx) {
            ec_encode((ec_enc*)pEcCtx, idx_mod, idx_mod + 1, blocksize);
        }
        else {
            n_bits += 6.0f;//log2(pitch_struct.blocksize);
        }
        prev_lagblk = blk;
        prev_lagidx = laginds[laginds_ix];
        laginds_ix += pBlocksegs->seglens[0];
        start_seg = 1;
    }
    const uint16_t* delta_lag_CMF = pPitchData->delta_lag_CMFs[mode];
    for (int k = start_seg; k < pBlocksegs->nblocks; k++) {
        blk = pBlocksegs->blocks[k];
        int idx = laginds[laginds_ix];
        laginds_ix += pBlocksegs->seglens[k];
        delta_blk = blk - prev_lagblk;
        int delta_idx = idx - prev_lagidx;
        int prev_lagidx_mod = prev_lagidx - prev_lagblk * blocksize;
        int delta_range_start = -prev_lagidx_mod + delta_blk * blocksize;
        const uint16_t* pCMF = &delta_lag_CMF[delta_range_start + 2 * blocksize - 1];
        int ix = delta_idx - delta_range_start;
        n_bits += ec_encode_wrap(pEcCtx, pCMF[ix] - pCMF[0], pCMF[ix + 1] - pCMF[0], pCMF[blocksize] - pCMF[0]);
        prev_lagblk = blk;
        prev_lagidx = idx;
    }
    return n_bits;
}

static inline void encode_lb_voiced(ParamsEncoder* pPE, ec_ctx* pECenc, const LbQuantParams* pLbParams, const int16_t nPulses[], int num_subfr, int pitch_num_subfr, int low_rate) {
    // Encode Gains
    int32_t mean_acbg_Q14 = 0; // Needs to be fixed point to make sure encoder and decoder in sync
    const int16_t* p_acbg_cbk = low_rate ? smpl_cb_acbgains_lr_Q14 : smpl_cb_acbgains_hr_Q14;
    const CelpTables* pCelp = smpl_get_celp_Tbls();
    smpl_assert(pCelp != NULL);
    for (int sf = 0; sf < num_subfr; sf++) {
        // ACB Gains
        const uint16_t* pCMF = low_rate ? pCelp->acbgains_cmf_lr : pCelp->acbgains_cmf_hr;
        pCMF += (pPE->prev_acb_idx + 1) * (SMPL_ACBG_N + 1);
        ec_encode(pECenc, pCMF[pLbParams->acbg_idx[sf]], pCMF[pLbParams->acbg_idx[sf] + 1], pCMF[SMPL_ACBG_N]);
        pPE->prev_acb_idx = pLbParams->acbg_idx[sf];
        mean_acbg_Q14 += p_acbg_cbk[pPE->prev_acb_idx * SMPL_ACBG_M] + 2 * p_acbg_cbk[pPE->prev_acb_idx * SMPL_ACBG_M + 1];
        if (nPulses[sf] > 0) {
            // FCB Gains
            if (pPE->prev_fcb_idx == -1) {
                pCMF = pCelp->fcbgains_v_cmf;
                ec_encode(pECenc, pCMF[pLbParams->fcbg_idx[sf]], pCMF[pLbParams->fcbg_idx[sf] + 1], pCMF[SMPL_FCBG_V_N]);
            }
            else {
                int delta = pLbParams->fcbg_idx[sf] - pPE->prev_fcb_idx;
                int min_delta = 0 - pPE->prev_fcb_idx;
                int max_delta = (SMPL_FCBG_V_N-1) - pPE->prev_fcb_idx;
                pCMF = &(pCelp->fcbgains_v_delta_cmf[SMPL_FCBG_V_N - 1]);
                ec_encode(pECenc, pCMF[delta] - pCMF[min_delta], pCMF[delta + 1] - pCMF[min_delta], pCMF[max_delta + 1] - pCMF[min_delta]);
            }
            pPE->prev_fcb_idx = pLbParams->fcbg_idx[sf];
        }
    }
    mean_acbg_Q14 /= num_subfr;

    // Encode Lags
    int mode = 2;
    if (mean_acbg_Q14 < smpl_pitch_acbgain_thr_20_Q14[0]) {
        mode = 0;
    }
    else if (mean_acbg_Q14 < smpl_pitch_acbgain_thr_20_Q14[1]) {
        mode = 1;
    }
    const PITCH_data* pPitchData = smpl_get_pitch_data(pitch_num_subfr);
    smpl_encode_lags(pPitchData, pECenc, pLbParams->blocksegs_ix, pLbParams->laginds, pPE->prev_lagblk, pPE->prev_lagidx, mode);
    const PITCH_blocksegs* pBlocksegs = &pPitchData->blocksegs[pLbParams->blocksegs_ix];
    pPE->prev_lagblk = pBlocksegs->blocks[pBlocksegs->nblocks - 1];
    pPE->prev_lagidx = pLbParams->laginds[pitch_num_subfr - 1];
}

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
    int SID)
{
    smpl_assert(pSt != NULL);
    smpl_assert(pEcCtx != NULL);
    ParamsEncoder* pPE = (ParamsEncoder*)pSt;
    ec_ctx* pECenc = (ec_ctx*)pEcCtx;
    const smpl_LSF_CBs* pLsfCb = smpl_get_lsf_CBks();
    smpl_assert(pLsfCb != NULL);

    // Encode voicing
    if (coded_as_active_voice) {
        const uint16_t *cmf = smpl_vuv_cmfs[frame_num == 0 ? 0 : prev_voiced == SMPL_FALSE ? 1 : 2];
        ec_encode(pECenc, cmf[pLbParams->voiced], cmf[pLbParams->voiced+1], cmf[2]);
    }

    if (cond_coding == SMPL_FALSE) {
        pPE->prev_acb_idx = -1;
        pPE->prev_fcb_idx = -1;
        pPE->prev_nrgres_idx = -1;
        pPE->prev_lagblk = -1;
        pPE->prev_lagidx = -1;
    }

    // Encode LSF
    smpl_assert(pLbParams->lsf_idx[0] >= 0 && pLbParams->lsf_idx[0] <= LSF_CB_CENTROIDS);
    const uint16_t* pCMF = NULL;
    int CMFlen = 0;
    if (cond_coding) {
        pCMF = pLbParams->voiced ? smpl_LSF_CMF_cond_v : smpl_LSF_CMF_cond_uv;
        CMFlen = SMPL_ARR_LEN(smpl_LSF_CMF_cond_v);
    }
    else {
        pCMF = pLbParams->voiced ? smpl_LSF_CMF_v : smpl_LSF_CMF_uv;
        CMFlen = SMPL_ARR_LEN(smpl_LSF_CMF_v);
    }
    ec_encode(pECenc, pCMF[pLbParams->lsf_idx[0]], pCMF[pLbParams->lsf_idx[0]+1], pCMF[CMFlen-1]);

    for (int i = 0; i < SMPL_LPC_ORDER; i++) {
        pCMF = pLsfCb->st2[pLbParams->voiced][low_rate][pLbParams->lsf_idx[0]].cmf[i];
        int nLevels = pLsfCb->st2[pLbParams->voiced][low_rate][pLbParams->lsf_idx[0]].numQlvls[i];
        smpl_assert(pLbParams->lsf_idx[i+1] >= 0 && pLbParams->lsf_idx[i+1] < nLevels);
        ec_encode(pECenc, pCMF[pLbParams->lsf_idx[i+1]], pCMF[pLbParams->lsf_idx[i+1] + 1], pCMF[nLevels]);
    }

    // Encode LSF interpolation index
    if (coded_as_active_voice && num_subfr > 1) {
        ec_encode(pECenc, smpl_lsf_interp_cmf[pLbParams->lsf_interpol_idx],
            smpl_lsf_interp_cmf[pLbParams->lsf_interpol_idx+1], smpl_lsf_interp_cmf[SMPL_ARR_LEN(smpl_lsf_interp_cmf)-1]);
    }
     // Encode pulses
    int16_t nPulses[MAX_NB_SUBFR];
    if(SID == SMPL_FALSE){
        smpl_encode_pulses(pECenc, pLbParams->pulses, framelen, num_subfr, low_rate, pLbParams->voiced, coded_as_active_voice, nPulses);
    }
    else {
        memset(nPulses, 0, sizeof(nPulses));
    }
    if (coded_as_active_voice && pLbParams->voiced == SMPL_TRUE) {
        encode_lb_voiced(pPE, pECenc, pLbParams, nPulses, num_subfr, framelen / SMPL_LAG_SUBFRLEN, low_rate);
    }
    else {
        encode_lb_unvoiced(pECenc, pLbParams, nPulses, num_subfr);
    }
}

void smpl_encode_hb_params(void* pSt, void* pEcCtx, void* pHe, const HbQuantParams* pHbParams, int frame_length_16, int voiced, int cond_coding, int low_rate)
{
    smpl_assert(pSt != NULL);
    smpl_assert(pEcCtx != NULL);
    const HB_LSF_CBs* pLsfCb = smpl_get_hb_lsf_CBks();
    const HB_GAIN_CBs* pGainCb = smpl_get_hb_gain_CBks();
    smpl_assert(pLsfCb != NULL);
    smpl_assert(pGainCb != NULL);

    ec_ctx* pECenc = (ec_ctx*)pEcCtx;
    HbEncoder* pHE = (HbEncoder*)pHe;

    // Encode gains
    const uint16_t* pCMF = pGainCb->hb_gain_vq_cmfs[frame_length_16 == 320][voiced][low_rate];
    int PMFLen = hb_gain_vq_sizes[frame_length_16 == 320][voiced][low_rate];
    ec_encode(pECenc, pCMF[pHbParams->gain_qi], pCMF[pHbParams->gain_qi + 1], pCMF[PMFLen]);

    // Encode LSFs
    PMFLen = hb_lpc_vq_sizes[voiced][low_rate];
    pCMF = (cond_coding && low_rate) ? pLsfCb->hb_lpc_vq_cmfs_cond[voiced] + (hb_lpc_vq_sel_cond[voiced][pHE->prev_hb_lpc_ix] * (PMFLen + 1)) : pLsfCb->hb_lpc_vq_cmfs[voiced][low_rate];
    ec_encode(pECenc, pCMF[pHbParams->lsf_idx], pCMF[pHbParams->lsf_idx + 1], pCMF[PMFLen]);
    pHE->prev_hb_lpc_ix = pHbParams->lsf_idx;
}

void* smpl_create_param_decoder(void)
{
    ParamsDecoder* pPD = (ParamsDecoder*)calloc(1, sizeof(ParamsDecoder));
    if (!pPD) {
        smpl_assert(0);
        return NULL;
    }
    pPD->prev_acb_idx = -1;
    pPD->prev_fcb_idx = -1;
    pPD->prev_lagblk = -1;
    pPD->prev_lagidx = -1;
    pPD->prev_nrgres_idx = -1;
    return (void*)pPD;
}

void smpl_decode_toc(unsigned char toc_byte, smpl_TOC *toc)
{
    memset(toc, 0, sizeof(smpl_TOC));

    // Decede SID
    toc->SID = (toc_byte >> 7) & 1;
    // Encode voice activity
    toc->VAD = (toc_byte >> 6) & 1;
    smpl_assert(!(toc->SID && toc->coded_as_active_voice));
    // Encode sampling frequency: 16000 / 32000
    toc->fs_Hz = (((toc_byte >> 5) & 1) + 1) * 16000;
    // Encode frame length
    int ix = (toc_byte >> 3) & 3;
    switch (ix) {
    case 0:
        toc->packet_len_ms = 10;
        break;
    case 1:
        toc->packet_len_ms = 20;
        break;
    case 2:
        toc->packet_len_ms = 60;
        break;
    case 3:
        toc->packet_len_ms = 120;
        break;
    default:
        smpl_assert(0); // Cant really happen
        break;
    }
    // Decode low rate mode
    toc->low_rate = (toc_byte >> 2) & 1;
    // Decode FEC and coded_as_active_voice
    toc->FEC = toc->VAD && ((toc_byte >> 1) & 1);
    toc->coded_as_active_voice = (toc->VAD || ((toc_byte >> 1) & 1));
    // Decode Stereo
    toc->stereo = (toc_byte) & 1;
    return;
}

static inline void decode_lb_voiced(ParamsDecoder* pPD, ec_ctx* pECdec, const int16_t nPulses[], int num_subfr,
    int pitch_num_subfr, int low_rate, LbQuantParams* pLbParams)
{
    int32_t mean_acbg_Q14 = 0; // Needs to be fixed point to make sure encoder and decoder in sync
    const int16_t* p_acbg_cbk = low_rate ? smpl_cb_acbgains_lr_Q14 : smpl_cb_acbgains_hr_Q14;
    const CelpTables* pCelp = smpl_get_celp_Tbls();
    smpl_assert(pCelp != NULL);
    for (int sf = 0; sf < num_subfr; sf++) {
        // ACB Gains
        const uint16_t *pCMF = low_rate ? pCelp->acbgains_cmf_lr : pCelp->acbgains_cmf_hr;
        pCMF += (pPD->prev_acb_idx + 1) * (SMPL_ACBG_N + 1);
        pLbParams->acbg_idx[sf] = smpl_ec_decode_update(pECdec, pCMF, SMPL_ACBG_N + 1);
        pPD->prev_acb_idx = pLbParams->acbg_idx[sf];
        mean_acbg_Q14 += p_acbg_cbk[pPD->prev_acb_idx * SMPL_ACBG_M] + 2 * p_acbg_cbk[pPD->prev_acb_idx * SMPL_ACBG_M + 1];
        if (nPulses[sf] > 0) {
            // FCB Gains
            if (pPD->prev_fcb_idx == -1) {
                pLbParams->fcbg_idx[sf] = smpl_ec_decode_update(pECdec, pCelp->fcbgains_v_cmf, SMPL_FCBG_V_N + 1);
            }
            else {
                int min_delta = 0 - pPD->prev_fcb_idx;
                int max_delta = (SMPL_FCBG_V_N - 1) - pPD->prev_fcb_idx;
                pCMF = &(pCelp->fcbgains_v_delta_cmf[SMPL_FCBG_V_N - 1]) + min_delta;
                int delta = smpl_ec_decode_update(pECdec, pCMF, max_delta - min_delta + 2) + min_delta;
                pLbParams->fcbg_idx[sf] = pPD->prev_fcb_idx + delta;
            }
            pPD->prev_fcb_idx = pLbParams->fcbg_idx[sf];
        }
    }

    mean_acbg_Q14 /= num_subfr;

    // Decode Lags
    const int blocksize = SMPL_PITCHBLOCK_MS * SMPL_PITCH_FS_KHZ * 2;
    const PITCH_data* pPitchData = smpl_get_pitch_data(pitch_num_subfr);
    int ix_julia = 0;
    if (pPD->prev_lagblk < 0) {
        ix_julia = smpl_ec_decode_update(pECdec, pPitchData->blockseg_idx_CMF, pPitchData->num_blocksegs+1) + 1;
    }
    else {
        smpl_assert(pPitchData->framelen_ms == 20);
        int block0 = smpl_ec_decode_update(pECdec, pPitchData->block_transition_CMF[pPD->prev_lagblk], PITCH_NUM_BLOCKS + 1);
        int start_ix = smpl_pitch_firstblock_range[block0][0];
        int cmf_len = smpl_pitch_firstblock_range[block0][1] - smpl_pitch_firstblock_range[block0][0] + 2;
        ix_julia = smpl_ec_decode_update(pECdec, &pPitchData->blockseg_idx_CMF[start_ix], cmf_len) + start_ix + 1;
    }
    pLbParams->blocksegs_ix = -1;
    for (int i = 0; i < (int)(SMPL_ARR_LEN(smpl_pitch_blocksegs2idx)); i++) { // To Do Make table that can do this conversion
        if (pPitchData->blocksegs2idx[i] == ix_julia) {
            pLbParams->blocksegs_ix = i;
            break;
        }
    }
    smpl_assert(pLbParams->blocksegs_ix >= 0 && pLbParams->blocksegs_ix < pPitchData->num_blocksegs);
    const PITCH_blocksegs* pBlocksegs = &pPitchData->blocksegs[pLbParams->blocksegs_ix];
    int blk = pBlocksegs->blocks[0];
    int delta_blk = blk - pPD->prev_lagblk;
    int start_seg = 0;
    int laginds_ix = 0;
    if (!((pPD->prev_lagblk > -1) && (-1 <= delta_blk && delta_blk <= 2))) {
        // decode first lag with uniform CMF
        int lagind = smpl_ec_decode_uniform(pECdec, SMPL_PITCHBLOCK_MS * SMPL_PITCH_FS_KHZ * 2) + blk * blocksize;
        for (int j = 0; j < pBlocksegs->seglens[0]; j++) {
            pLbParams->laginds[laginds_ix++] = lagind;
        }
        pLbParams->laginds[laginds_ix] = lagind;
        pPD->prev_lagblk = blk;
        pPD->prev_lagidx = lagind;
        start_seg = 1;
    }
    // decode remaining lags
    int mode = 2;
    if (mean_acbg_Q14 < smpl_pitch_acbgain_thr_20_Q14[0]) {
        mode = 0;
    }
    else if (mean_acbg_Q14 < smpl_pitch_acbgain_thr_20_Q14[1]) {
        mode = 1;
    }
    const uint16_t* delta_lag_CMF = pPitchData->delta_lag_CMFs[mode];
    for (int k = start_seg; k < pBlocksegs->nblocks; k++) {
        blk = pBlocksegs->blocks[k];
        delta_blk = blk - pPD->prev_lagblk;
        int prev_lagidx_mod = pPD->prev_lagidx - pPD->prev_lagblk * blocksize;
        int delta_range_start = -prev_lagidx_mod + delta_blk * blocksize;
        const uint16_t *pCMF = &delta_lag_CMF[delta_range_start + 2 * blocksize - 1];
        int idx = smpl_ec_decode_update(pECdec, pCMF, blocksize + 1);
        int lagind = idx + delta_range_start + pPD->prev_lagidx;
        for (int j = 0; j < pBlocksegs->seglens[k]; j++) {
            pLbParams->laginds[laginds_ix++] = lagind;
        }
        pPD->prev_lagblk = blk;
        pPD->prev_lagidx = lagind;
    }
}

static inline void decode_lb_unvoiced(ec_ctx* pECdec, const int16_t nPulses[], int num_subfr, LbQuantParams* pLbParams) {
    // decode resnrg
    const QuantNrgResData* pQNRD = smpl_get_nrgres_CBks();
    smpl_assert(pQNRD != NULL);
    if (num_subfr == 1) {
        pLbParams->nrgres_frame_qi = smpl_ec_decode_update(pECdec, pQNRD->smpl_nrgres_gain_1_cmf, SMPL_RES_NRG_Q_STEPS_1 + 1);
    }
    else if (num_subfr == 2) {
        pLbParams->nrgres_frame_qi = smpl_ec_decode_update(pECdec, pQNRD->smpl_nrgres_gain_2_cmf, SMPL_RES_NRG_Q_STEPS_2 + 1);
        pLbParams->nrgres_shape_qi = smpl_ec_decode_update(pECdec, pQNRD->nrgres_shape_CB_2_cmf, SMPL_RES_NRG_SHAPE_CB_N_2 + 1);
    }
    else {
        smpl_assert(num_subfr == 4);
        pLbParams->nrgres_frame_qi = smpl_ec_decode_update(pECdec, pQNRD->smpl_nrgres_gain_4_cmf, SMPL_RES_NRG_Q_STEPS_4 + 1);
        pLbParams->nrgres_shape_qi = smpl_ec_decode_update(pECdec, pQNRD->nrgres_shape_CB_4_cmf, SMPL_RES_NRG_SHAPE_CB_N_4 + 1);
    }

    int table_ix = num_subfr_to_idx(num_subfr);
    int32_t nrgres_frame_dbq_Q14 = ((int32_t)pLbParams->nrgres_frame_qi * (int32_t)smpl_nrg_step_db_Q14[table_ix]);
    nrgres_frame_dbq_Q14 += (((int32_t)SMPL_RES_NRG_MIN_DB) * (1 << 14));
    if (num_subfr == 1) {
        pLbParams->nrgres_dbq_Q14[0] = nrgres_frame_dbq_Q14;
    } else {
        const int16_t* cbPtr = num_subfr == 4 ? nrgres_shape_CB_4_Q10 : nrgres_shape_CB_2_Q10;
        for (int i = 0; i < num_subfr; i++) {
            pLbParams->nrgres_dbq_Q14[i] = nrgres_frame_dbq_Q14 + (((int32_t)cbPtr[pLbParams->nrgres_shape_qi * num_subfr + i]) * 16);
        }
    }
    // decode fcbgain
    for (int i = 0; i < num_subfr; i++) {
        if (nPulses[i] > 0) {
            int nrgres_dbq = (pLbParams->nrgres_dbq_Q14[i] + ((int)1 << 13)) >> 14;
            nrgres_dbq = SMPL_min(SMPL_max(nrgres_dbq, SMPL_RES_NRG_MIN_DB), SMPL_RES_NRG_MAX_DB);
            int min_offset = 0 - nrgres_dbq;
            smpl_assert(min_offset >= 0);
            int max_offset = SMPL_UV_GAIN_IDX_LEN - nrgres_dbq;
            int cmfLen = max_offset - min_offset + 2;
            int cmfIx = SMPL_min(nPulses[i] / SMPL_N_PULSES_STEP, SMPL_FCB_G_OFFSET_CMFS - 1);
            const uint16_t* pCMF = &pQNRD->fcbg_offset_cmf[table_ix][cmfIx][min_offset];
            pLbParams->fcbg_idx[i] = smpl_ec_decode_update(pECdec, pCMF, cmfLen);
        }
    }
}

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
    LbQuantParams* pLbParams)
{
    smpl_assert(pSt != NULL);
    smpl_assert(pEcCtx != NULL);
    ParamsDecoder* pPD = (ParamsDecoder*)pSt;
    ec_ctx* pECdec = (ec_ctx*)pEcCtx;
    const smpl_LSF_CBs* pLsfCb = smpl_get_lsf_CBks();
    smpl_assert(pLsfCb != NULL);

    // Decode voicing
    if (coded_as_active_voice) {
        const uint16_t *cmf = smpl_vuv_cmfs[frame_num == 0 ? 0 : pPD->prev_voiced == SMPL_FALSE ? 1 : 2];
        pLbParams->voiced = smpl_ec_decode_update(pECdec, cmf, 3);
    }
    else {
        pLbParams->voiced = SMPL_FALSE;
    }

    *cond_coding &= (pLbParams->voiced == pPD->prev_voiced);
    if (*cond_coding == SMPL_FALSE){
        pPD->prev_acb_idx = -1;
        pPD->prev_fcb_idx = -1;
        pPD->prev_nrgres_idx = -1;
        pPD->prev_lagblk = -1;
        pPD->prev_lagidx = -1;
    }
    pPD->prev_voiced = pLbParams->voiced;

    // Decode LSF
    const uint16_t* pCMF = NULL;
    int CMFlen = 0;
    if (*cond_coding) {
        pCMF = pLbParams->voiced ? smpl_LSF_CMF_cond_v : smpl_LSF_CMF_cond_uv;
        CMFlen = SMPL_ARR_LEN(smpl_LSF_CMF_cond_v);
    }
    else {
        pCMF = pLbParams->voiced ? smpl_LSF_CMF_v : smpl_LSF_CMF_uv;
        CMFlen = SMPL_ARR_LEN(smpl_LSF_CMF_v);
    }
    pLbParams->lsf_idx[0] = smpl_ec_decode_update(pECdec, pCMF, CMFlen);
    for (int i = 0; i < SMPL_LPC_ORDER; i++) {
        pCMF = pLsfCb->st2[pLbParams->voiced][low_rate][pLbParams->lsf_idx[0]].cmf[i];
        CMFlen = pLsfCb->st2[pLbParams->voiced][low_rate][pLbParams->lsf_idx[0]].numQlvls[i] + 1;
        pLbParams->lsf_idx[i+1] = smpl_ec_decode_update(pECdec, pCMF, CMFlen);
    }
    // Decode LSF interpolation index
    if (coded_as_active_voice && num_subfr > 1) {
        pLbParams->lsf_interpol_idx = smpl_ec_decode_update(pECdec, smpl_lsf_interp_cmf, SMPL_ARR_LEN(smpl_lsf_interp_cmf));
    }

    // Decode pulses
    TIC(pulses)
    if(SID == SMPL_FALSE){
        smpl_decode_pulses(pECdec, framelen, num_subfr, low_rate, pLbParams->voiced, coded_as_active_voice, pLbParams->positions,
            pLbParams->pos_pulses, &pLbParams->nPositions, &pLbParams->n_pulses, pLbParams->sf_pulses);
    }
    else {
        pLbParams->n_pulses = 0;
        memset(pLbParams->sf_pulses, 0, sizeof(pLbParams->sf_pulses));
    }
    TOC(pulses)

    // Decode Gains
    if (coded_as_active_voice && pLbParams->voiced == SMPL_TRUE) {
        decode_lb_voiced(pPD, pECdec, pLbParams->sf_pulses, num_subfr, framelen / SMPL_LAG_SUBFRLEN, low_rate, pLbParams);
    }
    else {
        decode_lb_unvoiced(pECdec, pLbParams->sf_pulses, num_subfr, pLbParams);
    }
    return pECdec->error;
}

int smpl_check_end_result(void* pEcCtx)
{
    ec_ctx* pECdec = (ec_ctx*)pEcCtx;
    uint32_t nBytes = (size_t)(ec_tell(pEcCtx) + 7) / 8;
    if (pECdec->storage > nBytes) {
        return -1;
    }
    if ((pECdec->storage + 2) < nBytes) {
        return -1;
    }
    return pECdec->error;
}

int smpl_decode_hb_params(void* pSt, void* pEcCtx, int frame_length_16, int voiced, int cond_coding, int low_rate, HbQuantParams* pHbParams)
{
    smpl_assert(pSt != NULL);
    smpl_assert(pEcCtx != NULL);
    const HB_LSF_CBs* pLsfCb = smpl_get_hb_lsf_CBks();
    const HB_GAIN_CBs* pGainCb = smpl_get_hb_gain_CBks();
    smpl_assert(pLsfCb != NULL);
    smpl_assert(pGainCb != NULL);

    ParamsDecoder* pPD = (ParamsDecoder*)pSt;
    ec_ctx* pECdec = (ec_ctx*)pEcCtx;

    // Decode gains
    const uint16_t* pCMF = pGainCb->hb_gain_vq_cmfs[frame_length_16 == 320][voiced][low_rate];
    int CMFLen = hb_gain_vq_sizes[frame_length_16 == 320][voiced][low_rate] + 1;
    pHbParams->gain_qi = smpl_ec_decode_update(pECdec, pCMF, CMFLen);

    // Decode LSFs
    CMFLen = hb_lpc_vq_sizes[voiced][low_rate] + 1;
    pCMF = (cond_coding && low_rate) ? pLsfCb->hb_lpc_vq_cmfs_cond[voiced] + (hb_lpc_vq_sel_cond[voiced][pPD->prev_hb_lpc_ix] * CMFLen) : pLsfCb->hb_lpc_vq_cmfs[voiced][low_rate];
    pHbParams->lsf_idx = smpl_ec_decode_update(pECdec, pCMF, CMFLen);
    pPD->prev_hb_lpc_ix = pHbParams->lsf_idx;

    return pECdec->error;
}

void smpl_free_param_decoder(void* pSt)
{
    if (!pSt) {
        return;
    }
    ParamsDecoder* pPD = (ParamsDecoder*)pSt;
    free(pPD);
}
