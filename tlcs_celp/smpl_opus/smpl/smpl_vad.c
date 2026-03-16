#include "celt/stack_alloc.h"
#include "silk/define.h"
#include "smpl_structs.h"
#include "smpl_vad.h"
#include "smpl_typedef.h"
#include "smpl_errors.h"
#include "silk/SigProc_FIX.h"

/* First order ARMA filter with zero at DC */
void smpl_filt_hp_FIX(
    const opus_int16            *in,                /* I     input signal                                               */
    const opus_int32            B_Q16,              /* I     Gain coefficient                                           */
    const opus_int32            A_neg_Q16,          /* I     AR coefficient                                             */
    opus_int32                  *S,                 /* I/O   State vector [1]                                           */
    opus_int16                  *out,               /* O     output signal                                              */
    const opus_int32            len                 /* I     signal length (must be even)                               */
)
{
    /* DIRECT FORM II TRANSPOSED (uses 1 element state vector) */
    for( int k = 0; k < len; k++ ) {
        int32_t inval = silk_SMULWB(B_Q16, in[ k ]);
        int16_t outval = (opus_int16)silk_SAT16(silk_SUB32(S[0], inval));
        S[0] = silk_SMLAWB(inval, A_neg_Q16, outval);
        out[ k ] = outval;
    }
}

/* Coefficients for 2-band filter bank based on first-order allpass filters */
static opus_int16 A_fb1_20 = 3894 << 1;
static opus_int16 A_fb1_21 = -29322; /* (opus_int16)(18107 << 1) */

/* Split signal into two decimated bands using first-order allpass filters */
void smpl_ana_filt_bank_1(
    const opus_int16            *in,                /* I    Input signal [N]                                            */
    opus_int32                  *S,                 /* I/O  State vector [2]                                            */
    opus_int16                  *outL,              /* O    Low band [N/2]                                              */
    opus_int16                  *outH,              /* O    High band [N/2]                                             */
    const opus_int32            N                   /* I    Number of input samples                                     */
)
{
    opus_int      k, N2 = silk_RSHIFT( N, 1 );
    opus_int32    in32, X, Y, out_1, out_2;

    /* Internal variables and state are in Q10 format */
    for( k = 0; k < N2; k++ ) {
        /* Convert to Q10 */
        in32 = silk_LSHIFT( (opus_int32)in[ 2 * k ], 10 );

        /* All-pass section for even input sample */
        Y      = silk_SUB32( in32, S[ 0 ] );
        X      = silk_SMLAWB( Y, Y, A_fb1_21 );
        out_1  = silk_ADD32( S[ 0 ], X );
        S[ 0 ] = silk_ADD32( in32, X );

        /* Convert to Q10 */
        in32 = silk_LSHIFT( (opus_int32)in[ 2 * k + 1 ], 10 );

        /* All-pass section for odd input sample, and add to output of previous section */
        Y      = silk_SUB32( in32, S[ 1 ] );
        X      = silk_SMULWB( Y, A_fb1_20 );
        out_2  = silk_ADD32( S[ 1 ], X );
        S[ 1 ] = silk_ADD32( in32, X );

        /* Add/subtract, convert back to int16 and store to output */
        outL[ k ] = (opus_int16)silk_SAT16( silk_RSHIFT_ROUND( silk_ADD32( out_2, out_1 ), 11 ) );
        outH[ k ] = (opus_int16)silk_SAT16( silk_RSHIFT_ROUND( silk_SUB32( out_2, out_1 ), 11 ) );
    }
}

opus_int smpl_VAD_Init(                                         /* O    Return value, 0 if success                  */
    smpl_VAD_state              *psSilk_VAD                     /* I/O  Pointer to Silk VAD state                   */
)
{
    opus_int b, ret = 0;

    /* reset state memory */
    silk_memset( psSilk_VAD, 0, sizeof( smpl_VAD_state ) );

    /* init noise levels */
    /* Initialize array with approx pink noise levels (psd proportional to inverse of frequency) */
    for( b = 0; b < VAD_N_BANDS; b++ ) {
        psSilk_VAD->NoiseLevelBias[ b ] = silk_max_32( silk_DIV32_16( VAD_NOISE_LEVELS_BIAS, b + 1 ), 1 );
    }

    /* Initialize state */
    for( b = 0; b < VAD_N_BANDS; b++ ) {
        psSilk_VAD->NL[ b ]     = silk_MUL( 100, psSilk_VAD->NoiseLevelBias[ b ] );
        psSilk_VAD->inv_NL[ b ] = silk_DIV32( silk_int32_MAX, psSilk_VAD->NL[ b ] );
    }
    psSilk_VAD->counter = 15;

    /* init smoothed energy-to-noise ratio*/
    // for( b = 0; b < VAD_N_BANDS; b++ ) {
    //     psSilk_VAD->NrgRatioSmth_Q8[ b ] = 100 * 256;       /* 100 * 256 --> 20 dB SNR */
    // }

    return( ret );
}

/* Silk VAD noise level estimation */
void smpl_VAD_GetNoiseLevels(
    const opus_int32             pX[ SMPL_VAD_N_BANDS ], /* I    subband energies                            */
    smpl_VAD_state              *psSilk_VAD         /* I/O  Pointer to Silk VAD state                   */
);

/* Weighting factors for tilt measure */
static const opus_int32 tiltWeights[ SMPL_VAD_N_BANDS ] = { 30000, 6000, -12000, -12000 };

/***************************************/
/* Get the speech activity level in Q8 */
/***************************************/
opus_int smpl_VAD_GetSA_Q8_c(                                   /* O    Return value, 0 if success                  */
    smpl_encoder                *psEncC,                        /* I/O  Encoder state                               */
    const opus_int16            pIn[],                          /* I    PCM input                                   */
    const opus_int              framelen                        /* I    Number of PCM samples                       */
)
{
    opus_int   SA_Q15, pSNR_dB_Q7, input_tilt;
    opus_int   decimated_framelength1, decimated_framelength2;
    opus_int   decimated_framelength;
    opus_int   dec_subframe_length, dec_subframe_offset, SNR_Q7, i, b, s;
    opus_int32 sumSquared;
    VARDECL( opus_int16, X );
    opus_int32 Xnrg[ SMPL_VAD_N_BANDS ];
    opus_int32 NrgToNoiseRatio_Q8[ SMPL_VAD_N_BANDS ];
    opus_int32 speech_nrg, x_tmp;
    opus_int   X_offset[ SMPL_VAD_N_BANDS ];
    opus_int   ret = 0;
    smpl_VAD_state *psSilk_VAD = &psEncC->sVAD;
    SAVE_STACK;

    /* Safety checks */
    smpl_assert( SMPL_VAD_N_BANDS == 4 );
    smpl_assert( MAX_FRAME_LENGTH >= framelen );
    smpl_assert( framelen <= 512 );
    smpl_assert( framelen == 8 * silk_RSHIFT( framelen, 3 ) );

    /***********************/
    /* Filter and Decimate */
    /***********************/
    decimated_framelength1 = silk_RSHIFT( framelen, 1 );
    decimated_framelength2 = silk_RSHIFT( framelen, 2 );
    decimated_framelength = silk_RSHIFT( framelen, 3 );
    /* Decimate into 4 bands:
       0       L      3L       L              3L                             5L
               -      --       -              --                             --
               8       8       2               4                              4

       [0-1 kHz| temp. |1-2 kHz|    2-4 kHz    |            4-8 kHz           |

       They're arranged to allow the minimal ( frame_length / 4 ) extra
       scratch space during the downsampling process */
    X_offset[ 0 ] = 0;
    X_offset[ 1 ] = decimated_framelength + decimated_framelength2;
    X_offset[ 2 ] = X_offset[ 1 ] + decimated_framelength;
    X_offset[ 3 ] = X_offset[ 2 ] + decimated_framelength2;
    ALLOC( X, X_offset[ 3 ] + decimated_framelength1, opus_int16 );

    /* 0-8 kHz to 0-4 kHz and 4-8 kHz */
    smpl_ana_filt_bank_1( pIn, &psSilk_VAD->AnaState[  0 ],
        X, &X[ X_offset[ 3 ] ], framelen );

    /* 0-4 kHz to 0-2 kHz and 2-4 kHz */
    smpl_ana_filt_bank_1( X, &psSilk_VAD->AnaState1[ 0 ],
        X, &X[ X_offset[ 2 ] ], decimated_framelength1 );

    /* 0-2 kHz to 0-1 kHz and 1-2 kHz */
    smpl_ana_filt_bank_1( X, &psSilk_VAD->AnaState2[ 0 ],
        X, &X[ X_offset[ 1 ] ], decimated_framelength2 );

    /*******************************************/
    /* HP filter on lowest band, -3 dB @ 66 Hz */
    /*******************************************/
    // freqz(0.91 * [1, -1], [1, -0.81], 2^12, 2000); axis([0, 500, -10, 1])
    opus_int32 a_neg_q16 = 53084;
    a_neg_q16 = (a_neg_q16 * (100 - psSilk_VAD->highpass_sharpness)) / 100;
    opus_int32 b_q16     = (65536 + a_neg_q16) / 2;
    smpl_filt_hp_FIX(X, b_q16, a_neg_q16, psSilk_VAD->HPstate, X, decimated_framelength);

    /*************************************/
    /* Calculate the energy in each band */
    /*************************************/
    for( b = 0; b < SMPL_VAD_N_BANDS; b++ ) {
        /* Find the decimated framelength in the non-uniformly divided bands */
        decimated_framelength = silk_RSHIFT( framelen, silk_min_int( SMPL_VAD_N_BANDS - b, SMPL_VAD_N_BANDS - 1 ) );

        /* Split length into subframe lengths */
        dec_subframe_length = silk_RSHIFT( decimated_framelength, VAD_INTERNAL_SUBFRAMES_LOG2 );
        dec_subframe_offset = 0;

        /* Compute energy per sub-frame */
        /* initialize with summed energy of last subframe */
        Xnrg[ b ] = psSilk_VAD->XnrgSubfr[ b ];
        for( s = 0; s < VAD_INTERNAL_SUBFRAMES; s++ ) {
            sumSquared = 0;
            for( i = 0; i < dec_subframe_length; i++ ) {
                /* The energy will be less than dec_subframe_length * ( silk_int16_MIN / 8 ) ^ 2.            */
                /* Therefore we can accumulate with no risk of overflow (unless dec_subframe_length > 128)  */
                x_tmp = silk_RSHIFT(
                    X[ X_offset[ b ] + i + dec_subframe_offset ], 3 );
                sumSquared = silk_SMLABB( sumSquared, x_tmp, x_tmp );

                /* Safety check */
                silk_assert( sumSquared >= 0 );
            }

            /* Add/saturate summed energy of current subframe */
            if( s < VAD_INTERNAL_SUBFRAMES - 1 ) {
                Xnrg[ b ] = silk_ADD_POS_SAT32( Xnrg[ b ], sumSquared );
            } else {
                /* Look-ahead subframe */
                Xnrg[ b ] = silk_ADD_POS_SAT32( Xnrg[ b ], silk_RSHIFT( sumSquared, 1 ) );
            }

            dec_subframe_offset += dec_subframe_length;
        }
        psSilk_VAD->XnrgSubfr[ b ] = sumSquared;
    }

    /********************/
    /* Noise estimation */
    /********************/
    smpl_VAD_GetNoiseLevels( &Xnrg[ 0 ], psSilk_VAD );

    /***********************************************/
    /* Signal-plus-noise to noise ratio estimation */
    /***********************************************/
    sumSquared = 0;
    input_tilt = 0;
    for( b = 0; b < SMPL_VAD_N_BANDS; b++ ) {
        speech_nrg = Xnrg[ b ] - psSilk_VAD->NL[ b ];
        if( speech_nrg > 0 ) {
            /* Divide, with sufficient resolution */
            if( ( Xnrg[ b ] & 0xFF800000 ) == 0 ) {
                NrgToNoiseRatio_Q8[ b ] = silk_DIV32( silk_LSHIFT( Xnrg[ b ], 8 ), psSilk_VAD->NL[ b ] + 1 );
            } else {
                NrgToNoiseRatio_Q8[ b ] = silk_DIV32( Xnrg[ b ], silk_RSHIFT( psSilk_VAD->NL[ b ], 8 ) + 1 );
            }

            /* Convert to log domain */
            SNR_Q7 = silk_lin2log( NrgToNoiseRatio_Q8[ b ] ) - 8 * 128;

            /* Sum-of-squares */
            sumSquared = silk_SMLABB( sumSquared, SNR_Q7, SNR_Q7 );          /* Q14 */

            /* Tilt measure */
            if( speech_nrg < ( (opus_int32)1 << 20 ) ) {
                /* Scale down SNR value for small subband speech energies */
                SNR_Q7 = silk_SMULWB( silk_LSHIFT( silk_SQRT_APPROX( speech_nrg ), 6 ), SNR_Q7 );
            }
            input_tilt = silk_SMLAWB( input_tilt, tiltWeights[ b ], SNR_Q7 );
        } else {
            NrgToNoiseRatio_Q8[ b ] = 256;
        }
    }

    /* Mean-of-squares */
    sumSquared = silk_DIV32_16( sumSquared, SMPL_VAD_N_BANDS ); /* Q14 */

    /* Root-mean-square approximation, scale to dBs, and write to output pointer */
    pSNR_dB_Q7 = (opus_int16)( 3 * silk_SQRT_APPROX( sumSquared ) ); /* Q7 */

    /*********************************/
    /* Speech Probability Estimation */
    /*********************************/
    opus_int32 vad_snr_factor_Q16 = (VAD_SNR_FACTOR_Q16 * (150 - psSilk_VAD->non_binariness)) / 150;
    SA_Q15 = silk_sigm_Q15( silk_SMULWB( vad_snr_factor_Q16, pSNR_dB_Q7 ) - VAD_NEGATIVE_OFFSET_Q5 );

    /**************************/
    /* Frequency Tilt Measure */
    /**************************/
    psEncC->input_tilt_Q15 = silk_LSHIFT( silk_sigm_Q15( input_tilt ) - 16384, 1 );

    /**************************************************/
    /* Scale the sigmoid output based on power levels */
    /**************************************************/
    // speech_nrg = 0;
    // for( b = 0; b < SMPL_VAD_N_BANDS; b++ ) {
    //     /* Accumulate signal-without-noise energies, higher frequency bands have more weight */
    //     speech_nrg += ( b + 1 ) * silk_RSHIFT( Xnrg[ b ] - psSilk_VAD->NL[ b ], 4 );
    // }

    // if( framelen == 20 * fs_kHz ) {
    //     speech_nrg = silk_RSHIFT32( speech_nrg, 1 );
    // }
    // /* Power scaling */
    // if( speech_nrg <= 0 ) {
    //     SA_Q15 = silk_RSHIFT( SA_Q15, 1 );
    // } else if( speech_nrg < 16384 ) {
    //     speech_nrg = silk_LSHIFT32( speech_nrg, 16 );

    //     /* square-root */
    //     speech_nrg = silk_SQRT_APPROX( speech_nrg );
    //     SA_Q15 = silk_SMULWB( 32768 + speech_nrg, SA_Q15 );
    // }

    /* Copy the resulting speech activity in Q8 */
    psEncC->speech_activity_Q8 = silk_min_int( silk_RSHIFT( SA_Q15, 7 ), silk_uint8_MAX );

    /***********************************/
    /* Energy Level and SNR estimation */
    /***********************************/
    /* Smoothing coefficient */
    // smooth_coef_Q16 = silk_SMULWB( VAD_SNR_SMOOTH_COEF_Q18, silk_SMULWB( (opus_int32)SA_Q15, SA_Q15 ) );

    // if( framelen == 10 * fs_kHz ) {
    //     smooth_coef_Q16 >>= 1;
    // }

    // for( b = 0; b < SMPL_VAD_N_BANDS; b++ ) {
        /* compute smoothed energy-to-noise ratio per band */
        // psSilk_VAD->NrgRatioSmth_Q8[ b ] = silk_SMLAWB( psSilk_VAD->NrgRatioSmth_Q8[ b ],
        //     NrgToNoiseRatio_Q8[ b ] - psSilk_VAD->NrgRatioSmth_Q8[ b ], smooth_coef_Q16 );

        // /* signal to noise ratio in dB per band */
        // SNR_Q7 = 3 * ( silk_lin2log( psSilk_VAD->NrgRatioSmth_Q8[b] ) - 8 * 128 );
        // /* quality = sigmoid( 0.25 * ( SNR_dB - 16 ) ); */
        // psEncC->input_quality_bands_Q15[ b ] = silk_sigm_Q15( silk_RSHIFT( SNR_Q7 - 16 * 128, 4 ) );
    // }

    RESTORE_STACK;
    return( ret );
}

/**************************/
/* Noise level estimation */
/**************************/
void smpl_VAD_GetNoiseLevels(
    const opus_int32            pX[ SMPL_VAD_N_BANDS ],  /* I    subband energies                            */
    smpl_VAD_state              *psSilk_VAD              /* I/O  Pointer to Silk VAD state                   */
)
{
    opus_int   k;
    opus_int32 nl, nrg, inv_nrg;
    opus_int   coef, min_coef;

    /* Initially faster smoothing */
    if( psSilk_VAD->counter < 1000 ) { /* 1000 = 20 sec */
        min_coef = silk_DIV32_16( silk_int16_MAX, silk_RSHIFT( psSilk_VAD->counter, 4 ) + 1 );
        /* Increment frame counter */
        psSilk_VAD->counter++;
    } else {
        min_coef = 0;
    }

    for( k = 0; k < SMPL_VAD_N_BANDS; k++ ) {
        /* Get old noise level estimate for current band */
        nl = psSilk_VAD->NL[ k ];
        silk_assert( nl >= 0 );

        /* Add bias */
        nrg = silk_ADD_POS_SAT32( pX[ k ], psSilk_VAD->NoiseLevelBias[ k ] );
        silk_assert( nrg > 0 );

        /* Invert energies */
        inv_nrg = silk_DIV32( silk_int32_MAX, nrg );
        silk_assert( inv_nrg >= 0 );

        /* Less update when subband energy is high */
        if( nrg > silk_LSHIFT( nl, 3 ) ) {
            coef = VAD_NOISE_LEVEL_SMOOTH_COEF_Q16 >> 3;
        } else if( nrg < nl ) {
            coef = VAD_NOISE_LEVEL_SMOOTH_COEF_Q16;
        } else {
            coef = silk_SMULWB( silk_SMULWW( inv_nrg, nl ), VAD_NOISE_LEVEL_SMOOTH_COEF_Q16 << 1 );
        }
        coef = (coef * (100 + psSilk_VAD->noise_lvl_update_speed)) / 100;

        /* Initially faster smoothing */
        coef = silk_max_int( coef, min_coef );

        /* Smooth inverse energies */
        psSilk_VAD->inv_NL[ k ] = silk_SMLAWB( psSilk_VAD->inv_NL[ k ], inv_nrg - psSilk_VAD->inv_NL[ k ], coef );
        silk_assert( psSilk_VAD->inv_NL[ k ] >= 0 );

        /* Compute noise level by inverting again */
        nl = silk_DIV32( silk_int32_MAX, psSilk_VAD->inv_NL[ k ] );
        silk_assert( nl >= 0 );

        /* Limit noise levels (guarantee 7 bits of head room) */
        nl = silk_min( nl, 0x00FFFFFF );

        /* Store as part of state */
        psSilk_VAD->NL[ k ] = nl;
    }
}

void update_vad_dtx_status(
    smpl_encoder *enc,
    smpl_EncControlStruct* enc_status,
    smpl_vad_status *vad,
    const int fs,
    const opus_int16 *samplesIn,
    const int nSamplesIn,
    const int framelen,
    const int frames_per_packet,
    const int activity)
{
    int packet_ms = (framelen * frames_per_packet * 1000) / fs;
    const opus_int16 *samplesPtr;
    smpl_assert(packet_ms == 10 || packet_ms == 20 || packet_ms == 60 || packet_ms == 120);
    smpl_assert(nSamplesIn == framelen*frames_per_packet);
    vad->frames_per_packet = frames_per_packet;
    vad->VAD = SMPL_FALSE;
    vad->coded_as_active_voice = SMPL_FALSE;
    int fs_vad = fs > 16000 ? 16000 : fs;
    OPUS_UNUSED int res = 0;
    int vad_framelen;

    if (fs > fs_vad) {
        // Reinitialize resampler if needed
        if ((enc->resampler_api_to_vad.Fs_out_kHz * 1000 != fs_vad) ||
            (enc->resampler_api_to_vad.Fs_in_kHz * 1000 != fs)) {
            res += silk_resampler_init(&(enc->resampler_api_to_vad), fs, fs_vad, 1);
            smpl_assert(res == SMPL_NO_ERROR);
        }
    }

    for (int i = 0; i < frames_per_packet; i++) {
        int t_frame = i * framelen;
        // SILK vad does not support the higher fs - need to resample if not satisfied
        opus_int16 x_16b_resampled[SMPL_MAX_WB_FRAME_SIZE_IN_SAMPLES];
        if (fs > fs_vad) {
            res += silk_resampler(&(enc->resampler_api_to_vad), x_16b_resampled, samplesIn + t_frame, framelen);
            smpl_assert(res == SMPL_NO_ERROR);
            vad_framelen = (framelen * 16000) / fs;
            samplesPtr = x_16b_resampled;
        }
        else {
            samplesPtr = samplesIn + t_frame;
            vad_framelen = framelen;
        }

        // TEMP for A/B testing
        smpl_VAD_state *psSilk_VAD = &enc->sVAD;
        psSilk_VAD->noise_lvl_update_speed = SMPL_min(SMPL_max(enc_status->vad_noise_lvl_update_speed, 0), 100);
        psSilk_VAD->non_binariness         = SMPL_min(SMPL_max(enc_status->vad_non_binariness,         0), 100);
        psSilk_VAD->highpass_sharpness     = SMPL_min(SMPL_max(enc_status->vad_highpass_sharpness,     0), 100);

        smpl_VAD_GetSA_Q8_c(enc, samplesPtr, vad_framelen);
        vad->vad_results[i] = enc->speech_activity_Q8 / 256.0f;
        // Use SILK vad if Opus vad is not running
        if (activity == SMPL_OPUS_VAD_NO_DECISION)
            vad->vad_results_type[i] = vad->vad_results[i] > SMPL_SPEECH_ACTIVITY_DTX_THRES ? ACTIVE : INACTIVE;
        else {
            const opus_int activity_threshold = SILK_FIX_CONST(SMPL_SPEECH_ACTIVITY_DTX_THRES, 8);
            if (activity == VAD_NO_ACTIVITY && enc->speech_activity_Q8 >= activity_threshold) {
                enc->speech_activity_Q8 = activity_threshold - 1;
                vad->vad_results[i] = enc->speech_activity_Q8 / 256.0f;
            }
            vad->vad_results_type[i] = vad->vad_results[i] > SMPL_SPEECH_ACTIVITY_DTX_THRES ? ACTIVE : INACTIVE;
        }
    }

    for (int i=0; i<frames_per_packet; i++) {
        if (vad->vad_results_type[i] == ACTIVE) {
            enc->dtx_state.remaining_dtx_hangover = enc->dtx_state.hangover_ms;
        } else {
            if (enc->dtx_state.remaining_dtx_hangover > 0) {
                vad->vad_results_type[i] = HANGOVER;
                enc->dtx_state.remaining_dtx_hangover -= packet_ms/frames_per_packet;
            }
        }
        if (vad->vad_results_type[i] == ACTIVE) {
            vad->VAD = SMPL_TRUE;
        }
        if (vad->vad_results_type[i] != INACTIVE) {
            vad->coded_as_active_voice = SMPL_TRUE;
        }
    }

    enc->dtx_state.sid_frame = enc_status->useDTX && !vad->coded_as_active_voice;
    if (enc->dtx_state.sid_frame) {
        enc->dtx_state.send_sid_frame = SMPL_TRUE;
        if (enc->dtx_state.sid_interval_ms > packet_ms) {
            if (enc->dtx_state.dtx_remaining_ms >= packet_ms) {
                enc->dtx_state.send_sid_frame = SMPL_FALSE;
                enc->dtx_state.dtx_remaining_ms -= packet_ms;
            } else {
                enc->dtx_state.send_sid_frame = SMPL_TRUE;
                enc->dtx_state.dtx_remaining_ms = enc->dtx_state.sid_interval_ms;
            }
        }
    } else {
       enc->dtx_state.dtx_remaining_ms = 0;
    }
}
