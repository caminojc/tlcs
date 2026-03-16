#ifndef OPUS_SMPL_REPACKETIZER_H
#define OPUS_SMPL_REPACKETIZER_H

#include "opus.h"
#include "opus_types.h"
#include "os_support.h"

#ifdef __cplusplus
extern "C"
{
#endif

// We want to pack upto 180ms for now. Minimum frame size is 10ms.
// This is sort of artifical limit to keep things sensible but we
// can represent upto 256 frames in the 1 byte space for no. of frames.
#define MAX_MLOW_FRAMES_PER_PACKET 18

// We use SID and FEC bits set to 1 to represent that it is a multiframe
// MLow packet.
#define MLOW_MULTI_TOC_MASK 0x82 // SID & FEC bits set.

// Bitmask for bits which are enforced to be same across all the frames
// being packaged. A change in this is considered a TOC change for MLow.
#define MLOW_TOC_FIXED_MASK 0x39  // {Fs, FrameSz, Ste} bits.

static inline int is_mlow_celt(const unsigned char first_byte) {
  return ((first_byte >> 6) == 3) ? 1 : 0;
}

int opus_smpl_repacketizer_cat(OpusRepacketizer* rp, const unsigned char* data, opus_int32 len);

int opus_smpl_repacketizer_cat_impl(
    OpusRepacketizer* rp,
    const unsigned char* data,
    opus_int32 len,
    int self_delimited);

opus_int32 opus_smpl_repacketizer_out_range_impl(
    OpusRepacketizer* rp, int begin, int end,
    unsigned char* data, opus_int32 maxlen, int self_delimited, int pad);

#ifdef __cplusplus
}
#endif

#endif
