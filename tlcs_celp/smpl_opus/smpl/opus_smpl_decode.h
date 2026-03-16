#ifndef OPUS_SMPL_DECODE_H
#define OPUS_SMPL_DECODE_H

#include "opus.h"
#include "opus_types.h"
#include "arch.h"
#include "src/opus_private.h"

#ifdef __cplusplus
extern "C"
{
#endif

typedef struct opus_smpl_TOC_parameters_ {
    int using_smpl;
    int fs;
    int fs_mode;
    int opus_samples_per_frame;
    int stereo;
} opus_smpl_TOC_parameters;

void opus_smpl_decode_TOC(const unsigned char *data, opus_smpl_TOC_parameters *toc_params);

int opus_smpl_get_frames_per_packet(const unsigned char* data, opus_int32 len);

int opus_smpl_packet_parse_impl(const unsigned char *data, opus_int32 len,
      int self_delimited, unsigned char *out_toc,
      const unsigned char *frames[48], opus_int16 size[48],
      int *payload_offset, opus_int32 *packet_offset);

int opus_smpl_decode_frame(OpusDecoder* st, const unsigned char* data,
    opus_int32 len, const unsigned char toc, opus_val16* pcm, int frame_size, int decode_fec);

#ifdef __cplusplus
}
#endif

#endif
