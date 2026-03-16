/*
 * tlcs_lpc.c — LPC analysis and synthesis.
 *
 * Autocorrelation, Levinson-Durbin, LPC <-> LSP conversion,
 * LSP interpolation, bandwidth expansion, synthesis filter.
 */
#include "tlcs_config.h"
#include "tlcs_lpc.h"

#include <math.h>
#include <string.h>

/* ================================================================== */
/* Autocorrelation with lag windowing                                  */
/* ================================================================== */

static void autocorrelation(const float *sig, int n, int order, float *r)
{
    /* Lag-windowed bandwidth expansion parameter (60 Hz equiv.) */
    const float bw = 60.0f;
    const float scale = 2.0f * 3.14159265f * bw / (float)TLCS_SAMPLE_RATE;

    for (int k = 0; k <= order; k++) {
        float sum = 0.0f;
        int len = n - k;
        for (int i = 0; i < len; i++) {
            sum += sig[i] * sig[i + k];
        }
        /* Lag windowing for stability */
        float w = expf(-0.5f * (scale * (float)k) * (scale * (float)k));
        r[k] = sum * w;
    }
}

/* ================================================================== */
/* Levinson-Durbin recursion                                           */
/* ================================================================== */

static float levinson_durbin(const float *r, int order, float *a)
{
    float a_tmp[TLCS_LPC_ORDER + 1];
    float e;

    memset(a, 0, (order + 1) * sizeof(float));
    a[0] = 1.0f;
    e = r[0];
    if (e <= 0.0f) {
        return 1e-10f;
    }

    for (int i = 1; i <= order; i++) {
        /* Compute reflection coefficient */
        float lam = 0.0f;
        for (int j = 1; j < i; j++) {
            lam += a[j] * r[i - j];
        }
        lam = -(lam + r[i]);
        float ki = lam / e;

        /* Clamp for stability */
        if (ki > 0.9999f)  ki = 0.9999f;
        if (ki < -0.9999f) ki = -0.9999f;

        /* Update coefficients */
        memcpy(a_tmp, a, (order + 1) * sizeof(float));
        for (int j = 1; j < i; j++) {
            a_tmp[j] = a[j] + ki * a[i - j];
        }
        a_tmp[i] = ki;
        memcpy(a, a_tmp, (order + 1) * sizeof(float));

        e *= (1.0f - ki * ki);
        if (e <= 0.0f) e = 1e-10f;
    }
    return e;
}

/* ================================================================== */
/* LPC analysis (windowed autocorrelation + Levinson-Durbin)           */
/* ================================================================== */

void tlcs_lpc_analysis(const float *frame, int frame_len, int order,
                       float *lpc_out, float *gain_out)
{
    float windowed[TLCS_FRAME_SIZE];
    float r[TLCS_LPC_ORDER + 1];

    int n = frame_len;
    if (n > TLCS_FRAME_SIZE) n = TLCS_FRAME_SIZE;

    /* Hamming window */
    for (int i = 0; i < n; i++) {
        float w = 0.54f - 0.46f * cosf(2.0f * 3.14159265f * (float)i / (float)(n - 1));
        windowed[i] = frame[i] * w;
    }

    autocorrelation(windowed, n, order, r);

    float e = levinson_durbin(r, order, lpc_out);

    /* Bandwidth expansion for stability */
    tlcs_bwe(lpc_out, order, TLCS_LPC_BWE);

    if (gain_out) {
        *gain_out = sqrtf(e > 1e-10f ? e : 1e-10f);
    }
}

/* ================================================================== */
/* Bandwidth expansion                                                 */
/* ================================================================== */

void tlcs_bwe(float *lpc, int order, float gamma)
{
    float g = gamma;
    for (int k = 1; k <= order; k++) {
        lpc[k] *= g;
        g *= gamma;
    }
}

/* ================================================================== */
/* SILK-style cosine lookup table and helpers                           */
/* ================================================================== */

/*
 * Piecewise-linear cosine table from SILK (Q12, 129 entries).
 * Maps NLSF in Q15 (0..32768 = 0..pi) to 2*cos(LSF) in Q12.
 * Entry i = round(2 * cos(i * pi / 128) * 4096).
 */
static const short silk_cos_tab_q12[129] = {
     8192,  8190,  8182,  8170,  8152,  8130,  8104,  8072,
     8034,  7994,  7946,  7896,  7840,  7778,  7714,  7644,
     7568,  7490,  7406,  7318,  7226,  7128,  7026,  6922,
     6812,  6698,  6580,  6458,  6332,  6204,  6070,  5934,
     5792,  5648,  5502,  5352,  5198,  5040,  4880,  4718,
     4552,  4382,  4212,  4038,  3862,  3684,  3502,  3320,
     3136,  2948,  2760,  2570,  2378,  2186,  1990,  1794,
     1598,  1400,  1202,  1002,   802,   602,   402,   202,
        0,  -202,  -402,  -602,  -802, -1002, -1202, -1400,
    -1598, -1794, -1990, -2186, -2378, -2570, -2760, -2948,
    -3136, -3320, -3502, -3684, -3862, -4038, -4212, -4382,
    -4552, -4718, -4880, -5040, -5198, -5352, -5502, -5648,
    -5792, -5934, -6070, -6204, -6332, -6458, -6580, -6698,
    -6812, -6922, -7026, -7128, -7226, -7318, -7406, -7490,
    -7568, -7644, -7714, -7778, -7840, -7896, -7946, -7994,
    -8034, -8072, -8104, -8130, -8152, -8170, -8182, -8190,
    -8192
};

#define LSF_COS_TAB_SZ  128
#define BIN_DIV_STEPS    3
#define MAX_A2NLSF_ITER  16
#define MY_PI            3.14159265358979323846f

/* ================================================================== */
/* LPC -> LSP conversion (SILK A2NLSF algorithm, float)                */
/* ================================================================== */

/*
 * Transform polynomial from cos(n*f) basis to cos(f)^n basis.
 * Same as silk_A2NLSF_trans_poly but in double precision.
 */
static void trans_poly(double *p, int dd)
{
    for (int k = 2; k <= dd; k++) {
        for (int n = dd; n > k; n--) {
            p[n - 2] -= p[n];
        }
        p[k - 2] -= 2.0 * p[k];
    }
}

/*
 * Evaluate polynomial in cos(f)^n basis using Horner's method.
 * x = cos(f) in Q12-equivalent float, p[] in Q16-equivalent float.
 * Returns value in Q16-equivalent float.
 */
static double eval_poly(const double *p, double x, int dd)
{
    double x_scaled = x * 16.0;   /* Q12 -> Q16 equivalent scaling */
    double y = p[dd];
    for (int n = dd - 1; n >= 0; n--) {
        /* SMLAWW equivalent: p[n] + (y * x_scaled) >> 16 in float */
        y = p[n] + (y * x_scaled) / 65536.0;
    }
    return y;
}

/*
 * Initialize P and Q polynomials from LPC coefficients.
 * a_q16[] = LPC coefficients scaled to Q16 (no leading 1).
 */
static void a2nlsf_init(const double *a_q16, double *P, double *Q, int dd, int d)
{
    /* Convert filter coefficients to even and odd polynomials */
    P[dd] = 65536.0;   /* 1 << 16 */
    Q[dd] = 65536.0;
    for (int k = 0; k < dd; k++) {
        P[k] = -a_q16[dd - k - 1] - a_q16[dd + k];
        Q[k] = -a_q16[dd - k - 1] + a_q16[dd + k];
    }

    /* Divide out trivial roots: z=1 from Q, z=-1 from P */
    for (int k = dd; k > 0; k--) {
        P[k - 1] -= P[k];
        Q[k - 1] += Q[k];
    }

    /* Transform from cos(n*f) to cos(f)^n */
    trans_poly(P, dd);
    trans_poly(Q, dd);
}

void tlcs_lpc_to_lsp(const float *lpc, int order, float *lsp_out)
{
    int dd = order / 2;

    /* Convert float LPC to Q16 double (same as SMPL wrapper) */
    double a_q16[TLCS_LPC_ORDER];
    for (int i = 0; i < order; i++) {
        a_q16[i] = (double)(-lpc[i + 1]) * 65536.0;
    }

    double P[TLCS_LPC_ORDER / 2 + 1];
    double Q[TLCS_LPC_ORDER / 2 + 1];
    double *PQ[2];
    PQ[0] = P;
    PQ[1] = Q;

    a2nlsf_init(a_q16, P, Q, dd, order);

    /* Find roots by walking the cosine table, alternating P and Q */
    double *p = P;
    double xlo = (double)silk_cos_tab_q12[0];   /* Q12 */
    double ylo = eval_poly(p, xlo, dd);
    int root_ix;

    int nlsf_q15[TLCS_LPC_ORDER];   /* output in NLSF Q15 */

    if (ylo < 0.0) {
        nlsf_q15[0] = 0;
        p = Q;
        ylo = eval_poly(p, xlo, dd);
        root_ix = 1;
    } else {
        root_ix = 0;
    }

    int k = 1;
    int iter = 0;
    double thr = 0.0;

    while (1) {
        double xhi = (double)silk_cos_tab_q12[k];
        double yhi = eval_poly(p, xhi, dd);

        /* Detect zero crossing */
        if ((ylo <= 0.0 && yhi >= thr) || (ylo >= 0.0 && yhi <= -thr)) {
            if (yhi == 0.0) {
                thr = 1.0;
            } else {
                thr = 0.0;
            }

            /* Binary division to refine root location */
            int ffrac = -256;
            for (int m = 0; m < BIN_DIV_STEPS; m++) {
                double xmid = (xlo + xhi) * 0.5;
                /* Round to nearest (matches silk_RSHIFT_ROUND) */
                if (xmid > 0.0) xmid = floor(xmid + 0.5);
                else             xmid = ceil(xmid - 0.5);

                double ymid = eval_poly(p, xmid, dd);

                if ((ylo <= 0.0 && ymid >= 0.0) || (ylo >= 0.0 && ymid <= 0.0)) {
                    xhi = xmid;
                    yhi = ymid;
                } else {
                    xlo = xmid;
                    ylo = ymid;
                    ffrac += (128 >> m);
                }
            }

            /* Linear interpolation for fractional part */
            double abs_ylo = ylo < 0.0 ? -ylo : ylo;
            if (abs_ylo < 65536.0) {
                double den = ylo - yhi;
                double nom = ylo * (double)(1 << (8 - BIN_DIV_STEPS)) + den * 0.5;
                if (den != 0.0) {
                    ffrac += (int)(nom / den);
                }
            } else {
                double denom = (ylo - yhi) / (double)(1 << (8 - BIN_DIV_STEPS));
                if (denom != 0.0) {
                    ffrac += (int)(ylo / denom);
                }
            }

            int nlsf_val = k * 256 + ffrac;
            if (nlsf_val > 32767) nlsf_val = 32767;
            if (nlsf_val < 0) nlsf_val = 0;
            nlsf_q15[root_ix] = nlsf_val;

            root_ix++;
            if (root_ix >= order) {
                break;   /* Found all roots */
            }

            /* Alternate between P and Q */
            p = PQ[root_ix & 1];

            xlo = (double)silk_cos_tab_q12[k - 1];
            ylo = (1 - (root_ix & 2)) * 4096.0;   /* ±4096 = ±(1<<12) */
        } else {
            k++;
            xlo = xhi;
            ylo = yhi;
            thr = 0.0;

            if (k > LSF_COS_TAB_SZ) {
                iter++;
                if (iter > MAX_A2NLSF_ITER) {
                    /* Fallback: white spectrum */
                    for (int i = 0; i < order; i++) {
                        lsp_out[i] = MY_PI * (float)(i + 1) / (float)(order + 1);
                    }
                    tlcs_lsp_stabilize(lsp_out, order, 0.005f);
                    return;
                }

                /* Bandwidth expansion and retry */
                double chirp = (65536.0 - (double)(1 << iter)) / 65536.0;
                double g = chirp;
                for (int i = 0; i < order; i++) {
                    a_q16[i] *= g;
                    g *= chirp;
                }

                a2nlsf_init(a_q16, P, Q, dd, order);
                p = P;
                xlo = (double)silk_cos_tab_q12[0];
                ylo = eval_poly(p, xlo, dd);
                if (ylo < 0.0) {
                    nlsf_q15[0] = 0;
                    p = Q;
                    ylo = eval_poly(p, xlo, dd);
                    root_ix = 1;
                } else {
                    root_ix = 0;
                }
                k = 1;
            }
        }
    }

    /* Convert NLSF Q15 back to radians */
    for (int i = 0; i < order; i++) {
        lsp_out[i] = (float)nlsf_q15[i] * (MY_PI / 32768.0f);
    }
    tlcs_lsp_stabilize(lsp_out, order, 0.005f);
}

/* ================================================================== */
/* LSP -> LPC reconstruction                                           */
/* ================================================================== */

/* ================================================================== */
/* LSP -> LPC reconstruction (SILK NLSF2A algorithm, float)            */
/* ================================================================== */

/*
 * SILK ordering tables — interleave LSFs for better numerical accuracy
 * in the polynomial convolution.
 */
static const unsigned char silk_ordering_16[16] = {
    0, 15, 8, 7, 4, 11, 12, 3, 2, 13, 10, 5, 6, 9, 14, 1
};
static const unsigned char silk_ordering_10[10] = {
    0, 9, 6, 3, 4, 5, 8, 1, 2, 7
};
static const unsigned char silk_ordering_4[4] = {
    0, 3, 2, 1
};

/*
 * Build polynomial via convolution (SILK find_poly), in double precision.
 * out[0..dd], cLSF = interleaved 2*cos(LSF) values.
 */
static void nlsf2a_find_poly(double *out, const double *cLSF, int dd)
{
    /* All values are in QA (=16) fixed-point equivalent.
     * Multiplications produce QA*2, so we divide by 2^QA after each multiply.
     * This matches SILK's silk_RSHIFT_ROUND64(silk_SMULL(ftmp, out[k]), QA). */
    const double QA_SCALE = 65536.0;  /* 1 << 16 */

    out[0] = QA_SCALE;  /* 1.0 in QA */
    out[1] = -cLSF[0];
    for (int k = 1; k < dd; k++) {
        double ftmp = cLSF[2 * k];
        out[k + 1] = 2.0 * out[k - 1] - floor(ftmp * out[k] / QA_SCALE + 0.5);
        for (int n = k; n > 1; n--) {
            out[n] += out[n - 2] - floor(ftmp * out[n - 1] / QA_SCALE + 0.5);
        }
        out[1] -= ftmp;
    }
}

void tlcs_lsp_to_lpc(const float *lsp, int order, float *lpc_out)
{
    /*
     * SILK NLSF2A algorithm in float/double.
     * 1. Convert LSPs (radians) to NLSF Q15
     * 2. Use piecewise-linear cosine table to get 2*cos(LSF) values
     * 3. Build P and Q polynomials via convolution with ordering trick
     * 4. Convert to LPC coefficients
     * 5. Stability check with bandwidth expansion fallback
     */
    int dd = order / 2;

    /* Select ordering table */
    const unsigned char *ordering;
    if (order == 16)      ordering = silk_ordering_16;
    else if (order == 10) ordering = silk_ordering_10;
    else if (order == 4)  ordering = silk_ordering_4;
    else {
        /* Fallback: identity ordering for other orders */
        static unsigned char identity[TLCS_LPC_ORDER];
        for (int i = 0; i < order; i++) identity[i] = (unsigned char)i;
        ordering = identity;
    }

    /* Convert LSP (radians) -> NLSF Q15 -> 2*cos(LSF) via table lookup */
    double cos_LSF[TLCS_LPC_ORDER];
    for (int k = 0; k < order; k++) {
        /* LSP to NLSF Q15: nlsf = round(lsp * 32768 / pi) */
        int nlsf = (int)(lsp[k] * (32768.0f / MY_PI) + 0.5f);
        if (nlsf < 0) nlsf = 0;
        if (nlsf > 32767) nlsf = 32767;

        /* Piecewise linear interpolation from cosine table (same as SILK) */
        int f_int = nlsf >> 8;          /* 0..127 */
        int f_frac = nlsf - (f_int << 8); /* 0..255 */

        if (f_int >= LSF_COS_TAB_SZ) f_int = LSF_COS_TAB_SZ - 1;

        int cos_val = silk_cos_tab_q12[f_int];               /* Q12 */
        int delta   = silk_cos_tab_q12[f_int + 1] - cos_val; /* Q12 */

        /* Linear interpolation, result in Q16 equivalent:
         * (cos_val << 8) + delta * f_frac, then shift to get Q16-scaled double.
         * In SILK: silk_RSHIFT_ROUND(silk_LSHIFT(cos_val,8) + delta*f_frac, 20-QA)
         * where QA=16, so shift by 4, with rounding. */
        double interp = (double)(cos_val * 256 + delta * f_frac);
        /* Shift right by 4 with rounding → divide by 16 */
        cos_LSF[ordering[k]] = (interp + 8.0) / 16.0;  /* now in QA=16 scale */
    }

    /* Generate even and odd polynomials using convolution */
    double P[TLCS_LPC_ORDER / 2 + 1];
    double Q_arr[TLCS_LPC_ORDER / 2 + 1];
    nlsf2a_find_poly(P, &cos_LSF[0], dd);
    nlsf2a_find_poly(Q_arr, &cos_LSF[1], dd);

    /* Convert P, Q to LPC coefficients in QA+1 scale, then to float */
    /* a[k] = -(Q[k+1] - Q[k]) - (P[k+1] + P[k])  (first half)
     * a[d-k-1] = (Q[k+1] - Q[k]) - (P[k+1] + P[k])  (second half) */
    double a_qa1[TLCS_LPC_ORDER];
    for (int k = 0; k < dd; k++) {
        double Ptmp = P[k + 1] + P[k];
        double Qtmp = Q_arr[k + 1] - Q_arr[k];
        a_qa1[k]             = -Qtmp - Ptmp;    /* QA+1 */
        a_qa1[order - k - 1] =  Qtmp - Ptmp;    /* QA+1 */
    }

    /* Convert QA+1 to float LPC: a[i+1] = -a_qa1[i] / (1 << (QA+1)) */
    /* QA = 16, so divide by 2^17 = 131072 */
    lpc_out[0] = 1.0f;
    for (int i = 0; i < order; i++) {
        lpc_out[i + 1] = (float)(-a_qa1[i] / 131072.0);
    }

    /* Stability check: verify LPC inverse prediction gain > 0.
     * If unstable, apply progressive bandwidth expansion (same as SILK). */
    for (int attempt = 0; attempt < 16; attempt++) {
        /* Check stability by computing reflection coefficients via
         * Schur/step-down; if any |k_i| >= 1, filter is unstable. */
        int stable = 1;
        double atmp[TLCS_LPC_ORDER + 1];
        for (int i = 0; i <= order; i++) atmp[i] = (double)lpc_out[i];

        for (int i = order; i >= 1; i--) {
            double ki = atmp[i];
            if (ki >= 1.0 || ki <= -1.0) { stable = 0; break; }
            double div = 1.0 - ki * ki;
            if (div <= 0.0) { stable = 0; break; }
            double prev[TLCS_LPC_ORDER + 1];
            for (int j = 0; j <= i; j++) prev[j] = atmp[j];
            for (int j = 1; j < i; j++) {
                atmp[j] = (prev[j] - ki * prev[i - j]) / div;
            }
        }

        if (stable) break;

        /* Apply bandwidth expansion to a_qa1 and reconvert */
        double chirp = (131072.0 - (double)(2 << attempt)) / 131072.0;
        double g = chirp;
        for (int i = 0; i < order; i++) {
            a_qa1[i] *= g;
            g *= chirp;
        }
        lpc_out[0] = 1.0f;
        for (int i = 0; i < order; i++) {
            lpc_out[i + 1] = (float)(-a_qa1[i] / 131072.0);
        }
    }
}

/* ================================================================== */
/* LSP interpolation and stabilisation                                 */
/* ================================================================== */

void tlcs_lsp_interpolate(const float *prev, const float *curr, float alpha,
                          int order, float *out)
{
    float a = alpha;
    float b = 1.0f - alpha;
    for (int i = 0; i < order; i++) {
        out[i] = b * prev[i] + a * curr[i];
    }
}

void tlcs_lsp_stabilize(float *lsp, int order, float min_gap)
{
    const float PI = 3.14159265f;

    /* Clamp */
    for (int i = 0; i < order; i++) {
        if (lsp[i] < 0.001f) lsp[i] = 0.001f;
        if (lsp[i] > PI - 0.001f) lsp[i] = PI - 0.001f;
    }

    /* Enforce ordering */
    for (int i = 1; i < order; i++) {
        if (lsp[i] <= lsp[i - 1] + min_gap) {
            lsp[i] = lsp[i - 1] + min_gap;
        }
    }

    /* Re-clamp */
    for (int i = 0; i < order; i++) {
        if (lsp[i] > PI - 0.001f) lsp[i] = PI - 0.001f;
    }
}

/* ================================================================== */
/* LPC synthesis filter                                                */
/* ================================================================== */

void tlcs_lpc_synthesis(const float *exc, int len, const float *lpc, int order,
                        float *state, float *out)
{
    /*
     * s[n] = exc[n] - sum_{k=1}^{order} a[k] * s[n-k]
     * state[0] = s[n-1], state[1] = s[n-2], etc.
     */
    for (int i = 0; i < len; i++) {
        float val = exc[i];
        for (int k = 0; k < order; k++) {
            val -= lpc[k + 1] * state[k];
        }
        out[i] = val;
        /* Shift state */
        for (int k = order - 1; k > 0; k--) {
            state[k] = state[k - 1];
        }
        state[0] = val;
    }
}

void tlcs_lpc_zero_state_response(const float *lpc, int order,
                                  const float *state, int len, float *out)
{
    float mem[TLCS_LPC_ORDER];
    memcpy(mem, state, order * sizeof(float));

    for (int i = 0; i < len; i++) {
        float val = 0.0f;
        for (int k = 0; k < order; k++) {
            val -= lpc[k + 1] * mem[k];
        }
        out[i] = val;
        for (int k = order - 1; k > 0; k--) {
            mem[k] = mem[k - 1];
        }
        mem[0] = val;
    }
}

void tlcs_lpc_impulse_response(const float *lpc, int order, int len, float *h)
{
    float mem[TLCS_LPC_ORDER];
    memset(mem, 0, sizeof(mem));

    for (int i = 0; i < len; i++) {
        float exc = (i == 0) ? 1.0f : 0.0f;
        float val = exc;
        for (int k = 0; k < order; k++) {
            val -= lpc[k + 1] * mem[k];
        }
        h[i] = val;
        for (int k = order - 1; k > 0; k--) {
            mem[k] = mem[k - 1];
        }
        mem[0] = val;
    }
}
