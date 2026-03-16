#include "smpl_defines.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
extern const int       hb_gain_vq_sizes[2][2][2];      // 10/20, uv/v, hr/lr
extern const uint8_t*  hb_gain_vq_cb_dshapes[2][2][2]; // 10/20, uv/v, hr/lr
extern const float     hb_gain_vq_cb_min[2][2][2];     // 10/20, uv/v, hr/lr
extern const float     hb_gain_vq_cb_scale[2][2][2];   // 10/20, uv/v, hr/lr
extern const uint8_t*  hb_gain_vq_dcmfs[2][2][2];      // 10/20, uv/v, hr/lr
extern const float     hb_gain_vq_lambdas[2][2][2];    // 10/20, uv/v, hr/lr
extern const float     hb_gain_vq_pwrs[2][2][2];       // 10/20, uv/v, hr/lr
#ifdef __cplusplus
}
#endif
