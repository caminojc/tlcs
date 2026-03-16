#include "smpl_plc.h"
#include "smpl_errors.h"
#include "smpl_lpc.h"
#include "smpl_defines.h"
#include "smpl_celp.h"
#include "smpl_codec_util.h"
#include "smpl_quant_nrg_res.h"
#include "smpl_typedef.h"
#include "smpl_lsf_wrapper.h"
#include "smpl_tables.h"

static inline float limit_acbgains(const PLC* plc,  const float* acb_gains, int subfrlen) {
    float ctr_tap = 0.0f;
    float subfrlen_comp = (float)subfrlen / SMPL_MIN_SF_LEN;

    for (int i = 0; i < SMPL_ACBG_M; i++) {
        ctr_tap += acb_gains[i] * ((i > 0) + 1);
    }
    ctr_tap = SMPL_abs(ctr_tap);
    ctr_tap = powf(ctr_tap, plc->precise_lag / (subfrlen_comp * subfrlen)); // compensate for subframe size and pitch lag
    ctr_tap = SMPL_min(SMPL_max(ctr_tap, SMPL_PLC_ACB_MIN), SMPL_PLC_ACB_MAX);

    return ctr_tap;
}

void smpl_plc_reset(PLC* plc, const smpl_TOC* toc) {
    plc->loss_count_subfr = 0;
    memcpy(&plc->toc, toc, sizeof(smpl_TOC));
    plc->hb_gain_attenuation = 1.0f;
}

void smpl_plc_update_celp(
    PLC* plc,
    const LbQuantParams* lb_params,
    const float* acb_gains,
    const float* A,
    const float* lsf,
    const float* lags,
    int lags_per_frame,
    int num_subframes,
    int fcb_subfrlen
) {
    // copy subset of LB parameters
    plc->voiced = lb_params->voiced;
    plc->last_nrgres_Q14 = lb_params->nrgres_dbq_Q14[num_subframes - 1];
    plc->last_subfr_pulses = lb_params->sf_pulses[num_subframes - 1];
    plc->last_fcb_idx = lb_params->fcbg_idx[num_subframes - 1];

    memcpy(plc->lsf, lsf, SMPL_LPC_ORDER * sizeof(float));
    memcpy(plc->A, A, (SMPL_LPC_ORDER+1) * sizeof(float));
    plc->precise_lag = lags[lags_per_frame - 1];

    if (plc->loss_count_subfr > 0 || plc->toc.SID)
    {
        int num_elems = SMPL_MAX_PITCH_LAG / fcb_subfrlen;
        memset(plc->acb_buf,     0,     num_elems * sizeof(float));
        memset(plc->exc_nrg_buf, 0,     num_elems * sizeof(float));
        memset(plc->lag_buf,     0, 2 * num_elems * sizeof(float));
    }

    int buf_left = 2 * SMPL_MAX_PITCH_LAG / SMPL_LAG_SUBFRLEN - lags_per_frame;
    memmove(plc->lag_buf, plc->lag_buf + lags_per_frame, buf_left * sizeof(float));
    memcpy(plc->lag_buf + buf_left, lags, lags_per_frame * sizeof(float));

    int to_add = SMPL_min(num_subframes, SMPL_MAX_PITCH_LAG / fcb_subfrlen);
    buf_left = SMPL_MAX_PITCH_LAG / fcb_subfrlen - to_add;
    memmove(plc->acb_buf, plc->acb_buf + to_add, buf_left * sizeof(float));
    for (int i = 0; i < to_add; i++) {
        plc->acb_buf[buf_left + i] = limit_acbgains(plc, acb_gains + i * SMPL_ACBG_M, fcb_subfrlen);
    }
}

void smpl_plc_update_hb(PLC* plc, const float* A_hb, float hb_gain) {
    memcpy(plc->A_hb, A_hb, (SMPL_HB_LPC_ORDER + 1) * sizeof(float));
    plc->hb_gain = hb_gain;
}

void smpl_plc_conceal_celp_dtx(PLC* plc,
    LbQuantParams* lb_params,
    float *lsf_prev,
    float* A,
    float* lsfs,
    int num_subframes,
    float* lags,
    int lags_per_frame)
{
    smpl_assert(plc->toc.SID == SMPL_TRUE);
    const float* p_lsf_dtx_interpol = num_subframes == 4 ? &smpl_lsf_interpol_dtx_4[0] : (num_subframes == 2 ? &smpl_lsf_interpol_dtx_2[0] : &smpl_lsf_interpol_dtx_1);
    smpl_lpc_interpol(plc->lsf, lsf_prev, p_lsf_dtx_interpol, SMPL_LPC_ORDER, num_subframes, A, lsfs);

    for (int i = 0; i < num_subframes; i++) {
        lb_params->nrgres_dbq_Q14[i] = plc->last_nrgres_Q14;
        lb_params->sf_pulses[i] = 0;
        lb_params->fcbg_idx[i] = 0;
    }
    lb_params->n_pulses = 0;
    memset(lags, 0, lags_per_frame * sizeof(float));
}

void smpl_plc_conceal_hb_dtx(const PLC* plc, float* A_hb, float* hb_gains, int num_subframes, int num_hb_subframes) {
    smpl_assert(plc->toc.SID == SMPL_TRUE);
    for (int i = 0; i < num_subframes; i++) {
        memcpy(A_hb + (SMPL_HB_LPC_ORDER + 1) * i, plc->A_hb, (SMPL_HB_LPC_ORDER + 1) * sizeof(float));
    }
    for (int i = 0; i < num_hb_subframes; i++) {
        hb_gains[i] = plc->hb_gain;
    }
}

static inline float smpl_round_to(float x, float step, float offset) {
    return step * roundf((x - offset) / step) + offset;
}

void smpl_plc_conceal_celp(
    PLC *plc,
    LbQuantParams *lb_params,
    float* acb_gains,
    float* A,
    float* lsfs,
    int num_subframes,
    int subfrlen,
    float* lags)
{
    int len_buffers = SMPL_MAX_PITCH_LAG / subfrlen;
    smpl_assert(plc->toc.SID == SMPL_FALSE);

    lb_params->n_pulses = plc->last_subfr_pulses * num_subframes;
    float subfrlen_comp = (float)subfrlen / SMPL_MIN_SF_LEN;
    lb_params->voiced = plc->voiced;
    for (int i = 0; i < num_subframes; i++) {
        plc->loss_count_subfr += 1;

        lb_params->nrgres_dbq_Q14[i] = plc->last_nrgres_Q14;
        lb_params->sf_pulses[i] = plc->last_subfr_pulses;
        lb_params->fcbg_idx[i] = plc->last_fcb_idx;

        smpl_bwe_expand(plc->A, SMPL_LPC_ORDER, powf(plc->voiced ? SMPL_PLC_BWE_V : SMPL_PLC_BWE_UV, subfrlen_comp));
        memcpy(A + i * (SMPL_LPC_ORDER + 1) + 1, plc->A + 1, SMPL_LPC_ORDER * sizeof(float));
        A[i * (SMPL_LPC_ORDER + 1)] = 1.0f;
        memcpy(lsfs + i * SMPL_LPC_ORDER, plc->lsf, SMPL_LPC_ORDER * sizeof(float));

        // Lower pitch gradually
        plc->precise_lag = SMPL_min(plc->precise_lag * powf(SMPL_PLC_LAG_DRIFT, subfrlen_comp), SMPL_MAX_PITCH_LAG);
        float lag_rounded = smpl_round_to(plc->precise_lag, 0.5f, 0.0f);
        for (int j = 0; j < subfrlen / SMPL_LAG_SUBFRLEN; j++) {
            lags[i * (subfrlen / SMPL_LAG_SUBFRLEN) + j] = lag_rounded;
        }

        // Find candidate ACB gain in pitch lag
        if (plc->voiced) {
            smpl_assert(plc->precise_lag <= SMPL_MAX_PITCH_LAG);
            smpl_assert(plc->precise_lag >= SMPL_MIN_PITCH_LAG);

            int precise_lag_span = ceilf(plc->precise_lag / (float)subfrlen);
            smpl_assert(precise_lag_span <= len_buffers);
            int highest_nrg_idx = 0;
            float highest_nrg = -1.f;
            for (int j = len_buffers - precise_lag_span; j < len_buffers; j++) {
                if (plc->exc_nrg_buf[j] > highest_nrg) {
                    highest_nrg = plc->exc_nrg_buf[j];
                    highest_nrg_idx = j;
                }
            }
            acb_gains[i * SMPL_ACBG_M] = plc->acb_buf[highest_nrg_idx];
            float late_atten = SMPL_PLC_LATE_ATTEN_SCALE * smpl_sigmoid((-plc->loss_count_subfr + SMPL_PLC_LATE_ATTEN_OFFSET) * SMPL_PLC_LATE_ATTEN_SHAPE) + (1.0f - SMPL_PLC_LATE_ATTEN_SCALE);
            acb_gains[i * SMPL_ACBG_M] *= powf(late_atten, subfrlen_comp);
            if (plc->loss_count_subfr <= SMPL_PLC_EARLY_ATTEN_MS * 16 / subfrlen) {
                float alpha = plc->loss_count_subfr * subfrlen / (SMPL_PLC_EARLY_ATTEN_MS * 16.0f);
                alpha = powf(alpha, SMPL_PLC_EARLY_ATTEN_POW);
                acb_gains[i * SMPL_ACBG_M] = powf(SMPL_PLC_EARLY_ATTEN_GAIN, subfrlen_comp) * (1 - alpha) + acb_gains[i * SMPL_ACBG_M] * alpha;
            }
            for (int j = 1; j < SMPL_ACBG_M; j++) {
                acb_gains[i * SMPL_ACBG_M + j] = 0.0f;
            }
        }
    }
}

void smpl_plc_conceal_hb(PLC* plc, float* A_hb, float* hb_gains, int num_hb_subframes) {
    for (int i = 0; i < num_hb_subframes; i++) {
        plc->hb_gain_attenuation *= SMPL_PLC_HB_ATTEN;
        hb_gains[i] = plc->hb_gain * plc->hb_gain_attenuation;
        memcpy(A_hb + i * (SMPL_HB_LPC_ORDER + 1) + 1, plc->A_hb + 1, SMPL_HB_LPC_ORDER * sizeof(float));
        A_hb[i * (SMPL_HB_LPC_ORDER + 1)] = 1.0f;
    }
}

void smpl_plc_decay_exc(PLC* plc, float* res_lpc, int subfrlen, int reset, int voiced) {
    if (reset) {
        plc->exc_attenuation = 1.0f;
        return;
    }

    if (voiced) {
        // generate noise
        float tgt_nrg = smpl_nrg(res_lpc, subfrlen);
        float noise[SMPL_MAX_SF_LEN];
        smpl_gen_rand_pulses(noise, subfrlen, &plc->rand_seed);

        // get apply envelope
        float scratch[SMPL_MAX_SF_LEN];
        smpl_get_env(res_lpc, subfrlen, SMPL_PLC_INJECT_SMTH, &plc->smth_state, scratch);
        smpl_mul_vec_inplace(noise, scratch, subfrlen);

        // hp and rescale
        float temp_state[1] = {0.0f};
        smpl_filt_ma1(scratch, subfrlen, smpl_plc_inject_coef, 2, temp_state, 1, noise);
        float nrg_ratio = sqrtf(tgt_nrg / (smpl_nrg(noise, subfrlen) + 1.0e-30f));
        smpl_scale_vec_inplace(noise, subfrlen, nrg_ratio * SMPL_PLC_INJECT_GAIN);

        smpl_add_vec_inplace(noise, res_lpc, subfrlen);
    } else {
        plc->exc_attenuation *= subfrlen == SMPL_MIN_SF_LEN ? SMPL_PLC_EXC_ATTEN : SMPL_PLC_EXC_ATTEN * SMPL_PLC_EXC_ATTEN;
        smpl_scale_vec_inplace(res_lpc, subfrlen, plc->exc_attenuation);
    }
}

void smpl_plc_update_nrg(PLC* plc, float res_nrg, int subfrlen) {
    memmove(plc->exc_nrg_buf, plc->exc_nrg_buf + 1, (SMPL_MAX_PITCH_LAG / subfrlen - 1) * sizeof(float));
    plc->exc_nrg_buf[SMPL_MAX_PITCH_LAG / subfrlen - 1] = res_nrg;
}

void smpl_plc_blend_ltp(const PLC* plc, float* acb_state, float lag) {
    if (!plc->voiced) {
        return;
    }
    int    acb_state_len = 2 * SMPL_MAX_PITCH_LAG + SMPL_LTP_INTERPOL_DELAY;
    float* acb_end       = acb_state + acb_state_len;

    float* pitch_cycle_1 = acb_end - (int)ceilf(lag);
    float  pitch_cycle_2[SMPL_MAX_PITCH_LAG];
    int double_lag = (int)(2.0f * lag);
    if (floorf(lag) == lag) {
        memcpy(pitch_cycle_2, acb_end - double_lag, (int)lag * sizeof(float));
    } else { // fractional lag
        smpl_assert(floorf(2 * lag) == 2.0f * lag);
        smpl_interpol(acb_end - double_lag + 1 - SMPL_LTP_INTERPOL_DELAY, pitch_cycle_2, (int)ceilf(lag));
    }

    float lookback = 2.0f * lag / SMPL_LAG_SUBFRLEN;
    float lag_instability = 0.0f;
    int lag_buf_len = 2 * SMPL_MAX_PITCH_LAG / SMPL_LAG_SUBFRLEN;
    for (int i = 0; i < (int)floorf(lookback); i++) {
        lag_instability += SMPL_abs(plc->lag_buf[lag_buf_len - 1 - i] - lag);
    }
    lag_instability += SMPL_abs(plc->lag_buf[lag_buf_len - (int)ceilf(lookback)] - lag) * (lookback - floorf(lookback));

    float ltp_blend_coef = smpl_sigmoid(SMPL_PLC_BLEND_SHAPE * (lag_instability - SMPL_PLC_BLEND_OFFSET)) / 2.0f;
    for (int i = 0; i < (int)ceilf(lag); i++) {
        acb_state[acb_state_len - (int)ceilf(lag) + i] = ltp_blend_coef * pitch_cycle_2[i] + (1.0f - ltp_blend_coef) * pitch_cycle_1[i];
    }
}

void smpl_update_loss_info(PLC* plc, int lostFlag, int packet_len_ms) {
    if (lostFlag == SMPL_FLAG_PACKET_LOST && plc->toc.SID != SMPL_TRUE) {
        plc->loss_len_ms += packet_len_ms;
    } else {
        plc->loss_len_ms = 0;
    }
}

// Adapt more to the new, received LSFs based on previous loss length
void smpl_plc_adapt_lsf(const PLC* plc, float* lsf_prev, int lpc_order) {
    if (plc->loss_len_ms > 0) {
        // If comfort noise is high, adapt from CNG LSFs rather than PLC's
        if (plc->comf_sig_ratio > SMPL_COMFORT_SIGNAL_THRESHOLD) {
            for (int i = 0; i < lpc_order; i++) {
                lsf_prev[i] = plc->cng.cng_candidates[plc->cng.best_candidate_idx].lsfs_lb[i];
            }
        } else {
            memcpy(lsf_prev, plc->lsf, lpc_order * sizeof(float));
        }
    }
}

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
    const float* hb_exc_gains)
{
    if (lb_params->voiced) {
        plc->cng.state_emph[0] = y[num_fcb_subframes * subfrlen - 1];
        return;
    }

    int subfr_idx = 0;
    float lowest_nrg_subfr = 1e30f;
    float nrg_frame  = 0.0f;
    float y_emph[SMPL_MAX_SF_LEN];
    float nyq_comp = 3.61f; // emph filt is [1, -0.9]
    for (int i = 0; i < num_fcb_subframes; i++) {
        float nrg_subfr;
        smpl_filt_ma1(y + i * subfrlen, subfrlen, smpl_cng_emph_coef, 2, plc->cng.state_emph, 1, y_emph);
        nrg_subfr = smpl_nrg(y_emph, subfrlen);
        if (swb) {
            nrg_subfr += nyq_comp * smpl_nrg(y_hb + i * subfrlen, subfrlen);
        }
        nrg_frame += nrg_subfr;
        if (nrg_subfr < lowest_nrg_subfr) {
            subfr_idx = i;
            lowest_nrg_subfr = nrg_subfr;
        }
    }
    smpl_assert(subfr_idx >= 0);

    if (!vad) {
        // record new candidate
        int tail = plc->cng.cng_candidates_tail;
        plc->cng.cng_candidates[tail].frame_nrg = nrg_frame;
        plc->cng.cng_candidates[tail].nrg_bands[SMPL_LB] = lb_params->nrgres[subfr_idx] / (float)subfrlen;
        memcpy(plc->cng.cng_candidates[tail].lsfs_lb, lsfs + SMPL_LPC_ORDER * subfr_idx, SMPL_LPC_ORDER * sizeof(float));
        memcpy(plc->cng.cng_candidates[tail].A_hb, A_hb + (SMPL_HB_LPC_ORDER + 1) * subfr_idx, (SMPL_HB_LPC_ORDER + 1) * sizeof(float));
        int len = subfrlen / SMPL_HB_SF_LEN;
        plc->cng.cng_candidates[tail].nrg_bands[SMPL_HB] = smpl_sum_vec(hb_exc_gains + len * subfr_idx, len) / (float)len;
        tail = (tail + 1) % SMPL_CNG_NO_CANDIDATES;
        plc->cng.cng_candidates_tail = tail;
        plc->cng.num_candidates = SMPL_min(plc->cng.num_candidates + 1, SMPL_CNG_NO_CANDIDATES);

        // pick best candidate
        float candidate_lowest_nrg = 1e30f;
        int best_candidate_idx = 0;
        for (int i = 0; i < plc->cng.num_candidates; i++) {
            if (plc->cng.cng_candidates[i].frame_nrg < candidate_lowest_nrg) {
                candidate_lowest_nrg = plc->cng.cng_candidates[i].frame_nrg;
                best_candidate_idx = i;
            }
        }

        plc->cng.best_candidate_idx = best_candidate_idx;
    }
}

void smpl_add_comfort_noise(PLC* plc, float* y, int y_len, int lostFlag, int band) {
    smpl_assert(y_len <= SMPL_MAX_WB_PACKET_SIZE_IN_SAMPLES);

    int is_PLC_frame = lostFlag == SMPL_FLAG_PACKET_LOST && plc->toc.SID != SMPL_TRUE;

    // generate noise
    if (is_PLC_frame || plc->is_PLC_frame_prev) {
        const CngModel* best_candidate = &plc->cng.cng_candidates[plc->cng.best_candidate_idx];
        float noise_[SMPL_MAX_WB_PACKET_SIZE_IN_SAMPLES + SMPL_LPC_ORDER];
        float *noise = noise_ + SMPL_LPC_ORDER;
        if (is_PLC_frame) {
            float A[SMPL_LPC_ORDER + 1];
            if (band == SMPL_LB) {
                smpl_NLSF2A_stabilize(A, best_candidate->lsfs_lb, SMPL_LPC_ORDER);
            } else {
                memcpy(A + 1, best_candidate->A_hb + 1, SMPL_HB_LPC_ORDER * sizeof(float));
                A[0] = 1.0f;
            }
            float scale = sqrtf(best_candidate->nrg_bands[band] + 1e-30f);
            smpl_gen_rand_pulses(noise, y_len, &plc->cng.rand_seed);
            smpl_scale_vec_inplace(noise, y_len, scale);
            if (band == SMPL_LB) {
                memcpy(noise - SMPL_LPC_ORDER, plc->cng.state_lb, SMPL_LPC_ORDER * sizeof(float));
                smpl_filt_ar16(noise, y_len, A, noise);
                memcpy(plc->cng.state_lb, noise + y_len - SMPL_LPC_ORDER, SMPL_LPC_ORDER * sizeof(float));
            } else {
                memcpy(noise - SMPL_HB_LPC_ORDER, plc->cng.state_hb, SMPL_HB_LPC_ORDER * sizeof(float));
                smpl_filt_ar4(noise, y_len, A, noise);
                memcpy(plc->cng.state_hb, noise + y_len - SMPL_HB_LPC_ORDER, SMPL_HB_LPC_ORDER * sizeof(float));
            }
        } else {
            memset(noise, 0, y_len * sizeof(float));
        }

        // delay noise to sync with output signal
        float* delay_buf = band == SMPL_LB ? plc->cng.lb_delay_buf : plc->cng.hb_delay_buf;
        float noise_out[SMPL_MAX_WB_PACKET_SIZE_IN_SAMPLES];
        memcpy(noise_out, delay_buf, SMPL_TOT_POSTFILT_DELAY * sizeof(float));
        memcpy(noise_out + SMPL_TOT_POSTFILT_DELAY, noise, (y_len - SMPL_TOT_POSTFILT_DELAY) * sizeof(float));
        memcpy(delay_buf, noise + y_len - SMPL_TOT_POSTFILT_DELAY, SMPL_TOT_POSTFILT_DELAY * sizeof(float));

        // fade in at start of packet loss
        if (plc->loss_len_ms == 0) {
            for (int i = 0; i < SMPL_TOT_POSTFILT_DELAY; i++) {
                noise_out[SMPL_TOT_POSTFILT_DELAY + i] *= (float)i * (1.0f / SMPL_TOT_POSTFILT_DELAY);
            }
        }

        // fade out before recovering from packet loss
        if (plc->loss_len_ms > 0 && (lostFlag != SMPL_FLAG_PACKET_LOST || plc->toc.SID == SMPL_TRUE)) {
            for (int i = 0; i < SMPL_TOT_POSTFILT_DELAY; i++) {
                noise_out[i] *= 1.0f - (float)i * (1.0f / SMPL_TOT_POSTFILT_DELAY);
            }
        }

        if (band == SMPL_LB) {
            // track energy ratio between comfort noise and signal for transitioning
            plc->comf_sig_ratio = smpl_nrg(noise_out, y_len) / (smpl_nrg(y, y_len) + 1e-12f);
        }

        // add noise
        smpl_add_vec_inplace(noise_out, y, y_len);
    } else {
        if (band == SMPL_LB) {
            // track energy ratio between comfort noise and signal for transitioning
            plc->comf_sig_ratio = 0.0f;
        }
    }

    plc->is_PLC_frame_prev = is_PLC_frame;
}

void smpl_plc_init(PLC* plc) {
    // initialize CNG with a low-level candidate to start
    plc->cng.cng_candidates[0].frame_nrg = 1e-8f;
    plc->toc.fs_Hz = 16000;
    plc->toc.packet_len_ms = 20;

    plc->cng.cng_candidates[0].nrg_bands[SMPL_LB] = 1e-10f;
    for (int i = 0; i < SMPL_LPC_ORDER; i++) {
        plc->cng.cng_candidates[0].lsfs_lb[i] = smpl_plc_cng_init[i];
    }

    plc->cng.best_candidate_idx = 0;
    plc->cng.cng_candidates_tail = 1;
    plc->cng.num_candidates = 1;

    // Make sure we have low (silent) residual energy level for unvoiced
    plc->last_nrgres_Q14 = -90 * ((int32_t)1 << 14);
}

void smpl_update_recovery_info(PLC* plc, const LbQuantParams* lb_params) {
    memcpy(plc->A_last, plc->A, (SMPL_LPC_ORDER + 1) * sizeof(float));
    plc->voiced_last = lb_params->voiced;
    plc->recovery_len_ms = 0;
}

// Assumed to be called when current packet (with lb_params) is not lost
#define SMPL_PLC_IMP_LEN 12
void smpl_plc_bwe_recover(PLC* plc, const LbQuantParams* lb_params, float* A, int num_subframes, int framelen_ms) {
    if (!(lb_params->voiced && plc->voiced_last)) { // only for V -> V
        return;
    }

    int lost_previous = plc->loss_len_ms > 0;
    int recovery_cont = (plc->recovery_len_ms > 0) && (plc->recovery_len_ms < SMPL_RECOVER_LEN_MS);
    if (lost_previous) { // only do this measurement once per packet loss
        float impulse_[SMPL_PLC_IMP_LEN + SMPL_LPC_ORDER];
		memset(impulse_, 0, (SMPL_PLC_IMP_LEN + SMPL_LPC_ORDER) * sizeof(float));
		float* impulse = impulse_ + SMPL_LPC_ORDER;
		impulse[0] = 1.0f;
        smpl_filt_ar16(impulse, SMPL_PLC_IMP_LEN, plc->A_last, impulse);
        plc->nrg_last = smpl_nrg(impulse, SMPL_PLC_IMP_LEN);
    }

    if (lost_previous || recovery_cont) {
        // estimate energy for end of frame
        float impulse_[SMPL_PLC_IMP_LEN + SMPL_LPC_ORDER];
		memset(impulse_, 0, (SMPL_PLC_IMP_LEN + SMPL_LPC_ORDER) * sizeof(float));
		float* impulse = impulse_ + SMPL_LPC_ORDER;
		impulse[0] = 1.0f;
        float* A_last = A + (num_subframes - 1) * (SMPL_LPC_ORDER + 1);
        smpl_filt_ar16(impulse, SMPL_PLC_IMP_LEN, A_last, impulse);
		float post_gain = smpl_nrg(impulse, SMPL_PLC_IMP_LEN);

        // apply bwe
        float log_ratio = 0.5f * log10f(post_gain / (plc->nrg_last + 1e-12f));
        float bwe = 1.0f - (1.0f - SMPL_RECOVER_MIN_BWE) * SMPL_max(SMPL_min(log_ratio, 1.0f), 0.0f);
        for (int num_subframe = 0; num_subframe < num_subframes; num_subframe++) {
            smpl_bwe_expand(A + num_subframe * (SMPL_LPC_ORDER + 1), SMPL_LPC_ORDER, bwe);
        }

        plc->recovery_len_ms += framelen_ms;
    }
}
