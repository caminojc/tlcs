#include "smpl_perc_wght.h"
#include "smpl_typedef.h"
#include "smpl_codec_util.h"
#include "smpl_filt.h"
#include "smpl_lpc.h"
#include "pffft.h"
#include "smpl_perc_wght.h"
#include <string.h>
#include <math.h> 
#include "silk/debug.h"

static void* g_smpl_perc_model = NULL;

typedef struct PercModel {
    void* pffft;
    float smthcoef[PERCW_NFFT / 2 + 1];
} PercModel;

void* smpl_create_perc_model_tables(void)
{
    if (g_smpl_perc_model != NULL) {
        return g_smpl_perc_model;
    }

    PercModel* pSt = (PercModel*)calloc(1, sizeof(PercModel));
    if (pSt == NULL) {
        smpl_assert(0);
        return NULL;
    }
    pSt->pffft = (void*)pffft_new_setup(PERCW_NFFT, PFFFT_REAL);
    if (pSt->pffft == NULL) {
        smpl_assert(0);
        free(pSt);
        return NULL;
    }
    float fs_step = (PERCW_FS_KHZ * 1000.0f) / PERCW_NFFT;
    for (int i = 0; i < PERCW_NFFT / 2 + 1; i++) {
        float perc_width_per_bin = SMPL_PERC_MASK_SMTH * (fs_step * i + SMPL_PERC_MEL_FC_HZ) / fs_step;
        pSt->smthcoef[i] = perc_width_per_bin / (perc_width_per_bin + 1.0f);
    }
    g_smpl_perc_model = (void *)pSt;
    return (void *)pSt;
}

void smpl_free_perc_model_tables(void)
{
    if (g_smpl_perc_model == NULL) {
        return;
    }
    PercModel* pSt = (PercModel*)g_smpl_perc_model;
    pffft_destroy_setup((PFFFT_Setup*)pSt->pffft);
    free(pSt);
    g_smpl_perc_model = NULL;
}

static inline void smth_filt(float f2[PERCW_NFFT], const float smthcoef[(PERCW_NFFT / 2) + 1])
{
    float f2smth = f2[0];
    for (int i = 1; i < PERCW_NFFT / 2; i++) {
        float f2new = f2[2 * i];
        f2smth = f2new + smthcoef[i] * (f2smth - f2new);
        f2[2 * i] = f2smth;
    }
    f2[1] = f2[1] + smthcoef[PERCW_NFFT / 2] * (f2smth - f2[1]);
    f2smth = f2[1];
    for (int i = (PERCW_NFFT / 2) - 1; i > 0; i--) {
        float f2new = f2[2 * i];
        f2smth = f2new + smthcoef[i] * (f2smth - f2new);
        f2[2 * i] = f2smth;
    }
    f2[0] = f2[0] + smthcoef[0] * (f2smth - f2[0]);
}

#define MALLOC_PERCW_NFFT_ALIGNMENT 64

void smpl_perc_model(float* buf, const float xsubfr[], int xsubfr_len, int frame_ms, int is_last_subfr, float R[], int len_R)
{
    smpl_assert(g_smpl_perc_model != NULL);
    smpl_assert(xsubfr_len <= PERCW_NFFT);

    const PercModel* pSt = (PercModel*)g_smpl_perc_model;

    // Memory align buf_win and f
    char buf_win_mem[PERCW_NFFT * sizeof(float) + MALLOC_PERCW_NFFT_ALIGNMENT];
    char f_mem[      PERCW_NFFT * sizeof(float) + MALLOC_PERCW_NFFT_ALIGNMENT];
    float* buf_win = (float*)(((size_t)buf_win_mem + MALLOC_PERCW_NFFT_ALIGNMENT) & (~((size_t)(MALLOC_PERCW_NFFT_ALIGNMENT - 1))));
    float* f       = (float*)(((size_t)f_mem       + MALLOC_PERCW_NFFT_ALIGNMENT) & (~((size_t)(MALLOC_PERCW_NFFT_ALIGNMENT - 1))));

    memmove(buf, &buf[xsubfr_len - (SMPL_WINNEXT_WB_LONG_LEN - SMPL_WINNEXT_WB_LEN)], (PERCW_NFFT - xsubfr_len) * sizeof(float));
    memcpy(&buf[(PERCW_NFFT - xsubfr_len)], xsubfr, xsubfr_len * sizeof(float));

    int winlen = SMPL_WINPREV_PERC_LEN + frame_ms * 16 + SMPL_WIN3_LONG_LEN;
    int skip_samples = PERCW_NFFT - winlen;
    smpl_assert(frame_ms == 10 || skip_samples == 0);
    memset(buf_win, 0.0f, skip_samples * sizeof(float));
    smpl_window(buf + skip_samples, buf_win + skip_samples, winlen, frame_ms, is_last_subfr == SMPL_FALSE, SMPL_FALSE);

    pffft_transform_ordered((PFFFT_Setup*)pSt->pffft, buf_win, f, NULL, PFFFT_FORWARD);
    f[0] = f[0] * f[0]; // DC
    f[1] = f[1] * f[1]; // Nyquist
    for (int i = 1; i < PERCW_NFFT / 2; i++) {
        // interleaved real and imag
        f[2 * i] = f[2 * i] * f[2 * i] + f[2 * i + 1] * f[2 * i + 1];
        f[2 * i + 1] = 0.0f; //imag part 
    }
    smth_filt(f, pSt->smthcoef);
    pffft_transform_ordered((PFFFT_Setup*)pSt->pffft, f, buf_win, NULL, PFFFT_BACKWARD);
    smpl_scale_vec(buf_win, R, len_R, 1.0f / PERCW_NFFT);
}

void smpl_perc_ac2a(const float R[], int len_R, const float perc_emph, float A[], int perc_resp_len, float reg)
{
    smpl_assert(len_R           >= perc_resp_len + 1);
    smpl_assert(SMPL_MAX_L_RESP >= perc_resp_len);

    float b[3], state[2];
    b[0] = perc_emph;
    b[1] = 1.0f + perc_emph * perc_emph;
    b[2] = perc_emph;
    state[0] = R[0];
    state[1] = R[1];
    float R_[SMPL_MAX_L_RESP];
    smpl_filt_ma2(&R[1], perc_resp_len, b, 3, state, 2, R_);

    float rc[SMPL_MAX_L_RESP];
    smpl_ac2rc(R_, perc_resp_len - 1, reg, rc);
    
    smpl_rc2a(rc, perc_resp_len - 1, A);
}
