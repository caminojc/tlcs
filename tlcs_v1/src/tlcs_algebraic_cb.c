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

        /* Update num/den for next pulse using Phi cross-terms */
        if (p < TLCS_ACB_NUM_PULSES - 1) {
            float sgn_p = signs[p];
            for (int n = 0; n < N; n++) {
                int diff = abs(n - best_pos);
                if (diff < N) {
                    /* Cross-correlation between new pulse and position n */
                    float cross = Phi[diff];
                    num[n] += d_abs[best_pos];
                    den[n] += 2.0f * sgn_p * d_sign[n] * cross + Phi[0];
                }
            }
        }
    }

    /* ---- Iterative refinement: reposition each pulse with others fixed ---- */
    /* This is the key quality improvement over pure greedy search.
     * MLOW uses delayed-decision; this is a simpler approximation. */
    for (int refine = 0; refine < 3; refine++) {
        for (int p = 0; p < TLCS_ACB_NUM_PULSES; p++) {
            int track = p % TLCS_ACB_NUM_TRACKS;
            int old_pos = positions[p];
            float old_sign = signs[p];

            /* Remove this pulse */
            excitation[old_pos] -= old_sign;

            /* Filter remaining excitation through h */
            float *filt_rest = (float *)calloc((size_t)N, sizeof(float));
            for (int i = 0; i < N; i++) {
                if (excitation[i] != 0.0f) {
                    for (int j = i; j < N; j++)
                        filt_rest[j] += excitation[i] * h[j - i];
                }
            }

            /* Compute residual target */
            float rest_nrg = 0.0f;
            for (int i = 0; i < N; i++) rest_nrg += filt_rest[i] * filt_rest[i];
            float g_rest = 0.0f;
            if (rest_nrg > 1e-10f) {
                float rest_corr = 0.0f;
                for (int i = 0; i < N; i++) rest_corr += target[i] * filt_rest[i];
                g_rest = rest_corr / rest_nrg;
            }
            float resid[160]; /* TLCS_SUBFRAME_SIZE */
            for (int i = 0; i < N; i++)
                resid[i] = target[i] - g_rest * filt_rest[i];

            /* Search best position for this pulse in its track */
            int best_pos = old_pos;
            float best_score = -1e30f;
            for (int k = 0; k < TLCS_ACB_POS_PER_TRACK; k++) {
                int pos = track + k * TLCS_ACB_NUM_TRACKS;
                if (pos >= N) break;
                float s = d_sign[pos];
                float c = 0.0f, e = 0.0f;
                for (int i = pos; i < N; i++) {
                    c += resid[i] * h[i - pos];
                    e += h[i - pos] * h[i - pos];
                }
                c *= s;
                if (e < 1e-10f) continue;
                float score = c * c / e;
                if (score > best_score) {
                    best_score = score;
                    best_pos = pos;
                }
            }

            positions[p] = best_pos;
            signs[p] = d_sign[best_pos];
            excitation[best_pos] += signs[p];
            free(filt_rest);
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
