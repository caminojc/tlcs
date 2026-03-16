#include "silk/resampler_private.h"

#ifdef __cplusplus
extern "C" {
#endif
void* smpl_silk_resampler_wrapper_create(opus_int32 Fs_Hz_in, opus_int32 Fs_Hz_out);
void smpl_silk_resampler_wrapper_free(void* S);
opus_int32 smpl_resampler_get_fs_in(silk_resampler_state_struct* S);
opus_int32 smpl_resampler_get_fs_out(silk_resampler_state_struct* S);
#ifdef __cplusplus
}
#endif
