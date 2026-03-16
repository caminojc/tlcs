#include "smpl_typedef.h"
#include "opus_smpl_encode.h"
#include "stack_alloc.h"
#include "silk/define.h"
#ifdef FIXED_POINT
#include "fixed/structs_FIX.h"
#else
#include "float/structs_FLP.h"
#endif
#include "float_cast.h"
#include "pitch.h"
#include "os_support.h"
#include "tuning_parameters.h"
#include "src/analysis.h"
#include "smpl_api.h"
#include "smpl_typedef.h"
#include "smpl_param_coding.h"
#include "opus_smpl_repacketizer.h"

#if defined(ENABLE_SMPL)

#define MAX_ENCODER_BUFFER 480

#ifndef DISABLE_FLOAT_API
#define PSEUDO_SNR_THRESHOLD 316.23f    /* 10^(25/10) */
#endif

typedef struct {
   opus_val32 XX, XY, YY;
   opus_val16 smoothed_width;
   opus_val16 max_follower;
} StereoWidthState;

struct OpusEncoder {
    int          celt_enc_offset;
    int          silk_enc_offset;
    silk_EncControlStruct silk_mode;
#if defined(ENABLE_SMPL)
    int          smpl_enc_offset;
    smpl_EncControlStruct smpl_mode;
#endif
    opus_int32   use_smpl;
    int          application;
    int          channels;
    int          delay_compensation;
    int          force_channels;
    int          signal_type;
    int          user_bandwidth;
    int          max_bandwidth;
    int          user_forced_mode;
    int          voice_ratio;
    opus_int32   Fs;
    int          use_vbr;
    int          vbr_constraint;
    int          variable_duration;
    opus_int32   bitrate_bps;
    opus_int32   user_bitrate_bps;
    int          lsb_depth;
    int          encoder_buffer;
    int          lfe;
    int          arch;
    int          use_dtx;                 /* general DTX for both SILK and CELT */
    int          fec_config;
#ifndef DISABLE_FLOAT_API
    TonalityAnalysisState analysis;
#endif

#define OPUS_ENCODER_RESET_START stream_channels
    int          stream_channels;
    opus_int16   hybrid_stereo_width_Q14;
    opus_int32   variable_HP_smth2_Q15;
    opus_val16   prev_HB_gain;
    opus_val32   hp_mem[4];
    int          mode;
    int          prev_mode;
    int          prev_channels;
    int          prev_framesize;
    int          bandwidth;
    /* Bandwidth determined automatically from the rate (before any other adjustment) */
    int          auto_bandwidth;
    int          silk_bw_switch;
    /* Sampling rate (at the API level) */
    int          first;
    opus_val16 * energy_masking;
    StereoWidthState width_mem;
    opus_val16   delay_buffer[MAX_ENCODER_BUFFER*2];
#ifndef DISABLE_FLOAT_API
    int          detected_bandwidth;
    int          nb_no_activity_ms_Q1;
    opus_val32   peak_signal_energy;
#endif
    int          nonfinal_frame; /* current frame is not the final in a packet */
    opus_uint32  rangeFinal;
};

// Functions sitting in opus_encoder.c
opus_val16 compute_stereo_width(const opus_val16 *pcm, int frame_size, opus_int32 Fs, StereoWidthState *mem);
int is_digital_silence(const opus_val16* pcm, int frame_size, int channels, int lsb_depth);

/* Transition tables for the voice and music. First column is the
   middle (memoriless) threshold. The second column is the hysteresis
   (difference with the middle) */
static const opus_int32 mono_voice_bandwidth_thresholds[8] = {
         9000,  700, /* NB<->MB */
         9000,  700, /* MB<->WB */
        13500, 1000, /* WB<->SWB */
        14000, 2000, /* SWB<->FB */
};
static const opus_int32 mono_music_bandwidth_thresholds[8] = {
         9000,  700, /* NB<->MB */
         9000,  700, /* MB<->WB */
        11000, 1000, /* WB<->SWB */
        12000, 2000, /* SWB<->FB */
};
static const opus_int32 stereo_voice_bandwidth_thresholds[8] = {
         9000,  700, /* NB<->MB */
         9000,  700, /* MB<->WB */
        13500, 1000, /* WB<->SWB */
        14000, 2000, /* SWB<->FB */
};
static const opus_int32 stereo_music_bandwidth_thresholds[8] = {
         9000,  700, /* NB<->MB */
         9000,  700, /* MB<->WB */
        11000, 1000, /* WB<->SWB */
        12000, 2000, /* SWB<->FB */
};
/* Threshold bit-rates for switching between mono and stereo */
static const opus_int32 stereo_voice_threshold = 19000;
static const opus_int32 stereo_music_threshold = 17000;

/* Threshold bit-rate for switching between SILK/hybrid and CELT-only */
static const opus_int32 mode_thresholds[2][2] = {
      /* voice */ /* music */
      {  64000,      10000}, /* mono */
      {  44000,      10000}, /* stereo */
};

// Rewrite TOC, but just for CELT. For smpl it is already filled in
static void gen_smpl_toc(unsigned char *toc, int nb_frames, int mode, int framerate, int bandwidth, int channels)
{
    int period;
    period = 0;
    if (mode == MODE_HYBRID) {
        smpl_assert(0);
    }
    if (mode == MODE_CELT_ONLY) {
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
        //
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

        *toc = (unsigned char)3<<6;
        while (framerate < 400)
        {
            framerate <<= 1;
            period++;
        }
        int tmp = bandwidth-OPUS_BANDWIDTH_MEDIUMBAND;
        if (tmp < 0)
            tmp = 0;
        // toc = 0x80;
        *toc |= tmp << 4;
        *toc |= period<<2;
        *toc |= (channels==2)<<1;
        *toc |= (nb_frames > 1); // multiple frames per packet for CELT
    }
    return;
}

#ifndef FIXED_POINT
static void silk_biquad_float(
    const opus_val16      *in,            /* I:    Input signal                   */
    const opus_int32      *B_Q28,         /* I:    MA coefficients [3]            */
    const opus_int32      *A_Q28,         /* I:    AR coefficients [2]            */
    opus_val32            *S,             /* I/O:  State vector [2]               */
    opus_val16            *out,           /* O:    Output signal                  */
    const opus_int32      len,            /* I:    Signal length (must be even)   */
    int stride
)
{
    /* DIRECT FORM II TRANSPOSED (uses 2 element state vector) */
    opus_int   k;
    opus_val32 vout;
    opus_val32 inval;
    opus_val32 A[2], B[3];

    A[0] = (opus_val32)(A_Q28[0] * (1.f/((opus_int32)1<<28)));
    A[1] = (opus_val32)(A_Q28[1] * (1.f/((opus_int32)1<<28)));
    B[0] = (opus_val32)(B_Q28[0] * (1.f/((opus_int32)1<<28)));
    B[1] = (opus_val32)(B_Q28[1] * (1.f/((opus_int32)1<<28)));
    B[2] = (opus_val32)(B_Q28[2] * (1.f/((opus_int32)1<<28)));

    /* Negate A_Q28 values and split in two parts */

    for( k = 0; k < len; k++ ) {
        /* S[ 0 ], S[ 1 ]: Q12 */
        inval = in[ k*stride ];
        vout = S[ 0 ] + B[0]*inval;

        S[ 0 ] = S[1] - vout*A[0] + B[1]*inval;

        S[ 1 ] = - vout*A[1] + B[2]*inval + VERY_SMALL;

        /* Scale back to Q0 and saturate */
        out[ k*stride ] = vout;
    }
}
#endif

static void hp_cutoff(const opus_val16 *in, opus_int32 cutoff_Hz, opus_val16 *out, opus_val32 *hp_mem, int len, int channels, opus_int32 Fs, int arch)
{
   opus_int32 B_Q28[ 3 ], A_Q28[ 2 ];
   opus_int32 Fc_Q19, r_Q28, r_Q22;
   (void)arch;

   silk_assert( cutoff_Hz <= silk_int32_MAX / SILK_FIX_CONST( 1.5 * 3.14159 / 1000, 19 ) );
   Fc_Q19 = silk_DIV32_16( silk_SMULBB( SILK_FIX_CONST( 1.5 * 3.14159 / 1000, 19 ), cutoff_Hz ), Fs/1000 );
   silk_assert( Fc_Q19 > 0 && Fc_Q19 < 32768 );

   r_Q28 = SILK_FIX_CONST( 1.0, 28 ) - silk_MUL( SILK_FIX_CONST( 0.92, 9 ), Fc_Q19 );

   /* b = r * [ 1; -2; 1 ]; */
   /* a = [ 1; -2 * r * ( 1 - 0.5 * Fc^2 ); r^2 ]; */
   B_Q28[ 0 ] = r_Q28;
   B_Q28[ 1 ] = silk_LSHIFT( -r_Q28, 1 );
   B_Q28[ 2 ] = r_Q28;

   /* -r * ( 2 - Fc * Fc ); */
   r_Q22  = silk_RSHIFT( r_Q28, 6 );
   A_Q28[ 0 ] = silk_SMULWW( r_Q22, silk_SMULWW( Fc_Q19, Fc_Q19 ) - SILK_FIX_CONST( 2.0,  22 ) );
   A_Q28[ 1 ] = silk_SMULWW( r_Q22, r_Q22 );

#ifdef FIXED_POINT
   if( channels == 1 ) {
      silk_biquad_alt_stride1( in, B_Q28, A_Q28, hp_mem, out, len );
   } else {
      silk_biquad_alt_stride2( in, B_Q28, A_Q28, hp_mem, out, len, arch );
   }
#else
   silk_biquad_float( in, B_Q28, A_Q28, hp_mem, out, len, channels );
   if( channels == 2 ) {
       silk_biquad_float( in+1, B_Q28, A_Q28, hp_mem+2, out+1, len, channels );
   }
#endif
}

#ifdef FIXED_POINT
static void dc_reject(const opus_val16 *in, opus_int32 cutoff_Hz, opus_val16 *out, opus_val32 *hp_mem, int len, int channels, opus_int32 Fs)
{
   int c, i;
   int shift;

   /* Approximates -round(log2(6.3*cutoff_Hz/Fs)) */
   shift=celt_ilog2(Fs/(cutoff_Hz*4));
   for (c=0;c<channels;c++)
   {
      for (i=0;i<len;i++)
      {
         opus_val32 x, y;
         x = SHL32(EXTEND32(in[channels*i+c]), 14);
         y = x-hp_mem[2*c];
         hp_mem[2*c] = hp_mem[2*c] + PSHR32(x - hp_mem[2*c], shift);
         out[channels*i+c] = EXTRACT16(SATURATE(PSHR32(y, 14), 32767));
      }
   }
}

#else
static void dc_reject(const opus_val16 *in, opus_int32 cutoff_Hz, opus_val16 *out, opus_val32 *hp_mem, int len, int channels, opus_int32 Fs)
{
   int i;
   float coef, coef2;
   coef = 6.3f*cutoff_Hz/Fs;
   coef2 = 1-coef;
   if (channels==2)
   {
      float m0, m2;
      m0 = hp_mem[0];
      m2 = hp_mem[2];
      for (i=0;i<len;i++)
      {
         opus_val32 x0, x1, out0, out1;
         x0 = in[2*i+0];
         x1 = in[2*i+1];
         out0 = x0-m0;
         out1 = x1-m2;
         m0 = coef*x0 + VERY_SMALL + coef2*m0;
         m2 = coef*x1 + VERY_SMALL + coef2*m2;
         out[2*i+0] = out0;
         out[2*i+1] = out1;
      }
      hp_mem[0] = m0;
      hp_mem[2] = m2;
   } else {
      float m0;
      m0 = hp_mem[0];
      for (i=0;i<len;i++)
      {
         opus_val32 x, y;
         x = in[i];
         y = x-m0;
         m0 = coef*x + VERY_SMALL + coef2*m0;
         out[i] = y;
      }
      hp_mem[0] = m0;
   }
}
#endif

static void stereo_fade(const opus_val16 *in, opus_val16 *out, opus_val16 g1, opus_val16 g2,
        int overlap48, int frame_size, int channels, const opus_val16 *window, opus_int32 Fs)
{
    int i;
    int overlap;
    int inc;
    inc = 48000/Fs;
    overlap=overlap48/inc;
    g1 = Q15ONE-g1;
    g2 = Q15ONE-g2;
    for (i=0;i<overlap;i++)
    {
       opus_val32 diff;
       opus_val16 g, w;
       w = MULT16_16_Q15(window[i*inc], window[i*inc]);
       g = SHR32(MAC16_16(MULT16_16(w,g2),
             Q15ONE-w, g1), 15);
       diff = EXTRACT16(HALF32((opus_val32)in[i*channels] - (opus_val32)in[i*channels+1]));
       diff = MULT16_16_Q15(g, diff);
       out[i*channels] = out[i*channels] - diff;
       out[i*channels+1] = out[i*channels+1] + diff;
    }
    for (;i<frame_size;i++)
    {
       opus_val32 diff;
       diff = EXTRACT16(HALF32((opus_val32)in[i*channels] - (opus_val32)in[i*channels+1]));
       diff = MULT16_16_Q15(g2, diff);
       out[i*channels] = out[i*channels] - diff;
       out[i*channels+1] = out[i*channels+1] + diff;
    }
}

static void gain_fade(const opus_val16 *in, opus_val16 *out, opus_val16 g1, opus_val16 g2,
        int overlap48, int frame_size, int channels, const opus_val16 *window, opus_int32 Fs)
{
    int i;
    int inc;
    int overlap;
    int c;
    inc = 48000/Fs;
    overlap=overlap48/inc;
    if (channels==1)
    {
       for (i=0;i<overlap;i++)
       {
          opus_val16 g, w;
          w = MULT16_16_Q15(window[i*inc], window[i*inc]);
          g = SHR32(MAC16_16(MULT16_16(w,g2),
                Q15ONE-w, g1), 15);
          out[i] = MULT16_16_Q15(g, in[i]);
       }
    } else {
       for (i=0;i<overlap;i++)
       {
          opus_val16 g, w;
          w = MULT16_16_Q15(window[i*inc], window[i*inc]);
          g = SHR32(MAC16_16(MULT16_16(w,g2),
                Q15ONE-w, g1), 15);
          out[i*2] = MULT16_16_Q15(g, in[i*2]);
          out[i*2+1] = MULT16_16_Q15(g, in[i*2+1]);
       }
    }
    c=0;do {
       for (i=overlap;i<frame_size;i++)
       {
          out[i*channels+c] = MULT16_16_Q15(g2, in[i*channels+c]);
       }
    }
    while (++c<channels);
}

static opus_int32 user_bitrate_to_bitrate(OpusEncoder *st, int frame_size, int max_data_bytes)
{
  if(!frame_size)frame_size=st->Fs/400;
  if (st->user_bitrate_bps==OPUS_AUTO)
    return 60*st->Fs/frame_size + st->Fs*st->channels;
  else if (st->user_bitrate_bps==OPUS_BITRATE_MAX)
    return max_data_bytes*8*st->Fs/frame_size;
  else
    return st->user_bitrate_bps;
}

/* Returns the equivalent bitrate corresponding to 20 ms frames,
   complexity 10 VBR operation. */
static opus_int32 compute_equiv_rate(opus_int32 bitrate, int channels,
      int frame_rate, int vbr, int mode, int complexity, int loss)
{
   opus_int32 equiv;
   equiv = bitrate;
   /* Take into account overhead from smaller frames. */
   if (frame_rate > 50)
      equiv -= (40*channels+20)*(frame_rate - 50);
   /* CBR is about a 8% penalty for both SILK and CELT. */
   if (!vbr)
      equiv -= equiv/12;
   /* Complexity makes about 10% difference (from 0 to 10) in general. */
   equiv = equiv * (90+complexity)/100;
   if (mode == MODE_SMPL_ONLY || mode == MODE_HYBRID)
   {
      /* SILK complexity 0-1 uses the non-delayed-decision NSQ, which
         costs about 20%. */
      if (complexity<2)
         equiv = equiv*4/5;
      equiv -= equiv*loss/(6*loss + 10);
   } else if (mode == MODE_CELT_ONLY) {
      /* CELT complexity 0-4 doesn't have the pitch filter, which costs
         about 10%. */
      if (complexity<5)
         equiv = equiv*9/10;
   } else {
      /* Mode not known yet */
      /* Half the SILK loss*/
      equiv -= equiv*loss/(12*loss + 20);
   }
   return equiv;
}

#ifdef FIXED_POINT
static opus_val32 compute_frame_energy(const opus_val16 *pcm, int frame_size, int channels, int arch)
{
   int i;
   opus_val32 sample_max;
   int max_shift;
   int shift;
   opus_val32 energy = 0;
   int len = frame_size*channels;
   (void)arch;
   /* Max amplitude in the signal */
   sample_max = celt_maxabs16(pcm, len);

   /* Compute the right shift required in the MAC to avoid an overflow */
   max_shift = celt_ilog2(len);
   shift = IMAX(0, (celt_ilog2(sample_max) << 1) + max_shift - 28);

   /* Compute the energy */
   for (i=0; i<len; i++)
      energy += SHR32(MULT16_16(pcm[i], pcm[i]), shift);

   /* Normalize energy by the frame size and left-shift back to the original position */
   energy /= len;
   energy = SHL32(energy, shift);

   return energy;
}
#else
static opus_val32 compute_frame_energy(const opus_val16 *pcm, int frame_size, int channels, int arch)
{
   int len = frame_size*channels;
   return celt_inner_prod(pcm, pcm, len, arch)/len;
}
#endif

static opus_int32 encode_multiframe_packet(OpusEncoder* st,
    const opus_val16* pcm,
    int nb_frames,
    int frame_size,
    unsigned char* data,
    opus_int32 out_data_bytes,
    int to_celt,
    int lsb_depth,
    int float_api)
{
    int i;
    int ret = 0;
    VARDECL(unsigned char, tmp_data);
    int bak_mode, bak_bandwidth, bak_channels, bak_to_mono;
    VARDECL(OpusRepacketizer, rp);
    int max_header_bytes;
    opus_int32 bytes_per_frame;
    opus_int32 cbr_bytes;
    opus_int32 repacketize_len;
    int tmp_len;
    ALLOC_STACK;

    /* Worst cases:
     * 2 frames: Code 2 with different compressed sizes
     * >2 frames: Code 3 VBR */
    max_header_bytes = nb_frames == 2 ? 3 : (2 + (nb_frames - 1) * 2);

    if (st->use_vbr || st->user_bitrate_bps == OPUS_BITRATE_MAX)
        repacketize_len = out_data_bytes;
    else {
        cbr_bytes = 3 * st->bitrate_bps / (3 * 8 * st->Fs / (frame_size * nb_frames));
        repacketize_len = IMIN(cbr_bytes, out_data_bytes);
    }
    bytes_per_frame = IMIN(1276, 1 + (repacketize_len - max_header_bytes) / nb_frames);

    ALLOC(tmp_data, nb_frames * bytes_per_frame, unsigned char);
    ALLOC(rp, 1, OpusRepacketizer);
    opus_repacketizer_init(rp);
    opus_repacketizer_set_using_mlow(rp, 1);

    bak_mode = st->user_forced_mode;
    bak_bandwidth = st->user_bandwidth;
    bak_channels = st->force_channels;

    st->user_forced_mode = st->mode;
    st->user_bandwidth = st->bandwidth;
    st->force_channels = st->stream_channels;

    bak_to_mono = st->smpl_mode.toMono;

    if (bak_to_mono)
        st->force_channels = 1;
    else
        st->prev_channels = st->stream_channels;

    for (i = 0; i < nb_frames; i++)
    {
        st->smpl_mode.toMono = 0;
        st->nonfinal_frame = i < (nb_frames - 1);

        /* When switching from SILK/Hybrid to CELT, only ask for a switch at the last frame */
        if (to_celt && i == nb_frames - 1)
            st->user_forced_mode = MODE_CELT_ONLY;

        tmp_len = opus_smpl_encode_native(st, pcm + i * (st->channels * frame_size), frame_size,
            tmp_data + i * bytes_per_frame, bytes_per_frame, lsb_depth, NULL, 0, 0, 0, 0,
            NULL, float_api);

        if (tmp_len < 0)
        {
            RESTORE_STACK;
            return OPUS_INTERNAL_ERROR;
        }

        ret = opus_smpl_repacketizer_cat(rp, tmp_data + i * bytes_per_frame, tmp_len);

        if (ret < 0)
        {
            RESTORE_STACK;
            return OPUS_INTERNAL_ERROR;
        }
    }

    ret = opus_smpl_repacketizer_out_range_impl(rp, 0, nb_frames, data, repacketize_len, 0, !st->use_vbr);

    if (ret < 0)
    {
        RESTORE_STACK;
        return OPUS_INTERNAL_ERROR;
    }

    /* Discard configs that were forced locally for the purpose of repacketization */
    st->user_forced_mode = bak_mode;
    st->user_bandwidth = bak_bandwidth;
    st->force_channels = bak_channels;
    st->smpl_mode.toMono = bak_to_mono;

    RESTORE_STACK;
    return ret;
}

opus_int32 opus_smpl_encode_secondary_native(OpusEncoder* st, unsigned char* data, opus_int32 out_data_bytes)
{
    if (st->mode != MODE_SMPL_ONLY || st->prev_mode != MODE_SMPL_ONLY) {
        return 0;
    }
    opus_int32 nBytes;
    ec_enc enc;
    unsigned char* smpl_toc_byte = &data[0];
    ec_enc_init(&enc, data + 1, out_data_bytes - 1);

    void *smpl_enc = (char*)st + st->smpl_enc_offset;
    smpl_Encode_secondary(smpl_enc, &st->smpl_mode, &enc, smpl_toc_byte, &nBytes);
    ec_enc_done(&enc);

    /*When in LPC only mode it's perfectly
    reasonable to strip off trailing zero bytes as
    the required range decoder behavior is to
    fill these in. This can't be done when the MDCT
    modes are used because the decoder needs to know
    the actual length for allocation purposes.*/
    while (nBytes > 2 && data[nBytes-1] == 0) nBytes--;

    return nBytes;
}

opus_int32 opus_smpl_encode_native(OpusEncoder *st, const opus_val16 *pcm, int frame_size,
                unsigned char *data, opus_int32 out_data_bytes, int lsb_depth,
                const void *analysis_pcm, opus_int32 analysis_size, int c1, int c2,
                int analysis_channels, downmix_func downmix, int float_api)
{
    void *smpl_enc;
    CELTEncoder *celt_enc;
    int i;
    int ret=0;
    opus_int32 nBytes;
    ec_enc enc;
    int prefill=0;
    int start_band = 0;
    int celt_to_smpl = 0;
    VARDECL(opus_val16, pcm_buf);
    int nb_compr_bytes;
    int to_celt = 0;
    int cutoff_Hz, hp_freq_smth1;
    int voice_est; /* Probability of voice in Q7 */
    opus_int32 equiv_rate;
    int delay_compensation;
    int frame_rate;
    opus_int32 max_rate; /* Max bitrate we're allowed to use */
    int curr_bandwidth;
    opus_val16 HB_gain;
    opus_int32 max_data_bytes; /* Max number of bytes we're allowed to use */
    int total_buffer;
    opus_val16 stereo_width;
    const CELTMode *celt_mode;
#ifndef DISABLE_FLOAT_API
    AnalysisInfo analysis_info;
    int analysis_read_pos_bak=-1;
    int analysis_read_subframe_bak=-1;
    int is_silence = 0;
#endif
    opus_int activity = VAD_NO_DECISION;

    VARDECL(opus_val16, tmp_prefill);

    ALLOC_STACK;

    max_data_bytes = IMIN(1276, out_data_bytes);

    st->rangeFinal = 0;
    if (frame_size <= 0 || max_data_bytes <= 0)
    {
       RESTORE_STACK;
       return OPUS_BAD_ARG;
    }

    /* Cannot encode 100 ms in 1 byte */
    if (max_data_bytes==1 && st->Fs==(frame_size*10))
    {
      RESTORE_STACK;
      return OPUS_BUFFER_TOO_SMALL;
    }

    smpl_enc = (char*)st+st->smpl_enc_offset;
    celt_enc = (CELTEncoder*)((char*)st+st->celt_enc_offset);
    if (st->application == OPUS_APPLICATION_RESTRICTED_LOWDELAY)
       delay_compensation = 0;
    else
       delay_compensation = st->delay_compensation;

    lsb_depth = IMIN(lsb_depth, st->lsb_depth);

    celt_encoder_ctl(celt_enc, CELT_GET_MODE(&celt_mode));
#ifndef DISABLE_FLOAT_API
    analysis_info.valid = 0;
#ifdef FIXED_POINT
    if (st->smpl_mode.complexity >= 10 && st->Fs>=16000)
#else
    if (st->smpl_mode.complexity >= 7 && st->Fs>=16000)
#endif
    {
       is_silence = is_digital_silence(pcm, frame_size, st->channels, lsb_depth);
       analysis_read_pos_bak = st->analysis.read_pos;
       analysis_read_subframe_bak = st->analysis.read_subframe;
       run_analysis(&st->analysis, celt_mode, analysis_pcm, analysis_size, frame_size,
             c1, c2, analysis_channels, st->Fs,
             lsb_depth, downmix, &analysis_info);

       /* Track the peak signal energy */
       if (!is_silence && analysis_info.activity_probability > DTX_ACTIVITY_THRESHOLD)
          st->peak_signal_energy = MAX32(MULT16_32_Q15(QCONST16(0.999f, 15), st->peak_signal_energy),
                compute_frame_energy(pcm, frame_size, st->channels, st->arch));
    } else if (st->analysis.initialized) {
       tonality_analysis_reset(&st->analysis);
    }
#else
    (void)analysis_pcm;
    (void)analysis_size;
    (void)c1;
    (void)c2;
    (void)analysis_channels;
    (void)downmix;
#endif

#ifndef DISABLE_FLOAT_API
    /* Reset voice_ratio if this frame is not silent or if analysis is disabled.
     * Otherwise, preserve voice_ratio from the last non-silent frame */
    if (!is_silence)
      st->voice_ratio = -1;

    if (is_silence)
    {
       activity = !is_silence;
    } else if (analysis_info.valid)
    {
       activity = analysis_info.activity_probability >= DTX_ACTIVITY_THRESHOLD;
       if (!activity)
       {
           /* Mark as active if this noise frame is sufficiently loud */
           opus_val32 noise_energy = compute_frame_energy(pcm, frame_size, st->channels, st->arch);
           activity = st->peak_signal_energy < (PSEUDO_SNR_THRESHOLD * noise_energy);
       }
    }

    st->detected_bandwidth = 0;
    if (analysis_info.valid)
    {
       int analysis_bandwidth;
       if (st->signal_type == OPUS_AUTO)
       {
          float prob;
          if (st->prev_mode == 0)
             prob = analysis_info.music_prob;
          else if (st->prev_mode == MODE_CELT_ONLY)
             prob = analysis_info.music_prob_max;
          else
             prob = analysis_info.music_prob_min;
          st->voice_ratio = (int)floor(.5+100*(1-prob));
       }

       analysis_bandwidth = analysis_info.bandwidth;
       if (analysis_bandwidth<=12)
          st->detected_bandwidth = OPUS_BANDWIDTH_NARROWBAND;
       else if (analysis_bandwidth<=14)
          st->detected_bandwidth = OPUS_BANDWIDTH_MEDIUMBAND;
       else if (analysis_bandwidth<=16)
          st->detected_bandwidth = OPUS_BANDWIDTH_WIDEBAND;
       else if (analysis_bandwidth<=18)
          st->detected_bandwidth = OPUS_BANDWIDTH_SUPERWIDEBAND;
       else
          st->detected_bandwidth = OPUS_BANDWIDTH_FULLBAND;
    }
#else
    st->voice_ratio = -1;
#endif

    if (st->channels==2 && st->force_channels!=1)
       stereo_width = compute_stereo_width(pcm, frame_size, st->Fs, &st->width_mem);
    else
       stereo_width = 0;
    total_buffer = delay_compensation;
    st->bitrate_bps = user_bitrate_to_bitrate(st, frame_size, max_data_bytes);

    frame_rate = st->Fs/frame_size;
    max_rate = frame_rate*max_data_bytes*8;

    /* Equivalent 20-ms rate for mode/channel/bandwidth decisions */
    equiv_rate = compute_equiv_rate(st->bitrate_bps, st->channels, st->Fs/frame_size,
          st->use_vbr, 0, st->smpl_mode.complexity, st->smpl_mode.packetLossPercentage);

    if (st->signal_type == OPUS_SIGNAL_VOICE)
       voice_est = 127;
    else if (st->signal_type == OPUS_SIGNAL_MUSIC)
       voice_est = 0;
    else if (st->voice_ratio >= 0)
    {
       voice_est = st->voice_ratio*327>>8;
       /* For AUDIO, never be more than 90% confident of having speech */
       if (st->application == OPUS_APPLICATION_AUDIO)
          voice_est = IMIN(voice_est, 115);
    } else if (st->application == OPUS_APPLICATION_VOIP)
       voice_est = 115;
    else
       voice_est = 48;

    if (st->force_channels!=OPUS_AUTO && st->channels == 2)
    {
        st->stream_channels = st->force_channels;
    } else {
#ifdef FUZZING
        (void)stereo_music_threshold;
        (void)stereo_voice_threshold;
       /* Random mono/stereo decision */
       if (st->channels == 2 && (rand()&0x1F)==0)
          st->stream_channels = 3-st->stream_channels;
#else
       /* Rate-dependent mono-stereo decision */
       if (st->channels == 2)
       {
          opus_int32 stereo_threshold;
          stereo_threshold = stereo_music_threshold + ((voice_est*voice_est*(stereo_voice_threshold-stereo_music_threshold))>>14);
          if (st->stream_channels == 2)
             stereo_threshold -= 1000;
          else
             stereo_threshold += 1000;
          st->stream_channels = (equiv_rate > stereo_threshold) ? 2 : 1;
       } else {
          st->stream_channels = st->channels;
       }
#endif
    }
    /* Update equivalent rate for channels decision. */
    equiv_rate = compute_equiv_rate(st->bitrate_bps, st->stream_channels, st->Fs/frame_size,
          st->use_vbr, 0, st->smpl_mode.complexity, st->smpl_mode.packetLossPercentage);

    /* Allow SMPL DTX if DTX is enabled but the generalized DTX cannot be used,
       e.g. because of the complexity setting or sample rate. */
#ifndef DISABLE_FLOAT_API
    st->smpl_mode.useDTX = st->use_dtx;
    if (!analysis_info.valid)
        activity = VAD_NO_DECISION;
#else
    st->smpl_mode.useDTX = st->use_dtx;
#endif

    /* Mode selection depending on application and signal type */
    if (st->application == OPUS_APPLICATION_RESTRICTED_LOWDELAY)
    {
       st->mode = MODE_CELT_ONLY;
    } else if (st->user_forced_mode == OPUS_AUTO)
    {
#ifdef FUZZING
        (void)stereo_width;
        (void)mode_thresholds;
       /* Random mode switching */
       if ((rand()&0xF)==0)
       {
          if ((rand()&0x1)==0)
             st->mode = MODE_CELT_ONLY;
          else
             st->mode = MODE_SMPL_ONLY;
       } else {
          if (st->prev_mode==MODE_CELT_ONLY)
             st->mode = MODE_CELT_ONLY;
          else
             st->mode = MODE_SMPL_ONLY;
       }
#else
       opus_int32 mode_voice, mode_music;
       opus_int32 threshold;

       /* Interpolate based on stereo width */
       mode_voice = (opus_int32)(MULT16_32_Q15(Q15ONE-stereo_width,mode_thresholds[0][0])
             + MULT16_32_Q15(stereo_width,mode_thresholds[1][0]));
       mode_music = (opus_int32)(MULT16_32_Q15(Q15ONE-stereo_width,mode_thresholds[1][1])
             + MULT16_32_Q15(stereo_width,mode_thresholds[1][1]));
       /* Interpolate based on speech/music probability */
       threshold = mode_music + ((voice_est*voice_est*(mode_voice-mode_music))>>14);
       /* Bias towards SMPL for VoIP because of some useful features */
       if (st->application == OPUS_APPLICATION_VOIP)
          threshold += 8000;

       /*printf("%f %d\n", stereo_width/(float)Q15ONE, threshold);*/
       /* Hysteresis */
       if (st->prev_mode == MODE_CELT_ONLY)
           threshold -= 4000;
       else if (st->prev_mode>0)
           threshold += 4000;

       st->mode = (equiv_rate >= threshold) ? MODE_CELT_ONLY: MODE_SMPL_ONLY;

       /* When FEC is enabled and there's enough packet loss, use SMPL.
          Unless the FEC is set to 2, in which case we don't switch to SMPL if we're confident we have music. */
       if (st->smpl_mode.useInBandFEC && st->smpl_mode.packetLossPercentage > (128-voice_est)>>4 && (st->fec_config != 2 || voice_est > 25))
          st->mode = MODE_SMPL_ONLY;
       /* When encoding voice and DTX is enabled but the generalized DTX cannot be used,
          use SMPL in order to make use of its DTX. */
       if (st->smpl_mode.useDTX && voice_est > 100)
          st->mode = MODE_SMPL_ONLY;
#endif

    } else {
       st->mode = st->user_forced_mode;
    }

    /* Override the chosen mode to make sure we meet the requested frame size */
    if (st->mode != MODE_CELT_ONLY && frame_size < st->Fs/100)
       st->mode = MODE_CELT_ONLY;
    if (st->lfe)
       st->mode = MODE_CELT_ONLY;

    // Check if below code is only for Hybrid / inband FEC
    if (st->prev_mode > 0 &&
        ((st->mode != MODE_CELT_ONLY && st->prev_mode == MODE_CELT_ONLY) ||
    (st->mode == MODE_CELT_ONLY && st->prev_mode != MODE_CELT_ONLY)))
    {
        celt_to_smpl = (st->mode != MODE_CELT_ONLY);
        if (!celt_to_smpl)
        {
            /* Switch to SMPL/hybrid if frame size is 10 ms or more*/
            if (frame_size >= st->Fs / 100)
            {
                st->mode = st->prev_mode;
                to_celt = 1;
            }
        }
    }

    /* When encoding multiframes, we can ask for a switch to CELT only in the last frame. This switch
     * is processed above as the requested mode shouldn't interrupt stereo->mono transition. */
    if (st->stream_channels == 1 && st->prev_channels ==2 && st->smpl_mode.toMono==0
          && st->mode != MODE_CELT_ONLY && st->prev_mode != MODE_CELT_ONLY)
    {
       /* Delay stereo->mono transition by two frames so that SILK can do a smooth downmix */
       st->smpl_mode.toMono = 1;
       st->stream_channels = 2;
    } else {
       st->smpl_mode.toMono = 0;
    }

    /* Update equivalent rate with mode decision. */
    equiv_rate = compute_equiv_rate(st->bitrate_bps, st->stream_channels, st->Fs/frame_size,
          st->use_vbr, st->mode, st->smpl_mode.complexity, st->smpl_mode.packetLossPercentage);

    if (st->mode != MODE_CELT_ONLY && st->prev_mode == MODE_CELT_ONLY)
    {
        smpl_EncControlStruct dummy;
        memcpy(&dummy, &st->smpl_mode, sizeof (smpl_EncControlStruct));
        smpl_InitEncoder( smpl_enc, &dummy);
        prefill=1;
    }

    /* Automatic (rate-dependent) bandwidth selection */
    if (st->mode == MODE_CELT_ONLY || st->first || st->smpl_mode.allowBandwidthSwitch)
    {
        const opus_int32 *voice_bandwidth_thresholds, *music_bandwidth_thresholds;
        opus_int32 bandwidth_thresholds[8];
        int bandwidth = OPUS_BANDWIDTH_FULLBAND;

        if (st->channels==2 && st->force_channels!=1)
        {
           voice_bandwidth_thresholds = stereo_voice_bandwidth_thresholds;
           music_bandwidth_thresholds = stereo_music_bandwidth_thresholds;
        } else {
           voice_bandwidth_thresholds = mono_voice_bandwidth_thresholds;
           music_bandwidth_thresholds = mono_music_bandwidth_thresholds;
        }
        /* Interpolate bandwidth thresholds depending on voice estimation */
        for (i=0;i<8;i++)
        {
           bandwidth_thresholds[i] = music_bandwidth_thresholds[i]
                    + ((voice_est*voice_est*(voice_bandwidth_thresholds[i]-music_bandwidth_thresholds[i]))>>14);
        }
        do {
            int threshold, hysteresis;
            threshold = bandwidth_thresholds[2*(bandwidth-OPUS_BANDWIDTH_MEDIUMBAND)];
            hysteresis = bandwidth_thresholds[2*(bandwidth-OPUS_BANDWIDTH_MEDIUMBAND)+1];
            if (!st->first)
            {
                if (st->auto_bandwidth >= bandwidth)
                    threshold -= hysteresis;
                else
                    threshold += hysteresis;
            }
            if (equiv_rate >= threshold)
                break;
        } while (--bandwidth>OPUS_BANDWIDTH_NARROWBAND);
        /* We don't use mediumband anymore, except when explicitly requested or during
           mode transitions. */
        if (bandwidth == OPUS_BANDWIDTH_MEDIUMBAND)
           bandwidth = OPUS_BANDWIDTH_WIDEBAND;
        st->bandwidth = st->auto_bandwidth = bandwidth;
    }

    if (st->bandwidth>st->max_bandwidth)
       st->bandwidth = st->max_bandwidth;

    if (st->user_bandwidth != OPUS_AUTO)
        st->bandwidth = st->user_bandwidth;

    /* This prevents us from using hybrid at unsafe CBR/max rates */
    if (st->mode != MODE_CELT_ONLY && max_rate < 15000)
    {
       st->bandwidth = IMIN(st->bandwidth, OPUS_BANDWIDTH_WIDEBAND);
    }

    /* Prevents Opus from wasting bits on frequencies that are above
       the Nyquist rate of the input signal */
    if (st->Fs <= 24000 && st->bandwidth > OPUS_BANDWIDTH_SUPERWIDEBAND)
        st->bandwidth = OPUS_BANDWIDTH_SUPERWIDEBAND;
    if (st->Fs <= 16000 && st->bandwidth > OPUS_BANDWIDTH_WIDEBAND)
        st->bandwidth = OPUS_BANDWIDTH_WIDEBAND;
    if (st->Fs <= 12000 && st->bandwidth > OPUS_BANDWIDTH_MEDIUMBAND)
        st->bandwidth = OPUS_BANDWIDTH_MEDIUMBAND;
    if (st->Fs <= 8000 && st->bandwidth > OPUS_BANDWIDTH_NARROWBAND)
        st->bandwidth = OPUS_BANDWIDTH_NARROWBAND;
#ifndef DISABLE_FLOAT_API
    /* Use detected bandwidth to reduce the encoded bandwidth. */
    if (st->detected_bandwidth && st->user_bandwidth == OPUS_AUTO)
    {
       int min_detected_bandwidth;
       /* Makes bandwidth detection more conservative just in case the detector
          gets it wrong when we could have coded a high bandwidth transparently.
          When operating in SILK/hybrid mode, we don't go below wideband to avoid
          more complicated switches that require redundancy. */
       if (equiv_rate <= 18000*st->stream_channels && st->mode == MODE_CELT_ONLY)
          min_detected_bandwidth = OPUS_BANDWIDTH_NARROWBAND;
       else if (equiv_rate <= 24000*st->stream_channels && st->mode == MODE_CELT_ONLY)
          min_detected_bandwidth = OPUS_BANDWIDTH_MEDIUMBAND;
       else if (equiv_rate <= 30000*st->stream_channels)
          min_detected_bandwidth = OPUS_BANDWIDTH_WIDEBAND;
       else if (equiv_rate <= 44000*st->stream_channels)
          min_detected_bandwidth = OPUS_BANDWIDTH_SUPERWIDEBAND;
       else
          min_detected_bandwidth = OPUS_BANDWIDTH_FULLBAND;

       st->detected_bandwidth = IMAX(st->detected_bandwidth, min_detected_bandwidth);
       st->bandwidth = IMIN(st->bandwidth, st->detected_bandwidth);
    }
#endif
    //st->smpl_mode.LBRR_coded = decide_fec(st->smpl_mode.useInBandFEC, st->smpl_mode.packetLossPercentage,
    //      st->smpl_mode.LBRR_coded, st->mode, &st->bandwidth, equiv_rate);
    celt_encoder_ctl(celt_enc, OPUS_SET_LSB_DEPTH(lsb_depth));

    /* CELT mode doesn't support mediumband, use wideband instead */
    if (st->mode == MODE_CELT_ONLY && st->bandwidth == OPUS_BANDWIDTH_MEDIUMBAND)
        st->bandwidth = OPUS_BANDWIDTH_WIDEBAND;
    if (st->lfe)
       st->bandwidth = OPUS_BANDWIDTH_NARROWBAND;

    curr_bandwidth = st->bandwidth;

    if (st->prev_mode > 0 &&
        ((st->mode != MODE_CELT_ONLY && st->prev_mode == MODE_CELT_ONLY) ||
            (st->mode == MODE_CELT_ONLY && st->prev_mode != MODE_CELT_ONLY)))
    {
        celt_to_smpl = (st->mode != MODE_CELT_ONLY);
        to_celt = (st->mode == MODE_CELT_ONLY);
    }

    // Supporting 10, 20, 60 and 120 ms packets for SMPL (checked at higher level), multi-frame is only for CELT
    if ((frame_size > st->Fs / 50) && (st->mode != MODE_SMPL_ONLY))
    {
       int enc_frame_size;
       int nb_frames;

       enc_frame_size = st->Fs/50;
       nb_frames = frame_size/enc_frame_size;

#ifndef DISABLE_FLOAT_API
       if (analysis_read_pos_bak!= -1)
       {
          st->analysis.read_pos = analysis_read_pos_bak;
          st->analysis.read_subframe = analysis_read_subframe_bak;
       }
#endif

       smpl_assert(st->mode == MODE_CELT_ONLY); // SMPL does not do multi-frame, but we need it for CELT
       gen_smpl_toc(&data[0], nb_frames, st->mode, st->Fs / (frame_size / nb_frames), curr_bandwidth, st->stream_channels);

       ret = encode_multiframe_packet(st, pcm, nb_frames, enc_frame_size, data,
                                      out_data_bytes, to_celt, lsb_depth, float_api);

       // Re-apply TOC as multi-frame may temper with the TOC byte
       gen_smpl_toc(&data[0], nb_frames, st->mode, st->Fs / (frame_size / nb_frames), curr_bandwidth, st->stream_channels);
       RESTORE_STACK;
       return ret;
    }

    /* For the first frame at a new SMPL bandwidth */
    if (st->silk_bw_switch)
    {
       celt_to_smpl = 1;
       st->silk_bw_switch = 0;
       /* Do a prefill without reseting the sampling rate control. */
       prefill=2;
    }

    unsigned char *smpl_toc_byte = &data[0];
    data += 1;

    ec_enc_init(&enc, data, max_data_bytes-1);

    ALLOC(pcm_buf, (total_buffer+frame_size)*st->channels, opus_val16);
    OPUS_COPY(pcm_buf, &st->delay_buffer[(st->encoder_buffer-total_buffer)*st->channels], total_buffer*st->channels);

    if (st->mode == MODE_CELT_ONLY)
       hp_freq_smth1 = silk_LSHIFT( silk_lin2log( VARIABLE_HP_MIN_CUTOFF_HZ ), 8 );
    else
       hp_freq_smth1 = 193731;

    st->variable_HP_smth2_Q15 = silk_SMLAWB( st->variable_HP_smth2_Q15,
          hp_freq_smth1 - st->variable_HP_smth2_Q15, SILK_FIX_CONST( VARIABLE_HP_SMTH_COEF2, 16 ) );

    /* convert from log scale to Hertz */
    cutoff_Hz = silk_log2lin( silk_RSHIFT( st->variable_HP_smth2_Q15, 8 ) );

    if (st->application != OPUS_APPLICATION_VOIP)
    {
        dc_reject(pcm, 3, &pcm_buf[total_buffer * st->channels], st->hp_mem, frame_size, st->channels, st->Fs);
    }
    else if (st->mode == MODE_CELT_ONLY) {
        hp_cutoff(pcm, cutoff_Hz, &pcm_buf[total_buffer * st->channels], st->hp_mem, frame_size, st->channels, st->Fs, st->arch);
    }
    else {
        memcpy(&pcm_buf[total_buffer * st->channels], pcm, frame_size * st->channels * sizeof(opus_val16));
    }

#ifndef FIXED_POINT
    if (float_api)
    {
       opus_val32 sum;
       sum = celt_inner_prod(&pcm_buf[total_buffer*st->channels], &pcm_buf[total_buffer*st->channels], frame_size*st->channels, st->arch);
       /* This should filter out both NaNs and ridiculous signals that could
          cause NaNs further down. */
       if (!(sum < 1e9f) || celt_isnan(sum))
       {
          OPUS_CLEAR(&pcm_buf[total_buffer*st->channels], frame_size*st->channels);
          st->hp_mem[0] = st->hp_mem[1] = st->hp_mem[2] = st->hp_mem[3] = 0;
       }
    }
#endif


    /* SMPL processing */
    HB_gain = Q15ONE;
    if (st->mode != MODE_CELT_ONLY)
    {
        //opus_int32 total_bitRate; //, celt_rate;
#ifdef FIXED_POINT
       const opus_int16 *pcm_smpl;
#else
       VARDECL(opus_int16, pcm_smpl);
       ALLOC(pcm_smpl, st->channels*frame_size, opus_int16);
#endif

        /* SMPL gets all bits */
        st->smpl_mode.bitRate = st->bitrate_bps;

        st->smpl_mode.payloadSize_ms = 1000 * frame_size / st->Fs;
        st->smpl_mode.nChannelsAPI = st->channels;
        st->smpl_mode.nChannelsInternal = st->stream_channels;
        st->smpl_mode.minInternalSampleRate = 16000;

        /* Max bits for SILK, counting ToC, redundancy bytes, and optionally redundancy. */
        st->smpl_mode.maxBits = (max_data_bytes-1)*8;

        if (prefill)
        {
            opus_int32 zero=0;
            int prefill_offset;
            /* Use a smooth onset for the SILK prefill to avoid the encoder trying to encode
               a discontinuity. The exact location is what we need to avoid leaving any "gap"
               in the audio when mixing with the redundant CELT frame. Here we can afford to
               overwrite st->delay_buffer because the only thing that uses it before it gets
               rewritten is tmp_prefill[] and even then only the part after the ramp really
               gets used (rather than sent to the encoder and discarded) */
            prefill_offset = st->channels*(st->encoder_buffer-st->delay_compensation-st->Fs/400);
            gain_fade(st->delay_buffer+prefill_offset, st->delay_buffer+prefill_offset,
                  0, Q15ONE, celt_mode->overlap, st->Fs/400, st->channels, celt_mode->window, st->Fs);
            OPUS_CLEAR(st->delay_buffer, prefill_offset);
#ifdef FIXED_POINT
            pcm_smpl = st->delay_buffer;
#else
            for (i=0;i<st->encoder_buffer*st->channels;i++)
                pcm_smpl[i] = FLOAT2INT16(st->delay_buffer[i]);
#endif
            smpl_Encode( smpl_enc, &st->smpl_mode, pcm_smpl, st->encoder_buffer, NULL, NULL, &zero, prefill, activity );
            /* Prevent a second switch in the real encode call. */
            st->smpl_mode.opusCanSwitch = 0;
        }

#ifdef FIXED_POINT
        pcm_smpl = pcm_buf+total_buffer*st->channels;
#else
        for (i=0;i<frame_size*st->channels;i++)
            pcm_smpl[i] = FLOAT2INT16(pcm_buf[total_buffer*st->channels + i]);
#endif
        ret = smpl_Encode( smpl_enc, &st->smpl_mode, pcm_smpl, frame_size, &enc, smpl_toc_byte, &nBytes, 0, activity );
        st->smpl_mode.allowBandwidthSwitch = 1;
        st->smpl_mode.opusCanSwitch = 1;
        if( ret ) {
            /*fprintf (stderr, "SILK encode error: %d\n", ret);*/
            /* Handle error */
           RESTORE_STACK;
           return OPUS_INTERNAL_ERROR;
        }

        /* Extract SILK internal bandwidth for signaling in first byte */
        if( st->mode == MODE_SMPL_ONLY ) {
            if( st->smpl_mode.internalSampleRate == 8000 ) {
               curr_bandwidth = OPUS_BANDWIDTH_NARROWBAND;
            } else if( st->smpl_mode.internalSampleRate == 12000 ) {
               curr_bandwidth = OPUS_BANDWIDTH_MEDIUMBAND;
            } else if( st->smpl_mode.internalSampleRate == 16000 ) {
               curr_bandwidth = OPUS_BANDWIDTH_WIDEBAND;
            }
        } else {
            smpl_assert( st->smpl_mode.internalSampleRate == 16000 );
        }

        st->smpl_mode.opusCanSwitch = st->smpl_mode.switchReady && !st->nonfinal_frame;

        if (nBytes==0)
        {
           st->rangeFinal = 0;
           gen_smpl_toc(&data[-1], 1, st->mode, st->Fs/frame_size, curr_bandwidth, st->stream_channels);
           RESTORE_STACK;
           return 1;
        }
    }

    /* CELT processing */
    {
        int endband=21;

        switch(curr_bandwidth)
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
        }
        celt_encoder_ctl(celt_enc, CELT_SET_END_BAND(endband));
        celt_encoder_ctl(celt_enc, CELT_SET_CHANNELS(st->stream_channels));
    }
    celt_encoder_ctl(celt_enc, OPUS_SET_BITRATE(OPUS_BITRATE_MAX));
    if (st->mode != MODE_SMPL_ONLY)
    {
        opus_val32 celt_pred=2;
        celt_encoder_ctl(celt_enc, OPUS_SET_VBR(0));
        /* We may still decide to disable prediction later */
        if (st->smpl_mode.reducedDependency)
           celt_pred = 0;
        celt_encoder_ctl(celt_enc, CELT_SET_PREDICTION(celt_pred));

        if (st->mode == MODE_HYBRID)
        {
            if( st->use_vbr ) {
                celt_encoder_ctl(celt_enc, OPUS_SET_BITRATE(st->bitrate_bps-st->smpl_mode.bitRate));
                celt_encoder_ctl(celt_enc, OPUS_SET_VBR_CONSTRAINT(0));
            }
        } else {
            if (st->use_vbr)
            {
                celt_encoder_ctl(celt_enc, OPUS_SET_VBR(1));
                celt_encoder_ctl(celt_enc, OPUS_SET_VBR_CONSTRAINT(st->vbr_constraint));
                celt_encoder_ctl(celt_enc, OPUS_SET_BITRATE(st->bitrate_bps));
            }
        }
    }

    ALLOC(tmp_prefill, st->channels*st->Fs/400, opus_val16);
    if (st->mode != MODE_SMPL_ONLY && st->mode != st->prev_mode && st->prev_mode > 0)
    {
       OPUS_COPY(tmp_prefill, &st->delay_buffer[(st->encoder_buffer-total_buffer-st->Fs/400)*st->channels], st->channels*st->Fs/400);
    }

    if (st->channels*(st->encoder_buffer-(frame_size+total_buffer)) > 0)
    {
       OPUS_MOVE(st->delay_buffer, &st->delay_buffer[st->channels*frame_size], st->channels*(st->encoder_buffer-frame_size-total_buffer));
       OPUS_COPY(&st->delay_buffer[st->channels*(st->encoder_buffer-frame_size-total_buffer)],
             &pcm_buf[0],
             (frame_size+total_buffer)*st->channels);
    } else {
       OPUS_COPY(st->delay_buffer, &pcm_buf[(frame_size+total_buffer-st->encoder_buffer)*st->channels], st->encoder_buffer*st->channels);
    }
    /* gain_fade() and stereo_fade() need to be after the buffer copying
       because we don't want any of this to affect the SILK part */
    if( st->prev_HB_gain < Q15ONE || HB_gain < Q15ONE ) {
       gain_fade(pcm_buf, pcm_buf,
             st->prev_HB_gain, HB_gain, celt_mode->overlap, frame_size, st->channels, celt_mode->window, st->Fs);
    }
    st->prev_HB_gain = HB_gain;
    if (st->mode != MODE_HYBRID || st->stream_channels==1)
    {
       if (equiv_rate > 32000)
          st->smpl_mode.stereoWidth_Q14 = 16384;
       else if (equiv_rate < 16000)
          st->smpl_mode.stereoWidth_Q14 = 0;
       else
          st->smpl_mode.stereoWidth_Q14 = 16384 - 2048*(opus_int32)(32000-equiv_rate)/(equiv_rate-14000);
    }
    if( st->channels == 2 ) {
        /* Apply stereo width reduction (at low bitrates) */
        if( st->hybrid_stereo_width_Q14 < (1 << 14) || st->smpl_mode.stereoWidth_Q14 < (1 << 14) ) {
            opus_val16 g1, g2;
            g1 = st->hybrid_stereo_width_Q14;
            g2 = (opus_val16)(st->smpl_mode.stereoWidth_Q14);
#ifdef FIXED_POINT
            g1 = g1==16384 ? Q15ONE : SHL16(g1,1);
            g2 = g2==16384 ? Q15ONE : SHL16(g2,1);
#else
            g1 *= (1.f/16384);
            g2 *= (1.f/16384);
#endif
            stereo_fade(pcm_buf, pcm_buf, g1, g2, celt_mode->overlap,
                  frame_size, st->channels, celt_mode->window, st->Fs);
            st->hybrid_stereo_width_Q14 = st->smpl_mode.stereoWidth_Q14;
        }
    }

    if (st->mode != MODE_CELT_ONLY)start_band=17;

    if (st->mode == MODE_SMPL_ONLY)
    {
        ret = (ec_tell(&enc)+7)>>3;
        ec_enc_done(&enc);
        nb_compr_bytes = ret;
    } else {
       nb_compr_bytes = (max_data_bytes - 1);
       ec_enc_shrink(&enc, nb_compr_bytes);
    }

#ifndef DISABLE_FLOAT_API
    if (st->mode != MODE_SMPL_ONLY)
       celt_encoder_ctl(celt_enc, CELT_SET_ANALYSIS(&analysis_info));
#endif
    if (st->mode == MODE_HYBRID) {
       SILKInfo info;
       info.signalType = st->smpl_mode.signalType;
       info.offset = st->smpl_mode.offset;
       celt_encoder_ctl(celt_enc, CELT_SET_SILK_INFO(&info));
    }

    celt_encoder_ctl(celt_enc, CELT_SET_START_BAND(start_band));

    if (st->mode != MODE_SMPL_ONLY)
    {
        if (st->mode != st->prev_mode && st->prev_mode > 0)
        {
           unsigned char dummy[2];
           celt_encoder_ctl(celt_enc, OPUS_RESET_STATE);

           /* Prefilling */
           celt_encode_with_ec(celt_enc, tmp_prefill, st->Fs/400, dummy, 2, NULL);
           celt_encoder_ctl(celt_enc, CELT_SET_PREDICTION(0));
        }
        /* If false, we already busted the budget and we'll end up with a "PLC frame" */
        if (ec_tell(&enc) <= 8*nb_compr_bytes)
        {
           /* Set the bitrate again if it was overridden in the redundancy code above*/
           celt_encoder_ctl(celt_enc, OPUS_SET_VBR(st->use_vbr));
           ret = celt_encode_with_ec(celt_enc, pcm_buf, frame_size, NULL, nb_compr_bytes, &enc);
           if (ret < 0)
           {
              RESTORE_STACK;
              return OPUS_INTERNAL_ERROR;
           }
        }
    }

    /* Signalling the mode in the first byte */
    data--;
    gen_smpl_toc(&data[0], 1, st->mode, st->Fs/frame_size, curr_bandwidth, st->stream_channels);

    st->rangeFinal = enc.rng;//^ redundant_rng;

    if (to_celt)
        st->prev_mode = MODE_CELT_ONLY;
    else
        st->prev_mode = st->mode;
    st->prev_channels = st->stream_channels;
    st->prev_framesize = frame_size;

    st->first = 0;

    /* In the unlikely case that the SMPL encoder busted its target, tell
       the decoder to call the PLC */
    if (ec_tell(&enc) > (max_data_bytes-1)*8)
    {
       if (max_data_bytes < 2)
       {
          RESTORE_STACK;
          return OPUS_BUFFER_TOO_SMALL;
       }
       data[1] = 0;
       ret = 1;
       st->rangeFinal = 0;
    } else if (st->mode==MODE_SMPL_ONLY)
    {
       /*When in LPC only mode it's perfectly
         reasonable to strip off trailing zero bytes as
         the required range decoder behavior is to
         fill these in. This can't be done when the MDCT
         modes are used because the decoder needs to know
         the actual length for allocation purposes.*/
       while(ret>2&&data[ret]==0)ret--;
    }
    /* Count ToC */
    ret += 1;
    if (!st->use_vbr)
    {
       if (opus_packet_pad(data, ret, max_data_bytes) != OPUS_OK)
       {
          RESTORE_STACK;
          return OPUS_INTERNAL_ERROR;
       }
       ret = max_data_bytes;
    }
    RESTORE_STACK;
    return ret;
}

#endif // #if defined(ENABLE_SMPL)
