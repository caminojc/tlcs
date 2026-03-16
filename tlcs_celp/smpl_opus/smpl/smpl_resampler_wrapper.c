#include "smpl_resampler_wrapper.h"
#include "silk/SigProc_FIX.h"
#include "silk/resampler_private.h"
#include <stdlib.h>

void* smpl_silk_resampler_wrapper_create(opus_int32 Fs_Hz_in, opus_int32 Fs_Hz_out) {
    silk_resampler_state_struct* S = (silk_resampler_state_struct*)malloc(sizeof(silk_resampler_state_struct));
    memset(S, 0, sizeof(silk_resampler_state_struct));

    S->inputDelay = 0;
    S->Fs_in_kHz  = silk_DIV32_16(Fs_Hz_in,  1000);
    S->Fs_out_kHz = silk_DIV32_16(Fs_Hz_out, 1000);
    S->batchSize = S->Fs_in_kHz * RESAMPLER_MAX_BATCH_SIZE_MS;

    // state initialization from Opus source
    opus_int up2x = 0;
    if (Fs_Hz_out > Fs_Hz_in) {
        /* Upsample */
        if( Fs_Hz_out == silk_MUL( Fs_Hz_in, 2 ) ) {                            /* Fs_out : Fs_in = 2 : 1 */
            /* Special case: directly use 2x upsampler */
            S->resampler_function = 1; // USE_silk_resampler_private_up2_HQ_wrapper
        } else {
            /* Default resampler */
            S->resampler_function = 2; // USE_silk_resampler_private_IIR_FIR
            up2x = 1;
        }
    } else if (Fs_Hz_out < Fs_Hz_in) {
        S->resampler_function = 3; // USE_silk_resampler_private_down_FIR
        if( silk_MUL( Fs_Hz_out, 3 ) == silk_MUL( Fs_Hz_in, 2 ) ) {             /* Fs_out : Fs_in = 2 : 3 */
            S->FIR_Fracs = 2;
            S->FIR_Order = RESAMPLER_DOWN_ORDER_FIR0;
            S->Coefs = silk_Resampler_2_3_COEFS;
        } else if( silk_MUL( Fs_Hz_out, 2 ) == Fs_Hz_in ) {                     /* Fs_out : Fs_in = 1 : 2 */
            S->FIR_Fracs = 1;
            S->FIR_Order = RESAMPLER_DOWN_ORDER_FIR1;
            S->Coefs = silk_Resampler_1_2_COEFS;
        }
    } else {
        S->resampler_function = 0; // USE_silk_resampler_copy
    }
    /* Ratio of input/output samples */
    S->invRatio_Q16 = silk_LSHIFT32( silk_DIV32( silk_LSHIFT32( Fs_Hz_in, 14 + up2x ), Fs_Hz_out ), 2 );
    /* Make sure the ratio is rounded up */
    while( silk_SMULWW( S->invRatio_Q16, Fs_Hz_out ) < silk_LSHIFT32( Fs_Hz_in, up2x ) ) {
        S->invRatio_Q16++;
    }

    return S;
}

void smpl_silk_resampler_wrapper_free(void* S) {
    free(S);
}

opus_int32 smpl_resampler_get_fs_in(silk_resampler_state_struct* S) {
    return S->Fs_in_kHz;
}

opus_int32 smpl_resampler_get_fs_out(silk_resampler_state_struct* S) {
    return S->Fs_out_kHz;
}
