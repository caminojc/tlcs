#include "tlcs_mdct.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Forward MDCT with sine window.
 * Transform size N = 2 * frame_size.
 * Input: [prev_block(frame_size) | curr_block(frame_size)] = N samples.
 * Output: M = frame_size spectral coefficients.
 *
 * X[k] = sum_{n=0}^{N-1} x[n] * w[n] * cos(pi/M * (n + 0.5 + M/2) * (k + 0.5))
 * where M = frame_size, w[n] = sin(pi * (n + 0.5) / N)
 */
void tlcs_mdct_forward(const float *prev_block, const float *curr_block,
                        float *spec_out, int32_t frame_size)
{
    int32_t N = 2 * frame_size;
    int32_t M = frame_size;
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
}

/* Inverse MDCT with overlap-add.
 * Transform size N = 2 * frame_size.
 * Input: M = frame_size spectral coefficients.
 * Output: frame_size time-domain samples via overlap-add.
 *
 * y[n] = (2/M) * sum_{k=0}^{M-1} X[k] * cos(pi/M * (n + 0.5 + M/2) * (k + 0.5))
 * yw[n] = y[n] * w[n]   (windowed IMDCT)
 * out[n] = overlap_buf[n] + yw[n]   for n = 0..frame_size-1
 * overlap_buf[n] = yw[frame_size + n]   for n = 0..frame_size-1
 */
void tlcs_mdct_inverse(const float *spec_in, float *time_out,
                        float *overlap_buf, int32_t frame_size)
{
    int32_t N = 2 * frame_size;
    int32_t M = frame_size;
    float scale = 2.0f / (float)M;
    float inv_M = 1.0f / (float)M;
    float inv_N = 1.0f / (float)N;
    float half_M = (float)M * 0.5f;

    /* Stack buffer for IMDCT output (max 640 floats) */
    float y[2 * TLCS_MAX_FRAME_SIZE];

    /* Compute IMDCT */
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

    /* Overlap-add: output = saved_overlap + first half of windowed IMDCT */
    for (int32_t n = 0; n < frame_size; n++) {
        time_out[n] = overlap_buf[n] + y[n];
    }

    /* Save second half for next frame's overlap */
    for (int32_t n = 0; n < frame_size; n++) {
        overlap_buf[n] = y[frame_size + n];
    }
}
