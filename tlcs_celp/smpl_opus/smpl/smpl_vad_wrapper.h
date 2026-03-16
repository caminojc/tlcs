#ifndef SMPL_VAD_WRAPPER_H
#define SMPL_VAD_WRAPPER_H

#include "silk/main.h"
#include "src/analysis.h"

#ifdef __cplusplus
extern "C" {
#endif
void* smpl_silk_vad_wrapper_create(void);
void smpl_silk_vad_wrapper_free(void* silk_vad_st);
int smpl_silk_vad_wrapper(void* silk_vad_st, opus_int16* x, opus_int x_len);

typedef struct opus_vad_state{
    CELTEncoder *celt_enc;
    const CELTMode *celt_mode;
    TonalityAnalysisState *tonality_state;
} opus_vad_state;

void* smpl_opus_vad_wrapper_create(opus_int32 fs);
void smpl_opus_vad_wrapper_free(void* opus_vad_st);
float smpl_opus_vad_wrapper(void* opus_vad_st, opus_int16* x, int frame_len, int fs);
#ifdef __cplusplus
}
#endif

#endif
