#ifndef SMPL_PITCH_H
#define SMPL_PITCH_H

#include <stdint.h>
#include "smpl_defines.h"
#include "smpl_pitch_tables.h"

#define PITCH_INTERPOL_DELAY_C 4
#define PITCH_INTERPOL_DELAY_E 1
#define PITCH_DOWNSAMP_DELAY 7

#define PITCH_CACHE_BITS_SF 3
#define PITCH_CACHE_BITS_SEG_LEN 3
#define PITCH_CACHE_BITS_BLOCK 4

typedef struct PITCH_blocktrack {
    int track[SMPL_PITCH_NUM_SUBFRAMES];
    float meanblock;
    float trackdeltas;
} PITCH_blocktrack;

typedef struct PITCH_blocksegs {
    int blocks[SMPL_PITCH_NUM_SUBFRAMES];
    int seglens[SMPL_PITCH_NUM_SUBFRAMES];
    int nblocks;
} PITCH_blocksegs;

typedef struct PITCH_data {
    PITCH_blocktrack blocktracks[NUM_BLOCKTRACKS];
    PITCH_blocksegs blocksegs[NUM_BLOCKSEGS];
    int num_blocktracks;
    int num_blocksegs;
    const uint8_t* blocksegs2idx;
    const uint16_t* blockseg_idx_CMF;
    const uint16_t (*delta_lag_CMFs)[LEN_DELTALAG_CMF];
    const uint8_t (*blocksegs_ix)[2];
    const uint8_t (*firstblock_range)[2];
    const int16_t* acbgain_thr_Q14;
    const uint16_t (*block_transition_CMF)[PITCH_NUM_BLOCKS + 1];
    int framelen_ms;
} PITCH_data;

typedef struct PitchEstScratch {
    float C[((2 * SMPL_PITCH_FS_KHZ / SMPL_PITCH_STAGE_1_FS_KHZ) * (SMPL_MAXPITCH_STAGE1 - SMPL_MINPITCH_STAGE1 + 1)) * SMPL_PITCH_NUM_SUBFRAMES];
    float H[((2 * SMPL_PITCH_FS_KHZ / SMPL_PITCH_STAGE_1_FS_KHZ) * (SMPL_MAXPITCH_STAGE1 - SMPL_MINPITCH_STAGE1 + 1)) * SMPL_PITCH_NUM_SUBFRAMES];
    float E1[((2 * SMPL_PITCH_FS_KHZ / SMPL_PITCH_STAGE_1_FS_KHZ) * (SMPL_MAXPITCH_STAGE1 - SMPL_MINPITCH_STAGE1 + 1)) * SMPL_PITCH_NUM_SUBFRAMES];
    float E2[SMPL_PITCH_NUM_SUBFRAMES];
    float E[((2 * SMPL_PITCH_FS_KHZ / SMPL_PITCH_STAGE_1_FS_KHZ) * (SMPL_MAXPITCH_STAGE1 - SMPL_MINPITCH_STAGE1 + 1)) * SMPL_PITCH_NUM_SUBFRAMES];
    float C_coarse[SMPL_PITCH_NUMLAGS_COARSE * SMPL_PITCH_NUM_SUBFRAMES];
    float H_coarse[SMPL_PITCH_NUMLAGS_COARSE * SMPL_PITCH_NUM_SUBFRAMES];
    float E_coarse[SMPL_PITCH_NUMLAGS_COARSE * SMPL_PITCH_NUM_SUBFRAMES];
    float ltp_buf_hp[(2 * SMPL_PITCH_FS_KHZ * 20) + SMPL_MAXPITCH_LEN];
    float ltp_buf_stage1[(2 * SMPL_PITCH_FS_KHZ * 20) + SMPL_MAXPITCH_LEN + 2 * PITCH_DOWNSAMP_DELAY];
    int laginds[NUM_BLOCKSEGS][SMPL_PITCH_NUM_SUBFRAMES];
    int8_t lagind_cache[1 << (PITCH_CACHE_BITS_SF + PITCH_CACHE_BITS_SEG_LEN + PITCH_CACHE_BITS_BLOCK)];
} PitchEstScratch;

typedef struct PitchEstimator{
    PitchEstScratch *scratchMem;
    //float C[((2*SMPL_PITCH_FS_KHZ/SMPL_PITCH_STAGE_1_FS_KHZ)*(SMPL_MAXPITCH_STAGE1 - SMPL_MINPITCH_STAGE1 + 1)) * SMPL_PITCH_NUM_SUBFRAMES];
    //float H[((2 * SMPL_PITCH_FS_KHZ / SMPL_PITCH_STAGE_1_FS_KHZ) * (SMPL_MAXPITCH_STAGE1 - SMPL_MINPITCH_STAGE1 + 1)) * SMPL_PITCH_NUM_SUBFRAMES];
    //float E1[((2 * SMPL_PITCH_FS_KHZ / SMPL_PITCH_STAGE_1_FS_KHZ) * (SMPL_MAXPITCH_STAGE1 - SMPL_MINPITCH_STAGE1 + 1)) * SMPL_PITCH_NUM_SUBFRAMES];
    //float E2[SMPL_PITCH_NUM_SUBFRAMES];
    //float E[((2 * SMPL_PITCH_FS_KHZ / SMPL_PITCH_STAGE_1_FS_KHZ) * (SMPL_MAXPITCH_STAGE1 - SMPL_MINPITCH_STAGE1 + 1)) * SMPL_PITCH_NUM_SUBFRAMES];
    //float C_coarse[SMPL_PITCH_NUMLAGS_COARSE * SMPL_PITCH_NUM_SUBFRAMES];
    //float H_coarse[SMPL_PITCH_NUMLAGS_COARSE * SMPL_PITCH_NUM_SUBFRAMES];
    //float E_coarse[SMPL_PITCH_NUMLAGS_COARSE * SMPL_PITCH_NUM_SUBFRAMES];
    //float ltp_buf_hp[(2 * SMPL_PITCH_FS_KHZ * 20) + SMPL_MAXPITCH_LEN];
    //float ltp_buf_stage1[(2 * SMPL_PITCH_FS_KHZ * 20) + SMPL_MAXPITCH_LEN + 2 * PITCH_DOWNSAMP_DELAY];
    //int laginds[NUM_BLOCKSEGS][SMPL_PITCH_NUM_SUBFRAMES]; 
    //int8_t lagind_cache[1 << (PITCH_CACHE_BITS_SF + PITCH_CACHE_BITS_SEG_LEN + PITCH_CACHE_BITS_BLOCK)];
    float prev_lag;
    float prev_pitch_corr;

    int prev_lagblk;
    int prev_lagidx;

    int offset_end; // Temporar

    int numstates1;
    int low_rate;
    int low_complexity_mode;
    int initialized;
} PitchEstimator;

#ifdef __cplusplus
extern "C" {
#endif

    void* smpl_create_pitch_estimator(void);
    void smpl_init_pitch_estimator(void* st, PitchEstScratch* scratchMem);
    //void smpl_init_pitch_estimator(void* st);
    void smpl_update_pitch_params(void* st, int pitch_numstates1, int low_rate);
    void smpl_delete_pitch_estimator(void* st);

    void* smpl_load_pitch_tables(void);
    void smpl_free_pitch_tables(void);
    const PITCH_data* smpl_get_pitch_data(int numsubfrs);
  
    void smpl_pitch(
        void* st, 
        const float ltp_buf[], 
        int L, 
        int lookAhead,
        float* F2,
        int coded_as_active_voice,
        int numsubfrs,
        float lags[SMPL_PITCH_NUM_SUBFRAMES],
        int *laginds, 
        float *pitchc, 
        int *blockseg_idx,
        float *avg_lag,
        float* harm_strength);

    void smpl_pitch_calc_E1(
        float E1[],
        const float ltpbuf[],
        int ltpbuf_len,
        int numsubfrs,
        int minpitch,
        int maxpitch,
        int lag_subfrlen);

    void smpl_pitch_calc_C_E2(
        float C[],
        float E2[],
        const float ltpbuf[],
        int ltpbuf_len,
        int numsubfrs);

    int smpl_pitch_downsample(PitchEstimator* pSt, const float ltp_buf[], int L);

    void smpl_upsamp_E_core(const float x[], float y[], int len);
    void smpl_upsamp_C_core(const float x[], float y[], int len);
    void smpl_upsamp_E_fast(int numsubfrs, int* minpitch_, int* numlags, float E[]);
    void smpl_upsamp_C_fast(int numsubfrs, int* minpitch_, int* numlags, float C[]);

    void smpl_pitch_reset_cond(void* st);

    // Used to interface with Julia
    int smpl_pitch_get_seglens(int blocksegs_ix_c, int16_t seglens[SMPL_PITCH_NUM_SUBFRAMES], int numsubfrs);
    int smpl_pitch_blocksegs_ix_julia(int blocksegs_ix_c, int numsubfrs);

#ifdef __cplusplus
}
#endif

#endif
