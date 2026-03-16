/*
 * tlcs_postfilter.c — Adaptive post-processing.
 *
 * - Harmonic (pitch) postfilter: comb enhancement
 * - Formant postfilter: A(z/gamma1) / A(z/gamma2)
 * - Tilt compensation: 1 - mu * z^-1
 * - Automatic gain control (AGC)
 */
#include "tlcs_config.h"
#include "tlcs_postfilter.h"

#include <math.h>
#include <string.h>

/* ================================================================== */
/* Init                                                                */
/* ================================================================== */

void tlcs_postfilter_init(TlcsPostfilterState *pf)
{
    memset(pf, 0, sizeof(*pf));
    pf->agc_gain = 1.0f;
    pf->harm_buf_pos = TLCS_PITCH_MAX_LAG;
}

/* ================================================================== */
/* Harmonic postfilter                                                 */
/* ================================================================== */

void tlcs_harmonic_postfilter(TlcsPostfilterState *pf,
                              const float *in, int len,
                              int pitch_lag, float *out)
{
    if (pitch_lag <= 0 || pitch_lag > TLCS_PITCH_MAX_LAG) {
        memcpy(out, in, len * sizeof(float));
        return;
    }

    /* ---- Step 1: Compute normalized cross-correlation ---- */
    /* between signal and its pitch-delayed version */
    float corr = 0.0f, en_sig = 0.0f, en_del = 0.0f;

    int buf_size = TLCS_PITCH_MAX_LAG + TLCS_FRAME_SIZE;
    for (int i = 0; i < len; i++) {
        int cur_idx = pf->harm_buf_pos + i;
        int del_idx = cur_idx - pitch_lag;
        float cur_val = in[i];
        float del_val = 0.0f;
        if (del_idx >= 0 && del_idx < buf_size) {
            del_val = pf->harm_buf[del_idx];
        }
        corr += cur_val * del_val;
        en_sig += cur_val * cur_val;
        en_del += del_val * del_val;
    }

    float norm_corr = 0.0f;
    if (en_sig > 1e-10f && en_del > 1e-10f) {
        norm_corr = corr / sqrtf(en_sig * en_del);
    }
    if (norm_corr < 0.0f) norm_corr = 0.0f;
    if (norm_corr > 1.0f) norm_corr = 1.0f;

    /* ---- Step 2: Adaptive gain from correlation strength ---- */
    /* Strong correlation -> more pitch enhancement; weak -> less */
    float strength = TLCS_HARM_POSTF_STRENGTH * norm_corr;

    /* ---- Step 3: Measure input energy for AGC ---- */
    float in_energy = en_sig;

    /* ---- Step 4: Apply comb filter with lowpass on delayed signal ---- */
    /* Simple 3-tap lowpass [0.25, 0.5, 0.25] on pitch-delayed signal
     * to prevent high-frequency noise amplification */
    for (int i = 0; i < len; i++) {
        int del_idx = pf->harm_buf_pos + i - pitch_lag;

        /* Fetch 3 delayed samples for lowpass */
        float d_m1 = 0.0f, d_0 = 0.0f, d_p1 = 0.0f;
        if (del_idx - 1 >= 0 && del_idx - 1 < buf_size)
            d_m1 = pf->harm_buf[del_idx - 1];
        if (del_idx >= 0 && del_idx < buf_size)
            d_0 = pf->harm_buf[del_idx];
        if (del_idx + 1 >= 0 && del_idx + 1 < buf_size)
            d_p1 = pf->harm_buf[del_idx + 1];

        float lp_delayed = 0.25f * d_m1 + 0.5f * d_0 + 0.25f * d_p1;

        /* Comb filter with lowpassed delayed signal */
        out[i] = in[i] + strength * lp_delayed;

        /* Update history buffer with current input */
        int buf_idx = pf->harm_buf_pos + i;
        if (buf_idx >= 0 && buf_idx < buf_size) {
            pf->harm_buf[buf_idx] = in[i];
        }
    }

    /* Shift buffer */
    int total = buf_size;
    if (len < total) {
        memmove(pf->harm_buf, pf->harm_buf + len,
                (total - len) * sizeof(float));
        memset(pf->harm_buf + total - len, 0, len * sizeof(float));
    }

    /* ---- Step 5: Smooth AGC to match input energy ---- */
    float out_energy = 0.0f;
    for (int i = 0; i < len; i++) {
        out_energy += out[i] * out[i];
    }

    if (out_energy > 1e-10f) {
        float target_gain = sqrtf(in_energy / out_energy);
        /* Smoothly interpolate AGC gain */
        const float agc_alpha = 0.85f;
        float g = pf->agc_gain;
        for (int i = 0; i < len; i++) {
            g = agc_alpha * g + (1.0f - agc_alpha) * target_gain;
            out[i] *= g;
        }
        pf->agc_gain = g;
    }
}

/* ================================================================== */
/* Formant postfilter + tilt + AGC                                     */
/* ================================================================== */

void tlcs_formant_postfilter(TlcsPostfilterState *pf,
                             const float *in, int len,
                             const float *lpc, int order, float *out)
{
    const float gamma_num = TLCS_FORMANT_PF_GAMMA_NUM;
    const float gamma_den = TLCS_FORMANT_PF_GAMMA_DEN;
    const float tilt_coeff = TLCS_FORMANT_PF_TILT;

    /* Build weighted LPC: a_num[k] = a[k] * gamma_num^k, a_den likewise */
    float a_num[TLCS_LPC_ORDER + 1];
    float a_den[TLCS_LPC_ORDER + 1];
    a_num[0] = 1.0f;
    a_den[0] = 1.0f;
    float gn = gamma_num;
    float gd = gamma_den;
    for (int k = 1; k <= order; k++) {
        a_num[k] = lpc[k] * gn;
        a_den[k] = lpc[k] * gd;
        gn *= gamma_num;
        gd *= gamma_den;
    }

    /* --- Numerator (FIR): A(z/gamma1) --- */
    float intermediate[TLCS_SUBFRAME_SIZE];
    for (int i = 0; i < len; i++) {
        float val = in[i];
        for (int k = 0; k < order; k++) {
            val += a_num[k + 1] * pf->fir_mem[k];
        }
        intermediate[i] = val;
        /* Shift memory */
        for (int k = order - 1; k > 0; k--) {
            pf->fir_mem[k] = pf->fir_mem[k - 1];
        }
        pf->fir_mem[0] = in[i];
    }

    /* --- Denominator (IIR): 1/A(z/gamma2) --- */
    float filtered[TLCS_SUBFRAME_SIZE];
    for (int i = 0; i < len; i++) {
        float val = intermediate[i];
        for (int k = 0; k < order; k++) {
            val -= a_den[k + 1] * pf->iir_mem[k];
        }
        filtered[i] = val;
        for (int k = order - 1; k > 0; k--) {
            pf->iir_mem[k] = pf->iir_mem[k - 1];
        }
        pf->iir_mem[0] = filtered[i];
    }

    /* --- Tilt compensation: 1 - mu * z^{-1} --- */
    float tilt_out[TLCS_SUBFRAME_SIZE];
    float prev_in = pf->tilt_prev_in;
    for (int i = 0; i < len; i++) {
        tilt_out[i] = filtered[i] - tilt_coeff * prev_in;
        prev_in = filtered[i];
    }
    pf->tilt_prev_in = prev_in;

    /* --- AGC: smooth gain to match input energy --- */
    float input_energy = 0.0f;
    float output_energy = 0.0f;
    for (int i = 0; i < len; i++) {
        input_energy += in[i] * in[i];
        output_energy += tilt_out[i] * tilt_out[i];
    }

    float target_gain = 1.0f;
    if (output_energy > 1e-10f) {
        target_gain = sqrtf(input_energy / output_energy);
    }

    const float agc_alpha = 0.9f;
    float g = pf->agc_gain;
    for (int i = 0; i < len; i++) {
        g = agc_alpha * g + (1.0f - agc_alpha) * target_gain;
        out[i] = tilt_out[i] * g;
    }
    pf->agc_gain = g;
}
