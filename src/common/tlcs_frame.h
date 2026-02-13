#ifndef TLCS_FRAME_H
#define TLCS_FRAME_H

#include "tlcs/tlcs_types.h"

/* Frame buffer: holds one frame of samples plus overlap for windowed analysis.
 * The analysis window needs look-back for autocorrelation, so we keep
 * a small history buffer prepended to the current frame.
 */

#define TLCS_FRAME_HISTORY  TLCS_LPC_ORDER_MAX  /* overlap for filter memory init */

typedef struct {
    int16_t  buf[TLCS_FRAME_HISTORY + TLCS_MAX_FRAME_SIZE];
    int32_t  frame_size;
    int32_t  history_size;
} tlcs_frame_buf;

/* Initialize frame buffer. */
void tlcs_frame_buf_init(tlcs_frame_buf *fb, int32_t frame_size);

/* Push a new frame into the buffer. History is updated from previous frame tail. */
void tlcs_frame_buf_push(tlcs_frame_buf *fb, const int16_t *pcm, int32_t n);

/* Get pointer to current frame samples (after history). */
static inline int16_t *tlcs_frame_buf_frame(tlcs_frame_buf *fb)
{
    return fb->buf + fb->history_size;
}

/* Get pointer to start of buffer (includes history). */
static inline int16_t *tlcs_frame_buf_full(tlcs_frame_buf *fb)
{
    return fb->buf;
}

#endif /* TLCS_FRAME_H */
