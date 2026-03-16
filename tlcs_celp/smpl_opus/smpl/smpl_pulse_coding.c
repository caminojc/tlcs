
#include <stdlib.h>
#include <stdio.h>
#include "smpl_pulse_coding.h"
#include "smpl_defines.h"
#include "smpl_typedef.h"
#include "smpl_entropy_wrapper.h"
#include "smpl_codec_util.h"
#include "smpl_tables.h"
#include "smpl_pulse_tables.h"
#include "smpl_helpers.h"

static int smpl_pdf_to_CMF(int32_t pdf[], int N, uint16_t cmf[], int32_t maxval)
{
    if (maxval == -1) {
        maxval = 32767;
    }
    int64_t sump = 0;
    for (int i = 0; i < N; i++) {
        sump += pdf[i];
    }
    if (sump <= 0) {
        return -1;
    }
    cmf[0] = 0;
    for (int i = 0; i < N; i++) {
        int32_t p = (int32_t)(((int64_t)pdf[i] * (int64_t)(maxval - N)) / sump) + 1;
        cmf[i + 1] = cmf[i] + p;
    }
    return 0;
}

#define ONE_Q31 ((int64_t)1 << 31)

static inline int create_runlen_table(int max_samples, uint16_t cmf_buf[], RunLenCMFs *pRunLenCMFs)
{
    smpl_assert(max_samples > 0);
    pRunLenCMFs->max_samples = max_samples;
    int ret = 0;
    for (int nump = 1; nump <= SMPL_MAX_PULSES_PER_SF; nump++) {
        int64_t Plonger_Q31 = ONE_Q31;
        int32_t p[SMPL_MAX_SF_LEN];
        for (int nums = 1; nums <= max_samples; nums++) {
            int64_t tmp = (int64_t)1 << 31;
            tmp = ONE_Q31 - (ONE_Q31 / (max_samples - nums + 1));
            int64_t p1_Q31 = tmp;
            for (int i = 0; i < nump - 1; i++) {
                p1_Q31 = (p1_Q31 * tmp) >> 31;
            }
            p1_Q31 = ONE_Q31 - p1_Q31;
            p1_Q31 = SMPL_min(p1_Q31, (int64_t)2147376274); // 0.99995
            opus_int32 log_out_Q7;
            if (nump > max_samples) {
               log_out_Q7 = silk_lin2log((nump << 10) / max_samples) - 10 * 128;
            }
            else {
               log_out_Q7 = -(silk_lin2log((max_samples << 10) / nump) - 10 * 128);
            }
            #define SIGM_BIAS_Q5  146
            #define SCALE_MAX_Q15 36000
            #define SCALE_MIN_Q15 26000
            int32_t scale_fac_Q15 = SCALE_MAX_Q15 - (((SCALE_MAX_Q15 - SCALE_MIN_Q15) * silk_sigm_Q15((log_out_Q7 >> 2) + SIGM_BIAS_Q5)) >> 15);
            p1_Q31 = ONE_Q31 - silk_log2lin(((scale_fac_Q15 * (silk_lin2log((opus_int32)(ONE_Q31 - p1_Q31)) - 31*128)) >> 15) + 31*128);
            p1_Q31 = SMPL_min(p1_Q31, (int64_t)2147376274); // 0.99995
            p[nums - 1] = (int32_t)((Plonger_Q31 * p1_Q31) >> 31);
            Plonger_Q31 = (Plonger_Q31 * (ONE_Q31 - p1_Q31)) >> 31;
        }
        pRunLenCMFs->cmfs[nump - 1] = cmf_buf;
        smpl_pdf_to_CMF(p, max_samples, cmf_buf, -1);
        cmf_buf += (max_samples + 1);
        ret += (max_samples + 1);
    }
    return ret;
}

//create_split_CMFs(max_pulses) = [pdf_to_cmf([prob_split_fast(k, num_pulses) for k = 0:num_pulses]) for num_pulses = 1:max_pulses]
static inline int create_split_CMFs(PulseCodingTables *pSt)
{
    // Calculate how much memory is needed for the CMFs
    int tot_cmf_len = 0;
    for (int num_pulses = 1; num_pulses < SMPL_MAX_PULSES_PER_SF * 4; num_pulses++) {
        int min_split = SMPL_max(num_pulses - SMPL_MAX_PULSES_PER_SF * 2, 0);
        int max_split = num_pulses - min_split;
        tot_cmf_len += max_split - min_split + 2;
    }
    pSt->SplitCmfBuf = (uint16_t*)calloc(tot_cmf_len, sizeof(uint16_t));
    smpl_assert(pSt->SplitCmfBuf != NULL);
    uint16_t* cmfPtr = pSt->SplitCmfBuf;
    int32_t p[(SMPL_MAX_PULSES_PER_SF * 2) + 1];
    for (int num_pulses = 1; num_pulses < SMPL_MAX_PULSES_PER_SF * 4; num_pulses++) {
        int min_split = SMPL_max(num_pulses - SMPL_MAX_PULSES_PER_SF * 2, 0);
        int max_split = num_pulses - min_split;
        for (int k = min_split; k <= max_split; k++) {
            p[k - min_split] = smpl_prob_split_fast(k, num_pulses);
        }
        smpl_pdf_to_CMF(p, max_split - min_split + 1, cmfPtr, -1);
        pSt->split_CMFs[num_pulses - 1].cmf = cmfPtr;
        pSt->split_CMFs[num_pulses - 1].cmfLen = max_split - min_split + 2;
        cmfPtr += max_split - min_split + 2;
    }
    smpl_assert(cmfPtr - pSt->SplitCmfBuf == tot_cmf_len);
    return 0;
}

//stirling(n) = n == 0 ? 0 : (n + 0.5) * log2(n) - log2(exp(1)) * n + 0.5 * log2(2 * pi) + log2(exp(1)) / 12n

#define LOG2_EXP1_Q15 47274
#define LOG2_2PI_Q14 43442

int32_t smpl_stirling(int32_t n)
{
    if (n == 0) {
        return 0;
    }
    int32_t ret_Q15 = ((n << 1) + 1) * (silk_lin2log(n) << 7);
    ret_Q15  -= LOG2_EXP1_Q15*n;
    ret_Q15 += LOG2_2PI_Q14;
    ret_Q15 += LOG2_EXP1_Q15 / (12 * n);
    return ret_Q15;
}

//prob_split_fast(k, N) = 2 ^ (stirling(N) - stirling(k) - stirling(N - k) - N)
int32_t smpl_prob_split_fast(int32_t k, int32_t N)
{
    int32_t tmp_Q15 = smpl_stirling(N) - smpl_stirling(k) - smpl_stirling(N - k) - N * (int32_t)(1 << 15);
    //assert(tmp_Q15 <= 0);
    if (tmp_Q15 == 0) {
        return (int32_t)1 << 30;
    }
    tmp_Q15 = -tmp_Q15;
    int32_t ret = silk_log2lin(tmp_Q15 >> 8);
    return ((int32_t)1 << 30) / ret;
}

void* g_smpl_pulse_tables = NULL;

void* smpl_create_pulse_tables(void)
{
    if (g_smpl_pulse_tables) {
        return g_smpl_pulse_tables;
    }

    PulseCodingTables* pSt = (PulseCodingTables*)calloc(1, sizeof(PulseCodingTables));
    if (!pSt) {
        smpl_assert(0);
        return NULL;
    }
    // Calculate memory needed for RunLength CMF's
    int RunLenCmfBufLen = 0;
    for (int i = 0; i < NUM_RUNLEN_CMFS; i++) {
        RunLenCmfBufLen += (((i + 1) * RUNLENGTH_STEP) + 1) * SMPL_MAX_PULSES_PER_SF;
    }
    pSt->RunLenCmfBuf = (uint16_t*)calloc(RunLenCmfBufLen, sizeof(uint16_t));
    if (!pSt->RunLenCmfBuf) {
        smpl_assert(0);
        free(pSt);
        return NULL;
    }
    uint16_t* pcmfBuf = pSt->RunLenCmfBuf;
    for (int i = 0; i < NUM_RUNLEN_CMFS; i++) {
        int cnt = create_runlen_table((i + 1) * RUNLENGTH_STEP, pcmfBuf, &pSt->runlen_CMFs[i]);
        pcmfBuf += cnt;
    }
    if (create_split_CMFs(pSt) != 0) {
        smpl_assert(0);
        // Could not create split_CMFs
        free(pSt);
        return NULL;
    }

    pSt->n_pulse_cmfs[0].cmfLen = SMPL_ARR_LEN(smpl_n_pulses_dcmf_bgn) + 1;
    pSt->n_pulse_cmfs[1].cmfLen = SMPL_ARR_LEN(smpl_n_pulses_dcmf_uv ) + 1;
    pSt->n_pulse_cmfs[2].cmfLen = SMPL_ARR_LEN(smpl_n_pulses_dcmf_v  ) + 1;
    pSt->n_pulse_cmfs[0].cmf = (uint16_t*)calloc(pSt->n_pulse_cmfs[0].cmfLen, sizeof(uint16_t));
    pSt->n_pulse_cmfs[1].cmf = (uint16_t*)calloc(pSt->n_pulse_cmfs[1].cmfLen, sizeof(uint16_t));
    pSt->n_pulse_cmfs[2].cmf = (uint16_t*)calloc(pSt->n_pulse_cmfs[2].cmfLen, sizeof(uint16_t));
    smpl_dcmf_to_cmf(smpl_n_pulses_dcmf_bgn, SMPL_ARR_LEN(smpl_n_pulses_dcmf_bgn), pSt->n_pulse_cmfs[0].cmf);
    smpl_dcmf_to_cmf(smpl_n_pulses_dcmf_uv,  SMPL_ARR_LEN(smpl_n_pulses_dcmf_uv ), pSt->n_pulse_cmfs[1].cmf);
    smpl_dcmf_to_cmf(smpl_n_pulses_dcmf_v,   SMPL_ARR_LEN(smpl_n_pulses_dcmf_v  ), pSt->n_pulse_cmfs[2].cmf);

    g_smpl_pulse_tables = pSt;
    return pSt;
}

void smpl_free_pulse_tables(void)
{
    if (g_smpl_pulse_tables) {
        PulseCodingTables* pSt = (PulseCodingTables*)g_smpl_pulse_tables;
        free(pSt->RunLenCmfBuf);
        free(pSt->SplitCmfBuf);
        free(pSt->n_pulse_cmfs[0].cmf);
        free(pSt->n_pulse_cmfs[1].cmf);
        free(pSt->n_pulse_cmfs[2].cmf);
        free(pSt);
        g_smpl_pulse_tables = NULL;
    }
}

static inline uint16_t smpl_num_pulses_cmf(int max_pulses, int n_pulses)
{
    return n_pulses == 0 ? 0 : max_pulses * (n_pulses + 1) - (((n_pulses - 2) * (n_pulses - 3)) >> 1);
}

static void encode_split_2_subfrs(ec_ctx* psRange, int n_pulses, int n_pulses_firsthalf, int max_subfr_pulses, const CmfVec *split_CMFs) {
    int min_split = SMPL_max(n_pulses - max_subfr_pulses, 0);
    int max_split = n_pulses - min_split;
    smpl_assert(max_split >= min_split);
    smpl_assert(n_pulses_firsthalf >= min_split);
    smpl_assert(n_pulses_firsthalf <= max_split);
    if (max_split == min_split) {
        return;
    }
    const CmfVec *pcmf = &split_CMFs[n_pulses - 1];
    uint16_t sub = pcmf->cmf[min_split];
    ec_encode(psRange, pcmf->cmf[n_pulses_firsthalf] - sub, pcmf->cmf[n_pulses_firsthalf + 1] - sub, pcmf->cmf[max_split + 1] - sub);
}

void smpl_encode_pulses(
    void* ec_range_enc,
    const int16_t pulses[SMPL_FRAME_LEN],
    int framelen,
    int nSubfr,
    int lowRate,
    int Voiced,
    int coded_as_active_voice,
    int16_t sf_pulses[])
{
    smpl_assert(g_smpl_pulse_tables != NULL);
    const PulseCodingTables* pSt = (PulseCodingTables*)g_smpl_pulse_tables;
    ec_ctx* psRange = (ec_ctx*)ec_range_enc;

    int subfrlen = framelen / nSubfr;
    int16_t positions[SMPL_FRAME_LEN];
    int nPositions = 0;
    memset(sf_pulses, 0, nSubfr * sizeof(int16_t));
    for (int sf = 0; sf < nSubfr; sf++) {
        const int16_t* pPtr = &pulses[sf * subfrlen];
        for (int i = 0; i < subfrlen; i++) {
            if (pPtr[i] != 0) {
                sf_pulses[sf] += SMPL_abs(pPtr[i]);
                positions[nPositions++] = sf * subfrlen + i;
            }
        }
    }
    int n_pulses = 0;
    for (int i = 0; i < nSubfr; i++) {
        n_pulses += sf_pulses[i];
    }

    // Encode n_pulses
    int max_pulses = smpl_max_pulses_per_frame[lowRate][coded_as_active_voice + Voiced] * framelen / 320;
    int max_subfr_pulses = max_pulses / nSubfr;
    smpl_assert(n_pulses <= max_pulses);
    if (lowRate) {
        const CmfVec *pcmf = &pSt->n_pulse_cmfs[coded_as_active_voice + Voiced];
        ec_encode(psRange, pcmf->cmf[n_pulses], pcmf->cmf[n_pulses + 1], pcmf->cmf[pcmf->cmfLen - 1]);
    } else {
        ec_encode(psRange, smpl_num_pulses_cmf(max_pulses + 1, n_pulses), smpl_num_pulses_cmf(max_pulses + 1, n_pulses + 1), smpl_num_pulses_cmf(max_pulses + 1, max_pulses + 1));
    }

    if (n_pulses == 0) {
        return;
    }
    if (nSubfr == 4) {
        int n_pulses_firsthalf = sf_pulses[0] + sf_pulses[1];
        int min_split = SMPL_max(n_pulses - SMPL_MAX_PULSES_PER_SF * 2, 0);
        OPUS_UNUSED int max_split = n_pulses - min_split;
        int min_split2 = SMPL_max(n_pulses - max_subfr_pulses * 2, 0);
        int max_split2 = n_pulses - min_split;
        smpl_assert(min_split2 >= min_split);
        smpl_assert(max_split2 <= max_split);
        smpl_assert(n_pulses_firsthalf >= min_split2);
        smpl_assert(n_pulses_firsthalf <= max_split2);
        if (max_split2 > min_split2) {
            int cmf_ix = n_pulses_firsthalf - min_split;
            const CmfVec* pcmf = &pSt->split_CMFs[n_pulses - 1];
            uint16_t sub = pcmf->cmf[min_split2 - min_split];
            ec_encode(psRange, pcmf->cmf[cmf_ix] - sub, pcmf->cmf[cmf_ix + 1] - sub, pcmf->cmf[max_split2 - min_split + 1] - sub);
        }
        if (n_pulses_firsthalf > 0)
        {
            encode_split_2_subfrs(psRange, n_pulses_firsthalf, sf_pulses[0], max_subfr_pulses, pSt->split_CMFs);
        }
        if (n_pulses_firsthalf < n_pulses)
        {
            encode_split_2_subfrs(psRange, n_pulses - n_pulses_firsthalf, sf_pulses[2], max_subfr_pulses, pSt->split_CMFs);
        }
    }
    else if (nSubfr == 2)
    {
        encode_split_2_subfrs(psRange, n_pulses, sf_pulses[0], max_subfr_pulses, pSt->split_CMFs);
    }
    else { // nSubfr == 1
        sf_pulses[0] = n_pulses;
    }
    int pos_ix = 0;
    for (int i = 0; i < nSubfr; i++) {
        int pulses_left = sf_pulses[i];
        int n_samples_left = subfrlen;
        int pos_prev = 1;
        while(pulses_left > 0) {
            int pos_frame = positions[pos_ix++];
            int n_stacked = SMPL_abs(pulses[pos_frame]);
            int pos = pos_frame - subfrlen * i + 1; // NB not C indexing
            for (int k = 0; k < n_stacked; k++) {
                int CMF_ind = ((n_samples_left + RUNLENGTH_STEP - 1) / RUNLENGTH_STEP) - 1;
                const uint16_t* cmfp = pSt->runlen_CMFs[CMF_ind].cmfs[pulses_left - 1];
                const int max_samples = pSt->runlen_CMFs[CMF_ind].max_samples;
                int ix_start = (max_samples - n_samples_left);
                int ix = ix_start + (pos - pos_prev);
                if (ix_start > 0)
                {
                    const uint16_t sub = cmfp[ix_start];
                    ec_encode(psRange, cmfp[ix] - sub, cmfp[ix + 1] - sub, cmfp[max_samples] - sub);
                }
                else
                {
                    ec_encode(psRange, cmfp[ix], cmfp[ix + 1], cmfp[max_samples]);
                }
                n_samples_left = subfrlen - pos + 1;
                pos_prev = pos;
                pulses_left -= 1;
            }
        }
    }
    // Signs
    uint16_t sym = 0;
    int signs_in_sym = 0;
    for (int i = 0; i < nPositions; i++) {
        int sgn = (pulses[positions[i]] > 0) ? 1 : 0;
        sym = (sym << 1) + sgn;
        signs_in_sym++;
        if (signs_in_sym == MAX_SIGNS_PER_SYMBOL) {
            // Encode Uniform
            ec_encode(psRange, sym, sym + 1, ((uint16_t)1 << MAX_SIGNS_PER_SYMBOL));
            sym = 0;
            signs_in_sym = 0;
        }
    }
    if (signs_in_sym > 0) {
        // Encode Uniform
        ec_encode(psRange, sym, sym + 1, ((uint16_t)1 << signs_in_sym) );
    }
}

static int decode_split_2_subfrs(ec_ctx* psRange, int n_pulses, int max_subfr_pulses, const CmfVec *split_CMFs) {
    int min_split = SMPL_max(n_pulses - max_subfr_pulses, 0);
    int max_split = n_pulses - min_split;
    if (max_split < min_split) {
        //smpl_assert(max_split >= min_split);
        return(-1);
    }
    if (max_split == min_split) {
        return min_split;
    }
    int cmfLen = max_split - min_split + 2;
    const CmfVec *pcmf = &split_CMFs[n_pulses - 1];
    return smpl_ec_decode_update(psRange, pcmf->cmf + min_split, cmfLen) + min_split;
}

void smpl_decode_pulses(
    void* ec_dec_st,
    int framelen,
    int nSubfr,
    int lowRate,
    int Voiced,
    int coded_as_active_voice,
    int16_t positions[],
    int16_t pos_pulses[],
    int *nPositionsOut,
    int *n_pulses,
    int16_t sf_pulses[]
)
{
    smpl_assert(g_smpl_pulse_tables != NULL);
    const PulseCodingTables* pSt = (PulseCodingTables*)g_smpl_pulse_tables;
    ec_ctx* psRange = (ec_ctx*)ec_dec_st;
    int subfrlen = framelen / nSubfr;
    *n_pulses = 0;
    int max_pulses = smpl_max_pulses_per_frame[lowRate][coded_as_active_voice + Voiced] * framelen / 320;
    int max_subfr_pulses = max_pulses / nSubfr;
    if (lowRate) {
        const CmfVec *pcmf = &pSt->n_pulse_cmfs[coded_as_active_voice + Voiced];
        *n_pulses = smpl_ec_decode_update(psRange, pcmf->cmf, pcmf->cmfLen);
    } else {
        unsigned cmf_max = smpl_num_pulses_cmf(max_pulses + 1, max_pulses + 1);
        unsigned cmf_low = smpl_ec_decode(psRange, cmf_max);
        unsigned cmf0 = 0;
        for (int s = 0; s <= max_pulses; s++) {
            unsigned cmf1 = smpl_num_pulses_cmf(max_pulses + 1, s + 1);
            if (cmf_low >= cmf0 && cmf_low < cmf1) {
                *n_pulses = s;
                smpl_ec_dec_update(psRange, cmf0, cmf1, cmf_max);
                break;
            }
            cmf0 = cmf1;
        }
    }
    *nPositionsOut = 0;
    memset(sf_pulses, 0, nSubfr * sizeof(int16_t));
    if (*n_pulses == 0) {
        return;
    }
    // pulses per subframe
    if (nSubfr == 4) {
        int min_split = SMPL_max(*n_pulses - SMPL_MAX_PULSES_PER_SF * 2, 0);
        int max_split = *n_pulses - min_split;
        int min_split2 = SMPL_max(*n_pulses - max_subfr_pulses * 2, 0);
        int max_split2 = *n_pulses - min_split;
        if (min_split2 < min_split || max_split2 > max_split) {
            smpl_assert(min_split2 >= min_split);
            smpl_assert(max_split2 <= max_split);
            return;
        }
        int n_pulses_firsthalf;
        if (max_split2 > min_split2) {
            const CmfVec* pcmf = &pSt->split_CMFs[*n_pulses - 1];
            int cmfLen = max_split2 - min_split2 + 2;
            n_pulses_firsthalf = smpl_ec_decode_update(psRange, pcmf->cmf + min_split2 - min_split, cmfLen) + min_split2;
        } else {
            n_pulses_firsthalf = min_split2;
        }
        if (n_pulses_firsthalf > 0) {
            sf_pulses[0] = decode_split_2_subfrs(psRange, n_pulses_firsthalf, max_subfr_pulses, pSt->split_CMFs);
            sf_pulses[1] = n_pulses_firsthalf - sf_pulses[0];
        }
        if (n_pulses_firsthalf < *n_pulses) {
            sf_pulses[2] = decode_split_2_subfrs(psRange, *n_pulses - n_pulses_firsthalf, max_subfr_pulses, pSt->split_CMFs);
            sf_pulses[3] = *n_pulses - n_pulses_firsthalf - sf_pulses[2];
        }
        if (sf_pulses[0]==-1 || sf_pulses[2] == -1) {
            // Some payload corruption
            memset(sf_pulses, 0, nSubfr * sizeof(int16_t));
            *n_pulses = 0;
        }
    }
    else if (nSubfr == 2) {
        sf_pulses[0] = decode_split_2_subfrs(psRange, *n_pulses, max_subfr_pulses, pSt->split_CMFs);
        sf_pulses[1] = *n_pulses - sf_pulses[0];
        if (sf_pulses[0]==-1) {
            // Some payload corruption
            memset(sf_pulses, 0, nSubfr * sizeof(int16_t));
            *n_pulses = 0;
        }
    }
    else { // nSubfr == 1
        sf_pulses[0] = *n_pulses;
    }
    smpl_decode_pulse_pos_signs(ec_dec_st, subfrlen, nSubfr, positions, pos_pulses, nPositionsOut, sf_pulses);
}
