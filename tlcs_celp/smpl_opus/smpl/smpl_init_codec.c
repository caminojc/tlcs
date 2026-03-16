
#include "smpl_api.h"
#include "smpl_lpc.h"
#include "smpl_celp.h"
#include "smpl_lsf_quant.h"
#include "smpl_perc_wght.h"
#include "smpl_pitch.h"
#include "smpl_pulse_coding.h"
#include "smpl_postfilter.h"
#include "smpl_quant_nrg_res.h"
#include "smpl_bandwidth_extension.h"
#include "smpl_typedef.h"
#include "smpl_codec_util.h"
#include "silk/define.h"
#include "silk/debug.h"

int g_global_created = SMPL_FALSE;

opus_int smpl_CreateCodec(void)
{
	TIC(CreateCodec)
	TIC(load_lsf)
	OPUS_UNUSED void* tbl = smpl_load_lsf_CBks();
	smpl_assert(tbl != NULL);
	TOC(load_lsf)
	TIC(load_lsf_hb)
	tbl = smpl_load_hb_lsf_CBks();
	smpl_assert(tbl != NULL);
	TOC(load_lsf_hb)
	tbl = smpl_load_hb_gain_CBks();
	smpl_assert(tbl != NULL);
	TIC(load_pulse_tables)
	tbl = smpl_create_pulse_tables();
	smpl_assert(tbl != NULL);
	TOC(load_pulse_tables)
	TIC(load_lpc_tables)
	tbl = smpl_create_lpc_tables();
	smpl_assert(tbl != NULL);
	TOC(load_lpc_tables)
	tbl = smpl_create_postfilt_tables();
	smpl_assert(tbl != NULL);
	tbl = smpl_create_harm_postfilt_tables();
	smpl_assert(tbl != NULL);
	tbl = smpl_create_celp_tables();
	smpl_assert(tbl != NULL);
	tbl = smpl_load_nrgresq_data();
	smpl_assert(tbl != NULL);
	TIC(load_pitch_tables)
	tbl = smpl_load_pitch_tables();
	smpl_assert(tbl != NULL);
	TOC(load_pitch_tables)
	if (smpl_create_lpc_windows() != SMPL_NO_ERROR) {
		smpl_assert(0);
		return -1;
	}
	tbl = smpl_create_perc_model_tables();
	smpl_assert(tbl != NULL);
	TOC(CreateCodec)
	g_global_created = SMPL_TRUE;

	return 0;
}

opus_int smpl_FreeCodec(void)
{
	smpl_free_lsf_CBks();
	smpl_free_hb_lsf_CBks();
	smpl_free_hb_gain_CBks();
	smpl_free_pulse_tables();
	smpl_free_lpc_tables();
	smpl_free_postfilt_tables();
	smpl_free_harm_postfilt_tables();
	smpl_free_celp_tables();
	smpl_free_nrgresq_data();
	smpl_free_pitch_tables();
	smpl_free_lpc_windows();
	smpl_free_perc_model_tables();
	g_global_created = SMPL_FALSE;
	return 0;
}

opus_int smpl_Created(void)
{
	return g_global_created;
}
