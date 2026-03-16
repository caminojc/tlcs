#ifndef OPUS_SMPL_ENCODE_H
#define OPUS_SMPL_ENCODE_H

#include "opus.h"
#include "opus_types.h"
#include "arch.h"
#include "src/opus_private.h"

#ifdef __cplusplus
extern "C"
{
#endif

opus_int32 opus_smpl_encode_native(OpusEncoder *st, const opus_val16 *pcm, int frame_size,
                unsigned char *data, opus_int32 out_data_bytes, int lsb_depth,
                const void *analysis_pcm, opus_int32 analysis_size, int c1, int c2,
                int analysis_channels, downmix_func downmix, int float_api);

opus_int32 opus_smpl_encode_secondary_native(OpusEncoder* st, unsigned char* data, opus_int32 out_data_bytes);

#ifdef __cplusplus
}
#endif

#endif
