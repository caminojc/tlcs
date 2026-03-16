#ifndef SMPL_CODEC_UTIL_H
#define SMPL_CODEC_UTIL_H

#include <math.h>
#include <stdint.h>

static inline float smpl_sigmoid(float x)
{
#if 1
    // avoid passing large exponents to exp below to avoid numerical issues like nan, inf etc when fast math used
    // expf(89) = inf, and expf(-89) is at de-normal range for float32. Limit to +-80 to be safe
    if (x > 80.0f) {
        return 1.0f;
    }
    if (x < -80.0f) {
        return 0.0f;
    }    
#endif    
    return 1.0f / (1.0f + expf(-x));
}

#ifdef __cplusplus
extern "C" {
#endif

    float smpl_dot_prod(const float a[], const float b[], int L);
    void smpl_sqrt_vec(float x[], int L);
    float smpl_nrg(const float x[], int N);
    float smpl_sum(const float x[], int N);
    float smpl_werr(const float *x, const float *y, const float *w);
    void smpl_matrix_mult_transp_16(const float C[], const float x[], float y[], int len_y, int len_x);
    void smpl_celp_q(const float num[], const float den[], int L, float Q[]);
    void smpl_mult_symtoepl2(const float C[], int L_resp, const float x[], float y[], int N);
    void smpl_matrix_mult(const float C[], const float x[], float y[], int N, int M);
    void smpl_reverse(float x[], int L);
    void smpl_reverse_into(const float x[], float y[], int L);
    void smpl_sub_vec_inplace(const float y[], float x[], int L);
    void smpl_sub_vec(const float y[], const float z[], float x[], int L);
    void smpl_add_vec_inplace(const float y[], float x[], int L);
    void smpl_add_vec(const float y[], const float z[], float x[], int L);
    void smpl_scale_vec_inplace(float x[], int L, float scale);
    void smpl_scale_vec(const float x[], float y[], int L, float g);
    void smpl_add_scale_vec_inplace(const float x[], float y[], int L, float g);
    void smpl_add_scale_vec(const float x0[], const float x1[], float y[], int L, float g);
    void smpl_mul_vec_inplace(const float x[], float y[], int L);
    void smpl_mul_vec(const float x[], const float y[], float z[], int L);
    void smpl_overlap_add(const float *x, float *y, const float *ramp, int L);
    void smpl_get_env(const float exc[], int len, float smth_coef, float* smth_state, float env[]);
    void smpl_get_env0(int len, float smth_coef, float* smth_state, float env[]);
    void smpl_float_to_int16(const float *x, int16_t *y, int N);
    float smpl_minimum(const float* x, int x_len);
    float smpl_maximum(const float* x, int x_len);
    float smpl_sum_vec(const float *x, int x_len);
    void smpl_gen_rand_pulses(float noise[], int L, int32_t* rand_seed);
    float smpl_gen_log(float x, float a);
    float smpl_gen_exp(float x, float a);
    int smpl_ecvq(const float* x, const int x_len, const float* CB, const float* lam_bits, const int cb_len);
    int smpl_ecvq_lpc(const float* R, const float* CB, const float* lam_prob, const int cb_len);
    int smpl_get_maxi(const float *const x, const int x_len);
    void smpl_get_maxi_K(const float *const x, int *result, const int x_len, const int K);
#ifdef SMPL_USE_POWF_FAST
    float smpl_powf_fast(float a, float b);
#endif

#ifdef __cplusplus
}
#endif

#endif
