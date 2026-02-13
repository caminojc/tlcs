#include "tlcs_qmf.h"
#include <string.h>

/* ══════════════════════════════════════════════════════════════════
 *  Complementary Allpass Filter Bank
 *
 *  Replaces G.722 QMF (which has a transfer function zero at
 *  crossover, giving terrible round-trip SNR) with an IIR allpass
 *  pair that provides near-perfect reconstruction.
 *
 *  Each filter is 2nd-order allpass:
 *    H(z) = (a2 + a1·z⁻¹ + z⁻²) / (1 + a1·z⁻¹ + a2·z⁻²)
 *
 *  |H(e^jω)| = 1 for all ω, so magnitude is perfectly preserved.
 *  Coefficients optimized for minimal sidelobe energy.
 * ══════════════════════════════════════════════════════════════════ */

/* Low-band allpass (A0) */
static const float A0_coef[3] = {1.0f, 0.60797656f, 0.036630828f};

/* High-band allpass (A1) */
static const float A1_coef[3] = {1.0f, 1.1034178f,  0.2197291f};

/* ── 2nd-order allpass filter ────────────────────────────────────
 * Decomposed as MA2 → AR2:
 *   MA: y_ma[n] = a2·x[n] + a1·x[n-1] + x[n-2]
 *   AR: y[n]    = y_ma[n] - a1·y[n-1] - a2·y[n-2]
 *
 * state layout: [0]=x[n-1], [1]=x[n-2], [2]=y[n-1], [3]=y[n-2]
 * ─────────────────────────────────────────────────────────────── */
static void allpass2(const float *x, int32_t n,
                     const float *coef, float *state,
                     float *y)
{
    float ma0 = state[0];   /* x[n-1] */
    float ma1 = state[1];   /* x[n-2] */
    float ar0 = state[2];   /* y[n-1] */
    float ar1 = state[3];   /* y[n-2] */

    const float a1 = coef[1];
    const float a2 = coef[2];

    for (int32_t i = 0; i < n; i++) {
        /* MA part: numerator = a2 + a1·z⁻¹ + z⁻² */
        float ma_out = a2 * x[i] + a1 * ma0 + ma1;
        ma1 = ma0;
        ma0 = x[i];

        /* AR part: denominator = 1 + a1·z⁻¹ + a2·z⁻² */
        float ar_out = ma_out - a1 * ar0 - a2 * ar1;
        ar1 = ar0;
        ar0 = ar_out;

        y[i] = ar_out;
    }

    state[0] = ma0;
    state[1] = ma1;
    state[2] = ar0;
    state[3] = ar1;
}

void tlcs_qmf_init(void)
{
    /* Compile-time constants — nothing to do */
}

/* ── Analysis: fullband → LB + HB ───────────────────────────────
 * 1. Demux input into even/odd polyphase components
 * 2. Apply A0 to odd samples, A1 to even samples
 * 3. Sum/difference → low-band / high-band
 *
 * ana_mem layout: [0..3] = A0 state, [4..7] = A1 state
 * ─────────────────────────────────────────────────────────────── */
void tlcs_qmf_analyze(float *ana_mem,
                       const float *in, int32_t n_in,
                       float *lb, float *hb)
{
    int32_t n_out = n_in / 2;
    float x_odd[TLCS_MAX_FRAME_SIZE];
    float x_even[TLCS_MAX_FRAME_SIZE];

    /* Step 1: Demultiplex */
    for (int32_t i = 0; i < n_out; i++) {
        x_odd[i]  = in[2 * i + 1];   /* odd-indexed samples */
        x_even[i] = in[2 * i];       /* even-indexed samples */
    }

    /* Step 2: Apply allpass filters */
    float y0[TLCS_MAX_FRAME_SIZE], y1[TLCS_MAX_FRAME_SIZE];
    allpass2(x_odd,  n_out, A0_coef, ana_mem,     y0);
    allpass2(x_even, n_out, A1_coef, ana_mem + 4, y1);

    /* Step 3: Combine into subbands */
    for (int32_t i = 0; i < n_out; i++) {
        lb[i] = (y0[i] + y1[i]) * 0.5f;
        hb[i] = (y0[i] - y1[i]) * 0.5f;
    }
}

/* ── Synthesis: LB + HB → fullband ──────────────────────────────
 * 1. Sum/difference of subbands
 * 2. Apply A0 to sum, A1 to difference
 * 3. Interleave to fullband rate
 *
 * lb_mem[0..3] = A0 state, hb_mem[0..3] = A1 state
 * ─────────────────────────────────────────────────────────────── */
void tlcs_qmf_synthesize(float *lb_mem, float *hb_mem,
                          const float *lb, const float *hb,
                          float *out, int32_t n_sub)
{
    float sum[TLCS_MAX_FRAME_SIZE], diff[TLCS_MAX_FRAME_SIZE];
    float y0[TLCS_MAX_FRAME_SIZE], y1[TLCS_MAX_FRAME_SIZE];

    /* Step 1: Separate into allpass inputs */
    for (int32_t i = 0; i < n_sub; i++) {
        sum[i]  = lb[i] + hb[i];
        diff[i] = lb[i] - hb[i];
    }

    /* Step 2: Apply allpass filters */
    allpass2(sum,  n_sub, A0_coef, lb_mem, y0);
    allpass2(diff, n_sub, A1_coef, hb_mem, y1);

    /* Step 3: Interleave to fullband */
    for (int32_t i = 0; i < n_sub; i++) {
        out[2 * i]     = y0[i];
        out[2 * i + 1] = y1[i];
    }
}
