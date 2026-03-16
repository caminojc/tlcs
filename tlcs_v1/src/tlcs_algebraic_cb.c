/*
 * tlcs_algebraic_cb.c — Algebraic (fixed) codebook, ISPP with Phi-based search.
 *
 * 8 pulses, 8 tracks, 160-sample subframe.
 * Uses SMPL-style num/den recurrence with autocorrelation matrix Phi.
 * Returns weighted-domain gain for dB quantization.
 *
 * Index encoding: 8 pulses x 6 bits = 48 bits total.
 *   Each pulse: 5 position bits + 1 sign bit.
 *   Packed into two 24-bit ints (lo = pulses 0-3, hi = pulses 4-7).
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
                         int num_pulses, int *out_lo, int *out_hi)
{
    /* Pack 8 pulses x 6 bits = 48 bits into two 24-bit ints.
     * Pulses 0-3 go into lo (bits 0-23), pulses 4-7 go into hi (bits 0-23). */
    int lo = 0, hi = 0;
    for (int p = 0; p < num_pulses; p++) {
        int track = p % TLCS_ACB_NUM_TRACKS;
        int pos = positions[p];
        int pos_in_track = (pos - track) / TLCS_ACB_NUM_TRACKS;
        if (pos_in_track < 0) pos_in_track = 0;
        if (pos_in_track >= TLCS_ACB_POS_PER_TRACK)
            pos_in_track = TLCS_ACB_POS_PER_TRACK - 1;
        int sign_bit = (signs[p] < 0.0f) ? 1 : 0;
        int pulse_idx = (pos_in_track << 1) | sign_bit;  /* 6 bits */

        if (p < 4) {
            lo |= (pulse_idx << (p * 6));
        } else {
            hi |= (pulse_idx << ((p - 4) * 6));
        }
    }
    *out_lo = lo;
    *out_hi = hi;
}

static void decode_index(int lo, int hi, int num_pulses,
                         int *positions, float *signs)
{
    for (int p = 0; p < num_pulses; p++) {
        int track = p % TLCS_ACB_NUM_TRACKS;
        int packed;
        if (p < 4) {
            packed = (lo >> (p * 6)) & 0x3F;
        } else {
            packed = (hi >> ((p - 4) * 6)) & 0x3F;
        }
        int sign_bit = packed & 1;
        int pos_in_track = packed >> 1;
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
                     int *out_index_lo, int *out_index_hi,
                     float *out_gain, float *out_exc)
{
    int N = subframe_size;

    /* Precompute Phi: autocorrelation of impulse response h.
     * Phi[k] = sum_{n=0}^{N-1-k} h[n] * h[n+k]  */
    float *Phi = (float *)malloc((size_t)N * sizeof(float));
    for (int k = 0; k < N; k++) {
        float sum = 0.0f;
        for (int n = 0; n < N - k; n++) sum += h[n] * h[n + k];
        Phi[k] = sum;
    }

    /* Precompute d[n] = <target, h_n> (cross-correlation with shifted IR) */
    float *d = (float *)malloc((size_t)N * sizeof(float));
    float *d_sign = (float *)malloc((size_t)N * sizeof(float));
    float *d_abs = (float *)malloc((size_t)N * sizeof(float));
    for (int n = 0; n < N; n++) {
        float sum = 0.0f;
        for (int i = n; i < N; i++) sum += target[i] * h[i - n];
        d[n] = sum;
        d_sign[n] = (d[n] >= 0.0f) ? 1.0f : -1.0f;
        d_abs[n] = fabsf(d[n]);
    }

    /* Initialize num/den for first pulse */
    float *num = (float *)malloc((size_t)N * sizeof(float));
    float *den = (float *)malloc((size_t)N * sizeof(float));
    for (int n = 0; n < N; n++) {
        num[n] = d_abs[n];
        den[n] = Phi[0] + 1e-16f;  /* self-energy of single pulse */
    }

    float *excitation = (float *)calloc((size_t)N, sizeof(float));

    int positions[TLCS_ACB_NUM_PULSES];
    float signs[TLCS_ACB_NUM_PULSES];

    /* MLOW-style search: no track constraint, global best position per pulse.
     * Each pulse placed at the position maximizing Q = num^2 / den.
     * num/den updated with exact Phi cross-terms after each placement. */
    for (int p = 0; p < TLCS_ACB_NUM_PULSES; p++) {
        /* Find GLOBAL best position (no track constraint) */
        int best_pos = 0;
        float best_Q = -1e30f;
        for (int pos = 0; pos < N; pos++) {
            float Q = (num[pos] * num[pos]) / (den[pos] + 1e-16f);
            if (Q > best_Q) {
                best_Q = Q;
                best_pos = pos;
            }
        }

        positions[p] = best_pos;
        signs[p] = d_sign[best_pos];
        excitation[best_pos] += signs[p];

        /* Update num/den for next pulse (MLOW-style exact update) */
        if (p < TLCS_ACB_NUM_PULSES - 1) {
            float sgn_p = signs[p];
            /* num: add absolute correlation of placed pulse to all positions */
            for (int n = 0; n < N; n++) {
                num[n] += d_abs[best_pos];
            }
            /* den: add cross-terms from Phi column at best_pos */
            /* d_den = constant offset from all previous pulses interacting with new one */
            float d_den = 0.0f;
            for (int prev = 0; prev < p; prev++) {
                int diff = abs(positions[prev] - best_pos);
                if (diff < N) d_den += Phi[diff] * signs[prev];
            }
            d_den *= 2.0f * sgn_p;
            d_den += Phi[0]; /* self-energy of new pulse */
            /* Add constant part to all positions */
            for (int n = 0; n < N; n++) {
                den[n] += d_den;
            }
            /* Add position-dependent part: 2 * sgn_p * sign[n] * Phi[|n-best_pos|] */
            for (int n = 0; n < N; n++) {
                int diff = abs(n - best_pos);
                if (diff < N) {
                    den[n] += 2.0f * sgn_p * d_sign[n] * Phi[diff];
                }
            }
        }
    }

    /* Final gain: recompute from full excitation for accuracy */
    {
        float *filtered = (float *)calloc((size_t)N, sizeof(float));
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
        *out_gain = corr / (nrg + 1e-10f);
        free(filtered);
    }

    /* Pack index and output */
    encode_index(positions, signs, TLCS_ACB_NUM_PULSES,
                 out_index_lo, out_index_hi);
    memcpy(out_exc, excitation, (size_t)N * sizeof(float));

    free(Phi);
    free(d);
    free(d_sign);
    free(d_abs);
    free(num);
    free(den);
    free(excitation);
}

/* ================================================================== */
/* Decode: reconstruct excitation from index                           */
/* ================================================================== */

void tlcs_acb_decode(int index_lo, int index_hi,
                     int subframe_size, float *out_exc)
{
    int positions[TLCS_ACB_NUM_PULSES];
    float signs[TLCS_ACB_NUM_PULSES];
    decode_index(index_lo, index_hi, TLCS_ACB_NUM_PULSES, positions, signs);

    memset(out_exc, 0, (size_t)subframe_size * sizeof(float));
    for (int p = 0; p < TLCS_ACB_NUM_PULSES; p++) {
        int pos = positions[p];
        if (pos >= 0 && pos < subframe_size) {
            out_exc[pos] += signs[p];
        }
    }
}
