#include "tlcs_mode.h"
#include <stdlib.h>

/* Frame-level mode decision for hybrid LP+TCX codec.
 *
 * All rates: always Mode T (TCX). TCX with range-coded dead-zone quantizer
 * + band-shaped noise fill + adaptive SBR outperforms CELP at every tier:
 *   VLR (5.0k): TCX 2.98 vs CELP 1.87 SCOREQ (+1.11)
 *   LR  (9.6k): TCX 3.24 vs CELP 2.47 SCOREQ (+0.77)
 *   HR  (25k):  TCX 4.09 vs CELP 3.95 SCOREQ (+0.14)
 */

int32_t tlcs_mode_decide(const float *speech, int32_t frame_size,
                          int32_t bitrate, int32_t prev_mode,
                          int32_t *hold_count)
{
    (void)speech;
    (void)frame_size;
    (void)bitrate;
    (void)prev_mode;

    *hold_count = 3;
    /* Check TLCS_MODE env var: celp/tcx/neural */
    {
        static int checked = 0, force_mode = -1;
        if (!checked) {
            const char *m = getenv("TLCS_MODE");
            if (m) {
                if (m[0] == 'c' || m[0] == 'C') force_mode = TLCS_CODEC_MODE_S;
                else if (m[0] == 'n' || m[0] == 'N') force_mode = TLCS_CODEC_MODE_N;
            }
            checked = 1;
        }
        if (force_mode >= 0) return force_mode;
    }
    return TLCS_CODEC_MODE_T;
}
