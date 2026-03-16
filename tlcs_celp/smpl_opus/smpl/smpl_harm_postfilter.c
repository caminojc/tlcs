#include <math.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include "smpl_postfilter.h"
#include "smpl_codec_util.h"
#include "smpl_defines.h"
#include "smpl_filt.h"
#include "smpl_typedef.h"
#include "silk/debug.h"

void* g_smpl_harm_postfilt_tables = NULL;

static inline int lag_to_filt_ix(int lag) {
    smpl_assert(lag != 0);
    return SMPL_HARM_POSTF_LP_FILT_RES / SMPL_max(lag + 30, 80) - SMPL_HARM_POSTF_LP_FILT_RES / SMPL_MAX_PITCH_LAG;
}

static inline void create_lp_filter(float omega0, const float *FiltWin, float *bLP) {
    float omegaC = SMPL_min(omega0 * SMPL_HARM_POSTF_NHARM_CUTOFF, SMPL_HARM_POSTF_CUTOFF_HZ / 16000.0f * SMPL_PI);
    float sumB = 0.0f;
    float omegaC_sum = omegaC;
    for (int i = 0; i < SMPL_HARM_POSTF_FB_DELAY; i++) {
        float b = FiltWin[i] * sinf(omegaC_sum);
        omegaC_sum += omegaC;
        bLP[SMPL_HARM_POSTF_FB_DELAY + i + 1] = b;
        bLP[SMPL_HARM_POSTF_FB_DELAY - i - 1] = b;
        sumB += 2.0f * b;
    }
    bLP[SMPL_HARM_POSTF_FB_DELAY] = omegaC;
    sumB += omegaC;
    smpl_scale_vec_inplace(bLP, 2 * SMPL_HARM_POSTF_FB_DELAY + 1, 1.0f / sumB);
}

void* smpl_create_harm_postfilt_tables(void)
{
	if (g_smpl_harm_postfilt_tables) {
		return g_smpl_harm_postfilt_tables;
	}

    smpl_assert((SMPL_HARM_POSTF_DELAY % SMPL_HARM_POSTF_LAG_SUBFR_LEN) == 0);

	HarmPostFiltTables* pSt = (HarmPostFiltTables*)calloc(1, sizeof(HarmPostFiltTables));
	if (!pSt) {
		smpl_assert(0);
		return NULL;
	}
        
    float FiltWin[SMPL_HARM_POSTF_FB_DELAY];
    float dOmega = (0.5f * SMPL_PI) / (SMPL_HARM_POSTF_FB_DELAY + 1.0f);
    float omega = dOmega;
    for (int i = 0; i < SMPL_HARM_POSTF_FB_DELAY; i++) {
        FiltWin[i] = cosf(omega) / (i + 1);
        omega += dOmega;
    }

    int ix_prev = -1;
    for (int lag = SMPL_MIN_PITCH_LAG; lag <= SMPL_MAX_PITCH_LAG; lag++) {
        int ix = lag_to_filt_ix(lag);
        smpl_assert(ix >= 0);
        smpl_assert(ix < SMPL_HARM_POSTF_NUM_LP_FILT);
        if (ix != ix_prev) {
            float omega0 = 2.0f * SMPL_PI / lag;
            create_lp_filter(omega0, FiltWin, pSt->LP_Filters[ix]);
            ix_prev = ix;
        }
    }

	g_smpl_harm_postfilt_tables = (void*)pSt;
	return g_smpl_harm_postfilt_tables;
}

void smpl_free_harm_postfilt_tables(void)
{
	if (!g_smpl_harm_postfilt_tables) {
		return;
	}
    free(g_smpl_harm_postfilt_tables);
    g_smpl_harm_postfilt_tables = NULL;
}

void* smpl_create_harm_postfilter(void)
{
    HarmPst *pHarmPst = (HarmPst*)calloc(1, sizeof(HarmPst));
    if(!pHarmPst){
        return NULL;
    }
    return (void*)pHarmPst;
}

void smpl_init_harm_postfilter(HarmPst* pHarmPst)
{
    memset(pHarmPst, 0, sizeof(HarmPst));
}

void smpl_free_harm_postfilter(void *st)
{
    if(st){
        free(st);
    }
}
