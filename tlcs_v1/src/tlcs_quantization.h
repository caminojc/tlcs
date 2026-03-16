/* Internal quantization API. */
#ifndef TLCS_QUANTIZATION_H
#define TLCS_QUANTIZATION_H

#include "tlcs_config.h"

/* ---- LSP Split VQ -------------------------------------------------- */

/* Initialise the LSP codebooks with linearly-spaced defaults.
 * Must be called once before quantize/dequantize. */
void tlcs_lsp_vq_init(void);

/* Quantize LSP vector (order elements) into 4 codebook indices.
 * lsp:     input LSP vector (TLCS_LPC_ORDER).
 * indices: output indices (TLCS_LSP_NUM_SPLITS).
 * lsp_q:   output quantised LSP (TLCS_LPC_ORDER). */
void tlcs_lsp_vq_quantize(const float *lsp, int *indices, float *lsp_q);

/* Dequantize: reconstruct LSP from indices. */
void tlcs_lsp_vq_dequantize(const int *indices, float *lsp_out);

/* ---- Joint Gain VQ ------------------------------------------------- */

/* Initialise gain codebook (8 pitch-gain x 8 cb-gain grid). */
void tlcs_gain_vq_init(void);

/* Quantize (pitch_gain, cb_gain) pair.
 * Returns index, writes quantised values to q_pg and q_cg. */
int tlcs_gain_vq_quantize(float pg, float cg, float *q_pg, float *q_cg);

/* Dequantize gain index. */
void tlcs_gain_vq_dequantize(int index, float *pg, float *cg);

/* ---- Scalar pitch gain quantizer ----------------------------------- */

/* Uniform scalar quantize pitch gain [0, 1.2] to TLCS_PITCH_GAIN_BITS index. */
int tlcs_pitch_gain_quantize(float gain);

/* Dequantize pitch gain index. */
float tlcs_pitch_gain_dequantize(int index);

#endif /* TLCS_QUANTIZATION_H */
