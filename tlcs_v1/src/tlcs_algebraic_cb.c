/*
 * tlcs_algebraic_cb.c — Algebraic (fixed) codebook, ISPP with Phi-based search.
 *
 * 3 pulses, 5 tracks, 80-sample subframe.
 * Uses SMPL-style num/den recurrence with autocorrelation matrix Phi.
 * Returns weighted-domain gain for dB quantization.
 */
#include "tlcs_config.h"
#include "tlcs_algebraic_cb.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>

/* ================================================================== */
/* Index encoding/decoding                                              */
/* ================================================================== */

static void encode_index(const int *positions, const float *signs,
                         int num_pulses, int *out_index)
{
    int index = 0;
    for (int p = 0; p < num_pulses; p++) {
        int track = p % TLCS_ACB_NUM_TRACKS;
        int pos = positions[p];
        int pos_in_track = (pos - track) / TLCS_ACB_NUM_TRACKS;
        if (pos_in_track < 0) pos_in_track = 0;
        if (pos_in_track >= TLCS_ACB_POS_PER_TRACK)
            pos_in_track = TLCS_ACB_POS_PER_TRACK - 1;
        int sign_bit = (signs[p] < 0.0f) ? 1 : 0;
        int bits_per = TLCS_ACB_POS_BITS + 1;
        int pulse_idx = (pos_in_track << 1) | sign_bit;
        index |= (pulse_idx << (p * bits_per));
    }
    *out_index = index;
}

static void decode_index(int index, int num_pulses,
                         int *positions, float *signs)
{
    for (int p = 0; p < num_pulses; p++) {
        int track = p % TLCS_ACB_NUM_TRACKS;
        int bits_per = TLCS_ACB_POS_BITS + 1;
        int mask = (1 << bits_per) - 1;
        int pulse_idx = (index >> (p * bits_per)) & mask;
        int sign_bit = pulse_idx & 1;
        int pos_in_track = pulse_idx >> 1;
        if (pos_in_track >= TLCS_ACB_POS_PER_TRACK)
            pos_in_track = TLCS_ACB_POS_PER_TRACK - 1;
        positions[p] = track + pos_in_track * TLCS_ACB_NUM_TRACKS;
        signs[p] = sign_bit ? -1.0f : 1.0f;
    }
}

/* ================================================================== */
/* FCB Search — Phi-based (SMPL-style num/den recurrence)               */
/* ================================================================== */

void tlcs_acb_search(const float *target, const float *h,
                     int subframe_size,
                     int *out_index, float *out_gain, float *out_exc)
{
    int N = subframe_size;

    /* Precompute Phi: autocorrelation of impulse response h.
     * Phi[k] = sum_{n=0}^{N-1-k} h[n] * h[n+k]  */
    float Phi[TLCS_SUBFRAME_SIZE];
    for (int k = 0; k < N; k++) {
        float sum = 0.0f;
        for (int n = 0; n < N - k; n++) sum += h[n] * h[n + k];
        Phi[k] = sum;
    }

    /* Precompute d[n] = <target, h_n> (cross-correlation with shifted IR) */
    float d[TLCS_SUBFRAME_SIZE];
    float d_sign[TLCS_SUBFRAME_SIZE];
    float d_abs[TLCS_SUBFRAME_SIZE];
    for (int n = 0; n < N; n++) {
        float sum = 0.0f;
        for (int i = n; i < N; i++) sum += target[i] * h[i - n];
        d[n] = sum;
        d_sign[n] = (d[n] >= 0.0f) ? 1.0f : -1.0f;
        d_abs[n] = fabsf(d[n]);
    }

    /* Initialize num/den for first pulse */
    float num[TLCS_SUBFRAME_SIZE];
    float den[TLCS_SUBFRAME_SIZE];
    for (int n = 0; n < N; n++) {
        num[n] = d_abs[n];
        den[n] = Phi[0] + 1e-16f;  /* self-energy of single pulse */
    }

    float excitation[TLCS_SUBFRAME_SIZE];
    memset(excitation, 0, N * sizeof(float));

    int positions[TLCS_ACB_NUM_PULSES];
    float signs[TLCS_ACB_NUM_PULSES];
    float best_gain = 0.0f;

    for (int p = 0; p < TLCS_ACB_NUM_PULSES; p++) {
        int track = p % TLCS_ACB_NUM_TRACKS;

        /* Find best position in this track: maximize Q = num^2 / den */
        int best_pos = track;
        float best_Q = -1e30f;

        for (int k = 0; k < TLCS_ACB_POS_PER_TRACK; k++) {
            int pos = track + k * TLCS_ACB_NUM_TRACKS;
            if (pos >= N) break;

            float Q = (num[pos] * num[pos]) / (den[pos] + 1e-16f);
            if (Q > best_Q) {
                best_Q = Q;
                best_pos = pos;
            }
        }

        positions[p] = best_pos;
        signs[p] = d_sign[best_pos];
        excitation[best_pos] += signs[p];

        /* Compute gain so far: sum of correlations / total energy */
        best_gain = num[best_pos] / (den[best_pos] + 1e-16f);

        /* Update num/den for next pulse using Phi cross-terms */
        if (p < TLCS_ACB_NUM_PULSES - 1) {
            float sgn_p = signs[p];
            for (int n = 0; n < N; n++) {
                int diff = abs(n - best_pos);
                if (diff < N) {
                    /* Cross-correlation between new pulse and position n */
                    float cross = Phi[diff];
                    /* num: add the abs correlation of the placed pulse
                     * This makes subsequent pulses aware of earlier ones */
                    num[n] += d_abs[best_pos];
                    /* den: add energy cross-term (accounts for pulse interaction) */
                    den[n] += 2.0f * sgn_p * d_sign[n] * cross + Phi[0];
                }
            }
        }
    }

    /* Final gain: recompute from full excitation for accuracy */
    {
        float filtered[TLCS_SUBFRAME_SIZE];
        memset(filtered, 0, N * sizeof(float));
        for (int i = 0; i < N; i++) {
            if (excitation[i] != 0.0f) {
                for (int j = i; j < N; j++) {
                    filtered[j] += excitation[i] * h[j - i];
                }
            }
        }
        float corr = 0.0f, nrg = 0.0f;
        for (int i = 0; i < N; i++) {
            corr += target[i] * filtered[i];
            nrg += filtered[i] * filtered[i];
        }
        best_gain = corr / (nrg + 1e-10f);
    }

    /* Pack index and output */
    encode_index(positions, signs, TLCS_ACB_NUM_PULSES, out_index);
    *out_gain = best_gain;  /* weighted-domain gain — pass to dB quantizer */
    memcpy(out_exc, excitation, N * sizeof(float));
}

/* ================================================================== */
/* Decode: reconstruct excitation from index                           */
/* ================================================================== */

void tlcs_acb_decode(int index, int subframe_size, float *out_exc)
{
    int positions[TLCS_ACB_NUM_PULSES];
    float signs[TLCS_ACB_NUM_PULSES];
    decode_index(index, TLCS_ACB_NUM_PULSES, positions, signs);

    memset(out_exc, 0, (size_t)subframe_size * sizeof(float));
    for (int p = 0; p < TLCS_ACB_NUM_PULSES; p++) {
        int pos = positions[p];
        if (pos >= 0 && pos < subframe_size) {
            out_exc[pos] += signs[p];
        }
    }
}
