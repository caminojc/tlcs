#include "smpl_typedef.h"
#include "smpl_structs.h"
#include "smpl_lsf_wrapper.h"
#include "smpl_lpc.h"
#include "smpl_codec_util.h"
#include "pffft.h"
#include "smpl_errors.h"
#include "silk/debug.h"
#include <string.h>
#include <math.h>

void  *g_smpl_lpc_tables     = NULL;
float *g_smpl_lpc_win1_10ms  = NULL;
float *g_smpl_lpc_win1_20ms  = NULL;
float *g_smpl_perc_win1_10ms = NULL;
float *g_smpl_perc_win1_20ms = NULL;
float *g_smpl_win3_short     = NULL;
float *g_smpl_win3_long      = NULL;

static void gen_sin_win(float *win, int N) {
    for (int i = 0; i < N; i++) {
        win[i] = sinf((i + 1.0f) / (N + 1) * SMPL_PI / 2);
    }
}

static void gen_cos_win(float *win, int N) {
    for (int i = 0; i < N; i++) {
        win[i] = cosf((i + 1.0f) / (N + 1) * SMPL_PI / 2);
    }
}

static void gen_cos_row(double *row, double domega, double scale) {
    double omega = 0.0;
    for(int k = 0; k < SMPL_LPC_NFFT/4; k++){
        row[k] = cos(omega) * scale;
        omega = fmod(omega + domega, 2.0 * SMPL_PI);
    }
}

void* smpl_create_lpc_tables(void)
{
    if (g_smpl_lpc_tables) {
        return g_smpl_lpc_tables;
    }
    LPCTables* pSt = (LPCTables*)calloc(1, sizeof(LPCTables));
    if (!pSt) {
        smpl_assert(0);
        return NULL;
    }    
    pSt->pffft_setup = pffft_new_setup(SMPL_LPC_NFFT, PFFFT_REAL);
    if (!pSt->pffft_setup) {
        smpl_assert(0);
        free(pSt);
        return NULL;
    }

    for (int j = 0; j < SMPL_LPC_ORDER / 2; j++)
    {
        gen_cos_row(pSt->Cdif[j], (1 + j * 2) * (2.0 * SMPL_PI) / SMPL_LPC_NFFT, 2.0 / SMPL_LPC_NFFT);
    }

    for (int j = 0; j < SMPL_LPC_ORDER / 4; j++)
    {
        gen_cos_row(pSt->Csumdiff[j], (2 + j * 4) * (2 * SMPL_PI) / SMPL_LPC_NFFT, 1.0 / SMPL_LPC_NFFT);
    }

    for (int j = 0; j < SMPL_LPC_ORDER / 4; j++)
    {
        gen_cos_row(pSt->Csumsum[j], (4 + j * 4) * (2 * SMPL_PI) / SMPL_LPC_NFFT, 1.0 / SMPL_LPC_NFFT);
    }

    g_smpl_lpc_tables = pSt;
    return pSt;    
}

void smpl_free_lpc_tables(void)
{
    if (g_smpl_lpc_tables) {
        LPCTables* pSt = (LPCTables*)g_smpl_lpc_tables;
        pffft_destroy_setup(pSt->pffft_setup);
        free(pSt);
        g_smpl_lpc_tables = NULL;
    }
}

int smpl_create_lpc_windows(void) {
    g_smpl_lpc_win1_10ms  = malloc(SMPL_LPC_WIN1_10MS_LEN  * sizeof(float));
    g_smpl_lpc_win1_20ms  = malloc(SMPL_LPC_WIN1_20MS_LEN  * sizeof(float));
    g_smpl_perc_win1_10ms = malloc(SMPL_PERC_WIN1_10MS_LEN * sizeof(float));
    g_smpl_perc_win1_20ms = malloc(SMPL_PERC_WIN1_20MS_LEN * sizeof(float));
    g_smpl_win3_short     = malloc(SMPL_WIN3_SHORT_LEN     * sizeof(float));
    g_smpl_win3_long      = malloc(SMPL_WIN3_LONG_LEN      * sizeof(float));
    if (g_smpl_lpc_win1_10ms  == NULL || g_smpl_lpc_win1_20ms  == NULL ||
        g_smpl_perc_win1_10ms == NULL || g_smpl_perc_win1_20ms == NULL ||
        g_smpl_win3_short     == NULL || g_smpl_win3_long      == NULL)
    {
        smpl_free_lpc_windows();
        smpl_assert(0);
        return(-1);
    }
    // Create the windows
    gen_sin_win(g_smpl_lpc_win1_10ms,  SMPL_LPC_WIN1_10MS_LEN);
    gen_sin_win(g_smpl_lpc_win1_20ms,  SMPL_LPC_WIN1_20MS_LEN);
    gen_sin_win(g_smpl_perc_win1_10ms, SMPL_PERC_WIN1_10MS_LEN);
    gen_sin_win(g_smpl_perc_win1_20ms, SMPL_PERC_WIN1_20MS_LEN);
    gen_cos_win(g_smpl_win3_short, SMPL_WIN3_SHORT_LEN);
    gen_cos_win(g_smpl_win3_long,  SMPL_WIN3_LONG_LEN);

    return(SMPL_NO_ERROR);
}

void smpl_free_lpc_windows(void) {
    if (g_smpl_lpc_win1_10ms) {
        free(g_smpl_lpc_win1_10ms);
        g_smpl_lpc_win1_10ms = NULL;
    }
    if (g_smpl_lpc_win1_20ms) {
        free(g_smpl_lpc_win1_20ms);
        g_smpl_lpc_win1_20ms = NULL;
    }
    if (g_smpl_perc_win1_10ms) {
        free(g_smpl_perc_win1_10ms);
        g_smpl_perc_win1_10ms = NULL;
    }
    if (g_smpl_perc_win1_20ms) {
        free(g_smpl_perc_win1_20ms);
        g_smpl_perc_win1_20ms = NULL;
    }
    if (g_smpl_win3_short) {
        free(g_smpl_win3_short);
        g_smpl_win3_short = NULL;
    }
    if (g_smpl_win3_long) {
        free(g_smpl_win3_long);
        g_smpl_win3_long = NULL;
    }
}

void smpl_window(const float *in, float* out, int len, int frame_ms, int use_long_win, int use_lpc_win) {
    int win1len = use_lpc_win ? ((frame_ms == 10) ? SMPL_LPC_WIN1_10MS_LEN  : SMPL_LPC_WIN1_20MS_LEN) :
                                ((frame_ms == 10) ? SMPL_PERC_WIN1_10MS_LEN : SMPL_PERC_WIN1_20MS_LEN);
    float *win1 = use_lpc_win ? ((frame_ms == 10) ? g_smpl_lpc_win1_10ms  : g_smpl_lpc_win1_20ms) :
                                ((frame_ms == 10) ? g_smpl_perc_win1_10ms : g_smpl_perc_win1_20ms);
    int win3len = use_long_win ? SMPL_WIN3_LONG_LEN : SMPL_WIN3_SHORT_LEN;
    float *win3 = use_long_win ? g_smpl_win3_long : g_smpl_win3_short;
    smpl_assert(win1);
    smpl_assert(win3);

    smpl_mul_vec(in, win1, out, win1len);
    memcpy(out + win1len, in + win1len, (len - win1len - SMPL_WIN3_LONG_LEN) * sizeof(float));
    smpl_mul_vec(&in[len - SMPL_WIN3_LONG_LEN], win3, &out[len - SMPL_WIN3_LONG_LEN], win3len);
    if (!use_long_win) {
        memset(out + len - SMPL_WIN3_LONG_LEN + SMPL_WIN3_SHORT_LEN, 0,
               (SMPL_WIN3_LONG_LEN - SMPL_WIN3_SHORT_LEN) * sizeof(float));
    }
}
