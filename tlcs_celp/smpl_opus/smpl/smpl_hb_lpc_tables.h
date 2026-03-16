#ifndef SMPL_HB_LPC_H
#define SMPL_HB_LPC_H

#include "smpl_defines.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
#define SMPL_HB_LPC_CB_N_COND 8
extern const int16_t   hb_lpc_vq_sizes[2][2];     // uv/v, hr/lr
extern const uint8_t*  hb_lpc_vq_dcmfs[2][2];     // uv/v, hr/lr
extern const float*    hb_lpc_vq_cb_lsfs[2][2];   // uv/v, hr/lr
extern const uint8_t*  hb_lpc_vq_dcmfs_cond[2];   // uv/v
extern const int8_t*   hb_lpc_vq_sel_cond[2];     // uv/v
extern const float     hb_lpc_vq_lambdas[2][2];   // uv/v, hr/lr
extern const uint8_t*  hb_lpc_vq_cb_dlsf[2][2];   // uv/v, hr/lr
extern const uint8_t*  hb_lpc_vq_cb_scales[2][2]; // uv/v, hr/lr
extern const int16_t*  hb_lpc_vq_cb_min[2][2];    // uv/v, hr/lr
#ifdef __cplusplus
}
#endif

#endif
