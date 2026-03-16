 #include <gtest/gtest.h>
#include "smpl_bitrate_controller.h"
#include "smpl_celp.h"
#include "smpl_errors.h"
#include "memory.h"
#include <math.h>
#include <fenv.h>

TEST(SmplCelpEncoder, TestAllZero)
{
	void* tbls = smpl_create_celp_tables();
	EXPECT_TRUE(tbls != nullptr);

	const int subfr_per_packet = 2;
	const int maxpitch = 20 * 16;
	const int lpcorder = SMPL_LPC_ORDER;

	float res_lpc[SMPL_MAX_SF_LEN];
	memset(res_lpc, 0, sizeof(res_lpc));
	float predcoef[SMPL_LPC_ORDER + 1];
	memset(predcoef, 0, sizeof(predcoef));
	predcoef[0] = 1.0f;
	float perc_wght_resp[SMPL_MAX_SF_LEN];
	memset(perc_wght_resp, 0, sizeof(perc_wght_resp));
	int L_resp = SMPL_MAX_L_RESP;
	float lags[4];
	memset(lags, 0, sizeof(lags));
	float subfr_importance[SMPL_CELP_MAX_RATES];
	memset(subfr_importance, 0, SMPL_CELP_MAX_RATES * sizeof(float));
	int16_t surv[SMPL_MAX_SF_LEN];
	for (int i = 0; i < SMPL_MAX_SF_LEN; i++) {
		surv[i] = 4;
	}
	int16_t pulses[SMPL_CELP_MAX_RATES][SMPL_MAX_PULSES_PER_SF];
	int16_t acb_idx[SMPL_CELP_MAX_RATES], gain_idx[SMPL_CELP_MAX_RATES];
	for (int16_t fcb_pulses_max_ : {0, 10, 64}) {
		for (bool low_rate : {false, true}) {
			const int fcb_subfrlen = low_rate ? 160 : 80;
			for (bool voiced : {false, true}) {

				feclearexcept(FE_ALL_EXCEPT);

				void* celpEncSt = smpl_create_celp_encoder();
				EXPECT_TRUE(celpEncSt != NULL);

				//smpl_init_celp_encoder(celpEncSt);

				smpl_update_celp_params(celpEncSt, fcb_subfrlen, subfr_per_packet, L_resp, SMPL_FALSE, low_rate);

				int min_lag = voiced ? 2 * 16 : 0;
				int max_lag = voiced ? 20 * 16 : 0;
				for (int lag = min_lag; lag <= max_lag; lag++) {
					for (int i = 0; i < (fcb_subfrlen / SMPL_LAG_SUBFRLEN); i++) {
						lags[i] = (float)lag;
					}
					int16_t fcb_pulses_max[SMPL_CELP_MAX_RATES];
					for (int i = 0; i < SMPL_CELP_MAX_RATES; i++) {
						fcb_pulses_max[i] = fcb_pulses_max_;
					}
					int16_t n_pulses[SMPL_CELP_MAX_RATES];
					smpl_celp_encoder(celpEncSt, res_lpc, predcoef, perc_wght_resp, lags, subfr_importance, fcb_pulses_max, surv, pulses, n_pulses, acb_idx, gain_idx);
					EXPECT_EQ(n_pulses[SMPL_CELP_IDX_MAIN], 0);
					if (voiced) {
						EXPECT_TRUE(acb_idx[SMPL_CELP_IDX_MAIN] >= 0);
					}
					else {
						EXPECT_TRUE(acb_idx[SMPL_CELP_IDX_MAIN] == -1);
					}
					EXPECT_TRUE(gain_idx[SMPL_CELP_IDX_MAIN] == -1);

					int excpt_reg = fetestexcept((FE_DIVBYZERO | FE_INVALID | FE_OVERFLOW | FE_UNDERFLOW)); // FE_INEXACT not tested
					EXPECT_EQ(excpt_reg, 0); // Test that code didnt raise any flp exceptions
				}

				smpl_free_celp_encoder(celpEncSt);
			}
		}
	}
	smpl_free_celp_tables();
}

TEST(SmplGetNonFlatness, TestAllZero)
{
	for (int L : {80, 160, 320}){
		std::vector<float> res_lpc(L, 0.0f);
		std::vector<float> wlsf(SMPL_LPC_ORDER, 0.0f);
		std::vector<float> state(SMPL_NON_FLAT_STATE_LEN, 0.0f);

		float ret = smpl_get_nonflatness(res_lpc.data(), L, wlsf.data(), state.data());
		EXPECT_TRUE(!isnan(ret));
	}
}