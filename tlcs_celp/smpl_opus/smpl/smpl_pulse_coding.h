#ifndef SMPL_PULSE_CODING_H
#define SMPL_PULSE_CODING_H

#include <stdint.h>
#include "smpl_defines.h"

#define RUNLENGTH_STEP   8
#define NUM_RUNLEN_CMFS  (SMPL_MAX_SF_LEN / RUNLENGTH_STEP)
#define MAX_SIGNS_PER_SYMBOL 15

typedef struct RunLenCMFs {
    uint16_t* cmfs[SMPL_MAX_PULSES_PER_SF];
    int max_samples;
} RunLenCMFs;

typedef struct CmfVec {
    uint16_t* cmf;
    int cmfLen;
} CmfVec;

typedef struct SplitCMFs {
    uint16_t* cmfBuf;
    CmfVec cmfVecs[4 * SMPL_MAX_PULSES_PER_SF];
    uint16_t* cmfs[4 * SMPL_MAX_PULSES_PER_SF];
    uint16_t cmfLen[4 * SMPL_MAX_PULSES_PER_SF];
} SplitCMFs;

typedef struct PulseCodingTables {
    uint16_t* RunLenCmfBuf;
    RunLenCMFs runlen_CMFs[NUM_RUNLEN_CMFS];
    uint16_t* SplitCmfBuf;
    CmfVec split_CMFs[4 * SMPL_MAX_PULSES_PER_SF];
    CmfVec n_pulse_cmfs[3];
} PulseCodingTables;

#ifdef __cplusplus
extern "C" {
#endif

extern void* g_smpl_pulse_tables;

int32_t smpl_stirling(int32_t n);
int32_t smpl_prob_split_fast(int32_t k, int32_t N);

void* smpl_create_pulse_tables(void);
void smpl_free_pulse_tables(void);

void smpl_encode_pulses(
    void* ec_range_enc, 
    const int16_t pulses[SMPL_FRAME_LEN],
    int framelen,
    int nSubfr,
    int lowRate,
    int Voiced, 
    int coded_as_active_voice,
    int16_t sf_pulses[]);

void smpl_decode_pulses(
    void* ec_dec_st,
    int framelen,
    int nSubfr,
    int lowRate,
    int Voiced,
    int coded_as_active_voice,
    int16_t positions[],
    int16_t pos_pulses[],
    int *nPositions,
    int *n_pulses,
    int16_t sf_pulses[]);

void smpl_decode_pulse_pos_signs(
    void* ec_dec_st,
    int subfrlen,
    int nSubfr,
    int16_t positions[],
    int16_t pos_pulses[],
    int *nPositionsOut,
    int16_t sf_pulses[]);

#ifdef __cplusplus
}
#endif

#endif
