#include <stdlib.h>
#include <string.h>
#include "smpl_bandwidth_extension.h"
#include "smpl_filt.h"
#include "smpl_codec_util.h"
#include "smpl_helpers.h"
#include "smpl_tables.h"
#include "smpl_codec_util.h"
#include "smpl_hb_gain_tables.h"
#include "smpl_hb_lpc_tables.h"
#include "smpl_lpc.h"
#include "smpl_errors.h"
#include "smpl_typedef.h"
#include "smpl_lsf_wrapper.h"
#include "silk/debug.h"

void* smpl_create_hb_encoder(void) {
	HbEncoder* pHE = (HbEncoder*)calloc(1, sizeof(HbEncoder));
    if (!pHE) {
        return NULL;
    }
	smpl_init_hb_encoder((void*)pHE);
    return (void*)pHE;
}

void smpl_init_hb_encoder(void* pSt) {
	smpl_assert(pSt != NULL);
	memset(pSt, 0, sizeof(HbEncoder));
}

void smpl_free_hb_encoder(void* pSt)
{
	smpl_assert(pSt != NULL);
    HbEncoder* pHE = (HbEncoder*)pSt;
    free(pHE);
}

static void* g_smpl_hb_lsf_CBks = NULL;
static void* g_smpl_hb_gain_CBks = NULL;

static inline void lpc_coef_conv(const float* A, float* A_conv) {
    for (int i = 0; i < SMPL_HB_LPC_ORDER + 1; i++) {
        A_conv[i] = 0.0f;
        for (int j = SMPL_HB_LPC_ORDER; j >= i; j--) {
            A_conv[i] += A[j] * A[j-i];
        }
        if (i > 0) {
            A_conv[i] *= 2.0f;
        }
    }
}

static inline void unpack_per_col(const uint8_t* ix, float* x, int m, const float* dx, const float* min) {
    for (int i = 0; i < m; i++) {
        for (int j = 0; j < SMPL_HB_LPC_ORDER; j++) {
            x[i * SMPL_HB_LPC_ORDER + j] = min[j] + ix[i * SMPL_HB_LPC_ORDER + j] * dx[j];
        }
    }
}

void* smpl_load_hb_lsf_CBks(void)
{
    if (g_smpl_hb_lsf_CBks) {
        return g_smpl_hb_lsf_CBks;
    }
    HB_LSF_CBs* pSt = (HB_LSF_CBs*)calloc(1, sizeof(HB_LSF_CBs));
	smpl_assert(pSt);
    if(!pSt) {
        return NULL;
    }
	int tot_vecs = hb_lpc_vq_sizes[0][0] + hb_lpc_vq_sizes[0][1] + hb_lpc_vq_sizes[1][0] + hb_lpc_vq_sizes[1][1];
	int sz = tot_vecs + 4;
	pSt->hb_lpc_vq_cmfs_table = (uint16_t*)calloc(sz, sizeof(uint16_t));
	smpl_assert(pSt->hb_lpc_vq_cmfs_table);
	if(!pSt->hb_lpc_vq_cmfs_table) {
		return NULL;
	}

	uint16_t *cmfs_ptr = pSt->hb_lpc_vq_cmfs_table;
    for (int voiced = SMPL_FALSE; voiced <= SMPL_TRUE; voiced++) {
        for (int lowRate = SMPL_FALSE; lowRate <= SMPL_TRUE; lowRate++) {
			pSt->hb_lpc_vq_cmfs[voiced][lowRate] = cmfs_ptr;
			smpl_dcmf_to_cmf(hb_lpc_vq_dcmfs[voiced][lowRate], hb_lpc_vq_sizes[voiced][lowRate], pSt->hb_lpc_vq_cmfs[voiced][lowRate]);
			cmfs_ptr += hb_lpc_vq_sizes[voiced][lowRate] + 1;
		}
	}

	int tot_vecs_lr = hb_lpc_vq_sizes[0][1] + hb_lpc_vq_sizes[1][1];
	sz = (tot_vecs_lr + 2) * SMPL_HB_LPC_CB_N_COND;
	pSt->hb_lpc_vq_cmfs_cond_table = (uint16_t*)calloc(sz, sizeof(uint16_t));
	smpl_assert(pSt->hb_lpc_vq_cmfs_cond_table);
	if(!pSt->hb_lpc_vq_cmfs_cond_table) {
		return NULL;
	}

	cmfs_ptr = pSt->hb_lpc_vq_cmfs_cond_table;
	const uint8_t *dcmfs_ptr;
	for (int voiced = 0; voiced <= SMPL_TRUE; voiced++) {
		pSt->hb_lpc_vq_cmfs_cond[voiced] = cmfs_ptr;
		dcmfs_ptr = hb_lpc_vq_dcmfs_cond[voiced];
		for (int sel_cond = 0; sel_cond < SMPL_HB_LPC_CB_N_COND; sel_cond++) {
			smpl_dcmf_to_cmf(dcmfs_ptr, hb_lpc_vq_sizes[voiced][1], cmfs_ptr);
			cmfs_ptr  += hb_lpc_vq_sizes[voiced][1] + 1;
			dcmfs_ptr += hb_lpc_vq_sizes[voiced][1];
		}
	}

	// only needed by decoder
	sz = SMPL_HB_LPC_ORDER * tot_vecs;
	pSt->hb_lpc_vq_cb_lsf_table = (float*)calloc(sz, sizeof(float));
	smpl_assert(pSt->hb_lpc_vq_cb_lsf_table);
	if(!pSt->hb_lpc_vq_cb_lsf_table) {
		return NULL;
	}

	float *cb_ptr = pSt->hb_lpc_vq_cb_lsf_table;
	float lsf_scale = SMPL_PI / (float)(1 << 15);
    float min[SMPL_HB_LPC_ORDER];
    float dx[SMPL_HB_LPC_ORDER];
	for (int voiced = SMPL_FALSE; voiced <= SMPL_TRUE; voiced++) {
        for (int lowRate = SMPL_FALSE; lowRate <= SMPL_TRUE; lowRate++) {
			pSt->hb_lpc_vq_cb_lsf[voiced][lowRate] = cb_ptr;
			for (int i = 0; i < SMPL_HB_LPC_ORDER; i++) {
				min[i] = hb_lpc_vq_cb_min[voiced][lowRate][i] * lsf_scale;
				dx[i]  = hb_lpc_vq_cb_scales[voiced][lowRate][i] * lsf_scale;
			}
			
			unpack_per_col(hb_lpc_vq_cb_dlsf[voiced][lowRate], pSt->hb_lpc_vq_cb_lsf[voiced][lowRate],
				hb_lpc_vq_sizes[voiced][lowRate], dx, min);

			cb_ptr += SMPL_HB_LPC_ORDER * hb_lpc_vq_sizes[voiced][lowRate];
		}
	}

	// only needed by encoder
	sz = (SMPL_HB_LPC_ORDER + 1) * tot_vecs;
	pSt->hb_lpc_vq_cb_conv_table = (float*)calloc(sz, sizeof(float));
	smpl_assert(pSt->hb_lpc_vq_cb_conv_table);
	if(!pSt->hb_lpc_vq_cb_conv_table) {
		return NULL;
	}

	cb_ptr = pSt->hb_lpc_vq_cb_conv_table;
	float *lsf_ptr = pSt->hb_lpc_vq_cb_lsf_table;
	for (int voiced = SMPL_FALSE; voiced <= SMPL_TRUE; voiced++) {
        for (int lowRate = SMPL_FALSE; lowRate <= SMPL_TRUE; lowRate++) {
			pSt->hb_lpc_vq_cb_conv[voiced][lowRate] = cb_ptr;
			for (int i = 0; i < hb_lpc_vq_sizes[voiced][lowRate]; i++) {
				float A[SMPL_HB_LPC_ORDER + 1];
				smpl_NLSF2A(A, lsf_ptr, SMPL_HB_LPC_ORDER);
				lpc_coef_conv(A, cb_ptr);
				cb_ptr  += SMPL_HB_LPC_ORDER + 1;
				lsf_ptr += SMPL_HB_LPC_ORDER;
			}
		}
	}

	sz = tot_vecs;
	pSt->hb_lpc_vq_lam_prob_table = (float*)calloc(sz, sizeof(float));
	smpl_assert(pSt->hb_lpc_vq_lam_prob_table);
	if(!pSt->hb_lpc_vq_lam_prob_table) {
		return NULL;
	}

	float *lam_ptr = pSt->hb_lpc_vq_lam_prob_table;
	for (int voiced = SMPL_FALSE; voiced <= SMPL_TRUE; voiced++) {
        for (int lowRate = SMPL_FALSE; lowRate <= SMPL_TRUE; lowRate++) {
			pSt->hb_lpc_vq_lam_prob[voiced][lowRate] = lam_ptr;
			cmfs_ptr = pSt->hb_lpc_vq_cmfs[voiced][lowRate];
			int bits_len = hb_lpc_vq_sizes[voiced][lowRate];
			smpl_cmf_to_bits(cmfs_ptr, bits_len + 1, lam_ptr);
			for (int i = 0; i < bits_len; i++) {
				lam_ptr[i] = powf(2.0f, hb_lpc_vq_lambdas[voiced][lowRate] * lam_ptr[i]);
			}
			lam_ptr += bits_len;
		}
	}

	sz = tot_vecs * SMPL_HB_LPC_CB_N_COND;
	pSt->hb_lpc_vq_lam_prob_cond_table = (float*)calloc(sz, sizeof(float));
	smpl_assert(pSt->hb_lpc_vq_lam_prob_cond_table);
	if (!pSt->hb_lpc_vq_lam_prob_cond_table) {
		return NULL;
	}

	lam_ptr = pSt->hb_lpc_vq_lam_prob_cond_table;
	for (int voiced = 0; voiced <= SMPL_TRUE; voiced++) {
		pSt->hb_lpc_vq_lam_prob_cond[voiced] = lam_ptr;
		cmfs_ptr = pSt->hb_lpc_vq_cmfs_cond[voiced];
		int bits_len = hb_lpc_vq_sizes[voiced][1];
		for (int sel_cond = 0; sel_cond < SMPL_HB_LPC_CB_N_COND; sel_cond++) {
			smpl_cmf_to_bits(cmfs_ptr, bits_len + 1, lam_ptr);
			for (int i = 0; i < bits_len; i++) {
				lam_ptr[i] = powf(2.0f, hb_lpc_vq_lambdas[voiced][1] * lam_ptr[i]);
			}
			lam_ptr  += bits_len;
			cmfs_ptr += bits_len + 1;
		}
	}

    g_smpl_hb_lsf_CBks = (void*)pSt;
    return g_smpl_hb_lsf_CBks;
}

void* smpl_load_hb_gain_CBks(void)
{
	if (g_smpl_hb_gain_CBks) {
        return g_smpl_hb_gain_CBks;
    }
    HB_GAIN_CBs* pSt = (HB_GAIN_CBs*)calloc(1, sizeof(HB_GAIN_CBs));
	smpl_assert(pSt);
	if(!pSt) {
        return NULL;
    }

	int tot_vecs_10 = hb_gain_vq_sizes[0][0][0] + hb_gain_vq_sizes[0][0][1] +
					  hb_gain_vq_sizes[0][1][0] + hb_gain_vq_sizes[0][1][1];
	int tot_vecs_20 = hb_gain_vq_sizes[1][0][0] + hb_gain_vq_sizes[1][0][1] +
					  hb_gain_vq_sizes[1][1][0] + hb_gain_vq_sizes[1][1][1];
	int sz = tot_vecs_10 * (16 * 10 / SMPL_HB_SF_LEN) + tot_vecs_20 * (16 * 20 / SMPL_HB_SF_LEN);
	pSt->hb_gain_vq_shapes_table = (float*)calloc(sz, sizeof(float));
	smpl_assert(pSt->hb_gain_vq_shapes_table);
	if(!pSt->hb_gain_vq_shapes_table) {
		return NULL;
	}
	float* cb_ptr = pSt->hb_gain_vq_shapes_table;
	for (int framelen_20 = 0; framelen_20 <= SMPL_TRUE; framelen_20++) {
		for (int voiced = 0; voiced <= SMPL_TRUE; voiced++) {
			for (int lowRate = 0; lowRate <= SMPL_TRUE; lowRate++) {
				pSt->hb_gain_vq_shapes[framelen_20][voiced][lowRate] = cb_ptr;
				const uint8_t* dcb_ptr = hb_gain_vq_cb_dshapes[framelen_20][voiced][lowRate];
				int num_elements = hb_gain_vq_sizes[framelen_20][voiced][lowRate] * (framelen_20 ? 20 : 10) * 16 / SMPL_HB_SF_LEN;
				float scale = hb_gain_vq_cb_scale[framelen_20][voiced][lowRate];
				float min   = hb_gain_vq_cb_min[framelen_20][voiced][lowRate];
				smpl_unpack8(dcb_ptr, cb_ptr, num_elements, scale, min);

				cb_ptr += num_elements;
			}
		}
	}

	sz = tot_vecs_10 + tot_vecs_20 + 8;
	pSt->hb_gain_vq_cmfs_table = (uint16_t*)calloc(sz, sizeof(uint16_t));
	smpl_assert(pSt->hb_gain_vq_shapes_table);
	if (!pSt->hb_gain_vq_cmfs_table) {
		return NULL;
	}
	uint16_t *cmfs_ptr = pSt->hb_gain_vq_cmfs_table;
	for (int framelen_20 = 0; framelen_20 <= SMPL_TRUE; framelen_20++) {
		for (int voiced = 0; voiced <= SMPL_TRUE; voiced++) {
			for (int lowRate = 0; lowRate <= SMPL_TRUE; lowRate++) {
				pSt->hb_gain_vq_cmfs[framelen_20][voiced][lowRate] = cmfs_ptr;
				const uint8_t* dcmf_ptr = hb_gain_vq_dcmfs[framelen_20][voiced][lowRate];
				int dcmf_len = hb_gain_vq_sizes[framelen_20][voiced][lowRate];
				smpl_dcmf_to_cmf(dcmf_ptr, dcmf_len, cmfs_ptr);

				cmfs_ptr += dcmf_len + 1;
				dcmf_ptr += dcmf_len;
			}
		}
	}

	sz = tot_vecs_10 + tot_vecs_20;
	pSt->hb_gain_vq_lam_bits_table = (float*)calloc(sz, sizeof(float));
	smpl_assert(pSt->hb_gain_vq_lam_bits_table);
	if (!pSt->hb_gain_vq_lam_bits_table) {
		return NULL;
	}

	float *lam_ptr = pSt->hb_gain_vq_lam_bits_table;
	for (int framelen_20 = 0; framelen_20 <= SMPL_TRUE; framelen_20++) {
		for (int voiced = 0; voiced <= SMPL_TRUE; voiced++) {
			for (int lowRate = 0; lowRate <= SMPL_TRUE; lowRate++) {
				cmfs_ptr = pSt->hb_gain_vq_cmfs[framelen_20][voiced][lowRate];
				pSt->hb_gain_vq_lam_bits[framelen_20][voiced][lowRate] = lam_ptr;
				int bits_len = hb_gain_vq_sizes[framelen_20][voiced][lowRate];
				smpl_cmf_to_bits(cmfs_ptr, bits_len + 1, lam_ptr);
				for (int i = 0; i < bits_len; i++) {
					lam_ptr[i] = hb_gain_vq_lambdas[framelen_20][voiced][lowRate] * lam_ptr[i];
				}
				lam_ptr += bits_len;
			}
		}
	}

    g_smpl_hb_gain_CBks = (void*)pSt;
	return g_smpl_hb_gain_CBks;
}

void smpl_free_hb_lsf_CBks(void)
{
    HB_LSF_CBs *pSt = (HB_LSF_CBs*)g_smpl_hb_lsf_CBks;
    if (pSt) {
		if (pSt->hb_lpc_vq_cmfs_table) {
			free(pSt->hb_lpc_vq_cmfs_table);
			pSt->hb_lpc_vq_cmfs_table = NULL;
		}
		if (pSt->hb_lpc_vq_cb_lsf_table) {
			free(pSt->hb_lpc_vq_cb_lsf_table);
			pSt->hb_lpc_vq_cb_lsf_table = NULL;
		}
		if (pSt->hb_lpc_vq_cb_conv_table) {
			free(pSt->hb_lpc_vq_cb_conv_table);
			pSt->hb_lpc_vq_cb_conv_table = NULL;
		}
		if (pSt->hb_lpc_vq_cmfs_cond_table) {
			free(pSt->hb_lpc_vq_cmfs_cond_table);
			pSt->hb_lpc_vq_cmfs_cond_table = NULL;
		}
		if (pSt->hb_lpc_vq_lam_prob_table) {
			free(pSt->hb_lpc_vq_lam_prob_table);
			pSt->hb_lpc_vq_lam_prob_table = NULL;
		}
		if (pSt->hb_lpc_vq_lam_prob_cond_table) {
			free(pSt->hb_lpc_vq_lam_prob_cond_table);
			pSt->hb_lpc_vq_lam_prob_cond_table = NULL;
		}
        free(pSt);
        g_smpl_hb_lsf_CBks = NULL;
	}
}

void smpl_free_hb_gain_CBks(void)
{
    HB_GAIN_CBs *pSt = (HB_GAIN_CBs*)g_smpl_hb_gain_CBks;
	if (pSt) {
		if (pSt->hb_gain_vq_shapes_table) {
			free(pSt->hb_gain_vq_shapes_table);
			pSt->hb_gain_vq_shapes_table = NULL;
		}
		if (pSt->hb_gain_vq_cmfs_table) {
			free(pSt->hb_gain_vq_cmfs_table);
			pSt->hb_gain_vq_cmfs_table = NULL;
		}
		if (pSt->hb_gain_vq_lam_bits_table) {
			free(pSt->hb_gain_vq_lam_bits_table);
			pSt->hb_gain_vq_lam_bits_table = NULL;
		}
		free(pSt);
		g_smpl_hb_gain_CBks = NULL;
	}
}

const HB_LSF_CBs* smpl_get_hb_lsf_CBks(void)
{
    if (g_smpl_hb_lsf_CBks) {
        return ((const HB_LSF_CBs*)g_smpl_hb_lsf_CBks);
    }
    return NULL;
}

const HB_GAIN_CBs* smpl_get_hb_gain_CBks(void)
{
    if (g_smpl_hb_gain_CBks) {
        return ((const HB_GAIN_CBs*)g_smpl_hb_gain_CBks);
    }
    return NULL;
}

void smpl_hb_lsf_dequant(int qi, int voiced, int low_rate, float *lsf) {
	const HB_LSF_CBs* pCb = smpl_get_hb_lsf_CBks();
	smpl_assert(pCb != NULL);
	for (int i = 0; i < SMPL_HB_LPC_ORDER; i++) {
		lsf[i] = pCb->hb_lpc_vq_cb_lsf[voiced][low_rate][qi * SMPL_HB_LPC_ORDER + i];
	}
}

#define SMPL_HB_IMP_LEN 16 // < SMPL_HB_SF_LEN
void smpl_hb_decode(
	HbDecoder* hb_decoder,
	const float exc[],
	const int voiced,
	const int coded_as_active_voice,
#if defined(SMPL_USE_LPC_POSTFILTER) || defined(SMPL_USE_TILT_POSTFILTER)
	const float nyquist_gain,
#endif
	const int num_hb_subframes,
	const int num_fcb_subframes,
	const float hb_gains[],
	const float* A_hb,
	int frame,
	int num_frames,
	float* y, // when frame == 0, should have SMPL_TOT_POSTFILT_DELAY length before y available
	float* hb_exc_gain,
	int low_rate)
{
	TIC(syn_loop)

	if (frame == 0) { // copy in state from previous packet
		memcpy(y - SMPL_TOT_POSTFILT_DELAY, hb_decoder->out_state, SMPL_TOT_POSTFILT_DELAY * sizeof(float));
	}
	for (int i = 0; i < num_hb_subframes; i++) {
		float noise_[SMPL_HB_SF_LEN + SMPL_HB_LPC_ORDER];
		float *noise = noise_ + SMPL_HB_LPC_ORDER;
		smpl_gen_rand_pulses(noise, SMPL_HB_SF_LEN, &hb_decoder->rand_seed);

		if (coded_as_active_voice) {
			float env[SMPL_HB_SF_LEN];
			smpl_get_env(exc + i * SMPL_HB_SF_LEN, SMPL_HB_SF_LEN, voiced ? SMPL_HB_SMTH_COEF_V : SMPL_HB_SMTH_COEF_UV, &hb_decoder->env_smth, env);
			smpl_mul_vec_inplace(env, noise, SMPL_HB_SF_LEN);
		} else {
			hb_decoder->env_smth = 0e0f;
		}

		float impulse_[SMPL_HB_IMP_LEN + SMPL_HB_LPC_ORDER];
		memset(impulse_, 0, (SMPL_HB_IMP_LEN + SMPL_HB_LPC_ORDER) * sizeof(float));
		float* impulse = impulse_ + SMPL_HB_LPC_ORDER;
		impulse[0] = 1.0f;
		const float *A_ptr = A_hb + (i * num_fcb_subframes / num_hb_subframes) * (SMPL_HB_LPC_ORDER + 1);
		smpl_filt_ar4(impulse, SMPL_HB_IMP_LEN, A_ptr, impulse);
		smpl_filt_ma3(impulse, SMPL_HB_IMP_LEN, smpl_hb_wght_coef, SMPL_HB_WGHT_LEN, y); // temp use y as scratch memory
		float nrg_gain = smpl_nrg(y, SMPL_HB_IMP_LEN);
		float scale = sqrtf(hb_gains[i] / ((nrg_gain * smpl_nrg(noise, SMPL_HB_SF_LEN)) / SMPL_HB_SF_LEN + 1e-12f) + 1e-30f);
#ifdef SMPL_USE_LPC_POSTFILTER
		scale *= nyquist_gain;
#endif
		smpl_scale_vec_inplace(noise, SMPL_HB_SF_LEN, scale);
		memcpy(noise - SMPL_HB_LPC_ORDER, hb_decoder->spec_env_state, SMPL_HB_LPC_ORDER * sizeof(float));
		smpl_filt_ar4(noise, SMPL_HB_SF_LEN, A_ptr, noise);
		memcpy(hb_decoder->spec_env_state, &noise[SMPL_HB_SF_LEN - SMPL_HB_LPC_ORDER], SMPL_HB_LPC_ORDER * sizeof(float));
		if (low_rate) {
			memcpy(y, noise, SMPL_HB_SF_LEN * sizeof(float));
			memcpy(hb_decoder->post_ma_state, y + SMPL_HB_SF_LEN - SMPL_HB_POST_LEN + 1, (SMPL_HB_POST_LEN - 1) * sizeof(float));
		} else {
			smpl_filt_ma1(noise, SMPL_HB_SF_LEN, smpl_hb_post_coef, SMPL_HB_POST_LEN, hb_decoder->post_ma_state, SMPL_HB_POST_LEN - 1, y);
		}

		if (coded_as_active_voice && !voiced) {
			hb_exc_gain[i] = hb_gains[i] / (nrg_gain + 1e-12f);
#ifdef SMPL_USE_LPC_POSTFILTER
			hb_exc_gain[i] *= nyquist_gain * nyquist_gain;
#endif
		} else {
			hb_exc_gain[i] = scale * scale;
		}

		y += SMPL_HB_SF_LEN;
	}
	if (frame == num_frames - 1) { // record state for next packet
		memcpy(hb_decoder->out_state, y - SMPL_TOT_POSTFILT_DELAY, SMPL_TOT_POSTFILT_DELAY * sizeof(float));
	}

	TOC(syn_loop)
}

void smpl_hb_gain_dequant(
	const int gain_qi,
	const int voiced,
	const int low_rate,
	const int num_hb_subframes,
	float hb_gains[],
	const float* y_wght,
	float low_nrg_frame)
{
	const HB_GAIN_CBs* pCb = smpl_get_hb_gain_CBks();
	smpl_assert(pCb != NULL);
	
	// get subframe-level energies
	const float* CB = pCb->hb_gain_vq_shapes[num_hb_subframes == SMPL_MAX_HB_SUBFR][voiced][low_rate];
	CB += num_hb_subframes * gain_qi;
	for (int i = 0; i < num_hb_subframes; i++) {
		float low_nrg_subframe = smpl_nrg(y_wght + i * SMPL_HB_SF_LEN, SMPL_HB_SF_LEN) / SMPL_HB_SF_LEN + 1e-12f;
		float pwr = hb_gain_vq_pwrs[num_hb_subframes == SMPL_MAX_HB_SUBFR][voiced][low_rate];
#ifdef SMPL_USE_POWF_FAST
		low_nrg_subframe = low_nrg_subframe * smpl_powf_fast(low_nrg_frame / low_nrg_subframe, pwr);
#else
		low_nrg_subframe = low_nrg_subframe * powf(low_nrg_frame / low_nrg_subframe, pwr);
#endif
		float hb_wght_nrg = SMPL_min(smpl_gen_exp(CB[i], SMPL_GEN_LOG_PWR), 2.0f);
		hb_wght_nrg *= low_nrg_subframe;
		hb_gains[i] = hb_wght_nrg;
	}
}

void smpl_hb_wght_lb(
	float lb_wght_mem[],
	const int num_hb_subframes,
	float x_pre_postfilter[], // input
	float y_wght[],           // output
	float* low_nrg_frame)
{
	const int framelen = num_hb_subframes * SMPL_HB_SF_LEN;

	// get lowband frame-level energy
	float tmp[SMPL_LB_WGHT_LEN - 1];
	memcpy(tmp, x_pre_postfilter - (SMPL_LB_WGHT_LEN - 1), (SMPL_LB_WGHT_LEN - 1) * sizeof(float));
	memcpy(x_pre_postfilter - (SMPL_LB_WGHT_LEN - 1), lb_wght_mem, (SMPL_LB_WGHT_LEN - 1) * sizeof(float));
	smpl_filt_ma9(x_pre_postfilter, framelen, smpl_lb_wght_coef, SMPL_LB_WGHT_LEN, y_wght);
	memcpy(x_pre_postfilter - (SMPL_LB_WGHT_LEN - 1), tmp, (SMPL_LB_WGHT_LEN - 1) * sizeof(float));
	memcpy(lb_wght_mem, x_pre_postfilter + framelen - (SMPL_LB_WGHT_LEN - 1), (SMPL_LB_WGHT_LEN - 1) * sizeof(float));
	*low_nrg_frame = smpl_nrg(y_wght, framelen) / framelen + 1e-12f;
}

void smpl_hb_encoder(
	void* pSt,
	HbQuantParams* pHbParams,
	const int voiced,
	const int cond_coding,
	const int low_rate,
	const float* x_16_lb,
	const float* x_16_hb,
	const int num_hb_subframes,
	const int bitrate)
{
	HbEncoder* pHE = (HbEncoder*)pSt;
	const int framelen = num_hb_subframes * SMPL_HB_SF_LEN;

	const HB_LSF_CBs* pLsfCb = smpl_get_hb_lsf_CBks();
	const HB_GAIN_CBs* pGainCb = smpl_get_hb_gain_CBks();
	smpl_assert(pLsfCb != NULL);
	smpl_assert(pGainCb != NULL);
 
	// get LPC VQ index
	int lpcbuf_len = (num_hb_subframes == 2) ? 304 : 448;
	int len_prev = lpcbuf_len - framelen;
	int frame_ms = (num_hb_subframes == 2) ? 10 : 20;
	memmove(pHE->hb_lpc_buf, pHE->hb_lpc_buf + framelen, len_prev * sizeof(float));
	memcpy(pHE->hb_lpc_buf + len_prev, x_16_hb, framelen * sizeof(float));
	float lpcbuf_windowed[448];
	smpl_window(pHE->hb_lpc_buf, lpcbuf_windowed, lpcbuf_len, frame_ms, SMPL_FALSE, SMPL_TRUE);

	float F2[SMPL_F_LEN];
	double R[SMPL_HB_LPC_ORDER + 1];
	TIC(hb_lpc)
	smpl_lpc(lpcbuf_windowed, lpcbuf_len, 0.0f, NULL, R, SMPL_HB_LPC_ORDER, F2);
	R[0] *= 1.0f + SMPL_HB_LPC_REG;
	TOC(hb_lpc)
	const int PMFLen = hb_lpc_vq_sizes[voiced][low_rate] + 1;
	if (cond_coding && low_rate) {
		smpl_assert(pHE->prev_hb_lpc_ix < hb_lpc_vq_sizes[voiced][low_rate]);
	}
	const float* lam_bits = (cond_coding && low_rate) ?
							 pLsfCb->hb_lpc_vq_lam_prob_cond[voiced] + (hb_lpc_vq_sel_cond[voiced][pHE->prev_hb_lpc_ix] * PMFLen) :
							 pLsfCb->hb_lpc_vq_lam_prob[voiced][low_rate];
	const float* CB = pLsfCb->hb_lpc_vq_cb_conv[voiced][low_rate];
	float R_[SMPL_HB_LPC_ORDER + 1];
	for (int i = 0; i < SMPL_HB_LPC_ORDER + 1; i++) {
		R_[i] = (float)R[i];
	}
	pHbParams->lsf_idx = smpl_ecvq_lpc(R_, CB, lam_bits, hb_lpc_vq_sizes[voiced][low_rate]);
	
	// get Gain VQ index
	float lb_nrgs[SMPL_MAX_HB_SUBFR];
	float hb_nrgs[SMPL_MAX_HB_SUBFR];
	float nrg_ratios[SMPL_MAX_HB_SUBFR];
	float mean_lb_nrg = 0.0f;
	for (int i = 0; i < num_hb_subframes; i++) {
		float x_wght[SMPL_HB_SF_LEN];
		smpl_filt_ma9(x_16_lb                    + i * SMPL_HB_SF_LEN, SMPL_HB_SF_LEN, smpl_lb_wght_coef, SMPL_LB_WGHT_LEN, x_wght);
		lb_nrgs[i] = smpl_nrg(x_wght, SMPL_HB_SF_LEN);
		smpl_filt_ma3(pHE->hb_lpc_buf + len_prev + i * SMPL_HB_SF_LEN, SMPL_HB_SF_LEN, smpl_hb_wght_coef, SMPL_HB_WGHT_LEN, x_wght);
		hb_nrgs[i] = smpl_nrg(x_wght, SMPL_HB_SF_LEN);
		mean_lb_nrg += lb_nrgs[i];
	}
	mean_lb_nrg /= num_hb_subframes;
	float scaled_bitrate = num_hb_subframes == 4 ? 3e-3f * (bitrate - 5000) : 1.8e-3f * (bitrate - 8500);
	float pwr = hb_gain_vq_pwrs[num_hb_subframes == SMPL_MAX_HB_SUBFR][voiced][low_rate];
	pwr = 1.0f - (1.0f - pwr) * smpl_sigmoid(scaled_bitrate);
	for (int i = 0; i < num_hb_subframes; i++) {
#ifdef SMPL_USE_POWF_FAST
		lb_nrgs[i] = lb_nrgs[i] * smpl_powf_fast(mean_lb_nrg / (lb_nrgs[i] + 1e-12f), pwr);
#else
		lb_nrgs[i] = lb_nrgs[i] * powf(mean_lb_nrg / (lb_nrgs[i] + 1e-12f), pwr);
#endif
		float nrg_ratio = hb_nrgs[i] / (lb_nrgs[i] + 1e-10f);
		nrg_ratio = SMPL_min(nrg_ratio, SMPL_HB_RATIO_LIMIT);
		nrg_ratios[i] = smpl_gen_log(nrg_ratio, SMPL_GEN_LOG_PWR);
	}
	lam_bits = pGainCb->hb_gain_vq_lam_bits[num_hb_subframes == SMPL_MAX_HB_SUBFR][voiced][low_rate];
	CB = pGainCb->hb_gain_vq_shapes[num_hb_subframes == SMPL_MAX_HB_SUBFR][voiced][low_rate];
	pHbParams->gain_qi = smpl_ecvq(nrg_ratios, num_hb_subframes, CB, lam_bits, hb_gain_vq_sizes[num_hb_subframes == SMPL_MAX_HB_SUBFR][voiced][low_rate]);
}
