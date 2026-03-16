#include "smpl_postfilter.h"
#include "smpl_codec_util.h"
#include "smpl_defines.h"
#include "smpl_filt.h"
#include "smpl_lpc.h"
#include "smpl_typedef.h"

static float lo_emph_coef[2] = {1.0f, -0.995f};
#define LAG_CHANGE_THRESHOLD  1.25f

void smpl_hp_postfilter(
	HpPst* pHpPst,
	float x[],
	int L,
	float const lags[],
	int n_lags,
	float y[]
)
{
	smpl_assert(L == SMPL_FRAME_LEN || L == (SMPL_FRAME_LEN / 2));
	const PostFiltTables* pTbl = (PostFiltTables*)g_smpl_postfilt_tables;
	smpl_assert(pTbl != NULL);
	smpl_filt_ar1(x, L, lo_emph_coef, 2, pHpPst->state_lo_emph1, 1, x);

	float lag = 0.0f;
	if (lags[0] > 0) {
		float sum_wghts = 0.0f;
		float sum_wlags = 0.0f;
		for (int i = 0; i < n_lags; i++) {
			sum_wghts += lags[i];
			sum_wlags += lags[i] * lags[i];
		}
		lag = sum_wlags / sum_wghts;
	}


	int overlap_add = SMPL_FALSE;
	float y_old[SMPL_FRAME_LEN];
	float y_tmp[SMPL_FRAME_LEN];
	if (pHpPst->lag_old < 0.0f) {
		new_coefs(pHpPst, lag);
		pHpPst->lag_old = lag;
	} else if (lag > LAG_CHANGE_THRESHOLD * pHpPst->lag_old || LAG_CHANGE_THRESHOLD * lag < pHpPst->lag_old) {
		overlap_add = SMPL_TRUE;
		smpl_filt_arma2(x, L, pHpPst->coef_ma, 3, pHpPst->coef_ar, 3, pHpPst->state_hp, 4, y_old);
		new_coefs(pHpPst, lag);
		pHpPst->lag_old = lag;
		float dummy[SMPL_FRAME_LEN];
		smpl_filt_arma2(pHpPst->x_old, L, pHpPst->coef_ma, 3, pHpPst->coef_ar, 3, pHpPst->state_hp, 4, dummy);
	} else if (lag != pHpPst->lag_old) {
		new_coefs(pHpPst, lag);
		pHpPst->lag_old = lag;
	}
	memcpy(pHpPst->x_old, x, L * sizeof(float));

	smpl_filt_arma2(x, L, pHpPst->coef_ma, 3, pHpPst->coef_ar, 3, pHpPst->state_hp, 4, y_tmp);

	if (overlap_add == SMPL_TRUE) {
		const float* ramp_dn = L == (20 * 16) ? pTbl->ramp_dn_20 : pTbl->ramp_dn_10;
		smpl_overlap_add(y_old, y_tmp, ramp_dn, L);
	}

	smpl_filt_ma1(y_tmp, L, lo_emph_coef, 2, pHpPst->state_lo_emph2, 1, y);
}
