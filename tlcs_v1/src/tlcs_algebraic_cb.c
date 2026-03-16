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
                         int num_pulses, int num_tracks, int *out_lo, int *out_hi)
{
    /* Pack pulses x 6 bits into two 24-bit ints.
     * Pulses 0-3 go into lo (bits 0-23), pulses 4-7 go into hi (bits 0-23). */
    int lo = 0, hi = 0;
    for (int p = 0; p < num_pulses; p++) {
        int track = p % num_tracks;
        int pos = positions[p];
        int pos_per_track = TLCS_SUBFRAME_SIZE / num_tracks;
        int pos_in_track = (pos - track) / num_tracks;
        if (pos_in_track < 0) pos_in_track = 0;
        if (pos_in_track >= pos_per_track)
            pos_in_track = pos_per_track - 1;
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

static void decode_index(int lo, int hi, int num_pulses, int num_tracks,
                         int *positions, float *signs)
{
    int pos_per_track = TLCS_SUBFRAME_SIZE / num_tracks;
    for (int p = 0; p < num_pulses; p++) {
        int track = p % num_tracks;
        int packed;
        if (p < 4) {
            packed = (lo >> (p * 6)) & 0x3F;
        } else {
            packed = (hi >> ((p - 4) * 6)) & 0x3F;
        }
        int sign_bit = packed & 1;
        int pos_in_track = packed >> 1;
        if (pos_in_track >= pos_per_track)
            pos_in_track = pos_per_track - 1;
        positions[p] = track + pos_in_track * num_tracks;
        signs[p] = sign_bit ? -1.0f : 1.0f;
    }
}

/* ================================================================== */
/* FCB Search — Phi-based (SMPL-style num/den recurrence)               */
/* ================================================================== */

void tlcs_acb_search_n(const float *target, const float *h,
                       int subframe_size, int num_pulses,
                       int *out_index_lo, int *out_index_hi,
                       float *out_gain, float *out_exc)
{
    int N = subframe_size;
    int num_tracks = num_pulses;  /* tracks == pulses (each pulse gets its own track) */
    int pos_per_track = N / num_tracks;

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

    int positions[8];  /* max 8 pulses */
    float signs[8];

    for (int p = 0; p < num_pulses; p++) {
        int track = p % num_tracks;

        /* Find best position in this track: maximize Q = num^2 / den */
        int best_pos = track;
        float best_Q = -1e30f;

        for (int k = 0; k < pos_per_track; k++) {
            int pos = track + k * num_tracks;
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
        if (p < num_pulses - 1) {
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
    encode_index(positions, signs, num_pulses, num_tracks,
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

void tlcs_acb_decode_n(int index_lo, int index_hi,
                       int subframe_size, int num_pulses, float *out_exc)
{
    int num_tracks = num_pulses;
    int positions[8];
    float signs[8];
    decode_index(index_lo, index_hi, num_pulses, num_tracks, positions, signs);

    memset(out_exc, 0, (size_t)subframe_size * sizeof(float));
    for (int p = 0; p < num_pulses; p++) {
        int pos = positions[p];
        if (pos >= 0 && pos < subframe_size) {
            out_exc[pos] += signs[p];
        }
    }
}
