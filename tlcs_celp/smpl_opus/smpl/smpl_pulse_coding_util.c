
#include <stdlib.h>
#include <stdio.h>
#include "smpl_pulse_coding.h"
#include "smpl_defines.h"
#include "smpl_typedef.h"
#include "smpl_entropy_wrapper.h"
#include "smpl_codec_util.h" 
#include "smpl_tables.h" 
#include "smpl_helpers.h"
#include "smpl_pulse_tables.h" 

void smpl_decode_pulse_pos_signs(
    void* ec_dec_st,
    int subfrlen,
    int nSubfr,
    int16_t positions[],
    int16_t pos_pulses[],
    int *nPositionsOut,
    int16_t sf_pulses[]
) {
    smpl_assert(g_smpl_pulse_tables != NULL);
    const PulseCodingTables* pSt = (PulseCodingTables*)g_smpl_pulse_tables;
    ec_ctx* psRange = (ec_ctx*)ec_dec_st;

    // positions
    int nPositions = -1;
    for (int i = 0; i < nSubfr; i++) {
        int pulses_left = sf_pulses[i];
        int n_samples_left = subfrlen;
        int pos = subfrlen * i;  // start of subframe
        for (int j = 0; j < sf_pulses[i]; j++) {
            pulses_left--;
            int CMF_ind = (n_samples_left + RUNLENGTH_STEP - 1) / RUNLENGTH_STEP - 1;
            const uint16_t* cmfp = pSt->runlen_CMFs[CMF_ind].cmfs[pulses_left];
            int max_samples = pSt->runlen_CMFs[CMF_ind].max_samples;
            int ix = smpl_ec_decode_update(psRange, cmfp + max_samples - n_samples_left, n_samples_left + 1);
            if (j == 0 || ix > 0) {
                pos += ix;
                positions[++nPositions] = pos;
                pos_pulses[nPositions] = 1;
                n_samples_left -= ix;
            } else {
                pos_pulses[nPositions] += 1;
            }
        }
   }
    nPositions++;
    // signs
    int signs_decoded = 0;
    while (signs_decoded < nPositions) {
        int signs_in_sym = SMPL_min(nPositions - signs_decoded, MAX_SIGNS_PER_SYMBOL);
        int sym = smpl_ec_decode_uniform(psRange, (uint16_t)1 << signs_in_sym);
        sym <<= ((MAX_SIGNS_PER_SYMBOL+1) - signs_in_sym);
        for (int i = 0; i < signs_in_sym; i++) {
            int sgn = (sym & (uint16_t)0x8000) >> (MAX_SIGNS_PER_SYMBOL-1);
            sym <<= 1;
            pos_pulses[signs_decoded++] *= sgn - 1;
        }
    }
    *nPositionsOut = nPositions;
}
