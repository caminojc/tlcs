#include "tlcs_tcx.h"
#include "tlcs_tune.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* -- Band structure ---------------------------------------------------------
 * 16 bark-like bands for MDCT bins.
 * For frame_size=320 (Nyquist=8kHz, 25 Hz per bin):
 *   Bands 0-3:   10 bins each (0-250, 250-500, 500-750, 750-1000 Hz)
 *   Bands 4-7:   15 bins each (1000-1375, ... 2125-2500 Hz)
 *   Bands 8-11:  25 bins each (2500-3125, ... 4375-5000 Hz)
 *   Bands 12-15: 30 bins each (5000-5750, ... 7250-8000 Hz)
 * Total: 4*10 + 4*15 + 4*25 + 4*30 = 320 bins
 */
static const int32_t band_widths_320[TCX_NUM_BANDS] = {
    10, 10, 10, 10,   /* 0-1000 Hz */
    15, 15, 15, 15,   /* 1000-2500 Hz */
    25, 25, 25, 25,   /* 2500-5000 Hz */
    30, 30, 30, 30    /* 5000-8000 Hz */
};

void tlcs_tcx_get_bands(int32_t num_bins, int32_t *band_start)
{
    /* Scale band widths proportionally if num_bins != 320 */
    int32_t pos = 0;
    for (int32_t b = 0; b < TCX_NUM_BANDS; b++) {
        band_start[b] = pos;
        int32_t w = band_widths_320[b];
        if (num_bins != 320) {
            w = w * num_bins / 320;
            if (w < 1) w = 1;
        }
        pos += w;
    }
    /* Ensure last band extends to end */
    band_start[TCX_NUM_BANDS] = num_bins;
}

/* -- Gain quantization (log-domain) ----------------------------------------
 * Global gain covers the full dynamic range of MDCT spectral coefficients.
 * MDCT of 16-bit PCM with frame_size=320 can produce RMS values up to ~50000.
 * gain_idx = clip(round(20*log10(rms)), 0, 127)
 * -> range: [0, +127] dB in 1.0 dB steps
 */
static int32_t quantize_gain(float rms)
{
    if (rms < 1.0f) return 0;
    float log_val = 20.0f * log10f(rms);
    int32_t idx = (int32_t)(log_val + 0.5f);
    if (idx < 0) idx = 0;
    if (idx >= TCX_GAIN_LEVELS) idx = TCX_GAIN_LEVELS - 1;
    return idx;
}

static float dequantize_gain(int32_t idx)
{
    return powf(10.0f, (float)idx / 20.0f);
}

/* -- LPC spectral envelope ------------------------------------------------ */
void tlcs_tcx_lpc_envelope(const float *a_q, int32_t order,
                             float *env_out, int32_t frame_size)
{
    int32_t M = frame_size;
    for (int32_t k = 0; k < M; k++) {
        /* MDCT bin frequency: w_k = pi * (k + 0.5) / M */
        float omega = (float)M_PI * ((float)k + 0.5f) / (float)M;
        /* A(w) = sum_{i=0}^{order} a[i] * e^{-j*i*w} */
        float re = 0.0f, im = 0.0f;
        for (int32_t i = 0; i <= order; i++) {
            float angle = (float)i * omega;
            re += a_q[i] * cosf(angle);
            im -= a_q[i] * sinf(angle);
        }
        float mag = sqrtf(re * re + im * im);
        /* Floor to avoid division by zero */
        env_out[k] = (mag > 0.01f) ? mag : 0.01f;
    }
}

/* -- Dead-zone quantizer step table --------------------------------------- */
const float tcx_step_table[TCX_STEP_LEVELS] = {
    0.30f, 0.40f, 0.50f, 0.75f, 1.00f, 1.25f, 1.50f, 2.50f
};

/* -- Laplacian CDF for range coder ---------------------------------------- */

/* Build CDF from a given Laplacian alpha value */
static void build_cdf_from_alpha(float alpha, uint16_t *cdf)
{
    float p_mag[TCX_QUANT_MAX + 1];
    p_mag[0] = 1.0f - expf(-alpha * 0.5f);
    for (int32_t k = 1; k < TCX_QUANT_MAX; k++)
        p_mag[k] = expf(-alpha * ((float)k - 0.5f))
                 - expf(-alpha * ((float)k + 0.5f));
    p_mag[TCX_QUANT_MAX] = expf(-alpha * ((float)TCX_QUANT_MAX - 0.5f));

    uint16_t freq[TCX_SPEC_NSYM];
    const uint16_t total = 4096;
    uint16_t sum = 0;

    freq[TCX_QUANT_MAX] = (uint16_t)(p_mag[0] * (float)total + 0.5f);
    if (freq[TCX_QUANT_MAX] < 1) freq[TCX_QUANT_MAX] = 1;

    for (int32_t k = 1; k <= TCX_QUANT_MAX; k++) {
        uint16_t f = (uint16_t)(p_mag[k] * 0.5f * (float)total + 0.5f);
        if (f < 1) f = 1;
        freq[TCX_QUANT_MAX - k] = f;
        freq[TCX_QUANT_MAX + k] = f;
    }

    for (int32_t i = 0; i < TCX_SPEC_NSYM; i++)
        sum += freq[i];
    int32_t delta = (int32_t)sum - (int32_t)total;
    int32_t adj = (int32_t)freq[TCX_QUANT_MAX] - delta;
    freq[TCX_QUANT_MAX] = (uint16_t)(adj > 1 ? adj : 1);

    cdf[0] = 0;
    for (int32_t i = 0; i < TCX_SPEC_NSYM; i++)
        cdf[i + 1] = cdf[i] + freq[i];
}

void tlcs_tcx_compute_cdf(int32_t step_idx, uint16_t *cdf)
{
    float step = tcx_step_table[step_idx];
    float alpha = step * 1.41421356f; /* step * sqrt(2) */
    build_cdf_from_alpha(alpha, cdf);
}

/* -- Per-band adaptive CDFs ----------------------------------------------- */
void tlcs_tcx_compute_adaptive_cdfs(const float *lpc_env, int32_t frame_size,
                                     int32_t step_idx,
                                     uint16_t cdfs_out[][TCX_SPEC_NSYM + 1])
{
    float step = tcx_step_table[step_idx];
    float base_alpha = step * 1.41421356f; /* step * sqrt(2) */

    int32_t band_start[TCX_NUM_BANDS + 1];
    tlcs_tcx_get_bands(frame_size, band_start);

    /* Compute per-band mean |A(w)|^2 and global mean */
    float band_energy[TCX_NUM_BANDS];
    float global_energy = 0.0f;
    for (int32_t b = 0; b < TCX_NUM_BANDS; b++) {
        float sum2 = 0.0f;
        int32_t bstart = band_start[b];
        int32_t bend   = band_start[b + 1];
        for (int32_t k = bstart; k < bend; k++)
            sum2 += lpc_env[k] * lpc_env[k];
        band_energy[b] = sum2 / (float)(bend - bstart);
        global_energy += sum2;
    }
    global_energy /= (float)frame_size;
    if (global_energy < 1e-20f) global_energy = 1e-20f;

    /* Build per-band CDF: alpha_b = base_alpha * sqrt(band_energy / global) */
    for (int32_t b = 0; b < TCX_NUM_BANDS; b++) {
        float ratio = band_energy[b] / global_energy;
        if (ratio < 0.1f) ratio = 0.1f;
        if (ratio > 10.0f) ratio = 10.0f;
        float alpha_b = base_alpha * sqrtf(ratio);
        if (alpha_b < 0.05f) alpha_b = 0.05f;
        build_cdf_from_alpha(alpha_b, cdfs_out[b]);
    }
}

/* -- ALFE (Adaptive Low Frequency Emphasis) ------------------------------- */
void tlcs_tcx_compute_alfe_weights(const float *lpc_env, int32_t frame_size,
                                     float *weights_out)
{
    /* Find F1 and F2: local minima of |A(w)| (formant = low envelope).
     * Boost through both formant regions for better 0-2.5 kHz coverage. */
    int32_t f1_bin = frame_size / 8;  /* fallback ~1 kHz */
    int32_t f2_bin = frame_size / 4;  /* fallback ~2 kHz */
    int32_t found_f1 = 0;

    for (int32_t k = 3; k < frame_size / 2; k++) {
        if (lpc_env[k] < lpc_env[k - 1] && lpc_env[k] < lpc_env[k + 1]) {
            if (!found_f1) {
                f1_bin = k;
                found_f1 = 1;
            } else {
                f2_bin = k;
                break;
            }
        }
    }
    /* Ensure F2 is beyond F1 */
    if (f2_bin <= f1_bin) f2_bin = f1_bin + frame_size / 8;
    if (f2_bin > frame_size / 2) f2_bin = frame_size / 2;

    /* Two-segment ramp:
     *   DC → F1:   ALFE_BOOST → ALFE_BOOST * 0.7 (strong boost through F1)
     *   F1 → F2:   ALFE_BOOST * 0.7 → 1.0 (taper through F2 region)
     *   F2+:       1.0 (no boost) */
    float boost = TUNE_ALFE_BOOST;
    float mid_boost = 1.0f + (boost - 1.0f) * 0.5f; /* halfway between boost and 1.0 */
    for (int32_t k = 0; k < frame_size; k++) {
        if (k <= f1_bin) {
            float t = (float)k / (float)(f1_bin + 1);
            weights_out[k] = boost - (boost - mid_boost) * t;
        } else if (k <= f2_bin) {
            float t = (float)(k - f1_bin) / (float)(f2_bin - f1_bin + 1);
            weights_out[k] = mid_boost - (mid_boost - 1.0f) * t;
        } else {
            weights_out[k] = 1.0f;
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * TNS (Temporal Noise Shaping)
 *
 * Problem: MDCT quantization noise spreads uniformly across the 20ms frame
 * in the time domain, creating a "bathroom reverb" effect — the noise
 * persists even during silent gaps between pitch pulses.
 *
 * Solution: Before quantization, apply a short LPC analysis filter ALONG
 * the MDCT coefficients (not in time — in the spectral index direction).
 * This whitens the spectral shape within each band, concentrating the
 * signal energy into fewer coefficients. After dequantization, the decoder
 * applies the inverse (synthesis) filter to restore the original shape.
 *
 * The key insight: filtering along MDCT bins is equivalent to shaping the
 * temporal envelope of quantization noise. The noise gets pushed into the
 * high-energy parts of the frame (where the speech is) and suppressed in
 * the quiet gaps — exactly removing the bathroom effect.
 *
 * No extra bits needed: the decoder derives TNS coefficients from the
 * dequantized spectrum (same as ALFE).
 * ══════════════════════════════════════════════════════════════════════════ */

/* Compute TNS LPC from autocorrelation of spectral segment */
int32_t tlcs_tns_analysis(const float *spec, int32_t start, int32_t end,
                           float *tns_coeff)
{
    int32_t len = end - start;
    if (len < TNS_ORDER * 4) return 0;  /* too short */

    /* Autocorrelation of spectral magnitudes */
    float r[TNS_ORDER + 1];
    for (int32_t k = 0; k <= TNS_ORDER; k++) {
        float sum = 0.0f;
        for (int32_t i = start; i < end - k; i++)
            sum += spec[i] * spec[i + k];
        r[k] = sum;
    }

    /* Skip TNS if signal is too flat (no temporal structure to shape) */
    if (r[0] < 1e-10f) return 0;
    float pred_gain = r[1] * r[1] / (r[0] * r[0]);
    if (pred_gain < 0.02f) return 0;  /* less than 2% predictable — skip */

    /* Levinson-Durbin for TNS coefficients */
    float a[TNS_ORDER + 1];
    a[0] = 1.0f;
    float err = r[0];

    for (int32_t i = 1; i <= TNS_ORDER; i++) {
        float sum = 0.0f;
        for (int32_t j = 1; j < i; j++)
            sum += a[j] * r[i - j];
        float k_ref = -(r[i] + sum) / err;

        /* Clamp reflection coefficient for stability */
        if (k_ref > 0.95f) k_ref = 0.95f;
        if (k_ref < -0.95f) k_ref = -0.95f;

        /* Update filter */
        float a_new[TNS_ORDER + 1];
        a_new[0] = 1.0f;
        for (int32_t j = 1; j < i; j++)
            a_new[j] = a[j] + k_ref * a[i - j];
        a_new[i] = k_ref;
        memcpy(a, a_new, (size_t)(i + 1) * sizeof(float));

        err *= (1.0f - k_ref * k_ref);
        if (err < 1e-10f) break;
    }

    for (int32_t i = 0; i < TNS_ORDER; i++)
        tns_coeff[i] = a[i + 1];

    return TNS_ORDER;
}

/* Forward (analysis) filter: A(z) applied along spectral bins */
void tlcs_tns_filter_forward(float *spec, int32_t start, int32_t end,
                              const float *tns_coeff, int32_t tns_order)
{
    if (tns_order <= 0) return;

    /* Apply FIR filter: y[n] = x[n] + sum(a[k] * x[n-k]) */
    float state[TNS_ORDER];
    memset(state, 0, sizeof(state));

    for (int32_t i = start; i < end; i++) {
        float x = spec[i];
        float y = x;
        for (int32_t k = 0; k < tns_order; k++)
            y += tns_coeff[k] * state[k];

        /* Shift state */
        for (int32_t k = tns_order - 1; k > 0; k--)
            state[k] = state[k - 1];
        state[0] = x;

        spec[i] = y;
    }
}

/* Inverse (synthesis) filter: 1/A(z) applied along spectral bins */
void tlcs_tns_filter_inverse(float *spec, int32_t start, int32_t end,
                              const float *tns_coeff, int32_t tns_order)
{
    if (tns_order <= 0) return;

    /* Apply IIR filter: y[n] = x[n] - sum(a[k] * y[n-k]) */
    float state[TNS_ORDER];
    memset(state, 0, sizeof(state));

    for (int32_t i = start; i < end; i++) {
        float x = spec[i];
        float y = x;
        for (int32_t k = 0; k < tns_order; k++)
            y -= tns_coeff[k] * state[k];

        /* Shift state */
        for (int32_t k = tns_order - 1; k > 0; k--)
            state[k] = state[k - 1];
        state[0] = y;

        spec[i] = y;
    }
}

/* -- TCX Encode ----------------------------------------------------------- */
float tlcs_tcx_encode(const float *spec, const float *lpc_env,
                       int32_t frame_size,
                       int32_t step_idx, tlcs_tcx_params *params)
{
    params->num_bins = frame_size;
    params->step_idx = step_idx;

    /* Compute ALFE weights — boost low-freq formant bins before quantization.
     * Decoder derives the same weights from quantized LPC: zero bits cost. */
    float alfe[TLCS_MAX_FRAME_SIZE];
    tlcs_tcx_compute_alfe_weights(lpc_env, frame_size, alfe);

    /* LPC-whiten the spectrum: multiply by |A(w)| then apply ALFE */
    float whitened[TLCS_MAX_FRAME_SIZE];
    for (int32_t i = 0; i < frame_size; i++)
        whitened[i] = spec[i] * lpc_env[i] * alfe[i];

    /* Compute global RMS of whitened spectrum */
    float sum2 = 0.0f;
    for (int32_t i = 0; i < frame_size; i++)
        sum2 += whitened[i] * whitened[i];
    float global_rms = sqrtf(sum2 / (float)frame_size);
    params->global_gain_idx = quantize_gain(global_rms);

    float global_gain = dequantize_gain(params->global_gain_idx);

    /* Perceptual weight for distortion: lower envelope = formant peak,
     * so weight = 1/env^2 emphasizes formant accuracy in step selection. */
    float pweight[TLCS_MAX_FRAME_SIZE];
    {
        float env_mean = 0.0f;
        for (int32_t i = 0; i < frame_size; i++)
            env_mean += lpc_env[i];
        env_mean /= (float)frame_size;
        if (env_mean < 0.01f) env_mean = 0.01f;
        for (int32_t i = 0; i < frame_size; i++) {
            float ratio = env_mean / lpc_env[i];
            /* Soft-clamp to avoid runaway weights in deep valleys */
            if (ratio > 4.0f) ratio = 4.0f;
            pweight[i] = ratio * ratio;
        }
    }

    /* Quantize all bins using global gain only (no per-band gains) */
    float step = tcx_step_table[step_idx];
    float dist = 0.0f;
    if (global_gain < 1e-10f) {
        memset(params->quant, 0, (size_t)frame_size * sizeof(int8_t));
        for (int32_t i = 0; i < frame_size; i++)
            dist += whitened[i] * whitened[i] * pweight[i];
    } else {
        for (int32_t i = 0; i < frame_size; i++) {
            float norm = whitened[i] / global_gain;
            float mag = fabsf(norm) / step;
            int32_t qmag = (int32_t)(mag + TUNE_DZ_OFFSET);
            if (qmag > TCX_QUANT_MAX) qmag = TCX_QUANT_MAX;
            params->quant[i] = (whitened[i] >= 0.0f) ? (int8_t)qmag : (int8_t)(-qmag);

            float recon = (float)params->quant[i] * step * global_gain;
            float err = whitened[i] - recon;
            dist += err * err * pweight[i];
        }
    }
    params->num_coded_bins = frame_size;

    return dist;
}

/* Simple pseudo-random float in [-1, 1] */
static float prng_float(uint32_t *seed)
{
    *seed = *seed * 1664525u + 1013904223u;
    return ((float)(int32_t)*seed) / 2147483648.0f;
}


/* -- TCX Decode ----------------------------------------------------------- */
void tlcs_tcx_decode(const tlcs_tcx_params *params, const float *lpc_env,
                      float *spec_out, uint32_t *noise_seed)
{
    int32_t frame_size = params->num_bins;
    int32_t num_coded  = params->num_coded_bins;
    float global_gain  = dequantize_gain(params->global_gain_idx);
    float step = tcx_step_table[params->step_idx];

    float nf_coded = step * TUNE_NF_CODED;

    /* -- Pass 1: Decode coded region (bins 0..num_coded-1) -- */
    float whitened[TLCS_MAX_FRAME_SIZE];
    int32_t nz_count = 0;
    for (int32_t i = 0; i < num_coded; i++) {
        if (params->quant[i] != 0) {
            whitened[i] = (float)params->quant[i] * step * global_gain;
            nz_count++;
        } else {
            float r = prng_float(noise_seed);
            whitened[i] = r * nf_coded * global_gain;
        }
    }

    /* -- Pass 1.5: Spectral smoothing DISABLED (caused gargle artifacts) */
#if 0
    {
        float prev = whitened[0];
        for (int32_t i = 1; i < num_coded - 1; i++) {
            float cur = whitened[i];
            float next = whitened[i + 1];
            whitened[i] = 0.25f * prev + 0.5f * cur + 0.25f * next;
            prev = cur;
        }
    }
#endif

    /* -- Pass 2: SBR extension for uncoded region -- */
    float nz_frac = (num_coded > 0) ? (float)nz_count / (float)num_coded : 0.0f;
    float sbr_base = nz_frac * TUNE_SBR_BASE_MULT;
    if (sbr_base > 0.6f) sbr_base = 0.6f;

    for (int32_t i = num_coded; i < frame_size; i++) {
        int32_t d = i - num_coded;
        float frac = (float)d / (float)(frame_size - num_coded);

        float nf_level = step * (TUNE_NF_UNCODED_HI - TUNE_NF_UNCODED_SLOPE * frac);
        if (nf_level < 0.15f) nf_level = 0.15f;
        float noise = prng_float(noise_seed) * nf_level * global_gain;

        /* SBR: use configurable fraction of coded spectrum */
        int32_t sbr_depth = (int32_t)((float)num_coded * TUNE_SBR_DEPTH_FRAC);
        if (sbr_depth < 16) sbr_depth = 16;
        if (sbr_depth > num_coded) sbr_depth = num_coded;
        int32_t src = num_coded - 1 - (d % sbr_depth);
        float sbr = whitened[src];

        float sbr_w = sbr_base * (1.0f - 0.6f * frac);
        if (sbr_w < 0.0f) sbr_w = 0.0f;
        whitened[i] = sbr_w * sbr + (1.0f - sbr_w) * noise;
    }

    /* -- Pass 3: Undo ALFE then dewhiten -- */
    float alfe[TLCS_MAX_FRAME_SIZE];
    tlcs_tcx_compute_alfe_weights(lpc_env, frame_size, alfe);
    for (int32_t i = 0; i < frame_size; i++)
        spec_out[i] = whitened[i] / (lpc_env[i] * alfe[i]);
}
