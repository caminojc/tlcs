#include "opus_smpl_decode.h"
#include "smpl_param_coding.h"
#include "stack_alloc.h"
#include "silk/define.h"
#ifdef FIXED_POINT
#include "fixed/structs_FIX.h"
#else
#include "float/structs_FLP.h"
#endif
#include "float_cast.h"
#include "modes.h"
#include "mathops.h"
#include "smpl_typedef.h"
#include "smpl_api.h"
#include "silk/debug.h"
#include "opus_smpl_repacketizer.h"

#if defined(ENABLE_SMPL)

struct OpusDecoder {
    int          celt_dec_offset;
    int          channels;
    opus_int32   Fs;          /** Sampling rate (at the API level) */
    silk_DecControlStruct DecControl;
    int          silk_dec_offset;
#if defined(ENABLE_SMPL)
    smpl_DecControlStruct smpl_DecControl;
    int          smpl_dec_offset;
#endif
    int          use_smpl;
    int          decode_gain;
    int          arch;

    /* Everything beyond this point gets cleared on a reset */
#define OPUS_DECODER_RESET_START stream_channels
    int          stream_channels;

    int          bandwidth;
    int          mode;
    int          prev_mode;
    int          frame_size;
    int          prev_redundancy;
    int          last_packet_duration;
#ifndef FIXED_POINT
    opus_val16   softclip_mem[2];
#endif

    opus_uint32  rangeFinal;
};


static int parse_size(const unsigned char *data, opus_int32 len, opus_int16 *size)
{
   if (len<1)
   {
      *size = -1;
      return -1;
   } else if (data[0]<252)
   {
      *size = data[0];
      return 1;
   } else if (len<2)
   {
      *size = -1;
      return -1;
   } else {
      *size = 4*data[1] + data[0];
      return 2;
   }
}


static inline int is_mlow_multiframe_packet(const unsigned char toc) {
   return !is_mlow_celt(toc) && ((toc & MLOW_MULTI_TOC_MASK) == MLOW_MULTI_TOC_MASK);
}

/*
Multiframe MLow packet will have the following layout:

  1. MultiFrame TOC Indicator Byte [1 Byte] (above)

    +--0–-+--1--+--2--+--3--+--4--+--5--+--6--+--7--+
    |  1  |  0  |  Fs |  FrameSz  | Rate|  1  |  0  |
    +-----+-----+-----+-----+-----+-----+-----+-----+
    |                  No. of frames                |
    +-----+-----+-----+-----+-----+-----+-----+-----+

  2. No. of Frames is represented using next byte. For now we allow upto
     18 frames max, such that for the case of 10ms frames, we can pack
     upto 180ms of data.

  3. Sequence of size for each frame represented using 1 or 2 bytes
    * check parse_size/encode_size methods for how 1 or 2 bytes are used.
    * Size of Nth frame is encoded only for self delimiting case.

     <Size-1><Size-2>...<Size-N-1>

  4. Actual payloads, original intact 1 frame MLow packets with TOC + data.
    * It is different from Opus because in case of MLow each frame can have
      different TOC value (SID, VAD, FEC flags).

    <TOC-1><Data-1><TOC-2><Data-2>....<TOC-N><Data-N>
*/
static int opus_smpl_packet_parse_multiframe_mlow(
      const unsigned char* data,
      opus_int32 len,
      int self_delimited,
      const unsigned char* frames[MAX_MLOW_FRAMES_PER_PACKET],
      opus_int16 size[MAX_MLOW_FRAMES_PER_PACKET],
      int* payload_offset)
{
  const unsigned char* const data_start = data;
  if (len < 2) {
    return OPUS_INVALID_PACKET;
  }
  smpl_assert(is_mlow_multiframe_packet(data[0]));

  // Parse no. of frames.
  const int num_frames = data[1];
  if (num_frames < 2 || num_frames > MAX_MLOW_FRAMES_PER_PACKET) {
    return OPUS_INVALID_PACKET;
  }

  data += 2; len -= 2;

  // Parse the frame size bytes.
  for (int i = 0; i < num_frames; i++) {
    if ((i == num_frames - 1) && !self_delimited) {
      // Last frame size only encoded if self delimited.
      break;
    }
    int num_bytes = parse_size(data, len, &size[i]);
    if (num_bytes < 0 || size[i] <= 0) {
      return OPUS_INVALID_PACKET; // Malformed size, min 1 Byte TOC
    }
    data += num_bytes;
    len -= num_bytes;
  }

  // Parse the frame data.
  if (payload_offset) {
    *payload_offset = data - data_start;
  }

  for (int i = 0; i < num_frames - 1; i++) {
    if (len <= 0) {
      return OPUS_INVALID_PACKET;
    }
    // Each internal frame TOC should be a single frame TOC only.
    if (is_mlow_multiframe_packet(data[0])) {
      return OPUS_INVALID_PACKET;
    }
    if (frames) {
      frames[i] = data;
    }
    data += size[i];
    len -= size[i];
  }
  if (len <= 0) { // Last frame should have non zero size.
    return OPUS_INVALID_PACKET;
  }
  if (frames) {
   frames[num_frames - 1] = data;
  }
  if (self_delimited) {
    smpl_assert(len == size[num_frames - 1]);
  } else {
    size[num_frames - 1] = len;
  }

  return num_frames;
}

static int opus_smpl_packet_parse_mlow_impl(const unsigned char *data, opus_int32 len,
      int self_delimited, unsigned char *out_toc,
      const unsigned char *frames[MAX_MLOW_FRAMES_PER_PACKET], opus_int16 size[MAX_MLOW_FRAMES_PER_PACKET],
      int *payload_offset, opus_int32 *packet_offset)
{
   if (size==NULL || len<0) {
      return OPUS_BAD_ARG;
   }
   if (len==0) {
      return OPUS_INVALID_PACKET;
   }

   int count = OPUS_INVALID_PACKET;
   const unsigned char *data0 = data;
   int offset = 0;
   if (is_mlow_multiframe_packet(data[0])) { // multiframe MLow
      count = opus_smpl_packet_parse_multiframe_mlow(
            data,
            len,
            self_delimited,
            frames,
            size,
            &offset);
   } else { // single MLow frame
      if (frames) {
         frames[0] = data;
      }
      size[0] = len;
      count = 1;
      offset = 0;
   }

   if (out_toc) {
      *out_toc = *data0;
   }
   if (payload_offset) {
      *payload_offset = offset;
   }
   if (packet_offset) {
      *packet_offset = offset;
   }

   return count;
}

static int opus_smpl_packet_parse_celt_impl(const unsigned char *data, opus_int32 len,
      int self_delimited, unsigned char *out_toc,
      const unsigned char *frames[48], opus_int16 size[48],
      int *payload_offset, opus_int32 *packet_offset)
{
    int i, bytes;
    int count;
    int cbr=0;
    unsigned char ch, toc;
    int framesize;
    opus_int32 last_size;
    opus_int32 pad = 0;
    const unsigned char *data0 = data;
    opus_smpl_TOC_parameters toc_parameters;

    if (size==NULL || len<0)
        return OPUS_BAD_ARG;
    if (len==0)
        return OPUS_INVALID_PACKET;

    framesize = mlow_packet_get_samples_per_frame(data, 48000);
    opus_smpl_decode_TOC(data, &toc_parameters);
    int opus_frames_per_packet = opus_smpl_get_frames_per_packet(data, len);
    if(opus_frames_per_packet <= 0){
        return OPUS_INVALID_PACKET;
    }
    toc = *data++ & 0xFE; // Clear last bit and make it single frame CELT packet
    len--;

    smpl_assert(toc_parameters.using_smpl == 0);  // Ensure CELT frame
    last_size = len;

   if (opus_frames_per_packet == 1) {
      size[0] = len;
      count = 1;
   } else {
      // Never uses two CBR frames or two VBR frames - always specify number of frames, corresponding to case 3 in opus TOC
      if (len<1)
            return OPUS_INVALID_PACKET;
      /* Number of frames encoded in bits 0 to 5 */
      ch = *data++;
      count = ch&0x3F;
      if (count <= 0 || framesize*(opus_int32)count > 5760)
            return OPUS_INVALID_PACKET;
      len--;
      /* Padding flag is bit 6 */
      if (ch&0x40)
      {
            int p;
            do {
               int tmp;
               if (len<=0)
               return OPUS_INVALID_PACKET;
               p = *data++;
               len--;
               tmp = p==255 ? 254: p;
               len -= tmp;
               pad += tmp;
            } while (p==255);
      }
      if (len<0)
            return OPUS_INVALID_PACKET;
      /* VBR flag is bit 7 */
      cbr = !(ch&0x80);
      if (!cbr)
      {
            /* VBR case */
            last_size = len;
            for (i=0;i<count-1;i++)
            {
               bytes = parse_size(data, len, size+i);
               len -= bytes;
               if (size[i]<0 || size[i] > len)
               return OPUS_INVALID_PACKET;
               data += bytes;
               last_size -= bytes+size[i];
            }
            if (last_size<0)
               return OPUS_INVALID_PACKET;
      } else if (!self_delimited)
      {
            /* CBR case */
            last_size = len/count;
            if (last_size*count!=len)
               return OPUS_INVALID_PACKET;
            for (i=0;i<count-1;i++)
               size[i] = (opus_int16)last_size;
      }
   }

   /* Self-delimited framing has an extra size for the last frame. */
   if (self_delimited)
   {
      bytes = parse_size(data, len, size+count-1);
      len -= bytes;
      if (size[count-1]<0 || size[count-1] > len)
         return OPUS_INVALID_PACKET;
      data += bytes;
      /* For CBR packets, apply the size to all the frames. */
      if (cbr)
      {
         if (size[count-1]*count > len)
            return OPUS_INVALID_PACKET;
         for (i=0;i<count-1;i++)
            size[i] = size[count-1];
      } else if (bytes+size[count-1] > last_size)
         return OPUS_INVALID_PACKET;
   } else
   {
      /* Because it's not encoded explicitly, it's possible the size of the
         last packet (or all the packets, for the CBR case) is larger than
         1275. Reject them here.*/
      if (last_size > 1275)
          return OPUS_INVALID_PACKET;
      size[count-1] = (opus_int16)last_size;
   }

   if (payload_offset)
      *payload_offset = (int)(data-data0);

   for (i=0;i<count;i++)
   {
      if (frames)
         frames[i] = data;
      data += size[i];
   }

   if (packet_offset)
      *packet_offset = pad+(opus_int32)(data-data0);

   if (out_toc)
      *out_toc = toc;

   return count;
}

int opus_smpl_packet_parse_impl(const unsigned char *data, opus_int32 len,
      int self_delimited, unsigned char *out_toc,
      const unsigned char *frames[48], opus_int16 size[48],
      int *payload_offset, opus_int32 *packet_offset) {
  if (len < 1) {
    return OPUS_INVALID_PACKET;
  }

  if (!is_mlow_celt(data[0])) { // MLow mode.
    return opus_smpl_packet_parse_mlow_impl(
      data, len, self_delimited, out_toc, frames, size, payload_offset, packet_offset);
  }
  // CELT mode.
  return opus_smpl_packet_parse_celt_impl(
    data, len, self_delimited, out_toc, frames, size, payload_offset, packet_offset);
}

void opus_smpl_decode_TOC(const unsigned char *data, opus_smpl_TOC_parameters *toc_params) {
    //  TOC, at the moment 8 bits - CELT is defined by both SID and VoA being 1
    //
    //  For SMPL
    //           +--1--+--2--+--3--+--4--+--5--+--6--+--7--+--8--+
    //           |  SID/VoA  | Fs  |  FrameSz  | Rate| FEC | Ste |
    //           +-----+-----+-----+-----+-----+-----+-----+-----+
    //
    // SID/VoA - 00: Normal frame(s) sent while there is no voice activity i.e. when DTX is off
    //           01: Normal frame(s) with some voice activity
    //           10: SID frame(s) (no voice activity)
    //           11: CELT used => See further down for bit representation
    // Fs      -  0: 16kHz mode without any bits for 32/48kHz mode
    //            1: 32/48kHz mode. Decoder can decide to get either 32 or 48kHz
    // FrameSz - 00:  10ms SMPL frame
    //           01:  20ms SMPL frame
    //           10:  60ms SMPL frame
    //           11: 120ms SMPL frame
    // Rate       0: Low Rate mode of SMPL
    //            1: High rate mode of SMPL
    // FEC     -  0: Packed does not contain FEC frame
    //            1: If VoA == 1 Packet contains FEC frame (not yet used)
    //               If VoA == 0 Packet does not contain FEC but frame(s) are encoded as if they may contain voiced (hang-over period)
    // Ste     -  0: Frame is mono
    //            1: Frame contains stereo information

    // For CELT (only 10 and 20ms includeing duplicates are used)
    //           +--1--+--2--+--3--+--4--+--5--+--6--+--7--+--8--+
    //           |  1     1  |         Mode          | Ste |  c  |
    //           +-----+-----+-----+-----+-----+-----+-----+-----+
    // Mode:
    //    +-----------------------+-----------+-----------+-------------------+
    //    | Configuration         | Mode      | Bandwidth | Frame Sizes       |
    //    | Number(s)             |           |           |                   |
    //    +-----------------------+-----------+-----------+-------------------+
    //    | 0...3                 | CELT-only | NB        | 2.5, 5, 10, 20 ms |
    //    |                       |           |           |                   |
    //    | 4...7                 | CELT-only | WB        | 2.5, 5, 10, 20 ms |
    //    |                       |           |           |                   |
    //    | 8...11                | CELT-only | SWB       | 2.5, 5, 10, 20 ms |
    //    |                       |           |           |                   |
    //    | 12...15               | CELT-only | FB        | 2.5, 5, 10, 20 ms |
    //    +-----------------------+-----------+-----------+-------------------+
    // c:
    //    0 - 1 frame per packet
    //    1 - an arbitrary number of frames in the packet
    unsigned char toc_byte = data[0];
    toc_params->using_smpl = ((toc_byte >> 6) != 3) ? 1 : 0;
    if (toc_params->using_smpl) {
        smpl_TOC smpl_toc;
        smpl_decode_toc(toc_byte, &smpl_toc);
        toc_params->fs = smpl_toc.fs_Hz;
        toc_params->fs_mode = (smpl_toc.fs_Hz == 16000) ? OPUS_BANDWIDTH_WIDEBAND : OPUS_BANDWIDTH_FULLBAND;
        toc_params->opus_samples_per_frame = (smpl_toc.packet_len_ms * toc_params->fs) / 1000;
        toc_params->stereo = smpl_toc.stereo;
        smpl_assert(!(smpl_toc.SID && smpl_toc.VAD))
    } else {
        int mode = (toc_byte >> 2) & 0xF;
        int fs = mode <= 3 ? 8000 : mode <= 7 ? 16000 : mode <= 11 ? 24000 : 48000;
        int micro_sec = (mode&3) == 0 ? 2500 : (mode&3) == 1 ? 5000 : (mode&3) == 2 ? 10000 : 20000;
        switch (fs) {
            case 8000:
                toc_params->fs_mode = OPUS_BANDWIDTH_NARROWBAND;
                break;
            case 16000:
                toc_params->fs_mode = OPUS_BANDWIDTH_WIDEBAND;
                break;
            case 24000:
                toc_params->fs_mode = OPUS_BANDWIDTH_SUPERWIDEBAND;
                break;
            case 48000:
                toc_params->fs_mode = OPUS_BANDWIDTH_FULLBAND;
                break;
            default:
                smpl_assert(0);
        }
        toc_params->opus_samples_per_frame = (fs * micro_sec) / 1000000;
        toc_params->fs = fs;
        toc_params->stereo = (toc_byte>>1)&1;
    }
}

int opus_smpl_get_frames_per_packet(const unsigned char* data, opus_int32 len)
{
    if (len < 1) {
        return OPUS_INVALID_PACKET;
    }
    const unsigned char toc_byte = data[0];
    if (!is_mlow_celt(toc_byte)) {
      if (is_mlow_multiframe_packet(toc_byte)) {
         if (len < 2 || data[1] < 2 || data[1] > MAX_MLOW_FRAMES_PER_PACKET) {
            return OPUS_INVALID_PACKET;
         }
         return data[1];
      } else {
         return 1;
      }
    }
    else {
        if ((toc_byte&1) == 0) {
            return 1;
        }
        else {
            if (len < 2) {
                return OPUS_INVALID_PACKET;
            }
            return data[1] & 0x3F;
        }
    }
    return OPUS_INVALID_PACKET;
}

static void smooth_fade(const opus_val16 *in1, const opus_val16 *in2,
      opus_val16 *out, int overlap, int channels,
      const opus_val16 *window, opus_int32 Fs)
{
   int i, c;
   int inc = 48000/Fs;
   for (c=0;c<channels;c++)
   {
      for (i=0;i<overlap;i++)
      {
         opus_val16 w = MULT16_16_Q15(window[i*inc], window[i*inc]);
         out[i*channels+c] = SHR32(MAC16_16(MULT16_16(w,in2[i*channels+c]),
                                   Q15ONE-w, in1[i*channels+c]), 15);
      }
   }
}



int opus_smpl_decode_frame(OpusDecoder* st, const unsigned char* data,
    opus_int32 len, const unsigned char toc, opus_val16* pcm, int frame_size, int decode_fec)
{
   TIC(smpl_decode_frame)
   void *smpl_dec;
   CELTDecoder *celt_dec;
   int i, smpl_ret=0, celt_ret=0;
   ec_dec dec;
   int pcm_smpl_size;
   VARDECL(opus_int16, pcm_smpl);
   int pcm_transition_smpl_size;
   VARDECL(opus_val16, pcm_transition_smpl);
   int pcm_transition_celt_size;
   VARDECL(opus_val16, pcm_transition_celt);
   opus_val16 *pcm_transition=NULL;
   int redundant_audio_size;
   VARDECL(opus_val16, redundant_audio);

   int audiosize;
   int mode;
   int bandwidth;
   int transition=0;
   int start_band;
   int redundancy=0;
   int redundancy_bytes = 0;
   int celt_to_smpl=0;
   int c;
   int F2_5, F5, F10, F20;
   const opus_val16 *window;
   opus_uint32 redundant_rng = 0;
   int celt_accum;
   ALLOC_STACK;

   // Make sure that we don't have a multiframe MLow TOC here.
   if(is_mlow_multiframe_packet(toc)) {
      return OPUS_INVALID_PACKET;
   }

   smpl_dec = (char*)st+st->smpl_dec_offset;
   celt_dec = (CELTDecoder*)((char*)st+st->celt_dec_offset);
   F20 = st->Fs/50;
   F10 = F20>>1;
   F5 = F10>>1;
   F2_5 = F5>>1;
   if (frame_size < F2_5)
   {
      RESTORE_STACK;
      return OPUS_BUFFER_TOO_SMALL;
   }
   /* Limit frame_size to avoid excessive stack allocations. */
   frame_size = IMIN(frame_size, st->Fs/25*3);
   /* Payloads of 1 (2 including ToC) or 0 trigger the PLC/DTX */
   if (len<=1)
   {
      data = NULL;
      /* In that case, don't conceal more than what the ToC says */
      frame_size = IMIN(frame_size, st->frame_size);
   }
   if (data != NULL)
   {
      audiosize = st->frame_size;
      mode = st->mode;
      bandwidth = st->bandwidth;
      ec_dec_init(&dec,(unsigned char*)data,len);
   } else {
      audiosize = frame_size;
      /* Run PLC using last used mode (CELT if we ended with CELT redundancy) */
      mode = st->prev_redundancy ? MODE_CELT_ONLY : st->prev_mode;
      bandwidth = 0;

      if (mode == 0)
      {
         /* If we haven't got any packet yet, all we can do is return zeros */
         for (i=0;i<audiosize*st->channels;i++)
            pcm[i] = 0;
         RESTORE_STACK;
         return audiosize;
      }

      /* Avoids trying to run the PLC on sizes other than 2.5 (CELT), 5 (CELT),
         10, or 20 (e.g. 12.5 or 30 ms). */
      if (audiosize > F20)
      {
         do {
            int ret = opus_smpl_decode_frame(st, NULL, 0, toc, pcm, IMIN(audiosize, F20), 0);
            if (ret<0)
            {
               RESTORE_STACK;
               return ret;
            }
            pcm += ret*st->channels;
            audiosize -= ret;
         } while (audiosize > 0);
         RESTORE_STACK;
         return frame_size;
      } else if (audiosize < F20)
      {
         if (audiosize > F10)
            audiosize = F10;
         else if (mode != MODE_SMPL_ONLY && audiosize > F5 && audiosize < F10)
            audiosize = F5;
      }
   }

   /* In fixed-point, we can tell CELT to do the accumulation on top of the
      SMPL PCM buffer. This saves some stack space. */
#ifdef FIXED_POINT
   celt_accum = (mode != MODE_CELT_ONLY) && (frame_size >= F10);
#else
   celt_accum = 0;
#endif

   pcm_transition_smpl_size = ALLOC_NONE;
   pcm_transition_celt_size = ALLOC_NONE;
   if (data!=NULL && st->prev_mode > 0 && (
       (mode == MODE_CELT_ONLY && st->prev_mode != MODE_CELT_ONLY && !st->prev_redundancy)
    || (mode != MODE_CELT_ONLY && st->prev_mode == MODE_CELT_ONLY) )
      )
   {
      transition = 1;
      /* Decide where to allocate the stack memory for pcm_transition */
      if (mode == MODE_CELT_ONLY)
         pcm_transition_celt_size = F5*st->channels;
      else
         pcm_transition_smpl_size = F5*st->channels;
   }
   ALLOC(pcm_transition_celt, pcm_transition_celt_size, opus_val16);
   if (transition && mode == MODE_CELT_ONLY)
   {
      pcm_transition = pcm_transition_celt;
      opus_smpl_decode_frame(st, NULL, 0, toc, pcm_transition, IMIN(F5, audiosize), 0);
   }
   if (audiosize > frame_size)
   {
      /*fprintf(stderr, "PCM buffer too small: %d vs %d (mode = %d)\n", audiosize, frame_size, mode);*/
      RESTORE_STACK;
      return OPUS_BAD_ARG;
   } else {
      frame_size = audiosize;
   }

   /* Don't allocate any memory when in CELT-only mode */
   pcm_smpl_size = (mode != MODE_CELT_ONLY && !celt_accum) ? IMAX(F20, frame_size)*st->channels : ALLOC_NONE;
   ALLOC(pcm_smpl, pcm_smpl_size, opus_int16);

   /* SMPL processing */
   if (mode != MODE_CELT_ONLY)
   {
      int lost_flag, decoded_samples;
      opus_int16 *pcm_ptr;
#ifdef FIXED_POINT
      if (celt_accum)
         pcm_ptr = pcm;
      else
#endif
         pcm_ptr = pcm_smpl;

      if (st->prev_mode==MODE_CELT_ONLY)
         smpl_InitDecoder( smpl_dec );

      /* The SMPL PLC cannot produce frames of less than 10 ms */
      st->smpl_DecControl.payloadSize_ms = IMAX(10, 1000 * audiosize / st->Fs);

      if (data != NULL)
      {
        st->smpl_DecControl.nChannelsInternal = st->stream_channels;
        if( mode == MODE_SMPL_ONLY ) {
           if( bandwidth == OPUS_BANDWIDTH_NARROWBAND ) {
              st->smpl_DecControl.internalSampleRate = 8000;
           } else if( bandwidth == OPUS_BANDWIDTH_MEDIUMBAND ) {
              st->smpl_DecControl.internalSampleRate = 12000;
           } else if( bandwidth == OPUS_BANDWIDTH_WIDEBAND ) {
              st->smpl_DecControl.internalSampleRate = 16000;
           } else if (bandwidth == OPUS_BANDWIDTH_SUPERWIDEBAND) {
               st->smpl_DecControl.internalSampleRate = 32000;
           } else if (bandwidth == OPUS_BANDWIDTH_FULLBAND) {
               st->smpl_DecControl.internalSampleRate = 32000;
           } else {
              st->smpl_DecControl.internalSampleRate = 16000;
              celt_assert( 0 );
           }
        } else {
           /* Hybrid mode */
            // SMPL, we do not use Hybrid mode
            celt_assert(0);
            st->smpl_DecControl.internalSampleRate = 16000;
        }
     }

     lost_flag = data == NULL ? 1 : 2 * decode_fec;
     decoded_samples = 0;
     do {
        /* Call SMPL decoder */
        int first_frame = decoded_samples == 0;
        if ((lost_flag == 1) && (frame_size == F5 * st->channels)) {
            // 5ms PLC for interpolating with CELT. SMPL decoder produces minimum 10 though
            frame_size *=2;
            smpl_ret = smpl_Decode(smpl_dec, &st->smpl_DecControl,
                lost_flag, first_frame, &dec, toc,
                pcm_ptr, &frame_size);
            frame_size /= 2;
        } else {
            // Normal use case
            smpl_ret = smpl_Decode(smpl_dec, &st->smpl_DecControl,
                lost_flag, first_frame, &dec, toc,
                pcm_ptr, &frame_size);
        }
        // Convert to Opus frame size which is per channel
        if( smpl_ret ) {
           if (lost_flag) {
              /* PLC failure should not be fatal */
              for (i=0;i<frame_size*st->channels;i++)
                 pcm_ptr[i] = 0;
           } else {
             RESTORE_STACK;
             return OPUS_INTERNAL_ERROR;
           }
        }
        pcm_ptr += frame_size * st->channels;
        decoded_samples += frame_size;
      } while( decoded_samples < frame_size );
   }

   start_band = 0;
   if (!decode_fec && mode != MODE_CELT_ONLY && data != NULL && ec_tell(&dec)+17 <= 8*len)
   {
      /* Check if we have a redundant 0-8 kHz band */
      redundancy = 1;
      if (redundancy)
      {
         celt_to_smpl = ec_dec_bit_logp(&dec, 1);
         /* redundancy_bytes will be at least two, in the non-hybrid
            case due to the ec_tell() check above */
         redundancy_bytes = len-((ec_tell(&dec)+7)>>3);
         len -= redundancy_bytes;
         /* This is a sanity check. It should never happen for a valid
            packet, so the exact behaviour is not normative. */
         if (len*8 < ec_tell(&dec))
         {
            len = 0;
            redundancy_bytes = 0;
            redundancy = 0;
         }
         /* Shrink decoder because of raw bits */
         dec.storage -= redundancy_bytes;
      }
   }
   if (mode != MODE_CELT_ONLY)
      start_band = 17;

   if (redundancy)
   {
      transition = 0;
      pcm_transition_smpl_size=ALLOC_NONE;
   }

   ALLOC(pcm_transition_smpl, pcm_transition_smpl_size, opus_val16);

   if (transition && mode != MODE_CELT_ONLY)
   {
      pcm_transition = pcm_transition_smpl;
      opus_smpl_decode_frame(st, NULL, 0, toc, pcm_transition, IMIN(F5, audiosize), 0);
   }


   if (bandwidth)
   {
      int endband=21;

      switch(bandwidth)
      {
      case OPUS_BANDWIDTH_NARROWBAND:
         endband = 13;
         break;
      case OPUS_BANDWIDTH_MEDIUMBAND:
      case OPUS_BANDWIDTH_WIDEBAND:
         endband = 17;
         break;
      case OPUS_BANDWIDTH_SUPERWIDEBAND:
         endband = 19;
         break;
      case OPUS_BANDWIDTH_FULLBAND:
         endband = 21;
         break;
      default:
         celt_assert(0);
         break;
      }
      MUST_SUCCEED(celt_decoder_ctl(celt_dec, CELT_SET_END_BAND(endband)));
   }
   MUST_SUCCEED(celt_decoder_ctl(celt_dec, CELT_SET_CHANNELS(st->stream_channels)));

   /* Only allocation memory for redundancy if/when needed */
   redundant_audio_size = redundancy ? F5*st->channels : ALLOC_NONE;
   ALLOC(redundant_audio, redundant_audio_size, opus_val16);

   /* 5 ms redundant frame for CELT->SMPL*/
   if (redundancy && celt_to_smpl)
   {
      /* If the previous frame did not use CELT (the first redundancy frame in
         a transition from SMPL may have been lost) then the CELT decoder is
         stale at this point and the redundancy audio is not useful, however
         the final range is still needed (for testing), so the redundancy is
         always decoded but the decoded audio may not be used */
      MUST_SUCCEED(celt_decoder_ctl(celt_dec, CELT_SET_START_BAND(0)));
      celt_decode_with_ec(celt_dec, data+len, redundancy_bytes,
                          redundant_audio, F5, NULL, 0);
      MUST_SUCCEED(celt_decoder_ctl(celt_dec, OPUS_GET_FINAL_RANGE(&redundant_rng)));
   }

   /* MUST be after PLC */
   MUST_SUCCEED(celt_decoder_ctl(celt_dec, CELT_SET_START_BAND(start_band)));

   if (mode != MODE_SMPL_ONLY)
   {
      int celt_frame_size = IMIN(F20, frame_size);
      /* Make sure to discard any previous CELT state */
      if (mode != st->prev_mode && st->prev_mode > 0 && !st->prev_redundancy)
         MUST_SUCCEED(celt_decoder_ctl(celt_dec, OPUS_RESET_STATE));
      /* Decode CELT */
      celt_ret = celt_decode_with_ec(celt_dec, decode_fec ? NULL : data,
                                     len, pcm, celt_frame_size, &dec, celt_accum);
   } else {
      if (!celt_accum)
      {
         for (i=0;i<frame_size*st->channels;i++)
            pcm[i] = 0;
      }
   }

   if (mode != MODE_CELT_ONLY && !celt_accum)
   {
#ifdef FIXED_POINT
      for (i=0;i<frame_size*st->channels;i++)
         pcm[i] = SAT16(ADD32(pcm[i], pcm_smpl[i]));
#else
      for (i=0;i<frame_size*st->channels;i++)
         pcm[i] = pcm[i] + (opus_val16)((1.f/32768.f)*pcm_smpl[i]);
#endif
   }

   {
      const CELTMode *celt_mode;
      MUST_SUCCEED(celt_decoder_ctl(celt_dec, CELT_GET_MODE(&celt_mode)));
      window = celt_mode->window;
   }

   /* 5 ms redundant frame for SMPL->CELT */
   if (redundancy && !celt_to_smpl)
   {
      MUST_SUCCEED(celt_decoder_ctl(celt_dec, OPUS_RESET_STATE));
      MUST_SUCCEED(celt_decoder_ctl(celt_dec, CELT_SET_START_BAND(0)));

      celt_decode_with_ec(celt_dec, data+len, redundancy_bytes, redundant_audio, F5, NULL, 0);
      MUST_SUCCEED(celt_decoder_ctl(celt_dec, OPUS_GET_FINAL_RANGE(&redundant_rng)));
      smooth_fade(pcm+st->channels*(frame_size-F2_5), redundant_audio+st->channels*F2_5,
                  pcm+st->channels*(frame_size-F2_5), F2_5, st->channels, window, st->Fs);
   }
   /* 5ms redundant frame for CELT->SMPL; ignore if the previous frame did not
      use CELT (the first redundancy frame in a transition from SMPL may have
      been lost) */
   if (redundancy && celt_to_smpl && (st->prev_mode != MODE_SMPL_ONLY || st->prev_redundancy))
   {
      for (c=0;c<st->channels;c++)
      {
         for (i=0;i<F2_5;i++)
            pcm[st->channels*i+c] = redundant_audio[st->channels*i+c];
      }
      smooth_fade(redundant_audio+st->channels*F2_5, pcm+st->channels*F2_5,
                  pcm+st->channels*F2_5, F2_5, st->channels, window, st->Fs);
   }
   if (transition)
   {
      if (audiosize >= F5)
      {
         for (i=0;i<st->channels*F2_5;i++)
            pcm[i] = pcm_transition[i];
         smooth_fade(pcm_transition+st->channels*F2_5, pcm+st->channels*F2_5,
                     pcm+st->channels*F2_5, F2_5,
                     st->channels, window, st->Fs);
      } else {
         /* Not enough time to do a clean transition, but we do it anyway
            This will not preserve amplitude perfectly and may introduce
            a bit of temporal aliasing, but it shouldn't be too bad and
            that's pretty much the best we can do. In any case, generating this
            transition it pretty silly in the first place */
         smooth_fade(pcm_transition, pcm,
                     pcm, F2_5,
                     st->channels, window, st->Fs);
      }
   }

   if(st->decode_gain)
   {
      opus_val32 gain;
      gain = celt_exp2(MULT16_16_P15(QCONST16(6.48814081e-4f, 25), st->decode_gain));
      for (i=0;i<frame_size*st->channels;i++)
      {
         opus_val32 x;
         x = MULT16_32_P16(pcm[i],gain);
         pcm[i] = SATURATE(x, 32767);
      }
   }

   if (len <= 1)
      st->rangeFinal = 0;
   else
      st->rangeFinal = dec.rng ^ redundant_rng;

   st->prev_mode = mode;
   st->prev_redundancy = redundancy && !celt_to_smpl;

   if (celt_ret>=0)
   {
      if (OPUS_CHECK_ARRAY(pcm, audiosize*st->channels))
         OPUS_PRINT_INT(audiosize);
   }

   RESTORE_STACK;
   TOC(smpl_decode_frame)
   return celt_ret < 0 ? celt_ret : audiosize;
}

# endif // #if defined(ENABLE_SMPL)
