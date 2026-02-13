#ifndef TLCS_DIAG_ENCODE_H
#define TLCS_DIAG_ENCODE_H

#include "tlcs/tlcs_types.h"

/* ── Diagnostic encoder ───────────────────────────────────────────
 * Instrumented version of tlcs_encode() that captures per-frame
 * metrics and component signals for quality decomposition.
 * No changes to encoder logic — only additional data capture.
 * ────────────────────────────────────────────────────────────────── */

typedef struct {
    int32_t frame_num;
    float   lsf_sd;                        /* spectral distortion (dB) */
    float   voicing;                       /* OL voicing [0,1] */
    float   pitch_gain[TLCS_SUBFRAMES];    /* quantized pitch gain */
    float   pitch_gain_unquant[TLCS_SUBFRAMES]; /* pre-quant pitch gain */
    int32_t pitch_lag[TLCS_SUBFRAMES];
    float   residual_energy;               /* total residual energy */
    float   pitch_energy;                  /* energy captured by pitch */
    float   cb_energy;                     /* energy captured by codebook */
    float   error_energy;                  /* uncaptured energy */
    float   cb_gain[TLCS_SUBFRAMES];       /* quantized FCB gain */
    float   cb_gain_unquant[TLCS_SUBFRAMES]; /* optimal gain before quant */
} tlcs_diag_frame;

typedef struct {
    float pitch_exc[TLCS_MAX_FRAME_SIZE];  /* gp * adaptive_vec */
    float cb_exc[TLCS_MAX_FRAME_SIZE];     /* innovation */
    float full_exc[TLCS_MAX_FRAME_SIZE];   /* combined excitation */
    float residual[TLCS_MAX_FRAME_SIZE];   /* LPC residual */
    float a_unquant[TLCS_LPC_ORDER_MAX + 1]; /* unquantized LPC */
    float a_quant[TLCS_LPC_ORDER_MAX + 1];   /* quantized LPC */
    tlcs_diag_frame metrics;
} tlcs_diag_output;

/* Encode one frame with diagnostic output.
 * Same interface as tlcs_encode(), plus diag output struct.
 * The encoder state is updated identically to tlcs_encode(). */
tlcs_status tlcs_diag_encode(tlcs_encoder *enc,
                              const int16_t *pcm_in,
                              uint8_t *bitstream_out,
                              int32_t *bytes_written,
                              tlcs_diag_output *diag);

#endif /* TLCS_DIAG_ENCODE_H */
