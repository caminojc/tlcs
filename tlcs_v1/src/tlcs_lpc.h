/* Internal LPC analysis/synthesis API. */
#ifndef TLCS_LPC_H
#define TLCS_LPC_H

#include "tlcs_config.h"

/* Autocorrelation + Levinson-Durbin LPC analysis.
 * frame:    pre-emphasised speech (frame_len samples)
 * order:    LPC order
 * lpc_out:  output coefficients [0..order], lpc_out[0] = 1.0
 * gain_out: prediction error energy (sqrt)
 */
void tlcs_lpc_analysis(const float *frame, int frame_len, int order,
                       float *lpc_out, float *gain_out);

/* Bandwidth expansion: a[k] *= gamma^k */
void tlcs_bwe(float *lpc, int order, float gamma);

/* LPC -> LSP (Chebyshev root search). lsp_out in (0, pi). */
void tlcs_lpc_to_lsp(const float *lpc, int order, float *lsp_out);

/* LSP -> LPC reconstruction. lpc_out[0..order], lpc_out[0] = 1.0. */
void tlcs_lsp_to_lpc(const float *lsp, int order, float *lpc_out);

/* Linear LSP interpolation: out = (1-alpha)*prev + alpha*curr. */
void tlcs_lsp_interpolate(const float *prev, const float *curr, float alpha,
                          int order, float *out);

/* Stabilise LSP: enforce ordering with min gap, clamp to (0, pi). */
void tlcs_lsp_stabilize(float *lsp, int order, float min_gap);

/* All-pole synthesis: s[n] = exc[n] - sum(a[k]*s[n-k]).
 * state: filter memory of 'order' samples, updated in place. */
void tlcs_lpc_synthesis(const float *exc, int len, const float *lpc, int order,
                        float *state, float *out);

/* Zero-state (ringing) response: feed zero excitation, observe ringing. */
void tlcs_lpc_zero_state_response(const float *lpc, int order,
                                  const float *state, int len, float *out);

/* Compute impulse response of 1/A(z) */
void tlcs_lpc_impulse_response(const float *lpc, int order, int len, float *h);

#endif /* TLCS_LPC_H */
