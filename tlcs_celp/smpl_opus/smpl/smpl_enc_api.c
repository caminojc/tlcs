#include "smpl_api.h"
#include "smpl_typedef.h"
#include <string.h>
#include <stdlib.h>
#include "smpl_vad.h"
#include "silk/main.h"
#include "smpl_pitch.h"
#include "smpl_perc_wght.h"
#include "smpl_core_encoder.h"
#include "smpl_param_coding.h"
#include "smpl_defines.h"

#if defined(ENABLE_SMPL)

opus_int smpl_Created(void);

/****************************************/
/* Encoder functions                    */
/****************************************/

opus_int smpl_Get_Encoder_Size(                         /* O    Returns error code                              */
    opus_int                        *encSizeBytes       /* O    Number of bytes in SMPL encoder state           */
)
{
    opus_int ret = SMPL_NO_ERROR;

    *encSizeBytes = sizeof( smpl_encoder );

    return ret;
}

static void smpl_DTX_init(smpl_dtx_status *dtx_state) {
    dtx_state->hangover_ms = SMPL_DEFAULT_HANGOVER_MS;
    dtx_state->sid_interval_ms = SMPL_DEFAULT_SID_INTERVAL_MS;
    dtx_state->remaining_dtx_hangover = SMPL_DEFAULT_HANGOVER_MS;
    for (int i = 0; i < SMPL_DTX_NO_CANDIDATES; i++) {
        dtx_state->energy[i] = FLT_MAX;
    }
}

/*************************/
/* Init or Reset encoder */
/*************************/
opus_int smpl_InitEncoder(                              /* O    Returns error code                              */
    void                            *encState,          /* I/O  State                                           */
    smpl_EncControlStruct           *encStatus          /* O    Encoder Status                                  */
)
{
    smpl_encoder *psEnc;
    opus_int ret = SMPL_NO_ERROR;

    psEnc = (smpl_encoder *)encState;

    /* Reset encoder */
    memset( psEnc, 0, sizeof( smpl_encoder ) );

    smpl_VAD_Init(&(psEnc->sVAD));
    smpl_DTX_init(&(psEnc->dtx_state));

    for (int i=0; i<SMPL_ENCODER_NUM_CHANNELS; i++) {
        smpl_core_encoder_init(&(psEnc->smpl_core_encoder_state[i]), &psEnc->pitchScratch, &psEnc->celpScratch);
    }

    return ret;
}

/**************************/
/* Encode frame with Smpl */
/**************************/
#include "smpl_core_encoder.h"
#include <stdio.h>

opus_int smpl_control_lbrr(smpl_EncControlStruct* encControl)
{
    // Simple version
    if (encControl->useInBandFEC && (encControl->packetLossPercentage >= 1)) {
        // Fec/Total rate split: 2% 1/4 FEC  -> 20% 1/2 FEC
        // Min Main bitrate    : 2% 12000 bps -> 20% 4500 bps
        float ratio = (encControl->packetLossPercentage - 2.0f) / (20.0f - 2.0f);
        ratio = SMPL_max(SMPL_min(ratio, 1.0f), 0.0f);
        float split = 0.25f + ratio * (0.5f - 0.25f);
        int minMainBitRate = (int)(12000.0f + ratio * (4500.0f - 12000.0f));
#define MIN_FEC_RATE 4500
        encControl->fecBitRate = SMPL_max((int)roundf(encControl->bitRate * split), MIN_FEC_RATE);
        encControl->mainBitRate = encControl->bitRate - encControl->fecBitRate;
        if (encControl->mainBitRate < minMainBitRate) {
            encControl->mainBitRate = minMainBitRate;
            encControl->fecBitRate = encControl->bitRate - encControl->mainBitRate;
        }
        if (encControl->fecBitRate < MIN_FEC_RATE) {
            encControl->fecBitRate = 0;
            encControl->mainBitRate = encControl->bitRate;
        }
        if ((encControl->mainBitRate - encControl->fecBitRate) <= 1000) {
            encControl->fecBitRate = encControl->bitRate / 2; 
            encControl->mainBitRate = encControl->bitRate - encControl->fecBitRate;
        }
    }
    else {
        encControl->mainBitRate = encControl->bitRate;
        encControl->fecBitRate = 0;
    }
    encControl->mainBitRate = SMPL_max(SMPL_min(encControl->mainBitRate, SMPL_MAX_RATE_BPS), SMPL_MIN_RATE_BPS);
    encControl->fecBitRate = SMPL_max(SMPL_min(encControl->fecBitRate, SMPL_MAX_RATE_BPS), 0);
    return 0;
}

typedef enum SmplChannelType_ {
    SMPL_CHANNEL_MONO = 0,
    SMPL_CHANNEL_LEFT = 1,
    SMPL_CHANNEL_RIGHT = 2
} SmplChannelType;

static void smooth_stereo_to_mono(const opus_int16* stereo_samples, opus_int16* mono_channel_samples, int mono_channel_nSamples, SmplChannelType channel) {
    float left_channel_frac = (channel == SMPL_CHANNEL_LEFT) ? (-0.5f / mono_channel_nSamples) : (0.5f / mono_channel_nSamples);
    float left_channel_contr = (channel == SMPL_CHANNEL_LEFT) ? 1.0f : 0.0f;
    for (int i = 0; i < mono_channel_nSamples; i++) {
        mono_channel_samples[i] = (opus_int16)((stereo_samples[i * 2] * left_channel_contr) + (stereo_samples[i * 2 + 1] * (1.0f - left_channel_contr)));
        left_channel_contr += left_channel_frac;
    }
}

static void smooth_mono_to_stereo(const opus_int16* stereo_samples, opus_int16* mono_channel_samples, int mono_channel_nSamples, SmplChannelType channel) {
    float left_channel_frac = (channel == SMPL_CHANNEL_LEFT) ? (0.5f / mono_channel_nSamples) : (-0.5f / mono_channel_nSamples);
    float left_channel_contr = 0.5f;
    for (int i = 0; i < mono_channel_nSamples; i++) {
        mono_channel_samples[i] = (opus_int16)((stereo_samples[i * 2] * left_channel_contr) + (stereo_samples[i * 2 + 1] * (1.0f - left_channel_contr)));
        left_channel_contr += left_channel_frac;
    }
}

static void stereo_usage_decision(smpl_encoder* enc, smpl_EncControlStruct* encControl, int *do_smooth_stereo_to_mono, int *do_smooth_mono_to_stereo) {
    // Switch from stereo to mono if bitrate is too low to encode stereo
    if ((encControl->nChannelsInternal == 2 && encControl->bitRate < SMPL_STEREO_BITRATE_THR)) {
        // Apply smooth downmixing if needed and bitrate allows it
        if ((enc->prev_nChannelsInternal == 1) || (enc->done_smooth_stereo_to_mono == 1) || (encControl->bitRate <= SMPL_STEREO_SMOOTHING_BITRATE_THR)) {
            encControl->nChannelsInternal = 1;
        }
        else {
            encControl->nChannelsInternal = 2;
            *do_smooth_stereo_to_mono = SMPL_TRUE;
        }
    }
    // Switch from mono to stereo if bitrate is high enough
    if ((encControl->nChannelsInternal == 2) && (enc->prev_nChannelsInternal == 1)) {
        if (encControl->bitRate > SMPL_SWITCH_TO_STEREO_BITRATE_THR) {
            *do_smooth_mono_to_stereo = SMPL_TRUE;
        }
        else {
            encControl->nChannelsInternal = 1;
        }
    }
}

opus_int smpl_Encode(                                   /* O    Returns error code                              */
    void                            *encState,          /* I/O  State                                           */
    smpl_EncControlStruct           *encControl,        /* I    Control status                                  */
    const opus_int16                *samplesIn,         /* I    Speech sample input vector                      */
    opus_int                        nSamplesIn,         /* I    Number of samples (per channel) in input vector */
    ec_enc                          *psRangeEnc,        /* I/O  Compressor data structure                       */
    unsigned char                   *toc_byte,          /* O    TOC byte to be put first in payload             */
    opus_int32                      *nBytesOut,         /* I/O  Number of bytes in payload TOC + range encoded (input: Max bytes)   */
    const opus_int                  prefillFlag,        /* I    Flag to indicate prefilling buffers no coding   */
    opus_int                        activity            /* I    Decision of Opus voice activity detector        */
)
{
    if (smpl_Created() == SMPL_FALSE) {
        return SMPL_ENC_NO_GLOBAL_DATA;
    }
    smpl_encoder *enc = (smpl_encoder*)encState;
    int ret = SMPL_NO_ERROR;
    int frames_per_packet, framelen, framelen_idx, low_rate_thr, lowRate;
    opus_int16 samplesBuf[120 * 48];
    //smpl_vad_status vad;
    smpl_TOC toc;
    int do_smooth_stereo_to_mono = SMPL_FALSE;
    int do_smooth_mono_to_stereo = SMPL_FALSE;
    // SMPL always uses the fs from API unless it is limited through OPUS_SET_BANDWIDTH in API. The overhead is small for SWB/FB
    encControl->internalSampleRate = SMPL_min(encControl->maxInternalSampleRate, encControl->API_sampleRate) <= 16000 ? 16000 : 32000;
    if ((encControl->payloadSize_ms != 10) && (encControl->payloadSize_ms != 20) &&
        (encControl->payloadSize_ms != 60) && (encControl->payloadSize_ms != 120)) {
        return SMPL_ENC_PACKET_SIZE_NOT_SUPPORTED;
    }
    if ((encControl->nChannelsAPI < 1) || (encControl->nChannelsAPI > 2) || (encControl->nChannelsInternal > encControl->nChannelsAPI)) {
        return SMPL_ENC_INVALID_NUMBER_OF_CHANNELS_ERROR;
    }
    if (((encControl->API_sampleRate * encControl->payloadSize_ms) / 1000) != nSamplesIn) {
        return SMPL_ENC_INPUT_INVALID_NO_OF_SAMPLES;
    }
    frames_per_packet = (encControl->payloadSize_ms == 10) ? 1 : encControl->payloadSize_ms / 20;
    framelen = nSamplesIn / frames_per_packet;
    framelen_idx = (encControl->payloadSize_ms == 10) ? 0 : (encControl->payloadSize_ms == 20) ? 1 : (encControl->payloadSize_ms == 60) ? 2 : 3;
    low_rate_thr = (int)smpl_low_rate_thr[encControl->internalSampleRate == 16000 ? 0 : 1][framelen_idx];

    stereo_usage_decision(enc, encControl, &do_smooth_stereo_to_mono, &do_smooth_mono_to_stereo);

    if (encControl->nChannelsInternal == 1) {

        // Run SMPL VAD decision on main channel
        const opus_int16 *samplesChannel;

        if (encControl->nChannelsAPI == 2) {
            for (int i = 0; i < nSamplesIn; i++)
                samplesBuf[i] = (samplesIn[i * 2] + samplesIn[i * 2 + 1] + 1) / 2;
            samplesChannel = samplesBuf;
        }
        else {
            samplesChannel = samplesIn;
        }
        update_vad_dtx_status(enc, encControl, &enc->vad, encControl->API_sampleRate, samplesChannel, nSamplesIn, framelen, frames_per_packet, activity);

        smpl_control_lbrr(encControl);
        lowRate = encControl->mainBitRate <= low_rate_thr;

        ret = smpl_core_encode(&(enc->smpl_core_encoder_state[0]), encControl, psRangeEnc, samplesChannel, nSamplesIn,
                                    &enc->vad, &enc->dtx_state, lowRate);

        if (psRangeEnc != NULL) {
            if (!enc->dtx_state.sid_frame || enc->dtx_state.send_sid_frame) {
                // Build TOC
                toc.fs_Hz = encControl->internalSampleRate;
                toc.packet_len_ms = encControl->payloadSize_ms;
                toc.VAD = enc->vad.VAD;
                toc.coded_as_active_voice = enc->vad.coded_as_active_voice;
                toc.SID = enc->dtx_state.sid_frame;
                toc.low_rate = lowRate;
                toc.FEC = encControl->LBRR_coded;
                toc.stereo = SMPL_FALSE;
                *toc_byte = smpl_encode_toc(&toc);
                *nBytesOut = (ec_tell(psRangeEnc) + 7) >> 3;
                *nBytesOut += 1; // TOC byte
            } else {
                smpl_assert((ec_tell(psRangeEnc) + 7) >> 3 <= 1);
                *nBytesOut = 0;
            }
        } else {
            *nBytesOut = 0;
        }
    } else {
        // No DTX or LBR for stereo use scenario
        framelen_idx = (encControl->payloadSize_ms == 10) ? 0 : (encControl->payloadSize_ms == 20) ? 1 : (encControl->payloadSize_ms == 60) ? 2 : 3;
        low_rate_thr = (int)smpl_low_rate_thr[encControl->internalSampleRate == 16000 ? 0 : 1][framelen_idx];
        opus_int16* samplesChannel;

        // Divide bitrate on the two channels
        int channel_bitrate = encControl->bitRate / 2;

        // Initialize side channel state if switching to stereo
        if (enc->prev_nChannelsInternal == 1) {
            memcpy(&(enc->smpl_core_encoder_state[1]), &(enc->smpl_core_encoder_state[0]), sizeof(smpl_core_encoder));
        }

        // VAD/DTX on mono channel
        for (int i = 0; i < nSamplesIn; i++)
            samplesBuf[i] = (samplesIn[i * 2] + samplesIn[i * 2 + 1] + 1) / 2;
        samplesChannel = samplesBuf;

        update_vad_dtx_status(enc, encControl, &enc->vad, encControl->API_sampleRate, samplesChannel, nSamplesIn, framelen, frames_per_packet, activity);

        enc->dtx_state.sid_frame = 0;
        enc->dtx_state.remaining_dtx_hangover = enc->dtx_state.hangover_ms;

        // Left channel
        if (do_smooth_stereo_to_mono) {
            smooth_stereo_to_mono(samplesIn, samplesBuf, nSamplesIn, SMPL_CHANNEL_LEFT);
        } else if (do_smooth_mono_to_stereo) {
            smooth_mono_to_stereo(samplesIn, samplesBuf, nSamplesIn, SMPL_CHANNEL_LEFT);
            enc->done_smooth_stereo_to_mono = 0;
        } else {
            enc->done_smooth_stereo_to_mono = 0;
            for (int i = 0; i < nSamplesIn; i++)
                samplesBuf[i] = samplesIn[i * 2];
        }
        samplesChannel = samplesBuf;

        encControl->bitRate = channel_bitrate;
        smpl_control_lbrr(encControl);
        lowRate = encControl->mainBitRate <= low_rate_thr;
        ret = smpl_core_encode(&(enc->smpl_core_encoder_state[0]), encControl, psRangeEnc, samplesChannel, nSamplesIn,
                    &enc->vad, &enc->dtx_state, lowRate);

        // Right channel
        if (do_smooth_stereo_to_mono) {
            smooth_stereo_to_mono(samplesIn, samplesBuf, nSamplesIn, SMPL_CHANNEL_RIGHT);
            enc->done_smooth_stereo_to_mono = 1;
        } else if (do_smooth_mono_to_stereo) {
            smooth_mono_to_stereo(samplesIn, samplesBuf, nSamplesIn, SMPL_CHANNEL_RIGHT);
        } else {
            for (int i = 0; i < nSamplesIn; i++)
                samplesBuf[i] = samplesIn[(i * 2) + 1];
        }
        samplesChannel = samplesBuf;

        ret = smpl_core_encode(&(enc->smpl_core_encoder_state[1]), encControl, psRangeEnc, samplesChannel, nSamplesIn,
                    &enc->vad, &enc->dtx_state, lowRate);

        if (psRangeEnc != NULL) {
            // Build TOC
            toc.FEC = SMPL_FALSE;
            toc.fs_Hz = encControl->internalSampleRate;
            toc.packet_len_ms = encControl->payloadSize_ms;
            toc.VAD = enc->vad.VAD;
            toc.coded_as_active_voice = enc->vad.coded_as_active_voice;
            toc.SID = enc->dtx_state.sid_frame;
            toc.low_rate = lowRate;
            toc.FEC = encControl->LBRR_coded;
            toc.stereo = SMPL_TRUE;
            *toc_byte = smpl_encode_toc(&toc);
            *nBytesOut = (ec_tell(psRangeEnc) + 7) >> 3;
            *nBytesOut += 1;
        } else {
            *nBytesOut = 0;
        }

        // Towards outside show the combination of the two channels bitrate
        encControl->bitRate *= 2;
        encControl->mainBitRate *= 2;
        encControl->fecBitRate *= 2;
    }
    enc->prev_nChannelsInternal = encControl->nChannelsInternal;

    return ret;
}

opus_int smpl_Encode_secondary(                                   /* O    Returns error code                              */
    void* encState,               /* I/O  State                                           */
    smpl_EncControlStruct* encControl,        /* I    Control status                                  */
    ec_enc* psRangeEnc,           /* I/O  Compressor data structure                       */
    unsigned char* toc_byte,      /* O    TOC byte to be put first in payload             */
    opus_int32* nBytesOut         /* I/O  Number of bytes in payload TOC + range encoded (input: Max bytes)   */
)
{
    smpl_encoder* enc = (smpl_encoder*)encState;
    int ret = SMPL_NO_ERROR;
    if (encControl->nChannelsAPI > 1 || encControl->bitRateSecondary == 0) {
        *nBytesOut = 0;
        return ret;
    }
    smpl_TOC toc;
    smpl_EncControlStruct encControlSecond;
    memcpy(&encControlSecond, encControl, sizeof(smpl_EncControlStruct));
    encControlSecond.mainBitRate = encControl->bitRateSecondary;
    encControlSecond.complexity = encControl->secondary_complexity;
    encControlSecond.fecBitRate = 0;

    int framelen_idx = (encControl->payloadSize_ms == 10) ? 0 : (encControl->payloadSize_ms == 20) ? 1 : (encControl->payloadSize_ms == 60) ? 2 : 3;
    int low_rate_thr = (int)smpl_low_rate_thr[encControl->internalSampleRate == 16000 ? 0 : 1][framelen_idx];
    int lowRate = encControlSecond.mainBitRate <= low_rate_thr;

    ret = smpl_core_encode_second(&(enc->smpl_core_encoder_state[0]), &encControlSecond, psRangeEnc, &enc->vad, &enc->dtx_state, lowRate);
    smpl_assert(encControl->LBRR_coded == SMPL_FALSE);
    if (psRangeEnc != NULL) {
        if (!enc->dtx_state.sid_frame || enc->dtx_state.send_sid_frame) {
            // Build TOC
            toc.fs_Hz = encControlSecond.internalSampleRate;
            toc.packet_len_ms = encControlSecond.payloadSize_ms;
            toc.VAD = enc->vad.VAD;
            toc.coded_as_active_voice = enc->vad.coded_as_active_voice;
            toc.SID = enc->dtx_state.sid_frame;
            toc.low_rate = lowRate;
            toc.FEC = encControlSecond.LBRR_coded;
            toc.stereo = SMPL_FALSE;
            *toc_byte = smpl_encode_toc(&toc);
            *nBytesOut = (ec_tell(psRangeEnc) + 7) >> 3;
            *nBytesOut += 1; // TOC byte
        }
        else {
            smpl_assert((ec_tell(psRangeEnc) + 7) >> 3 <= 1);
            *nBytesOut = 0;
        }
    }
    else {
        *nBytesOut = 0;
    }

    return ret;
}

void smpl_open_enc_files(void* encState)          /* I/O  State                                           */
{
#if SMPL_DUMP_FEATURES
    smpl_encoder* enc = (smpl_encoder*)encState;

    enc->smpl_core_encoder_state[0].align_samples = 32; // Compensate with delay
    if (enc->smpl_core_encoder_state[0].fp_clean) {
        fclose(enc->smpl_core_encoder_state[0].fp_clean);
    }
    enc->smpl_core_encoder_state[0].fp_clean = fopen("clean.s16", "wb");
    if (enc->smpl_core_encoder_state[0].fp_clean_hp) {
        fclose(enc->smpl_core_encoder_state[0].fp_clean_hp);
    }
    enc->smpl_core_encoder_state[0].fp_clean_hp = fopen("clean_hp.s16", "wb");
    if (enc->smpl_core_encoder_state[0].fp_features_lsf) {
        fclose(enc->smpl_core_encoder_state[0].fp_features_lsf);
    }
    enc->smpl_core_encoder_state[0].fp_features_lsf = fopen("features_enc_lsf.f32", "wb");
    if (enc->smpl_core_encoder_state[0].fp_reslpc) {
        fclose(enc->smpl_core_encoder_state[0].fp_reslpc);
    }
    enc->smpl_core_encoder_state[0].fp_reslpc = fopen("reslpc.f32", "wb");    
    if (enc->smpl_core_encoder_state[0].fp_clean_hb) {
        fclose(enc->smpl_core_encoder_state[0].fp_clean_hb);
    }
    enc->smpl_core_encoder_state[0].fp_clean_hb = fopen("clean_hb.s16", "wb");
#endif
}

void smpl_close_enc_files(void* encState)          /* I/O  State                                           */
{
#if SMPL_DUMP_FEATURES
    smpl_encoder* enc = (smpl_encoder*)encState;

    if (enc->smpl_core_encoder_state[0].fp_clean) {
        fclose(enc->smpl_core_encoder_state[0].fp_clean);
    }
    if (enc->smpl_core_encoder_state[0].fp_clean_hp) {
        fclose(enc->smpl_core_encoder_state[0].fp_clean_hp);
    }
    if (enc->smpl_core_encoder_state[0].fp_features_lsf) {
        fclose(enc->smpl_core_encoder_state[0].fp_features_lsf);
    }
    if (enc->smpl_core_encoder_state[0].fp_reslpc) {
        fclose(enc->smpl_core_encoder_state[0].fp_reslpc);
    }
    if (enc->smpl_core_encoder_state[0].fp_clean_hb) {
        fclose(enc->smpl_core_encoder_state[0].fp_clean_hb);
    }
#endif
}

#endif // #if defined(ENABLE_SMPL)
