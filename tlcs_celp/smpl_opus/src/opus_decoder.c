/* Copyright (c) 2010 Xiph.Org Foundation, Skype Limited
   Written by Jean-Marc Valin and Koen Vos */
/*
   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.

   - Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER
   OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#ifndef OPUS_BUILD
# error "OPUS_BUILD _MUST_ be defined to build Opus. This probably means you need other defines as well, as in a config.h. See the included build files for details."
#endif

#if defined(__GNUC__) && (__GNUC__ >= 2) && !defined(__OPTIMIZE__) && !defined(OPUS_WILL_BE_SLOW)
# pragma message "You appear to be compiling without optimization, if so opus will be very slow."
#endif

#include <stdarg.h>
#include "celt.h"
#include "opus.h"
#include "entdec.h"
#include "modes.h"
#include "API.h"
#include "stack_alloc.h"
#include "float_cast.h"
#include "opus_private.h"
#include "os_support.h"
#include "structs.h"
#include "define.h"
#include "mathops.h"
#include "cpu_support.h"
#include "smpl/smpl_api.h"
#include "smpl/opus_smpl_decode.h"
#include "smpl/opus_smpl_repacketizer.h"

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

#if defined(ENABLE_HARDENING) || defined(ENABLE_ASSERTIONS)
static void validate_opus_decoder(OpusDecoder *st)
{
   celt_assert(st->channels == 1 || st->channels == 2);
   celt_assert(st->Fs == 48000 || st->Fs == 24000 || st->Fs == 16000 || st->Fs == 12000 || st->Fs == 8000);
   celt_assert(st->DecControl.API_sampleRate == st->Fs);
   celt_assert(st->DecControl.internalSampleRate == 0 || st->DecControl.internalSampleRate == 16000 || st->DecControl.internalSampleRate == 12000 || st->DecControl.internalSampleRate == 8000);
   celt_assert(st->DecControl.nChannelsAPI == st->channels);
   celt_assert(st->DecControl.nChannelsInternal == 0 || st->DecControl.nChannelsInternal == 1 || st->DecControl.nChannelsInternal == 2);
   celt_assert(st->DecControl.payloadSize_ms == 0 || st->DecControl.payloadSize_ms == 10 || st->DecControl.payloadSize_ms == 20 || st->DecControl.payloadSize_ms == 40 || st->DecControl.payloadSize_ms == 60);
#if defined(ENABLE_SMPL)
   celt_assert(st->smpl_DecControl.API_sampleRate == st->Fs);
   celt_assert(st->smpl_DecControl.internalSampleRate == 0 || st->smpl_DecControl.internalSampleRate == 8000 || st->smpl_DecControl.internalSampleRate == 16000 ||
                st->smpl_DecControl.internalSampleRate == 32000 || st->smpl_DecControl.internalSampleRate == 48000);
   celt_assert(st->smpl_DecControl.nChannelsAPI == st->channels);
   celt_assert(st->smpl_DecControl.nChannelsInternal == 0 || st->smpl_DecControl.nChannelsInternal == 1 || st->smpl_DecControl.nChannelsInternal == 2);
   celt_assert(st->smpl_DecControl.payloadSize_ms == 0 || st->smpl_DecControl.payloadSize_ms == 10 || st->smpl_DecControl.payloadSize_ms == 20 ||
       st->smpl_DecControl.payloadSize_ms == 60 || st->smpl_DecControl.payloadSize_ms == 120);
#endif
#ifdef OPUS_ARCHMASK
   celt_assert(st->arch >= 0);
   celt_assert(st->arch <= OPUS_ARCHMASK);
#endif
   celt_assert(st->stream_channels == 1 || st->stream_channels == 2);
}
#define VALIDATE_OPUS_DECODER(st) validate_opus_decoder(st)
#else
#define VALIDATE_OPUS_DECODER(st)
#endif

int opus_decoder_get_size(int channels)
{
   int silkDecSizeBytes=0, smplDecSizeBytes = 0, celtDecSizeBytes;
   int ret;
   if (channels<1 || channels > 2)
      return 0;
   ret = silk_Get_Decoder_Size( &silkDecSizeBytes );
   if(ret)
      return 0;
#if defined(ENABLE_SMPL)
   ret = smpl_Get_Decoder_Size(&smplDecSizeBytes);
   if (ret)
       return 0;
#endif
   silkDecSizeBytes = align(silkDecSizeBytes);
   smplDecSizeBytes = align(smplDecSizeBytes);
   celtDecSizeBytes = celt_decoder_get_size(channels);
   return align(sizeof(OpusDecoder))+silkDecSizeBytes+smplDecSizeBytes+celtDecSizeBytes;
}

int opus_decoder_init(OpusDecoder *st, opus_int32 Fs, int channels)
{
    void *silk_dec;
#if defined(ENABLE_SMPL)
   void* smpl_dec;
#endif
   CELTDecoder *celt_dec;
   int ret, silkDecSizeBytes=0, smplDecSizeBytes=0;

   if ((Fs!=48000&&Fs!=24000&&Fs!=16000&&Fs!=12000&&Fs!=8000)
    || (channels!=1&&channels!=2))
      return OPUS_BAD_ARG;

   OPUS_CLEAR((char*)st, opus_decoder_get_size(channels));
   /* Initialize SILK decoder */
   ret = silk_Get_Decoder_Size(&silkDecSizeBytes);
   if (ret)
      return OPUS_INTERNAL_ERROR;
#if defined(ENABLE_SMPL)
   ret = smpl_Get_Decoder_Size(&smplDecSizeBytes);
   if (ret)
       return OPUS_INTERNAL_ERROR;
#endif

   silkDecSizeBytes = align(silkDecSizeBytes);
   st->silk_dec_offset = align(sizeof(OpusDecoder));
   silk_dec = (char*)st + st->silk_dec_offset;
#if defined(ENABLE_SMPL)
   st->smpl_dec_offset = align(sizeof(OpusDecoder))+silkDecSizeBytes;
   smpl_dec = (char*)st + st->smpl_dec_offset;
#endif
   st->celt_dec_offset = align(sizeof(OpusDecoder))+silkDecSizeBytes+smplDecSizeBytes;
   celt_dec = (CELTDecoder*)((char*)st+st->celt_dec_offset);
   st->stream_channels = st->channels = channels;

   st->Fs = Fs;
   st->DecControl.API_sampleRate = st->Fs;
   st->DecControl.nChannelsAPI      = st->channels;
#if defined(ENABLE_SMPL)
   st->smpl_DecControl.API_sampleRate = st->Fs;
   st->smpl_DecControl.nChannelsAPI = st->channels;
   st->smpl_DecControl.LPC_postfilter_mode = SMPL_LPC_PSTF_MODE_WB_OFF_SWB_ON;
#endif

   /* Reset decoder */
   ret = silk_InitDecoder( silk_dec );
   if(ret)return OPUS_INTERNAL_ERROR;
#if defined(ENABLE_SMPL)
   ret = smpl_InitDecoder(smpl_dec);
   if (ret)return OPUS_INTERNAL_ERROR;
   smpl_open_dec_files(smpl_dec);
#endif

   /* Initialize CELT decoder */
   ret = celt_decoder_init(celt_dec, Fs, channels);
   if(ret!=OPUS_OK)return OPUS_INTERNAL_ERROR;

   celt_decoder_ctl(celt_dec, CELT_SET_SIGNALLING(0));

   st->prev_mode = 0;
   st->frame_size = Fs/400;
   st->arch = opus_select_arch();
   st->use_smpl = 0;
   return OPUS_OK;
}

OpusDecoder *opus_decoder_create(opus_int32 Fs, int channels, int *error)
{
   int ret;
   OpusDecoder *st;
   if ((Fs!=48000&&Fs!=24000&&Fs!=16000&&Fs!=12000&&Fs!=8000)
    || (channels!=1&&channels!=2))
   {
      if (error)
         *error = OPUS_BAD_ARG;
      return NULL;
   }
   st = (OpusDecoder *)opus_alloc(opus_decoder_get_size(channels));
   if (st == NULL)
   {
      if (error)
         *error = OPUS_ALLOC_FAIL;
      return NULL;
   }
   ret = opus_decoder_init(st, Fs, channels);
   if (error)
      *error = ret;
   if (ret != OPUS_OK)
   {
      opus_free(st);
      st = NULL;
   }
   return st;
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

static int opus_smpl_packet_get_mode(int using_smpl, const unsigned char *data)
{
   int mode;
   if (using_smpl) {
#if defined(ENABLE_SMPL)
       opus_smpl_TOC_parameters toc_params;
       opus_smpl_decode_TOC(data, &toc_params);
       return (!toc_params.using_smpl) ? MODE_CELT_ONLY : MODE_SMPL_ONLY;
#else
       return MODE_CELT_ONLY;
#endif
   }
   if (data[0]&0x80)
   {
      mode = MODE_CELT_ONLY;
   } else if ((data[0]&0x60) == 0x60)
   {
      mode = MODE_HYBRID;
   } else {
      mode = MODE_SILK_ONLY;
   }
   return mode;
}

static int opus_decode_frame(OpusDecoder *st, const unsigned char *data,
      opus_int32 len, opus_val16 *pcm, int frame_size, int decode_fec)
{
   void *silk_dec;
   CELTDecoder *celt_dec;
   int i, silk_ret=0, celt_ret=0;
   ec_dec dec;
   opus_int32 silk_frame_size;
   int pcm_silk_size;
   VARDECL(opus_int16, pcm_silk);
   int pcm_transition_silk_size;
   VARDECL(opus_val16, pcm_transition_silk);
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
   int celt_to_silk=0;
   int c;
   int F2_5, F5, F10, F20;
   const opus_val16 *window;
   opus_uint32 redundant_rng = 0;
   int celt_accum;
   ALLOC_STACK;

   silk_dec = (char*)st+st->silk_dec_offset;
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
            int ret = opus_decode_frame(st, NULL, 0, pcm, IMIN(audiosize, F20), 0);
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
         else if (mode != MODE_SILK_ONLY && mode != MODE_SMPL_ONLY &&audiosize > F5 && audiosize < F10)
            audiosize = F5;
      }
   }

   /* In fixed-point, we can tell CELT to do the accumulation on top of the
      SILK PCM buffer. This saves some stack space. */
#ifdef FIXED_POINT
   celt_accum = (mode != MODE_CELT_ONLY) && (frame_size >= F10);
#else
   celt_accum = 0;
#endif

   pcm_transition_silk_size = ALLOC_NONE;
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
         pcm_transition_silk_size = F5*st->channels;
   }
   ALLOC(pcm_transition_celt, pcm_transition_celt_size, opus_val16);
   if (transition && mode == MODE_CELT_ONLY)
   {
      pcm_transition = pcm_transition_celt;
      opus_decode_frame(st, NULL, 0, pcm_transition, IMIN(F5, audiosize), 0);
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
   pcm_silk_size = (mode != MODE_CELT_ONLY && !celt_accum) ? IMAX(F10, frame_size)*st->channels : ALLOC_NONE;
   ALLOC(pcm_silk, pcm_silk_size, opus_int16);

   /* SILK processing */
   if (mode != MODE_CELT_ONLY)
   {
      int lost_flag, decoded_samples;
      opus_int16 *pcm_ptr;
#ifdef FIXED_POINT
      if (celt_accum)
         pcm_ptr = pcm;
      else
#endif
         pcm_ptr = pcm_silk;

      if (st->prev_mode==MODE_CELT_ONLY)
         silk_InitDecoder( silk_dec );

      /* The SILK PLC cannot produce frames of less than 10 ms */
      st->DecControl.payloadSize_ms = IMAX(10, 1000 * audiosize / st->Fs);

      if (data != NULL)
      {
        st->DecControl.nChannelsInternal = st->stream_channels;
        if( mode == MODE_SILK_ONLY ) {
           if( bandwidth == OPUS_BANDWIDTH_NARROWBAND ) {
              st->DecControl.internalSampleRate = 8000;
           } else if( bandwidth == OPUS_BANDWIDTH_MEDIUMBAND ) {
              st->DecControl.internalSampleRate = 12000;
           } else if( bandwidth == OPUS_BANDWIDTH_WIDEBAND ) {
              st->DecControl.internalSampleRate = 16000;
           } else {
              st->DecControl.internalSampleRate = 16000;
              celt_assert( 0 );
           }
        } else {
           /* Hybrid mode */
           st->DecControl.internalSampleRate = 16000;
        }
     }

     lost_flag = data == NULL ? 1 : 2 * decode_fec;
     decoded_samples = 0;
     do {
        /* Call SILK decoder */
        int first_frame = decoded_samples == 0;
        silk_ret = silk_Decode( silk_dec, &st->DecControl,
                                lost_flag, first_frame, &dec, pcm_ptr, &silk_frame_size, st->arch );
        if( silk_ret ) {
           if (lost_flag) {
              /* PLC failure should not be fatal */
              silk_frame_size = frame_size;
              for (i=0;i<frame_size*st->channels;i++)
                 pcm_ptr[i] = 0;
           } else {
             RESTORE_STACK;
             return OPUS_INTERNAL_ERROR;
           }
        }
        pcm_ptr += silk_frame_size * st->channels;
        decoded_samples += silk_frame_size;
      } while( decoded_samples < frame_size );
   }

   start_band = 0;
   if (!decode_fec && mode != MODE_CELT_ONLY && data != NULL
    && ec_tell(&dec)+17+20*(mode == MODE_HYBRID) <= 8*len)
   {
      /* Check if we have a redundant 0-8 kHz band */
      if (mode == MODE_HYBRID)
         redundancy = ec_dec_bit_logp(&dec, 12);
      else
         redundancy = 1;
      if (redundancy)
      {
         celt_to_silk = ec_dec_bit_logp(&dec, 1);
         /* redundancy_bytes will be at least two, in the non-hybrid
            case due to the ec_tell() check above */
         redundancy_bytes = mode==MODE_HYBRID ?
               (opus_int32)ec_dec_uint(&dec, 256)+2 :
               len-((ec_tell(&dec)+7)>>3);
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
      pcm_transition_silk_size=ALLOC_NONE;
   }

   ALLOC(pcm_transition_silk, pcm_transition_silk_size, opus_val16);

   if (transition && mode != MODE_CELT_ONLY)
   {
      pcm_transition = pcm_transition_silk;
      opus_decode_frame(st, NULL, 0, pcm_transition, IMIN(F5, audiosize), 0);
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

   /* 5 ms redundant frame for CELT->SILK*/
   if (redundancy && celt_to_silk)
   {
      /* If the previous frame did not use CELT (the first redundancy frame in
         a transition from SILK may have been lost) then the CELT decoder is
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

   if (mode != MODE_SILK_ONLY && mode != MODE_SMPL_ONLY)
   {
      int celt_frame_size = IMIN(F20, frame_size);
      /* Make sure to discard any previous CELT state */
      if (mode != st->prev_mode && st->prev_mode > 0 && !st->prev_redundancy)
         MUST_SUCCEED(celt_decoder_ctl(celt_dec, OPUS_RESET_STATE));
      /* Decode CELT */
      celt_ret = celt_decode_with_ec(celt_dec, decode_fec ? NULL : data,
                                     len, pcm, celt_frame_size, &dec, celt_accum);
   } else {
      unsigned char silence[2] = {0xFF, 0xFF};
      if (!celt_accum)
      {
         for (i=0;i<frame_size*st->channels;i++)
            pcm[i] = 0;
      }
      /* For hybrid -> SILK transitions, we let the CELT MDCT
         do a fade-out by decoding a silence frame */
      if (st->prev_mode == MODE_HYBRID && !(redundancy && celt_to_silk && st->prev_redundancy) )
      {
         MUST_SUCCEED(celt_decoder_ctl(celt_dec, CELT_SET_START_BAND(0)));
         celt_decode_with_ec(celt_dec, silence, 2, pcm, F2_5, NULL, celt_accum);
      }
   }

   if (mode != MODE_CELT_ONLY && !celt_accum)
   {
#ifdef FIXED_POINT
      for (i=0;i<frame_size*st->channels;i++)
         pcm[i] = SAT16(ADD32(pcm[i], pcm_silk[i]));
#else
      for (i=0;i<frame_size*st->channels;i++)
         pcm[i] = pcm[i] + (opus_val16)((1.f/32768.f)*pcm_silk[i]);
#endif
   }

   {
      const CELTMode *celt_mode;
      MUST_SUCCEED(celt_decoder_ctl(celt_dec, CELT_GET_MODE(&celt_mode)));
      window = celt_mode->window;
   }

   /* 5 ms redundant frame for SILK->CELT */
   if (redundancy && !celt_to_silk)
   {
      MUST_SUCCEED(celt_decoder_ctl(celt_dec, OPUS_RESET_STATE));
      MUST_SUCCEED(celt_decoder_ctl(celt_dec, CELT_SET_START_BAND(0)));

      celt_decode_with_ec(celt_dec, data+len, redundancy_bytes, redundant_audio, F5, NULL, 0);
      MUST_SUCCEED(celt_decoder_ctl(celt_dec, OPUS_GET_FINAL_RANGE(&redundant_rng)));
      smooth_fade(pcm+st->channels*(frame_size-F2_5), redundant_audio+st->channels*F2_5,
                  pcm+st->channels*(frame_size-F2_5), F2_5, st->channels, window, st->Fs);
   }
   /* 5ms redundant frame for CELT->SILK; ignore if the previous frame did not
      use CELT (the first redundancy frame in a transition from SILK may have
      been lost) */
   if (redundancy && celt_to_silk && (st->prev_mode != MODE_SILK_ONLY || st->prev_redundancy))
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
   st->prev_redundancy = redundancy && !celt_to_silk;

   if (celt_ret>=0)
   {
      if (OPUS_CHECK_ARRAY(pcm, audiosize*st->channels))
         OPUS_PRINT_INT(audiosize);
   }

   RESTORE_STACK;
   return celt_ret < 0 ? celt_ret : audiosize;

}

int opus_decode_native(OpusDecoder *st, const unsigned char *data,
      opus_int32 len, opus_val16 *pcm, int frame_size, int decode_fec,
      int self_delimited, opus_int32 *packet_offset, int soft_clip)
{
   int i, nb_samples;
   int count=1, offset;
   unsigned char toc = ((data == NULL) || (len==0)) ? 0 : data[0];
   int packet_frame_size, packet_bandwidth, packet_mode, packet_stream_channels;
   /* 48 x 2.5 ms = 120 ms */
   opus_int16 size[48];
   VALIDATE_OPUS_DECODER(st);
   if (decode_fec<0 || decode_fec>1)
      return OPUS_BAD_ARG;
   /* For FEC/PLC, frame_size has to be to have a multiple of 2.5 ms */
   if ((decode_fec || len==0 || data==NULL) && frame_size%(st->Fs/400)!=0)
      return OPUS_BAD_ARG;
   if (len==0 || data==NULL)
   {
      int pcm_count=0;
      do {
         int ret = -1;
         if (!st->use_smpl) {
            ret = opus_decode_frame(st, NULL, 0, pcm + pcm_count * st->channels, frame_size - pcm_count, 0);
         } else {
#if defined(ENABLE_SMPL)
             ret = opus_smpl_decode_frame(st, NULL, 0, toc, pcm + pcm_count * st->channels, frame_size - pcm_count, 0);
#endif
         }
         if (ret<0)
            return ret;
         pcm_count += ret;
      } while (pcm_count < frame_size);
      celt_assert(pcm_count == frame_size);
      if (OPUS_CHECK_ARRAY(pcm, pcm_count*st->channels))
         OPUS_PRINT_INT(pcm_count);
      st->last_packet_duration = pcm_count;
      return pcm_count;
   } else if (len<0)
      return OPUS_BAD_ARG;

   packet_mode = opus_smpl_packet_get_mode(st->use_smpl, data);
   packet_bandwidth = opus_mlow_packet_get_bandwidth(st->use_smpl, data);
   packet_frame_size = opus_mlow_packet_get_samples_per_frame(st->use_smpl, data, st->Fs);
   packet_stream_channels = opus_mlow_packet_get_nb_channels(st->use_smpl, data);

   if (!st->use_smpl) {
      count = opus_packet_parse_impl(data, len, self_delimited, &toc, NULL,
                                    size, &offset, packet_offset);
   } else {
#if defined(ENABLE_SMPL)
       count = opus_smpl_packet_parse_impl(data, len, self_delimited, &toc, NULL,
                                    size, &offset, packet_offset);
#endif
   }
   if (count<0)
      return count;

   data += offset;

   if (decode_fec)
   {
      int duration_copy;
      int ret=-1;
      /* If no FEC can be present, run the PLC (recursive call) */
      if (frame_size < packet_frame_size || packet_mode == MODE_CELT_ONLY || st->mode == MODE_CELT_ONLY)
         return opus_decode_native(st, NULL, 0, pcm, frame_size, 0, 0, NULL, soft_clip);
      /* Otherwise, run the PLC on everything except the size for which we might have FEC */
      duration_copy = st->last_packet_duration;
      if (frame_size-packet_frame_size!=0)
      {
         ret = opus_decode_native(st, NULL, 0, pcm, frame_size-packet_frame_size, 0, 0, NULL, soft_clip);
         if (ret<0)
         {
            st->last_packet_duration = duration_copy;
            return ret;
         }
         celt_assert(ret==frame_size-packet_frame_size);
      }
      /* Complete with FEC */
      st->mode = packet_mode;
      st->bandwidth = packet_bandwidth;
      st->frame_size = packet_frame_size;
      st->stream_channels = packet_stream_channels;
      if (!st->use_smpl) {
         ret = opus_decode_frame(st, data, size[0], pcm+st->channels*(frame_size-packet_frame_size),
                              packet_frame_size, 1);
      } else {
#if defined(ENABLE_SMPL)
         // MLow packets, the TOC is embedded in the payload and needs
         // to be extracted separately.
         const int toc_offset = is_mlow_celt(toc) ? 0 : 1;
         ret = opus_smpl_decode_frame(
            st, data + toc_offset, size[0] - toc_offset,
            (toc_offset == 1) ? data[0] : toc,
            pcm + st->channels * (frame_size - packet_frame_size),
            packet_frame_size, 1);
#endif
      }
      if (ret<0)
         return ret;
      else {
         if (OPUS_CHECK_ARRAY(pcm, frame_size*st->channels))
            OPUS_PRINT_INT(frame_size);
         st->last_packet_duration = frame_size;
         return frame_size;
      }
   }

   if (count*packet_frame_size > frame_size)
      return OPUS_BUFFER_TOO_SMALL;

   /* Update the state as the last step to avoid updating it on an invalid packet */
   st->mode = packet_mode;
   st->bandwidth = packet_bandwidth;
   st->frame_size = packet_frame_size;
   st->stream_channels = packet_stream_channels;

   nb_samples=0;
   for (i=0;i<count;i++)
   {
      int ret=-1;
      if (!st->use_smpl) {
          ret = opus_decode_frame(st, data, size[i], pcm + nb_samples * st->channels, frame_size - nb_samples, 0);
      } else {
#if defined(ENABLE_SMPL)
         // MLow packets, the TOC is embedded in the payload and needs
         // to be extracted separately.
         const int toc_offset = is_mlow_celt(toc) ? 0 : 1;
         ret = opus_smpl_decode_frame(
            st, data + toc_offset, size[i] - toc_offset,
            (toc_offset == 1) ? data[0] : toc,
            pcm + nb_samples * st->channels, frame_size - nb_samples, 0);
#endif
      }
      if (ret<0)
         return ret;
      celt_assert(ret==packet_frame_size);
      data += size[i];
      nb_samples += ret;
   }
   st->last_packet_duration = nb_samples;
   if (OPUS_CHECK_ARRAY(pcm, nb_samples*st->channels))
      OPUS_PRINT_INT(nb_samples);
#ifndef FIXED_POINT
   if (soft_clip && ((st->decode_gain > 0) || (st->use_smpl == 0) || (st->mode == MODE_CELT_ONLY)))
      opus_pcm_soft_clip(pcm, nb_samples, st->channels, st->softclip_mem);
   else
      st->softclip_mem[0]=st->softclip_mem[1]=0;
#endif
   return nb_samples;
}

#ifdef FIXED_POINT

int opus_decode(OpusDecoder *st, const unsigned char *data,
      opus_int32 len, opus_val16 *pcm, int frame_size, int decode_fec)
{
   if(frame_size<=0)
      return OPUS_BAD_ARG;
   return opus_decode_native(st, data, len, pcm, frame_size, decode_fec, 0, NULL, 0);
}

#ifndef DISABLE_FLOAT_API
int opus_decode_float(OpusDecoder *st, const unsigned char *data,
      opus_int32 len, float *pcm, int frame_size, int decode_fec)
{
   VARDECL(opus_int16, out);
   int ret, i;
   int nb_samples;
   ALLOC_STACK;

   if(frame_size<=0)
   {
      RESTORE_STACK;
      return OPUS_BAD_ARG;
   }
   if (data != NULL && len > 0 && !decode_fec)
   {
      nb_samples = opus_decoder_get_nb_samples(st, data, len);
      if (nb_samples>0)
         frame_size = IMIN(frame_size, nb_samples);
      else
         return OPUS_INVALID_PACKET;
   }
   celt_assert(st->channels == 1 || st->channels == 2);
   ALLOC(out, frame_size*st->channels, opus_int16);

   ret = opus_decode_native(st, data, len, out, frame_size, decode_fec, 0, NULL, 0);
   if (ret > 0)
   {
      for (i=0;i<ret*st->channels;i++)
         pcm[i] = (1.f/32768.f)*(out[i]);
   }
   RESTORE_STACK;
   return ret;
}
#endif


#else
int opus_decode(OpusDecoder *st, const unsigned char *data,
      opus_int32 len, opus_int16 *pcm, int frame_size, int decode_fec)
{
   VARDECL(float, out);
   int ret, i;
   int nb_samples;
   ALLOC_STACK;

   if(frame_size<=0)
   {
      RESTORE_STACK;
      return OPUS_BAD_ARG;
   }

   if (data != NULL && len > 0 && !decode_fec)
   {
      nb_samples = opus_decoder_get_nb_samples(st, data, len);
      if (nb_samples>0)
         frame_size = IMIN(frame_size, nb_samples);
      else
         return OPUS_INVALID_PACKET;
   }
   celt_assert(st->channels == 1 || st->channels == 2);
   ALLOC(out, frame_size*st->channels, float);

   ret = opus_decode_native(st, data, len, out, frame_size, decode_fec, 0, NULL, 1);
   if (ret > 0)
   {
      for (i=0;i<ret*st->channels;i++)
         pcm[i] = FLOAT2INT16(out[i]);
   }
   RESTORE_STACK;
   return ret;
}

int opus_decode_float(OpusDecoder *st, const unsigned char *data,
      opus_int32 len, opus_val16 *pcm, int frame_size, int decode_fec)
{
   if(frame_size<=0)
      return OPUS_BAD_ARG;
   return opus_decode_native(st, data, len, pcm, frame_size, decode_fec, 0, NULL, 0);
}

#endif

int opus_decoder_ctl(OpusDecoder *st, int request, ...)
{
   int ret = OPUS_OK;
   va_list ap;
   void *silk_dec;
#if defined(ENABLE_SMPL)
   void *smpl_dec;
#endif
   CELTDecoder *celt_dec;

   silk_dec = (char*)st+st->silk_dec_offset;
#if defined(ENABLE_SMPL)
   smpl_dec = (char*)st + st->smpl_dec_offset;
#endif
   celt_dec = (CELTDecoder*)((char*)st+st->celt_dec_offset);

   va_start(ap, request);

   switch (request)
   {
   case OPUS_GET_BANDWIDTH_REQUEST:
   {
      opus_int32 *value = va_arg(ap, opus_int32*);
      if (!value)
      {
         goto bad_arg;
      }
      *value = st->bandwidth;
   }
   break;
   case OPUS_GET_FINAL_RANGE_REQUEST:
   {
      opus_uint32 *value = va_arg(ap, opus_uint32*);
      if (!value)
      {
         goto bad_arg;
      }
      *value = st->rangeFinal;
   }
   break;
   case OPUS_RESET_STATE:
   {
      OPUS_CLEAR((char*)&st->OPUS_DECODER_RESET_START,
            sizeof(OpusDecoder)-
            ((char*)&st->OPUS_DECODER_RESET_START - (char*)st));

      celt_decoder_ctl(celt_dec, OPUS_RESET_STATE);
      silk_InitDecoder( silk_dec );
#if defined(ENABLE_SMPL)
      smpl_InitDecoder( smpl_dec );
#endif
      st->stream_channels = st->channels;
      st->frame_size = st->Fs/400;
   }
   break;
   case OPUS_GET_SAMPLE_RATE_REQUEST:
   {
      opus_int32 *value = va_arg(ap, opus_int32*);
      if (!value)
      {
         goto bad_arg;
      }
      *value = st->Fs;
   }
   break;
   case OPUS_GET_PITCH_REQUEST:
   {
      opus_int32 *value = va_arg(ap, opus_int32*);
      if (!value)
      {
         goto bad_arg;
      }
      if (st->prev_mode == MODE_CELT_ONLY)
         ret = celt_decoder_ctl(celt_dec, OPUS_GET_PITCH(value));
      else {
          if (!st->use_smpl) {
              *value = st->DecControl.prevPitchLag;
          } else {
#if defined(ENABLE_SMPL)
              goto bad_arg;
#endif
          }
      }
   }
   break;
   case OPUS_GET_GAIN_REQUEST:
   {
      opus_int32 *value = va_arg(ap, opus_int32*);
      if (!value)
      {
         goto bad_arg;
      }
      *value = st->decode_gain;
   }
   break;
   case OPUS_SET_GAIN_REQUEST:
   {
       opus_int32 value = va_arg(ap, opus_int32);
       if (value<-32768 || value>32767)
       {
          goto bad_arg;
       }
       st->decode_gain = value;
   }
   break;
   case OPUS_GET_LAST_PACKET_DURATION_REQUEST:
   {
      opus_int32 *value = va_arg(ap, opus_int32*);
      if (!value)
      {
         goto bad_arg;
      }
      *value = st->last_packet_duration;
   }
   break;
   case OPUS_SET_PHASE_INVERSION_DISABLED_REQUEST:
   {
       opus_int32 value = va_arg(ap, opus_int32);
       if(value<0 || value>1)
       {
          goto bad_arg;
       }
       ret = celt_decoder_ctl(celt_dec, OPUS_SET_PHASE_INVERSION_DISABLED(value));
   }
   break;
   case OPUS_GET_PHASE_INVERSION_DISABLED_REQUEST:
   {
       opus_int32 *value = va_arg(ap, opus_int32*);
       if (!value)
       {
          goto bad_arg;
       }
       ret = celt_decoder_ctl(celt_dec, OPUS_GET_PHASE_INVERSION_DISABLED(value));
   }
   break;
   case OPUS_SET_USE_SMPL_REQUEST:
   {
       opus_int32 value = va_arg(ap, opus_int32);
#if defined(ENABLE_SMPL)
       if (value != 0 && value != 1)
           goto bad_arg;
#else
       if (value != 0)
           goto bad_arg;
#endif
       st->use_smpl = value;
   }
   break;
   case OPUS_GET_USE_SMPL_REQUEST:
   {
       opus_int32* value = va_arg(ap, opus_int32*);
       if (!value)
       {
           goto bad_arg;
       }
       *value = st->use_smpl;
   }
   break;

   case OPUS_SET_USE_LPC_POSTFILTER_REQUEST:
   {
       opus_int32 value = va_arg(ap, opus_int32);
#if defined(ENABLE_SMPL)
       if (value < 0 || value > SMPL_LPC_PSTF_MODE_WB_OFF_SWB_OFF)
           goto bad_arg;
       st->smpl_DecControl.LPC_postfilter_mode = (SmplLpcPostFilterMode)value;
#else
       if (value != 0)
           goto bad_arg;
#endif
   }
   break;

   case OPUS_GET_USE_LPC_POSTFILTER_REQUEST:
   {
       opus_int32* value = va_arg(ap, opus_int32*);
       if (!value)
       {
           goto bad_arg;
       }
#if defined(ENABLE_SMPL)
       *value = (opus_int32)st->smpl_DecControl.LPC_postfilter_mode;
#else
       *value = 0;
#endif
   }
   break;

   default:
      /*fprintf(stderr, "unknown opus_decoder_ctl() request: %d", request);*/
      ret = OPUS_UNIMPLEMENTED;
      break;
   }

   va_end(ap);
   return ret;
bad_arg:
   va_end(ap);
   return OPUS_BAD_ARG;
}

void opus_decoder_destroy(OpusDecoder *st)
{
#if defined(ENABLE_SMPL)
   if (!st) {
      return;
   }
   smpl_close_dec_files((char*)st + st->smpl_dec_offset);
#endif
   opus_free(st);
}


int opus_packet_get_bandwidth(const unsigned char *data)
{
   int bandwidth;
   if (data[0]&0x80)
   {
      bandwidth = OPUS_BANDWIDTH_MEDIUMBAND + ((data[0]>>5)&0x3);
      if (bandwidth == OPUS_BANDWIDTH_MEDIUMBAND)
         bandwidth = OPUS_BANDWIDTH_NARROWBAND;
   } else if ((data[0]&0x60) == 0x60)
   {
      bandwidth = (data[0]&0x10) ? OPUS_BANDWIDTH_FULLBAND :
                                   OPUS_BANDWIDTH_SUPERWIDEBAND;
   } else {
      bandwidth = OPUS_BANDWIDTH_NARROWBAND + ((data[0]>>5)&0x3);
   }
   return bandwidth;
}

int opus_packet_get_nb_channels(const unsigned char *data)
{
   return (data[0] & 0x4) ? 2 : 1;
}

int opus_packet_get_nb_frames(const unsigned char packet[], opus_int32 len)
{
   int count;
   if (len<1)
      return OPUS_BAD_ARG;
   count = packet[0]&0x3;
   if (count==0)
      return 1;
   else if (count!=3)
      return 2;
   else if (len<2)
      return OPUS_INVALID_PACKET;
   else
      return packet[1]&0x3F;
}

int opus_packet_get_nb_samples(const unsigned char packet[], opus_int32 len,
      opus_int32 Fs)
{
   int samples;
   int count = opus_packet_get_nb_frames(packet, len);

   if (count<0)
      return count;

   samples = count*opus_packet_get_samples_per_frame(packet, Fs);
   /* Can't have more than 120 ms */
   if (samples*25 > Fs*3)
      return OPUS_INVALID_PACKET;
   else
      return samples;
}

int opus_decoder_get_nb_samples(const OpusDecoder *dec,
      const unsigned char packet[], opus_int32 len)
{
   return opus_mlow_packet_get_nb_samples(dec->use_smpl, packet, len, dec->Fs);
}

/**
 * MLow APIs.
 */
#if defined(ENABLE_SMPL)
int mlow_packet_get_bandwidth(const unsigned char *data) {
      opus_smpl_TOC_parameters toc_params;
      opus_smpl_decode_TOC(data, &toc_params);
      return toc_params.fs_mode;
}

int mlow_packet_get_nb_channels(const unsigned char *data)
{
   if (data[0] >> 6 != 3)
      return (data[0] & 0x1) ? 2 : 1;
   else
      return ((data[0] >> 1) & 0x1) ? 2 : 1;
}

int mlow_packet_get_nb_frames(const unsigned char packet[], opus_int32 len)
{
    return opus_smpl_get_frames_per_packet(packet, len);
}

int mlow_packet_get_nb_samples(const unsigned char packet[], opus_int32 len,
      opus_int32 Fs)
{
   int samples;
   int count = mlow_packet_get_nb_frames(packet, len);

   if (count<0)
      return count;

   samples = count*mlow_packet_get_samples_per_frame(packet, Fs);
   /* Can't have more than 120 ms */
   if (samples*25 > Fs*3)
      return OPUS_INVALID_PACKET;
   else
      return samples;
}
#endif // defined(ENABLE_SMPL)
