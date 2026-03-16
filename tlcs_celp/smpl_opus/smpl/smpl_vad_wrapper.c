#include <stdlib.h>
#include <stdio.h>
#include "smpl_vad_wrapper.h"
#include "smpl_structs.h"
#include "smpl_vad.h"

void* smpl_silk_vad_wrapper_create(void)
{
    smpl_encoder* silk_vad_st = (smpl_encoder*)malloc(sizeof(smpl_encoder));
    memset(silk_vad_st, 0, sizeof(smpl_encoder));
    smpl_VAD_Init(&(silk_vad_st->sVAD));
    return silk_vad_st;
}

void smpl_silk_vad_wrapper_free(void* silk_vad_st)
{
    free(silk_vad_st);
}

int smpl_silk_vad_wrapper(void* silk_vad_st, opus_int16* x, opus_int x_len)
{
    smpl_encoder* silk_state = (smpl_encoder*) silk_vad_st;
    smpl_VAD_GetSA_Q8_c(silk_state, x, x_len);
    return silk_state->speech_activity_Q8;
}


void* smpl_opus_vad_wrapper_create(opus_int32 fs)
{
    opus_vad_state* opus_vad_st = (opus_vad_state*)malloc(sizeof(opus_vad_state));
    opus_vad_st->celt_enc = (CELTEncoder*)malloc(celt_encoder_get_size(1));
    celt_encoder_init(opus_vad_st->celt_enc, fs, 1, opus_select_arch());
    // int err = celt_encoder_init(opus_vad_st->celt_enc, fs, 1, opus_select_arch());
    // if (err != OPUS_OK)
    //     printf("Error creating celt: %i\n", err);

    celt_encoder_ctl(opus_vad_st->celt_enc, CELT_GET_MODE(&(opus_vad_st->celt_mode)));
    opus_vad_st->tonality_state = (TonalityAnalysisState*)malloc(sizeof(TonalityAnalysisState));
    tonality_analysis_init(opus_vad_st->tonality_state, fs);
    return opus_vad_st;
}

void smpl_opus_vad_wrapper_free(void* opus_vad_st)
{
    free(((opus_vad_state*)opus_vad_st)->celt_enc);
    free(((opus_vad_state*)opus_vad_st)->tonality_state);
    free(opus_vad_st);
}

float smpl_opus_vad_wrapper(void* opus_vad_st, opus_int16* x, int frame_len, int fs)
{
    AnalysisInfo analysis_info;
    int analysis_size = frame_len;
    int channels = 1;
    int lsb_depth = 24;
    run_analysis(((opus_vad_state*)opus_vad_st)->tonality_state, ((opus_vad_state*)opus_vad_st)->celt_mode, x, 
                analysis_size, frame_len, 0, -2, channels, fs, lsb_depth, downmix_int, &analysis_info);
    // printf("activity %f activity_prob %f bandwidth %i music_prob %f\n", 
    //     analysis_info.activity, analysis_info.activity_probability, analysis_info.bandwidth, analysis_info.music_prob);
    return analysis_info.activity_probability;
}
