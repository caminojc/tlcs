#ifndef TLCS_MDCT_H
#define TLCS_MDCT_H

#include "tlcs/tlcs_types.h"

/* MDCT for TCX mode.
 * Transform size N = 2 * frame_size (640 for 16kHz 20ms).
 * Produces M = frame_size spectral bins.
 * Sine window, 50% overlap-add for perfect reconstruction. */

/* Forward MDCT.
 * prev_block: previous frame's data (frame_size samples) — from overlap memory
 * curr_block: current frame's data (frame_size samples)
 * spec_out:   output spectral coefficients (frame_size values)
 * frame_size: 320 for 16kHz 20ms */
void tlcs_mdct_forward(const float *prev_block, const float *curr_block,
                        float *spec_out, int32_t frame_size);

/* Inverse MDCT with overlap-add.
 * spec_in:     input spectral coefficients (frame_size values)
 * time_out:    output time-domain samples (frame_size values)
 * overlap_buf: decoder OLA memory (frame_size floats, updated in-place)
 * frame_size:  320 for 16kHz 20ms */
void tlcs_mdct_inverse(const float *spec_in, float *time_out,
                        float *overlap_buf, int32_t frame_size);

#endif /* TLCS_MDCT_H */
