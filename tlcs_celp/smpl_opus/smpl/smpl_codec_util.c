#include "smpl_codec_util.h"
#include "smpl_typedef.h"
#include "smpl_tables.h"
#include "smpl_defines.h"
#include "smpl_lpc.h"
#include "smpl_filt.h"
#include <math.h>
#include <string.h>
#include <float.h>

#define smpl_ADD32_ovflw(a, b)              ((opus_int32)((opus_uint32)(a) + (opus_uint32)(b)))
/* Multiply-accumulate macros that allow overflow in the addition (ie, no asserts in debug mode) */
#define smpl_MLA_ovflw(a32, b32, c32)       smpl_ADD32_ovflw((a32), (opus_uint32)(b32) * (opus_uint32)(c32))

/* PSEUDO-RANDOM GENERATOR                                                          */
/* Make sure to store the result as the seed for the next call (also in between     */
/* frames), otherwise result won't be random at all. When only using some of the    */
/* bits, take the most significant bits by right-shifting.                          */
#define RAND_MULTIPLIER                     196314165
#define RAND_INCREMENT                      907633515
#define smpl_RAND(seed)                     (smpl_MLA_ovflw((RAND_INCREMENT), (seed), (RAND_MULTIPLIER)))

float smpl_dot_prod(const float a[], const float b[], int L)
{
    float ret = 0.0f;
    for (int i = 0; i < L; i++){
        ret += a[i] * b[i];
    }
    return ret;
}

float smpl_nrg(const float x[], int N)
{
    float nrg = 0.0f;
    for (int n = 0; n < N; n++) {
        nrg += x[n] * x[n];
    }
    return nrg;
}

float smpl_sum(const float x[], int N)
{
    float s = 0.0f;
    for (int n = 0; n < N; n++) {
        s += x[n];
    }
    return s;
}

float smpl_werr(const float *x, const float *y, const float *w)
{
    float s = 0.0f;
    for (int k = 0; k < SMPL_LPC_ORDER; k++) {
        float e_ = x[k] - y[k];
        s += w[k] * e_ * e_;
    }
    return s;
}

void smpl_sqrt_vec(float x[], int L) {
    for (int i = 0; i < L; i++) {
        x[i] = sqrtf(x[i]);
    }
}

void smpl_matrix_mult_transp_16(const float C[], const float x[], float y[], int len_y, int len_x)
{
    smpl_assert(len_y == 16);
    smpl_assert(len_x > 0);
    float y_[16];
    float xtmp = x[0];
    for (int i = 0; i < 16; i++) {
        y_[i] = C[i] * xtmp;
    }
    for (int j = 1; j < len_x; j++) {
        xtmp = x[j];
        for (int i = 0; i < 16; i++) {
            y_[i] += C[j * len_y + i] * xtmp;
        }
    }
    memcpy(y, y_, 16 * sizeof(float));
}

void smpl_celp_q(const float num[], const float den[], int L, float Q[])
{
    for (int i = 0; i < L; i++) {
        Q[i] = (num[i] * num[i]) / den[i];
    }
}

void smpl_mult_symtoepl2(const float C[], int L_resp, const float x[], float y[], int N)
{
    smpl_assert(2 * L_resp <= N);
    smpl_assert(C[2 * L_resp - 1] == 0.0f);   // needed to help SIMD later on

    int n = 0;
    int len = L_resp;
    for ( ; n < L_resp - 1; n++) {
        y[n] = smpl_dot_prod(&C[L_resp - 1 - n], &x[0], len++);
    }
    len = 2 * L_resp;  // one more than strictly needed, to help SIMD. we've added an extra zero to colSym, and don't read beyond x[N-1]
    for ( ; n < N - L_resp; n++) {
        y[n] = smpl_dot_prod(&C[0], &x[n - L_resp + 1], len);
    }
    for ( ; n < N; n++) {
        y[n] = smpl_dot_prod(&C[0], &x[n - L_resp + 1], --len);
    }
}

void smpl_matrix_mult(const float C[], const float x[], float y[], int len_y, int len_x)
{
    for(int i = 0; i < len_y; i++) {
        y[i] = smpl_dot_prod(&C[i * len_x], x, len_x);
    }
}

void smpl_reverse(float x[], int L)
{
    for(int i = 0; i < L/2; i++){
        float tmp = x[i];
        x[i] = x[L - i - 1];
        x[L - i - 1] = tmp;
    }
}

void smpl_reverse_into(const float x[], float y[], int L)
{
    for (int i = 0; i < L; i++) {
        y[i] = x[L - i - 1];
    }
}

// x -= y
void smpl_sub_vec_inplace(const float y[], float x[], int L)
{
    for(int i = 0; i < L; i++) {
        x[i] -= y[i];
    }
}

// x = y - z
void smpl_sub_vec(const float y[], const float z[], float x[], int L)
{
    for(int i = 0; i < L; i++) {
        x[i] = y[i] - z[i];
    }
}

// x += y
void smpl_add_vec_inplace(const float y[], float x[], int L)
{
    for(int i = 0; i < L; i++) {
        x[i] += y[i];
    }
}

// x = y + z
void smpl_add_vec(const float y[], const float z[], float x[], int L)
{
    for(int i = 0; i < L; i++) {
        x[i] = y[i] + z[i];
    }
}

void smpl_scale_vec_inplace(float x[], int L, float g)
{
    for(int i = 0; i < L; i++) {
        x[i] *= g;
    }
}

void smpl_scale_vec(const float x[], float y[], int L, float g)
{
    for(int i = 0; i < L; i++) {
        y[i] = x[i] * g;
    }
}

void smpl_add_scale_vec_inplace(const float x[], float y[], int L, float g)
{
    for(int i = 0; i < L; i++){
        y[i] += g * x[i];
    }
}

void smpl_add_scale_vec(const float x0[], const float x1[], float y[], int L, float g)
{
    for(int i = 0; i < L; i++){
        y[i] = x0[i] + g * x1[i];
    }
}

void smpl_mul_vec_inplace(const float x[], float y[], int L)
{
    for(int i = 0; i < L; i++) {
        y[i] *= x[i];
    }
}

void smpl_mul_vec(const float x[], const float y[], float z[], int L)
{
    for(int i = 0; i < L; i++) {
        z[i] = y[i] * x[i];
    }
}

void smpl_overlap_add(const float *x, float *y, const float *ramp, int L) {
    for (int i = 0; i < L; i++) {
        y[i] += (x[i] - y[i]) * ramp[i];
    }
}

void smpl_get_env(const float exc[], int len, float smth_coef, float* smth_state, float env[])
{
    smth_coef *= smth_coef;  // because we operate on the squared signal
    smpl_assert((1.0f - smth_coef) * len < 30.0f);  // guarantees not getting too small numbers; if this asserts, smth_coef is too small
    float state = *smth_state + 1e-8f;  // add small value to: 1) avoid too small numbers; 2) ensure we always have a decaying envelope
    state *= state;  // because we operate on squared signal
    float gain_coef      = 1.0f - smth_coef;
    float smth_coef2     = smth_coef * smth_coef;
    float gain_smth_coef = gain_coef * smth_coef;
    smpl_assert((len & 3) == 0);
    for (int i = 0; i < len - 3; i += 4) {
        float tmp0 = exc[i + 0] * exc[i + 0] + exc[i + 1] * exc[i + 1];
        float tmp1 = exc[i + 2] * exc[i + 2] + exc[i + 3] * exc[i + 3];
        float y1 = gain_coef * tmp1 + gain_smth_coef * tmp0 + smth_coef2 * state;
        float y0 = gain_coef * tmp0 + smth_coef * state;
        env[i + 0] = env[i + 1] = sqrtf(y0);
        env[i + 2] = env[i + 3] = sqrtf(y1);
        state = y1;
    }
    *smth_state = env[len - 1];
}

void smpl_get_env0(int len, float smth_coef, float* smth_state, float env[])
{
    float smth_coef2 = smth_coef * smth_coef;
    smpl_assert((len & 3) == 0);
    env[0] = env[1] = (*smth_state + 1e-8f) * smth_coef;
    for (int i = 2; i < len - 2; i += 4) {
        env[i + 2] = env[i + 3] = env[i - 1] * smth_coef2;
        env[i + 0] = env[i + 1] = env[i - 1] * smth_coef;
    }
    env[len - 2] = env[len - 1] = env[len - 3] * smth_coef;
    *smth_state = env[len - 1];
}

void smpl_float_to_int16(const float *x, int16_t *y, int N)
{
    for (int i = 0; i < N; i++) {
        y[i] = (int16_t)SMPL_max(SMPL_min((x[i] * 32767.0f), 32767.0f), -32767.0f);
    }
}

float smpl_minimum(const float *x, int x_len)
{
    float x_min = x[0];
    for (int i = 1; i < x_len; i++) {
        if (x[i] < x_min) {
            x_min = x[i];
        }
    }
    return x_min;
}

float smpl_maximum(const float *x, int x_len)
{
    float x_max = x[0];
    for (int i = 1; i < x_len; i++) {
        if (x[i] > x_max) {
            x_max = x[i];
        }
    }
    return x_max;
}

float smpl_sum_vec(const float *x, int x_len) {
    float x_sum = x[0];
    for (int i=1; i<x_len; i++) {
        x_sum += x[i];
    }
    return(x_sum);
}

// void smpl_gen_rand_pulses(float noise[], int L, int32_t* rand_seed) {
//     int i = 0;
//     for ( ; i < L - 3; i += 4) {
//         *rand_seed = smpl_RAND(*rand_seed);
//         noise[i + 0] = 2 * ((*rand_seed >> 31) & 0x1) - 1;
//         noise[i + 1] = 2 * ((*rand_seed >> 30) & 0x1) - 1;
//         noise[i + 2] = 2 * ((*rand_seed >> 29) & 0x1) - 1;
//         noise[i + 3] = 2 * ((*rand_seed >> 28) & 0x1) - 1;
//         // print here
//     }
//     for ( ; i < L; i++) {
//         *rand_seed = smpl_RAND(*rand_seed);
//         noise[i] = 2 * ((*rand_seed >> 31) & 0x1) - 1;
//     }
// }
void smpl_gen_rand_pulses(float noise[], int L, int32_t* rand_seed) {
    int i = 0;
    for ( ; i < L - 3; i += 4) {
        *rand_seed = smpl_RAND(*rand_seed);
        noise[i + 0] = 8.1e-10f * (float)                      (*rand_seed);
        noise[i + 1] = 8.1e-10f * (float)((int32_t)(((uint32_t)(*rand_seed)) << 8));
        noise[i + 2] = 8.1e-10f * (float)((int32_t)(((uint32_t)(*rand_seed)) << 16));
        noise[i + 3] = 8.1e-10f * (float)((int32_t)(((uint32_t)(*rand_seed)) << 24));
    }
    for ( ; i < L; i++) {
        *rand_seed = smpl_RAND(*rand_seed);
        noise[i] = 8.1e-10f * (float)(*rand_seed);
    }
}
#ifdef SMPL_USE_POWF_FAST
float smpl_powf_fast(float a, float b) {
	union { float d; int x; } u = { a };
	u.x = (int)(b * (u.x - 1064866805) + ((float) 1064866805));
	return u.d;
}
#endif

float smpl_gen_log(float x, float a) {
    return (powf(x, a) - 1.0f) / a;
}

float smpl_gen_exp(float x, float a) {
    return powf(a * x + 1.0f, 1.0f / a);
}

int smpl_ecvq(const float* x, const int x_len, const float* CB, const float* lam_bits, const int cb_len) {
	float bestRD = 1e30f;
	int qi = 0;
	for (int i = 0; i < cb_len; i++) {
		float e = 0;
		for (int j = 0; j < x_len; j++) {
			float d = CB[i * x_len + j] - x[j];
			e += d * d;
		}
		e += lam_bits[i];
		if (e < bestRD) {
			bestRD = e;
			qi = i;
		}
	}
	return qi;
}

int smpl_ecvq_lpc(const float* R, const float* CB, const float* lam_prob, const int cb_len) {
	float bestRD = 1e30f;
	int qi = 0;
	for (int i = 0; i < cb_len; i++) {
		float e = 0;
		for (int j = 0; j < SMPL_HB_LPC_ORDER + 1; j++) {
			e += CB[i * (SMPL_HB_LPC_ORDER + 1) + j] * R[j];
		}
		e *= lam_prob[i];
		if (e < bestRD) {
			bestRD = e;
			qi = i;
		}
	}
	return qi;
}

static inline void smpl_max_vec(const float *x, const float *y, float *res, const int len) {
    for (int n = 0; n < len; n++) {
        res[n] = SMPL_max(x[n], y[n]);
    }
}
// get index of highest value
 int smpl_get_maxi(const float *const x, const int x_len) {
     smpl_assert(x_len <= 160);
     float buf[160];
     int num_halves = 0;
     int len = (x_len + 1) >> 1;
     smpl_max_vec(x, x + len, buf, x_len - len);
     buf[x_len - len] = x[x_len - len];  // only necessasry when x_len is odd, but harmless otherwise
     float *buf_ptr = buf;
     while ((len & 1) == 0) {
         buf_ptr += len;
         len >>= 1;
         smpl_max_vec(buf_ptr - 2 * len, buf_ptr - len, buf_ptr, len);
         num_halves++;
     }
     int i = 0;
     float maxtmp = buf_ptr[0];
     for (int n = 1; n < len; n++) {
         float xtmp = buf_ptr[n];
         if (xtmp > maxtmp) {
             maxtmp = xtmp;
             i = n;
         }
     }
     for (int n = 0; n < num_halves; n++) {
         buf_ptr -= 2 * len;
         if (buf_ptr[i] < buf_ptr[i + len]) {
             i += len;
         }
         len <<= 1;
     }
     if (i + len < x_len && x[i] < x[i + len]) {
         i += len;
     }
     return i;
 }

// get sorted indices of K highest values
#define MAX_SORT_LEN  187  // NUM_BLOCKTRACKS
void smpl_get_maxi_K(const float *const x, int *idx, const int x_len, const int K) {
    smpl_assert(x_len <= MAX_SORT_LEN);
    float buf[MAX_SORT_LEN];
    int8_t flags[MAX_SORT_LEN / 2] = {0};
    int is[7];   // [floor(log2(MAX_SORT_LEN))]
    int num_halves = 0;
    int len = (x_len + 1) >> 1;
    smpl_max_vec(x, x + len, buf, x_len - len);
    buf[x_len - len] = x[x_len - len];  // only necessasry when x_len is odd, but harmless otherwise
    float *buf_ptr = buf;
    while ((len & 1) == 0) {
        buf_ptr += len;
        len >>= 1;
        smpl_max_vec(buf_ptr - 2 * len, buf_ptr - len, buf_ptr, len);
        num_halves++;
    }
    for (int k = 0; k < K; k++) {
        int i = 0;
        float maxtmp = buf_ptr[0];
        for (int n = 1; n < len; n++) {
            float xtmp = buf_ptr[n];
            if (xtmp > maxtmp) {
                maxtmp = xtmp;
                i = n;
            }
        }
        for (int n = 0; n < num_halves; n++) {
            is[n] = i;
            buf_ptr -= 2 * len;
            if (buf_ptr[i] < buf_ptr[i + len]) {
                i += len;
            }
            len <<= 1;
        }
        float xtmp = -FLT_MAX;
        int i_final = i;
        if (i + len < x_len) {
            if (flags[i]++ == 0) {
                if (x[i] < x[i + len]) {
                    xtmp = x[i];
                    i_final += len;
                } else {
                    xtmp = x[i + len];
                }
            } else {
                if (x[i] >= x[i + len]) {
                    i_final += len;
                }
            }
        }
        idx[k] = i_final;
        if (k == K - 1) return;
        buf_ptr[i] = xtmp;
        for (int n = num_halves - 1; n >= 0; n--) {
            i = is[n];
            len >>= 1;
            buf_ptr[i + 2 * len] = SMPL_max(buf_ptr[i], buf_ptr[i + len]);
            buf_ptr += 2 * len;
        }
    }
}
