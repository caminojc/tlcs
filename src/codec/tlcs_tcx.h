#ifndef TLCS_TCX_H
#define TLCS_TCX_H

#include "tlcs/tlcs_types.h"

/* TCX spectral quantization for hybrid LP+TCX codec.
 * Gain-shape coding with adaptive per-band CDFs. */

/* Global gain quantization */
#define TCX_GAIN_BITS       7
#define TCX_GAIN_LEVELS     (1 << TCX_GAIN_BITS)

/* Band structure: bark-like bands spanning frame_size MDCT bins.
 * Per-band adaptive CDFs replace transmitted band gains — the LPC envelope
 * shapes the Laplacian width per band, allocating bits automatically. */
#define TCX_NUM_BANDS       16

/* Dead-zone quantizer step size selection (3 bits -> 8 entries) */
#define TCX_STEP_BITS       3
#define TCX_STEP_LEVELS     (1 << TCX_STEP_BITS)

/* Spectral symbol alphabet: quantized values -15..+15 = 31 symbols */
#define TCX_SPEC_NSYM       31
#define TCX_QUANT_MAX       15

/* LTP (Long-Term Prediction) for TCX */
#define TCX_LTP_LAG_BITS    8    /* 256 values covering lag 20-275 */
#define TCX_LTP_GAIN_BITS   3    /* 8 gain levels: 0, 0.1, 0.2, ..., 0.7 */
#define TCX_LTP_GAIN_LEVELS (1 << TCX_LTP_GAIN_BITS)

/* Step table declaration (defined in tlcs_tcx.c) */
extern const float tcx_step_table[TCX_STEP_LEVELS];

/* TCX encoded parameters */
typedef struct {
    int32_t global_gain_idx;                /* 7 bits: log-scale global RMS */
    int32_t step_idx;                       /* 3 bits: quantizer step size index */
    int8_t  quant[TLCS_MAX_FRAME_SIZE];     /* quantized value per coded bin (+/-15 range) */
    int32_t num_coded_bins;                 /* bins with entropy-coded values */
    int32_t num_bins;                       /* total MDCT bins = frame_size */
} tlcs_tcx_params;

/* Get band boundaries.
 * band_start[b] = first bin of band b, band_start[TCX_NUM_BANDS] = num_bins */
void tlcs_tcx_get_bands(int32_t num_bins, int32_t *band_start);

/* Compute LPC spectral envelope |A(w_k)| at MDCT bin frequencies. */
void tlcs_tcx_lpc_envelope(const float *a_q, int32_t order,
                             float *env_out, int32_t frame_size);

/* Compute Laplacian CDF for range-coding spectral symbols at a given step. */
void tlcs_tcx_compute_cdf(int32_t step_idx, uint16_t *cdf);

/* Compute per-band adaptive CDFs from LPC envelope.
 * Alpha per band scales with local |A(w)| energy: valleys get narrow CDFs
 * (fewer bits for small values), formants get wide CDFs (more bits). */
void tlcs_tcx_compute_adaptive_cdfs(const float *lpc_env, int32_t frame_size,
                                     int32_t step_idx,
                                     uint16_t cdfs_out[][TCX_SPEC_NSYM + 1]);

/* Compute ALFE (Adaptive Low Frequency Emphasis) weights from LPC envelope.
 * Boosts low-frequency bins before quantization to allocate more bits there.
 * Weights are deterministic from quantized LPC — no extra bits needed. */
void tlcs_tcx_compute_alfe_weights(const float *lpc_env, int32_t frame_size,
                                     float *weights_out);

/* TCX encode: LPC-whitened MDCT spectrum -> quantized parameters. */
float tlcs_tcx_encode(const float *spec, const float *lpc_env,
                       int32_t frame_size,
                       int32_t step_idx, tlcs_tcx_params *params);

/* TCX decode: quantized parameters -> reconstructed MDCT spectrum. */
void tlcs_tcx_decode(const tlcs_tcx_params *params, const float *lpc_env,
                      float *spec_out, uint32_t *noise_seed);

/* ── TNS (Temporal Noise Shaping) ──────────────────────────────────
 * Applies a short LPC filter along MDCT coefficients to constrain
 * quantization noise to the temporal envelope of the signal.
 * This removes the "bathroom reverb" inherent to transform coding.
 *
 * Encoder: analysis filter (whiten temporal structure)
 * Decoder: synthesis filter (restore temporal structure)
 *
 * TNS is applied per-band in the whitened MDCT domain.
 * Filter coefficients are derived from the MDCT spectrum itself
 * (no extra bits needed — decoder derives same coefficients from
 * dequantized spectrum).
 */
#define TNS_ORDER       2      /* TNS filter order (keep low for stability) */
#define TNS_MAX_BANDS   4      /* number of TNS processing regions */

/* Compute TNS filter coefficients from MDCT spectrum.
 * Returns filter order actually used (0 = TNS disabled for this band). */
int32_t tlcs_tns_analysis(const float *spec, int32_t start, int32_t end,
                           float *tns_coeff);

/* Apply TNS analysis (forward) filter to MDCT bins in-place. */
void tlcs_tns_filter_forward(float *spec, int32_t start, int32_t end,
                              const float *tns_coeff, int32_t tns_order);

/* Apply TNS synthesis (inverse) filter to MDCT bins in-place. */
void tlcs_tns_filter_inverse(float *spec, int32_t start, int32_t end,
                              const float *tns_coeff, int32_t tns_order);

#endif /* TLCS_TCX_H */
