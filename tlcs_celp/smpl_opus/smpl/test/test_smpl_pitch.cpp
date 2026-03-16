#include <gtest/gtest.h>
#include "smpl_pitch.h"
#include "smpl_errors.h"
#include "memory.h"
#include <math.h>
#include "pffft.h"

TEST(SmplPitchTest, CreateDestroy)
{
	void* pitchEstSt = smpl_create_pitch_estimator();
	EXPECT_TRUE(pitchEstSt != NULL);

	smpl_delete_pitch_estimator(pitchEstSt);
}

static void generate_sig(float sig[], int L, float lag, int nHarm)
{
	memset(sig, 0, L * sizeof(float));
	for (int harm = 0; harm < nHarm; harm++) {
		float omega = 0.0f;
		float dOmega = harm*(2.0f * 3.14f / lag);
		for (int i = 0; i < L; i++){
			sig[i] += sin(omega);
			omega += dOmega;
		}
	}
}

TEST(SmplPitchTest, AllLags)
{
	void* tables = smpl_load_pitch_tables();
	EXPECT_TRUE(tables != nullptr);

	void* pitchEstSt = smpl_create_pitch_estimator();
	EXPECT_TRUE(pitchEstSt != NULL);

	smpl_update_pitch_params(pitchEstSt, 8, SMPL_FALSE);

	#define L 659
	#define LOOK_AHEAD 7
	#define MALLOC_PERCW_NFFT_ALIGNMENT 64

	char ltp_buf_mem[L * sizeof(float) + MALLOC_PERCW_NFFT_ALIGNMENT];
	float* ltp_buf = (float*)(((size_t)ltp_buf_mem + MALLOC_PERCW_NFFT_ALIGNMENT) & (~((size_t)(MALLOC_PERCW_NFFT_ALIGNMENT - 1))));
	float lags[SMPL_PITCH_NUM_SUBFRAMES];
	int laginds[SMPL_PITCH_NUM_SUBFRAMES];
	float pitchc;
	int blockseg_idx;

	char F_mem[SMPL_LPC_NFFT * sizeof(float) + MALLOC_PERCW_NFFT_ALIGNMENT];
	float* F = (float*)(((size_t)F_mem + MALLOC_PERCW_NFFT_ALIGNMENT) & (~((size_t)(MALLOC_PERCW_NFFT_ALIGNMENT - 1))));


	int cnt = 0;
	for(int lag = SMPL_MINPITCH_MS * SMPL_PITCH_FS_KHZ; lag < SMPL_MAXPITCH_MS * SMPL_PITCH_FS_KHZ; lag++){
		if (cnt % 3 == 0) {
			smpl_pitch_reset_cond(pitchEstSt);
		}
		cnt++;
		generate_sig(ltp_buf, L, lag, 4);
		float avg_lag, harm_strength;

		float F2[SMPL_F_LEN];
		PFFFT_Setup *pffft_setup = pffft_new_setup(SMPL_LPC_NFFT, PFFFT_REAL);
		pffft_transform_ordered(pffft_setup, ltp_buf, F, NULL, PFFFT_FORWARD);
		F2[0] = F[0] * F[0];
		F2[SMPL_LPC_NFFT / 2] = F[1] * F[1];
		for (int i = 1; i < SMPL_LPC_NFFT / 2; i++) {
			F2[i] = F[2 * i] * F[2 * i] + F[2 * i + 1] * F[2 * i + 1];
		}

		smpl_pitch(pitchEstSt, ltp_buf, L, LOOK_AHEAD, F2, SMPL_TRUE, SMPL_PITCH_NUM_SUBFRAMES, lags, laginds, &pitchc, &blockseg_idx, &avg_lag, &harm_strength);

		EXPECT_TRUE(pitchc > 0.8f && pitchc < 1.1f);
		avg_lag = lags[0];
		for (int i = 1; i < SMPL_PITCH_NUM_SUBFRAMES; i++) {
			avg_lag += lags[i];
		}
		avg_lag /= SMPL_PITCH_NUM_SUBFRAMES;
		EXPECT_TRUE(avg_lag / lag > 0.95f && avg_lag / lag < 1.05f);
		EXPECT_TRUE(blockseg_idx >= 0 && blockseg_idx < NUM_BLOCKSEGS);
		pffft_destroy_setup(pffft_setup);
	}

	smpl_delete_pitch_estimator(pitchEstSt);

	smpl_free_pitch_tables();
}
