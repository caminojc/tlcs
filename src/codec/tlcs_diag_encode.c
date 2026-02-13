#include "tlcs_diag_encode.h"
#include "tlcs/tlcs.h"
#include "tlcs_lpc.h"
#include "tlcs_pitch.h"
#include "tlcs_codebook.h"
#include "../bitstream/tlcs_bitstream.h"
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Compute spectral distortion between unquantized and quantized LPC.
 * SD = sqrt( (1/N) * sum_w [ 10*log10(|1/A_q(w)|^2) - 10*log10(|1/A(w)|^2) ]^2 )
 * Evaluated at N equally spaced points on [0, pi]. */
static float compute_spectral_distortion(const float *a_unq, const float *a_q,
                                          int32_t order, int32_t npts)
{
    float sum_sq = 0.0f;
    for (int32_t i = 0; i < npts; i++) {
        float w = (float)M_PI * (float)(i + 1) / (float)(npts + 1);

        /* Evaluate |A(w)|^2 for both */
        float re_u = 1.0f, im_u = 0.0f;
        float re_q = 1.0f, im_q = 0.0f;
        for (int32_t k = 1; k <= order; k++) {
            float c = cosf((float)k * w);
            float s = sinf((float)k * w);
            re_u += a_unq[k] * c;
            im_u += a_unq[k] * s;
            re_q += a_q[k] * c;
            im_q += a_q[k] * s;
        }
        float mag2_u = re_u * re_u + im_u * im_u;
        float mag2_q = re_q * re_q + im_q * im_q;

        /* 10*log10(1/|A|^2) = -10*log10(|A|^2) */
        if (mag2_u < 1e-10f) mag2_u = 1e-10f;
        if (mag2_q < 1e-10f) mag2_q = 1e-10f;
        float db_u = -10.0f * log10f(mag2_u);
        float db_q = -10.0f * log10f(mag2_q);
        float diff = db_q - db_u;
        sum_sq += diff * diff;
    }
    return sqrtf(sum_sq / (float)npts);
}

tlcs_status tlcs_diag_encode(tlcs_encoder *enc,
                              const int16_t *pcm_in,
                              uint8_t *bitstream_out,
                              int32_t *bytes_written,
                              tlcs_diag_output *diag)
{
    if (!enc || !pcm_in || !bitstream_out || !bytes_written || !diag)
        return TLCS_ERR_INVALID_ARG;

    memset(diag, 0, sizeof(*diag));
    diag->metrics.frame_num = (int32_t)enc->frame_count;

    int32_t n       = enc->cfg.frame_size;
    int32_t order   = enc->cfg.lpc_order;
    int32_t subfr   = enc->cfg.subfr_size;
    int32_t n_subfr = TLCS_SUBFRAMES;

    /* ── Step 1: Pre-emphasis ─────────────────────────────────── */
    float speech[TLCS_MAX_FRAME_SIZE];
    float preemph_mem_f = (float)enc->preemph_mem;
    tlcs_preemph(pcm_in, speech, n, &preemph_mem_f);
    enc->preemph_mem = (int16_t)preemph_mem_f;

    /* ── Step 2: LPC analysis → LSF → quantize ───────────────── */
    float windowed[TLCS_MAX_FRAME_SIZE];
    tlcs_hamming_window(speech, windowed, n);

    float r[TLCS_LPC_ORDER_MAX + 1];
    tlcs_autocorrelation(windowed, n, r, order);

    float a[TLCS_LPC_ORDER_MAX + 1];
    tlcs_levinson(r, order, a, NULL);

    /* DIAG: save unquantized LPC */
    memcpy(diag->a_unquant, a, (size_t)(order + 1) * sizeof(float));

    float lsf[TLCS_LPC_ORDER_MAX];
    int lsf_ok = tlcs_lpc_to_lsf(a, order, lsf);
    if (lsf_ok != 0) {
        for (int32_t i = 0; i < order; i++)
            lsf[i] = (float)enc->prev_lsf[i] / 5000.0f;
    }
    tlcs_lsf_stabilize(lsf, order);

    /* Predictive LSF quantization */
    float lsf_pred[TLCS_LPC_ORDER_MAX];
    for (int32_t i = 0; i < order; i++)
        lsf_pred[i] = (float)enc->prev_lsf[i] / 5000.0f;

    int16_t lsf_indices[TLCS_LPC_ORDER_MAX];
    tlcs_lsf_quantize_pred(lsf, lsf_pred, order, lsf_indices);

    float lsf_q[TLCS_LPC_ORDER_MAX];
    tlcs_lsf_dequantize_pred(lsf_indices, lsf_pred, order, lsf_q);
    tlcs_lsf_stabilize(lsf_q, order);

    float a_q[TLCS_LPC_ORDER_MAX + 1];
    tlcs_lsf_to_lpc(lsf_q, order, a_q);

    /* DIAG: save quantized LPC + spectral distortion */
    memcpy(diag->a_quant, a_q, (size_t)(order + 1) * sizeof(float));
    diag->metrics.lsf_sd = compute_spectral_distortion(a, a_q, order, 256);

    /* ── Step 3: Compute LPC residual ─────────────────────────── */
    float residual[TLCS_MAX_FRAME_SIZE];
    float ana_mem[TLCS_LPC_ORDER_MAX];
    for (int32_t i = 0; i < order; i++)
        ana_mem[i] = (float)enc->synth_mem[i];
    tlcs_analysis_filter(a_q, order, speech, residual, n, ana_mem);
    for (int32_t i = 0; i < order; i++)
        enc->synth_mem[i] = (int16_t)ana_mem[i];

    /* DIAG: save residual */
    memcpy(diag->residual, residual, (size_t)n * sizeof(float));

    /* Compute residual energy */
    float res_energy = 0.0f;
    for (int32_t i = 0; i < n; i++)
        res_energy += residual[i] * residual[i];
    diag->metrics.residual_energy = res_energy;

    /* ── Step 4: Load excitation buffer ───────────────────────── */
    float *exc = enc->exc_buf;
    int32_t exc_total = TLCS_MAX_PITCH_LAG + n;

    tlcs_exc_buf_shift(exc, n);

    /* Copy residual into exc for pitch search */
    for (int32_t i = 0; i < n; i++)
        exc[TLCS_MAX_PITCH_LAG + i] = residual[i];

    /* ── Step 4b: Update speech history buffer for OL pitch ───── */
    memmove(enc->speech_buf,
            enc->speech_buf + n,
            (size_t)TLCS_MAX_PITCH_LAG * sizeof(float));
    memcpy(enc->speech_buf + TLCS_MAX_PITCH_LAG,
           speech, (size_t)n * sizeof(float));

    /* ── Step 5: Open-loop pitch detection on speech signal ───── */
    int32_t min_lag = TLCS_MIN_PITCH_LAG;
    int32_t max_lag = TLCS_MAX_PITCH_LAG;

    int32_t half = n / 2;
    float voicing1 = 0.0f;
    int32_t ol_lag1 = tlcs_pitch_ol_search(enc->speech_buf, half, min_lag, max_lag, &voicing1);
    float voicing2 = 0.0f;
    int32_t ol_lag2 = tlcs_pitch_ol_search(enc->speech_buf + half, half, min_lag, max_lag, &voicing2);
    float voicing = (voicing1 + voicing2) * 0.5f;
    int32_t ol_lag = ol_lag1;

    /* DIAG: save voicing */
    diag->metrics.voicing = voicing;

    /* Configure algebraic codebook: 8 pulses */
    tlcs_cb_config cb_cfg;
    tlcs_cb_config_init(&cb_cfg, subfr, TLCS_CB_NUM_PULSES);

    /* Compute weighted impulse response and BW-expanded coefficients */
    float h_w[TLCS_MAX_SUBFR_SIZE];
    tlcs_cb_impulse_response(a_q, order, h_w, subfr);

    float a_g1[TLCS_LPC_ORDER_MAX + 1];
    float a_g2[TLCS_LPC_ORDER_MAX + 1];
    tlcs_lpc_weight_coeffs(a_q, order, TLCS_GAMMA1, a_g1);
    tlcs_lpc_weight_coeffs(a_q, order, TLCS_GAMMA2, a_g2);

    /* H_w filter state */
    float hw_synth_mem[TLCS_LPC_ORDER_MAX];
    float hw_wgt_mem[TLCS_LPC_ORDER_MAX];
    for (int32_t i = 0; i < order; i++) {
        hw_synth_mem[i] = (float)enc->wgt_synth_mem[i];
        hw_wgt_mem[i]   = (float)enc->wgt_mem[i];
    }

    /* ── Step 6: Per-subframe processing (analysis-by-synthesis) ── */
    int32_t pitch_lags[TLCS_SUBFRAMES];
    int32_t pitch_fracs[TLCS_SUBFRAMES];
    int32_t pitch_gain_indices[TLCS_SUBFRAMES];
    tlcs_cb_entry cb_entries[TLCS_SUBFRAMES];

    float total_pitch_energy = 0.0f;
    float total_cb_energy = 0.0f;
    float total_error_energy = 0.0f;

    for (int32_t sf = 0; sf < n_subfr; sf++) {
        int32_t sf_offset = sf * subfr;
        int32_t exc_offset = TLCS_MAX_PITCH_LAG + sf_offset;

        float *target = &residual[sf_offset];

        /* Save H_w state */
        float synth_mem_save[TLCS_LPC_ORDER_MAX];
        memcpy(synth_mem_save, hw_synth_mem,
               (size_t)order * sizeof(float));

        /* ── 6a: Compute ZIR of H_w ── */
        float zir_synth[TLCS_MAX_SUBFR_SIZE];
        float zero_input[TLCS_MAX_SUBFR_SIZE];
        memset(zero_input, 0, (size_t)subfr * sizeof(float));
        float sm_copy[TLCS_LPC_ORDER_MAX];
        memcpy(sm_copy, hw_synth_mem, (size_t)order * sizeof(float));
        tlcs_synthesis_filter(a_q, order, zero_input,
                              zir_synth, subfr, sm_copy);

        float zir_fir[TLCS_MAX_SUBFR_SIZE];
        for (int32_t j = 0; j < subfr; j++) {
            zir_fir[j] = zir_synth[j];
            for (int32_t k = 1; k <= order; k++) {
                float past;
                if (j - k >= 0)
                    past = zir_synth[j - k];
                else
                    past = synth_mem_save[k - 1 - j];
                zir_fir[j] += a_g1[k] * past;
            }
        }

        float zir[TLCS_MAX_SUBFR_SIZE];
        float wm_copy[TLCS_LPC_ORDER_MAX];
        memcpy(wm_copy, hw_wgt_mem, (size_t)order * sizeof(float));
        tlcs_synthesis_filter(a_g2, order, zir_fir,
                              zir, subfr, wm_copy);

        /* ── 6b: Weighted target ── */
        float w_target[TLCS_MAX_SUBFR_SIZE];
        for (int32_t j = 0; j < subfr; j++) {
            w_target[j] = zir[j];
            for (int32_t k = 0; k <= j; k++)
                w_target[j] += target[k] * h_w[j - k];
        }

        /* ── 6c: Fractional closed-loop pitch search ── */
        int32_t cl_lag, cl_frac;
        float pitch_gain;
        tlcs_pitch_cl_search_frac(w_target, h_w, exc, exc_offset,
                                   subfr, ol_lag, TLCS_PITCH_CL_DELTA,
                                   min_lag, max_lag,
                                   &cl_lag, &cl_frac, &pitch_gain);

        /* DIAG: save pre-quantization pitch gain */
        diag->metrics.pitch_gain_unquant[sf] = pitch_gain;

        int32_t gi = tlcs_pitch_gain_quantize(pitch_gain);
        float gq = tlcs_pitch_gain_dequantize(gi);

        /* DIAG: save quantized pitch gain + lag */
        diag->metrics.pitch_gain[sf] = gq;
        diag->metrics.pitch_lag[sf] = cl_lag;

        /* ── 6d: Extract adaptive vector with fractional lag ── */
        float adaptive_vec[TLCS_MAX_SUBFR_SIZE];
        tlcs_pitch_get_adaptive_vec_frac(exc, exc_offset, cl_lag, cl_frac,
                                          adaptive_vec, subfr);

        float w_adaptive[TLCS_MAX_SUBFR_SIZE];
        for (int32_t j = 0; j < subfr; j++) {
            w_adaptive[j] = 0.0f;
            for (int32_t k = 0; k <= j; k++)
                w_adaptive[j] += adaptive_vec[k] * h_w[j - k];
        }

        /* ── 6e: Innovation target in weighted domain ── */
        float w_innov[TLCS_MAX_SUBFR_SIZE];
        for (int32_t i = 0; i < subfr; i++)
            w_innov[i] = w_target[i] - gq * w_adaptive[i];

        /* ── 6f: Algebraic codebook search ── */
        tlcs_cb_search_weighted(w_innov, h_w, &cb_cfg, &cb_entries[sf]);

        /* Build quantized innovation vector */
        float innovation[TLCS_MAX_SUBFR_SIZE];
        tlcs_cb_build_innovation(&cb_entries[sf], &cb_cfg, innovation, subfr);

        /* DIAG: compute optimal FCB gain (before quantization)
         * optimal_gain = <w_innov, h_w*innov_raw> / <h_w*innov_raw, h_w*innov_raw>
         * But we already have quantized gain in cb_entries. We can compute
         * what the gain should be from unscaled innovation. */
        {
            /* Build unscaled innovation (gain=1) */
            float innov_raw[TLCS_MAX_SUBFR_SIZE];
            memset(innov_raw, 0, (size_t)subfr * sizeof(float));
            for (int32_t p = 0; p < cb_cfg.num_pulses; p++) {
                int32_t track = p;
                int32_t abs_pos = cb_entries[sf].pulse_pos[p] * cb_cfg.num_pulses + track;
                if (abs_pos >= 0 && abs_pos < subfr)
                    innov_raw[abs_pos] += (float)cb_entries[sf].pulse_sign[p];
            }

            /* Filter through h_w */
            float hw_innov[TLCS_MAX_SUBFR_SIZE];
            for (int32_t j = 0; j < subfr; j++) {
                hw_innov[j] = 0.0f;
                for (int32_t k = 0; k <= j; k++)
                    hw_innov[j] += innov_raw[k] * h_w[j - k];
            }

            float num = 0.0f, den = 0.0f;
            for (int32_t i = 0; i < subfr; i++) {
                num += w_innov[i] * hw_innov[i];
                den += hw_innov[i] * hw_innov[i];
            }
            float opt_gain = (den > 1e-10f) ? num / den : 0.0f;
            diag->metrics.cb_gain_unquant[sf] = opt_gain;
            diag->metrics.cb_gain[sf] = cb_entries[sf].gain;
        }

        /* DIAG: copy component excitations */
        for (int32_t i = 0; i < subfr; i++) {
            diag->pitch_exc[sf_offset + i] = gq * adaptive_vec[i];
            diag->cb_exc[sf_offset + i] = innovation[i];
        }

        /* ── 6g: Update excitation buffer ── */
        for (int32_t i = 0; i < subfr; i++)
            exc[exc_offset + i] = gq * adaptive_vec[i] + innovation[i];

        /* DIAG: copy combined excitation + energy decomposition */
        for (int32_t i = 0; i < subfr; i++) {
            diag->full_exc[sf_offset + i] = exc[exc_offset + i];

            float p_e = gq * adaptive_vec[i];
            float c_e = innovation[i];
            float err = target[i] - exc[exc_offset + i];
            total_pitch_energy += p_e * p_e;
            total_cb_energy += c_e * c_e;
            total_error_energy += err * err;
        }

        /* ── 6h: Update H_w state with excitation error ── */
        float err[TLCS_MAX_SUBFR_SIZE];
        for (int32_t i = 0; i < subfr; i++)
            err[i] = target[i] - exc[exc_offset + i];

        float err_synth[TLCS_MAX_SUBFR_SIZE];
        tlcs_synthesis_filter(a_q, order, err, err_synth,
                              subfr, hw_synth_mem);

        float err_fir[TLCS_MAX_SUBFR_SIZE];
        for (int32_t j = 0; j < subfr; j++) {
            err_fir[j] = err_synth[j];
            for (int32_t k = 1; k <= order; k++) {
                float past;
                if (j - k >= 0)
                    past = err_synth[j - k];
                else
                    past = synth_mem_save[k - 1 - j];
                err_fir[j] += a_g1[k] * past;
            }
        }

        float err_wgt[TLCS_MAX_SUBFR_SIZE];
        tlcs_synthesis_filter(a_g2, order, err_fir, err_wgt,
                              subfr, hw_wgt_mem);

        pitch_lags[sf] = cl_lag;
        pitch_fracs[sf] = cl_frac;
        pitch_gain_indices[sf] = gi;
        /* Use second-half OL pitch at half-frame boundary */
        if (sf == n_subfr / 2 - 1)
            ol_lag = ol_lag2;
        else
            ol_lag = cl_lag;
    }

    /* DIAG: save energy decomposition */
    diag->metrics.pitch_energy = total_pitch_energy;
    diag->metrics.cb_energy = total_cb_energy;
    diag->metrics.error_energy = total_error_energy;

    /* Save H_w state for next frame */
    for (int32_t i = 0; i < order; i++) {
        enc->wgt_synth_mem[i]  = (int16_t)hw_synth_mem[i];
        enc->wgt_mem[i]        = (int16_t)hw_wgt_mem[i];
    }

    /* ── Step 7: Clamp excitation buffer ── */
    for (int32_t i = 0; i < exc_total; i++) {
        if (exc[i] > 32767.0f) exc[i] = 32767.0f;
        if (exc[i] < -32768.0f) exc[i] = -32768.0f;
    }

    /* ── Step 9: Pack bitstream (bit-level) ── */
    tlcs_bs_writer bsw;
    tlcs_bs_writer_init(&bsw, bitstream_out, TLCS_MAX_FRAME_BYTES);

    for (int32_t i = 0; i < order; i++)
        tlcs_bs_write(&bsw, (uint32_t)(lsf_indices[i] & ((1 << TLCS_LSF_BITS) - 1)),
                      TLCS_LSF_BITS);

    int32_t prev_lag_idx_diag = 0;
    for (int32_t sf = 0; sf < n_subfr; sf++) {
        int32_t lag_idx_diag = tlcs_pitch_encode_lag(pitch_lags[sf],
                                                      pitch_fracs[sf]);
        if (sf == 0 || sf == n_subfr / 2) {
            tlcs_bs_write(&bsw, (uint32_t)lag_idx_diag, 9);
        } else {
            int32_t delta = lag_idx_diag - prev_lag_idx_diag;
            if (delta < -TLCS_PITCH_DELTA_OFFSET) delta = -TLCS_PITCH_DELTA_OFFSET;
            if (delta > TLCS_PITCH_DELTA_OFFSET - 1) delta = TLCS_PITCH_DELTA_OFFSET - 1;
            tlcs_bs_write(&bsw, (uint32_t)(delta + TLCS_PITCH_DELTA_OFFSET),
                          TLCS_PITCH_DELTA_BITS);
        }
        prev_lag_idx_diag = lag_idx_diag;

        tlcs_bs_write(&bsw, (uint32_t)pitch_gain_indices[sf],
                      TLCS_PITCH_GAIN_BITS);

        for (int32_t p = 0; p < cb_cfg.num_pulses; p++) {
            tlcs_bs_write(&bsw,
                          (uint32_t)cb_entries[sf].pulse_pos[p],
                          (int32_t)cb_cfg.pos_bits);
            tlcs_bs_write(&bsw,
                          (cb_entries[sf].pulse_sign[p] > 0) ? 1u : 0u, 1);
        }

        tlcs_bs_write(&bsw,
                      (uint32_t)cb_entries[sf].gain_index,
                      TLCS_FCB_GAIN_BITS);
    }

    *bytes_written = tlcs_bs_writer_flush(&bsw);

    /* Update state: save QUANTIZED LSFs for next frame's prediction */
    for (int32_t i = 0; i < order; i++)
        enc->prev_lsf[i] = (int16_t)(lsf_q[i] * 5000.0f);
    enc->prev_pitch_lag = (int16_t)pitch_lags[n_subfr - 1];
    enc->frame_count++;

    return TLCS_OK;
}
