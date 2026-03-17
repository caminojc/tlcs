#include "tlcs_mdct.h"
#include "pffft.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ══════════════════════════════════════════════════════════════════════════
 * FFT-accelerated forward MDCT using PFFFT.
 *
 * MDCT of size N (N = 2*frame_size, M = frame_size) via M/2-point
 * complex FFT with pre/post twiddle rotations (Opus/CELT algorithm).
 *
 * Steps:
 *   1. Window + fold N samples → M real values (standard MDCT fold)
 *   2. Pack M reals as M/2 complex pairs, pre-rotate by twiddle
 *   3. M/2-point complex FFT (PFFFT)
 *   4. Post-rotate to extract DCT-IV coefficients
 *
 * Complexity: O(M log M) vs O(M²) for brute-force DCT-IV.
 *
 * PFFFT complex FFT requires N % 16 == 0.
 * frame_size=320 → M/2=160, 160%16=0 ✓
 * frame_size=160 → M/2=80,   80%16=0 ✓
 * ══════════════════════════════════════════════════════════════════════════ */

/* Lazy-initialized PFFFT complex FFT setups (size = frame_size / 2) */
static PFFFT_Setup *cfft_setup_160 = NULL;  /* for frame_size=320, M/2=160 */
static PFFFT_Setup *cfft_setup_80  = NULL;  /* for frame_size=160, M/2=80  */

static PFFFT_Setup *get_cfft_setup(int32_t half_m)
{
    if (half_m == 160) {
        if (!cfft_setup_160) cfft_setup_160 = pffft_new_setup(160, PFFFT_COMPLEX);
        return cfft_setup_160;
    }
    if (half_m == 80) {
        if (!cfft_setup_80) cfft_setup_80 = pffft_new_setup(80, PFFFT_COMPLEX);
        return cfft_setup_80;
    }
    return NULL;  /* unsupported size — fallback to brute force */
}

/* ── Forward MDCT ──────────────────────────────────────────────────────── */
void tlcs_mdct_forward(const float *prev_block, const float *curr_block,
                        float *spec_out, int32_t frame_size)
{
    int32_t N = 2 * frame_size;
    int32_t M = frame_size;
    int32_t M2 = M / 2;    /* complex FFT size */

    PFFFT_Setup *setup = get_cfft_setup(M2);

    if (!setup) {
        /* Fallback: brute-force for unsupported sizes */
        float inv_M = 1.0f / (float)M;
        float inv_N = 1.0f / (float)N;
        float half_M = (float)M * 0.5f;
        for (int32_t k = 0; k < M; k++) {
            float sum = 0.0f;
            float kk = (float)k + 0.5f;
            for (int32_t n = 0; n < N; n++) {
                float x = (n < frame_size) ? prev_block[n] : curr_block[n - frame_size];
                float w = sinf((float)M_PI * ((float)n + 0.5f) * inv_N);
                float angle = (float)M_PI * inv_M * ((float)n + 0.5f + half_M) * kk;
                sum += x * w * cosf(angle);
            }
            spec_out[k] = sum;
        }
        return;
    }

    /* ── Step 1: Window and fold ─────────────────────────────────────── */

    /* Assemble windowed input: [prev_block | curr_block] with sine window */
    float *buf = (float *)pffft_aligned_malloc((size_t)N * sizeof(float));
    float inv_N = 1.0f / (float)N;
    for (int32_t n = 0; n < N; n++) {
        float x = (n < frame_size) ? prev_block[n] : curr_block[n - frame_size];
        float w = sinf((float)M_PI * ((float)n + 0.5f) * inv_N);
        buf[n] = x * w;
    }

    /* Fold N-point windowed signal into M-point buffer:
     * folded[n] = -buf[3N/4 + n] - buf[3N/4 - 1 - n]  for n < N/4
     * folded[n] =  buf[n - N/4]  - buf[3N/4 - 1 - n]  for n >= N/4 */
    int32_t N4 = N / 4;
    float *folded = (float *)pffft_aligned_malloc((size_t)M * sizeof(float));
    for (int32_t n = 0; n < N4; n++) {
        folded[n] = -buf[N - N4 + n] - buf[N - N4 - 1 - n];
    }
    for (int32_t n = N4; n < M; n++) {
        folded[n] = buf[n - N4] - buf[N - N4 - 1 - n];
    }
    pffft_aligned_free(buf);

    /* ── Step 2: Pre-rotation ────────────────────────────────────────── */
    /* Pack folded[0..M-1] as M/2 complex pairs and multiply by twiddle.
     *
     * Complex input:  z[i] = folded[2i] + j*folded[2i+1]
     * Twiddle:        t[i] = exp(-j * 2*pi*(i + 1/8) / N)
     *                      = exp(-j * pi*(8i+1) / (4M))
     * Pre-rotated:    z'[i] = z[i] * t[i]
     *
     * This is the same twiddle as Opus/CELT (cos(2*pi*(i+0.125)/N)). */
    float *fft_in = (float *)pffft_aligned_malloc((size_t)(2 * M2) * sizeof(float));
    for (int32_t i = 0; i < M2; i++) {
        float angle = (float)M_PI * (float)(8 * i + 1) / (float)(4 * M);
        float t_cos = cosf(angle);
        float t_sin = sinf(angle);
        float re = folded[2 * i];
        float im = folded[2 * i + 1];
        /* z'[i] = (re + j*im) * (cos - j*sin) = (re*cos + im*sin) + j*(im*cos - re*sin) */
        fft_in[2 * i]     = re * t_cos + im * t_sin;
        fft_in[2 * i + 1] = im * t_cos - re * t_sin;
    }
    pffft_aligned_free(folded);

    /* ── Step 3: M/2-point complex FFT ───────────────────────────────── */
    float *fft_out = (float *)pffft_aligned_malloc((size_t)(2 * M2) * sizeof(float));
    pffft_transform_ordered(setup, fft_in, fft_out, NULL, PFFFT_FORWARD);
    pffft_aligned_free(fft_in);

    /* ── Step 4: Post-rotation ───────────────────────────────────────── */
    /* For FFT output Z[k], apply twiddle and extract MDCT coefficients.
     *
     * Twiddle:  t[k] = exp(-j * pi*(8k+1) / (4M))
     * Rotated:  w[k] = Z[k] * t[k]
     *
     * Then (following Opus/CELT sign convention for correct MDCT):
     *   out[2k]     = -(w[k].im * (-sin) - w[k].re * cos)  ... see below
     *   out[M-1-2k] = ...
     *
     * The Opus post-rotate extracts:
     *   yr = Z.im*t_msin - Z.re*t_cos   (= -(Z.re*cos + Z.im*sin))
     *   yi = Z.re*t_msin + Z.im*t_cos   (=  (Z.im*cos - Z.re*sin))
     *   out[2k]     = yr
     *   out[M-1-2k] = yi
     *
     * Scale by 2/N to match brute-force MDCT normalization. */
    float scale = 2.0f / (float)N;
    for (int32_t k = 0; k < M2; k++) {
        float angle = (float)M_PI * (float)(8 * k + 1) / (float)(4 * M);
        float t_cos = cosf(angle);
        float t_sin = sinf(angle);
        float zr = fft_out[2 * k];
        float zi = fft_out[2 * k + 1];
        /* yr = -(zr*cos + zi*sin), yi = zi*cos - zr*sin */
        float yr = -(zr * t_cos + zi * t_sin);
        float yi =   zi * t_cos - zr * t_sin;
        spec_out[2 * k]         = yr * scale;
        spec_out[M - 1 - 2 * k] = yi * scale;
    }

    pffft_aligned_free(fft_out);
}

/* ── Inverse MDCT with overlap-add ─────────────────────────────────────── */
void tlcs_mdct_inverse(const float *spec_in, float *time_out,
                        float *overlap_buf, int32_t frame_size)
{
    int32_t N = 2 * frame_size;
    int32_t M = frame_size;
    float scale = 2.0f / (float)M;
    float inv_N = 1.0f / (float)N;

    /* IMDCT: y[n] = (2/M) * sum_{k=0}^{M-1} X[k] * cos(pi/M * (n+0.5+M/2) * (k+0.5))
     * This is also O(N²) — but decode performance is already fine (0.7% RT).
     * Can be optimized to FFT-based later if needed. */
    float y[2 * TLCS_MAX_FRAME_SIZE];
    float half_M = (float)M * 0.5f;
    float inv_M = 1.0f / (float)M;

    for (int32_t n = 0; n < N; n++) {
        float sum = 0.0f;
        float nn = (float)n + 0.5f + half_M;
        for (int32_t k = 0; k < M; k++) {
            float angle = (float)M_PI * inv_M * nn * ((float)k + 0.5f);
            sum += spec_in[k] * cosf(angle);
        }
        y[n] = sum * scale;
    }

    /* Apply sine window */
    for (int32_t n = 0; n < N; n++) {
        float w = sinf((float)M_PI * ((float)n + 0.5f) * inv_N);
        y[n] *= w;
    }

    /* Overlap-add */
    for (int32_t n = 0; n < frame_size; n++)
        time_out[n] = overlap_buf[n] + y[n];
    for (int32_t n = 0; n < frame_size; n++)
        overlap_buf[n] = y[frame_size + n];
}
