#include "smpl_defines.h"
#include "smpl_bitrate_controller.h"
#include "smpl_codec_util.h"
#include "smpl_errors.h"
#include "smpl_typedef.h"
#include <math.h>

static inline float nonflat(const float x[], int L)
{
    float sumx = 0.0f;
    for (int n = 0; n < L; n++) {
        sumx += x[n];
    }
    float sumx_sq = sumx * sumx;
    if (sumx_sq <= 0.0f) {
        return -1.0f;
    }
    float nonflat = (L * smpl_nrg(x, L) / (sumx_sq)) - 1.0f;
    return nonflat;
}

float smpl_get_nonflatness(const float res_lpc[], int L, float wlsf[SMPL_LPC_ORDER], float state[SMPL_NON_FLAT_STATE_LEN])
{
    float nrgs[SMPL_NON_FLAT_NRGS_LEN];
    memset(nrgs, 0, sizeof(nrgs));
    int N = L / SMPL_NON_FLAT_SUBFR_LEN;
    smpl_assert(N >= SMPL_NON_FLAT_STATE_LEN);
    for (int n = 0; n < N; n++) {
        nrgs[n + SMPL_NON_FLAT_STATE_LEN] = smpl_nrg(&res_lpc[n * SMPL_NON_FLAT_SUBFR_LEN], SMPL_NON_FLAT_SUBFR_LEN) + SMPL_NON_FLAT_SUBFR_LEN * 2e-10f;
    }
    float sum_state = 0.0f;    
    float sum_nrgs = 0.0f;
    for (int n = 0; n < SMPL_NON_FLAT_STATE_LEN; n++) {
        sum_state += state[n];
        sum_nrgs += nrgs[n + SMPL_NON_FLAT_STATE_LEN];
    }

    if (sum_state < sum_nrgs) {
        memcpy(nrgs, state, SMPL_NON_FLAT_STATE_LEN * sizeof(float));
        N += SMPL_NON_FLAT_STATE_LEN;
    }
    memcpy(state, &nrgs[L / SMPL_NON_FLAT_SUBFR_LEN], SMPL_NON_FLAT_STATE_LEN * sizeof(float));
    
    return (nonflat(&nrgs[(L / SMPL_NON_FLAT_SUBFR_LEN) + SMPL_NON_FLAT_STATE_LEN - N], N) + 0.05f * nonflat(wlsf, SMPL_LPC_ORDER));
}

float smpl_get_hr_nonflat_thres(int bitrate, float sp_act_prob) {
    float bitrates[2] = { 10000.0f, 18000.0f };
    float thresholds[2] = { 0.5f, 0.0f };
    float a = (thresholds[1] - thresholds[0]) / (bitrates[1] - bitrates[0]);
    float b = thresholds[0] - a * bitrates[0];
#ifdef SMPL_UV_NONFLATNESS_SA
    smpl_assert(sp_act_prob >= 0.0f);
    bitrate *= sqrtf(sp_act_prob + 1e-12f); // use sqrt to slow down effect
#endif
    float thres = SMPL_min(SMPL_max(a * bitrate + b, 0.0f), SMPL_UV_NONFLATNESS_THR);
    return thres;
}

float bitrate2pulses(float rate_kbps, const float* coeff) {
    return coeff[0] +
           coeff[1] * rate_kbps +
           coeff[2] * rate_kbps * rate_kbps +
           coeff[3] * powf(rate_kbps, 3.0f) +
           coeff[4] * powf(rate_kbps, 4.0f) +
           coeff[5] * powf(SMPL_E, (rate_kbps - coeff[6]) * coeff[7]);
}

static float bitrate2pulses_hr_fec(float rate_kbps, const float* coeff, float one_pulse_rate_bps) {
#define RATE_THRES_KBPS 9.0f // Only compensate below this value
    if (rate_kbps >= RATE_THRES_KBPS) {
        return bitrate2pulses(rate_kbps, coeff);
    }
    else if (one_pulse_rate_bps >= RATE_THRES_KBPS * 1000.0f) {
        return 1.0f;
    }
    else {
        // interpolate between rate with one pulse and pulses @RATE_THRES_KBPS
        float pulsesThres = bitrate2pulses(RATE_THRES_KBPS, coeff);
        float sc = (RATE_THRES_KBPS - rate_kbps) / (RATE_THRES_KBPS - one_pulse_rate_bps / 1000.0f);
        return pulsesThres - sc * (pulsesThres - 1.0f);
    }
}

void bitrate_controller_init(BitrateController* rateCtrl)
{
    for (int r = 0; r < SMPL_CELP_MAX_RATES; r++) {
        rateCtrl->adjustment_factor[r] = 1.0f;
    }
}

void bitrate_controller(
    BitrateController* rateCtrl,
    const smpl_EncControlStruct* enc_status,
    const smpl_dtx_status* dtx,
    int coded_as_active_voice,
    float sp_act_prob,
    float nonflatness,
    float voicing_strength,
    int voiced,
    float wnrg,
    float wnrg_next,
    int low_rate,
    int framelen,
    int subfrlen,
    int16_t max_pulses_per_subfr[SMPL_CELP_MAX_RATES],
    float subfr_importance[SMPL_CELP_MAX_RATES]) {

    smpl_assert(sp_act_prob >= 0.0f && sp_act_prob <= 1.0f);
    
    int bwe_bitrate = 0;
    if (enc_status->internalSampleRate > 16000) {
        bwe_bitrate += low_rate ? 450 : 750;
        bwe_bitrate += enc_status->payloadSize_ms == 10 ? 450 : 0;
    }

    rateCtrl->rate_cont_wnrg_smth += 0.6f * (wnrg - rateCtrl->rate_cont_wnrg_smth);

    // Use the model to get target pulses
    int framelen_idx = (enc_status->payloadSize_ms == 10) ? 0 : 
                        enc_status->payloadSize_ms == 20 ? 1 : 
                        enc_status->payloadSize_ms == 60 ? 2 : 3;
    memset(max_pulses_per_subfr, 0, SMPL_CELP_MAX_RATES * sizeof(int16_t));
    memset(subfr_importance, 0, SMPL_CELP_MAX_RATES * sizeof(float));
    int start_r = SMPL_CELP_IDX_FEC + (enc_status->fecBitRate == 0) || (enc_status->fecBitRate == enc_status->mainBitRate);
    for (int r = start_r; r <= SMPL_CELP_IDX_MAIN; r++) {
        float bitRate = (r == SMPL_CELP_IDX_FEC) ? (float)enc_status->fecBitRate : (float)enc_status->mainBitRate;
        bitRate = SMPL_min(bitRate, 30000.0f); // ensure don't extrapolate pulses_per_20ms_target_max curves
        float rate_kbps = (bitRate - bwe_bitrate) / 1000.0f;
        if (!low_rate) { // rescale bitrate to get different complexities closer to target
            rate_kbps *= enc_status->complexity == 1 ? 0.9900990f :
                         enc_status->complexity == 2 ? 0.9900990f :
                         enc_status->complexity == 3 ? 1.0101010f :
                         enc_status->complexity == 4 ? 1.0101010f :
                         1.0f; // complexity 5+
        }
        float pulses_per_20ms_target_max;
        const float rate_control_thrs = smpl_rate_control_thrs_comp5[framelen_idx][low_rate ? 0 : 1];
        if (bitRate - bwe_bitrate < rate_control_thrs) {
            pulses_per_20ms_target_max = 1.0f;
        }
        else {
            if ((r == SMPL_CELP_IDX_FEC) && !low_rate && enc_status->useFecRateCompensation){ // Add Gate option
                pulses_per_20ms_target_max = SMPL_max(bitrate2pulses_hr_fec(rate_kbps, smpl_rate_control_model_comp5[framelen_idx][low_rate ? 0 : 1], rate_control_thrs), 1.0f);
            }
            else {
                pulses_per_20ms_target_max = SMPL_max(bitrate2pulses(rate_kbps, smpl_rate_control_model_comp5[framelen_idx][low_rate ? 0 : 1]), 1.0f);
            }
        }

        float rel_pulserate = pulses_per_20ms_target_max / 16.0f * (320.0f / framelen);
        smpl_assert(rel_pulserate > 0.0f);
        float rel_pulserate_log = logf(rel_pulserate);
        if (rateCtrl->rate_cont_bitrate[r] != bitRate) {
            float bitrate_scale = SMPL_RATE_CONT_SCALE * rel_pulserate * (1 + 0.4f * rel_pulserate_log * rel_pulserate_log);
            rateCtrl->rate_cont_bitrate_scale[r] = bitrate_scale;
            rateCtrl->rate_cont_bitrate[r] = bitRate;
        }
        
        int numsubfrs = framelen / subfrlen;
        max_pulses_per_subfr[r] = 1 + (int)roundf(pulses_per_20ms_target_max * (1 + 0.5f) / numsubfrs);
        if (enc_status->useDTX && dtx->sid_frame) {
            max_pulses_per_subfr[r] = 0;
        }
        else {
            max_pulses_per_subfr[r] = (int)roundf(max_pulses_per_subfr[r] * (0.5f + 0.5f * sqrtf(sp_act_prob + 1e-12f)));
            SmplFrameTypes frame_type = !coded_as_active_voice ? BACKGROUND_NOISE : (voiced == 1) ? VOICED : UNVOICED;
            int max_pulses = smpl_max_pulses_per_frame[low_rate][frame_type] * framelen / 320;
            max_pulses_per_subfr[r] = SMPL_min(max_pulses_per_subfr[r], max_pulses / numsubfrs); // Ensure it doesn't overshoot the PDF            
        }
        smpl_assert(max_pulses_per_subfr[r] <= SMPL_MAX_PULSES_PER_SF);
        subfr_importance[r] = (wnrg + 0.01f * wnrg_next) / (rateCtrl->rate_cont_wnrg_smth + 0.02f * wnrg_next + 1e-12f);
        if (voiced) {
            if (bitRate <= 9000) {
                subfr_importance[r] = sqrtf(subfr_importance[r] + 1e-12f);
            }
        }
        else {
            subfr_importance[r] *= 0.9f + 0.3f * smpl_sigmoid(nonflatness - 2.0f);
            subfr_importance[r] *= 0.8f;
        }
        if (voiced != rateCtrl->prev_voiced){
            subfr_importance[r] *= 1.1f;
        }
        subfr_importance[r] *= 0.9f + 0.3f * 1.0f / (1.0f + 25.0f * voicing_strength * voicing_strength);
        
        // META: We are experimenting with lowering the bitrate for non active frames and there are a couple
        // of different ways to adjust the bitrate allocation based on speech activity:
        // 1. sqrtf(): Least aggressive and it allocates sufficient bits even for very small speech activity as well
        // 2. direct: Bitrate is directly proportional to the speech activity probability
        // 3. square(): Bitrate is aggressively dropped as active speech probability reduces
        float imp_factor = enc_status->subFrameImportanceFactor;
        if (imp_factor <= 1.0) {
            subfr_importance[r] *= (1 - imp_factor) + imp_factor * sqrtf(sp_act_prob + 1e-12f);
        }
        else if (imp_factor <= 2.0) {
            imp_factor -= 1;
            subfr_importance[r] *= (1 - imp_factor) + imp_factor * sp_act_prob;
        }
        else {
            smpl_assert(imp_factor <= 3.0);
            imp_factor -= 2;
            subfr_importance[r] *= (1 - imp_factor) + imp_factor * sp_act_prob * sp_act_prob;
        }
        subfr_importance[r] *= rateCtrl->adjustment_factor[r] * rateCtrl->rate_cont_bitrate_scale[r];
        rateCtrl->prev_voiced = voiced;
    }
}

void bitrate_controller_update_scale(
    BitrateController* rateCtrl,
    smpl_EncControlStruct* enc_status,
    int frame_ms,
    int frames_per_packet,
    const float bits_used[SMPL_CELP_MAX_RATES],
    int coded_as_active_voice) {

    int start_r = SMPL_CELP_IDX_FEC + (enc_status->fecBitRate == 0) || (enc_status->fecBitRate == enc_status->mainBitRate);
    float external_bits = 8.0f / (float)frames_per_packet / (SMPL_CELP_MAX_RATES - start_r); // TOC bits
    external_bits += 4.5f / (float)frames_per_packet / (SMPL_CELP_MAX_RATES - start_r); // Last bits in payload round up to byte (avg)
    for (int r = start_r; r <= SMPL_CELP_IDX_MAIN; r++) {
        if (!coded_as_active_voice) { // decay bitrate scale back to neutral state
            float smth_coef = 1.0f - (float)frame_ms * 0.00125f;
            rateCtrl->bitrate_delta_smth[r] *= smth_coef;
        }
        else {
            float bitRate = (r == SMPL_CELP_IDX_FEC) ? (float)enc_status->fecBitRate : (float)enc_status->mainBitRate;
            // calculate instant bitrate (split external bits between main and FEC)
            float measured_bitrate = (bits_used[r] + external_bits) * (1000.0f / (float)frame_ms);

            // integrate actual - target bitrates
            float measured_bitrate_delta = (measured_bitrate - bitRate) / bitRate;
            rateCtrl->bitrate_delta_smth[r] += measured_bitrate_delta * SMPL_RATE_CONT_GAIN * frame_ms / 20.0f;
            rateCtrl->bitrate_delta_smth[r] = SMPL_max(SMPL_min(rateCtrl->bitrate_delta_smth[r], SMPL_RATE_CONT_CLAMP_MAX), SMPL_RATE_CONT_CLAMP_MIN);
            rateCtrl->adjustment_factor[r] = SMPL_max(1.0f - rateCtrl->bitrate_delta_smth[r], 0.0f);
        }
    }
}