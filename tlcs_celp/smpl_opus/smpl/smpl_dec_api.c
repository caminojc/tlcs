#include "smpl_api.h"
#include "smpl_typedef.h"
#include <string.h>
#include "smpl_core_decoder.h"
#include "smpl_param_coding.h"
#include "silk/SigProc_FIX.h"
#include "silk/debug.h"

#if defined(ENABLE_SMPL)

opus_int smpl_Created(void);

/*********************/
/* Decoder functions */
/*********************/

opus_int smpl_Get_Decoder_Size(                         /* O    Returns error code                              */
    opus_int                        *decSizeBytes       /* O    Number of bytes in SNPL decoder state           */
)
{
    opus_int ret = SMPL_NO_ERROR;

    *decSizeBytes = sizeof( smpl_decoder );

    return ret;
}

/* Reset decoder state */
opus_int smpl_InitDecoder(                              /* O    Returns error code                              */
    void                            *decState           /* I/O  State                                           */
)
{
    opus_int ret = SMPL_NO_ERROR;
    smpl_decoder* dec = (smpl_decoder*)decState;
    memset(dec, 0, sizeof(smpl_decoder));
    smpl_core_decode_init(&(dec->smpl_core_decoder_state[0]));
    smpl_core_decode_init(&(dec->smpl_core_decoder_state[1]));
    return ret;
}

opus_int smpl_Decode(                                   /* O    Returns error code                              */
    void                            *decState,          /* I/O  State                                           */
    smpl_DecControlStruct           *decControl,        /* I/O  Control Structure                               */
    opus_int                        lostFlag,           /* I    0: no loss, 1 loss, 2 decode fec                */
    opus_int                        newPacketFlag,      /* I    Indicates first decoder call for this packet    */
    ec_dec                          *psRangeDec,        /* I/O  Compressor data structure                       */
    unsigned char                   toc_byte,           /* I    TOC byte describing the payload                 */
    opus_int16                      *samplesOut,        /* O    Decoded output speech vector                    */
    opus_int32                      *nSamplesOut        /* I/O  I: Buffer size (per channel)                    */
                                                        /*      O: Number of samples decoded per channel        */
)
{
    if (smpl_Created() == SMPL_FALSE) {
        return SMPL_ENC_NO_GLOBAL_DATA;
    }
    int ret = 0;
    opus_int16 samplesBuf[120 * 48];
    if ((decControl->nChannelsAPI < 1) || (decControl->nChannelsAPI > 2)) {
        return SMPL_ENC_INVALID_NUMBER_OF_CHANNELS_ERROR;
    }
    if (decControl->API_sampleRate != 8000 && decControl->API_sampleRate != 16000 && decControl->API_sampleRate != 24000 && decControl->API_sampleRate != 32000 &&
        decControl->API_sampleRate != 44100 && decControl->API_sampleRate != 48000) {
        return(SMPL_DEC_INVALID_SAMPLING_FREQUENCY);
    }
    smpl_decoder* dec = (smpl_decoder*)decState;
    smpl_TOC toc;
    if (lostFlag == SMPL_FLAG_DECODE_LBRR) {
        smpl_decode_toc(toc_byte, &toc);
        if (!toc.FEC) {
            lostFlag = SMPL_FLAG_PACKET_LOST;
        }
    }
    
    if (lostFlag == SMPL_FLAG_PACKET_LOST) {
        memcpy(&toc, &((smpl_core_decoder*)decState)->plc.toc, sizeof(smpl_TOC));
    } else {
        smpl_decode_toc(toc_byte, &toc);
        decControl->payloadSize_ms = toc.packet_len_ms;
    }
    decControl->internalSampleRate = toc.fs_Hz;
    decControl->nChannelsInternal = (toc.stereo == 1) ? 2 : 1;
    if (*nSamplesOut < ((toc.packet_len_ms * decControl->API_sampleRate) / 1000) && !lostFlag) {
        return(SMPL_DEC_PAYLOAD_ERROR);
    }

    // Reinitialize second channel decoder if previous was mono and now moving to stereo
    if (toc.stereo && !dec->smpl_core_decoder_state[0].plc.toc.stereo) {
        memcpy(&(dec->smpl_core_decoder_state[1]), &(dec->smpl_core_decoder_state[0]), sizeof(smpl_core_decoder));
    }

    if (decControl->nChannelsAPI == 1) {
        opus_int32 nSamplesOut_secondchannel = *nSamplesOut;
        decControl->isLastChannel = !toc.stereo;
        ret = smpl_core_decode(&(dec->smpl_core_decoder_state[0]), decControl, &toc, lostFlag, newPacketFlag, psRangeDec, samplesOut, nSamplesOut);
        if (toc.stereo) {
            // Decode to get range decoder in sync (also in case anything additional comes after)
            decControl->isLastChannel = SMPL_TRUE;
            ret += smpl_core_decode(&(dec->smpl_core_decoder_state[1]), decControl, &toc, lostFlag, newPacketFlag, psRangeDec, samplesBuf, &nSamplesOut_secondchannel);
            for (int i = 0; i < (*nSamplesOut); i++) {
                samplesOut[i] = (samplesOut[i] + samplesBuf[i] + 1) / 2;
            }
        }
        return ret;
    }
    else {
        decControl->isLastChannel = !toc.stereo;
        ret = smpl_core_decode(&(dec->smpl_core_decoder_state[0]), decControl, &toc, lostFlag, newPacketFlag, psRangeDec, samplesBuf, nSamplesOut);
        if (ret != SMPL_NO_ERROR) {
            return(SMPL_DEC_PAYLOAD_ERROR);
        }
        if (!toc.stereo) {
            // Mono -> Stereo
            for (int i = 0; i < (*nSamplesOut); i++) {
                samplesOut[i * 2]     = samplesBuf[i];
                samplesOut[i * 2 + 1] = samplesBuf[i];
            }
            return ret;
        }
        else {
            decControl->isLastChannel = SMPL_TRUE;
            ret += smpl_core_decode(&(dec->smpl_core_decoder_state[1]), decControl, &toc, lostFlag, newPacketFlag, psRangeDec, &samplesOut[(*nSamplesOut)], nSamplesOut);
            for (int i = 0; i < (*nSamplesOut); i++) {
                samplesOut[i * 2]     = samplesBuf[i];
                samplesOut[i * 2 + 1] = samplesOut[(*nSamplesOut) + i];
            }
            return ret;
        }
    }
}

void smpl_open_dec_files(void* decState)          /* I/O  State                                           */
{
#if SMPL_DUMP_FEATURES
    smpl_decoder* dec = (smpl_decoder*)decState;

    if (dec->smpl_core_decoder_state[0].fp_noisy) {
        fclose(dec->smpl_core_decoder_state[0].fp_noisy);
    }
    dec->smpl_core_decoder_state[0].fp_noisy = fopen("coded.s16", "wb");
    if (dec->smpl_core_decoder_state[0].fp_features_period) {
        fclose(dec->smpl_core_decoder_state[0].fp_features_period);
    }
    dec->smpl_core_decoder_state[0].fp_features_period = fopen("features_period.s16", "wb");
    if (dec->smpl_core_decoder_state[0].fp_features_lpc) {
        fclose(dec->smpl_core_decoder_state[0].fp_features_lpc);
    }
    dec->smpl_core_decoder_state[0].fp_features_lpc = fopen("features_lpc.f32", "wb");
    if (dec->smpl_core_decoder_state[0].fp_features_gain) {
        fclose(dec->smpl_core_decoder_state[0].fp_features_gain);
    }
    dec->smpl_core_decoder_state[0].fp_features_gain = fopen("features_gain.f32", "wb");
    if (dec->smpl_core_decoder_state[0].fp_features_ltp) {
        fclose(dec->smpl_core_decoder_state[0].fp_features_ltp);
    }
    dec->smpl_core_decoder_state[0].fp_features_ltp = fopen("features_ltp.f32", "wb");
    if (dec->smpl_core_decoder_state[0].fp_features_num_bits) {
        fclose(dec->smpl_core_decoder_state[0].fp_features_num_bits);
    }
    dec->smpl_core_decoder_state[0].fp_features_num_bits = fopen("features_num_bits.s32", "wb");
    if (dec->smpl_core_decoder_state[0].fp_features_num_bits_smooth) {
        fclose(dec->smpl_core_decoder_state[0].fp_features_num_bits_smooth);
    }
    dec->smpl_core_decoder_state[0].fp_features_num_bits_smooth = fopen("features_num_bits_smooth.f32", "wb");
    if (dec->smpl_core_decoder_state[0].fp_features_lsf) {
        fclose(dec->smpl_core_decoder_state[0].fp_features_lsf);
    }
    dec->smpl_core_decoder_state[0].fp_features_lsf = fopen("features_lsf.f32", "wb");
    if (dec->smpl_core_decoder_state[0].fp_features_offset) {
        fclose(dec->smpl_core_decoder_state[0].fp_features_offset);
    }
    dec->smpl_core_decoder_state[0].fp_features_offset = fopen("features_offset.f32", "wb");
    if (dec->smpl_core_decoder_state[0].fp_features_packet_losses) {
        fclose(dec->smpl_core_decoder_state[0].fp_features_packet_losses);
    }
    dec->smpl_core_decoder_state[0].fp_features_packet_losses = fopen("features_packet_losses.s8", "wb");
    if (dec->smpl_core_decoder_state[0].fp_exclpc_dec) {
        fclose(dec->smpl_core_decoder_state[0].fp_exclpc_dec);
    }
    dec->smpl_core_decoder_state[0].fp_exclpc_dec = fopen("exclpc_dec.f32", "wb");    
    if (dec->smpl_core_decoder_state[0].fp_coded_hb) {
        fclose(dec->smpl_core_decoder_state[0].fp_coded_hb);
    }
    dec->smpl_core_decoder_state[0].fp_coded_hb = fopen("coded_hb.s16", "wb");    
    if (dec->smpl_core_decoder_state[0].fp_features_hb_lpc) {
        fclose(dec->smpl_core_decoder_state[0].fp_features_hb_lpc);
    }
    dec->smpl_core_decoder_state[0].fp_features_hb_lpc = fopen("features_hb_lpc.f32", "wb");
    if (dec->smpl_core_decoder_state[0].fp_features_hb_gain) {
        fclose(dec->smpl_core_decoder_state[0].fp_features_hb_gain);
    }
    dec->smpl_core_decoder_state[0].fp_features_hb_gain = fopen("features_hb_gain.f32", "wb");
#endif
}

void smpl_close_dec_files(void* decState)          /* I/O  State                                           */
{
#if SMPL_DUMP_FEATURES
    smpl_decoder* dec = (smpl_decoder*)decState;

    if (dec->smpl_core_decoder_state[0].fp_noisy) {
        fclose(dec->smpl_core_decoder_state[0].fp_noisy);
    }
    if (dec->smpl_core_decoder_state[0].fp_features_period) {
        fclose(dec->smpl_core_decoder_state[0].fp_features_period);
    }
    if (dec->smpl_core_decoder_state[0].fp_features_lpc) {
        fclose(dec->smpl_core_decoder_state[0].fp_features_lpc);
    }
    if (dec->smpl_core_decoder_state[0].fp_features_gain) {
        fclose(dec->smpl_core_decoder_state[0].fp_features_gain);
    }
    if (dec->smpl_core_decoder_state[0].fp_features_ltp) {
        fclose(dec->smpl_core_decoder_state[0].fp_features_ltp);
    }
    if (dec->smpl_core_decoder_state[0].fp_features_num_bits) {
        fclose(dec->smpl_core_decoder_state[0].fp_features_num_bits);
    }
    if (dec->smpl_core_decoder_state[0].fp_features_num_bits_smooth) {
        fclose(dec->smpl_core_decoder_state[0].fp_features_num_bits_smooth);
    }
    if (dec->smpl_core_decoder_state[0].fp_features_lsf) {
        fclose(dec->smpl_core_decoder_state[0].fp_features_lsf);
    }
    if (dec->smpl_core_decoder_state[0].fp_features_offset) {
        fclose(dec->smpl_core_decoder_state[0].fp_features_offset);
    }
    if (dec->smpl_core_decoder_state[0].fp_features_packet_losses) {
        fclose(dec->smpl_core_decoder_state[0].fp_features_packet_losses);
    }
    if (dec->smpl_core_decoder_state[0].fp_exclpc_dec) {
        fclose(dec->smpl_core_decoder_state[0].fp_exclpc_dec);
    }
    if (dec->smpl_core_decoder_state[0].fp_coded_hb) {
        fclose(dec->smpl_core_decoder_state[0].fp_coded_hb);
    }
    if (dec->smpl_core_decoder_state[0].fp_features_hb_lpc) {
        fclose(dec->smpl_core_decoder_state[0].fp_features_hb_lpc);
    }
    if (dec->smpl_core_decoder_state[0].fp_features_hb_gain) {
        fclose(dec->smpl_core_decoder_state[0].fp_features_hb_gain);
    }
#endif
}

#endif // #if defined(ENABLE_SMPL)
