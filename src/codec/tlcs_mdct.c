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
 * MDCT of size N (N = 2*frame_size, M = frame_size):
 *   1. Window + fold N samples → M real values (standard MDCT fold)
 *   2. DCT-IV of folded signal via 2M-point complex FFT:
 *      a. Pre-twiddle: z[n] = folded[n] * exp(-j*pi*n/(2M)), zero-pad to 2M
 *      b. 2M-point complex FFT (PFFFT)
 *      c. Post-twiddle: X[k] = Re(Z[k]*exp(-j*pi*(2k+1)/(4M)))
 *
 * Complexity: O(M log M) vs O(M²) for brute-force DCT-IV.
 *
 * PFFFT complex FFT requires N % 16 == 0.
 * frame_size=320 → 2M=640, 640%16=0 ✓
 * frame_size=160 → 2M=320, 320%16=0 ✓
 * ══════════════════════════════════════════════════════════════════════════ */

/* Lazy-initialized PFFFT complex FFT setups (size = 2 * frame_size) */
static PFFFT_Setup *cfft_setup_640 = NULL;  /* for frame_size=320, 2M=640 */
static PFFFT_Setup *cfft_setup_320 = NULL;  /* for frame_size=160, 2M=320 */

static PFFFT_Setup *get_cfft_setup(int32_t two_m)
{
    if (two_m == 640) {
        if (!cfft_setup_640) cfft_setup_640 = pffft_new_setup(640, PFFFT_COMPLEX);
        return cfft_setup_640;
    }
    if (two_m == 320) {
        if (!cfft_setup_320) cfft_setup_320 = pffft_new_setup(320, PFFFT_COMPLEX);
        return cfft_setup_320;
    }
    return NULL;  /* unsupported size — fallback to brute force */
}

/* ── Forward MDCT ──────────────────────────────────────────────────────── */
void tlcs_mdct_forward(const float *prev_block, const float *curr_block,
                        float *spec_out, int32_t frame_size)
{
    int32_t N = 2 * frame_size;
    int32_t M = frame_size;
    int32_t L = 2 * M;     /* complex FFT size */

    PFFFT_Setup *setup = get_cfft_setup(L);

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

    /* ── Step 2: Pre-twiddle + 2M-point complex FFT ──────────────────── */
    /* Compute DCT-IV of folded[0..M-1] via 2M-point complex FFT.
     *
     * Pre-twiddle: z[n] = folded[n] * exp(-j*pi*n/(2M))  for n=0..M-1
     *              z[n] = 0                                for n=M..2M-1
     *
     * Then DFT_{2M}[k](z) = sum_{n<M} folded[n]*exp(-j*pi*n*(2k+1)/(2M))
     *
     * Post-twiddle:
     * DCT-IV[k] = Re(DFT[k]) * cos(alpha_k) + Im(DFT[k]) * sin(alpha_k)
     * where alpha_k = pi*(2k+1)/(4M)
     */
    float *fft_in  = (float *)pffft_aligned_malloc((size_t)(2 * L) * sizeof(float));
    float *fft_out = (float *)pffft_aligned_malloc((size_t)(2 * L) * sizeof(float));

    /* Zero the entire input (covers zero-padding for n=M..2M-1) */
    memset(fft_in, 0, (size_t)(2 * L) * sizeof(float));

    /* Pre-twiddle: z[n] = folded[n] * exp(-j*pi*n/(2M)) for n=0..M-1 */
    float inv_2M = 1.0f / (float)(2 * M);
    for (int32_t n = 0; n < M; n++) {
        float angle = (float)M_PI * (float)n * inv_2M;
        fft_in[2 * n]     =  folded[n] * cosf(angle);   /* real */
        fft_in[2 * n + 1] = -folded[n] * sinf(angle);   /* imag */
    }
    pffft_aligned_free(folded);

    /* ── Step 3: 2M-point complex FFT ────────────────────────────────── */
    pffft_transform_ordered(setup, fft_in, fft_out, NULL, PFFFT_FORWARD);
    pffft_aligned_free(fft_in);

    /* ── Step 4: Post-twiddle ────────────────────────────────────────── */
    /* DCT-IV[k] = Re(DFT[k])*cos(alpha_k) + Im(DFT[k])*sin(alpha_k)
     * where alpha_k = pi*(2k+1)/(4M)
     *
     * No extra scaling: MDCT = DCT-IV(folded) by the folding identity. */
    for (int32_t k = 0; k < M; k++) {
        float re = fft_out[2 * k];
        float im = fft_out[2 * k + 1];
        float alpha = (float)M_PI * (float)(2 * k + 1) / (float)(4 * M);
        spec_out[k] = re * cosf(alpha) + im * sinf(alpha);
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
