#include "tlcs/tlcs.h"
#include <string.h>

tlcs_status tlcs_config_init(tlcs_config *cfg, int32_t sample_rate, int32_t bitrate)
{
    if (!cfg) return TLCS_ERR_INVALID_ARG;
    if (sample_rate != 8000 && sample_rate != 16000) return TLCS_ERR_INVALID_ARG;
    if (bitrate < 4000 || bitrate > 32000) return TLCS_ERR_INVALID_ARG;

    memset(cfg, 0, sizeof(*cfg));
    cfg->sample_rate = sample_rate;
    cfg->bitrate     = bitrate;
    cfg->frame_size  = sample_rate / 1000 * TLCS_FRAME_MS;

    if (bitrate < TLCS_VLR_BITRATE_THRESHOLD) {
        cfg->n_subfr           = 4;
        cfg->subfr_size        = cfg->frame_size / cfg->n_subfr;
        cfg->lpc_order         = TLCS_LPC_ORDER_WB;
        cfg->num_pulses        = 2;
        cfg->lsf_bits          = 6;
        cfg->fcb_gain_bits     = 5;
        cfg->pitch_delta_bits  = 6;
        cfg->pitch_delta_offset = 32;
        cfg->use_ec            = 0;
        cfg->use_lsf_vq       = 1;
    } else if (bitrate < TLCS_LR_BITRATE_THRESHOLD) {
        cfg->n_subfr           = 4;
        cfg->subfr_size        = cfg->frame_size / cfg->n_subfr;
        cfg->lpc_order         = TLCS_LPC_ORDER_WB;
        cfg->num_pulses        = 5;
        cfg->lsf_bits          = 7;
        cfg->fcb_gain_bits     = 5;
        cfg->pitch_delta_bits  = 6;
        cfg->pitch_delta_offset = 32;
        cfg->use_ec            = 0;
        cfg->use_lsf_vq       = 1;
    } else {
        cfg->n_subfr           = 8;
        cfg->subfr_size        = cfg->frame_size / cfg->n_subfr;
        cfg->lpc_order         = (sample_rate == 16000) ? TLCS_LPC_ORDER_WB
                                                         : TLCS_LPC_ORDER_NB;
        cfg->num_pulses        = 10;
        cfg->lsf_bits          = 7;
        cfg->fcb_gain_bits     = 7;
        cfg->pitch_delta_bits  = 7;
        cfg->pitch_delta_offset = 64;
    }

    return TLCS_OK;
}
