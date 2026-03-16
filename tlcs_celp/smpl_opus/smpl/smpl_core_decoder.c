#include "smpl_core_decoder.h"
#include "smpl_codec_util.h"
#include "smpl_filt.h"
#include "smpl_postfilter.h"
#include "smpl_lpc.h"
#include "smpl_lsf_quant.h"
#include "smpl_helpers.h"
#include "smpl_param_coding.h"
#include "smpl_quant_nrg_res.h"
#include "smpl_tables.h"
#include "smpl_typedef.h"
#include "smpl_celp.h"
#include "smpl_bandwidth_extension.h"
#include "smpl_filt_allpass_fb.h"
#include "smpl_flpexcpt_check.h"
#include "smpl_plc.h"
#include "silk/debug.h"
#include "silk/SigProc_FIX.h"

void smpl_core_decode_init(
    smpl_core_decoder* dec_state
)
{
    memset(dec_state, 0, sizeof(smpl_core_decoder));
    smpl_init_harm_postfilter(&dec_state->harm_postfilter);
    smpl_hp_postfilter_init(&dec_state->hp_postfilter);
    smpl_plc_init(&dec_state->plc);
}

static inline uint32_t skip_bits(
    smpl_core_decoder* dec_state,
    ec_dec* psRangeDec,                        /* I/O  Compressor data structure                       */
    int coded_as_active_voice,
    const smpl_TOC* toc
)
{
    const int frame_length_16 = (1 + (toc->packet_len_ms > 10)) * (10 * SMPL_CELP_FS_KHZ);
    const int num_frames      = (toc->packet_len_ms + 10) / 20;
    const int num_subframes   = 1 << (1 - toc->low_rate + (toc->packet_len_ms > 10));
    int res = SMPL_NO_ERROR;
    int cond_coding = SMPL_FALSE;
    int nBitsPre = ec_tell(psRangeDec);
    for (int frame = 0; frame < num_frames; frame++) {
        LbQuantParams lb_params;
        memset(&lb_params, 0, sizeof(LbQuantParams));
        res = smpl_decode_lb_params((void*)&dec_state->param_decoder, (void*)psRangeDec, frame_length_16, 
            num_subframes, SMPL_TRUE, &cond_coding, toc->low_rate, frame, SMPL_FALSE, &lb_params);
        if (toc->fs_Hz > 16000) {
            HbQuantParams hb_params;
            memset(&hb_params, 0, sizeof(HbQuantParams));
            res += smpl_decode_hb_params((void*)&dec_state->param_decoder, (void*)psRangeDec, frame_length_16, lb_params.voiced, cond_coding, toc->low_rate, &hb_params);
        }
        cond_coding = SMPL_TRUE;
    }
    uint32_t nBytes = (uint32_t)((ec_tell(psRangeDec) - nBitsPre) + 7) / 8;
    return nBytes;
}

static void smpl_up_32_48_chunked(const float* yBuf, int yBuf_len, float* up_32_48_state, int state_len, opus_int16* samplesOut) {
    // chunk in 10ms in case we use 10ms frames
    smpl_assert(state_len == SMPL_FIR_N_32_48);
    smpl_assert(yBuf_len % SMPL_FRAME_LEN == 0);
    int num_chunks = yBuf_len / SMPL_FRAME_LEN;
    for (int i = 0; i < num_chunks; i++) {
        float yResampled[3 * SMPL_FRAME_LEN / 2];
        smpl_up_32_48(yBuf, SMPL_FRAME_LEN, up_32_48_state, SMPL_FIR_N_32_48, yResampled);
        smpl_float_to_int16(yResampled, samplesOut, 3 * SMPL_FRAME_LEN / 2);
        yBuf       += SMPL_FRAME_LEN;
        samplesOut += 3 * SMPL_FRAME_LEN / 2;
    }
}

static void silk_resampler_chunked(const float* yBuf, int yBuf_len, silk_resampler_state_struct* resampler_internal_to_api, opus_int16* samplesOut, int samplesOut_len) {
    // chunk in 10ms in case we use 10ms frames
    smpl_assert(yBuf_len % (SMPL_FRAME_LEN / 2) == 0);
    int num_chunks = yBuf_len / (SMPL_FRAME_LEN / 2);
    int out_chunk_size = samplesOut_len / num_chunks;
    for (int i = 0; i < num_chunks; i++) {
        int16_t yBuf_16b[SMPL_FRAME_LEN / 2];
        smpl_float_to_int16(yBuf, yBuf_16b, SMPL_FRAME_LEN / 2);
        silk_resampler(resampler_internal_to_api, samplesOut, yBuf_16b, SMPL_FRAME_LEN / 2);
        yBuf       += SMPL_FRAME_LEN / 2;
        samplesOut += out_chunk_size;
    }
}

static int get_LPC_postfilter_enabled(int fs_Hz, SmplLpcPostFilterMode mode)
{
    if (fs_Hz == 16000) {
        return ((mode == SMPL_LPC_PSTF_MODE_WB_ON_SWB_ON) || (mode == SMPL_LPC_PSTF_MODE_WB_ON_SWB_OFF));
    }
    else {
        return ((mode == SMPL_LPC_PSTF_MODE_WB_ON_SWB_ON) || (mode == SMPL_LPC_PSTF_MODE_WB_OFF_SWB_ON));
    }
}

int smpl_core_decode(
    smpl_core_decoder *dec_state,
    smpl_DecControlStruct *decControl, /* I/O  Control Structure                               */
    smpl_TOC *toc,                     /* I/O  Table of Contents                               */
    const opus_int lostFlag,           /* I    0: no loss, 1 loss, 2 decode fec                */
    opus_int newPacketFlag,            /* I    Indicates first decoder call for this packet    */
    ec_dec *psRangeDec,                /* I/O  Compressor data structure                       */
    opus_int16 *samplesOut,            /* O    Decoded output speech vector                    */
    opus_int32 *nSamplesOut            /* O    Number of samples decoded                       */
)
{
    CLEAR_FLP_CHECK();

    int res = 0;
    int buffer_len_ms = (*nSamplesOut * 1000) / decControl->API_sampleRate;
    int updLostFlag = lostFlag;
    uint32_t fec_bytes = 0;
    if (updLostFlag == SMPL_FLAG_DECODE_LBRR) {
        smpl_assert(toc->FEC);
        updLostFlag = SMPL_FLAG_DECODE_NORMAL;
    }
    else if (updLostFlag == SMPL_FLAG_DECODE_NORMAL && toc->FEC) {
        // Strip fec bits to get to main data
        fec_bytes = skip_bits(dec_state, psRangeDec, SMPL_TRUE, toc);
    }
    if (updLostFlag == SMPL_FLAG_PACKET_LOST){
        // In case of Lost packet the caller might ask for a different duration than last packet size
        // This is indicated in *nSamplesOut
        if (buffer_len_ms < 10) { // Can happen if we get a corrupt CELT packet
            memset(samplesOut, 0, *nSamplesOut * sizeof(opus_int16));
            return 0;
        }
        toc->packet_len_ms = buffer_len_ms;
        smpl_assert(toc->packet_len_ms <= SMPL_MAX_FRAMES_PER_PACKET * 20);
    }
    if (buffer_len_ms < toc->packet_len_ms) {
        smpl_assert(0);
        return -1; // Caller has not allocated enough memory to decode this packet
    }
    if (updLostFlag == SMPL_FLAG_DECODE_NORMAL) {
        smpl_plc_reset(&dec_state->plc, toc);
    }
    if (newPacketFlag == SMPL_FALSE) {
        smpl_assert(0);
        return 0;
    }

    const int LPC_postfilter_enabled = get_LPC_postfilter_enabled(toc->fs_Hz, decControl->LPC_postfilter_mode);

    const int frame_length_16    = (1 + (toc->packet_len_ms > 10)) * (10 * SMPL_CELP_FS_KHZ);
    const int subframe_length_16 = (1 + toc->low_rate) * (5 * SMPL_CELP_FS_KHZ);
    const int num_frames         = (toc->packet_len_ms + 10) / 20;
    const int num_subframes      = 1 << (1 - toc->low_rate + (toc->packet_len_ms > 10));
    const int num_hb_subframes   = frame_length_16 / SMPL_HB_SF_LEN;
    const int packetlen_16       = frame_length_16 * num_frames;
    int cond_coding = SMPL_FALSE;
    const float* p_lsf_interpol;
    const float* p_lsf_dtx_interpol; 
    if (num_subframes == 4) {
        p_lsf_interpol = &smpl_lsf_interpol_4[0][0];
        p_lsf_dtx_interpol = &smpl_lsf_interpol_dtx_4[0];
    } 
    else if (num_subframes == 2) {
        p_lsf_interpol = &smpl_lsf_interpol_2[0][0];
        p_lsf_dtx_interpol = &smpl_lsf_interpol_dtx_2[0];
    } 
    else {
        p_lsf_interpol = &smpl_lsf_interpol_1;
        p_lsf_dtx_interpol = &smpl_lsf_interpol_dtx_1;
    }
    *nSamplesOut = 0;
    float yBuf_[SMPL_MAX_FB_PACKET_SIZE_IN_SAMPLES + SMPL_TOT_POSTFILT_DELAY + SMPL_LPC_POST_IMPZ_LEN];
    float *yBuf = yBuf_ + SMPL_LPC_POST_IMPZ_LEN;
    float *y = yBuf;
    float y_hb[SMPL_FRAME_LEN * SMPL_MAX_FRAMES_PER_PACKET + SMPL_TOT_POSTFILT_DELAY];
    float lags[SMPL_PITCH_NUM_SUBFRAMES * SMPL_MAX_FRAMES_PER_PACKET];
    const int lags_per_subframe = subframe_length_16 / SMPL_LAG_SUBFRLEN;
    const int lags_per_frame = frame_length_16 / SMPL_LAG_SUBFRLEN;
    float avg_normalized_bitrate = 0.0f;
    LbQuantParams lb_params;
    memset(&lb_params, 0, sizeof(LbQuantParams));
    for (int frame = 0; frame < num_frames; frame++) {
        // Decode CELP parameters
        float A[SMPL_MAX_N_SUBFR][SMPL_LPC_ORDER + 1];
        float lsfs[SMPL_MAX_N_SUBFR][SMPL_LPC_ORDER];
        float lsfq[SMPL_LPC_ORDER];
        float acb_gains[SMPL_MAX_N_SUBFR][SMPL_ACBG_M];
        // Normal decoding of parameters for regular packets and first frame for SID frames
        int bits_used = 0;
        if (updLostFlag == SMPL_FLAG_DECODE_NORMAL && !(toc->SID == SMPL_TRUE && frame > 0)) {

            bits_used = ec_tell(psRangeDec);

            // Decode LB parameters
            TIC(lb_params)
            int res_param = smpl_decode_lb_params((void*)&dec_state->param_decoder, (void*)psRangeDec, frame_length_16, num_subframes, 
                toc->coded_as_active_voice, &cond_coding, toc->low_rate, frame, toc->SID, &lb_params);
            TOC(lb_params)
            if (res_param) { // payload failed to decode correctly
                smpl_plc_conceal_celp(&dec_state->plc, &lb_params, &acb_gains[0][0], &A[0][0], &lsfs[0][0], num_subframes, subframe_length_16, lags + frame * lags_per_frame);
                smpl_plc_blend_ltp(&dec_state->plc, dec_state->celp_decoder.acb_state, lags[0]);
                smpl_update_recovery_info(&dec_state->plc, &lb_params);
                updLostFlag = SMPL_FLAG_PACKET_LOST;
            }
            else {
                // Dequantize LSFs
                TIC(dequant_lsfs)
                smpl_lsf_dequant(lb_params.lsf_idx, dec_state->lsfq_prev, lb_params.voiced, toc->low_rate, lsfq);
                smpl_assert(!res);
                memcpy(dec_state->lsfq_prev, lsfq, SMPL_LPC_ORDER * sizeof(float));
                TOC(dequant_lsfs)

                // Decode Lags
                for (int i = 0; i < lags_per_frame; i++) {
                    lags[frame * lags_per_frame + i] = lb_params.voiced ? lb_params.laginds[i] * 0.5f + SMPL_MIN_PITCH_LAG : 0.0f;
                }

                // Dequantize ACB gains
                for (int i = 0; i < num_subframes; i++) {
                    acb_dequant(toc->low_rate, lb_params.acbg_idx[i], acb_gains[i]);
                }

                // LSF interpolate
                TIC(lsf_interp)
                smpl_assert(!(num_subframes == 1 && lb_params.lsf_interpol_idx == 1))
                if (frame == 0) {
                    smpl_plc_adapt_lsf(&dec_state->plc, dec_state->lsf_prev, SMPL_LPC_ORDER);
                }
                if (!toc->SID) {
                    smpl_lpc_interpol(lsfq, dec_state->lsf_prev, p_lsf_interpol + (lb_params.lsf_interpol_idx * num_subframes), SMPL_LPC_ORDER, num_subframes, &A[0][0], &lsfs[0][0]);
                }
                else {
                    smpl_lpc_interpol(lsfq, dec_state->lsf_prev, p_lsf_dtx_interpol, SMPL_LPC_ORDER, num_subframes, &A[0][0], &lsfs[0][0]);
                }
                smpl_plc_bwe_recover(&dec_state->plc, &lb_params, &A[0][0], num_subframes, toc->packet_len_ms * frame_length_16 / packetlen_16);
                TOC(lsf_interp)

                TIC(update_celp)
                smpl_plc_update_celp(&dec_state->plc, &lb_params, &acb_gains[0][0], A[num_subframes - 1], lsfs[num_subframes - 1],
                    lags + frame * lags_per_frame, lags_per_frame, num_subframes, subframe_length_16);
                TOC(update_celp)
            }
        } else if (dec_state->plc.toc.SID == SMPL_TRUE) {
            smpl_plc_conceal_celp_dtx(&dec_state->plc, &lb_params, dec_state->lsf_prev, &A[0][0], &lsfs[0][0], num_subframes, lags + frame * lags_per_frame, lags_per_frame);
        } else {
            smpl_plc_conceal_celp(&dec_state->plc, &lb_params, &acb_gains[0][0], &A[0][0], &lsfs[0][0], num_subframes, subframe_length_16, lags + frame * lags_per_frame);
            smpl_plc_blend_ltp(&dec_state->plc, dec_state->celp_decoder.acb_state, lags[0]);
            smpl_update_recovery_info(&dec_state->plc, &lb_params);
        }
        float normalized_bitrate = smpl_get_normalized_bitrate(lb_params.n_pulses, frame_length_16);
        avg_normalized_bitrate += normalized_bitrate;
        smpl_assert(!res);

        // Excitation signal
        float lpc_res[SMPL_FRAME_LEN];
        smpl_gen_excitation(lb_params.fcbg_idx, lb_params.voiced, num_subframes, subframe_length_16, lb_params.nPositions, lb_params.positions, lb_params.pos_pulses, lpc_res);

        float fcb[SMPL_FRAME_LEN];
        memcpy(fcb, lpc_res, num_subframes * subframe_length_16 * sizeof(float));

        TIC(subfr_loop)
        float ytmp[SMPL_LPC_ORDER];
        if (frame > 0) {
            memcpy(ytmp, &y[-SMPL_LPC_ORDER], SMPL_LPC_ORDER * sizeof(float));
        }
        memcpy(&y[-SMPL_LPC_ORDER], dec_state->lpc_synth_mem, SMPL_LPC_ORDER * sizeof(float));
        float nyquist_gain = 1.0f;
        for (int sf = 0; sf < num_subframes; sf++) {
            // CELP decoder
            TIC(celp)
            smpl_celp_decode(&dec_state->celp_decoder, lb_params.voiced, acb_gains[sf], &lags[frame * lags_per_frame + sf * lags_per_subframe], 
                lags_per_subframe, subframe_length_16, toc->low_rate, normalized_bitrate, &lpc_res[sf * subframe_length_16]);
            smpl_assert(!res);
            TOC(celp)

            // Celp noise
            TIC(resnrg)
            float nrgres = smpl_decode_resnrg(lb_params.nrgres_dbq_Q14[sf], subframe_length_16);
            if (toc->SID == SMPL_TRUE || (updLostFlag == SMPL_FLAG_PACKET_LOST && dec_state->plc.toc.SID == SMPL_TRUE)) {
                // Interpolate gain for DTX periods
                nrgres = (SMPL_RESNRG_UPD_FACTOR_DTX * nrgres) + (1.0f - SMPL_RESNRG_UPD_FACTOR_DTX) * dec_state->prev_nrgres;
            }
            if (!lb_params.voiced) {
                dec_state->prev_nrgres = nrgres;
            }
            lb_params.nrgres[sf] = nrgres;
            TOC(resnrg)

            TIC(gen_noise)
            float noise[SMPL_MAX_SF_LEN];
            smpl_celp_gen_noise(&dec_state->celp_decoder.noise_generator, &lpc_res[sf * subframe_length_16], subframe_length_16, lb_params.voiced, lb_params.sf_pulses[sf],
                lb_params.nrgres[sf], lb_params.fcbg_idx[sf], lsfs[sf], normalized_bitrate, noise);

#if SMPL_DUMP_FEATURES
            if (dec_state->fp_exclpc_dec) {
                float exc_interl[SMPL_MAX_SF_LEN * 3];
                for (int i = 0; i < subframe_length_16; i++) {
                    exc_interl[i * 3] = fcb[sf * subframe_length_16 + i];
                    exc_interl[i * 3 + 1] = lpc_res[sf * subframe_length_16 + i] - fcb[sf * subframe_length_16 + i];
                    exc_interl[i * 3 + 2] = noise[i];
                }
                fwrite(exc_interl, sizeof(float), 3 * subframe_length_16, dec_state->fp_exclpc_dec);
            }
#endif
            if (!lb_params.voiced && (lb_params.sf_pulses[sf] > 0) && (smpl_uv_pulse_shaping_coefs[toc->low_rate][0][0] < 1.0f)) {
                smpl_filt_arma1(&lpc_res[sf * subframe_length_16], subframe_length_16, smpl_uv_pulse_shaping_coefs[toc->low_rate][0], 2, 
                                smpl_uv_pulse_shaping_coefs[toc->low_rate][1], 2,
                                dec_state->uv_pulse_shaping_state, 2, &lpc_res[sf * subframe_length_16]);
            } else {
                memset(dec_state->uv_pulse_shaping_state, 0, 2*sizeof(float));
            }

            smpl_add_vec_inplace(noise, &lpc_res[sf * subframe_length_16], subframe_length_16);

            TOC(gen_noise)

#ifdef SMPL_USE_TILT_POSTFILTER
#ifdef SMPL_USE_LPC_POSTFILTER
            if (LPC_postfilter_enabled == SMPL_FALSE)
#endif
            {
                if (toc->low_rate && (toc->fs_Hz == 16000) && (smpl_post_tilt_coefs[lb_params.voiced][0] < 1.0f)) {
                    smpl_filt_ma1(&lpc_res[sf * subframe_length_16], subframe_length_16, smpl_post_tilt_coefs[lb_params.voiced], 2, &dec_state->tilt_postfilter, 1, &y[sf * subframe_length_16]);
                    memcpy(&lpc_res[sf * subframe_length_16], &y[sf * subframe_length_16], subframe_length_16 * sizeof(float));
                    nyquist_gain = smpl_post_tilt_coefs[lb_params.voiced][0] - smpl_post_tilt_coefs[lb_params.voiced][1];
                } else {
                    dec_state->tilt_postfilter = lpc_res[(sf + 1) * subframe_length_16 - 1];
                }
            }
#endif

            smpl_plc_decay_exc(&dec_state->plc, lpc_res + sf * subframe_length_16, subframe_length_16, updLostFlag != SMPL_FLAG_PACKET_LOST || !toc->coded_as_active_voice, lb_params.voiced);
            smpl_plc_update_nrg(&dec_state->plc, smpl_nrg(lpc_res + sf * subframe_length_16, subframe_length_16), subframe_length_16);

            // LPC Synthesize
            smpl_filt_ar16(&lpc_res[sf * subframe_length_16], subframe_length_16, A[sf], &y[sf * subframe_length_16]);
            smpl_assert(!res);
        }
        if (frame > 0) {
            memcpy(&y[-SMPL_LPC_ORDER], ytmp, SMPL_LPC_ORDER * sizeof(float));
        }
        memcpy(dec_state->lpc_synth_mem, &y[frame_length_16 - SMPL_LPC_ORDER], SMPL_LPC_ORDER * sizeof(float));
        TOC(subfr_loop)

        // LPC postfilter
        float *y_pre_postfilter = y;
#ifdef SMPL_USE_LPC_POSTFILTER
        TIC(lpc_postfilter)
        float y_pre_postfilter_[SMPL_FRAME_LEN + SMPL_LB_WGHT_LEN - 1];
        if(LPC_postfilter_enabled) {
            y_pre_postfilter = y_pre_postfilter_ + SMPL_LB_WGHT_LEN - 1;
            memcpy(y_pre_postfilter, y, SMPL_FRAME_LEN * sizeof(float));
            float gain = 0;
            for (int sf = 0; sf < num_subframes; sf++) {
                gain += smpl_lpc_postfilter(&dec_state->lpc_postfilter, &y[subframe_length_16 * sf], subframe_length_16, A[sf], lb_params.voiced, toc->low_rate);
            }
            gain /= num_subframes;
            nyquist_gain *= gain;
        }
        else {
            smpl_lpc_postfilter_state_upd(&dec_state->lpc_postfilter, y, frame_length_16);
        }
        TOC(lpc_postfilter)
#endif

#if !SMPL_DUMP_FEATURES
            // HP postfilter
            TIC(hp_postfilter)
            smpl_hp_postfilter(&dec_state->hp_postfilter, y, frame_length_16, &lags[frame * lags_per_frame], lags_per_frame, y);
        smpl_assert(!res);
        TOC(hp_postfilter)
#else
        float hp_a2[3] = { 1.0f, -1.9808896f, 0.9810795f };
        float hp_b2[3] = { 0.99049276f, -1.9809836f, 0.99049276f };
        smpl_filt_arma2(y, frame_length_16, hp_b2, SMPL_HP_A_LEN, hp_a2, SMPL_HP_A_LEN, dec_state->hp_arma2_state, (SMPL_HP_A_LEN - 1) * 2, y);
#endif

        float hb_exc_gains[SMPL_MAX_HB_SUBFR] = {0};
        float A_hb[SMPL_MAX_N_SUBFR][SMPL_HB_LPC_ORDER + 1] = {0};
        if (toc->fs_Hz > 16000) {
            TIC(HB)
            HbQuantParams hb_params;
            float hb_gains[SMPL_MAX_HB_SUBFR];
            float low_wght_frame_nrg = 0;
            float y_wght[SMPL_FRAME_LEN];
            smpl_hb_wght_lb(dec_state->lb_wght_mem, num_hb_subframes, y_pre_postfilter, y_wght, &low_wght_frame_nrg);
            if (updLostFlag == SMPL_FLAG_DECODE_NORMAL && !(toc->SID == SMPL_TRUE && frame > 0)) {
                res += smpl_decode_hb_params((void*)&dec_state->param_decoder, (void*)psRangeDec, frame_length_16, lb_params.voiced, cond_coding, toc->low_rate, &hb_params);

                // Dequantize LSFs
                float hb_lsfq[SMPL_LPC_ORDER];
                smpl_hb_lsf_dequant(hb_params.lsf_idx, lb_params.voiced, toc->low_rate, hb_lsfq);

                // Interpolate LSFs
                smpl_lpc_interpol(hb_lsfq, dec_state->hb_lsf_prev, p_lsf_interpol + (lb_params.lsf_interpol_idx * num_subframes), SMPL_HB_LPC_ORDER, num_subframes, &A_hb[0][0], NULL);
                for (int num_subfr = 0; num_subfr < num_subframes; num_subfr++) {
                    smpl_bwe_expand(A_hb[num_subfr], SMPL_HB_LPC_ORDER, SMPL_HB_LPC_BWE);
                }

                // Dequantize gains
                smpl_hb_gain_dequant(hb_params.gain_qi, lb_params.voiced, toc->low_rate, num_hb_subframes, hb_gains, y_wght, low_wght_frame_nrg);

                smpl_plc_update_hb(&dec_state->plc, A_hb[num_subframes - 1], hb_gains[num_hb_subframes - 1]);
            } else if (dec_state->plc.toc.SID == SMPL_TRUE) {
                smpl_plc_conceal_hb_dtx(&dec_state->plc, &A_hb[0][0], hb_gains, num_subframes, num_hb_subframes);
            } else {
                smpl_plc_conceal_hb(&dec_state->plc, &A_hb[0][0], hb_gains, num_hb_subframes);
            }

            // Since CELP is not using ACB high boost use lpc_res directly
#if SMPL_DUMP_FEATURES
            int tot_postfilt_delay = 0;
#else
            int tot_postfilt_delay = SMPL_TOT_POSTFILT_DELAY;
#endif
            smpl_hb_decode(&dec_state->hb_decoder, lpc_res, lb_params.voiced, toc->coded_as_active_voice, 
#if defined(SMPL_USE_LPC_POSTFILTER) || defined(SMPL_USE_TILT_POSTFILTER)
                                nyquist_gain,
#endif
                                num_hb_subframes, num_subframes, hb_gains, &A_hb[0][0], frame, num_frames,
                                (y_hb + tot_postfilt_delay) + frame * frame_length_16, hb_exc_gains, toc->low_rate);
            TOC(HB)
        }

        // update CNG model
        if (updLostFlag == SMPL_FLAG_DECODE_NORMAL || toc->SID == SMPL_TRUE) {
            smpl_plc_update_cng(&dec_state->plc, y, &lb_params, y_hb + frame * frame_length_16, toc->fs_Hz > 16000, toc->coded_as_active_voice,
                toc->VAD, num_subframes, subframe_length_16, &lsfs[0][0], &A_hb[0][0], hb_exc_gains);
        } else {
            dec_state->plc.cng.state_emph[0] = y[num_subframes * subframe_length_16 - 1];
        }

        cond_coding = SMPL_TRUE;
        y += frame_length_16;

#if SMPL_DUMP_FEATURES
        // Dump feature for ML enhanmcements
        if (dec_state->fp_features_period) {
            for (int i = 0; i < lags_per_frame; i += 2) {
                int16_t ilag = (int16_t)round(lags[frame * lags_per_frame + i]);
                fwrite(&ilag, sizeof(int16_t), 1, dec_state->fp_features_period);
            }
        }
        if (dec_state->fp_features_lpc && dec_state->fp_features_lsf) {
            int rep = toc->low_rate ? 2 : 1;
            for (int sf = 0; sf < num_subframes; sf++) {
                for (int r = 0; r < rep; r++) {
                    for (int i = 0; i < SMPL_LPC_ORDER; i++) {
                        float tmp = -A[sf][i + 1];
                        fwrite(&tmp, sizeof(float), 1, dec_state->fp_features_lpc);
                    }
                    float tmp_lsf[SMPL_LPC_ORDER];
                    smpl_A2NLSF_16(tmp_lsf, A[sf]);
                    fwrite(tmp_lsf, sizeof(float), SMPL_LPC_ORDER, dec_state->fp_features_lsf);
                }
            }
        }
        if (dec_state->fp_features_gain) {
            int rep = toc->low_rate ? 2 : 1;
            CelpTables* pTbl = (CelpTables*)g_smpl_celp_tables;
            float* gain_tab = lb_params.voiced ? pTbl->fcbgains_v : pTbl->fcbgains_uv;
            for (int sf = 0; sf < num_subframes; sf++) {
                for (int r = 0; r < rep; r++) {
                    float tot_gain = gain_tab[lb_params.fcbg_idx[sf]] + lb_params.nrgres[sf];
                    fwrite(&tot_gain, sizeof(float), 1, dec_state->fp_features_gain);
                }
            }
        }
        if (dec_state->fp_features_ltp) {
            int rep = toc->low_rate ? 2 : 1;
            float ltp[5];
            memset(ltp, 0, sizeof(ltp));
            for (int sf = 0; sf < num_subframes; sf++) {
                if(lb_params.voiced){
                    ltp[1] = acb_gains[sf][1];
                    ltp[2] = acb_gains[sf][0];
                    ltp[3] = acb_gains[sf][1];
                }
                for (int r = 0; r < rep; r++) {
                    fwrite(&ltp, sizeof(float), 5, dec_state->fp_features_ltp);
                }
            }
        }
        if (dec_state->fp_features_num_bits) {
            int32_t tmp = bits_used;
            fwrite(&tmp, sizeof(int32_t), 1, dec_state->fp_features_num_bits);
        }
        if (dec_state->fp_features_num_bits_smooth) {
            dec_state->bits_used_smth += ((float)bits_used - dec_state->bits_used_smth) * 0.1f;
            fwrite(&dec_state->bits_used_smth, sizeof(float), 1, dec_state->fp_features_num_bits_smooth);
        }
        if (dec_state->fp_features_offset) {
            float offset[SMPL_MAX_N_SUBFR];
            memset(offset, 0, sizeof(offset));
            fwrite(offset, sizeof(float), SMPL_MAX_N_SUBFR, dec_state->fp_features_offset);
        }
        if (dec_state->fp_features_packet_losses) {
            int8_t tmp8 = (int8_t)lostFlag;
            fwrite(&tmp8, sizeof(int8_t), 1, dec_state->fp_features_packet_losses);
        }
        if (toc->fs_Hz > 16000) {
            if (dec_state->fp_coded_hb) {
                int16_t yTmp[SMPL_FRAME_LEN];
                for (int i = 0; i < frame_length_16; i++) {
                    yTmp[i] = SMPL_min(SMPL_max(roundf(y_hb[frame * frame_length_16 + i] * 32687.0f), -32768), 32767);
                }
                fwrite(yTmp, sizeof(int16_t), frame_length_16, dec_state->fp_coded_hb);
            }
            if (dec_state->fp_features_hb_lpc) {
                int rep = toc->low_rate ? 2 : 1;
                for (int sf = 0; sf < num_subframes; sf++) {
                    for (int r = 0; r < rep; r++) {
                        for (int i = 0; i < SMPL_HB_LPC_ORDER; i++) {
                            float tmp = -A_hb[sf][i + 1];
                            fwrite(&tmp, sizeof(float), 1, dec_state->fp_features_hb_lpc);
                        }
                    }
                }
            }
            if (dec_state->fp_features_hb_gain) {
                int rep = toc->low_rate ? 2 : 1;
                for (int sf = 0; sf < num_subframes; sf++) {
                    for (int r = 0; r < rep; r++) {
                        fwrite(&hb_exc_gains[sf], sizeof(float), 1, dec_state->fp_features_hb_gain);
                    }
                }
            }
        }
#endif
    }
    if (lostFlag == SMPL_FLAG_DECODE_LBRR) {
        // Update inband FEC metric
        fec_bytes = (uint32_t)(ec_tell(psRangeDec) + 7) / 8;
    }
    // Track inband FEC and main payload sizes
    dec_state->ifec_payload_size = (updLostFlag == SMPL_FLAG_DECODE_NORMAL) ? fec_bytes : 0;
    dec_state->main_payload_size = (updLostFlag == SMPL_FLAG_DECODE_NORMAL) ? (psRangeDec->storage - fec_bytes) : 0;

    if(lostFlag == SMPL_FLAG_DECODE_LBRR && updLostFlag == SMPL_FLAG_DECODE_NORMAL){
        skip_bits(dec_state, psRangeDec, toc->coded_as_active_voice, toc);
    }

#if !SMPL_DUMP_FEATURES
    if (updLostFlag != SMPL_FLAG_PACKET_LOST && decControl->isLastChannel) {
        // Check that payload didnt have any errors
        int payload_err = smpl_check_end_result((void*)psRangeDec);
        if (payload_err) {
            *nSamplesOut = buffer_len_ms * (decControl->API_sampleRate / 1000);
            memset(samplesOut, 0, *nSamplesOut * sizeof(opus_int16));
            return 0;
        }
    }
#endif

#if !SMPL_DUMP_FEATURES
    TIC(harm_postfilter)
    smpl_harm_postfilter(&dec_state->harm_postfilter, yBuf, packetlen_16, lags, lags_per_subframe * num_subframes * num_frames, avg_normalized_bitrate/num_frames);
    smpl_assert(!res);
    TOC(harm_postfilter)
#endif

    smpl_add_comfort_noise(&dec_state->plc, yBuf, packetlen_16, updLostFlag, SMPL_LB);
    if (toc->fs_Hz > 16000) {
        smpl_add_comfort_noise(&dec_state->plc, y_hb, packetlen_16, updLostFlag, SMPL_HB);
    }

    smpl_update_loss_info(&dec_state->plc, updLostFlag, toc->packet_len_ms);


    TIC(output)
    // handle change in internal sampling rate
    int16_t samplesOut_tmp[10 * 48];
    int nSamplesOut_tmp = 0;
    if ((toc->fs_Hz != dec_state->toc_fs_Hz_prev) && (dec_state->toc_fs_Hz_prev > 0)) {
        // resample old state with 10 ms zero input
        int len_16 = 16 * 10;
        float ybuf_tmp[32 * 10];
        if (dec_state->toc_fs_Hz_prev > 16000) {
            float zeros[10 * 16] = {0};
            smpl_filt_allpass_fb_syn(zeros, len_16, zeros, len_16, smpl_filterbankL_coef, SMPL_FILTERBANK_A_LEN,
                smpl_filterbankH_coef, SMPL_FILTERBANK_A_LEN, ybuf_tmp, dec_state->filterbank_syn_state, (SMPL_FILTERBANK_A_LEN - 1) * 4);
        } else {
            memset(ybuf_tmp, 0, (16 * 10) * sizeof(float));
        }

        int packetlen = len_16 * (1 + (dec_state->toc_fs_Hz_prev == 32000));
        if (dec_state->toc_fs_Hz_prev != decControl->API_sampleRate) {
            if (dec_state->toc_fs_Hz_prev == 32000 && decControl->API_sampleRate == 48000) {
                smpl_assert(decControl->API_sampleRate == 48000);
                smpl_up_32_48_chunked(ybuf_tmp, packetlen, dec_state->up_32_48_state, SMPL_FIR_N_32_48, samplesOut_tmp);
            } else { // resample 
                silk_resampler_chunked(ybuf_tmp, packetlen, &dec_state->resampler_internal_to_api, samplesOut_tmp,
                                            decControl->API_sampleRate * packetlen / dec_state->toc_fs_Hz_prev);
                smpl_assert(!res);
            }
            nSamplesOut_tmp = (packetlen * decControl->API_sampleRate) / dec_state->toc_fs_Hz_prev;
        } else {
            nSamplesOut_tmp = packetlen;
            smpl_float_to_int16(ybuf_tmp, samplesOut_tmp, nSamplesOut_tmp);
        }
    }

#if SMPL_DUMP_FEATURES
if (dec_state->fp_noisy) {
    int16_t yTmp[16 * 100];
    for (int i = 0; i < packetlen_16; i++) {
        yTmp[i] = SMPL_min(SMPL_max(round(yBuf[i] * 32767.0f), -32768.0f), 32767.0f);
    }
    fwrite(yTmp, sizeof(int16_t), packetlen_16, dec_state->fp_noisy);
}
#endif

    if (toc->fs_Hz > 16000) {
        // yBuf is both 16 kHz low-band input and 32 kHz output
        smpl_filt_allpass_fb_syn(yBuf, packetlen_16, y_hb, packetlen_16, smpl_filterbankL_coef, SMPL_FILTERBANK_A_LEN, 
            smpl_filterbankH_coef, SMPL_FILTERBANK_A_LEN, yBuf, dec_state->filterbank_syn_state, (SMPL_FILTERBANK_A_LEN - 1) * 4);
    }

    int packetlen = packetlen_16 * (1 + (toc->fs_Hz == 32000));
    if (decControl->internalSampleRate != decControl->API_sampleRate) {        
        if (decControl->internalSampleRate == 32000 && decControl->API_sampleRate == 48000) {
            smpl_assert(decControl->API_sampleRate == 48000);
            smpl_up_32_48_chunked(yBuf, 2 * packetlen_16, dec_state->up_32_48_state, SMPL_FIR_N_32_48, samplesOut);
        } else { // resample 
            if ((dec_state->resampler_internal_to_api.Fs_in_kHz * 1000 != decControl->internalSampleRate) ||
                (dec_state->resampler_internal_to_api.Fs_out_kHz * 1000 != decControl->API_sampleRate)) {
                res += silk_resampler_init(&(dec_state->resampler_internal_to_api), decControl->internalSampleRate, decControl->API_sampleRate, 0);
                smpl_assert(res == SMPL_NO_ERROR);
            }
            // Resample
            silk_resampler_chunked(yBuf, packetlen, &dec_state->resampler_internal_to_api, samplesOut,
                                          decControl->API_sampleRate * packetlen / decControl->internalSampleRate);
            smpl_assert(!res);
        }
        *nSamplesOut = (packetlen * decControl->API_sampleRate) / toc->fs_Hz;
    } else {
        *nSamplesOut = packetlen;
        smpl_float_to_int16(yBuf, samplesOut, *nSamplesOut);
    }

    // add output samples from resampling switch (if any)
    for (int n = 0; n < nSamplesOut_tmp; n++) {
        samplesOut[n] = (int16_t)SMPL_min(SMPL_max(samplesOut[n] + (int)samplesOut_tmp[n], -32768), 32767);
    }
    dec_state->toc_fs_Hz_prev = toc->fs_Hz;
    TOC(output)

    FLP_CHECK();

    return res;
}
