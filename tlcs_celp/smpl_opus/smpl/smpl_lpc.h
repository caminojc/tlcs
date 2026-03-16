#ifndef SMPL_LPC_H
#define SMPL_LPC_H

#ifdef __cplusplus
extern "C" {
#endif

    extern void  *g_smpl_lpc_tables;
    extern float *g_smpl_lpc_win1_10ms;
    extern float *g_smpl_lpc_win1_20ms;
    extern float *g_smpl_perc_win1_10ms;
    extern float *g_smpl_perc_win1_20ms;
    extern float *g_smpl_win3_short;
    extern float *g_smpl_win3_long;

    void* smpl_create_lpc_tables(void);
    void smpl_free_lpc_tables(void);
    int smpl_create_lpc_windows(void);
    void smpl_free_lpc_windows(void);
    void smpl_window(const float *in, float* out, int len, int frame_ms, int use_long_win, int use_lpc_win);
    void smpl_lpc_stabilize(float A[], const int lpc_order);
    void smpl_NLSF2A_stabilize(float a[], const float nlsf[], const int d);
    void smpl_lpc_interpol(const float lsf[], float prev_lsf[], const float lsf_interpol[], int lpc_order, int num_subfr, float A[], float lsfs[]);

    void smpl_ac2rc(const float corr[], int order, float reg, float rc[]);
    void smpl_rc2a(const float rc[], int order, float A[]);
    void smpl_lpc(const float x[], int x_len, float reg, float A[], double R[], int order, float F2[]);
    void smpl_bwe_expand(float A[], const int lpc_order, const float bwe);
    int smpl_lpc_is_stable(const float A[], const int lpc_order);
    void smpl_spec_fact2(float c[3], float A[3]);

#ifdef __cplusplus
}
#endif

#endif
