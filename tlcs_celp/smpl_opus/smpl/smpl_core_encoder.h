#ifndef SMPL_CORE_ENCODER_H
#define SMPL_CORE_ENCODER_H

#include "smpl_structs.h"
#include "smpl_api.h"

#ifdef __cplusplus
extern "C" {
#endif

void smpl_core_encoder_init(smpl_core_encoder* enc_state, PitchEstScratch* pitchScratch, CelpScratch* celpScratch);

int smpl_core_encode(
    smpl_core_encoder* enc_state,
    smpl_EncControlStruct* enc_status,
    ec_enc *psRangeEnc,        /* I/O  Compressor data structure                       */
    const opus_int16* x_16b,
    int x_16b_len,
    smpl_vad_status* vad_status,
    smpl_dtx_status* dtx,
    int lowRate);

int smpl_core_encode_second(
    smpl_core_encoder* enc_state,
    smpl_EncControlStruct* enc_status,
    ec_enc* psRangeEnc,        /* I/O Compressor data structure */
    smpl_vad_status* vad_status,
    smpl_dtx_status* dtx,
    int lowRate);

#ifdef __cplusplus
}
#endif

#endif
