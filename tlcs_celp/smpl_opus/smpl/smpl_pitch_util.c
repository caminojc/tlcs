#include "smpl_param_coding.h"
#include "smpl_pitch.h"
#include "smpl_typedef.h"
#include "smpl_defines.h"
#include "smpl_filt.h"
#include "smpl_codec_util.h"
#include <math.h>
#include "debug.h"

static inline void smpl_calc_E1(float E1[], const float ltpbuf[], int t, int minpitch, int maxpitch, int lag_subfrlen)
{
    int numlags = maxpitch - minpitch + 1;
    const float* reg = &ltpbuf[t - minpitch];
    E1[0] = SMPL_max(smpl_nrg(reg, lag_subfrlen), 1e-9f);
    for (int i = 1; i < numlags; i++) {
        E1[i] = SMPL_max(E1[i - 1] + (reg[-i] * reg[-i]) - reg[lag_subfrlen - i] * reg[lag_subfrlen - i], 1e-9f);
    }
}

void smpl_pitch_calc_E1(
    float E1[],
    const float ltpbuf[],
    int ltpbuf_len,
    int numsubfrs,
    int minpitch,
    int maxpitch,
    int lag_subfrlen)
{
    const int numlags = maxpitch - minpitch + 1;
    const int maxpitch_ = maxpitch + (numsubfrs - 1) * lag_subfrlen;
    const int numlags_ = maxpitch_ - minpitch + 1;
    int t = ltpbuf_len - lag_subfrlen;
    float E1_[1024];
    smpl_assert(numlags_ <= (int)(SMPL_ARR_LEN(E1_)));
    smpl_calc_E1(E1_, ltpbuf, t, minpitch, maxpitch_, lag_subfrlen);
    int offset = numlags_ - numlags;
    for (int sf = 0; sf < numsubfrs; sf++) {
        for (int i = 0; i < numlags; i++) {
            E1[sf * numlags + i] = E1_[offset + i];
        }
        offset -= lag_subfrlen;
    }
}

static inline float dot_prod_20(const float a[], const float b[])
{
    float ret = 0.0f;
    for (int i = 0; i < 20; i++) {
        ret += a[i] * b[i];
    }
    return ret;
}

void smpl_pitch_calc_C_E2(
    float C[],
    float E2[],
    const float ltpbuf[],
    int ltpbuf_len,
    int numsubfrs)
{
#define NUM_LAGS_STAGE1 (SMPL_MAXPITCH_STAGE1 - SMPL_MINPITCH_STAGE1 + 1)
    int t = ltpbuf_len - SMPL_PITCH_LAG_SUBFRLEN_STAGE1 * numsubfrs;
    for (int sf = 0; sf < numsubfrs; sf++) {
        const float* tgt = &ltpbuf[t];
        const float* reg = &ltpbuf[t - SMPL_MINPITCH_STAGE1];
        for (int i = 0; i < NUM_LAGS_STAGE1; i++) {
            C[sf * NUM_LAGS_STAGE1 + i] = dot_prod_20(tgt, &reg[-i]);
        }
        t += SMPL_PITCH_LAG_SUBFRLEN_STAGE1;
        E2[sf] = SMPL_max(dot_prod_20(tgt, tgt), 1.0e-9f);
    }
}

static const float  pitch_downsamp_filt[2 * PITCH_DOWNSAMP_DELAY + 1] = {
    -0.045472838f, 0.0f, 0.06366198f, 0.0f, -0.10610329f, 0.0f, 0.31830987f, 0.5f, 0.31830987f, 0.0f, -0.10610329f, 0.0f, 0.06366198f, 0.0f, -0.045472838f
};

int smpl_pitch_downsample(PitchEstimator* pSt, const float ltp_buf[], int L)
{
    const float* ptr_in = ltp_buf;
    float* ptr_out = pSt->scratchMem->ltp_buf_stage1;
    for (int j = 0; j < (L - 2 * PITCH_DOWNSAMP_DELAY) / 2; j++) {
        float tmp = ptr_in[2 * j + PITCH_DOWNSAMP_DELAY] * pitch_downsamp_filt[PITCH_DOWNSAMP_DELAY];
        for (int i = 0; i < PITCH_DOWNSAMP_DELAY; i += 2) {            // half the length
            tmp += (ptr_in[2 * j + i] + ptr_in[2 * j + 2 * PITCH_DOWNSAMP_DELAY - i]) * pitch_downsamp_filt[i];
        }
        ptr_out[j] = tmp;
    }

    L = (L - 2 * PITCH_DOWNSAMP_DELAY) / 2;

    return L;
}

static inline void upsamp_E_core(const float x[], float y[], int len)
{
    for (int i = 0; i < len; i++) {
        *y-- = (x[0] + x[1]) * 0.5f;
        *y-- = *x--;
    }
}

void smpl_upsamp_E_core(const float x[], float y[], int len)
{
    upsamp_E_core(x, y, len);
}

static const float pitch_interpol_filt_C[2 * PITCH_INTERPOL_DELAY_C] = {
    -0.0024414062f, 0.023925781f, -0.119628906f, 0.59814453f, 0.59814453f, -0.119628906f, 0.023925783f, -0.0024414062f
};

static inline void upsamp_C_core(const float x[], float y[], int len)
{
    for (int i = 0; i < len; i++) {
        float tmp = 0.0f;
        for (int j = 0; j < PITCH_INTERPOL_DELAY_C; j++) {
            tmp += (x[j - (PITCH_INTERPOL_DELAY_C - 1)] + x[PITCH_INTERPOL_DELAY_C - j]) * pitch_interpol_filt_C[j];
        }
        *y-- = tmp;
        *y-- = *x--;
    }
}
void smpl_upsamp_C_core(const float x[], float y[], int len)
{
    upsamp_C_core(x, y, len);
}

void smpl_upsamp_E_fast(int numsubfrs, int* minpitch_, int* numlags, float E[])
{
    const int nlags_in = *numlags;
    const int nlags_out = (nlags_in - 1) * 2;
    for (int sf = numsubfrs - 1; sf >= 0; sf--) {
        const float* x = &E[sf * nlags_in + nlags_in - 2];
        float* y = &E[sf * nlags_out + nlags_out - 1];
        upsamp_E_core(x, y, nlags_in - 1);
    }
    *numlags = nlags_out;
    *minpitch_ = *minpitch_ * 2;
}

void smpl_upsamp_C_fast(int numsubfrs, int* minpitch_, int* numlags, float C[])
{
    const int nlags_in = *numlags;
    const int nlags_out = (nlags_in - PITCH_INTERPOL_DELAY_C) * 2;
    for (int sf = numsubfrs - 1; sf >= 0; sf--) { // start high to avoid overwriting input for other subframes
        const float* x = &C[sf * nlags_in + nlags_in - 1 - PITCH_INTERPOL_DELAY_C];
        float* y = &C[sf * nlags_out + nlags_out - 1];
        upsamp_C_core(x, y, nlags_in - (PITCH_INTERPOL_DELAY_C * 2 - 1));
    }
    *numlags = nlags_out;
    *minpitch_ = *minpitch_ * 2;
}

static inline float dot_prod_40(const float a[], const float b[])
{
    float ret = 0.0f;
    for (int i = 0; i < 40; i++) {
        ret += a[i] * b[i];
    }
    return ret;
}

static inline void update_uniqueblocks(const int track[], int num_subframes, uint16_t uniqueblocks[SMPL_PITCH_NUM_SUBFRAMES])
{
    for (int i = 0; i < num_subframes; i++) {
        uniqueblocks[i] |= (1 << track[i]);
    }
}

static inline void calc_sf_weights(float E2[], int num_subframes, float sf_wght[])
{
    float sum_E2 = 0.0f;
    for (int sf = 0; sf < num_subframes; sf++) {
        sum_E2 += E2[sf];
    }
    for (int sf = 0; sf < num_subframes; sf++) {
        sf_wght[sf] = E2[sf] / sum_E2;
    }
}

#ifdef SMPL_PITCH_SHORTWGHT2
static inline float get_short_lag_bias(float pitchcorr, float corr_multiples, float lag)
{
    if (corr_multiples <= -1.0f) {
        return 0.0f;
    }
    float pitch_bias = (SMPL_max(SMPL_MAXPITCH_LEN / lag - 1.0f, 0.0f) * SMPL_MINPITCH_LEN / (SMPL_MAXPITCH_LEN - SMPL_MINPITCH_LEN)) * SMPL_PITCH_SHORTWGHT2;
    pitch_bias *= smpl_sigmoid(SMPL_PITCH_SIGM_SCALE * (corr_multiples - pitchcorr));
    return pitch_bias;
}
#endif

static inline float get_prev_lag_bias(PitchEstimator* pSt, float lag)
{
    float lag_diff = SMPL_abs(lag - pSt->prev_lag);
    float diff_thres = SMPL_PITCH_PREVWGHT_SPAN * pSt->prev_lag;
    if (lag_diff < diff_thres) {
        return (pSt->prev_pitch_corr * (1.0f - (lag_diff / diff_thres))) * SMPL_PITCH_PREVWGHT;
    }
    return 0.0f;
}

static inline int sumdeltas(int laginds[], int numsubfrs) {
    int ret = 0;
    for (int i = 1; i < numsubfrs; i++) {
        ret += SMPL_abs(laginds[i] - laginds[i - 1]);
    }
    return ret;
}

#define HARMONICITY_UNDEF -10000.0f
static inline float spectral_harmonicity(float avg_lag, float F2w[], float cache[], int cacheLen, int reset_cache)
{
    TIC(harmonicity)
    if (reset_cache) {
        for (int i = 0; i < cacheLen; i++) {
            cache[i] = HARMONICITY_UNDEF;
        }
    }

    const float inv_F2_step_Hz = 2 * (SMPL_F_LEN - 1) / 16000.0f;

    // spectral harmonicity: measure ratio of harmonic peaks and valleys at lowish frequencies
    float harm_Hz = 16000 / avg_lag;

    int harm_ix = (int)roundf(harm_Hz * 2 * inv_F2_step_Hz);
    smpl_assert(harm_ix >= 0);
    smpl_assert(harm_ix < cacheLen);
    if (cache[harm_ix] > HARMONICITY_UNDEF) {
        TOC(harmonicity)
        return cache[harm_ix];
    }

    float harm_width = harm_Hz * inv_F2_step_Hz;
     
    float harm_strength = 0.1f;
#define NUM_HARMS 4
    if (harm_width > 1.97) { // Pitch frequencey is above ~60Hz
        float peak_valley_mags[2 * NUM_HARMS + 1];
        for (int num_harm = 0; num_harm <= NUM_HARMS * 2; num_harm++) {
            float ix_start = 0.5f * num_harm * harm_width;
            float ix_end = ix_start + harm_width;
            int idx_start = (int)ceilf(ix_start);
            int idx_end = (int)floorf(ix_end);
            float weights[20];
            int weigths_len = idx_end - idx_start + 1;
            smpl_assert(weigths_len <= 20);
            float inv_harm_width = 1.0f / harm_width;
            for (int i = 0; i < weigths_len; i++) {
                float tmp = (idx_start - ix_start + i) * inv_harm_width;
                tmp -= tmp * tmp;
                weights[i] = tmp * tmp;
            }
            float peak_valley_nrg = smpl_dot_prod(F2w + idx_start, weights, weigths_len) / smpl_sum_vec(weights, weigths_len);
            peak_valley_mags[num_harm] = sqrtf(peak_valley_nrg + 1e-30f);
        }
        float mag_ratios_log[NUM_HARMS];
        float mag_weights[NUM_HARMS];
        const float mag_peak_weights[] = { 1.0f, 10.0f, 1.0f };
        const float mag_valley_weights[] = { 5.0f, 2.0f, 5.0f };
        for (int num_harm = 0; num_harm < NUM_HARMS; num_harm++) {
            float mag_peak = mag_peak_weights[0] * peak_valley_mags[2 * num_harm + 0] +
                mag_peak_weights[1] * peak_valley_mags[2 * num_harm + 1] +
                mag_peak_weights[2] * peak_valley_mags[2 * num_harm + 2];
            float mag_valley = mag_valley_weights[0] * peak_valley_mags[2 * num_harm + 0] +
                mag_valley_weights[1] * peak_valley_mags[2 * num_harm + 1] +
                mag_valley_weights[2] * peak_valley_mags[2 * num_harm + 2];
            mag_ratios_log[num_harm] = logf(mag_peak / mag_valley);
            mag_weights[num_harm] = sqrtf(mag_peak + mag_valley + 1e-30f);
        }
        harm_strength = smpl_dot_prod(mag_weights, mag_ratios_log, NUM_HARMS) / smpl_sum_vec(mag_weights, NUM_HARMS); // -1.5 ... 1.5
    }
    cache[harm_ix] = harm_strength;
    TOC(harmonicity)
    return harm_strength;
}

static const float pitch_hp_b[] = { 1.0f, -1.0f };
static const float pitch_hp_a[] = { 1.0f, -0.96f };

void smpl_pitch(
    void* st,
    const float ltp_buf[],
    int L,
    int lookAhead,
    float* F2,
    int coded_as_active_voice,
    int numsubfrs,
    float lags[SMPL_PITCH_NUM_SUBFRAMES],
    int* laginds,
    float* pitchc,
    int* blockseg_idx,
    float *avg_lag,
    float* harm_strength)
{
    PitchEstimator* pSt = (PitchEstimator*)st;
    smpl_assert(pSt != NULL);
    smpl_assert(pSt->initialized == SMPL_TRUE);
    PitchEstScratch *pMem = pSt->scratchMem;
    smpl_assert(numsubfrs == SMPL_PITCH_NUM_SUBFRAMES || numsubfrs == SMPL_PITCH_NUM_SUBFRAMES / 2);
    const PITCH_data* pPitchData = smpl_get_pitch_data(numsubfrs);

    smpl_assert(L <= (int)SMPL_ARR_LEN(pMem->ltp_buf_hp));
    if (coded_as_active_voice == SMPL_FALSE) {
        for (int i = 0; i < numsubfrs; i++) {
            lags[i] = SMPL_MINPITCH_MS * SMPL_PITCH_FS_KHZ;
        }
        *avg_lag = SMPL_MINPITCH_MS * SMPL_PITCH_FS_KHZ;
        memset(laginds, 0, numsubfrs * sizeof(int));
        *pitchc = 0.0f;
        *blockseg_idx = 0;
        pSt->prev_lag = 0.0f;
        pSt->prev_pitch_corr = 0.0f;
        pSt->prev_lagblk = -1;
        pSt->prev_lagidx = -1;
        return;
    }

    float state[2];
    memset(state, 0, sizeof(state));
    int offset = PITCH_DOWNSAMP_DELAY;
    memset(pMem->ltp_buf_stage1, 0, sizeof(float) * offset);
    // HP: -3dB @ 60 Hz, to get rid of low-frequency noise
    smpl_filt_arma1(ltp_buf, L, pitch_hp_b, 2, pitch_hp_a, 2, state, 2, &pMem->ltp_buf_stage1[offset]);
    memcpy(pMem->ltp_buf_hp, &pMem->ltp_buf_stage1[offset], (L - lookAhead) * sizeof(float));
    const float* ltp_buf_hp = pMem->ltp_buf_hp;

    int stage1_len = smpl_pitch_downsample(pSt, pMem->ltp_buf_stage1, L + offset);

    smpl_pitch_calc_E1(pMem->E1, pMem->ltp_buf_stage1, stage1_len, numsubfrs, SMPL_MINPITCH_STAGE1, SMPL_MAXPITCH_STAGE1, SMPL_PITCH_LAG_SUBFRLEN_STAGE1);
    smpl_pitch_calc_C_E2(pMem->C, pMem->E2, pMem->ltp_buf_stage1, stage1_len, numsubfrs);

    int numlags = SMPL_MAXPITCH_STAGE1 - SMPL_MINPITCH_STAGE1 + 1;
    smpl_assert(numlags <= 16 * (SMPL_MAXPITCH_MS - SMPL_MINPITCH_MS));
    for (int sf = 0; sf < numsubfrs; sf++) {
        float sqrt_E1[16 * (SMPL_MAXPITCH_MS - SMPL_MINPITCH_MS)];
        for (int i = 0; i < numlags; i++) {
            sqrt_E1[i] = pMem->E1[sf * numlags + i] + 1e-30f;
        }
        smpl_sqrt_vec(sqrt_E1, numlags);
        float sqrt_E2 = sqrtf(pMem->E2[sf] + 1e-30f);
        for (int i = 0; i < numlags; i++) {
            float tmp = 0.5f * (sqrt_E1[i] + sqrt_E2);
            pMem->E[sf * numlags + i] = tmp * tmp;
        }
    }
    //int upsamp_steps = (int)log2f((float)(SMPL_PITCH_COARSE_FS_KHZ / SMPL_PITCH_STAGE_1_FS_KHZ));
    //assert(((1 << upsamp_steps) * pSt->cfg.pitch_stage_1_fs_Hz) == pSt->cfg.pitch_coarse_search_fs_Hz);
    int minpitch_C = SMPL_MINPITCH_STAGE1, numlags_C = numlags;
    int minpitch_E = SMPL_MINPITCH_STAGE1, numlags_E = numlags;
    if (pSt->low_complexity_mode) {
        smpl_upsamp_E_fast(numsubfrs, &minpitch_C, &numlags_C, pMem->C);
    }
    else {
        smpl_upsamp_C_fast(numsubfrs, &minpitch_C, &numlags_C, pMem->C);
    }
    smpl_upsamp_E_fast(numsubfrs, &minpitch_E, &numlags_E, pMem->E);

    const int minpitch_coarse = SMPL_PITCH_COARSE_FS_KHZ * SMPL_MINPITCH_MS;
    const int numlags_coarse = SMPL_PITCH_COARSE_FS_KHZ * (SMPL_MAXPITCH_MS - SMPL_MINPITCH_MS);
    int offset_C = minpitch_coarse - minpitch_C;
    int offset_E = minpitch_coarse - minpitch_E;
    for (int sf = 0; sf < numsubfrs; sf++) { // Needs to be variable for 10 ms frames
        for (int i = 0; i < numlags_coarse; i++) {
            pMem->H[sf * numlags_coarse + i] = pMem->C[sf * numlags_C + offset_C + i] / pMem->E[sf * numlags_E + offset_E + i];
        }
        memcpy(&pMem->H_coarse[sf * numlags_coarse], &pMem->H[sf * numlags_coarse], numlags_coarse * sizeof(float));
        memcpy(&pMem->C_coarse[sf * numlags_coarse], &pMem->C[sf * numlags_C + offset_C], numlags_coarse * sizeof(float));
        memcpy(&pMem->E_coarse[sf * numlags_coarse], &pMem->E[sf * numlags_E + offset_E], numlags_coarse * sizeof(float));
    }

    int pitchblock_coarse = SMPL_PITCHBLOCK_MS * SMPL_PITCH_COARSE_FS_KHZ;
    float Hblk[SMPL_PITCH_NUM_SUBFRAMES][PITCH_NUM_BLOCKS];
    for (int sf = 0; sf < numsubfrs; sf++) {
        float* block_ptr = &pMem->H[sf * numlags_coarse];
        for (int block = 0; block < PITCH_NUM_BLOCKS; block++) {
            Hblk[sf][block] = smpl_maximum(block_ptr, pitchblock_coarse);
            block_ptr += pitchblock_coarse;
        }
    }
#define PITCHBLOCK (SMPL_PITCHBLOCK_MS * SMPL_PITCH_FS_KHZ)
#define BLOCKSIZE (PITCHBLOCK * 2)
#define REDUCTION_FACTOR 0.7f
#define PITCH_DELTAWGHT (SMPL_PITCH_DELTAWGHT / BLOCKSIZE)
    float utils[NUM_BLOCKTRACKS];
    int track_idx[NUM_BLOCKTRACKS];
    float sf_wght[SMPL_PITCH_NUM_SUBFRAMES];
    calc_sf_weights(pMem->E2, numsubfrs, sf_wght);

    for (int i = 0; i < pPitchData->num_blocktracks; i++) {
        float corr = 0.0f;
        for (int sf = 0; sf < numsubfrs; sf++) {
            corr += Hblk[sf][pPitchData->blocktracks[i].track[sf]] * sf_wght[sf];
        }
        float shortlagbias1 = (SMPL_MAXPITCH_LEN / ((pPitchData->blocktracks[i].meanblock + 1.5f) * PITCHBLOCK) - 1.0f) * SMPL_PITCH_SHORTWGHT1;
        utils[i] = 1.0f / (1.1f - corr) - REDUCTION_FACTOR * PITCHBLOCK * PITCH_DELTAWGHT * pPitchData->blocktracks[i].trackdeltas + shortlagbias1;
    }
    smpl_get_maxi_K(utils, track_idx, pPitchData->num_blocktracks, pSt->numstates1);

#define NUMLAGS_FS (SMPL_PITCH_FS_KHZ * (SMPL_MAXPITCH_MS - SMPL_MINPITCH_MS))

    // Calculate E1 and E2 again in input sampling frequency
    smpl_pitch_calc_E1(pMem->E1, ltp_buf_hp, L - lookAhead, numsubfrs, minpitch_E, minpitch_E + numlags_E - 1, SMPL_LAG_SUBFRLEN);

    uint16_t uniqueblocks[SMPL_PITCH_NUM_SUBFRAMES];
    memset(uniqueblocks, 0, sizeof(uniqueblocks));
    for (int i = 0; i < pSt->numstates1; i++) {
        update_uniqueblocks(pPitchData->blocktracks[track_idx[i]].track, numsubfrs, uniqueblocks);
    }

    const float H_thres = pSt->low_complexity_mode ? 0.0f : 0.25f;
    offset_C = SMPL_MINPITCH_MS * SMPL_PITCH_FS_KHZ - minpitch_C;
    offset_E = SMPL_MINPITCH_MS * SMPL_PITCH_FS_KHZ - minpitch_E;
    for (int sf = 0; sf < numsubfrs; sf++) {
        uint16_t mask = 1;
        float* C_ptr = &pMem->C[offset_C + sf * numlags_C];
        float* E_ptr = &pMem->E[offset_E + sf * numlags_E];
        const float* E1_ptr = &pMem->E1[offset_E + sf * numlags_E];
        const float* H_ptr = &pMem->H[sf * NUMLAGS_FS];
        const float* ltp_buf_ptr = &ltp_buf_hp[L - lookAhead + (sf - numsubfrs) * SMPL_LAG_SUBFRLEN];
        pMem->E2[sf] = SMPL_max(dot_prod_40(ltp_buf_ptr, ltp_buf_ptr), 1e-9f);
        float sqrt_E2 = sqrtf(pMem->E2[sf] + 1e-30f);
        float sqrt_E1[SMPL_PITCHBLOCK_MS * 16 + 1];
        for (int block = 0; block < PITCH_NUM_BLOCKS; block++) {
            if (uniqueblocks[sf] & mask) {
                for (int i = 0; i < PITCHBLOCK + 1; i++) {
                    sqrt_E1[i] = E1_ptr[block * PITCHBLOCK + i] + 1e-30f;
                }
                smpl_sqrt_vec(sqrt_E1, PITCHBLOCK + 1);
                for (int i = 0; i < PITCHBLOCK + 1; i++) { // Add one more sample for interpolation
                    float tmp = 0.5f * (sqrt_E1[i] + sqrt_E2);
                    E_ptr[block * PITCHBLOCK + i] = 0.5f * tmp * tmp;
                }
                // Update Correlations around peaks only
                for (int i = 0; i < PITCHBLOCK; i++) {
                    if (H_ptr[block * PITCHBLOCK + i] > H_thres) {
                        int lag = SMPL_MINPITCH_LEN + block * PITCHBLOCK + i;
                        C_ptr[block * PITCHBLOCK + i] = 0.5f * dot_prod_40(ltp_buf_ptr, ltp_buf_ptr - lag);
                    }
                }
            }
            mask <<= 1;
        }
    }

    // Upsample C and E and calculate H. Update from high to low to avoid overwriting when upsampling
    for (int sf = numsubfrs - 1; sf >= 0; sf--) {
        float* C_ptr = &pMem->C[offset_C + sf * numlags_C];
        float* C_ptr_frac = &pMem->C[offset_C + sf * (PITCH_NUM_BLOCKS * 2 * PITCHBLOCK + offset_C)];
        float* E_ptr = &pMem->E[offset_E + sf * numlags_E];
        float* E_ptr_frac = &pMem->E[offset_E + sf * (PITCH_NUM_BLOCKS * 2 * PITCHBLOCK + offset_E)];
        float* H_ptr = &pMem->H[sf * 2 * PITCHBLOCK * PITCH_NUM_BLOCKS];
        uint16_t mask = 1 << (PITCH_NUM_BLOCKS - 1);
        for (int block = PITCH_NUM_BLOCKS - 1; block >= 0; block--) {
            if (uniqueblocks[sf] & mask) {
                const float* Ein = &E_ptr[block * PITCHBLOCK];
                float* Eout = &E_ptr_frac[block * 2 * PITCHBLOCK];
                smpl_upsamp_E_core(Ein + PITCHBLOCK - 1, Eout + 2 * PITCHBLOCK - 1, PITCHBLOCK); // Update pointers to last sample. upsamp_E_core can do that              
                const float* Cin = &C_ptr[block * PITCHBLOCK];
                float* Cout = &C_ptr_frac[block * 2 * PITCHBLOCK];
                if (pSt->low_complexity_mode) {
                    smpl_upsamp_E_core(Cin + PITCHBLOCK - 1, Cout + 2 * PITCHBLOCK - 1, PITCHBLOCK); // Update pointers to last sample. upsamp_E_core can do that                              
                }
                else {
                    smpl_upsamp_C_core(Cin + PITCHBLOCK - 1, Cout + 2 * PITCHBLOCK - 1, PITCHBLOCK); // Update pointers to last sample. upsamp_E_core can do that                              
                }
                for (int i = 0; i < 2 * PITCHBLOCK; i++) {
                    H_ptr[block * 2 * PITCHBLOCK + i] = Cout[i] / Eout[i]; // No need to check for zero as E has been added a noise floor above 
                }
            }
            mask >>= 1;
        }
    }

    // Fine search
    float H_comb[SMPL_PITCHBLOCK_MS * SMPL_PITCH_FS_KHZ * 2];
    int nlaginds = 0;
    uint8_t blocksegs_ix[NUM_BLOCKSEGS];
    memset(pMem->lagind_cache, -1, sizeof(pMem->lagind_cache));
    for (int surv = 0; surv < pSt->numstates1; surv++) {
        int idx = track_idx[surv];
        for (uint8_t j = 0; j < pPitchData->blocksegs_ix[idx][1]; j++) {
            blocksegs_ix[nlaginds] = pPitchData->blocksegs_ix[idx][0] + j;
            const PITCH_blocksegs* pBlocksegs = &pPitchData->blocksegs[blocksegs_ix[nlaginds]];
            int start_sf = 0;
            for (int n = 0; n < pBlocksegs->nblocks; n++) {
                int lookup_key = (((start_sf << PITCH_CACHE_BITS_SEG_LEN) + pBlocksegs->seglens[n]) << PITCH_CACHE_BITS_BLOCK) + pBlocksegs->blocks[n];
                int best_i = pMem->lagind_cache[lookup_key];
                if (best_i == -1) {
                    memset(H_comb, 0, sizeof(H_comb));
                    for (int sf = start_sf; sf < start_sf + pBlocksegs->seglens[n]; sf++) {
                        float* H_ptr = &pMem->H[sf * 2 * PITCHBLOCK * PITCH_NUM_BLOCKS + pBlocksegs->blocks[n] * 2 * PITCHBLOCK];
                        for (int i = 0; i < 2 * PITCHBLOCK; i++) { // Potential optimization by first searching inter lag. And then do only +- 0.5 from that
                            H_comb[i] += H_ptr[i] * pMem->E2[sf]; // Just premultiply H up front. We might also be able to cache some partial results
                        }
                    }
                    best_i = smpl_get_maxi(H_comb, 2 * PITCHBLOCK);
                    pMem->lagind_cache[lookup_key] = best_i;
                }
                for (int sf = start_sf; sf < start_sf + pBlocksegs->seglens[n]; sf++) {
                    pMem->laginds[nlaginds][sf] = best_i + pBlocksegs->blocks[n] * 2 * PITCHBLOCK;
                }
                start_sf += pBlocksegs->seglens[n];
            }
            nlaginds++;
        }
    }

#ifdef SMPL_PITCH_SHORTWGHT2
    // Correlation for pitch multiple
    float corr_multiples[NUM_BLOCKSEGS];
    for (int surv = 0; surv < nlaginds; surv++) {
        corr_multiples[surv] = -1.0f;
    }
    memset(pMem->lagind_cache, -1, sizeof(pMem->lagind_cache));
    for (int surv = 0; surv < nlaginds; surv++) {
        int lagMult = 2;
        int max_ind = SMPL_max(pMem->laginds[surv][0], pMem->laginds[surv][1]);
        for (int sf = 2; sf < numsubfrs; sf++) {
            max_ind = SMPL_max(max_ind, pMem->laginds[surv][sf]);
        }
        int lag2 = (lagMult * (max_ind + 2 * SMPL_MINPITCH_LEN)) / 2;
        if (lag2 < SMPL_MAXPITCH_LEN) {
            const PITCH_blocksegs* pBlocksegs = &pPitchData->blocksegs[blocksegs_ix[surv]];
            int start_sf = 0;
            float sumC = 0.0f, sumE = 0.0f;
            for (int n = 0; n < pBlocksegs->nblocks; n++) {
                lag2 = (lagMult * (pMem->laginds[surv][start_sf] + 2 * SMPL_MINPITCH_LEN)) / 2;
                const int search_reach = (int)roundf(((float)lag2 / SMPL_PITCH_SEARCH_MULT_FRAC) + 1.0f);
                int idx_mult = (lag2 - SMPL_MINPITCH_LEN);
                int lookup_key = (((start_sf << PITCH_CACHE_BITS_SEG_LEN) + pBlocksegs->seglens[n]) << PITCH_CACHE_BITS_BLOCK) + pBlocksegs->blocks[n];
                int best_i = pMem->lagind_cache[lookup_key];
                if (best_i == -1) {
                    int start_idx = SMPL_max(idx_mult - search_reach, 0);
                    int end_idx = SMPL_min(idx_mult + search_reach, (pitchblock_coarse * PITCH_NUM_BLOCKS) - 1);
                    int n_search = SMPL_min(end_idx - start_idx + 1, ((int)(SMPL_ARR_LEN(H_comb))));
                    memset(H_comb, 0, n_search * sizeof(float));
                    for (int sf = start_sf; sf < start_sf + pBlocksegs->seglens[n]; sf++) {
                        const float* H_ptr = &pMem->H_coarse[sf * pitchblock_coarse * PITCH_NUM_BLOCKS];
                        for (int i = 0; i < n_search; i++) {
                            H_comb[i] += H_ptr[i + start_idx] * pMem->E2[sf]; // Just premultiply H up front. We might also be able to cache some partial results
                        }
                    }
                    best_i = smpl_get_maxi(H_comb, n_search);
                    pMem->lagind_cache[lookup_key] = best_i;
                }
                best_i = SMPL_max(idx_mult - search_reach, 0) + best_i;
                for (int sf = start_sf; sf < start_sf + pBlocksegs->seglens[n]; sf++) {
                    const float* C_ptr = &pMem->C_coarse[sf * (PITCH_NUM_BLOCKS * pitchblock_coarse)];
                    const float* E_ptr = &pMem->E_coarse[sf * (PITCH_NUM_BLOCKS * pitchblock_coarse)];
                    sumC += C_ptr[best_i];
                    sumE += E_ptr[best_i];
                }
                start_sf += pBlocksegs->seglens[n];
            }
            corr_multiples[surv] = SMPL_max(corr_multiples[surv], sumC / sumE);
        }
    }
#endif
    // Final search
    float best_util = 0.0f, best_pitchcorr = 0.0f;
    int best_surv = 0;
    float pitch_ratewght = pSt->low_rate ? SMPL_PITCH_RATEWGHT_LR : SMPL_PITCH_RATEWGHT_HR;

    float F2w[SMPL_F_LEN];
    F2w[0] = 0.0f;
    F2w[1] = 0.0f;
    for (int i = 2; i < SMPL_F_LEN; i++) {
        float tmp = F2[i] * (i + 3);
        F2w[i] = tmp;
    }
    int max_ix = smpl_get_maxi(sf_wght, numsubfrs);
    float spectral_harm_cache[50];
    for (int surv = 0; surv < nlaginds; surv++) {
        float mean_lag = 0.0f, sumC = 0.0f, sumE = 0.0f;
        for (int sf = 0; sf < numsubfrs; sf++) {
            const float* C_ptr = &pMem->C[offset_C + sf * (PITCH_NUM_BLOCKS * 2 * PITCHBLOCK + offset_C)];
            const float* E_ptr = &pMem->E[offset_E + sf * (PITCH_NUM_BLOCKS * 2 * PITCHBLOCK + offset_E)];
            // mean_lag += sf_wght[sf] * (pMem->laginds[surv][sf] * 0.5f + SMPL_MINPITCH_LEN);
            sumC += C_ptr[pMem->laginds[surv][sf]];
            sumE += E_ptr[pMem->laginds[surv][sf]];
        }
        float rate_bias = smpl_encode_lags(pPitchData, NULL, blocksegs_ix[surv], pMem->laginds[surv], pSt->prev_lagblk, pSt->prev_lagidx, 1) * pitch_ratewght;

        mean_lag = pMem->laginds[surv][max_ix] * 0.5f + SMPL_MINPITCH_LEN;

        float pitchcorr = sumC / sumE;
        float first_lag = 0.5f * pMem->laginds[surv][0] + SMPL_MINPITCH_LEN;
#ifdef SMPL_PITCH_SHORTWGHT2
        float short_lag_bias = get_short_lag_bias(pitchcorr, corr_multiples[surv], mean_lag);
#else
        float short_lag_bias = 0.0f;
#endif
        float prev_lag_bias = get_prev_lag_bias(pSt, first_lag);
#ifdef SMPL_PITCH_SPEC_HARM_BIAS
        float spectral_harm_bias = SMPL_PITCH_SPEC_HARM_BIAS * spectral_harmonicity(mean_lag, F2w, spectral_harm_cache, SMPL_ARR_LEN(spectral_harm_cache), surv == 0);
#else
        float spectral_harm_bias = 0.0f;
#endif

        float util = 1.0f / (1.1f - pitchcorr) - PITCH_DELTAWGHT * sumdeltas(pMem->laginds[surv], numsubfrs) + spectral_harm_bias + short_lag_bias + prev_lag_bias - rate_bias;
        if (surv == 0 || util > best_util) {
            best_util = util;
            best_surv = surv;
        }
        if (surv == 0 || pitchcorr > best_pitchcorr) {
            best_pitchcorr = pitchcorr;
        }
    }
    // *avg_lag = 0.0f;
    for (int sf = 0; sf < numsubfrs; sf++) {
        lags[sf] = pMem->laginds[best_surv][sf] * 0.5f + SMPL_MINPITCH_LEN;
        laginds[sf] = pMem->laginds[best_surv][sf];
        // *avg_lag += sf_wght[sf] * lags[sf];
    }
    *avg_lag = pMem->laginds[best_surv][max_ix] * 0.5f + SMPL_MINPITCH_LEN;
#ifdef SMPL_PITCH_SPEC_HARM_BIAS
    *harm_strength = spectral_harmonicity(*avg_lag, F2w, spectral_harm_cache, SMPL_ARR_LEN(spectral_harm_cache), SMPL_FALSE);
#else
    *harm_strength = spectral_harmonicity(*avg_lag, F2w, spectral_harm_cache, SMPL_ARR_LEN(spectral_harm_cache), SMPL_TRUE);
#endif
    pSt->prev_lag = lags[numsubfrs - 1];
    pSt->prev_pitch_corr = best_pitchcorr;

    pSt->prev_lagidx = pMem->laginds[best_surv][numsubfrs - 1];
    pSt->prev_lagblk = pSt->prev_lagidx / (2 * PITCHBLOCK); // Tmp Similar to Julia we will let the encode parameters module handle this book-keeping

    *pitchc = best_pitchcorr;
    *blockseg_idx = blocksegs_ix[best_surv];

    pSt->offset_end = offset_C; // Temp just to copy out to Julia
}
