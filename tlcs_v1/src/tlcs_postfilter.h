/* Internal postfilter API. */
#ifndef TLCS_POSTFILTER_H
#define TLCS_POSTFILTER_H

#include "tlcs_config.h"

/* Postfilter state — kept opaque, allocated by encoder/decoder. */
typedef struct {
    /* Formant postfilter memories */
    float fir_mem[TLCS_LPC_ORDER];
    float iir_mem[TLCS_LPC_ORDER];
    /* Tilt compensation */
    float tilt_prev_in;
    /* AGC */
    float agc_gain;
    /* Harmonic postfilter buffer */
    float harm_buf[TLCS_PITCH_MAX_LAG + TLCS_FRAME_SIZE];
    int   harm_buf_pos;
} TlcsPostfilterState;

/* Initialise (zero all state). */
void tlcs_postfilter_init(TlcsPostfilterState *pf);

/* Apply harmonic postfilter to one subframe.
 * in:  input speech (subframe_size).
 * pitch_lag: integer pitch lag.
 * out: output (subframe_size, may alias in). */
void tlcs_harmonic_postfilter(TlcsPostfilterState *pf,
                              const float *in, int len,
                              int pitch_lag, float *out);

/* Apply formant postfilter + tilt + AGC to one subframe.
 * lpc: LPC coefficients (order+1).
 * in:  input (subframe_size).
 * out: output (subframe_size). */
void tlcs_formant_postfilter(TlcsPostfilterState *pf,
                             const float *in, int len,
                             const float *lpc, int order, float *out);

#endif /* TLCS_POSTFILTER_H */
