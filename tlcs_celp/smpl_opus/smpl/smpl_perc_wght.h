#ifndef SMPL_PERC_WGHT_H
#define SMPL_PERC_WGHT_H

#include "smpl_defines.h"
    
#define PERCW_NFFT (512+64)
#define PERCW_FS_KHZ 16

#ifdef __cplusplus
extern "C" {
#endif
    void* smpl_create_perc_model_tables(void);
    void smpl_free_perc_model_tables(void);
    void smpl_perc_model(float* buf, const float xsubfr[], int xsubfr_len, int frame_ms, int is_last_subfr, float R[], int len_R);
    void smpl_perc_ac2a(const float R[], int len_R, const float perc_emph, float A[], int perc_resp_len, float reg);
#ifdef __cplusplus
}
#endif

#endif
