#include "smpl_postfilter.h"
#include "smpl_codec_util.h"
#include "smpl_defines.h"
#include "smpl_filt.h"
#include "smpl_calc_hp_coefs.h"
#include "smpl_lpc.h"
#include "smpl_typedef.h"
#include "pffft.h"
#include "debug.h"

void* g_smpl_postfilt_tables = NULL;

void* smpl_create_postfilt_tables(void)	
{
	if (g_smpl_postfilt_tables) {
		return g_smpl_postfilt_tables;
	}
	PostFiltTables* pSt = (PostFiltTables*)calloc(1, sizeof(PostFiltTables));
	if (!pSt) {
		smpl_assert(0);
		return NULL;
	}
#ifdef SMPL_USE_LPC_POSTFILTER
	pSt->pffft_setup =  pffft_new_setup(LPC_POST_IMPZ_LEN, PFFFT_REAL);
	if (!pSt->pffft_setup) {
		smpl_assert(0);
		free(pSt);
		return NULL;
	} 
#endif

	float dOmega = SMPL_PI / (2.0f * (SMPL_FRAME_LEN + 1.0f));
	float omega = dOmega;
	for (int i = 0; i < SMPL_FRAME_LEN; i++) {
		pSt->ramp_dn_20[i] = powf(cosf(omega), SMPL_HP_POSTF_TRANSITION_SPEED);
		omega += dOmega;
	}
	dOmega = SMPL_PI / (2.0f * ((SMPL_FRAME_LEN/2) + 1.0f));
	omega = dOmega;
	for (int i = 0; i < SMPL_FRAME_LEN/2; i++) {
		pSt->ramp_dn_10[i] = powf(cosf(omega), SMPL_HP_POSTF_TRANSITION_SPEED);
		omega += dOmega;
	}

	g_smpl_postfilt_tables = (void*)pSt;
	return pSt;
}

void smpl_free_postfilt_tables(void)
{
	if (!g_smpl_postfilt_tables) {
		return;
	}
	PostFiltTables* pSt = (PostFiltTables*)g_smpl_postfilt_tables;
#ifdef SMPL_USE_LPC_POSTFILTER
	pffft_destroy_setup(pSt->pffft_setup);
#endif
	free(pSt);

	g_smpl_postfilt_tables = NULL;
}

#ifdef SMPL_USE_LPC_POSTFILTER

#define MALLOC_PERCW_NFFT_ALIGNMENT 64

float smpl_lpc_postfilter(
	LpcPostfilter* pSt,
	float x[],
	int len,
	const float predcoef[SMPL_LPC_ORDER + 1],
	int voiced,
	int low_rate)
{
	TIC(lpc_postfilter)
	smpl_assert(len <= SMPL_MAX_WB_FRAME_SIZE_IN_SAMPLES);
	PFFFT_Setup* pFFT = ((PostFiltTables*)g_smpl_postfilt_tables)->pffft_setup;
	smpl_assert(pFFT);
	smpl_assert(voiced == SMPL_TRUE || voiced == SMPL_FALSE);
	smpl_assert(low_rate == SMPL_TRUE || low_rate == SMPL_FALSE);
	float nyquist_gain = 0.0f;
	char temp_mem[SMPL_MAX_WB_FRAME_SIZE_IN_SAMPLES * sizeof(float) + MALLOC_PERCW_NFFT_ALIGNMENT];
	char H_mem[LPC_POST_IMPZ_LEN * sizeof(float) + MALLOC_PERCW_NFFT_ALIGNMENT];
	float A1[SMPL_LPC_ORDER + 1];
	float A2[SMPL_LPC_ORDER + 1];
	memcpy(A1, predcoef, (SMPL_LPC_ORDER + 1) * sizeof(float));
	memcpy(A2, predcoef, (SMPL_LPC_ORDER + 1) * sizeof(float));
	smpl_bwe_expand(A1, SMPL_LPC_ORDER, lpc_postfilt_gamma[low_rate][voiced][0]);
	smpl_bwe_expand(A2, SMPL_LPC_ORDER, lpc_postfilt_gamma[low_rate][voiced][1]);
	char impz_[(LPC_POST_IMPZ_LEN + SMPL_LPC_ORDER) * sizeof(float) + MALLOC_PERCW_NFFT_ALIGNMENT];
	float* impz = (float*)(((size_t)impz_ + MALLOC_PERCW_NFFT_ALIGNMENT) & (~((size_t)(MALLOC_PERCW_NFFT_ALIGNMENT - 1)))) + SMPL_LPC_ORDER;
	float* temp = (float*)(((size_t)temp_mem + MALLOC_PERCW_NFFT_ALIGNMENT) & (~((size_t)(MALLOC_PERCW_NFFT_ALIGNMENT - 1))));
	memset(impz - SMPL_LPC_ORDER, 0, SMPL_LPC_ORDER * sizeof(float));
	memcpy(impz, A1, (SMPL_LPC_ORDER + 1) * sizeof(float));
	memset(&impz[(SMPL_LPC_ORDER + 1)], 0, (LPC_POST_IMPZ_LEN - (SMPL_LPC_ORDER + 1)) * sizeof(float));
	smpl_filt_ar16(impz, LPC_POST_IMPZ_LEN, A2, impz);
	TIC(lpc_pf_gain_tilt)
	float* H = (float*)(((size_t)H_mem + MALLOC_PERCW_NFFT_ALIGNMENT) & (~((size_t)(MALLOC_PERCW_NFFT_ALIGNMENT - 1))));
	pffft_transform_ordered(pFFT, impz, H, NULL, PFFFT_FORWARD);
	float maxHsq = SMPL_max(H[0] * H[0], H[1] * H[1]);
	for (int i = 2; i < LPC_POST_IMPZ_LEN; i += 2) {
		maxHsq = SMPL_max(H[i] * H[i] + H[i + 1] * H[i + 1], maxHsq);
	}
	float g = SMPL_min(1.0f, 1.0f / sqrtf(maxHsq + 1e-30f));
	nyquist_gain = g * sqrtf(H[1] * H[1] + 1e-30f);
	TOC(lpc_pf_gain_tilt)

#if SMPL_LPC_POSTFILT_FAST
	float state_tmp[LPC_POST_IMPZ_LEN];
	memcpy(state_tmp, &x[-LPC_POST_IMPZ_LEN], (LPC_POST_IMPZ_LEN) * sizeof(float));
	smpl_scale_vec(temp, impz, LPC_POST_IMPZ_LEN, g);
	memcpy(&x[-LPC_POST_IMPZ_LEN], pSt->state_ma, (LPC_POST_IMPZ_LEN) * sizeof(float));
	smpl_filt_ma(x, len, impz, LPC_POST_IMPZ_LEN, temp);
	memcpy(pSt->state_ma, &x[len - LPC_POST_IMPZ_LEN], (LPC_POST_IMPZ_LEN) * sizeof(float));
	memcpy(x, temp, len * sizeof(float));
	memcpy(&x[-LPC_POST_IMPZ_LEN], state_tmp, LPC_POST_IMPZ_LEN * sizeof(float));
#else
    float state_tmp[SMPL_LPC_ORDER + 1];
    memcpy(state_tmp, &x[-SMPL_LPC_ORDER - 1], (SMPL_LPC_ORDER + 1) * sizeof(float));

	memcpy(&x[-SMPL_LPC_ORDER], pSt->state_ma, SMPL_LPC_ORDER * sizeof(float));
	smpl_scale_vec_inplace(A1, SMPL_LPC_ORDER + 1, g);
	smpl_filt_ma(x, len, A1, SMPL_LPC_ORDER + 1, temp);
	memcpy(pSt->state_ma, &x[len - SMPL_LPC_ORDER], SMPL_LPC_ORDER * sizeof(float));

    memcpy(&x[-SMPL_LPC_ORDER], pSt->state_ar, SMPL_LPC_ORDER * sizeof(float));
	smpl_filt_ar16(temp, len, A2, x);
    memcpy(pSt->state_ar, &x[len - SMPL_LPC_ORDER], SMPL_LPC_ORDER * sizeof(float));

    memcpy(&x[-SMPL_LPC_ORDER - 1], state_tmp, (SMPL_LPC_ORDER + 1) * sizeof(float));
#endif
	TOC(lpc_postfilter)
	return nyquist_gain;
}

void smpl_lpc_postfilter_state_upd(
	LpcPostfilter* pSt,
	float x[],
	int len
)
{
	memcpy(pSt->state_ma, &x[len - SMPL_LPC_ORDER], SMPL_LPC_ORDER * sizeof(float));
	memcpy(pSt->state_ar, &x[len - SMPL_LPC_ORDER], SMPL_LPC_ORDER * sizeof(float));
}

#endif

void* smpl_create_hp_postfilter(void)
{
	HpPst* pHpPst = (HpPst*)calloc(1,sizeof(HpPst));
	if (!pHpPst) {
		return NULL;
	}
	pHpPst->lag_old = -1.0f;

	return (void*)pHpPst;
}

static inline void get_hp_pitch_coefs(HpPst* pHpPst, float lag)
{
	smpl_assert(lag > 0.0f);
	// 0.5 dB peak
	// const float maf = 0.100f;
	// const float arf[2] = { 0.472770843f, 0.049300269f};
	// const float arr[2] = {-2.046446422f, 1.971171746f};
	// 1.2 dB peak
	const float maf = 0.100f;
	const float arf[2] = { 0.608057355f, 0.070939485f};
	const float arr[2] = {-2.187380512f, 2.291030664f};
	// // 2 dB peak
	// const float maf = 0.100f;
	// const float arf[2] = { 0.700345021f, 0.049784405f};
	// const float arr[2] = {-2.140379397f, 2.332905985f};	
	const float f = 1.0f / lag;
	smpl_assert(f > 0.001f); // Pitch frequency too low
	smpl_assert(f < 0.07f); // Pitch frequency too high
	smpl_calc_hp_coefs(maf, arf, arr, f, pHpPst->coef_ma, pHpPst->coef_ar);
}

void smpl_hp_postfilter_init(
	HpPst* pHpPst
)
{
	memset(pHpPst, 0, sizeof(HpPst));
	pHpPst->lag_old = -1.0f;
}

void new_coefs(HpPst* pHpPst, float lag)
{
	if(lag > 0.0f){
		get_hp_pitch_coefs(pHpPst, lag);
	}else{
		smpl_get_hp_coefs(SMPL_HP_POSTF_FCORNER_3DB_HZ, pHpPst->coef_ma, pHpPst->coef_ar);
	}
}
