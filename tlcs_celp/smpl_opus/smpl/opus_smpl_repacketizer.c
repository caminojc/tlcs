#include "opus_smpl_repacketizer.h"
#include "smpl_typedef.h"
#include "opus_smpl_decode.h"

/*
    MLow Repacketizer Design Details

Opus has first class support to pack multiple encoded frames into one Opus
packet. Opus TOC itself has the support to indicate 1/2/multiple frames in
the packet and the Opus repacketizer basically just exploits that feature.

MLow similarly has a way to indicate if a packet is single or multiframe by
exploiting an unused combination of bits in the TOC byte (SID = 1, FEC = 1).

One key difference is that Opus TOC is very stable across the frames as it
doesn't contain volatile information like VAD state or inband FEC existence
etc in the TOC header. As a result Opus repacketizer is able to common out
the TOC information and encode that only once in the repacketized packet. All
frames in the packet are deemed to have the same exact TOC byte.

Whereas in case of MLow, the TOC byte is very dynamic because it encodes much
more state and hence we DO NOT share the TOC among the frames being packetized.
But we still enfoce that the basic encoder properties remain the same like
FrameSize, SamplingRate, No. of Channels etc.

So the first byte in MLow is an overall dummy TOC byte, which communicates the
key fixed characteristics of encoder but each frame has it's original TOC
intact and packed in the data chunk. This difference causes the decoder to
carefully find the TOC while processing Opus vs MLow multiframe packets.
*/

/**
 * Method to concat more MLow frames/packets to the repacketizer.
 * It has the following constraints:
 * - All frames should be CELT or MLow mode. If mode change is detected
 *   then cat operation fails.
 * - Upto 18 frames are allowed to be packaged for MLow, 48 for CELT
 * - For MLow we additionally check that the {Fs, FrameSz, Rate} bits
 *   haven't changed. It is not a hard requirement for concat to work
 *   but for now we enforce it.
 */
int opus_smpl_repacketizer_cat_impl(
    OpusRepacketizer* rp,
    const unsigned char* data,
    opus_int32 len,
    int self_delimited) {
  if (len < 1) {
    return OPUS_INVALID_PACKET;
  }

  // Check for TOC change b/w MLow <-> CELT mode.
  if (rp->nb_frames > 0 && (is_mlow_celt(rp->toc) != is_mlow_celt(data[0]))){
    return OPUS_INVALID_PACKET;
  }

  const int is_mlow_mode = (data[0] >> 6) != 3;
  const int curr_nb_frames = rp->nb_frames;
  const int new_frames = opus_smpl_packet_parse_impl(
      data, len, self_delimited, &rp->toc, &rp->frames[rp->nb_frames], &rp->len[rp->nb_frames], NULL, NULL);

  if (new_frames < 1) {
    return OPUS_INVALID_PACKET;
  }

  if (is_mlow_mode) {
    // Make sure we have no. of frames upto allowed maximum.
    if (rp->nb_frames + new_frames > MAX_MLOW_FRAMES_PER_PACKET) {
      return OPUS_INVALID_PACKET;
    }

    // Validations for the cases when concatenation is allowed.
    for (int i = 1; rp->nb_frames > 0 && i < rp->nb_frames + new_frames; i++) {
      if ((rp->toc & MLOW_TOC_FIXED_MASK) != (rp->frames[i][0] & MLOW_TOC_FIXED_MASK)) {
        // MLow core TOC bits have changed.
        return OPUS_INVALID_PACKET;
      }
    }
  } else {
    // Make sure CELT TOC hasn't changed.
    if (rp->nb_frames > 0 && ((rp->toc & 0xFE) != (data[0] & 0xFE))) {
      return OPUS_INVALID_PACKET;
    }
  }

  if (curr_nb_frames == 0) {
    rp->framesize = mlow_packet_get_samples_per_frame(data, 8000);
  }
  rp->nb_frames += new_frames;

  return OPUS_OK;
}

int opus_smpl_repacketizer_cat(
    OpusRepacketizer* rp,
    const unsigned char* data,
    opus_int32 len) {
  return opus_smpl_repacketizer_cat_impl(rp, data, len, 0);
}

static opus_int32 opus_smpl_repacketizer_out_range_mlow_impl(
    OpusRepacketizer* rp,
    int begin,
    int end,
    unsigned char* data,
    opus_int32 maxlen,
    int self_delimited) {
  int count = end - begin;

  opus_int16* len = rp->len + begin;
  const unsigned char** frames = rp->frames + begin;
  unsigned char* ptr = data;

  // Handle single frame MLow packet here.
  if (count == 1) {
    /* Single frame MLow packet */
    if (len[0] > maxlen) {
      return OPUS_BUFFER_TOO_SMALL;
    }
    OPUS_MOVE(ptr, frames[0], len[0]);
    return len[0];
  }

  // Multiframe MLow packet.
  if (count > MAX_MLOW_FRAMES_PER_PACKET) {
    return OPUS_BAD_ARG;
  }

  // 1. Make sure we have enough space to repack.
  opus_int32 tot_size = 0;
  tot_size += 2; // 1 Byte multiframe TOC, 1 Byte for frame count
  for (int i = 0; i < count; i++) {
    tot_size += 1 + (len[i] >= 252); // 1 or 2 bytes for size
    tot_size += len[i]; // size of actual buffer
  }
  if (!self_delimited) { // Remove size bytes if not self delimited
    tot_size = tot_size - 1 - (len[count - 1] >= 252);
  }

  if (tot_size > maxlen) {
    return OPUS_BUFFER_TOO_SMALL;
  }

  // 2. Encode the multiframe packet here.
  *ptr++ = MLOW_MULTI_TOC_MASK | (rp->toc & MLOW_TOC_FIXED_MASK);
  *ptr++ = count;
  for (int i = 0; i < count - 1; i++) { // encode N-1 sizes
    ptr += encode_size(len[i], ptr);
  }
  if (self_delimited) { // encode Nth size if self delimited
    ptr += encode_size(len[count - 1], ptr);
  }
  for (int i = 0; i < count; i++) { // Move payload buffer.
    OPUS_MOVE(ptr, frames[i], len[i]);
    ptr += len[i];
  }

  smpl_assert(ptr - data == tot_size);
  return tot_size;
}

// This method is an exact copy of the corresponding method
// `opus_repacketizer_out_range_impl` in Opus repacketizer.c file.
// TODO: Possibly refactor a bit and reuse from repacketizer.c
static opus_int32 opus_smpl_repacketizer_out_range_celt_impl(
    OpusRepacketizer* rp,
    int begin,
    int end,
    unsigned char* data,
    opus_int32 maxlen,
    int self_delimited,
    int pad) {
    int i, count;
    opus_int32 tot_size;
    opus_int16* len;
    const unsigned char** frames;
    unsigned char* ptr;

    if (begin<0 || begin >= end || end>rp->nb_frames)
    {
        /*fprintf(stderr, "%d %d %d\n", begin, end, rp->nb_frames);*/
        return OPUS_BAD_ARG;
    }
    count = end - begin;

    len = rp->len + begin;
    frames = rp->frames + begin;
    if (self_delimited)
        tot_size = 1 + (len[count - 1] >= 252);
    else
        tot_size = 0;

    ptr = data;
    if (count == 1)
    {
        /* Code 0 */
        tot_size += len[0] + 1;
        if (tot_size > maxlen)
            return OPUS_BUFFER_TOO_SMALL;
        *ptr++ = rp->toc & 0xFC;
    }
    else if (count == 2)
    {
        if (len[1] == len[0])
        {
            /* Code 1 */
            tot_size += 2 * len[0] + 1;
            if (tot_size > maxlen)
                return OPUS_BUFFER_TOO_SMALL;
            *ptr++ = (rp->toc & 0xFC) | 0x1;
        }
        else {
            /* Code 2 */
            tot_size += len[0] + len[1] + 2 + (len[0] >= 252);
            if (tot_size > maxlen)
                return OPUS_BUFFER_TOO_SMALL;
            *ptr++ = (rp->toc & 0xFC) | 0x2;
            ptr += encode_size(len[0], ptr);
        }
    }
    if (count > 2 || (pad && tot_size < maxlen))
    {
        /* Code 3 */
        int vbr;
        int pad_amount = 0;

        /* Restart the process for the padding case */
        ptr = data;
        if (self_delimited)
            tot_size = 1 + (len[count - 1] >= 252);
        else
            tot_size = 0;
        vbr = 0;
        for (i = 1; i < count; i++)
        {
            if (len[i] != len[0])
            {
                vbr = 1;
                break;
            }
        }
        if (vbr)
        {
            tot_size += 2;
            for (i = 0; i < count - 1; i++)
                tot_size += 1 + (len[i] >= 252) + len[i];
            tot_size += len[count - 1];

            if (tot_size > maxlen)
                return OPUS_BUFFER_TOO_SMALL;
            *ptr++ = (rp->toc & 0xFC) | 0x3;
            *ptr++ = count | 0x80;
        }
        else {
            tot_size += count * len[0] + 2;
            if (tot_size > maxlen)
                return OPUS_BUFFER_TOO_SMALL;
            *ptr++ = (rp->toc & 0xFC) | 0x3;
            *ptr++ = count;
        }
        pad_amount = pad ? (maxlen - tot_size) : 0;
        if (pad_amount != 0)
        {
            int nb_255s;
            data[1] |= 0x40;
            nb_255s = (pad_amount - 1) / 255;
            for (i = 0; i < nb_255s; i++)
                *ptr++ = 255;
            *ptr++ = pad_amount - 255 * nb_255s - 1;
            tot_size += pad_amount;
        }
        if (vbr)
        {
            for (i = 0; i < count - 1; i++)
                ptr += encode_size(len[i], ptr);
        }
    }
    if (self_delimited) {
        int sdlen = encode_size(len[count - 1], ptr);
        ptr += sdlen;
    }
    /* Copy the actual data */
    for (i = 0; i < count; i++)
    {
        /* Using OPUS_MOVE() instead of OPUS_COPY() in case we're doing in-place
           padding from opus_packet_pad or opus_packet_unpad(). */
           /* assert disabled because it's not valid in C. */
           /* celt_assert(frames[i] + len[i] <= data || ptr <= frames[i]); */
        OPUS_MOVE(ptr, frames[i], len[i]);
        ptr += len[i];
    }
    if (pad)
    {
        /* Fill padding with zeros. */
        while (ptr < data + maxlen)
            *ptr++ = 0;
    }
    return tot_size;
}


/**
 * Retrieve a single or multi frame packet from repacketized buffer.
 * If the current repacketizer state has N frames then one can request
 * a new packet with a subrange from 0 <= x, y <= N.
 * - If no. of frames requested (y-x) is 1 and the underlying frames are
 *   MLow frames, then we return a single valid MLow packet.
 * - If no. of frames requested is > 1 and underlying frames are MLow then
 *   we return a valid multiframe MLow packet.
 * - If the underlying frames are CELT, then CELT encodes it accordingly
 *   following the original Opus RFC, implementation is copied from
 *   repacketizer.c
 */
opus_int32 opus_smpl_repacketizer_out_range_impl(
    OpusRepacketizer* rp,
    int begin,
    int end,
    unsigned char* data,
    opus_int32 maxlen,
    int self_delimited,
    int pad) {
  if (begin < 0 || begin >= end || end > rp->nb_frames) {
    return OPUS_BAD_ARG;
  }
  if (is_mlow_celt(rp->toc)) {
    int size = opus_smpl_repacketizer_out_range_celt_impl(
        rp, begin, end, data, maxlen, self_delimited, pad);
    // Recover toc byte and format it for MLow packet. If it is a multi frame
    // packet then set last bit to 1, else set to 0. This adheres to the MLow
    // packet format described in `gen_smpl_toc()` in opus_smpl_encode.c
    data[0] = (end - begin > 1) ? (rp->toc | 0x1) : (rp->toc & 0xFE);
    return size;
  }
  return opus_smpl_repacketizer_out_range_mlow_impl(
      rp, begin, end, data, maxlen, self_delimited);
}
