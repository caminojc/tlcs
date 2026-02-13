#include "tlcs_codebook.h"
#include "tlcs_lpc.h"
#include <string.h>
#include <math.h>

/* ══════════════════════════════════════════════════════════════════
 *  Codebook Configuration
 * ══════════════════════════════════════════════════════════════════ */

void tlcs_cb_config_init(tlcs_cb_config *cfg,
                          int32_t subfr_size, int32_t num_pulses)
{
    cfg->num_pulses = num_pulses;
    cfg->subfr_size = subfr_size;
    cfg->positions_per_track = subfr_size / num_pulses;

    /* Bits needed: ceil(log2(positions_per_track)) */
    int32_t ppt = cfg->positions_per_track;
    int32_t bits = 0;
    int32_t v = 1;
    while (v < ppt) { bits++; v <<= 1; }
    cfg->pos_bits = bits;
}

/* ══════════════════════════════════════════════════════════════════
 *  Weighted Synthesis Impulse Response
 *
 *  H_w(z) = A(z/γ₁) / (A(z) · A(z/γ₂))
 *
 *  Computed as cascade:
 *    δ(n) → 1/A(z) → A(z/γ₁) → 1/A(z/γ₂)
 * ══════════════════════════════════════════════════════════════════ */

void tlcs_cb_impulse_response_gamma(const float *a_q, int32_t order,
                                     float *h_w, int32_t subfr_size,
                                     float gamma1, float gamma2)
{
    /* Bandwidth-expanded coefficients */
    float a_g1[TLCS_LPC_ORDER_MAX + 1];
    float a_g2[TLCS_LPC_ORDER_MAX + 1];
    tlcs_lpc_weight_coeffs(a_q, order, gamma1, a_g1);
    tlcs_lpc_weight_coeffs(a_q, order, gamma2, a_g2);

    /* Step 1: Impulse response of 1/A(z) */
    float h[TLCS_MAX_SUBFR_SIZE];
    h[0] = 1.0f;
    for (int32_t n = 1; n < subfr_size; n++) {
        h[n] = 0.0f;
        int32_t kmax = (n < order) ? n : order;
        for (int32_t k = 1; k <= kmax; k++)
            h[n] -= a_q[k] * h[n - k];
    }

    /* Step 2: Apply FIR filter A(z/g1) */
    float h_prime[TLCS_MAX_SUBFR_SIZE];
    for (int32_t n = 0; n < subfr_size; n++) {
        h_prime[n] = h[n];  /* a_g1[0] = 1.0 */
        int32_t kmax = (n < order) ? n : order;
        for (int32_t k = 1; k <= kmax; k++)
            h_prime[n] += a_g1[k] * h[n - k];
    }

    /* Step 3: Apply IIR filter 1/A(z/g2) */
    h_w[0] = h_prime[0];
    for (int32_t n = 1; n < subfr_size; n++) {
        h_w[n] = h_prime[n];
        int32_t kmax = (n < order) ? n : order;
        for (int32_t k = 1; k <= kmax; k++)
            h_w[n] -= a_g2[k] * h_w[n - k];
    }
}

void tlcs_cb_impulse_response(const float *a_q, int32_t order,
                               float *h_w, int32_t subfr_size)
{
    tlcs_cb_impulse_response_gamma(a_q, order, h_w, subfr_size,
                                    TLCS_GAMMA1, TLCS_GAMMA2);
}

/* ══════════════════════════════════════════════════════════════════
 *  Algebraic Codebook Search
 *
 *  Greedy pulse placement using backward-filtered target.
 *
 *  1. Filter innovation target through H_w(z) → weighted target x(n)
 *  2. Backward-filter x(n) with h_w → d(n) (correlation at each pos)
 *  3. For each track, place pulse at position with max |d(pos)|
 *  4. Set sign = sign(d(pos))
 *  5. Compute optimal gain from filtered codebook vector
 * ══════════════════════════════════════════════════════════════════ */

void tlcs_cb_search(const float *target, const float *h_w,
                    const tlcs_cb_config *cfg, tlcs_cb_entry *entry)
{
    int32_t L = cfg->subfr_size;
    int32_t N = cfg->num_pulses;
    int32_t M = cfg->positions_per_track;

    /* Step 1: Weighted target x(n) = target convolved with h_w */
    float x[TLCS_MAX_SUBFR_SIZE];
    for (int32_t n = 0; n < L; n++) {
        x[n] = 0.0f;
        for (int32_t k = 0; k <= n; k++)
            x[n] += target[k] * h_w[n - k];
    }

    /* Step 2: Iterative (successive cancellation) pulse placement.
     * After placing each pulse, subtract its filtered contribution
     * from x so subsequent pulses account for earlier ones. */
    for (int32_t k = 0; k < N; k++) {
        /* Backward-filter current x for this track's positions */
        float best_abs = -1.0f;
        int32_t best_m = 0;
        float best_d = 0.0f;
        for (int32_t m = 0; m < M; m++) {
            int32_t pos = k + m * N;
            if (pos >= L) break;
            /* d(pos) = correlation of x with h_w shifted to pos */
            float dval = 0.0f;
            for (int32_t i = pos; i < L; i++)
                dval += x[i] * h_w[i - pos];
            float ad = fabsf(dval);
            if (ad > best_abs) {
                best_abs = ad;
                best_m = m;
                best_d = dval;
            }
        }
        entry->pulse_pos[k] = best_m;
        entry->pulse_sign[k] = (best_d >= 0.0f) ? 1 : -1;

        /* Subtract this pulse's filtered contribution from x */
        int32_t abs_pos = k + best_m * N;
        float sign_f = (float)entry->pulse_sign[k];
        for (int32_t n = abs_pos; n < L; n++)
            x[n] -= sign_f * h_w[n - abs_pos];
    }

    /* Step 3: Build unit codebook vector and filter through H_w */
    float c[TLCS_MAX_SUBFR_SIZE];
    memset(c, 0, (size_t)L * sizeof(float));
    for (int32_t k = 0; k < N; k++) {
        int32_t abs_pos = k + entry->pulse_pos[k] * N;
        if (abs_pos < L)
            c[abs_pos] = (float)entry->pulse_sign[k];
    }

    /* Recompute x from original target for gain calculation */
    for (int32_t n = 0; n < L; n++) {
        x[n] = 0.0f;
        for (int32_t k = 0; k <= n; k++)
            x[n] += target[k] * h_w[n - k];
    }

    float Hc[TLCS_MAX_SUBFR_SIZE];
    for (int32_t n = 0; n < L; n++) {
        Hc[n] = 0.0f;
        for (int32_t i = 0; i <= n; i++)
            Hc[n] += c[i] * h_w[n - i];
    }

    /* Step 4: Optimal gain = (x · Hc) / (Hc · Hc) */
    float corr = 0.0f, energy = 0.0f;
    for (int32_t n = 0; n < L; n++) {
        corr   += x[n] * Hc[n];
        energy += Hc[n] * Hc[n];
    }
    if (energy < 1.0f) energy = 1.0f;

    float g = corr / energy;

    /* Unsigned gain: if negative, flip pulse signs */
    if (g < 0.0f) {
        g = -g;
        for (int32_t k = 0; k < N; k++)
            entry->pulse_sign[k] = -entry->pulse_sign[k];
    }

    entry->gain_index = tlcs_fcb_gain_quantize(g);
    entry->gain = tlcs_fcb_gain_dequantize(entry->gain_index);
}

/* ══════════════════════════════════════════════════════════════════
 *  Algebraic Codebook Search (Pre-Weighted Target)
 *
 *  Same as tlcs_cb_search but accepts a target already in the
 *  weighted domain (including ZIR contribution). Skips the initial
 *  h_w convolution of the target.
 * ══════════════════════════════════════════════════════════════════ */

void tlcs_cb_search_weighted(const float *w_target, const float *h_w,
                              const tlcs_cb_config *cfg, tlcs_cb_entry *entry)
{
    int32_t L = cfg->subfr_size;
    int32_t N = cfg->num_pulses;
    int32_t M = cfg->positions_per_track;

    /* ── Step 1: Precompute d(n) = backward-filtered target ────── */
    float d[TLCS_MAX_SUBFR_SIZE];
    for (int32_t n = 0; n < L; n++) {
        float sum = 0.0f;
        for (int32_t i = n; i < L; i++)
            sum += w_target[i] * h_w[i - n];
        d[n] = sum;
    }

    /* ── Step 2: Precompute phi(i,j) = h_w autocorrelation ─────── */
    static float phi[TLCS_MAX_SUBFR_SIZE * TLCS_MAX_SUBFR_SIZE];
    for (int32_t i = 0; i < L; i++) {
        for (int32_t j = i; j < L; j++) {
            float sum = 0.0f;
            for (int32_t k = j; k < L; k++)
                sum += h_w[k - i] * h_w[k - j];
            phi[i * L + j] = sum;
            phi[j * L + i] = sum;
        }
    }

    /* ── Step 3: 2-pulse pair search ──────────────────────────── */
    /* d_rem tracks remaining target correlation after placing pulses */
    float d_rem[TLCS_MAX_SUBFR_SIZE];
    memcpy(d_rem, d, (size_t)L * sizeof(float));

    int32_t n_pairs = N / 2;
    for (int32_t pair = 0; pair < n_pairs; pair++) {
        int32_t ta = pair * 2;       /* first track in pair */
        int32_t tb = pair * 2 + 1;   /* second track in pair */

        float best_crit = -1.0f;
        int32_t best_ma = 0, best_mb = 0;
        int32_t best_sa = 1, best_sb = 1;

        for (int32_t ma = 0; ma < M; ma++) {
            int32_t pa = ta + ma * N;
            if (pa >= L) continue;
            float sa = (d_rem[pa] >= 0.0f) ? 1.0f : -1.0f;
            float da = sa * d_rem[pa];  /* always >= 0 */

            for (int32_t mb = 0; mb < M; mb++) {
                int32_t pb = tb + mb * N;
                if (pb >= L) continue;
                float sb = (d_rem[pb] >= 0.0f) ? 1.0f : -1.0f;
                float db = sb * d_rem[pb];  /* always >= 0 */

                float num = da + db;
                float den = phi[pa * L + pa] + phi[pb * L + pb]
                          + 2.0f * sa * sb * phi[pa * L + pb];
                if (den < 1e-6f) den = 1e-6f;

                float crit = num * num / den;
                if (crit > best_crit) {
                    best_crit = crit;
                    best_ma = ma;
                    best_mb = mb;
                    best_sa = (int32_t)sa;
                    best_sb = (int32_t)sb;
                }
            }
        }

        entry->pulse_pos[ta] = best_ma;
        entry->pulse_sign[ta] = best_sa;
        entry->pulse_pos[tb] = best_mb;
        entry->pulse_sign[tb] = best_sb;

        /* Update d_rem: subtract placed pulses' contribution */
        int32_t pa = ta + best_ma * N;
        int32_t pb = tb + best_mb * N;
        for (int32_t n = 0; n < L; n++) {
            d_rem[n] -= (float)best_sa * phi[n * L + pa];
            d_rem[n] -= (float)best_sb * phi[n * L + pb];
        }
    }

    /* Handle odd pulse if N is odd */
    if (N % 2 != 0) {
        int32_t k = N - 1;
        float best_crit = -1.0f;
        int32_t best_m = 0;
        for (int32_t m = 0; m < M; m++) {
            int32_t pos = k + m * N;
            if (pos >= L) break;
            float ad = fabsf(d_rem[pos]);
            float den = phi[pos * L + pos];
            if (den < 1e-6f) den = 1e-6f;
            float crit = ad * ad / den;
            if (crit > best_crit) {
                best_crit = crit;
                best_m = m;
            }
        }
        int32_t pos = k + best_m * N;
        entry->pulse_pos[k] = best_m;
        entry->pulse_sign[k] = (d_rem[pos] >= 0.0f) ? 1 : -1;
        for (int32_t n = 0; n < L; n++)
            d_rem[n] -= (float)entry->pulse_sign[k] * phi[n * L + pos];
    }

    /* ── Step 4: Coordinate descent refinement ──────────────── */
    for (int32_t iter = 0; iter < 8; iter++) {
        for (int32_t k = 0; k < N; k++) {
            int32_t old_pos = k + entry->pulse_pos[k] * N;
            float old_sign = (float)entry->pulse_sign[k];

            if (old_pos < L) {
                for (int32_t n = 0; n < L; n++)
                    d_rem[n] += old_sign * phi[n * L + old_pos];
            }

            float best_crit = -1.0f;
            int32_t best_m = 0;
            int32_t best_s = 1;
            for (int32_t m = 0; m < M; m++) {
                int32_t pos = k + m * N;
                if (pos >= L) break;
                float s = (d_rem[pos] >= 0.0f) ? 1.0f : -1.0f;
                float num = s * d_rem[pos];
                float den = phi[pos * L + pos];
                if (den < 1e-6f) den = 1e-6f;
                float crit = num * num / den;
                if (crit > best_crit) {
                    best_crit = crit;
                    best_m = m;
                    best_s = (int32_t)s;
                }
            }

            entry->pulse_pos[k] = best_m;
            entry->pulse_sign[k] = best_s;
            int32_t new_pos = k + best_m * N;

            if (new_pos < L) {
                for (int32_t n = 0; n < L; n++)
                    d_rem[n] -= (float)best_s * phi[n * L + new_pos];
            }
        }
    }

    /* ── Step 5: Compute optimal gain ──────────────────────────── */
    float c[TLCS_MAX_SUBFR_SIZE];
    memset(c, 0, (size_t)L * sizeof(float));
    for (int32_t k = 0; k < N; k++) {
        int32_t abs_pos = k + entry->pulse_pos[k] * N;
        if (abs_pos < L)
            c[abs_pos] = (float)entry->pulse_sign[k];
    }

    float Hc[TLCS_MAX_SUBFR_SIZE];
    for (int32_t n = 0; n < L; n++) {
        Hc[n] = 0.0f;
        for (int32_t i = 0; i <= n; i++)
            Hc[n] += c[i] * h_w[n - i];
    }

    float corr = 0.0f, energy = 0.0f;
    for (int32_t n = 0; n < L; n++) {
        corr   += w_target[n] * Hc[n];
        energy += Hc[n] * Hc[n];
    }
    if (energy < 1.0f) energy = 1.0f;

    float g = corr / energy;

    /* Unsigned gain: if negative, flip pulse signs */
    if (g < 0.0f) {
        g = -g;
        for (int32_t k = 0; k < N; k++)
            entry->pulse_sign[k] = -entry->pulse_sign[k];
    }

    entry->gain_index = tlcs_fcb_gain_quantize(g);
    entry->gain = tlcs_fcb_gain_dequantize(entry->gain_index);
}

/* ══════════════════════════════════════════════════════════════════
 *  Multi-Survivor Tree Search
 *
 *  Explores the pulse configuration space using a tree search with
 *  survivor pruning. For each pulse (track), expands all positions
 *  from all current survivors, then keeps only the top-K candidates
 *  based on accumulated distortion. Followed by coordinate descent
 *  refinement on the best configuration.
 *
 *  This is much more thorough than greedy pair search:
 *    Greedy:  ~100 evaluations (25 per pair × 4 pairs)
 *    Tree:    ~2500+ evaluations (S × M × N stages)
 * ══════════════════════════════════════════════════════════════════ */

#define CB_TREE_SURVIVORS 128

typedef struct {
    int32_t pos[TLCS_CB_MAX_PULSES];
    int32_t sign[TLCS_CB_MAX_PULSES];
    float   dist;
} cb_tree_node;

void tlcs_cb_search_tree(const float *w_target, const float *h_w,
                          const tlcs_cb_config *cfg, tlcs_cb_entry *entry)
{
    int32_t L = cfg->subfr_size;
    int32_t N = cfg->num_pulses;
    int32_t M = cfg->positions_per_track;

    /* ── Precompute d(n) and phi(i,j) ─────────────────────────── */
    float d[TLCS_MAX_SUBFR_SIZE];
    for (int32_t n = 0; n < L; n++) {
        float sum = 0.0f;
        for (int32_t i = n; i < L; i++)
            sum += w_target[i] * h_w[i - n];
        d[n] = sum;
    }

    static float phi[TLCS_MAX_SUBFR_SIZE * TLCS_MAX_SUBFR_SIZE];
    for (int32_t i = 0; i < L; i++) {
        for (int32_t j = i; j < L; j++) {
            float sum = 0.0f;
            for (int32_t k = j; k < L; k++)
                sum += h_w[k - i] * h_w[k - j];
            phi[i * L + j] = sum;
            phi[j * L + i] = sum;
        }
    }

    /* ── Tree search with survivor pruning ────────────────────── */
    static cb_tree_node pool_a[CB_TREE_SURVIVORS];
    static cb_tree_node pool_b[CB_TREE_SURVIVORS];
    cb_tree_node *cur = pool_a;
    cb_tree_node *nxt = pool_b;
    int32_t n_cur = 1;

    memset(&cur[0], 0, sizeof(cb_tree_node));

    for (int32_t k = 0; k < N; k++) {
        int32_t n_nxt = 0;

        for (int32_t s = 0; s < n_cur; s++) {
            for (int32_t m = 0; m < M; m++) {
                int32_t pos = k + m * N;
                if (pos >= L) continue;

                /* Effective correlation: d minus contribution of placed pulses */
                float d_eff = d[pos];
                for (int32_t j = 0; j < k; j++) {
                    int32_t pp = j + cur[s].pos[j] * N;
                    d_eff -= (float)cur[s].sign[j] * phi[pos * L + pp];
                }

                int32_t sgn = (d_eff >= 0.0f) ? 1 : -1;
                float delta = -2.0f * fabsf(d_eff) + phi[pos * L + pos];
                float total = cur[s].dist + delta;

                /* Sorted insertion into next pool (ascending distortion) */
                int32_t ins = n_nxt;
                while (ins > 0 && nxt[ins - 1].dist > total)
                    ins--;

                if (ins < CB_TREE_SURVIVORS) {
                    int32_t end = (n_nxt < CB_TREE_SURVIVORS) ? n_nxt : CB_TREE_SURVIVORS - 1;
                    for (int32_t i = end; i > ins; i--)
                        nxt[i] = nxt[i - 1];

                    nxt[ins] = cur[s];
                    nxt[ins].pos[k] = m;
                    nxt[ins].sign[k] = sgn;
                    nxt[ins].dist = total;

                    if (n_nxt < CB_TREE_SURVIVORS)
                        n_nxt++;
                }
            }
        }

        /* Swap pools */
        cb_tree_node *tmp = cur;
        cur = nxt;
        nxt = tmp;
        n_cur = n_nxt;
    }

    /* Copy best survivor to entry */
    if (n_cur > 0) {
        for (int32_t k = 0; k < N; k++) {
            entry->pulse_pos[k] = cur[0].pos[k];
            entry->pulse_sign[k] = cur[0].sign[k];
        }
    }

    /* ── Coordinate descent refinement on best config ─────────── */
    float d_rem[TLCS_MAX_SUBFR_SIZE];
    memcpy(d_rem, d, (size_t)L * sizeof(float));
    for (int32_t k = 0; k < N; k++) {
        int32_t pos = k + entry->pulse_pos[k] * N;
        if (pos < L) {
            for (int32_t n = 0; n < L; n++)
                d_rem[n] -= (float)entry->pulse_sign[k] * phi[n * L + pos];
        }
    }

    for (int32_t iter = 0; iter < 8; iter++) {
        for (int32_t k = 0; k < N; k++) {
            int32_t old_pos = k + entry->pulse_pos[k] * N;
            if (old_pos < L) {
                for (int32_t n = 0; n < L; n++)
                    d_rem[n] += (float)entry->pulse_sign[k] * phi[n * L + old_pos];
            }

            float best_crit = -1.0f;
            int32_t best_m = 0, best_s = 1;
            for (int32_t m = 0; m < M; m++) {
                int32_t pos = k + m * N;
                if (pos >= L) break;
                float s = (d_rem[pos] >= 0.0f) ? 1.0f : -1.0f;
                float num = s * d_rem[pos];
                float den = phi[pos * L + pos];
                if (den < 1e-6f) den = 1e-6f;
                float crit = num * num / den;
                if (crit > best_crit) {
                    best_crit = crit;
                    best_m = m;
                    best_s = (int32_t)s;
                }
            }

            entry->pulse_pos[k] = best_m;
            entry->pulse_sign[k] = best_s;
            int32_t new_pos = k + best_m * N;
            if (new_pos < L) {
                for (int32_t n = 0; n < L; n++)
                    d_rem[n] -= (float)best_s * phi[n * L + new_pos];
            }
        }
    }

    /* ── Compute optimal gain ─────────────────────────────────── */
    float c[TLCS_MAX_SUBFR_SIZE];
    memset(c, 0, (size_t)L * sizeof(float));
    for (int32_t k = 0; k < N; k++) {
        int32_t abs_pos = k + entry->pulse_pos[k] * N;
        if (abs_pos < L)
            c[abs_pos] = (float)entry->pulse_sign[k];
    }

    float Hc[TLCS_MAX_SUBFR_SIZE];
    for (int32_t n = 0; n < L; n++) {
        Hc[n] = 0.0f;
        for (int32_t i = 0; i <= n; i++)
            Hc[n] += c[i] * h_w[n - i];
    }

    float corr = 0.0f, energy = 0.0f;
    for (int32_t n = 0; n < L; n++) {
        corr   += w_target[n] * Hc[n];
        energy += Hc[n] * Hc[n];
    }
    if (energy < 1.0f) energy = 1.0f;

    float g = corr / energy;
    if (g < 0.0f) {
        g = -g;
        for (int32_t k = 0; k < N; k++)
            entry->pulse_sign[k] = -entry->pulse_sign[k];
    }

    entry->gain_index = tlcs_fcb_gain_quantize(g);
    entry->gain = tlcs_fcb_gain_dequantize(entry->gain_index);
}

/* ══════════════════════════════════════════════════════════════════
 *  Build Innovation Vector
 * ══════════════════════════════════════════════════════════════════ */

void tlcs_cb_build_innovation(const tlcs_cb_entry *entry,
                               const tlcs_cb_config *cfg,
                               float *innov, int32_t subfr_size)
{
    memset(innov, 0, (size_t)subfr_size * sizeof(float));
    for (int32_t k = 0; k < cfg->num_pulses; k++) {
        int32_t abs_pos = k + entry->pulse_pos[k] * cfg->num_pulses;
        if (abs_pos < subfr_size)
            innov[abs_pos] = entry->gain * (float)entry->pulse_sign[k];
    }
}

/* ══════════════════════════════════════════════════════════════════
 *  Pitch Sharpening
 *
 *  Adds periodicity to the fixed codebook innovation vector.
 *  For voiced speech (high pitch gain), this makes the codebook
 *  contribution periodic, which improves adaptive codebook quality
 *  in subsequent frames.
 *
 *  beta = min(pitch_gain, 0.85) for pitch_gain > 0.4, else 0
 * ══════════════════════════════════════════════════════════════════ */

void tlcs_cb_pitch_sharpen(float *innov, int32_t subfr_size,
                            int32_t pitch_lag, float pitch_gain)
{
    if (pitch_lag < 1 || pitch_lag >= subfr_size) return;
    if (pitch_gain < 0.6f) return;  /* skip for unvoiced/transition */

    float beta = pitch_gain * 0.4f;
    if (beta > 0.4f) beta = 0.4f;

    for (int32_t n = pitch_lag; n < subfr_size; n++)
        innov[n] += beta * innov[n - pitch_lag];
}

/* ══════════════════════════════════════════════════════════════════
 *  Phase Dispersion
 *
 *  Applies a short all-pass FIR filter to the innovation vector
 *  to spread sparse pulse energy in time. Reduces scratchiness
 *  artifacts, especially for female speech.
 *
 *  The filter has approximately flat magnitude response (all-pass
 *  property) while smearing the pulse energy across ~7 samples.
 *  Must be applied identically in encoder and decoder.
 * ══════════════════════════════════════════════════════════════════ */

void tlcs_cb_phase_disperse(float *innov, int32_t subfr_size, float pitch_gain)
{
    (void)pitch_gain;  /* reserved for future use */

    /* 11-tap phase dispersion FIR (wider spread, alternating signs)
     * DC gain ≈ 0.80, Nyquist gain ≈ 1.24: mild HF boost + wider spreading */
    static const float pd_fir[11] = {
        0.06f, -0.10f, 0.15f, -0.20f, 0.30f, 0.38f, 0.30f, -0.20f, 0.15f, -0.10f, 0.06f
    };
    static const int32_t pd_len = 11;
    static const int32_t pd_center = 5;  /* center tap index */

    float tmp[TLCS_MAX_SUBFR_SIZE];

    for (int32_t n = 0; n < subfr_size; n++) {
        float sum = 0.0f;
        for (int32_t k = 0; k < pd_len; k++) {
            int32_t idx = n - pd_center + k;
            if (idx >= 0 && idx < subfr_size)
                sum += pd_fir[k] * innov[idx];
        }
        tmp[n] = sum;
    }

    memcpy(innov, tmp, (size_t)subfr_size * sizeof(float));
}

/* ══════════════════════════════════════════════════════════════════
 *  Innovation Spreading
 *
 *  Convolves the innovation vector with a symmetric 3-tap kernel
 *  [alpha, 1, alpha] to reduce sparsity of ACELP pulses.
 *  Each delta pulse becomes a shaped burst of 3 samples.
 *  Must be applied identically in encoder and decoder.
 * ══════════════════════════════════════════════════════════════════ */

void tlcs_cb_spread_innovation(float *innov, int32_t subfr_size, float alpha)
{
    if (alpha <= 0.0f) return;

    float tmp[TLCS_MAX_SUBFR_SIZE];
    tmp[0] = innov[0] + alpha * innov[1];
    for (int32_t n = 1; n < subfr_size - 1; n++)
        tmp[n] = alpha * innov[n - 1] + innov[n] + alpha * innov[n + 1];
    tmp[subfr_size - 1] = alpha * innov[subfr_size - 2] + innov[subfr_size - 1];

    memcpy(innov, tmp, (size_t)subfr_size * sizeof(float));
}

/* ══════════════════════════════════════════════════════════════════
 *  Spread Impulse Response
 *
 *  Convolves h_w with [alpha, 1, alpha] so the tree search
 *  optimizes pulse positions for the spread codebook vectors.
 * ══════════════════════════════════════════════════════════════════ */

void tlcs_cb_spread_impulse_response(float *h_w, int32_t subfr_size, float alpha)
{
    if (alpha <= 0.0f) return;

    float tmp[TLCS_MAX_SUBFR_SIZE];
    tmp[0] = h_w[0] + alpha * h_w[1];
    for (int32_t n = 1; n < subfr_size - 1; n++)
        tmp[n] = alpha * h_w[n - 1] + h_w[n] + alpha * h_w[n + 1];
    tmp[subfr_size - 1] = alpha * h_w[subfr_size - 2] + h_w[subfr_size - 1];

    memcpy(h_w, tmp, (size_t)subfr_size * sizeof(float));
}

/* ══════════════════════════════════════════════════════════════════
 *  Fixed Codebook Gain Quantization
 *
 *  7-bit unsigned (sign absorbed into pulse signs).
 *  Level 0 = zero gain.
 *  Levels 1–127: log-scale from FCB_GAIN_MIN to FCB_GAIN_MAX.
 *  127 magnitude levels give ~2.5% relative precision.
 * ══════════════════════════════════════════════════════════════════ */

#define FCB_GAIN_MIN    1.0f
#define FCB_GAIN_MAX    10000.0f

int32_t tlcs_fcb_gain_quantize(float gain)
{
    /* Gain must be non-negative (encoder flips pulse signs if needed) */
    if (gain < FCB_GAIN_MIN * 0.5f)
        return 0;

    if (gain < FCB_GAIN_MIN) gain = FCB_GAIN_MIN;
    if (gain > FCB_GAIN_MAX) gain = FCB_GAIN_MAX;

    /* Log-scale: level i (1–N-1) → MIN * (MAX/MIN)^((i-1)/(N-2)) */
    float log_ratio = logf(gain / FCB_GAIN_MIN)
                    / logf(FCB_GAIN_MAX / FCB_GAIN_MIN);
    int32_t idx = 1 + (int32_t)(log_ratio * (float)(TLCS_FCB_GAIN_LEVELS - 2) + 0.5f);
    if (idx < 1) idx = 1;
    if (idx >= TLCS_FCB_GAIN_LEVELS) idx = TLCS_FCB_GAIN_LEVELS - 1;

    return idx;
}

float tlcs_fcb_gain_dequantize(int32_t index)
{
    if (index <= 0) return 0.0f;
    if (index >= TLCS_FCB_GAIN_LEVELS) index = TLCS_FCB_GAIN_LEVELS - 1;

    return FCB_GAIN_MIN
         * powf(FCB_GAIN_MAX / FCB_GAIN_MIN,
                (float)(index - 1) / (float)(TLCS_FCB_GAIN_LEVELS - 2));
}

/* ══════════════════════════════════════════════════════════════════
 *  Predictive FCB Gain Quantization (Unsigned)
 *
 *  Quantizes log(gain / pred_gain) centered around 0.
 *  Index 64 = prediction (ratio = 1.0).
 *  Range covers pred * [1/40, 40] = ~1600:1 effective ratio.
 *  126 steps give ~0.5 dB precision.
 *  Falls back to absolute quantization if pred_gain <= 0.
 * ══════════════════════════════════════════════════════════════════ */

#define FCB_PRED_LOG_RANGE   3.7f   /* ±3.7 nats = exp(±3.7) ≈ ×40 */
#define FCB_PRED_CENTER      (TLCS_FCB_GAIN_LEVELS / 2)  /* center index */
#define FCB_PRED_MIN_PRED    2.0f   /* minimum prediction to use pred mode */

int32_t tlcs_fcb_gain_quantize_pred(float gain, float pred_gain)
{
    /* Fall back to absolute if no valid prediction */
    if (pred_gain < FCB_PRED_MIN_PRED)
        return tlcs_fcb_gain_quantize(gain);

    /* Gain must be non-negative (encoder flips pulse signs if needed) */
    if (gain < 0.5f)
        return 0;

    float log_ratio = logf(gain / pred_gain);

    /* Clamp to range */
    if (log_ratio < -FCB_PRED_LOG_RANGE) log_ratio = -FCB_PRED_LOG_RANGE;
    if (log_ratio >  FCB_PRED_LOG_RANGE) log_ratio =  FCB_PRED_LOG_RANGE;

    /* Map to index: center, step = 2*RANGE/(LEVELS-2) */
    float step = 2.0f * FCB_PRED_LOG_RANGE / (float)(TLCS_FCB_GAIN_LEVELS - 2);
    int32_t idx = FCB_PRED_CENTER + (int32_t)(log_ratio / step + 0.5f);
    if (idx < 1) idx = 1;
    if (idx >= TLCS_FCB_GAIN_LEVELS) idx = TLCS_FCB_GAIN_LEVELS - 1;

    return idx;
}

float tlcs_fcb_gain_dequantize_pred(int32_t index, float pred_gain)
{
    if (pred_gain < FCB_PRED_MIN_PRED)
        return tlcs_fcb_gain_dequantize(index);

    if (index <= 0) return 0.0f;
    if (index >= TLCS_FCB_GAIN_LEVELS) index = TLCS_FCB_GAIN_LEVELS - 1;

    float step = 2.0f * FCB_PRED_LOG_RANGE / 126.0f;
    float log_ratio = (float)(index - FCB_PRED_CENTER) * step;
    return pred_gain * expf(log_ratio);
}

/* ══════════════════════════════════════════════════════════════════
 *  Parameterized Predictive FCB Gain Quantization
 *
 *  Same algorithm as above but with configurable bit depth.
 *  For low-rate: 5-bit (32 levels), same log-ratio range.
 * ══════════════════════════════════════════════════════════════════ */

int32_t tlcs_fcb_gain_quantize_pred_n(float gain, float pred_gain, int32_t bits)
{
    int32_t levels = 1 << bits;
    int32_t center = levels / 2;

    if (pred_gain < FCB_PRED_MIN_PRED) {
        /* Absolute quantization fallback with variable bits */
        if (gain < FCB_GAIN_MIN * 0.5f) return 0;
        if (gain < FCB_GAIN_MIN) gain = FCB_GAIN_MIN;
        if (gain > FCB_GAIN_MAX) gain = FCB_GAIN_MAX;
        float log_r = logf(gain / FCB_GAIN_MIN) / logf(FCB_GAIN_MAX / FCB_GAIN_MIN);
        int32_t idx = 1 + (int32_t)(log_r * (float)(levels - 2) + 0.5f);
        if (idx < 1) idx = 1;
        if (idx >= levels) idx = levels - 1;
        return idx;
    }

    if (gain < 0.5f) return 0;

    float log_ratio = logf(gain / pred_gain);
    if (log_ratio < -FCB_PRED_LOG_RANGE) log_ratio = -FCB_PRED_LOG_RANGE;
    if (log_ratio >  FCB_PRED_LOG_RANGE) log_ratio =  FCB_PRED_LOG_RANGE;

    float step = 2.0f * FCB_PRED_LOG_RANGE / (float)(levels - 2);
    int32_t idx = center + (int32_t)(log_ratio / step + 0.5f);
    if (idx < 1) idx = 1;
    if (idx >= levels) idx = levels - 1;

    return idx;
}

float tlcs_fcb_gain_dequantize_pred_n(int32_t index, float pred_gain, int32_t bits)
{
    int32_t levels = 1 << bits;
    int32_t center = levels / 2;

    if (pred_gain < FCB_PRED_MIN_PRED) {
        /* Absolute dequantization fallback with variable bits */
        if (index <= 0) return 0.0f;
        if (index >= levels) index = levels - 1;
        return FCB_GAIN_MIN
             * powf(FCB_GAIN_MAX / FCB_GAIN_MIN,
                    (float)(index - 1) / (float)(levels - 2));
    }

    if (index <= 0) return 0.0f;
    if (index >= levels) index = levels - 1;

    float step = 2.0f * FCB_PRED_LOG_RANGE / (float)(levels - 2);
    float log_ratio = (float)(index - center) * step;
    return pred_gain * expf(log_ratio);
}

/* ══════════════════════════════════════════════════════════════════
 *  FFT-based Perceptual Weighting Filter (SMPL-style)
 *
 *  1. DFT of windowed speech → power spectrum (LINEAR domain)
 *  2. Mel-scale bidirectional exponential smoothing (LINEAR)
 *  3. Spectral emphasis: |1 + perc_emph * e^(-jω)|² (high-pass)
 *  4. IDFT → autocorrelation of emphasized masking curve
 *  5. Levinson-Durbin at high order (31) → A_perc(z)
 *
 *  Key: perc_order >> LPC order prevents A_perc ≈ A cancellation.
 *  h_w = A_perc(z)/A(z) has rich spectral structure.
 * ══════════════════════════════════════════════════════════════════ */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define PERC_DFT_SIZE 256
#define PERC_DFT_HALF (PERC_DFT_SIZE / 2)

int32_t tlcs_compute_perceptual_filter(const float *speech, int32_t frame_size,
                                        float sample_rate,
                                        float *b_perc, int32_t perc_order)
{
    int32_t N = PERC_DFT_SIZE;
    int32_t N2 = PERC_DFT_HALF;
    int32_t len = frame_size < N ? frame_size : N;

    /* Step 1: Hanning window the speech */
    float windowed[PERC_DFT_SIZE];
    memset(windowed, 0, sizeof(windowed));
    if (len > 1) {
        float inv_nm1 = 1.0f / (float)(len - 1);
        for (int32_t i = 0; i < len; i++) {
            float w = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)i * inv_nm1));
            windowed[i] = speech[i] * w;
        }
    }

    /* Step 2: Power spectrum via DFT (brute-force, N=256) */
    float power[PERC_DFT_HALF + 1];
    for (int32_t k = 0; k <= N2; k++) {
        float re = 0.0f, im = 0.0f;
        float ang_step = 2.0f * (float)M_PI * (float)k / (float)N;
        for (int32_t n = 0; n < len; n++) {
            float angle = ang_step * (float)n;
            re += windowed[n] * cosf(angle);
            im -= windowed[n] * sinf(angle);
        }
        power[k] = re * re + im * im;
    }

    /* Step 3: Mel-scale bidirectional smoothing in LINEAR domain
     * (SMPL smooths power spectrum directly, not log spectrum) */
    float freq_step = sample_rate / (float)N;

    /* Forward pass (in-place on power) */
    float smoothed[PERC_DFT_HALF + 1];
    smoothed[0] = power[0];
    for (int32_t k = 1; k <= N2; k++) {
        float freq_hz = (float)k * freq_step;
        float perc_width = 0.11f * (freq_hz + 320.0f) / freq_step;
        float alpha = perc_width / (perc_width + 1.0f);
        smoothed[k] = power[k] + alpha * (smoothed[k - 1] - power[k]);
    }

    /* Backward pass (in-place, matches SMPL smth_filt) */
    float smth = smoothed[N2];
    for (int32_t k = N2 - 1; k >= 0; k--) {
        float freq_hz = (float)(k + 1) * freq_step;
        float perc_width = 0.11f * (freq_hz + 320.0f) / freq_step;
        float alpha = perc_width / (perc_width + 1.0f);
        smoothed[k] = smoothed[k] + alpha * (smth - smoothed[k]);
        smth = smoothed[k];
    }

    /* Step 4: Apply spectral emphasis |1 + emph * e^(-jω)|²
     * emph = -0.72 → high-pass: boosts high-freq masking
     * DC attenuated by 0.08, Nyquist boosted by 2.96 */
    float emph = -0.72f;
    float mask[PERC_DFT_HALF + 1];
    for (int32_t k = 0; k <= N2; k++) {
        float omega = (float)M_PI * (float)k / (float)N2;
        float tilt = 1.0f + 2.0f * emph * cosf(omega) + emph * emph;
        mask[k] = smoothed[k] * tilt;
    }

    /* Step 5: Autocorrelation from masking spectrum via IDFT
     * R(m) = (2/N) * Σ_k w(k) * P(k) * cos(π*k*m/N2) */
    float r[TLCS_MAX_SUBFR_SIZE]; /* supports up to order subfr-1 */
    for (int32_t m = 0; m <= perc_order; m++) {
        float sum = 0.0f;
        for (int32_t k = 0; k <= N2; k++) {
            float wt = (k == 0 || k == N2) ? 1.0f : 2.0f;
            sum += wt * mask[k] * cosf((float)M_PI * (float)k * (float)m / (float)N2);
        }
        r[m] = sum / (float)N;
    }

    /* Step 6: Lag windowing for stability (60 Hz bandwidth expansion) */
    float bw = 60.0f * (float)M_PI / sample_rate;
    for (int32_t m = 1; m <= perc_order; m++)
        r[m] *= expf(-0.5f * (float)(m * m) * bw * bw);

    r[0] *= (1.0f + 1e-3f);  /* SMPL_PERC_REG = 1e-3 */

    if (r[0] <= 0.0f)
        return -1;

    /* Step 7: Levinson-Durbin → perceptual filter A_perc(z) */
    float pred_gain = tlcs_levinson(r, perc_order, b_perc, NULL);
    (void)pred_gain;

    /* Verify stability */
    float energy = 0.0f;
    for (int32_t k = 1; k <= perc_order; k++)
        energy += b_perc[k] * b_perc[k];
    if (energy > 200.0f || energy != energy)
        return -1;

    return 0;
}

void tlcs_perceptual_impulse_response(const float *a_q, int32_t order,
                                       const float *b_perc, int32_t perc_order,
                                       float *h_w, int32_t subfr_size)
{
    /* Step 1: Impulse response of 1/A(z) — synthesis filter */
    float h[TLCS_MAX_SUBFR_SIZE];
    h[0] = 1.0f;
    for (int32_t n = 1; n < subfr_size; n++) {
        h[n] = 0.0f;
        int32_t kmax = (n < order) ? n : order;
        for (int32_t k = 1; k <= kmax; k++)
            h[n] -= a_q[k] * h[n - k];
    }

    /* Step 2: Apply B(z) FIR perceptual whitening filter */
    for (int32_t n = 0; n < subfr_size; n++) {
        h_w[n] = h[n]; /* b_perc[0] = 1.0 */
        int32_t kmax = (n < perc_order) ? n : perc_order;
        for (int32_t k = 1; k <= kmax; k++)
            h_w[n] += b_perc[k] * h[n - k];
    }
}
