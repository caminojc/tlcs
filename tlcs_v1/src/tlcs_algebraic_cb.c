/*
 * tlcs_algebraic_cb.c — Algebraic (fixed) codebook, ISPP design.
 *
 * 2 pulses, 4 tracks, 80-sample subframe.
 * Sequential pulse placement with iterative refinement.
 * Index encoding: per pulse = 5 bits position + 1 bit sign = 6 bits.
 * Total: 2 pulses x 6 bits = 12 bits per subframe.
 */
#include "tlcs_config.h"
#include "tlcs_algebraic_cb.h"
#include "tlcs_pitch.h"   /* tlcs_convolve */

#include <math.h>
#include <string.h>

/* ================================================================== */
/* Helpers                                                             */
/* ================================================================== */

/* Filter excitation through impulse response (causal convolution). */
static void filter_exc(const float *exc, const float *h, int N, float *out)
{
    tlcs_convolve(exc, h, N, out);
}

/* ================================================================== */
/* Index encoding/decoding                                             */
/* ================================================================== */

/*
 * Per pulse: 5 bits position-in-track (20 positions), 1 bit sign.
 * Pulse 0 on track 0, pulse 1 on track 1.
 * Layout: pulse0 in bits [0..5], pulse1 in bits [6..11].
 */

static void encode_index(const int *positions, const float *signs,
                         int num_pulses, int *out_index)
{
    int index = 0;
    for (int p = 0; p < num_pulses; p++) {
        int track = p % TLCS_ACB_NUM_TRACKS;
        int pos = positions[p];
        int pos_in_track = (pos - track) / TLCS_ACB_NUM_TRACKS;
        if (pos_in_track < 0) pos_in_track = 0;
        if (pos_in_track >= TLCS_ACB_POS_PER_TRACK) {
            pos_in_track = TLCS_ACB_POS_PER_TRACK - 1;
        }
        int sign_bit = (signs[p] < 0.0f) ? 1 : 0;
        int pulse_idx = (pos_in_track << 1) | sign_bit;
        index |= (pulse_idx << (p * 6));
    }
    *out_index = index;
}

static void decode_index(int index, int num_pulses,
                         int *positions, float *signs)
{
    for (int p = 0; p < num_pulses; p++) {
        int track = p % TLCS_ACB_NUM_TRACKS;
        int pulse_idx = (index >> (p * 6)) & 0x3F;
        int sign_bit = pulse_idx & 1;
        int pos_in_track = pulse_idx >> 1;
        if (pos_in_track >= TLCS_ACB_POS_PER_TRACK) {
            pos_in_track = TLCS_ACB_POS_PER_TRACK - 1;
        }
        positions[p] = track + pos_in_track * TLCS_ACB_NUM_TRACKS;
        signs[p] = sign_bit ? -1.0f : 1.0f;
    }
}

/* ================================================================== */
/* Search: sequential greedy + refinement                              */
/* ================================================================== */

void tlcs_acb_search(const float *target, const float *h,
                     int subframe_size,
                     int *out_index, float *out_gain, float *out_exc)
{
    int N = subframe_size;

    /* Pre-compute d[n] = <target, h_n> where h_n is h shifted by n */
    float d[TLCS_SUBFRAME_SIZE];
    for (int n = 0; n < N; n++) {
        float sum = 0.0f;
        for (int i = n; i < N; i++) {
            sum += target[i] * h[i - n];
        }
        d[n] = sum;
    }

    /* Determine pulse signs from target correlation */
    float signs_arr[TLCS_SUBFRAME_SIZE];
    for (int n = 0; n < N; n++) {
        signs_arr[n] = (d[n] >= 0.0f) ? 1.0f : -1.0f;
    }

    /* Build excitation pulse by pulse */
    float excitation[TLCS_SUBFRAME_SIZE];
    memset(excitation, 0, N * sizeof(float));

    int pulse_pos[TLCS_ACB_NUM_PULSES];
    float pulse_sign[TLCS_ACB_NUM_PULSES];

    float residual[TLCS_SUBFRAME_SIZE];
    memcpy(residual, target, N * sizeof(float));

    for (int p = 0; p < TLCS_ACB_NUM_PULSES; p++) {
        int track = p % TLCS_ACB_NUM_TRACKS;
        int best_pos = track;
        float best_score = -1e30f;

        /* Search all positions in this track */
        for (int k = 0; k < TLCS_ACB_POS_PER_TRACK; k++) {
            int pos = track + k * TLCS_ACB_NUM_TRACKS;
            if (pos >= N) break;

            float s = signs_arr[pos];
            float corr = 0.0f;
            float energy = 0.0f;
            for (int i = pos; i < N; i++) {
                float hv = h[i - pos];
                corr += residual[i] * hv;
                energy += hv * hv;
            }
            corr *= s;
            if (energy < 1e-10f) continue;
            float score = corr * corr / energy;
            if (score > best_score) {
                best_score = score;
                best_pos = pos;
            }
        }

        float s = signs_arr[best_pos];
        pulse_pos[p] = best_pos;
        pulse_sign[p] = s;

        /* Compute pulse gain and update residual */
        float corr_val = 0.0f, energy_val = 0.0f;
        for (int i = best_pos; i < N; i++) {
            float hv = h[i - best_pos];
            corr_val += residual[i] * hv;
            energy_val += hv * hv;
        }
        float pg = (s * corr_val) / (energy_val + 1e-10f);
        for (int i = best_pos; i < N; i++) {
            residual[i] -= pg * s * h[i - best_pos];
        }

        excitation[best_pos] += s;
    }

    /* ---- Refinement passes ---- */
    float filtered[TLCS_SUBFRAME_SIZE];

    for (int refine = 0; refine < 2; refine++) {
        for (int p = 0; p < TLCS_ACB_NUM_PULSES; p++) {
            int track = p % TLCS_ACB_NUM_TRACKS;
            int old_pos = pulse_pos[p];
            float old_sign = pulse_sign[p];

            /* Remove this pulse */
            excitation[old_pos] -= old_sign;

            /* Filter excitation without this pulse */
            filter_exc(excitation, h, N, filtered);

            /* Compute residual with current other pulses */
            float fw_energy = 0.0f;
            for (int i = 0; i < N; i++) {
                fw_energy += filtered[i] * filtered[i];
            }

            float resid[TLCS_SUBFRAME_SIZE];
            if (fw_energy > 1e-10f) {
                float g_tmp = 0.0f;
                for (int i = 0; i < N; i++) {
                    g_tmp += target[i] * filtered[i];
                }
                g_tmp /= fw_energy;
                for (int i = 0; i < N; i++) {
                    resid[i] = target[i] - g_tmp * filtered[i];
                }
            } else {
                memcpy(resid, target, N * sizeof(float));
            }

            /* Search best position for this pulse */
            int best_pos = old_pos;
            float best_score = -1e30f;

            for (int k = 0; k < TLCS_ACB_POS_PER_TRACK; k++) {
                int pos = track + k * TLCS_ACB_NUM_TRACKS;
                if (pos >= N) break;

                float s = signs_arr[pos];
                float corr = 0.0f, energy = 0.0f;
                for (int i = pos; i < N; i++) {
                    float hv = h[i - pos];
                    corr += resid[i] * hv;
                    energy += hv * hv;
                }
                corr *= s;
                if (energy < 1e-10f) continue;
                float score = corr * corr / energy;
                if (score > best_score) {
                    best_score = score;
                    best_pos = pos;
                }
            }

            pulse_pos[p] = best_pos;
            pulse_sign[p] = signs_arr[best_pos];
            excitation[best_pos] += signs_arr[best_pos];
        }
    }

    /* Compute overall gain: g = <target, H*c> / <H*c, H*c> */
    filter_exc(excitation, h, N, filtered);
    float corr_total = 0.0f, energy_total = 0.0f;
    float target_energy = 0.0f;
    for (int i = 0; i < N; i++) {
        corr_total += target[i] * filtered[i];
        energy_total += filtered[i] * filtered[i];
        target_energy += target[i] * target[i];
    }
    float gain = corr_total / (energy_total + 1e-10f);

    /* Energy compensation: with only 2 pulses, the MSE-optimal gain
     * underestimates the needed energy. Scale up to match target RMS.
     * This trades MSE for perceptual energy match. */
    if (energy_total > 1e-10f && target_energy > 1e-10f) {
        float synth_energy = gain * gain * energy_total;
        float energy_ratio = sqrtf(target_energy / (synth_energy + 1e-10f));
        /* Blend: 70% energy-matched, 30% MSE-optimal */
        gain *= (0.3f + 0.7f * energy_ratio);
    }

    /* Pack index and output */
    encode_index(pulse_pos, pulse_sign, TLCS_ACB_NUM_PULSES, out_index);
    *out_gain = gain;
    memcpy(out_exc, excitation, N * sizeof(float));
}

/* ================================================================== */
/* Decode: reconstruct excitation from index                           */
/* ================================================================== */

void tlcs_acb_decode(int index, int subframe_size, float *out_exc)
{
    int positions[TLCS_ACB_NUM_PULSES];
    float signs[TLCS_ACB_NUM_PULSES];

    memset(out_exc, 0, subframe_size * sizeof(float));
    decode_index(index, TLCS_ACB_NUM_PULSES, positions, signs);

    for (int p = 0; p < TLCS_ACB_NUM_PULSES; p++) {
        int pos = positions[p];
        if (pos >= 0 && pos < subframe_size) {
            out_exc[pos] += signs[p];
        }
    }
}
