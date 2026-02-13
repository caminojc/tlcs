#include "tlcs_frame.h"
#include <string.h>

void tlcs_frame_buf_init(tlcs_frame_buf *fb, int32_t frame_size)
{
    memset(fb, 0, sizeof(*fb));
    fb->frame_size   = frame_size;
    fb->history_size = TLCS_FRAME_HISTORY;
}

void tlcs_frame_buf_push(tlcs_frame_buf *fb, const int16_t *pcm, int32_t n)
{
    /* Shift tail of previous frame into history region */
    if (fb->frame_size > 0) {
        memmove(fb->buf,
                fb->buf + fb->frame_size,
                (size_t)fb->history_size * sizeof(int16_t));
    }
    /* Copy new frame into frame region */
    int32_t copy_n = (n < fb->frame_size) ? n : fb->frame_size;
    memcpy(fb->buf + fb->history_size, pcm, (size_t)copy_n * sizeof(int16_t));
    /* Zero-pad if short */
    if (copy_n < fb->frame_size) {
        memset(fb->buf + fb->history_size + copy_n, 0,
               (size_t)(fb->frame_size - copy_n) * sizeof(int16_t));
    }
}
