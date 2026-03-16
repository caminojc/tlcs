#ifndef SMPL_BWE_H
#define SMPL_BWE_H

#include "smpl_structs.h"
#include "smpl_defines.h"

typedef struct HB_LSF_CBs {
	uint16_t *hb_lpc_vq_cmfs_table;
	uint16_t *hb_lpc_vq_cmfs[2][2];
	float    *hb_lpc_vq_cb_conv_table;
	float    *hb_lpc_vq_cb_conv[2][2];
	float    *hb_lpc_vq_cb_lsf_table;
	float    *hb_lpc_vq_cb_lsf[2][2];
	uint16_t *hb_lpc_vq_cmfs_cond_table;
	uint16_t *hb_lpc_vq_cmfs_cond[2];
	float    *hb_lpc_vq_lam_prob_table;
	float    *hb_lpc_vq_lam_prob[2][2];
	float    *hb_lpc_vq_lam_prob_cond_table;
	float    *hb_lpc_vq_lam_prob_cond[2];
} HB_LSF_CBs;

typedef struct HB_GAIN_CBs_ {
	float    *hb_gain_vq_shapes_table;
	float    *hb_gain_vq_shapes[2][2][2];
	uint16_t *hb_gain_vq_cmfs_table;
	uint16_t *hb_gain_vq_cmfs[2][2][2];
	float    *hb_gain_vq_lam_bits_table;
	float    *hb_gain_vq_lam_bits[2][2][2];
} HB_GAIN_CBs;

#ifdef __cplusplus
extern "C" {
#endif
void* smpl_create_hb_encoder(void);
void smpl_init_hb_encoder(void* pSt);
void smpl_free_hb_encoder(void* pSt);
void* smpl_load_hb_lsf_CBks(void);
void* smpl_load_hb_gain_CBks(void);
void smpl_free_hb_lsf_CBks(void);
void smpl_free_hb_gain_CBks(void);
const HB_LSF_CBs* smpl_get_hb_lsf_CBks(void);
const HB_GAIN_CBs* smpl_get_hb_gain_CBks(void);

void smpl_hb_lsf_dequant(int qi, int voiced, int low_rate, float *lsf);

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
	int low_rate);

void smpl_hb_gain_dequant(
	const int gain_qi,
	const int voiced,
	const int low_rate,
	const int num_hb_subframes,
	float hb_gains[],
	const float* x_pre_wght,
	float low_nrg_frame);

void smpl_hb_wght_lb(
	float lb_wght_mem[],
	const int num_hb_subframes,
	float x_pre_postfilter[], // input
	float y_wght[],           // output
	float* low_nrg_frame);

void smpl_hb_encoder(
	void* pSt,
	HbQuantParams* pHbParams,
	const int voiced,
	const int cond_coding,
	const int low_rate,
	const float* x_16_lb, // reconstructed from CELP params
	const float* x_16_hb,
	const int num_hb_subframes,
	const int bitrate);

#ifdef __cplusplus
}
#endif

#endif
