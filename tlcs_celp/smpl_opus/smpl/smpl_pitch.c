#include "smpl_codec_util.h"
#include "smpl_defines.h"
#include "smpl_filt.h"
#include "smpl_param_coding.h"
#include "smpl_pitch.h"
#include "smpl_pitch_tables.h"
#include "smpl_entropy_wrapper.h"
#include "smpl_helpers.h"
#include "smpl_typedef.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "silk/debug.h"

typedef struct PITCH_Tables {
    PITCH_data data10ms;
    PITCH_data data20ms;
    uint16_t blockseg_idx_CMF_buf[NUM_BLOCKSEGS + NUM_BLOCKSEGS_10 + 2];
    uint16_t delta_lag_CMFs[N_DELTALAG_CMFS][LEN_DELTALAG_CMF];
    uint16_t block_transition_CMF_20[PITCH_NUM_BLOCKS][PITCH_NUM_BLOCKS+1];
} PITCH_Tables;

static void* g_smpl_pitch_CBks = NULL;

#define BITS_PER_SF 4
#define MASK ((1<<BITS_PER_SF)-1)

void smpl_gen_blocktrack(const PITCH_blocksegs* blocksegs, PITCH_blocktrack* block_tracks, int num_tracks, int pitch_num_subfr, const uint8_t* blocksegs_ix) {
    for (int track_idx = 0; track_idx < num_tracks; track_idx++) {
        const PITCH_blocksegs* cur_seg = blocksegs + blocksegs_ix[2 * track_idx];
        int seg_idx = 0;
        for (int block_idx = 0; block_idx < cur_seg->nblocks; block_idx++) {
            for (int k = 0; k < cur_seg->seglens[block_idx]; k++) {
                block_tracks[track_idx].track[seg_idx] = cur_seg->blocks[block_idx];
                seg_idx += 1;
            }
            block_tracks[track_idx].meanblock += cur_seg->blocks[block_idx] * cur_seg->seglens[block_idx];
            if (block_idx == 0) {
                continue;
            }
            block_tracks[track_idx].trackdeltas += SMPL_abs(cur_seg->blocks[block_idx - 1] - cur_seg->blocks[block_idx]);
        }
        block_tracks[track_idx].meanblock /= (float)pitch_num_subfr;
    }
}

int decode_blocksegs(ec_dec* _this, int blocks[], int seglens[])
{
#define N_LEN 6
#define N_BLOCK 9
#define N_SEGLEN 4
    int len = smpl_ec_decode_uniform(_this, N_LEN) + 1;
    for (int i = 0; i < len; i++) {
        blocks[i] = smpl_ec_decode_uniform(_this, N_BLOCK);
        seglens[i] = smpl_ec_decode_uniform(_this, N_SEGLEN) + 1;
    }
    return len;
}

void* smpl_load_pitch_tables(void)
{
    if (g_smpl_pitch_CBks) {
        return g_smpl_pitch_CBks;
    }
    PITCH_Tables* pSt = (PITCH_Tables*)calloc(1, sizeof(PITCH_Tables));
    if (!pSt) {
        smpl_assert(0);
        return NULL;
    }
    // Generate data for 20 ms frames
    pSt->data20ms.num_blocksegs = NUM_BLOCKSEGS;
    ec_dec decoder;
    ec_dec_init(&decoder, (uint8_t*)smpl_pitch_blocksegs, NUM_BLOCKSEGS_BYTES);
    for (int i = 0; i < NUM_BLOCKSEGS; i++) {
        pSt->data20ms.blocksegs[i].nblocks = decode_blocksegs(&decoder, pSt->data20ms.blocksegs[i].blocks, pSt->data20ms.blocksegs[i].seglens);
    }
    pSt->data20ms.num_blocktracks = NUM_BLOCKTRACKS;
    smpl_gen_blocktrack(pSt->data20ms.blocksegs, pSt->data20ms.blocktracks, NUM_BLOCKTRACKS, SMPL_PITCH_NUM_SUBFRAMES, &smpl_pitch_blocksegs_ix[0][0]);
    //pSt->data20ms.enc_blocksegs = smpl_pitch_blocksegs;
    pSt->data20ms.blocksegs2idx = smpl_pitch_blocksegs2idx;
    smpl_dcmf_to_cmf(smpl_pitch_blockseg_idx_DCMF_20, NUM_BLOCKSEGS, pSt->blockseg_idx_CMF_buf);
    pSt->data20ms.blockseg_idx_CMF = pSt->blockseg_idx_CMF_buf;
    for (int i = 0; i < N_DELTALAG_CMFS; i++) {
        smpl_dcmf_to_cmf(smpl_pitch_delta_lag_DCMFs[i], LEN_DELTALAG_CMF - 1, pSt->delta_lag_CMFs[i]);
    }
    pSt->data20ms.delta_lag_CMFs = pSt->delta_lag_CMFs;          
    pSt->data20ms.blocksegs_ix = smpl_pitch_blocksegs_ix;
    pSt->data20ms.firstblock_range = smpl_pitch_firstblock_range;
    pSt->data20ms.acbgain_thr_Q14 = smpl_pitch_acbgain_thr_20_Q14;  // Probably not needed for both 10 and 20 ms
    for (int i = 0; i < PITCH_NUM_BLOCKS; i++) {
        smpl_dcmf_to_cmf(smpl_pitch_block_transition_DCMF_20[i], PITCH_NUM_BLOCKS, pSt->block_transition_CMF_20[i]);
    }
    pSt->data20ms.block_transition_CMF = pSt->block_transition_CMF_20;
    pSt->data20ms.framelen_ms = 20;

    // Generate data for 10 ms frames
    pSt->data10ms.num_blocksegs = NUM_BLOCKSEGS_10;
    ec_dec_init(&decoder, (uint8_t*)smpl_pitch_blocksegs_10, NUM_BLOCKSEGS_10_BYTES);
    for (int i = 0; i < NUM_BLOCKSEGS_10; i++) {
        pSt->data10ms.blocksegs[i].nblocks = decode_blocksegs(&decoder, pSt->data10ms.blocksegs[i].blocks, pSt->data10ms.blocksegs[i].seglens);
    }
    pSt->data10ms.num_blocktracks = NUM_BLOCKTRACKS_10;
    smpl_gen_blocktrack(pSt->data10ms.blocksegs, pSt->data10ms.blocktracks, NUM_BLOCKTRACKS_10, SMPL_PITCH_NUM_SUBFRAMES / 2, &smpl_pitch_blocksegs_ix_10[0][0]);
    //pSt->data10ms.enc_blocksegs = smpl_pitch_blocksegs_10;
    pSt->data10ms.blocksegs2idx = smpl_pitch_blocksegs2idx_10;
    smpl_dcmf_to_cmf(smpl_pitch_blockseg_idx_DCMF_10, NUM_BLOCKSEGS_10, &pSt->blockseg_idx_CMF_buf[NUM_BLOCKSEGS+1]);
    pSt->data10ms.blockseg_idx_CMF = &pSt->blockseg_idx_CMF_buf[NUM_BLOCKSEGS + 1];
    pSt->data10ms.delta_lag_CMFs = pSt->delta_lag_CMFs;          
    pSt->data10ms.blocksegs_ix = smpl_pitch_blocksegs_ix_10;
    pSt->data10ms.firstblock_range = smpl_pitch_firstblock_range_10;
    pSt->data10ms.acbgain_thr_Q14 = smpl_pitch_acbgain_thr_10_Q14;  // Probably not needed for both 10 and 20 ms 
    pSt->data10ms.block_transition_CMF = NULL;                      // 10 ms frames cant be used in multiframe packets
    pSt->data10ms.framelen_ms = 10;

    g_smpl_pitch_CBks = (void*)pSt;
    return g_smpl_pitch_CBks;
}

void smpl_free_pitch_tables(void)
{
    PITCH_Tables* pSt = (PITCH_Tables*)g_smpl_pitch_CBks;
    if (pSt) {
        free(pSt);
        g_smpl_pitch_CBks = NULL;
    }
}

const PITCH_data* smpl_get_pitch_data(int numsubfrs) {
    PITCH_Tables* pSt = (PITCH_Tables*)g_smpl_pitch_CBks;
    if (!pSt) {
        smpl_assert(0);
        return NULL;
    }
    smpl_assert(numsubfrs == SMPL_PITCH_NUM_SUBFRAMES || numsubfrs == (SMPL_PITCH_NUM_SUBFRAMES/2));
    return (numsubfrs == SMPL_PITCH_NUM_SUBFRAMES) ? &pSt->data20ms : &pSt->data10ms;
}

void *smpl_create_pitch_estimator(void)
{
    PitchEstimator *pSt = (PitchEstimator*)calloc(1,sizeof(PitchEstimator));
    if(!pSt){
        return NULL;
    }

    int tot_interp_delay = PITCH_INTERPOL_DELAY_C;
    for(int i = 1; i < SMPL_PITCH_UPSAMP_STAGES; i++){
        tot_interp_delay = (tot_interp_delay / 2) + PITCH_INTERPOL_DELAY_C;
    }
    smpl_assert(SMPL_PITCH_TOT_INTERP_DELAY == tot_interp_delay);
    smpl_assert(SMPL_PITCH_FS_KHZ == SMPL_PITCH_STAGE_1_FS_KHZ * (int)powf(2, SMPL_PITCH_DOWNSAMP_STAGES));

    PitchEstScratch* pScratch = (PitchEstScratch*)calloc(1, sizeof(PitchEstScratch));
    if (!pScratch) {
        free(pScratch);
        return NULL;
    }

    smpl_init_pitch_estimator((void*)pSt, pScratch);

    return (void*)pSt;
}

void smpl_init_pitch_estimator(void* st, PitchEstScratch* scratchMem)
{
    PitchEstimator* pSt = (PitchEstimator*)st;
    pSt->prev_lag = 0.0f;
    pSt->prev_pitch_corr = 0.0f;
    pSt->prev_lagblk = -1;
    pSt->prev_lagidx = -1;
    pSt->initialized = SMPL_TRUE;
    pSt->scratchMem = scratchMem;
    smpl_update_pitch_params(st, 8, SMPL_FALSE);
}

void smpl_update_pitch_params(void* st, int pitch_numstates1, int low_rate)
{
    PitchEstimator* pSt = (PitchEstimator*)st;
    smpl_assert(low_rate == SMPL_TRUE || low_rate == SMPL_FALSE);
    pSt->numstates1 = pitch_numstates1;
    pSt->low_complexity_mode = pitch_numstates1 <= 4 ? SMPL_TRUE : SMPL_FALSE;
    pSt->low_rate = low_rate;
}

void smpl_pitch_reset_cond(void* st)
{
    PitchEstimator* pSt = (PitchEstimator*)st;
    smpl_assert(pSt != NULL);
    pSt->prev_lagblk = -1;
    pSt->prev_lagidx = -1;
}

int smpl_pitch_blocksegs_ix_julia(int blocksegs_ix_c, int numsubfrs)
{
    uint8_t blocksegs_ix_julia = (numsubfrs == SMPL_PITCH_NUM_SUBFRAMES) ? smpl_pitch_blocksegs2idx[blocksegs_ix_c] : smpl_pitch_blocksegs2idx_10[blocksegs_ix_c];
    return blocksegs_ix_julia;
}

int smpl_pitch_get_seglens(int blocksegs_ix_c, int16_t seglens[SMPL_PITCH_NUM_SUBFRAMES], int numsubfrs)
{
    const PITCH_data* pData = smpl_get_pitch_data(numsubfrs);
    int ret = pData->blocksegs[blocksegs_ix_c].nblocks;
    for (int i = 0; i < ret; i++) {
        seglens[i] = (int16_t)pData->blocksegs[blocksegs_ix_c].seglens[i];
    }
    return ret;
}

void smpl_delete_pitch_estimator(void* st)
{
    PitchEstimator *pSt = (PitchEstimator*)st;
    if (pSt->scratchMem) {
        free(pSt->scratchMem);
    }
    free(pSt);
}
