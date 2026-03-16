#ifndef TLCS_MODE_H
#define TLCS_MODE_H

#include "tlcs/tlcs_types.h"

/* Uses tlcs_codec_mode enum from tlcs_types.h:
 * TLCS_CODEC_MODE_S = 0 (CELP), TLCS_CODEC_MODE_T = 1 (TCX) */

/* Frame-level mode decision.
 * speech:      pre-emphasized speech (frame_size samples)
 * frame_size:  320 for 16kHz 20ms
 * bitrate:     target bitrate in bps
 * prev_mode:   previous frame's mode
 * hold_count:  hysteresis counter (updated in-place)
 * Returns: TLCS_CODEC_MODE_S or TLCS_CODEC_MODE_T */
int32_t tlcs_mode_decide(const float *speech, int32_t frame_size,
                          int32_t bitrate, int32_t prev_mode,
                          int32_t *hold_count);

#endif /* TLCS_MODE_H */
