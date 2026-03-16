#ifndef SMPL_POSTFILTER_H
#define SMPL_POSTFILTER_H

#include <stdint.h>
#include "smpl_structs.h"
#include "smpl_defines.h"

#define SMPL_HARM_POSTF_LP_FILT_RES         2500  // higher means more filters
#define SMPL_HARM_POSTF_NUM_LP_FILT         ((SMPL_HARM_POSTF_LP_FILT_RES / 80) - SMPL_HARM_POSTF_LP_FILT_RES / SMPL_MAX_PITCH_LAG + 1)
typedef struct HarmPostFiltTables {
    float LP_Filters[SMPL_HARM_POSTF_NUM_LP_FILT][2 * SMPL_HARM_POSTF_FB_DELAY + 1];
} HarmPostFiltTables;

#ifdef SMPL_USE_LPC_POSTFILTER
#define LPC_POST_IMPZ_LEN SMPL_LPC_POST_IMPZ_LEN
static const float lpc_postfilt_gamma[2][2][2] = { { {0.9705f, 0.9833f}, {0.9086f, 0.9900f} },    // highRate: UV, V
                                                   { {0.9727f, 0.9691f}, {0.9359f, 0.9025f} } };  // lowRate:  UV, V, V
#endif 

typedef struct PostFiltTables {
#ifdef SMPL_USE_LPC_POSTFILTER
	PFFFT_Setup* pffft_setup;
#endif
	float ramp_dn_20[SMPL_FRAME_LEN];
	float ramp_dn_10[SMPL_FRAME_LEN/2];
} PostFiltTables;

extern void* g_smpl_harm_postfilt_tables;

extern void* g_smpl_postfilt_tables;

#ifdef __cplusplus
extern "C" {
#endif

    void* smpl_create_harm_postfilt_tables(void);
    void smpl_free_harm_postfilt_tables(void);
    void* smpl_create_harm_postfilter(void);
    void smpl_init_harm_postfilter(HarmPst* pHarmPst);
    void smpl_free_harm_postfilter(void* st);
    void smpl_harm_postfilter(void* st, float x[], const int x_len, float lags[], int nLags, float normalized_bitrate);

    void* smpl_create_postfilt_tables(void);
    void smpl_free_postfilt_tables(void);

#ifdef SMPL_USE_LPC_POSTFILTER
void* smpl_create_lpc_postfilter(void);
float smpl_lpc_postfilter(
    LpcPostfilter* pSt,
    float x[],
    int len,
    const float predcoef[SMPL_LPC_ORDER + 1],
    int voiced,
    int low_rate
);
void smpl_lpc_postfilter_state_upd(
    LpcPostfilter* pSt,
    float x[],
    int len
);
#endif

void* smpl_create_hp_postfilter(void);
void smpl_hp_postfilter_init(
    HpPst* pHpPst
);
void new_coefs(HpPst* pHpPst, float lag);
void smpl_hp_postfilter(
    HpPst* pHpPst,
    float x[],
    int L,
    float const lags[],
    int n_lags,
    float y[]
);

#ifdef __cplusplus
}
#endif

#endif
