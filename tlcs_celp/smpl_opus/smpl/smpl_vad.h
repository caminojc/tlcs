#ifndef SMPL_VAD_H
#define SMPL_VAD_H

#include "smpl_structs.h"

#ifdef __cplusplus
extern "C"
{
#endif

// Coming in externally from analyzer
#define SMPL_OPUS_VAD_NO_DECISION                         -1
#define SMPL_OPUS_VAD_NO_ACTIVITY                         0
#define SMPL_OPUS_VAD_ACTIVITY                            1

opus_int smpl_VAD_Init(                                         /* O    Return value, 0 if success                  */
    smpl_VAD_state              *psSilk_VAD                     /* I/O  Pointer to Silk VAD state                   */
);

opus_int smpl_VAD_GetSA_Q8_c(                                   /* O    Return value, 0 if success                  */
    smpl_encoder                *psEncC,                        /* I/O  Encoder state                               */
    const opus_int16            pIn[],                          /* I    PCM input                                   */
    const opus_int              framelen                        /* I    Number of PCM samples                       */
);

void update_vad_dtx_status(
    smpl_encoder* enc,
    smpl_EncControlStruct* enc_status,
    smpl_vad_status* vad,
    const int fs,
    const opus_int16* samplesIn,
    const int nSamplesIn,
    const int framelen,
    const int frames_per_packet,
    const int activity);

#ifdef __cplusplus
}
#endif

#endif
