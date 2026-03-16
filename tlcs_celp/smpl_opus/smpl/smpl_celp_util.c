
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "smpl_celp.h"
#include <math.h>
#include "smpl_codec_util.h"
#include "smpl_filt.h"
#include "smpl_lpc.h"
#include "smpl_tables.h"
#include "smpl_typedef.h"
#include <stdio.h>
#include "silk/debug.h"

static int check_if_better_deldec(CelpScratch *pScratch, FCB *fcb, FCB *best_fcb, FCBstate *best_fcb_state, float *nrg_thr, float wnrg_per_pulse)
{
    *nrg_thr += wnrg_per_pulse;
    if(fcb->wnrg > *nrg_thr){
        *nrg_thr = fcb->wnrg;
        memcpy(best_fcb, fcb, sizeof(*fcb));
        memcpy(best_fcb_state, &pScratch->fcb_states[pScratch->read_idx][fcb->fcb_state_idx], sizeof(*best_fcb_state));
    }
    return SMPL_TRUE;
}

static void update_read_write_idx(CelpScratch* pScratch)
{
    int tmp = pScratch->write_idx;
    pScratch->write_idx = pScratch->read_idx;
    pScratch->read_idx = tmp;
}

static int is_unique(CelpScratch * pScratch, uint64_t sgntr)
{
    for(int i = 0; i < pScratch->unique_sgntr_size; i++){
        if(pScratch->unique_sgntr[i] == sgntr){
            return SMPL_FALSE;
        }
    }
    return SMPL_TRUE;
}

static inline const float* get_PhiCol(CelpEncoder* pSt, int col, int non_zero_range[2])
{
    smpl_assert(pSt->fcb_subfrlen > pSt->perc_resp_len);
    non_zero_range[0] = SMPL_max(col - pSt->perc_resp_len + 1, 0);              // inclusive
    non_zero_range[1] = SMPL_min(col + pSt->perc_resp_len, pSt->fcb_subfrlen);  // exclusive
    return &pSt->scratchMem->PhiFlip[SMPL_MAX_SF_LEN - col];
}

static void calc_d_abs_and_sign(const float d[], int L, float d_abs[], float d_sign[]) {
    for (int i = 0; i < L; i++) {
        if (d[i] > 0.0f) {
            d_abs[i] = d[i];
            d_sign[i] = 1.0f;
        }
        else {
            d_abs[i] = -d[i];
            d_sign[i] = -1.0f;
        }
    }
}

static void add_pulse(CelpEncoder *pSt, FCB *fcb, const float d_abs[], const float d_sign[], int numsurv, int idx, int lag, float pitch_sharp)
{
    CelpScratch* pScratch = pSt->scratchMem;
    FCBstate *fcb_state_w = &pScratch->fcb_states[pScratch->write_idx][idx]; // Write into this buffer
    const FCBstate *fcb_state_r = &pScratch->fcb_states[pScratch->read_idx][fcb->fcb_state_idx]; // read from this buffer

    for(int i = 0; i < pSt->fcb_subfrlen; i++){
        fcb_state_w->num[i] = fcb_state_r->num[i]  + d_abs[fcb->pos_new];
    }
    memcpy(fcb_state_w->den, fcb_state_r->den, pSt->fcb_subfrlen * sizeof(float));
    if(pitch_sharp == 0.0f){
        int non_zero_range[2];
        const float* PhiCol = get_PhiCol(pSt, fcb->pos_new, non_zero_range);
        float d_den = 0.0f;
        for(int i = 0; i < fcb->n_pulses; i++){
            d_den += PhiCol[fcb_state_r->pulse_positions[i]] * fcb_state_r->pulse_signs[i];
        }
        d_den *= 2.0f * fcb->sign_new;
        d_den += PhiCol[fcb->pos_new];
        for (int i = 0; i < pSt->fcb_subfrlen; i++) {
            fcb_state_w->den[i] += d_den;
        }
        for(int i = non_zero_range[0]; i < non_zero_range[1]; i++){
            fcb_state_w->den[i] += (2.0f * fcb->sign_new * d_sign[i] * PhiCol[i]);
        }
    }else{
        int non_zero_range[2];
        // Loop over pitch sharped new pulses
        float g1 = 1.0f;
        float d_den = 0.0f;
        for (int pos = fcb->pos_new; pos < pSt->fcb_subfrlen; pos += lag) {
            const float* PhiCol = get_PhiCol(pSt, pos, non_zero_range);
            for (int i = 0; i < fcb->n_pulses; i++) {
                // loop over all previous
                float g2 = g1;
                for (int pos_ = fcb_state_r->pulse_positions[i]; pos_ < pSt->fcb_subfrlen; pos_ += lag) {
                    d_den += g2 * PhiCol[pos_] * fcb_state_r->pulse_signs[i];
                    g2 *= pitch_sharp;
                }
            }
            g1 *= pitch_sharp;
        }
        d_den *= 2.0f * fcb->sign_new;
        g1 = 1.0f;
        for (int pos1 = fcb->pos_new; pos1 < pSt->fcb_subfrlen; pos1 += lag) {
            const float* PhiCol = get_PhiCol(pSt, pos1, non_zero_range);
            float g2 = g1;
            for (int pos2 = fcb->pos_new; pos2 < pSt->fcb_subfrlen; pos2 += lag){
                d_den += g2 * PhiCol[pos2];
                g2 *= pitch_sharp;
            }
            g1 *= pitch_sharp;
        }
        for (int i = 0; i < pSt->fcb_subfrlen; i++) {
            fcb_state_w->den[i] += d_den;
        }
        float dd_den[SMPL_MAX_SF_LEN];
        memset(dd_den, 0, pSt->fcb_subfrlen * sizeof(float));
        g1 = 1.0f;
        for (int pos = fcb->pos_new; pos < pSt->fcb_subfrlen; pos += lag) {
            const float* PhiCol = get_PhiCol(pSt, pos, non_zero_range);
            float g2 = g1;
            for (int k = 0; k < pSt->fcb_subfrlen; k += lag) {
                int start_i = SMPL_max(0, non_zero_range[0] - k);
                int end_i = SMPL_min(pSt->fcb_subfrlen - k, non_zero_range[1] - k);
                for (int i = start_i; i < end_i; i++) {
                    dd_den[i] += (g2 * PhiCol[i + k]);
                }
                g2 *= pitch_sharp;
            }
            g1 *= pitch_sharp;
        }
        for (int i = 0; i < pSt->fcb_subfrlen; i++) {
            fcb_state_w->den[i] += 2.0f * fcb->sign_new * d_sign[i] * dd_den[i];
        }
    }
    memcpy(fcb_state_w->pulse_positions, fcb_state_r->pulse_positions, fcb->n_pulses * sizeof(int));
    fcb_state_w->pulse_positions[fcb->n_pulses] = fcb->pos_new;
    memcpy(fcb_state_w->pulse_signs, fcb_state_r->pulse_signs, fcb->n_pulses * sizeof(float));
    fcb_state_w->pulse_signs[fcb->n_pulses] = fcb->sign_new;

    fcb->n_pulses++;
    fcb->fcb_state_idx = idx;

    float Q[SMPL_MAX_SF_LEN];
    smpl_celp_q(fcb_state_w->num, fcb_state_w->den, pSt->fcb_subfrlen, Q);
    int sort_ix[SMPL_CELP_MAX_NUMSURV];
    smpl_get_maxi_K(Q, sort_ix, pSt->fcb_subfrlen, numsurv);

    uint64_t fcb_sgntr = fcb->sgntr;
    for(int i = 0; i < numsurv; i++){
        fcb->sgntr = fcb_sgntr + pSt->sgntrs[sort_ix[i]];
        // Check that this sgntr is not in the map of unique signatures
        if(is_unique(pScratch, fcb->sgntr) == SMPL_TRUE){
            fcb->pos_new = sort_ix[i];
            fcb->sign_new = d_sign[sort_ix[i]];
            fcb->wnrg = Q[sort_ix[i]];
            memcpy(&pScratch->fcb_candidates[pScratch->fcb_candidates_size++], fcb, sizeof(*fcb));
            pScratch->unique_sgntr[pScratch->unique_sgntr_size++] = fcb->sgntr;
        }
    }
}

static int check_if_better(float wnrg, float* nrg_thr, float wnrg_per_pulse)
{
    *nrg_thr += wnrg_per_pulse;
    if (wnrg > *nrg_thr) {
        *nrg_thr = wnrg;
        return SMPL_TRUE;
    }
    return SMPL_FALSE;
}

void smpl_fcb_search(
    CelpEncoder* pSt,
    const float d[],
    float wnrg_per_pulse[SMPL_CELP_MAX_RATES],
    int16_t fcb_pulses_max[SMPL_CELP_MAX_RATES],
    int16_t pulses[SMPL_CELP_MAX_RATES][SMPL_MAX_PULSES_PER_SF],
    int16_t n_pulses[SMPL_CELP_MAX_RATES],
    float wnrg[SMPL_CELP_MAX_RATES],
    float gain_from_search[SMPL_CELP_MAX_RATES],
    float fcb_wnrg[SMPL_CELP_MAX_RATES])
{
    smpl_assert(pSt != NULL);
    memset(n_pulses, 0, SMPL_CELP_MAX_RATES * sizeof(int16_t));

    int positions[SMPL_MAX_PULSES_PER_SF];
    float d_abs[SMPL_MAX_SF_LEN], d_sign[SMPL_MAX_SF_LEN];
    float num[SMPL_MAX_SF_LEN], den[SMPL_MAX_SF_LEN];
    const float* Phi = pSt->scratchMem->Phi;
    calc_d_abs_and_sign(d, pSt->fcb_subfrlen, d_abs, d_sign);
    // First Pulse
    for (int i = 0; i < pSt->fcb_subfrlen; i++) {
        den[i] = Phi[0] + 1e-16f;
    }
    memcpy(num, d_abs, pSt->fcb_subfrlen * sizeof(float));
    positions[0] = smpl_get_maxi(num, pSt->fcb_subfrlen);
    float nrg_thr[SMPL_CELP_MAX_RATES];
    memset(nrg_thr, 0, SMPL_CELP_MAX_RATES * sizeof(float));
    float ratio = num[positions[0]] / den[positions[0]];
    float wnrg_ = num[positions[0]] * ratio;
    if (check_if_better(wnrg_, &nrg_thr[SMPL_CELP_IDX_MAIN], wnrg_per_pulse[SMPL_CELP_IDX_MAIN])) {
        n_pulses[SMPL_CELP_IDX_MAIN] = 1;
        wnrg[SMPL_CELP_IDX_MAIN] = wnrg[SMPL_CELP_IDX_FEC] = wnrg_;
        gain_from_search[SMPL_CELP_IDX_MAIN] = gain_from_search[SMPL_CELP_IDX_FEC] = ratio;
        fcb_wnrg[SMPL_CELP_IDX_MAIN] = fcb_wnrg[SMPL_CELP_IDX_FEC] = den[positions[0]];
        if (fcb_pulses_max[SMPL_CELP_IDX_FEC] > 0) {
            n_pulses[SMPL_CELP_IDX_FEC] = n_pulses[SMPL_CELP_IDX_MAIN];
            wnrg[SMPL_CELP_IDX_FEC] = wnrg[SMPL_CELP_IDX_MAIN];
            gain_from_search[SMPL_CELP_IDX_FEC] = gain_from_search[SMPL_CELP_IDX_MAIN];
            fcb_wnrg[SMPL_CELP_IDX_FEC] = fcb_wnrg[SMPL_CELP_IDX_MAIN];
        }
    }

    for (int pulse_nr = 1; pulse_nr < fcb_pulses_max[SMPL_CELP_IDX_MAIN]; pulse_nr++) {
        int position = positions[pulse_nr - 1];
        float sgn = d_sign[position];
        for (int i = 0; i < pSt->fcb_subfrlen; i++) {
            num[i] += d_abs[position];
        }
        int non_zero_range[2];
        const float* PhiCol = get_PhiCol(pSt, position, non_zero_range);
        float d_den = 0.0f;
        for (int i = 0; i < pulse_nr-1; i++) {
            d_den += PhiCol[positions[i]] * d_sign[positions[i]];
        }
        d_den *= 2.0f * sgn;
        d_den += PhiCol[position];
        for (int i = 0; i < pSt->fcb_subfrlen; i++) {
            den[i] += d_den;
        }
        for (int i = non_zero_range[0]; i < non_zero_range[1]; i++) {
            den[i] += (2.0f * sgn * d_sign[i] * PhiCol[i]);
        }
        float Q[SMPL_MAX_SF_LEN];
        smpl_celp_q(num, den, pSt->fcb_subfrlen, Q);
        positions[pulse_nr] = smpl_get_maxi(Q, pSt->fcb_subfrlen);
        if (check_if_better(Q[positions[pulse_nr]], &nrg_thr[SMPL_CELP_IDX_MAIN], wnrg_per_pulse[SMPL_CELP_IDX_MAIN])) {
            n_pulses[SMPL_CELP_IDX_MAIN] = pulse_nr + 1;
            wnrg[SMPL_CELP_IDX_MAIN] = Q[positions[pulse_nr]];
            gain_from_search[SMPL_CELP_IDX_MAIN] = num[positions[pulse_nr]] / den[positions[pulse_nr]];
            fcb_wnrg[SMPL_CELP_IDX_MAIN] = den[positions[pulse_nr]];
        }
        if (fcb_pulses_max[SMPL_CELP_IDX_FEC] >= pulse_nr && check_if_better(Q[positions[pulse_nr]], &nrg_thr[SMPL_CELP_IDX_FEC], wnrg_per_pulse[SMPL_CELP_IDX_FEC])) {
            n_pulses[SMPL_CELP_IDX_FEC] = pulse_nr + 1;
            wnrg[SMPL_CELP_IDX_FEC] = Q[positions[pulse_nr]];
            gain_from_search[SMPL_CELP_IDX_FEC] = num[positions[pulse_nr]] / den[positions[pulse_nr]];
            fcb_wnrg[SMPL_CELP_IDX_FEC] = den[positions[pulse_nr]];
        }
    }
    for (int r = SMPL_CELP_IDX_FEC; r < SMPL_CELP_IDX_MAIN + 1; r++) {
        // Generate pulses vector
        if (nrg_thr[r] > 0.0f) {
            for (int i = 0; i < n_pulses[r]; i++) {
                int position = positions[i];
                pulses[r][i] = d_sign[position] > 0 ? (1 + position) : -(1 + position);
            }
        }
        else {
            wnrg[r] = 0.0f;
            gain_from_search[r] = 0.0f;
            fcb_wnrg[r] = 0.0f;
            n_pulses[r] = 0;
        }
    }
}

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
    float fcb_wnrg[SMPL_CELP_MAX_RATES]
)
{
    smpl_assert(pSt != NULL);
    CelpScratch* pScratch = pSt->scratchMem;
    smpl_assert(pScratch != NULL);
    float d_new[SMPL_MAX_SF_LEN];
    float d_abs[SMPL_MAX_SF_LEN], d_sign[SMPL_MAX_SF_LEN];
    const float* Phi_ptr = pScratch->Phi;
    if(pitch_sharp != 0.0f && lag > 0 && lag < pSt->fcb_subfrlen){
        // H = toeplitz_square(impz)
        // d = H'*d
        memcpy(d_new, d, sizeof(float) * pSt->fcb_subfrlen);
        for(int j = 0; j < pSt->fcb_subfrlen; j++){
            float g = pitch_sharp;
            for(int i = lag + j; i < pSt->fcb_subfrlen; i += lag) {
                d_new[j] += g * d[i];
                g *= pitch_sharp;
            }
        }
        calc_d_abs_and_sign(d_new, pSt->fcb_subfrlen, d_abs, d_sign);
    }else{
        calc_d_abs_and_sign(d, pSt->fcb_subfrlen, d_abs, d_sign);
        pitch_sharp = 0.0f;
    }

    pScratch->read_idx = 0;
    pScratch->write_idx = 1;
    FCB best_fcb[SMPL_CELP_MAX_RATES];
    FCBstate best_fcb_state[SMPL_CELP_MAX_RATES];
    memset(best_fcb, 0, SMPL_CELP_MAX_RATES * sizeof(FCB));
    memset(best_fcb_state, 0, SMPL_CELP_MAX_RATES * sizeof(FCBstate));
    float nrg_thr[SMPL_CELP_MAX_RATES];
    memset(nrg_thr, 0, SMPL_CELP_MAX_RATES *sizeof(float));
    FCBstate *fcb_state = &pScratch->fcb_states[pScratch->write_idx][0];
    memcpy(fcb_state->num, d_abs, pSt->fcb_subfrlen * sizeof(float));
    if(pitch_sharp == 0.0f){
        for(int i = 0; i < pSt->fcb_subfrlen; i++){
            fcb_state->den[i] = Phi_ptr[0] + 1e-16f;
        }
    }else{
        int non_zero_range[2];
        int offset = pSt->fcb_subfrlen - 1;
        for (int i = pSt->fcb_subfrlen-1; i >= 0; i -= lag) {
            float res = 1e-16f;
            float g_1 = 1.0f;
            for (int j = i; j < pSt->fcb_subfrlen; j += lag) {
                const float* phiCol = get_PhiCol(pSt, j, non_zero_range);
                float g_2 = 1.0f;
                for (int k = i; k < pSt->fcb_subfrlen; k += lag) {
                    res += g_1 * g_2 * phiCol[k];
                    g_2 *= pitch_sharp;
                }
                g_1 *= pitch_sharp;
            }
            int len = SMPL_min(lag, offset+1);
            for (int j = 0; j < len; j++){
                fcb_state->den[offset - j] = res;
            }
            offset -= len;
        }
    }

    update_read_write_idx(pScratch);
    float Q[SMPL_MAX_SF_LEN];
    if (pitch_sharp == 0.0f) {
        memcpy(Q, fcb_state->num, pSt->fcb_subfrlen * sizeof(float));
    }else{
        smpl_celp_q(fcb_state->num, fcb_state->den, pSt->fcb_subfrlen, Q);
    }

    int sort_ix[SMPL_CELP_MAX_NUMSURV];
    smpl_get_maxi_K(Q, sort_ix, pSt->fcb_subfrlen, surv[0]);
    FCB fcb;
    pScratch->fcbs_size = 0;
    for(int i = 0; i < surv[0]; i++){
        int pos = sort_ix[i];
        fcb.sgntr = pSt->sgntrs[pos];
        fcb.pos_new = pos;
        fcb.sign_new = d_sign[pos];
        fcb.wnrg = (fcb_state->num[pos]*fcb_state->num[pos])/fcb_state->den[pos]; // not needed for non Toeplitz
        fcb.n_pulses = 0;
        fcb.fcb_state_idx = 0;
        memcpy(&pScratch->fcbs[pScratch->fcbs_size++], &fcb, sizeof(fcb));
    }

    check_if_better_deldec(pScratch, &pScratch->fcbs[0], &best_fcb[SMPL_CELP_IDX_MAIN], &best_fcb_state[SMPL_CELP_IDX_MAIN], &nrg_thr[SMPL_CELP_IDX_MAIN], wnrg_per_pulse[SMPL_CELP_IDX_MAIN]);
    if (fcb_pulses_max[SMPL_CELP_IDX_FEC] > 0) {
        check_if_better_deldec(pScratch, &pScratch->fcbs[0], &best_fcb[SMPL_CELP_IDX_FEC], &best_fcb_state[SMPL_CELP_IDX_FEC], &nrg_thr[SMPL_CELP_IDX_FEC], wnrg_per_pulse[SMPL_CELP_IDX_FEC]);
    }
    if(fcb_pulses_max[SMPL_CELP_IDX_MAIN] > 1) {
        for(int pulse_nr = 2; pulse_nr < fcb_pulses_max[SMPL_CELP_IDX_MAIN]; pulse_nr++){
            pScratch->fcb_candidates_size = 0;
            pScratch->unique_sgntr_size = 0;
            int idx = 0;
            for(int i = 0; i < pScratch->fcbs_size; i++){
                add_pulse(pSt, &pScratch->fcbs[i], d_abs, d_sign, surv[pulse_nr-1], idx, lag, pitch_sharp);
                idx++;
            }
            update_read_write_idx(pScratch);
            // Sort fcb_candidates
            for(int i = 0; i < pScratch->fcb_candidates_size; i++){
                Q[i] = pScratch->fcb_candidates[i].wnrg;
            }
            smpl_get_maxi_K(Q, sort_ix, pScratch->fcb_candidates_size, surv[pulse_nr-1]);
            // Move best from fcb_candidates to fcbs
            pScratch->fcbs_size = 0;
            for(int i = 0; i < surv[pulse_nr-1]; i++){
                memcpy(&pScratch->fcbs[pScratch->fcbs_size++], &pScratch->fcb_candidates[sort_ix[i]], sizeof(pScratch->fcbs[0]));
            }
            check_if_better_deldec(pScratch, &pScratch->fcbs[0], &best_fcb[SMPL_CELP_IDX_MAIN], &best_fcb_state[SMPL_CELP_IDX_MAIN], &nrg_thr[SMPL_CELP_IDX_MAIN], wnrg_per_pulse[SMPL_CELP_IDX_MAIN]);
            if (fcb_pulses_max[SMPL_CELP_IDX_FEC] >= pulse_nr) {
                check_if_better_deldec(pScratch, &pScratch->fcbs[0], &best_fcb[SMPL_CELP_IDX_FEC], &best_fcb_state[SMPL_CELP_IDX_FEC], &nrg_thr[SMPL_CELP_IDX_FEC], wnrg_per_pulse[SMPL_CELP_IDX_FEC]);
            }
        }
        // last pulse
        pScratch->fcb_candidates_size = 0;
        pScratch->unique_sgntr_size = 0;
        for(int i = 0; i < pScratch->fcbs_size; i++){
            add_pulse(pSt, &pScratch->fcbs[i], d_abs, d_sign, 1, i, lag, pitch_sharp);
        }
        update_read_write_idx(pScratch);
        int best_idx = 0;
        float max_wnrg = pScratch->fcb_candidates[0].wnrg;
        for(int i = 1 ; i < pScratch->fcb_candidates_size; i++){
            if(pScratch->fcb_candidates[i].wnrg > max_wnrg){
                max_wnrg = pScratch->fcb_candidates[i].wnrg;
                best_idx = i;
            }
        }
        check_if_better_deldec(pScratch, &pScratch->fcb_candidates[best_idx], &best_fcb[SMPL_CELP_IDX_MAIN], &best_fcb_state[SMPL_CELP_IDX_MAIN], &nrg_thr[SMPL_CELP_IDX_MAIN], wnrg_per_pulse[SMPL_CELP_IDX_MAIN]);
    }
    for (int r = SMPL_CELP_IDX_FEC; r < SMPL_CELP_IDX_MAIN + 1; r++) {
        for (int i = 0; i < best_fcb[r].n_pulses; i++) {
            pulses[r][i] = best_fcb_state[r].pulse_signs[i] > 0 ? (1 + best_fcb_state[r].pulse_positions[i]) : -(1 + best_fcb_state[r].pulse_positions[i]);
        }
        pulses[r][best_fcb[r].n_pulses] = best_fcb[r].sign_new > 0 ? 1 + best_fcb[r].pos_new : -(1 + best_fcb[r].pos_new);

        if (best_fcb[r].wnrg > 0.0f) {
            wnrg[r] = best_fcb[r].wnrg;
            gain_from_search[r] = best_fcb_state[r].num[best_fcb[r].pos_new] / best_fcb_state[r].den[best_fcb[r].pos_new];
            fcb_wnrg[r] = best_fcb_state[r].den[best_fcb[r].pos_new];
            n_pulses[r] = (int16_t)best_fcb[r].n_pulses + 1;
        }
        else {
            wnrg[r] = 0.0f;
            gain_from_search[r] = 0.0f;
            fcb_wnrg[r] = 0.0f;
            n_pulses[r] = 0;
        }
    }
}

void acb_synthesize(int fcb_subfrlen, float acb_basis[], float acb_g[SMPL_ACBG_M], float acb[], float high_boost)
{
    adjust_acbgains(acb_g, high_boost);
    #if SMPL_ACBG_M == 2
        smpl_scale_vec(acb_basis, acb, fcb_subfrlen, acb_g[0]);
        smpl_add_scale_vec_inplace(acb_basis + fcb_subfrlen, acb, fcb_subfrlen, acb_g[1]);
    #else
        memset(acb, 0, fcb_subfrlen * sizeof(float));
        for(int m = 0; m < SMPL_ACBG_M; m++){
            for(int i = 0; i < fcb_subfrlen; i++){
                acb[i] += acb_basis[m * fcb_subfrlen + i] * acb_g[m];
            }
        }
    #endif
}

void smpl_syn_ltp_basis(const float lags[], int n_lags, float state[], int state_len, float acb_basis[])
{
    smpl_assert(SMPL_ACBG_M == 2);  // Hardcoded to 3 tap symmetric
    smpl_assert(state_len > 0);
    float *p_exclpc_end = &state[state_len - n_lags * SMPL_LAG_SUBFRLEN];
    for(int subfr = 0; subfr < n_lags; subfr++){
        int i_lag = (int)floor(lags[subfr]);
        if((float)i_lag == lags[subfr]){
            for(int i = 0; i < SMPL_LAG_SUBFRLEN; i++){
                p_exclpc_end[i] = p_exclpc_end[i - i_lag];           
            }
            memcpy(&acb_basis[subfr*SMPL_LAG_SUBFRLEN], p_exclpc_end, SMPL_LAG_SUBFRLEN * sizeof(float));
            smpl_add_vec(p_exclpc_end - i_lag - 1, p_exclpc_end - i_lag + 1, acb_basis + (n_lags + subfr) * SMPL_LAG_SUBFRLEN, SMPL_LAG_SUBFRLEN);
        } else {
            int i = -1;
            float first = smpl_dot_prod(&p_exclpc_end[i - i_lag - SMPL_LTP_INTERPOL_DELAY], smpl_interpol_kernel, 2 * SMPL_LTP_INTERPOL_DELAY);
            i++;
            smpl_interpol(p_exclpc_end - i_lag - SMPL_LTP_INTERPOL_DELAY, p_exclpc_end, SMPL_LAG_SUBFRLEN);
            i += SMPL_LAG_SUBFRLEN;
            float last = smpl_dot_prod(&p_exclpc_end[i - i_lag - SMPL_LTP_INTERPOL_DELAY], smpl_interpol_kernel, 2 * SMPL_LTP_INTERPOL_DELAY);
            memcpy(&acb_basis[subfr*SMPL_LAG_SUBFRLEN], p_exclpc_end, SMPL_LAG_SUBFRLEN * sizeof(float));
            acb_basis[(n_lags + subfr) * SMPL_LAG_SUBFRLEN] = first + p_exclpc_end[1];
            smpl_add_vec(p_exclpc_end, p_exclpc_end + 2, acb_basis + (n_lags + subfr) * SMPL_LAG_SUBFRLEN + 1, SMPL_LAG_SUBFRLEN - 2);
            i = SMPL_LAG_SUBFRLEN - 1;
            acb_basis[(n_lags + subfr) * SMPL_LAG_SUBFRLEN + i] = p_exclpc_end[i-1] + last;
        }
        p_exclpc_end += SMPL_LAG_SUBFRLEN;
    }
}

void smpl_pitch_sharp(float x[], int lag, int L)
{
    for(int i = lag; i < L; i++) {
        x[i] += x[i - lag] * SMPL_PITCH_SHARPENING_COEF;
    }
}

const float smpl_dec_acb_high_boost[2] = { 0.35f, 0.18f };

void smpl_celp_decode(
    CelpDecoder* pCelpDec,
    const int voiced,
    float acb_gain[SMPL_ACBG_M],
    const float lags[],
    const int num_lags,
    const int subfrlen,
    const int low_rate,
    const float normalized_bitrate,
    float lpc_res[])
{
    smpl_assert(low_rate == SMPL_TRUE || low_rate == SMPL_FALSE);
    smpl_assert(subfrlen <= SMPL_MAX_SF_LEN);
    int acb_state_len = subfrlen + 2 * SMPL_MAX_PITCH_LAG + SMPL_LTP_INTERPOL_DELAY;
    if (voiced) {
        float high_boost = smpl_dec_acb_high_boost[0] + (smpl_dec_acb_high_boost[1] - smpl_dec_acb_high_boost[0]) * normalized_bitrate;
        int i_lag = (int)lags[num_lags - 1];
        if (low_rate) {
            smpl_pitch_sharp(lpc_res, i_lag, subfrlen);
        }
        float acb_basis[SMPL_MAX_SF_LEN * SMPL_ACBG_M], acb[SMPL_MAX_SF_LEN];
        smpl_syn_ltp_basis(lags, num_lags, pCelpDec->acb_state, acb_state_len, acb_basis);
        acb_synthesize(subfrlen, acb_basis, acb_gain, acb, high_boost);
        smpl_add_vec_inplace(acb, lpc_res, subfrlen);
    }
    // Update adaptive codebook state
    memmove(pCelpDec->acb_state, &pCelpDec->acb_state[subfrlen], (acb_state_len - 2 * subfrlen) * sizeof(float));
    memcpy(&pCelpDec->acb_state[acb_state_len - 2 * subfrlen], lpc_res, subfrlen * sizeof(float));
}

void acb_dequant(int low_rate, int acb_idx, float acb_g[]) {
    const int16_t *cb_acbgains = low_rate == SMPL_TRUE ? smpl_cb_acbgains_lr_Q14 : smpl_cb_acbgains_hr_Q14;
    const float sc_Q14 = 1.0f / ((int)1 << 14);
    for (int m = 0; m < SMPL_ACBG_M; m++) {
        acb_g[m] = cb_acbgains[acb_idx * SMPL_ACBG_M + m] * sc_Q14;
    }
}

void adjust_acbgains(float acb_g[SMPL_ACBG_M], float high_boost)
{
    if (high_boost == 0.0f) {
        return;
    }
    float f[SMPL_ACBG_M];
    f[0] = acb_g[0] + 2.0f * acb_g[1];
    f[1] = acb_g[0] - acb_g[1];
    float abs_f2new = SMPL_min( SMPL_abs(f[1]) + high_boost, SMPL_abs(f[0]) );
    f[1] *= (abs_f2new / (SMPL_abs(f[1]) + 1e-12f));
    acb_g[0] = (f[0] + 2.0f * f[1]) / 3.0f;
    acb_g[1] = (f[0] - f[1]) / 3.0f;
}

static const float coef_ma_v[3] = { 0.25f, -0.496f, 0.25f };

static inline void add_noise_uv(NoiseGenerator* pNoiseGen, float exc_noise_uv[], int L, const float lsf[SMPL_LPC_ORDER], float nrg_ratio, float noise[]) {
    // unvoiced HP cutoff frequency goes up when first two LSFs are close together(indicating a peak in LPC spectrum)
    float lsf_hz = 16000.0f * (lsf[0] + lsf[1]) / (4.0f * SMPL_PI);
    float min_uv_fcorner_Hz = lsf_hz * 3.0f * smpl_sigmoid(0.2f / (lsf[1] - lsf[0] + 1e-30f) - 3.0f);
    float uv_fcorner_Hz = SMPL_DEC_NOISE_UV_FCORNER_HZ * SMPL_min(1.0f, 0.6f + 0.4f * nrg_ratio);
    uv_fcorner_Hz = SMPL_max(uv_fcorner_Hz, min_uv_fcorner_Hz);
    uv_fcorner_Hz = SMPL_min(uv_fcorner_Hz, 1500.0f);
    float coef_tmp = 6.0f * uv_fcorner_Hz / 16000.0f;
    float coef_ma_uv[2], coef_ar_uv[2];
    coef_ma_uv[0] = (1.0f - 0.5f * coef_tmp) * SMPL_DEC_NOISE_UV_NOISE_GAIN;
    coef_ma_uv[1] = -coef_ma_uv[0];
    coef_ar_uv[0] = 1.0f;
    coef_ar_uv[1] = -1.0f + coef_tmp;
    smpl_filt_arma1(exc_noise_uv, L, coef_ma_uv, 2, coef_ar_uv, 2, pNoiseGen->out_state_uv, 2, exc_noise_uv);
    smpl_add_vec_inplace(exc_noise_uv, noise, L);
}

void smpl_celp_gen_noise(
    NoiseGenerator* pNoiseGen,
    const float exc_lpc[],
    const int L,
    const int voiced,
    const int num_pulses,
    const float nrgres,
    const int fcbg_idx,
    const float lsf[SMPL_LPC_ORDER],
    const float normalized_bitrate,
    float noise[])
{
    const CelpTables* pTbl = smpl_get_celp_Tbls();
    smpl_assert(pTbl != NULL);
    float nrg_ratio = 1.0f;
    float noise_uv[SMPL_MAX_SF_LEN];
    float noise_v[SMPL_MAX_SF_LEN];
    float noise_v2_[SMPL_MAX_SF_LEN + 3];
    float* noise_v2 = noise_v2_ + 3;
    float env[SMPL_MAX_SF_LEN];
    if (voiced == SMPL_TRUE) {
        TIC(noise_V)
        float corrs[SMPL_NOISE_CORR_ORDER + 1], c[SMPL_NOISE_CORR_ORDER + 1], ctgt[SMPL_NOISE_CORR_ORDER + 1];
        for (int i = 0; i < SMPL_NOISE_CORR_ORDER + 1; i++) {
            corrs[i] = smpl_dot_prod(exc_lpc, &exc_lpc[i], L - i);
        }
        corrs[0] += 1e-12f;
#define CORR_SMTH_COEF_5MS 0.16f
#define CORR_SMTH_COEF_10MS 0.4f
        float corr_smth_coef = L == (SMPL_CELP_FS_KHZ * 10) ? CORR_SMTH_COEF_10MS : CORR_SMTH_COEF_5MS;
        for (int i = 0; i < SMPL_NOISE_CORR_ORDER + 1; i++) {
            pNoiseGen->corr_smth[i] += corr_smth_coef * (corrs[i] - pNoiseGen->corr_smth[i]);
        }
        float scale = SMPL_DEC_NOISE_V_NOISE_GAIN * SMPL_DEC_NOISE_V_NOISE_GAIN * corrs[0] / pNoiseGen->corr_smth[0];
        for (int i = 0; i < SMPL_NOISE_CORR_ORDER + 1; i++) {
            c[i] = pNoiseGen->corr_smth[i] * scale;
        }
        c[1] *= 2.0f;
        c[2] *= 2.0f;

        float f2[SMPL_NOISE_DCT_ORDER], f2_tgt[SMPL_NOISE_DCT_ORDER];
        smpl_matrix_mult_transp_16(&pTbl->dct_mat_t[0][0], c, f2, SMPL_NOISE_DCT_ORDER, SMPL_NOISE_CORR_ORDER + 1);
        float m = smpl_maximum(f2, SMPL_NOISE_DCT_ORDER) * 1.5f;
        for (int i = 0; i < SMPL_NOISE_DCT_ORDER; i++) {
            f2_tgt[i] = m - f2[i];
        }
        smpl_matrix_mult(&pTbl->dct_mat_t[0][0], f2_tgt, ctgt, SMPL_NOISE_CORR_ORDER + 1, SMPL_NOISE_DCT_ORDER);
        smpl_gen_rand_pulses(noise_v, L, &pNoiseGen->rand_seed);
        if (!pNoiseGen->prev_voiced) {
            pNoiseGen->env_smth = pNoiseGen->env_last;
        }
        smpl_get_env(exc_lpc, L, SMPL_ENV_SMTH_COEF_V, &pNoiseGen->env_smth, env);
        for (int i = 0; i < L; i++) {
            noise_v[i] *= env[i];
        }
        float nrg_noise = smpl_nrg(noise_v, L);
        smpl_scale_vec_inplace(ctgt, SMPL_NOISE_CORR_ORDER + 1, 1.0f / (nrg_noise + 1e-12f));
        float coef_ma[SMPL_NOISE_CORR_ORDER + 1];
        smpl_spec_fact2(ctgt, coef_ma);

        smpl_filt_ma2(noise_v, L, coef_ma, SMPL_NOISE_CORR_ORDER + 1, pNoiseGen->shape_state, SMPL_NOISE_CORR_ORDER, noise_v2);

        if (!pNoiseGen->prev_voiced) {
            smpl_gen_rand_pulses(noise_uv, L, &pNoiseGen->rand_seed);
            float env_val = pNoiseGen->env_last * SMPL_ENV_SMTH_COEF_UV_V;
            for (int i = 0; i < L; i += 2) {
                noise_uv[i + 0] *= env_val;
                noise_uv[i + 1] *= env_val * SMPL_ENV_SMTH_COEF_UV_V;
                env_val *= SMPL_ENV_SMTH_COEF_UV_V * SMPL_ENV_SMTH_COEF_UV_V;
            }
        } else if (pNoiseGen->since_unvoiced < 2) {
            memset(noise_uv, 0, L * sizeof(float));
        }

        pNoiseGen->env_last = env[L-1];        
        TOC(noise_V)
    }
    else {
        TIC(noise_UV)
        memset(pNoiseGen->corr_smth, 0, (SMPL_NOISE_CORR_ORDER + 1) * sizeof(float));
        memset(pNoiseGen->shape_state, 0, SMPL_NOISE_CORR_ORDER * sizeof(float));
        memset(noise_v2, 0, L * sizeof(float));

        float nrg_tgt;
        if (num_pulses > 0) {
            nrg_ratio = smpl_nrg(exc_lpc, L) / (nrgres + 1e-20f);
            float hardness = 10.0f + 20.0f * normalized_bitrate;  // lower -> add more noise when nrg_ratio is close to 1
            nrg_tgt = nrgres * logf(expf(hardness * (1.0f - nrg_ratio)) + 1) / hardness;
            smpl_get_env(exc_lpc, L, SMPL_ENV_SMTH_COEF_UV, &pNoiseGen->env_smth, env);
        }
        else {
            nrg_ratio = 0.0f;
            nrg_tgt = nrgres;
            smpl_get_env0(L, SMPL_ENV_SMTH_COEF_UV, &pNoiseGen->env_smth, env);
        }

        float scale = 1.0f / L;
        nrg_tgt = nrg_tgt * scale + 1e-30f;
        float nrg_env = smpl_nrg(env, L) * scale;
        smpl_assert(nrg_env > 0.0f);  // get_env() or get_env0() should never return a zero envelope
        // f = floor, g = gain; final envelope is f + g * env
        float f = sqrtf(nrg_tgt);
        float g = sqrtf(nrg_tgt / nrg_env);
        float ge = g * env[0];
        float env_last = pNoiseGen->env_last;
        if (env_last < SMPL_min(f, ge)) {
            if (f < ge) {
                g = 0.0f;
            } else {
                f = 0.0f;
            }
        } 
        else if (env_last > SMPL_max(f, ge)) {
            if (f > ge) {
                g = 0.0f;
            } else {
                f = 0.0f;
            }
        } 
        else {
            // f = env_last - env[0] * g
            // a ^ 2 * g + b * g + c = 0
            float sum_env = smpl_sum(env, L) * scale;
            float a = nrg_env + env[0] * env[0] - 2.0f * sum_env * env[0];
            float b = 2.0f * env_last * (sum_env - env[0]);
            float c = env_last * env_last - nrg_tgt;
            float tmp = b * b - 4.0f * a * c;
            if ((tmp < 1e-35f) || (a < 1e-25f)) {
                f = 0.0f;
                g = 0.0f;
            } else {
                tmp = sqrtf(tmp);
                scale = 0.5f / a;
                g = (-b + tmp) * scale;
                f = env_last - env[0] * g;
                if (f < 0) {
                    g = (-b - tmp) * scale;
                    f = env_last - env[0] * g;
                }
            }
        }

        smpl_gen_rand_pulses(noise_uv, L, &pNoiseGen->rand_seed);
        if (num_pulses > 0) {
            float max_val = pTbl->fcbgains_uv[fcbg_idx] * 0.5f;
            for (int i = 0; i < L; i++) {
                if (exc_lpc[i] == 0.0f) {
                    noise_uv[i] *= SMPL_min(f + g * env[i], max_val);
                } else {
                    noise_uv[i] = 0.0f;
                }
            }
            pNoiseGen->env_last = SMPL_min(f + g * env[L-1], max_val);
        }
        else {
            for (int i = 0; i < L; i++) {
                noise_uv[i] *= f + g * env[i];
            }
            pNoiseGen->env_last = f + g * env[L-1];
        }

        TOC(noise_UV)
    }
    TIC(add_noise)
    if (pNoiseGen->prev_voiced || voiced) {
        smpl_filt_ma2(noise_v2, L, coef_ma_v, 3, pNoiseGen->out_state_v, 2, noise);
    } else {
        memset(noise, 0, L * sizeof(float));
    }
    if ((pNoiseGen->since_unvoiced < 2) || !voiced) {
        add_noise_uv(pNoiseGen, noise_uv, L, lsf, nrg_ratio, noise);
    } else {
        memset(pNoiseGen->out_state_uv, 0, 2 * sizeof(float));
    }
    pNoiseGen->prev_voiced = voiced;
    if (voiced) {
        pNoiseGen->since_unvoiced++;
    } else {
        pNoiseGen->since_unvoiced = 0;
    }
    TOC(add_noise)
}

static int8_t pos2idx[20] = {0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 3, 3};
void smpl_gen_excitation(
    const int16_t fcb_gain_ix[],
    const int voiced,
    const int num_subfr,
    const int subfr_len,
    const int nPositions,
    const int16_t positions[],
    const int16_t pos_pulses[],
    float exc[])
{
    TIC(excitation)
    CelpTables* pTbl = (CelpTables*)g_smpl_celp_tables;
    smpl_assert(pTbl != NULL);

    float *gain_tab = voiced ? pTbl->fcbgains_v : pTbl->fcbgains_uv;
    float fcb_gains[SMPL_MAX_N_SUBFR];
    for (int sf = 0; sf < num_subfr; sf++) {
        fcb_gains[sf] = gain_tab[fcb_gain_ix[sf]];
    }

    const int shift = subfr_len == 80 ? 4 : 5;
    memset(exc, 0.0f, num_subfr * subfr_len * sizeof(float));
    for (int num_pos = 0; num_pos < nPositions; num_pos++) {
        int pos = positions[num_pos];
        exc[pos] = (float)pos_pulses[num_pos] * fcb_gains[pos2idx[pos >> shift]];  // pos2idx[pos >> shift] = pos / subfr_len
    }
    TOC(excitation)
}
