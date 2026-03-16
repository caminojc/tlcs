/* Internal quantization API. */
#ifndef TLCS_QUANTIZATION_H
#define TLCS_QUANTIZATION_H

#include "tlcs_config.h"

/* ---- LSP Split VQ -------------------------------------------------- */
void tlcs_lsp_vq_init(void);
void tlcs_lsp_vq_quantize(const float *lsp, int *indices, float *lsp_q);
void tlcs_lsp_vq_dequantize(const int *indices, float *lsp_out);

/* ---- Scalar pitch gain quantizer ----------------------------------- */
/* Uniform scalar quantize pitch gain [0, 1.2] to TLCS_PITCH_GAIN_BITS index. */
int tlcs_pitch_gain_quantize(float gain);
float tlcs_pitch_gain_dequantize(int index);

/* ---- 2-basis ACB joint gain codebook (MLOW-style) ------------------ */
/* 8-entry codebook of (g0, g1) pairs, indexed by 3-bit pitch_gain_idx. */
void tlcs_acb_gain_dequantize(int index, float *g0, float *g1);
/* Returns all 8 entries for encoder search. */
const float (*tlcs_acb_gain_codebook(void))[2];

/* ---- FCB gain quantizer — dB-stepped (SMPL-style) ------------------ */
/* Quantize weighted-domain FCB gain to 4-bit dB index.
 * voiced: use voiced dB table; !voiced: use unvoiced dB table.
 * Returns index, writes quantized gain to *out_gain. */
int tlcs_fcbgain_quantize(float gain_weighted, int voiced, float *out_gain);
float tlcs_fcbgain_dequantize(int index, int voiced);

/* ---- Legacy joint gain VQ (kept for compatibility) ----------------- */
void tlcs_gain_vq_init(void);
int tlcs_gain_vq_quantize(float pg, float cg, float *q_pg, float *q_cg);
void tlcs_gain_vq_dequantize(int index, float *pg, float *cg);

#endif /* TLCS_QUANTIZATION_H */
