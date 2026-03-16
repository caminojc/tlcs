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
/* LPC -> LSP via Chebyshev polynomial evaluation                      */
/* ================================================================== */

/*
 * Evaluate the Chebyshev polynomial series at cos(omega).
 * coef: coefficients of the polynomial, length = (order/2 + 1).
 */
static float cheb_eval(const float *coef, int m, float x)
{
    float b0, b1, b2;
    b1 = 0.0f;
    b0 = 0.0f;
    for (int k = m; k >= 1; k--) {
        b2 = b1;
        b1 = b0;
        b0 = 2.0f * x * b1 - b2 + coef[k];
    }
    return x * b0 - b1 + 0.5f * coef[0];
}

/* Evaluate P(omega) or Q(omega) directly from LPC coefficients.
 * P(omega) = sum_{k=0}^{m} p[k] * cos(k*omega)
 * where p[k] = a[k] + a[order+1-k], q[k] = a[k] - a[order+1-k]
 * and a[order+1] = 0.
 * For even order, P has m+1 = order/2 + 1 roots in (0,pi),
 * Q has m roots in (0,pi). Total = order roots.
 */
void tlcs_lpc_to_lsp(const float *lpc, int order, float *lsp_out)
{
    /* Build P, Q polynomials, deconvolve trivial roots, evaluate as Chebyshev. */
    int m = order / 2;  /* 8 */
    float a_ext[TLCS_LPC_ORDER + 2];
    for (int i = 0; i <= order; i++) a_ext[i] = lpc[i];
    a_ext[order + 1] = 0.0f;

    float p_poly[TLCS_LPC_ORDER + 2], q_poly[TLCS_LPC_ORDER + 2];
    for (int i = 0; i <= order + 1; i++) {
        p_poly[i] = a_ext[i] + a_ext[order + 1 - i];
        q_poly[i] = a_ext[i] - a_ext[order + 1 - i];
    }

    /* Deconvolve: P/(1+z^-1) and Q/(1-z^-1) */
    float f1[TLCS_LPC_ORDER + 1], f2[TLCS_LPC_ORDER + 1];
    f1[0] = p_poly[0];
    for (int i = 1; i <= order; i++) f1[i] = p_poly[i] + f1[i - 1];
    f2[0] = q_poly[0];
    for (int i = 1; i <= order; i++) f2[i] = q_poly[i] + f2[i - 1];

    /* Build Chebyshev coefficients from symmetric polynomial first half */
    float c1[TLCS_LPC_ORDER / 2 + 1], c2[TLCS_LPC_ORDER / 2 + 1];
    c1[0] = f1[m];
    for (int k = 1; k <= m; k++) c1[k] = 2.0f * f1[m - k];
    c2[0] = f2[m];
    for (int k = 1; k <= m; k++) c2[k] = 2.0f * f2[m - k];

    /* Find roots by grid search + bisection on Chebyshev polynomials */
    const int GRID = 1024;
    int nroots = 0;

    float prev_f1 = cheb_eval(c1, m, 1.0f);
    float prev_f2 = cheb_eval(c2, m, 1.0f);

    for (int i = 1; i <= GRID && nroots < order; i++) {
        float omega = 3.14159265f * (float)i / (float)GRID;
        float x = cosf(omega);
        float cur_f1 = cheb_eval(c1, m, x);
        float cur_f2 = cheb_eval(c2, m, x);

        if (prev_f1 * cur_f1 < 0.0f && nroots < order) {
            float lo = 3.14159265f * (float)(i - 1) / (float)GRID;
            float hi = omega;
            for (int b = 0; b < 24; b++) {
                float mid = 0.5f * (lo + hi);
                float v = cheb_eval(c1, m, cosf(mid));
                float vlo = cheb_eval(c1, m, cosf(lo));
                if (v * vlo <= 0.0f) hi = mid; else lo = mid;
            }
            lsp_out[nroots++] = 0.5f * (lo + hi);
        }
        if (prev_f2 * cur_f2 < 0.0f && nroots < order) {
            float lo = 3.14159265f * (float)(i - 1) / (float)GRID;
            float hi = omega;
            for (int b = 0; b < 24; b++) {
                float mid = 0.5f * (lo + hi);
                float v = cheb_eval(c2, m, cosf(mid));
                float vlo = cheb_eval(c2, m, cosf(lo));
                if (v * vlo <= 0.0f) hi = mid; else lo = mid;
            }
            lsp_out[nroots++] = 0.5f * (lo + hi);
        }
        prev_f1 = cur_f1;
        prev_f2 = cur_f2;
    }

    /* Sort */
    for (int i = 0; i < nroots - 1; i++)
        for (int j = i + 1; j < nroots; j++)
            if (lsp_out[j] < lsp_out[i]) {
                float tmp = lsp_out[i]; lsp_out[i] = lsp_out[j]; lsp_out[j] = tmp;
            }

    if (nroots < order) {
        for (int i = 0; i < order; i++)
            lsp_out[i] = 3.14159265f * (float)(i + 1) / (float)(order + 1);
    }
    tlcs_lsp_stabilize(lsp_out, order, 0.005f);

#if 0 /* OLD BROKEN CODE — kept for reference */
    /* Direct evaluation of P(omega) and Q(omega) on unit circle.
     * P(w) = 2*Re[A(e^jw) * e^{j(p+1)w/2}] = 2*cos((p+1)w/2) + 2*sum...
     * Simpler: P(w) = sum_{k=0}^{p+1} (a[k]+a[p+1-k]) * cos((k-(p+1)/2)*w)
     * But easiest: just evaluate A(e^jw) directly and compute P,Q from it. */
    const int GRID = 1024;
    int nroots = 0;
    int p1 = order + 1;  /* 17 for order 16 */

    /* Evaluate P(w) = Re[A(e^jw)] * 2cos(p1*w/2) + Im[A(e^jw)] * 2sin(p1*w/2)
     * Actually: P(e^jw) = A(e^jw) + e^{-jp1w}A(e^{-jw})
     * = A(e^jw) + conj(A(e^jw)) * e^{-jp1w}
     * Let A(e^jw) = Ar + jAi, then:
     * P(w) = 2*Ar*cos(p1*w/2)*cos(w*(p1/2)) + 2*Ai*sin(...)
     * Simplest correct: P(w) = 2 * sum_{k=0}^{p/2} f1[k] * cos(kw)
     * where f1[k] = a[k] + a[p1-k] for the DECONVOLVED polynomial.
     *
     * Let's just evaluate P and Q directly: */

    /* P(w) = sum_{k=0}^{p1} a_ext[k] * cos(kw) where a_ext = [a; 0] + reversed
     * This is just 2*Re[A(e^jw) * e^{jp1w/2}] */

    /* Simplest approach: evaluate A(e^jw) and compute:
     * P(w) = |A(e^jw) + e^{-j(p+1)w} * A(e^{-jw})| with phase
     * For LSP: we need the REAL part only */

    float a_ext[TLCS_LPC_ORDER + 2];
    for (int i = 0; i <= order; i++) a_ext[i] = lpc[i];
    a_ext[p1] = 0.0f;

    /* P_coeffs[k] = a[k] + a[p1-k], Q_coeffs[k] = a[k] - a[p1-k], k=0..p1 */
    float pc[TLCS_LPC_ORDER + 2], qc[TLCS_LPC_ORDER + 2];
    for (int k = 0; k <= p1; k++) {
        pc[k] = a_ext[k] + a_ext[p1 - k];
        qc[k] = a_ext[k] - a_ext[p1 - k];
    }

    /* Evaluate P(w) = sum pc[k]*cos(kw), Q(w) = sum qc[k]*cos(kw) */
    /* Note: Q(0)=0 always (since q[k]+q[p1-k]=0), Q(pi)=0 always.
     * P(pi) might be 0. These known roots must be excluded. */

    float prev_p = 0, prev_q = 0;
    for (int k = 0; k <= p1; k++) { prev_p += pc[k]; prev_q += qc[k]; }
    /* prev_p = P(0), prev_q = Q(0) = 0 always → skip Q at 0 */

    for (int i = 1; i <= GRID && nroots < order; i++) {
        float omega = 3.14159265f * (float)i / (float)GRID;
        float cur_p = 0, cur_q = 0;
        for (int k = 0; k <= p1; k++) {
            float cw = cosf((float)k * omega);
            cur_p += pc[k] * cw;
            cur_q += qc[k] * cw;
        }

        if (prev_p * cur_p < 0.0f && nroots < order) {
            float lo = 3.14159265f * (float)(i - 1) / (float)GRID;
            float hi = omega;
            for (int b = 0; b < 24; b++) {
                float mid = 0.5f * (lo + hi);
                float v = 0;
                for (int k = 0; k <= p1; k++) v += pc[k] * cosf((float)k * mid);
                float vlo = 0;
                for (int k = 0; k <= p1; k++) vlo += pc[k] * cosf((float)k * lo);
                if (v * vlo <= 0.0f) hi = mid; else lo = mid;
            }
            lsp_out[nroots++] = 0.5f * (lo + hi);
        }
        /* Skip Q sign changes at omega≈0 and omega≈pi (trivial roots) */
        if (prev_q * cur_q < 0.0f && nroots < order &&
            omega > 0.01f && omega < 3.13f) {
            float lo = 3.14159265f * (float)(i - 1) / (float)GRID;
            float hi = omega;
            for (int b = 0; b < 24; b++) {
                float mid = 0.5f * (lo + hi);
                float v = 0;
                for (int k = 0; k <= p1; k++) v += qc[k] * cosf((float)k * mid);
                float vlo = 0;
                for (int k = 0; k <= p1; k++) vlo += qc[k] * cosf((float)k * lo);
                if (v * vlo <= 0.0f) hi = mid; else lo = mid;
            }
            lsp_out[nroots++] = 0.5f * (lo + hi);
        }
        prev_p = cur_p;
        prev_q = cur_q;
    }

    /* Sort */
    for (int i = 0; i < nroots - 1; i++)
        for (int j = i + 1; j < nroots; j++)
            if (lsp_out[j] < lsp_out[i]) {
                float tmp = lsp_out[i]; lsp_out[i] = lsp_out[j]; lsp_out[j] = tmp;
            }

    if (nroots < order) {
        for (int i = 0; i < order; i++)
            lsp_out[i] = 3.14159265f * (float)(i + 1) / (float)(order + 1);
    }
    tlcs_lsp_stabilize(lsp_out, order, 0.005f);
#endif /* OLD BROKEN CODE */
}

/* ================================================================== */
/* LSP -> LPC reconstruction                                           */
/* ================================================================== */

void tlcs_lsp_to_lpc(const float *lsp, int order, float *lpc_out)
{
    /*
     * Build P'(z) and Q'(z) from their roots (pairs of cos(w_k)),
     * then P = (1+z^-1)*P', Q = (1-z^-1)*Q', A = 0.5*(P+Q).
     *
     * Even-indexed LSPs -> P, odd-indexed -> Q.
     */
    int m = order / 2;

    /* P'(z): product of (1 - 2*cos(w)*z^-1 + z^-2) for even LSPs */
    float p[TLCS_LPC_ORDER + 2];
    float q[TLCS_LPC_ORDER + 2];
    memset(p, 0, sizeof(p));
    memset(q, 0, sizeof(q));
    p[0] = 1.0f;
    q[0] = 1.0f;

    for (int i = 0; i < m; i++) {
        float cw_p = -2.0f * cosf(lsp[2 * i]);
        float cw_q = -2.0f * cosf(lsp[2 * i + 1]);

        /* Convolve P with [1, cw_p, 1] */
        for (int j = 2 * (i + 1); j >= 2; j--) {
            p[j] += cw_p * p[j - 1] + p[j - 2];
        }
        p[1] += cw_p * p[0];

        /* Convolve Q with [1, cw_q, 1] */
        for (int j = 2 * (i + 1); j >= 2; j--) {
            q[j] += cw_q * q[j - 1] + q[j - 2];
        }
        q[1] += cw_q * q[0];
    }

    /* P(z) = P'(z) * (1 + z^-1),  Q(z) = Q'(z) * (1 - z^-1) */
    float pp[TLCS_LPC_ORDER + 2];
    float qq[TLCS_LPC_ORDER + 2];
    memset(pp, 0, sizeof(pp));
    memset(qq, 0, sizeof(qq));

    for (int i = 0; i <= order; i++) {
        pp[i] += p[i];
        pp[i + 1] += p[i];
        qq[i] += q[i];
        qq[i + 1] -= q[i];
    }

    /* A(z) = 0.5 * (P(z) + Q(z)) */
    lpc_out[0] = 1.0f;
    for (int i = 1; i <= order; i++) {
        lpc_out[i] = 0.5f * (pp[i] + qq[i]);
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
