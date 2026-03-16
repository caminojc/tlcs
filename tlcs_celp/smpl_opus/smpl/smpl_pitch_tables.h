#include <stdint.h> 
#include "smpl_defines.h" 

#ifdef __cplusplus
extern "C" {
#endif
#define PITCH_NUM_BLOCKS (SMPL_MAXPITCH_MS - SMPL_MINPITCH_MS)/SMPL_PITCHBLOCK_MS 
#define NUM_BLOCKTRACKS 187
extern const uint8_t smpl_pitch_blocksegs_ix[NUM_BLOCKTRACKS][2];
#define NUM_BLOCKTRACKS_10 37
extern const uint8_t smpl_pitch_blocksegs_ix_10[NUM_BLOCKTRACKS_10][2];
#define NUM_BLOCKSEGS 217
#define NUM_BLOCKSEGS_BYTES 676
extern const uint8_t smpl_pitch_blocksegs[NUM_BLOCKSEGS_BYTES];
extern const uint8_t smpl_pitch_blocksegs2idx[NUM_BLOCKSEGS];
extern const uint8_t smpl_pitch_firstblock_range[PITCH_NUM_BLOCKS][2];
extern const uint8_t smpl_pitch_blockseg_idx_DCMF_20[NUM_BLOCKSEGS];
#define NUM_BLOCKSEGS_10 44
#define NUM_BLOCKSEGS_10_BYTES 82
extern const uint8_t smpl_pitch_blocksegs_10[NUM_BLOCKSEGS_10_BYTES];
extern const uint8_t smpl_pitch_blocksegs2idx_10[NUM_BLOCKSEGS_10];
extern const uint8_t smpl_pitch_firstblock_range_10[PITCH_NUM_BLOCKS][2];
extern const uint8_t smpl_pitch_blockseg_idx_DCMF_10[NUM_BLOCKSEGS_10];
extern const uint8_t smpl_pitch_block_transition_DCMF_20[PITCH_NUM_BLOCKS][PITCH_NUM_BLOCKS];
extern const int16_t smpl_pitch_acbgain_thr_10_Q14[2];
extern const int16_t smpl_pitch_acbgain_thr_20_Q14[2];
#define N_DELTALAG_CMFS 3
#define LEN_DELTALAG_CMF 320
extern const uint8_t smpl_pitch_delta_lag_DCMFs[N_DELTALAG_CMFS][LEN_DELTALAG_CMF-1];
#ifdef __cplusplus
}
#endif
