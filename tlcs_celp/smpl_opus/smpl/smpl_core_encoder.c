#include "smpl_core_encoder.h"
#include "smpl_defines.h"
#include "smpl_codec_util.h"
#include "smpl_tables.h"
#include "smpl_typedef.h"
#include "smpl_filt_allpass_fb.h"
#include "smpl_filt.h"
#include "smpl_calc_hp_coefs.h"
#include "smpl_lpc.h"
#include "smpl_helpers.h"
#include "smpl_perc_wght.h"
#include "smpl_pitch.h"
#include "smpl_get_signal_mode.h"
#include "smpl_lsf_quant.h"
#include "smpl_bitrate_controller.h"
#include "smpl_celp.h"
#include "smpl_param_coding.h"
#include "smpl_quant_nrg_res.h"
#include "smpl_bandwidth_extension.h"
#include <math.h>
#include "smpl_flpexcpt_check.h"
#include "silk/debug.h"
#include "silk/SigProc_FIX.h"

static inline void update_hp_coefs(smpl_core_encoder* enc_state, int hp_fcorner_3dB_Hz)
{
    smpl_assert(hp_fcorner_3dB_Hz <= 500);
    smpl_assert(hp_fcorner_3dB_Hz >= 0);
    if (hp_fcorner_3dB_Hz > 0 && enc_state->hp_fcorner_3dB_Hz != hp_fcorner_3dB_Hz) {
        smpl_get_hp_coefs((float)hp_fcorner_3dB_Hz, enc_state->hp_b2, enc_state->hp_a2);
    }
    enc_state->hp_fcorner_3dB_Hz = hp_fcorner_3dB_Hz;
}

void smpl_core_encoder_init(smpl_core_encoder* enc_state, PitchEstScratch *pitchScratch, CelpScratch *celpScratch) {
    memset(enc_state, 0, sizeof(smpl_core_encoder));
    smpl_init_pitch_estimator((void*)&enc_state->pitch_state, pitchScratch);
    bitrate_controller_init(&enc_state->rateCtrl[0]);
    bitrate_controller_init(&enc_state->rateCtrl[1]);
    smpl_init_celp_encoder((void*)&enc_state->celp_state[0], celpScratch);
    smpl_init_celp_encoder((void*)&enc_state->celp_state[1], celpScratch);
    smpl_init_param_encoder((void*)&enc_state->parm_encoder_state);
    smpl_init_hb_encoder((void*)&enc_state->hb_state);
    memset(&enc_state->noise_generator, 0, sizeof(NoiseGenerator));
    update_hp_coefs(enc_state, SMPL_ENC_HP_FCORNER_3DB_HZ);
}

static inline uint32_t add_pending_fec(
    smpl_core_encoder* enc_state,
    smpl_EncControlStruct* enc_status,
    ec_enc* psRangeEnc,
    smpl_vad_status* vad,
    int lowRate,
    int packet_ms)
{
    enc_status->LBRR_coded = SMPL_FALSE;
    uint32_t fec_bytes = 0;
    if (enc_state->hasFecData && (vad->VAD) &&
        enc_state->prevLowRate == lowRate &&
        enc_state->prevInternalSampleRate == enc_status->internalSampleRate &&
        enc_state->prev_packet_ms == packet_ms) {

        const int frame_ms = packet_ms == 10 ? 10 : 20;
        const int framelen = frame_ms * SMPL_CELP_FS_KHZ;
        const int frames_per_packet = packet_ms / frame_ms;
        const int subfrlen = (lowRate == 1) ? 10 * SMPL_CELP_FS_KHZ : 5 * SMPL_CELP_FS_KHZ;
        const int numsubfrs = framelen / subfrlen;

        int prev_voiced = SMPL_FALSE;
        for (int numframe = 0; numframe < frames_per_packet; numframe++) {
            int cond_coding = (enc_state->lbrr_lb_quant_params[numframe].voiced == prev_voiced) && (numframe > 0);
            smpl_encode_lb_params((void*)&enc_state->parm_encoder_state, psRangeEnc, &enc_state->lbrr_lb_quant_params[numframe],
                framelen, numsubfrs, SMPL_TRUE, cond_coding, lowRate, numframe, prev_voiced, SMPL_FALSE);
            if (enc_status->internalSampleRate > 16000) {
                smpl_encode_hb_params((void*)&enc_state->parm_encoder_state, psRangeEnc, (void*)&enc_state->hb_state, &enc_state->lbrr_hb_quant_params[numframe], framelen, enc_state->lbrr_lb_quant_params[numframe].voiced, cond_coding, lowRate);
            }
            prev_voiced = enc_state->lbrr_lb_quant_params[numframe].voiced;
        }
        enc_status->LBRR_coded = SMPL_TRUE;
        fec_bytes = (uint32_t)(ec_tell(psRangeEnc) + 7) / 8;
    }
    return fec_bytes;
}

void smpl_band_split(const float* x, int x_len, float* x_low, float* x_high, float* filterbank_buf) {
    // iterate in reverse, so we can run filterbank forward in reverse time
    TIC(fb_ana)
    int num_chunks = x_len / SMPL_FRAME_LEN;
    const float* chunk_start = x + x_len;
    float chunk[SMPL_FRAME_LEN];
    float fb_ana_state[(SMPL_FILTERBANK_A_LEN-1)*4];
    memset(fb_ana_state, 0, (SMPL_FILTERBANK_A_LEN-1)*4 * sizeof(float));
    float* low  = x_low;
    float* high = x_high;
    for (int i = 0; i < num_chunks; i++) {
        chunk_start -= SMPL_FRAME_LEN;
        smpl_reverse_into(chunk_start, chunk, SMPL_FRAME_LEN);
        smpl_filt_allpass_fb_ana(chunk, SMPL_FRAME_LEN, smpl_filterbankL_coef, SMPL_FILTERBANK_A_LEN,
                                 smpl_filterbankH_coef, SMPL_FILTERBANK_A_LEN, low, high, fb_ana_state, (SMPL_FILTERBANK_A_LEN-1)*4);
        low  += SMPL_FRAME_LEN / 2;
        high += SMPL_FRAME_LEN / 2;
    }
    // last lookahead samples
    smpl_reverse_into(filterbank_buf, chunk, 2*SMPL_WINNEXT_WB_LEN);
    float x_high_extra[SMPL_WINNEXT_WB_LEN];
    smpl_filt_allpass_fb_ana(chunk, 2 * SMPL_WINNEXT_WB_LEN, smpl_filterbankL_coef, SMPL_FILTERBANK_A_LEN,
                             smpl_filterbankH_coef, SMPL_FILTERBANK_A_LEN, low, x_high_extra, fb_ana_state, (SMPL_FILTERBANK_A_LEN-1)*4);

    memmove(filterbank_buf, x + (x_len - 2*SMPL_WINNEXT_WB_LEN), 2*SMPL_WINNEXT_WB_LEN * sizeof(float));
    smpl_reverse(x_low,  x_len / 2 + SMPL_WINNEXT_WB_LEN);
    smpl_reverse(x_high + SMPL_WINNEXT_WB_LEN, x_len / 2 - SMPL_WINNEXT_WB_LEN);
    smpl_reverse_into(x_high_extra, x_high, SMPL_WINNEXT_WB_LEN);
    TOC(fb_ana)
}

typedef struct ComplexitySetting {
    int lsf_surv;
    int lsf_interpol_search;
    int celp_ignore_zir;
    int pitch_numstates1;
    int fcb_tot_surv_20ms_max[2];
    int perc_resp_len;
    int pitch_perc_resp_len;
    void (*pitch_perc_filt_ma)(const float *x, int N, const float *coef, const int coef_len, float *y);
} ComplexitySetting;

static inline void update_complexity_setting(const smpl_EncControlStruct* enc_status, ComplexitySetting* setting)
{
    if (enc_status->complexity <= 1) {
        setting->lsf_surv = 2;
        setting->lsf_interpol_search = SMPL_FALSE;
        setting->celp_ignore_zir = SMPL_TRUE;
        setting->pitch_numstates1 = 4;
        setting->fcb_tot_surv_20ms_max[0] = 30;
        setting->fcb_tot_surv_20ms_max[1] = 30;
        setting->perc_resp_len = 10;
        setting->pitch_perc_resp_len = 5;
        setting->pitch_perc_filt_ma = smpl_filt_ma;
    }
    else if (enc_status->complexity <= 2) {
        setting->lsf_surv = 6;
        setting->lsf_interpol_search = SMPL_FALSE;
        setting->celp_ignore_zir = SMPL_TRUE;
        setting->pitch_numstates1 = 4;
        setting->fcb_tot_surv_20ms_max[0] = 30;
        setting->fcb_tot_surv_20ms_max[1] = 30;
        setting->perc_resp_len = 10;
        setting->pitch_perc_resp_len = 5;
        setting->pitch_perc_filt_ma = smpl_filt_ma;
    }
    else if (enc_status->complexity <= 3) {
        setting->lsf_surv = 3;
        setting->lsf_interpol_search = SMPL_FALSE;
        setting->celp_ignore_zir = SMPL_FALSE;
        setting->pitch_numstates1 = 10;
        setting->fcb_tot_surv_20ms_max[0] = 60;
        setting->fcb_tot_surv_20ms_max[1] = 60;
        setting->perc_resp_len = 16;
        setting->pitch_perc_resp_len = 10;
        setting->pitch_perc_filt_ma = smpl_filt_ma;
    }
    else if (enc_status->complexity <= 4) {
        setting->lsf_surv = 4;
        setting->lsf_interpol_search = SMPL_TRUE;
        setting->celp_ignore_zir = SMPL_FALSE;
        setting->pitch_numstates1 = 16;
        setting->fcb_tot_surv_20ms_max[0] = 80;
        setting->fcb_tot_surv_20ms_max[1] = 80;
        setting->perc_resp_len = 24;
        setting->pitch_perc_resp_len = 12;
        setting->pitch_perc_filt_ma = smpl_filt_ma;
    }
    else if (enc_status->complexity <= 8) {
        setting->lsf_surv = 6;
        setting->lsf_interpol_search = SMPL_TRUE;
        setting->celp_ignore_zir = SMPL_FALSE;
        setting->pitch_numstates1 = 24;
        setting->fcb_tot_surv_20ms_max[0] = smpl_fcb_tot_surv_20ms_max[0];
        setting->fcb_tot_surv_20ms_max[1] = smpl_fcb_tot_surv_20ms_max[1];
        setting->perc_resp_len = SMPL_PERC_RESP_LEN;
        setting->pitch_perc_resp_len = 17;
        setting->pitch_perc_filt_ma = smpl_filt_ma16_monic;  // must match len on previous line
    } else {  // highest complexity
        setting->lsf_surv = 8;
        setting->lsf_interpol_search = SMPL_TRUE;
        setting->celp_ignore_zir = SMPL_FALSE;
        setting->pitch_numstates1 = 30;
        setting->fcb_tot_surv_20ms_max[0] = 130;
        setting->fcb_tot_surv_20ms_max[1] = 130;
        setting->perc_resp_len = SMPL_PERC_RESP_LEN;
        setting->pitch_perc_resp_len = 17;
        setting->pitch_perc_filt_ma = smpl_filt_ma16_monic;  // must match len on previous line
    }
    smpl_assert(setting->perc_resp_len <= SMPL_MAX_L_RESP);
    smpl_assert(setting->pitch_perc_resp_len <= SMPL_LPC_ORDER + 1);
    smpl_assert(setting->perc_resp_len >= setting->pitch_perc_resp_len);
    smpl_assert(setting->pitch_numstates1 > 0);
    smpl_assert(setting->lsf_surv > 0);
}

static void smpl_base_encode(
    smpl_core_encoder* enc_state,
    smpl_EncControlStruct* enc_status,
    const smpl_vad_status* vad,
    const smpl_dtx_status* dtx,
    const ComplexitySetting* cmplx_setting,
    LbQuantParams* lb_quant_params,
    HbQuantParams* hb_quant_params,
    const smpl_core_encoder_settings* enc_settings,
    int numframe,
    int cond_coding,
    int state_ix
)
{
    // Quantize LSF
    TIC(lsf_quant)
        //    int cond_coding = (enc_state->voiced_buf[numframe] == prev_voiced) && (numframe > 0);
        int surv = cmplx_setting->lsf_surv;;
    float RDw_adj = sqrtf(enc_settings->lowRate ? enc_status->mainBitRate / 5000.0f : enc_status->mainBitRate / 14000.0f);
    float qlsf[SMPL_LPC_ORDER], wlsf[SMPL_LPC_ORDER], bits, RDbest;
    float* A = enc_state->A_buf[numframe];
    if (cond_coding) {
        smpl_lsf_quant_cond(surv, A, enc_state->prev_lsf[state_ix], RDw_adj, enc_state->voiced_buf[numframe], enc_settings->lowRate, qlsf, lb_quant_params->lsf_idx, &bits, &RDbest, wlsf);
    }
    else {
        smpl_lsf_quant(surv, A, RDw_adj, enc_state->voiced_buf[numframe], enc_settings->lowRate, qlsf, lb_quant_params->lsf_idx, &bits, &RDbest, wlsf);
    }
    TOC(lsf_quant)

        int xhp_packet_extra = enc_status->internalSampleRate > 16000 ? SMPL_WINNEXT_WB_LEN : 0;
    float* xhp_frame = enc_state->xhp_packet_buf + SMPL_LPC_BUF_MEM_LEN + enc_settings->framelen * numframe + xhp_packet_extra;

    // Interpolate LSFs
    int lsf_interpol_idx = 0;
    float lsfs[SMPL_MAX_N_SUBFR][SMPL_LPC_ORDER];
    float predcoefs[SMPL_MAX_N_SUBFR][SMPL_LPC_ORDER + 1];
    float prev_lsf2[SMPL_LPC_ORDER];         // Used in alternative interpolation
    memcpy(prev_lsf2, enc_state->prev_lsf[state_ix], SMPL_LPC_ORDER * sizeof(float));
    const float* p_lsf_interpol = enc_settings->numsubfrs == 4 ? &smpl_lsf_interpol_4[lsf_interpol_idx][0] :
        (enc_settings->numsubfrs == 2 ? &smpl_lsf_interpol_2[lsf_interpol_idx][0] : &smpl_lsf_interpol_1);
    smpl_lpc_interpol(qlsf, enc_state->prev_lsf[state_ix], p_lsf_interpol, SMPL_LPC_ORDER, enc_settings->numsubfrs, &predcoefs[0][0], &lsfs[0][0]);
    float reslpc[SMPL_MAX_WB_FRAME_SIZE_IN_SAMPLES];
    for (int i = 0; i < enc_settings->numsubfrs; i++)
    {
        smpl_filt_ma16_monic(xhp_frame + i * enc_settings->subfrlen, enc_settings->subfrlen, predcoefs[i], SMPL_LPC_ORDER + 1, reslpc + i * enc_settings->subfrlen);
    }

    // Try alternative interpolation
    if (cmplx_setting->lsf_interpol_search && vad->coded_as_active_voice && (enc_settings->numsubfrs > 1)) {
        float nrgs1[SMPL_MAX_N_SUBFR];
        float nrgs2[SMPL_MAX_N_SUBFR];
        float lsfs2[SMPL_MAX_N_SUBFR][SMPL_LPC_ORDER];
        float predcoefs2[SMPL_MAX_N_SUBFR][SMPL_LPC_ORDER + 1];
        float reslpc2[SMPL_MAX_WB_FRAME_SIZE_IN_SAMPLES];
        for (int i = 0; i < enc_settings->numsubfrs; i++)
        {
            nrgs1[i] = sqrtf(smpl_nrg(reslpc + i * enc_settings->subfrlen, enc_settings->subfrlen) + 1e-30f);
        }

        lsf_interpol_idx = 1;
        smpl_assert(enc_settings->numsubfrs > 1);
        p_lsf_interpol = enc_settings->numsubfrs == 4 ? &smpl_lsf_interpol_4[lsf_interpol_idx][0] : &smpl_lsf_interpol_2[lsf_interpol_idx][0];
        smpl_lpc_interpol(qlsf, prev_lsf2, p_lsf_interpol, SMPL_LPC_ORDER, enc_settings->numsubfrs, &predcoefs2[0][0], &lsfs2[0][0]);

        for (int i = 0; i < enc_settings->numsubfrs; i++) {
            smpl_filt_ma16_monic(xhp_frame + i * enc_settings->subfrlen, enc_settings->subfrlen, predcoefs2[i], SMPL_LPC_ORDER + 1, reslpc2 + i * enc_settings->subfrlen);
        }
        for (int i = 0; i < enc_settings->numsubfrs; i++) {
            nrgs2[i] = sqrtf(smpl_nrg(reslpc2 + i * enc_settings->subfrlen, enc_settings->subfrlen) + 1e-30f);
        }

        if (smpl_sum_vec(nrgs2, enc_settings->numsubfrs) < smpl_sum_vec(nrgs1, enc_settings->numsubfrs) * 0.998f) {
            memcpy(enc_state->prev_lsf[state_ix], prev_lsf2, SMPL_LPC_ORDER * sizeof(float));
            memcpy(lsfs, lsfs2, enc_settings->numsubfrs * SMPL_LPC_ORDER * sizeof(float));
            memcpy(predcoefs, predcoefs2, enc_settings->numsubfrs * (SMPL_LPC_ORDER + 1) * sizeof(float));
            memcpy(reslpc, reslpc2, enc_settings->framelen * sizeof(float));
        }
        else {
            lsf_interpol_idx = 0;
        }
    }

    const int perc_resp_len = cmplx_setting->perc_resp_len;
    float perc_wght_resp[SMPL_MAX_N_SUBFR][SMPL_MAX_L_RESP];
    if (enc_state->voiced_buf[numframe] == 1) {
        for (int i = 0; i < enc_settings->numsubfrs; i++) {
            smpl_perc_ac2a(enc_state->perc_corrs_buf[numframe][i], perc_resp_len + SMPL_PERC_EMPH_V_LEN - 1,
                smpl_perc_emph_v[enc_settings->lowRate], perc_wght_resp[i], perc_resp_len, SMPL_PERC_REG);
        }
    }
    else {  // voiced == 0
        for (int i = 0; i < enc_settings->numsubfrs; i++) {
            smpl_perc_ac2a(enc_state->perc_corrs_buf[numframe][i], perc_resp_len + SMPL_PERC_EMPH_V_LEN - 1,
                smpl_perc_emph_uv[enc_settings->lowRate], perc_wght_resp[i], perc_resp_len, SMPL_PERC_REG);
        }
    }

    //const int perc_resp_len = cmplx_setting->perc_resp_len;
    smpl_update_celp_params((void*)&enc_state->celp_state[state_ix], enc_settings->subfrlen,
        (enc_settings->packet_ms * SMPL_CELP_FS_KHZ) / enc_settings->subfrlen, perc_resp_len, cmplx_setting->celp_ignore_zir, enc_settings->lowRate);

    float sp_act_prob_used = enc_status->useSpActFlatnessThres ? vad->vad_results[numframe] : 1.0f;
    float uv_nonflatness_thres = enc_settings->lowRate ? SMPL_UV_NONFLATNESS_THR : smpl_get_hr_nonflat_thres(enc_status->mainBitRate, sp_act_prob_used);
    float nonflatness[SMPL_MAX_N_SUBFR];
    float nonflat = uv_nonflatness_thres + 0.1f;
    smpl_assert(nonflat > uv_nonflatness_thres);  // default value must not trigger
#ifdef SMPL_UV_NONFLATNESS_PER_SF
    for (int numsubfr = 0; numsubfr < enc_settings->numsubfrs; numsubfr++) {
        if (enc_state->voiced_buf[numframe] == SMPL_FALSE) {
            nonflat = smpl_get_nonflatness(reslpc + numsubfr * enc_settings->subfrlen, enc_settings->subfrlen, wlsf, enc_state->nonflatness_state[state_ix]);
        }
        nonflatness[numsubfr] = nonflat;
    }
#else
    if (enc_state->voiced_buf[numframe] == SMPL_FALSE) {
        nonflat = smpl_get_nonflatness(reslpc, enc_settings->framelen, wlsf, enc_state->nonflatness_state[state_ix]);
    }
    for (int numsubfr = 0; numsubfr < enc_settings->numsubfrs; numsubfr++) {
        nonflatness[numsubfr] = nonflat;
    }
#endif

    // CELP encode
    float nrgres[SMPL_MAX_N_SUBFR];
    float exc_lpc[SMPL_FRAME_LEN];
    int prev_voiced = (numframe == 0) ? enc_state->prev_voiced[state_ix] : enc_state->voiced_buf[numframe - 1];
    for (int numsubfr = 0; numsubfr < enc_settings->numsubfrs; numsubfr++) {
        int t_fcb = numsubfr * enc_settings->subfrlen;
        float wnrg_next = (numsubfr < (enc_settings->numsubfrs - 1)) ? enc_state->wnrgs_buf[numframe][numsubfr + 1] : enc_state->wnrgs_buf[numframe][numsubfr];
        int16_t max_pulses_per_subfr[SMPL_CELP_MAX_RATES];
        float subfr_importance[SMPL_CELP_MAX_RATES];
        int lagind = numsubfr * enc_settings->lag_sf_per_fcb_sf;

        bitrate_controller(&enc_state->rateCtrl[state_ix], enc_status, dtx, vad->coded_as_active_voice, vad->vad_results[numframe], nonflatness[numsubfr],
            enc_state->voicing_strength_buf[numframe], enc_state->voiced_buf[numframe], enc_state->wnrgs_buf[numframe][numsubfr],
            wnrg_next, enc_settings->lowRate, enc_settings->framelen, enc_settings->subfrlen, max_pulses_per_subfr, subfr_importance);

        if (enc_state->voiced_buf[numframe] == SMPL_FALSE && prev_voiced == SMPL_FALSE && nonflatness[numsubfr] < uv_nonflatness_thres){
            memset(max_pulses_per_subfr, 0, sizeof(max_pulses_per_subfr));
        }

        // CELP
        float* lags = enc_state->lags_buf[numframe];
        int tot_surv = 1000 * (cmplx_setting->fcb_tot_surv_20ms_max[enc_settings->lowRate] * enc_settings->subfrlen) / (20 * 16000);
        if (enc_settings->lowRate && enc_state->voiced_buf[numframe]) {
            tot_surv = (int)roundf(tot_surv * SMPL_min(lags[lagind + enc_settings->lag_sf_per_fcb_sf - 1] / enc_settings->subfrlen, 1.0f));
        }
        int16_t numsurv[160];
        int16_t n_pulses[SMPL_CELP_MAX_RATES];
        int16_t pulsepositions[SMPL_CELP_MAX_RATES][SMPL_MAX_PULSES_PER_SF];
        int16_t acbg_idx[SMPL_CELP_MAX_RATES], fcbg_idx[SMPL_CELP_MAX_RATES];

        smpl_distribute_fcb_surv(numsurv, max_pulses_per_subfr[SMPL_CELP_IDX_MAIN], tot_surv);
        TIC(celp)
            smpl_celp_encoder((void*)&enc_state->celp_state[state_ix], reslpc + t_fcb, &predcoefs[numsubfr][0], &perc_wght_resp[numsubfr][0],
                lags + lagind, subfr_importance, max_pulses_per_subfr, numsurv, pulsepositions, n_pulses, acbg_idx, fcbg_idx);
        TOC(celp)
            int start_r = SMPL_CELP_IDX_FEC + (max_pulses_per_subfr[SMPL_CELP_IDX_FEC] == 0);
        for (int r = start_r; r <= SMPL_CELP_IDX_MAIN; r++) {
            LbQuantParams* pParams = (r == SMPL_CELP_IDX_FEC) ? &enc_state->lbrr_lb_quant_params[numframe] : lb_quant_params;
            pParams->n_pulses += n_pulses[r];
            pParams->sf_pulses[numsubfr] = n_pulses[r];
            pParams->fcbg_idx[numsubfr] = fcbg_idx[r];
            pParams->acbg_idx[numsubfr] = acbg_idx[r];
            int16_t* pPulses = &pParams->pulses[numsubfr * enc_settings->subfrlen];
            for (int i = 0; i < n_pulses[r]; i++) {
                int16_t sign = 1 + 2 * (pulsepositions[r][i] >> 15);
                int16_t pos = (pulsepositions[r][i] * sign) - 1;
                pPulses[pos] += sign;
            }
        }
        memcpy(exc_lpc + numsubfr * enc_settings->subfrlen, enc_state->celp_state[state_ix].scratchMem->exc_lpc, enc_settings->subfrlen * sizeof(float));
        nrgres[numsubfr] = enc_state->voiced_buf[numframe] ? 0.0f : smpl_nrg(reslpc + t_fcb, enc_settings->subfrlen) / enc_settings->subfrlen;        
    }

#if SMPL_DUMP_FEATURES
    if (enc_state->fp_features_lsf && state_ix == 0) {
        int rep = enc_settings->lowRate ? 2 : 1;
        for (int sf = 0; sf < enc_settings->numsubfrs; sf++) {
            for (int r = 0; r < rep; r++) {
                float tmp_lsf[SMPL_LPC_ORDER];
                smpl_A2NLSF_16(tmp_lsf, predcoefs[sf]);
                fwrite(tmp_lsf, sizeof(float), SMPL_LPC_ORDER, enc_state->fp_features_lsf);
            }
        }
    }
    if (enc_state->fp_reslpc && state_ix == 0) {
        fwrite(reslpc, sizeof(float), enc_settings->numsubfrs* enc_settings->subfrlen, enc_state->fp_reslpc);
    }
#endif

    if (!enc_state->voiced_buf[numframe]) {
        smpl_quant_nrg_res(nrgres, enc_settings->numsubfrs, lb_quant_params);
    }

    lb_quant_params->voiced = enc_state->voiced_buf[numframe];
    lb_quant_params->lsf_interpol_idx = lsf_interpol_idx;
    memcpy(lb_quant_params->laginds, enc_state->laginds_buf[numframe], sizeof(lb_quant_params->laginds));
    lb_quant_params->blocksegs_ix = enc_state->blocksegs_ix_buf[numframe];

    if (enc_status->internalSampleRate > 16000) {
        // synthesize lowband like decoder
        int n_pulses = 0;
        for (int i = 0; i < enc_settings->numsubfrs; i++) {
            n_pulses += lb_quant_params->sf_pulses[i];
        }
        float normalized_bitrate = smpl_get_normalized_bitrate(n_pulses, enc_settings->framelen);

        float x_reconst_[SMPL_MAX_WB_FRAME_SIZE_IN_SAMPLES + SMPL_LPC_ORDER];
        float* x_reconst = x_reconst_ + SMPL_LPC_ORDER;
        memcpy(x_reconst - SMPL_LPC_ORDER, enc_state->lpc_synth_mem[state_ix], SMPL_LPC_ORDER * sizeof(float));
        float* xhigh_packet = enc_state->xhigh_packet_buf;
        for (int i = 0; i < enc_settings->numsubfrs; i++) {
            float exclpc_temp[SMPL_MAX_SF_LEN];
            memcpy(exclpc_temp, exc_lpc + i * enc_settings->subfrlen, enc_settings->subfrlen * sizeof(float));
            float nrgres_dec = smpl_decode_resnrg(lb_quant_params->nrgres_dbq_Q14[i], enc_settings->subfrlen);
            float noise[SMPL_MAX_SF_LEN];
            smpl_celp_gen_noise(&enc_state->noise_generator[state_ix], exclpc_temp, enc_settings->subfrlen, enc_state->voiced_buf[numframe], lb_quant_params->sf_pulses[i], nrgres_dec,
                lb_quant_params->fcbg_idx[i], lsfs[i], normalized_bitrate, noise);
            if (!enc_state->voiced_buf[numframe] && (lb_quant_params->sf_pulses[i] > 0) && (smpl_uv_pulse_shaping_coefs[enc_settings->lowRate][0][0] < 1.0f)) {
                smpl_filt_arma1(exclpc_temp, enc_settings->subfrlen, smpl_uv_pulse_shaping_coefs[enc_settings->lowRate][0], 2,
                smpl_uv_pulse_shaping_coefs[enc_settings->lowRate][1], 2,
                enc_state->uv_pulse_shaping_state, 2, exclpc_temp);
            } else {
                memset(enc_state->uv_pulse_shaping_state, 0, 2*sizeof(float));
            }

            smpl_add_vec_inplace(noise, exclpc_temp, enc_settings->subfrlen);
            smpl_filt_ar16(exclpc_temp, enc_settings->subfrlen, predcoefs[i], x_reconst + i * enc_settings->subfrlen);
        }
        memcpy(enc_state->lpc_synth_mem[state_ix], x_reconst + enc_settings->framelen - SMPL_LPC_ORDER, SMPL_LPC_ORDER * sizeof(float));
        smpl_hb_encoder((void*)&enc_state->hb_state[state_ix], hb_quant_params, enc_state->voiced_buf[numframe],
            cond_coding, enc_settings->lowRate, x_reconst, xhigh_packet + numframe * enc_settings->framelen, enc_settings->numsubfrs_hb, enc_status->bitRate);
    }

    enc_state->base_encoder_cnt[state_ix]++; // Used to keep track of when to reset secondary encoder
}

static void update_core_encoder_settings(smpl_core_encoder_settings *enc_settings, int packet_ms, int lowRate, int frames_per_packet)
{
    enc_settings->packet_ms = packet_ms;
    enc_settings->frames_per_packet = frames_per_packet;
    enc_settings->frame_ms = enc_settings->packet_ms == 10 ? 10 : 20;
    enc_settings->framelen = enc_settings->frame_ms * SMPL_CELP_FS_KHZ;
    enc_settings->subfrlen = (lowRate == 1) ? 10 * SMPL_CELP_FS_KHZ : 5 * SMPL_CELP_FS_KHZ;
    enc_settings->numsubfrs = enc_settings->framelen / enc_settings->subfrlen;
    enc_settings->numsubfrs_hb = enc_settings->framelen / SMPL_HB_SF_LEN;
    enc_settings->lag_sf_per_fcb_sf = enc_settings->subfrlen / SMPL_LAG_SUBFRLEN;
    enc_settings->lowRate = lowRate;
}

int smpl_core_encode(
    smpl_core_encoder* enc_state,
    smpl_EncControlStruct* enc_status,
    ec_enc *psRangeEnc,        /* I/O Compressor data structure */
    const opus_int16* x_16b,
    int x_16b_len,
    smpl_vad_status* vad,
    smpl_dtx_status* dtx,
    int lowRate)
{
    CLEAR_FLP_CHECK();

    int res = SMPL_NO_ERROR;
    float x[SMPL_MAX_FB_PACKET_SIZE_IN_SAMPLES];
    int x_len = (x_16b_len * enc_status->internalSampleRate) / enc_status->API_sampleRate;
    const int xhp_packet_len = (x_16b_len * 16000) / enc_status->API_sampleRate;
    int xhp_packet_extra = 0;
    smpl_core_encoder_settings enc_settings;
    {
        int packet_ms = (x_len * 1000) / enc_status->internalSampleRate;
        smpl_assert(packet_ms * enc_status->API_sampleRate / 1000 == x_16b_len);
        update_core_encoder_settings(&enc_settings, packet_ms, lowRate, vad->frames_per_packet);
    }
    LbQuantParams lb_quant_params;
    HbQuantParams hb_quant_params;
    float bits_used[SMPL_CELP_MAX_RATES];
    bits_used[SMPL_CELP_IDX_FEC] = (psRangeEnc != NULL) ? ec_tell(psRangeEnc) : 0;

    ComplexitySetting cmplx_setting;
    update_complexity_setting(enc_status, &cmplx_setting);

    uint32_t fec_bytes = add_pending_fec(enc_state, enc_status, psRangeEnc, vad, lowRate, enc_settings.packet_ms);
    bits_used[SMPL_CELP_IDX_FEC] = (psRangeEnc != NULL) ? (ec_tell(psRangeEnc) - bits_used[SMPL_CELP_IDX_FEC]) / (float)enc_settings.frames_per_packet : 0;

    smpl_update_pitch_params((void*)&enc_state->pitch_state, cmplx_setting.pitch_numstates1, lowRate);

    update_hp_coefs(enc_state, enc_status->hp_f_corner_Hz);

    // Resampling and resampler initialzation could possibly be done outside
    // if not it shuld be done here
    if (enc_status->internalSampleRate != enc_status->API_sampleRate) {
        if ((enc_state->resampler_api_to_internal.Fs_out_kHz * 1000 != enc_status->internalSampleRate) ||
            (enc_state->resampler_api_to_internal.Fs_in_kHz * 1000 != enc_status->API_sampleRate)) {
            res += silk_resampler_init(&(enc_state->resampler_api_to_internal), enc_status->API_sampleRate, enc_status->internalSampleRate, 1);
            smpl_assert(res == SMPL_NO_ERROR);
        }
        // Resample
        opus_int16 x_16b_resampled[SMPL_MAX_FB_PACKET_SIZE_IN_SAMPLES];
        res += silk_resampler(&(enc_state->resampler_api_to_internal), x_16b_resampled, x_16b, x_16b_len);
        smpl_assert(res == SMPL_NO_ERROR);
        for (int i=0; i<x_len; i++)
            x[i] = x_16b_resampled[i]/32768.0f;
    } else {
        for (int i=0; i<x_len; i++)
            x[i] = x_16b[i]/32768.0f;
    }

#if SMPL_DUMP_FEATURES
    if (enc_state->align_samples) {
        opus_int16 tmp = 0;
        for (int i = 0; i < enc_state->align_samples; i++){
            if (enc_state->fp_clean) { fwrite(&tmp, sizeof(opus_int16), 1, enc_state->fp_clean); }
            if (enc_state->fp_clean_hp) { fwrite(&tmp, sizeof(opus_int16), 1, enc_state->fp_clean_hp); }
            if (enc_state->fp_clean_hb) { fwrite(&tmp, sizeof(opus_int16), 1, enc_state->fp_clean_hb); }
        }
        enc_state->align_samples = 0;
    }
#endif
    // Use bwe if sample rate is 32kHz or 48kHz
    memcpy(enc_state->xhp_packet_buf, enc_state->lpc_buf_mem, (SMPL_LPC_BUF_MEM_LEN + SMPL_WINNEXT_WB_LEN) * sizeof(float));
    float* x_in16k = &enc_state->xhp_packet_buf[SMPL_LPC_BUF_MEM_LEN + SMPL_WINNEXT_WB_LEN];
    float* xhigh_packet = enc_state->xhigh_packet_buf;
    if (enc_status->internalSampleRate > 16000)
    {
        if (enc_status->internalSampleRate != enc_status->internalSampleRate_prev) {
            memset(enc_state->filterbank_buf, 0, sizeof(enc_state->filterbank_buf));
        }
        smpl_band_split(x, x_len, x_in16k, xhigh_packet, enc_state->filterbank_buf);
#if SMPL_DUMP_FEATURES
        if (enc_state->fp_clean_hb) {
            int16_t xTemp16[SMPL_ARR_LEN(x)];
            for (int i = 0; i < xhp_packet_len; i++) {
                xTemp16[i] = (int16_t)roundf(xhigh_packet[i] * 32768.0f);
            }
            fwrite(xTemp16, sizeof(opus_int16), xhp_packet_len, enc_state->fp_clean_hb);
        }
        if (enc_state->fp_clean) {
            int16_t xTemp16[SMPL_ARR_LEN(x)];
            for (int i = 0; i < xhp_packet_len; i++) {
                xTemp16[i] = (int16_t)roundf(x_in16k[i] * 32768.0f);
            }
            if (enc_state->fp_clean) {
                fwrite(xTemp16, sizeof(opus_int16), xhp_packet_len, enc_state->fp_clean);
            }
        }
#endif
        // # xhp_packet_extra: the BWE filterbank uses the lookahead to run the FB in reverse time. But the resulting lookahead
        // # samples are slightly distorted due to the FB's zero initial state. Therefore we need to regenerate those samples
        // # for the next packet, which means xhp_packet is cfg.winnext longer when using BWE
        // Loop over frames
        for (int numframe = 0; numframe < enc_settings.frames_per_packet; numframe++)
        {
            if (enc_state->hp_fcorner_3dB_Hz > 0) {
                smpl_filt_arma2(x_in16k + numframe * enc_settings.framelen, enc_settings.framelen, enc_state->hp_b2, SMPL_HP_A_LEN, enc_state->hp_a2, SMPL_HP_A_LEN,
                    enc_state->hp_arma2_state, (SMPL_HP_A_LEN - 1) * 2, x_in16k + numframe * enc_settings.framelen);
            }
        }
        if (enc_state->hp_fcorner_3dB_Hz > 0) {
            float state_copy[(SMPL_HP_A_LEN - 1) * 2];
            memcpy(state_copy, enc_state->hp_arma2_state, (SMPL_HP_A_LEN - 1) * 2 * sizeof(float));
            smpl_filt_arma2(x_in16k + xhp_packet_len, SMPL_WINNEXT_WB_LEN, enc_state->hp_b2, SMPL_HP_A_LEN, enc_state->hp_a2, SMPL_HP_A_LEN, state_copy, (SMPL_HP_A_LEN - 1) * 2, x_in16k + xhp_packet_len);
        }
        xhp_packet_extra = SMPL_WINNEXT_WB_LEN;
#if SMPL_DUMP_FEATURES
        if (enc_state->fp_clean_hp) {
            int16_t xTemp16[SMPL_ARR_LEN(x)];
            for (int i = 0; i < xhp_packet_len; i++) {
                xTemp16[i] = (int16_t)roundf(x_in16k[i] * 32768.0f);
            }
            if (enc_state->fp_clean_hp) {
                fwrite(xTemp16, sizeof(opus_int16), xhp_packet_len, enc_state->fp_clean_hp);
            }
        }
#endif
        //memcpy(x_in16k, xhp_packet, (xhp_packet_len + xhp_packet_extra) * sizeof(float));
    } else {
        if (enc_state->hp_fcorner_3dB_Hz > 0) {
            smpl_filt_arma2(x, x_len, enc_state->hp_b2, SMPL_HP_A_LEN, enc_state->hp_a2, SMPL_HP_A_LEN, enc_state->hp_arma2_state, (SMPL_HP_A_LEN - 1) * 2, x_in16k);
        } else{
            memcpy(x_in16k, x, x_len*sizeof(float));
        }
#if SMPL_DUMP_FEATURES
        {
            int16_t xTemp16[SMPL_ARR_LEN(x)];
            for (int i = 0; i < x_len; i++) {
                xTemp16[i] = (int16_t)roundf(x[i] * 32768.0f);
            }
            if (enc_state->fp_clean) {
                fwrite(xTemp16, sizeof(opus_int16), x_len, enc_state->fp_clean);
            }
            for (int i = 0; i < x_len; i++) {
                xTemp16[i] = (int16_t)roundf(x_in16k[i] * 32768.0f);
            }
            if (enc_state->fp_clean_hp) {
                fwrite(xTemp16, sizeof(opus_int16), x_16b_len, enc_state->fp_clean_hp);
            }
        }
#endif
    }
    enc_status->internalSampleRate_prev = enc_status->internalSampleRate;

    // Difference in Julia and C with speech activity. C calculates over entire packet rather than per frame
    // activity comes in through API, and SID frame has to be calculated on stereo image

    int prev_voiced = SMPL_FALSE;
    int send_packet = !(dtx->sid_frame) || dtx->send_sid_frame;
    enc_state->hasFecData = SMPL_FALSE;
    // Loop over frames
    for (int numframe = 0; numframe < enc_settings.frames_per_packet; numframe++)
    {
        bits_used[SMPL_CELP_IDX_MAIN] = (psRangeEnc != NULL) ? ec_tell(psRangeEnc) : 0;
        memset(&lb_quant_params, 0, sizeof(LbQuantParams));
        memset(&hb_quant_params, 0, sizeof(HbQuantParams));
        if (enc_status->fecBitRate > 0 && enc_status->fecBitRate != enc_status->mainBitRate) {
            memset(&enc_state->lbrr_lb_quant_params[numframe], 0, sizeof(LbQuantParams));
            memset(&enc_state->lbrr_hb_quant_params[numframe], 0, sizeof(HbQuantParams));
        }
        const int lpcbuf_len = (enc_settings.packet_ms == 10) ? 304 : 448;
        int shorter = SMPL_WINNEXT_WB_LONG_LEN - SMPL_WINNEXT_WB_LEN;

        float* xhp_frame = x_in16k - SMPL_WINNEXT_WB_LEN + enc_settings.framelen * numframe + xhp_packet_extra; // Frame we are going to encode
        float* lpcbuf = xhp_frame + enc_settings.framelen + SMPL_WINNEXT_WB_LONG_LEN - lpcbuf_len;

        float frame_energy = smpl_nrg(xhp_frame, enc_settings.framelen);

        // LPC
        float lpcbuf_windowed[448];
        smpl_window(lpcbuf, lpcbuf_windowed, lpcbuf_len, enc_settings.frame_ms, numframe < (enc_settings.frames_per_packet - 1), SMPL_TRUE);

        float* A = enc_state->A_buf[numframe];
        double R[SMPL_LPC_ORDER + 1];
        float lpcbuf_F2[SMPL_F_LEN];

        smpl_lpc(lpcbuf_windowed, lpcbuf_len, SMPL_LPC_REG, A, R, SMPL_LPC_ORDER, lpcbuf_F2);
        smpl_assert(res == SMPL_NO_ERROR);
        // Bandwidth expansion
        smpl_bwe_expand(A, SMPL_LPC_ORDER, SMPL_LPC_BWE);

        TIC(perc_model)
        //float perc_corrs[MAX_NUM_SUBFR][SMPL_MAX_L_RESP + SMPL_PERC_EMPH_V_LEN - 1];
        if (enc_settings.subfrlen == 80) { // 5ms subframes
            // Only compute perceptual corrs for every second subframe and interpolate result for in-between subframes
            for (int numsubfr = 1; numsubfr < enc_settings.numsubfrs; numsubfr+=2) {
                // pass in two subframes at the same time
                int t_subfr = lpcbuf_len - enc_settings.framelen - shorter + (numsubfr - 1) * enc_settings.subfrlen;
                int t_subfr_len = 2 * enc_settings.subfrlen + shorter;
                int is_last_subfr = (numframe == (enc_settings.frames_per_packet-1)) && (numsubfr == (enc_settings.numsubfrs-1));
                float *R_ptr = enc_state->perc_corrs_buf[numframe][numsubfr];
                smpl_perc_model(enc_state->perc_wght_buf, lpcbuf + t_subfr, t_subfr_len, enc_settings.frame_ms, is_last_subfr, R_ptr, cmplx_setting.perc_resp_len + SMPL_PERC_EMPH_V_LEN - 1);

                // Calculate interpolated perceptual corrs
                for (int i = 0; i < cmplx_setting.perc_resp_len + SMPL_PERC_EMPH_V_LEN - 1; i++)
                {
                    enc_state->perc_corrs_buf[numframe][numsubfr-1][i] = 0.5f * (enc_state->perc_corrs_buf[numframe][numsubfr][i] + enc_state->perc_corrs_prev[i]);
                }
                memcpy(enc_state->perc_corrs_prev, enc_state->perc_corrs_buf[numframe][numsubfr], (cmplx_setting.perc_resp_len + SMPL_PERC_EMPH_V_LEN - 1)*sizeof(float));
            }
        } else { // 10ms subframes
            for (int numsubfr = 0; numsubfr < enc_settings.numsubfrs; numsubfr++)
            {
                int t_subfr = lpcbuf_len - enc_settings.framelen - shorter + (numsubfr * enc_settings.subfrlen);
                int is_last_subfr = (numframe == (enc_settings.frames_per_packet-1)) && (numsubfr == (enc_settings.numsubfrs-1));
                float *R_ptr = enc_state->perc_corrs_buf[numframe][numsubfr];
                smpl_perc_model(enc_state->perc_wght_buf, lpcbuf + t_subfr, enc_settings.subfrlen + shorter, enc_settings.frame_ms, is_last_subfr, R_ptr, SMPL_PERC_RESP_LEN + SMPL_PERC_EMPH_V_LEN - 1);
            }
        }
        TOC(perc_model)
        float perc_wght_resps_pitch[MAX_NUM_SUBFR][SMPL_LPC_ORDER+1];
        for (int i = 0; i < enc_settings.numsubfrs; i++)
        {
            smpl_perc_ac2a(enc_state->perc_corrs_buf[numframe][i], cmplx_setting.perc_resp_len + 1, smpl_perc_emph_pitch,
                perc_wght_resps_pitch[i], cmplx_setting.pitch_perc_resp_len, SMPL_PERC_REG);
        }
        int ltp_buf_len = enc_settings.framelen + SMPL_MAX_PITCH_LEN + SMPL_PITCH_LOOKAHEAD_LEN + SMPL_PITCH_TOT_INTERPOL_DELAY_LEN;
        memmove(enc_state->ltp_buf, enc_state->ltp_buf + enc_settings.framelen, (SMPL_MAX_LTP_BUF_LEN- enc_settings.framelen-SMPL_PITCH_LOOKAHEAD_LEN)*sizeof(float));
        float *w_speech = enc_state->ltp_buf + SMPL_MAX_LTP_BUF_LEN - enc_settings.numsubfrs* enc_settings.subfrlen - SMPL_PITCH_LOOKAHEAD_LEN;
        for (int i = 0; i < enc_settings.numsubfrs; i++)
        {
            cmplx_setting.pitch_perc_filt_ma(xhp_frame + i * enc_settings.subfrlen, enc_settings.subfrlen, perc_wght_resps_pitch[i], cmplx_setting.pitch_perc_resp_len, w_speech + i * enc_settings.subfrlen);
        }
        cmplx_setting.pitch_perc_filt_ma(xhp_frame + enc_settings.framelen, SMPL_PITCH_LOOKAHEAD_LEN, perc_wght_resps_pitch[enc_settings.numsubfrs-1],
            cmplx_setting.pitch_perc_resp_len, enc_state->ltp_buf + SMPL_MAX_LTP_BUF_LEN - SMPL_PITCH_LOOKAHEAD_LEN);

        float* lags = enc_state->lags_buf[numframe];
        float pitchcorr = 0.0f;
        memset(lags, 0, SMPL_PITCH_NUM_SUBFRAMES * sizeof(float));
        float avg_lag = 0.0f, harm_strength = 0.0f;;
        TIC(pitch)
        smpl_pitch((void*)&enc_state->pitch_state, enc_state->ltp_buf + SMPL_MAX_LTP_BUF_LEN - ltp_buf_len, ltp_buf_len, SMPL_PITCH_LOOKAHEAD_LEN, lpcbuf_F2, vad->coded_as_active_voice,
            enc_settings.lag_sf_per_fcb_sf * enc_settings.numsubfrs, lags, enc_state->laginds_buf[numframe], &pitchcorr, &enc_state->blocksegs_ix_buf[numframe], &avg_lag, &harm_strength);
        smpl_assert(!res);
        TOC(pitch)

        // voiced vs unvoiced
        TIC(signal_mode)
        enc_state->voicing_strength_buf[numframe] = smpl_get_signal_mode(pitchcorr, lags, avg_lag, harm_strength, enc_settings.framelen / SMPL_LAG_SUBFRLEN,
            lpcbuf_F2, SMPL_F_LEN, vad->vad_results[numframe], &(enc_state->vuv_mode));
        enc_state->voiced_buf[numframe] = (enc_state->voicing_strength_buf[numframe] > 0.0f) && vad->coded_as_active_voice;
        if (!enc_state->voiced_buf[numframe]) {
            memset(lags, 0, (enc_settings.framelen / SMPL_LAG_SUBFRLEN) * sizeof(float));
        }
        TOC(signal_mode)

        for (int i = 0; i < enc_settings.numsubfrs; i++) {
            enc_state->wnrgs_buf[numframe][i] = smpl_nrg(w_speech + i * enc_settings.subfrlen, enc_settings.subfrlen);
        }
        int cond_coding = (enc_state->voiced_buf[numframe] == prev_voiced) && (numframe > 0);
        smpl_base_encode(enc_state, enc_status, vad, dtx, &cmplx_setting,
            &lb_quant_params, &hb_quant_params, &enc_settings, numframe, cond_coding, SMPL_BASE_ENCODER_1);

        if (!enc_state->voiced_buf[numframe] || numframe == (enc_settings.frames_per_packet - 1)) {
            smpl_pitch_reset_cond((void*)&enc_state->pitch_state);
        }

        // Update dtx candidate list if frame is inactive
        if (vad->vad_results_type[numframe] == INACTIVE || vad->vad_results_type[numframe] == HANGOVER) {
            // Only works for first frame in multi-frame packet due to conditional coding of LSF
            if (!enc_state->voiced_buf[numframe] && numframe == 0) {
                dtx->candidate_insert_idx = (dtx->candidate_insert_idx + 1) % SMPL_DTX_NO_CANDIDATES;
                dtx->energy[dtx->candidate_insert_idx] = frame_energy;
                dtx->lowRate[dtx->candidate_insert_idx] = lowRate;
                // Small optimization. No need to copy pulses as always zero n
                memcpy(&(dtx->LbQuantParams[dtx->candidate_insert_idx]), &lb_quant_params, sizeof(LbQuantParams));
                memcpy(&(dtx->HbQuantParams[dtx->candidate_insert_idx]), &hb_quant_params, sizeof(HbQuantParams));
            }
        }

        if (send_packet && !dtx->sid_frame && psRangeEnc != NULL) {
            // Encode a regular packet
            smpl_encode_lb_params((void*)&enc_state->parm_encoder_state, psRangeEnc, &lb_quant_params, enc_settings.framelen,
                enc_settings.numsubfrs, vad->coded_as_active_voice, cond_coding, lowRate, numframe, prev_voiced, dtx->sid_frame);
            if (enc_status->internalSampleRate > 16000) {
                smpl_encode_hb_params((void*)&enc_state->parm_encoder_state, psRangeEnc, (void*)&enc_state->hb_state, &hb_quant_params, enc_settings.framelen, enc_state->voiced_buf[numframe], cond_coding, lowRate);
            }
            if (enc_status->fecBitRate > 0 && vad->coded_as_active_voice) {
                if (enc_status->fecBitRate == enc_status->mainBitRate) {
                    memcpy(&enc_state->lbrr_lb_quant_params[numframe], &lb_quant_params, sizeof(LbQuantParams));
                }
                else {
                    // FEC celp parameters generated by Celp code at lower bitrate
                    enc_state->lbrr_lb_quant_params[numframe].voiced = lb_quant_params.voiced;
                    enc_state->lbrr_lb_quant_params[numframe].lsf_interpol_idx = lb_quant_params.lsf_interpol_idx;
                    memcpy(&enc_state->lbrr_lb_quant_params[numframe].lsf_idx, lb_quant_params.lsf_idx, (SMPL_LPC_ORDER + 1) * sizeof(int8_t));
                    memcpy(&enc_state->lbrr_lb_quant_params[numframe].laginds, lb_quant_params.laginds, SMPL_PITCH_NUM_SUBFRAMES * sizeof(int));
                    enc_state->lbrr_lb_quant_params[numframe].blocksegs_ix = lb_quant_params.blocksegs_ix;
                    memcpy(&enc_state->lbrr_lb_quant_params[numframe].nrgres_dbq_Q14, lb_quant_params.nrgres_dbq_Q14, enc_settings.numsubfrs * sizeof(int32_t));
                    enc_state->lbrr_lb_quant_params[numframe].nrgres_frame_qi = lb_quant_params.nrgres_frame_qi;
                    enc_state->lbrr_lb_quant_params[numframe].nrgres_shape_qi = lb_quant_params.nrgres_shape_qi;
                }
                if (enc_status->internalSampleRate > 16000) {
                    memcpy(&enc_state->lbrr_hb_quant_params[numframe], &hb_quant_params, sizeof(HbQuantParams));
                }
                enc_state->hasFecData = SMPL_TRUE;
            }
        }
        else if (send_packet && dtx->sid_frame && numframe == 0) {
            // Encode a SID packet. For multi frame we only encode one frame for the whole packet
            float min_nrg = FLT_MAX;
            int min_idx = -1;
            for (int i = 0; i < SMPL_DTX_NO_CANDIDATES; i++) {
                if ((dtx->energy[i] < min_nrg) && dtx->lowRate[i] == lowRate) {
                    min_idx = i;
                    min_nrg = dtx->energy[i];
                }
            }
            smpl_assert(min_idx != -1);
            smpl_encode_lb_params((void*)&enc_state->parm_encoder_state, psRangeEnc, &(dtx->LbQuantParams[min_idx]),
                enc_settings.framelen, enc_settings.numsubfrs, SMPL_FALSE, SMPL_FALSE, lowRate, numframe, prev_voiced, dtx->sid_frame);
            if (enc_status->internalSampleRate > 16000) {
                smpl_encode_hb_params((void*)&enc_state->parm_encoder_state, psRangeEnc, (void*)&enc_state->hb_state[SMPL_BASE_ENCODER_1], &(dtx->HbQuantParams[min_idx]), enc_settings.framelen, SMPL_FALSE, SMPL_FALSE, lowRate);
            }
        } else {
            // Fixes crash in hp lsf encoder
            enc_state->hb_state[SMPL_BASE_ENCODER_1].prev_hb_lpc_ix = hb_quant_params.lsf_idx;
        }
        if (psRangeEnc != NULL) {
            bits_used[SMPL_CELP_IDX_MAIN] = (psRangeEnc != NULL) ? ec_tell(psRangeEnc) - bits_used[SMPL_CELP_IDX_MAIN] : 0;
            bitrate_controller_update_scale(&enc_state->rateCtrl[SMPL_BASE_ENCODER_1], enc_status, enc_settings.frame_ms, enc_settings.frames_per_packet, bits_used, vad->coded_as_active_voice);
        }
        prev_voiced = enc_state->voiced_buf[numframe];

        // Update buffer
        memcpy(enc_state->lpc_buf_mem, &enc_state->xhp_packet_buf[xhp_packet_len], (SMPL_LPC_BUF_MEM_LEN + SMPL_WINNEXT_WB_LEN) * sizeof(float));

        FLP_CHECK();
    }

    enc_state->prevLowRate = lowRate;
    enc_state->prevInternalSampleRate = enc_status->internalSampleRate;
    enc_state->prev_packet_ms = enc_settings.packet_ms;
    enc_state->prev_voiced[SMPL_BASE_ENCODER_1] = enc_state->voiced_buf[enc_settings.frames_per_packet - 1];

    // Track inband FEC and main payload sizes
    if (psRangeEnc) {
        enc_state->ifec_payload_size = send_packet ? fec_bytes : 0;
        enc_state->main_payload_size = send_packet ? (((ec_tell(psRangeEnc) + 7) / 8) - enc_state->ifec_payload_size) : 0;
    }
    return res;
}

int smpl_core_encode_second(
    smpl_core_encoder* enc_state,
    smpl_EncControlStruct* enc_status,
    ec_enc* psRangeEnc,        /* I/O Compressor data structure */
    smpl_vad_status* vad_status,
    smpl_dtx_status* dtx,
    int lowRate)
{
    int res = 0;

    if (enc_status->fecBitRate > 0) {
        smpl_assert(0);
        return -1;
    }
    int send_packet = !(dtx->sid_frame) || dtx->send_sid_frame;
    if (!send_packet) {
        return -1;
    }
    if (enc_state->base_encoder_cnt[SMPL_BASE_ENCODER_1] <= enc_state->base_encoder_cnt[SMPL_BASE_ENCODER_2]) {
        smpl_assert(0); // smpl_core_encode has to be called before smpl_core_encode_second
        return -1;
    }

    if (enc_state->base_encoder_cnt[SMPL_BASE_ENCODER_1] > enc_state->base_encoder_cnt[SMPL_BASE_ENCODER_2] + vad_status->frames_per_packet) {
        // Copy ENCODER_1 States to ENCODER_2
        memcpy(&enc_state->celp_state[SMPL_BASE_ENCODER_2], &enc_state->celp_state[SMPL_BASE_ENCODER_1], sizeof(enc_state->celp_state[SMPL_BASE_ENCODER_1]));
        memcpy(&enc_state->hb_state[SMPL_BASE_ENCODER_2], &enc_state->hb_state[SMPL_BASE_ENCODER_1], sizeof(enc_state->hb_state[SMPL_BASE_ENCODER_1]));
        memcpy(&enc_state->prev_lsf[SMPL_BASE_ENCODER_2], &enc_state->prev_lsf[SMPL_BASE_ENCODER_1], sizeof(enc_state->prev_lsf[SMPL_BASE_ENCODER_1]));
        memcpy(&enc_state->lpc_synth_mem[SMPL_BASE_ENCODER_2], &enc_state->lpc_synth_mem[SMPL_BASE_ENCODER_1], sizeof(enc_state->lpc_synth_mem[SMPL_BASE_ENCODER_1]));
        memset(enc_state->base_encoder_cnt, 0, sizeof(enc_state->base_encoder_cnt));
    }

    smpl_core_encoder_settings enc_settings;
    update_core_encoder_settings(&enc_settings, enc_state->prev_packet_ms, lowRate, vad_status->frames_per_packet);

    ComplexitySetting cmplx_setting;
    update_complexity_setting(enc_status, &cmplx_setting);

    LbQuantParams lb_quant_params;
    HbQuantParams hb_quant_params;
    float bits_used[SMPL_CELP_MAX_RATES];
    bits_used[SMPL_CELP_IDX_FEC] = 0;

    // Loop over frames
    int prev_voiced = SMPL_FALSE;
    for (int numframe = 0; numframe < enc_settings.frames_per_packet; numframe++)
    {
        bits_used[SMPL_CELP_IDX_MAIN] = (psRangeEnc != NULL) ? ec_tell(psRangeEnc) : 0;
        memset(&lb_quant_params, 0, sizeof(LbQuantParams));
        memset(&hb_quant_params, 0, sizeof(HbQuantParams));

        if (enc_state->prevLowRate == SMPL_FALSE && lowRate == SMPL_TRUE) {
            // Perceptual weights for 5ms subframes but we now have 10 ms subframes
            // average correlations for 2 5 ms subframes to give 10 ms subframe
            for (int sf = 0; sf < enc_settings.numsubfrs; sf++) {
                for (int i = 0; i < cmplx_setting.perc_resp_len + SMPL_PERC_EMPH_V_LEN - 1; i++){
                    enc_state->perc_corrs_buf[numframe][sf][i] = 0.5f * (enc_state->perc_corrs_buf[numframe][2 * sf][i] + enc_state->perc_corrs_buf[numframe][2 * sf + 1][i]);
                }
            }
        }

        int cond_coding = (enc_state->voiced_buf[numframe] == prev_voiced) && (numframe > 0);
        smpl_base_encode(enc_state, enc_status, vad_status, dtx, &cmplx_setting,
            &lb_quant_params, &hb_quant_params, &enc_settings, numframe, cond_coding, SMPL_BASE_ENCODER_2);

        if (send_packet && !dtx->sid_frame && psRangeEnc != NULL) {
            smpl_encode_lb_params((void*)&enc_state->parm_encoder_state, psRangeEnc, &lb_quant_params, enc_settings.framelen,
                enc_settings.numsubfrs, vad_status->coded_as_active_voice, cond_coding, lowRate, numframe, prev_voiced, dtx->sid_frame);
            if (enc_status->internalSampleRate > 16000) {
                smpl_encode_hb_params((void*)&enc_state->parm_encoder_state, psRangeEnc, (void*)&enc_state->hb_state[SMPL_BASE_ENCODER_2], &hb_quant_params, enc_settings.framelen, enc_state->voiced_buf[numframe], cond_coding, lowRate);
            }
        }
        else {
            // Fixes crash in hp lsf encoder
            enc_state->hb_state[SMPL_BASE_ENCODER_2].prev_hb_lpc_ix = hb_quant_params.lsf_idx;
        }
        prev_voiced = enc_state->voiced_buf[numframe];
        if (psRangeEnc != NULL) {
            bits_used[SMPL_CELP_IDX_MAIN] = (psRangeEnc != NULL) ? ec_tell(psRangeEnc) - bits_used[SMPL_CELP_IDX_MAIN] : 0;
            bitrate_controller_update_scale(&enc_state->rateCtrl[SMPL_BASE_ENCODER_2], enc_status, enc_settings.frame_ms, enc_settings.frames_per_packet, bits_used, vad_status->coded_as_active_voice);
        }
    }
    enc_state->prev_voiced[SMPL_BASE_ENCODER_2] = enc_state->voiced_buf[enc_settings.frames_per_packet - 1];
    return res;
}
