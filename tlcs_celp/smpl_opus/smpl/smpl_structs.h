#ifndef SMPL_STRUCTS_H
#define SMPL_STRUCTS_H

#include "opus_types.h"
#include "opus_defines.h"
#include "smpl_defines.h"
#include "smpl_pitch.h"
#include "pffft.h"
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define SMPL_DELAY_MS               4
#define SMPL_BITS_PER_SAMPLE        7
#define SMPL_LOG_QUANT_UNTIL_BITS   12
#define SMPL_sign(a)                ( ( (a) < 0 )  ?  -1   : ( (a) > 0 ) )

/***********************************************/
/* Structure for controlling encoder operation */
/***********************************************/
typedef struct smpl_EncControlStruct_{
    /* I:   Number of channels; 1/2                                                         */
    opus_int32 nChannelsAPI;

    /* I:   Number of channels; 1/2                                                         */
    opus_int32 nChannelsInternal;

    /* I:   Input signal sampling rate in Hertz; 8000/12000/16000/24000/32000/44100/48000   */
    opus_int32 API_sampleRate;

    /* I:   Maximum internal sampling rate in Hertz; 8000/12000/16000                       */
    opus_int32 maxInternalSampleRate;

    /* I:   Minimum internal sampling rate in Hertz; 8000/12000/16000                       */
    opus_int32 minInternalSampleRate;

    /* I:   Number of samples per packet in milliseconds; 10/20/40/60                       */
    opus_int payloadSize_ms;

    /* I:   Bitrate during active speech in bits/second; internally limited                 */
    opus_int32 bitRate;

    /* I:   Bitrate during active speech in bits/second; internally limited (secondary encoder) */
    opus_int32 bitRateSecondary;

    /* I/O:   Main Bitrate during active speech in bits/second; internally limited          */
    opus_int32 mainBitRate;

    /* I/O:   Fec Bitrate during active speech in bits/second; internally limited           */
    opus_int32 fecBitRate;

    /* I:   Uplink packet loss in percent (0-100)                                           */
    opus_int packetLossPercentage;

    /* I:   Complexity mode; 0 is lowest, 10 is highest complexity                          */
    opus_int complexity;

    /* I:   Secondary encoder Complexity mode; 0 is lowest, 10 is highest complexity        */
    opus_int secondary_complexity;

    /* I:   Flag to enable in-band Forward Error Correction (FEC); 0/1                      */
    opus_int useInBandFEC;

    /* I:   Flag to actually code in-band Forward Error Correction (FEC) in the current packet; 0/1 */
    opus_int LBRR_coded;

    /* I:   Flag to enable discontinuous transmission (DTX); 0/1                            */
    opus_int useDTX;

    /* I:   Flag to use constant bitrate                                                    */
    opus_int useCBR;

    /* I:   Maximum number of bits allowed for the frame                                    */
    opus_int maxBits;

    /* I:   Causes a smooth downmix to mono                                                 */
    opus_int toMono;

    /* I:   Opus encoder is allowing us to switch bandwidth                                 */
    opus_int opusCanSwitch;

    /* I: Make frames as independent as possible (but still use LPC)                        */
    opus_int reducedDependency;

    /* I: -3DB frequency for encoder HP filter (0 disables HP filter)                       */
    opus_int hp_f_corner_Hz;

    /* I: Multiplier for bitrate allocation based on speech activity (range: 0-1) */
    float subFrameImportanceFactor;

    /* I: Flag to enable speech activity based flatness threshold. */
    opus_int useSpActFlatnessThres;

    /* I: Parameter to control VAD noise level update speed (0-100) */
    opus_int32 vad_noise_lvl_update_speed;

    /* I: Parameter to control VAD non binariness of output (0-100) */
    opus_int32 vad_non_binariness;

    /* I: Parameter to control VAD highpass filter sharpness (0-100) */
    opus_int32 vad_highpass_sharpness;

    /* I: Flag to enable FEC rate compensation. Avoids FEC undershooting */
    opus_int useFecRateCompensation;

    /* O:   Internal sampling rate used, in Hertz; 16000/32000                              */
    opus_int32 internalSampleRate;

    /* O:   Previous internal sampling rate used, in Hertz; 16000/32000                     */
    opus_int32 internalSampleRate_prev;

    /* O: Flag that bandwidth switching is allowed (because low voice activity)             */
    opus_int allowBandwidthSwitch;

    /* O:   Stereo width */
    opus_int stereoWidth_Q14;

    /* O:   Tells the Opus encoder we're ready to switch                                    */
    opus_int switchReady;

    /* O: SILK Signal type */
    opus_int signalType;

    /* O: SILK offset (dithering) */
    opus_int offset;
} smpl_EncControlStruct;

/**************************************************************************/
/* Structure for controlling decoder operation and reading decoder status */
/**************************************************************************/
typedef struct smpl_DecControlStruct_{
    /* I:   Number of channels; 1/2                                                         */
    opus_int32 nChannelsAPI;

    /* I:   Number of channels; 1/2                                                         */
    opus_int32 nChannelsInternal;

    /* I:   Output signal sampling rate in Hertz; 8000/16000/24000/32000/44100/48000        */
    opus_int32 API_sampleRate;

    /* I:   Internal sampling rate used, in Hertz; 8000/12000/16000                         */
    opus_int32 internalSampleRate;

    /* I:   Number of samples per packet in milliseconds; 10/20/40/60                       */
    opus_int payloadSize_ms;

    /* I: enable LPC postfilter                                                             */
    SmplLpcPostFilterMode LPC_postfilter_mode;

    /* I: Last channel in packet                                                            */
    opus_int isLastChannel;
} smpl_DecControlStruct;

#include "smpl_tables.h"
#include "resampler_structs.h"

#define SMPL_ENCODER_NUM_CHANNELS 2
#define SMPL_DECODER_NUM_CHANNELS 2

typedef struct FCB {
    float wnrg;
    int n_pulses;
    int pos_new;
    float sign_new;
    uint64_t sgntr;
    int fcb_state_idx;
} FCB;

typedef struct FCBstate {
    int pulse_positions[SMPL_MAX_SF_LEN];
    float pulse_signs[SMPL_MAX_SF_LEN];
    float num[SMPL_MAX_SF_LEN];
    float den[SMPL_MAX_SF_LEN];
} FCBstate;

typedef struct ACBGparams {
    float werr_in;
    float Phi_acb[SMPL_ACBG_M * SMPL_ACBG_M];
    float d_acb_lpc[SMPL_ACBG_M];
    float acb_basis_phi[SMPL_ACBG_M * SMPL_MAX_SF_LEN];
} ACBGparams;

typedef struct CelpScratch {
    FCB fcbs[SMPL_CELP_MAX_NUMSURV];                       // Can be Scratch memory
    int fcbs_size;
    FCB fcb_candidates[SMPL_CELP_MAX_NUMSURV * SMPL_CELP_MAX_NUMSURV]; // Can be Scratch memory
    int fcb_candidates_size;
    uint64_t unique_sgntr[SMPL_CELP_MAX_NUMSURV * SMPL_CELP_MAX_NUMSURV];
    int unique_sgntr_size;
    FCBstate fcb_states[2][SMPL_CELP_MAX_NUMSURV];
    float imp_lpc_[SMPL_MAX_SF_LEN + SMPL_LPC_ORDER];
    float* imp_lpc;
    float Phi[SMPL_MAX_SF_LEN];
    float PhiFlip[2 * SMPL_MAX_SF_LEN];
    int read_idx;
    int write_idx;
    float nrg_wtgt;
    float wnrg[SMPL_CELP_MAX_RATES];
    float res_ltp[SMPL_MAX_SF_LEN];
    float exc_fcb_raw[SMPL_MAX_SF_LEN];
    float exc_fcb[SMPL_MAX_SF_LEN];
    float exc_lpc[SMPL_MAX_SF_LEN];
} CelpScratch;

typedef struct CelpEncoder {
    CelpScratch *scratchMem;
    float state_wght_[SMPL_MAX_SF_LEN + SMPL_LPC_ORDER];
    float* state_wght;
    float state_err_lpc_syn[SMPL_LPC_ORDER];
    float hanning_win[SMPL_MAX_L_RESP];
    uint64_t sgntrs[SMPL_MAX_SF_LEN];
    float acb_state[SMPL_MAX_PITCH_LAG + SMPL_MAX_SF_LEN + SMPL_LTP_INTERPOL_DELAY];
    int acb_state_len;
    int prev_acb_idx[SMPL_CELP_MAX_RATES];
    int prev_fcb_idx[SMPL_CELP_MAX_RATES];
    int subfr_cnt;
    int subfr_per_packet;
    int fcb_subfrlen;
    int perc_resp_len;
    int low_rate;
    int ignore_zir;
    float fcbgain;
    int initialized;
    void (*perc_filt_ma)(const float *x, int N, const float *coef, const int coef_len, float *y);
} CelpEncoder;

typedef struct NoiseGenerator_ {
    float env_smth;
    float env_last;
    float out_state_uv[2];
    float out_state_v[2];
    float corr_smth[SMPL_NOISE_CORR_ORDER + 1];
    float shape_state[SMPL_NOISE_CORR_ORDER];
    int   prev_voiced;
    int   since_unvoiced;
    opus_int32 rand_seed;
} NoiseGenerator;

typedef struct LbQuantParams_ {
    int voiced;
    int16_t fcbg_idx[SMPL_MAX_N_SUBFR];
    int16_t acbg_idx[SMPL_MAX_N_SUBFR];
    int8_t lsf_idx[SMPL_LPC_ORDER + 1];
    int lsf_interpol_idx;
    int blocksegs_ix;
    int laginds[SMPL_PITCH_NUM_SUBFRAMES];
    int nrgres_frame_qi;
    int nrgres_shape_qi;
    float nrgres[SMPL_MAX_N_SUBFR];
    int32_t nrgres_dbq_Q14[SMPL_MAX_N_SUBFR];
    int16_t pulses[SMPL_FRAME_LEN];
    int16_t positions[ SMPL_MAX_N_SUBFR * SMPL_MAX_PULSES_PER_SF];
    int16_t pos_pulses[SMPL_MAX_N_SUBFR * SMPL_MAX_PULSES_PER_SF];
    int nPositions;
    int n_pulses;
    int16_t sf_pulses[SMPL_MAX_N_SUBFR];
} LbQuantParams;

typedef struct HbQuantParams_ {
    int gain_qi;
    int lsf_idx;
} HbQuantParams;

typedef struct smpl_TOC_ {
    int SID;
    int coded_as_active_voice;
    int VAD;
    int low_rate;
    int fs_Hz;
    int packet_len_ms;
    int FEC;
    int stereo;
} smpl_TOC;

typedef struct ParamsEncoder_ {
    int subframe_length;
    int frames_per_packet;
    int subframe_ix;
    int prev_acb_idx;
    int prev_fcb_idx;
    int prev_lagblk;
    int prev_lagidx;
    int prev_nrgres_idx;
    // Stats
} ParamsEncoder;

typedef struct ParamsDecoder_ {
    int subframe_length;
    int frames_per_packet;
    int subframe_ix;
    int prev_acb_idx;
    int prev_fcb_idx;
    int prev_lagblk;
    int prev_lagidx;
    int prev_nrgres_idx;
    int prev_voiced;
    int prev_hb_lpc_ix;
} ParamsDecoder;

typedef struct VUV_Mode_ {
    float voicing_prev;
    float last_lag_prev;
    float nrg_lo_bgn;
    float nrg_hi_bgn;
} VUV_Mode;

typedef struct HbEncoder_ {
    float hb_lpc_buf[448]; // for LPC analysis
    int prev_hb_lpc_ix;
} HbEncoder;

typedef struct BitrateController_ {
    int prev_voiced;
    float rate_cont_wnrg_smth;
    float rate_cont_bitrate_scale[SMPL_CELP_MAX_RATES];
    float bitrate_delta_smth[SMPL_CELP_MAX_RATES];
    float rate_cont_bitrate[SMPL_CELP_MAX_RATES];
    float adjustment_factor[SMPL_CELP_MAX_RATES]; 
} BitrateController;

typedef struct smpl_core_encoder_settings_ {
    int packet_ms;
    int frames_per_packet;
    int frame_ms;
    int framelen;
    int subfrlen;
    int numsubfrs;
    int numsubfrs_hb;
    int lag_sf_per_fcb_sf;
    int lowRate;
} smpl_core_encoder_settings;

typedef struct smpl_core_encoder_ {
    // High level resampler towards API
    silk_resampler_state_struct resampler_api_to_internal;

    // HP Filter state
    float hp_a2[SMPL_HP_A_LEN];
    float hp_b2[SMPL_HP_A_LEN];
    int hp_fcorner_3dB_Hz;
    float hp_arma2_state[(SMPL_HP_A_LEN-1)*2];

    // Filterbank
    float filterbank_buf[2*(SMPL_WINNEXT_WB_LEN)];

    float xhp_packet_buf[SMPL_LPC_BUF_MEM_LEN + SMPL_MAX_WB_PACKET_SIZE_IN_SAMPLES + SMPL_WINNEXT_WB_LONG_LEN + SMPL_WINNEXT_WB_LEN];
    float xhigh_packet_buf[SMPL_MAX_WB_PACKET_SIZE_IN_SAMPLES];

    // LPC
    float lpc_buf_mem[SMPL_LPC_BUF_MEM_LEN + SMPL_WINNEXT_WB_LEN];

    // Perc Weight
    float perc_wght_buf[512+64];

    float perc_wght_buf2[512 + 64];

    // LTP
    float ltp_buf[SMPL_MAX_LTP_BUF_LEN];

    // PITCH
    float perc_corrs_prev[SMPL_PERC_RESP_LEN + SMPL_PERC_EMPH_V_LEN - 1];
    PitchEstimator pitch_state;

    // Voicing decision
    VUV_Mode vuv_mode;

    // LSF
    float prev_lsf[SMPL_BASE_ENCODERS][SMPL_LPC_ORDER];
    float lpc_synth_mem[SMPL_BASE_ENCODERS][SMPL_LPC_ORDER]; // For HB

    float A_buf[SMPL_MAX_FRAMES_PER_PACKET][SMPL_LPC_ORDER+1];
    float perc_corrs_buf[SMPL_MAX_FRAMES_PER_PACKET][MAX_NUM_SUBFR][SMPL_MAX_L_RESP + SMPL_PERC_EMPH_V_LEN - 1];
    float lags_buf[SMPL_MAX_FRAMES_PER_PACKET][SMPL_PITCH_NUM_SUBFRAMES];
    int laginds_buf[SMPL_MAX_FRAMES_PER_PACKET][SMPL_PITCH_NUM_SUBFRAMES];
    int blocksegs_ix_buf[SMPL_MAX_FRAMES_PER_PACKET];
    float voicing_strength_buf[SMPL_MAX_FRAMES_PER_PACKET];
    int voiced_buf[SMPL_MAX_FRAMES_PER_PACKET];
    float wnrgs_buf[SMPL_MAX_FRAMES_PER_PACKET][SMPL_MAX_N_SUBFR];

    // nonflatness
    float nonflatness_state[SMPL_BASE_ENCODERS][SMPL_NON_FLAT_STATE_LEN];

    // Bitrate controller
    BitrateController rateCtrl[SMPL_BASE_ENCODERS];

    // CELP encoding
    CelpEncoder celp_state[SMPL_BASE_ENCODERS];

    // HB encoding
    HbEncoder hb_state[SMPL_BASE_ENCODERS];
    NoiseGenerator noise_generator[SMPL_BASE_ENCODERS];
    float uv_pulse_shaping_state[2];

    // Parameter encoder
    ParamsEncoder parm_encoder_state;

    // LBRR data
    LbQuantParams lbrr_lb_quant_params[SMPL_MAX_FRAMES_PER_PACKET];
    HbQuantParams lbrr_hb_quant_params[SMPL_MAX_FRAMES_PER_PACKET];
    int hasFecData;
    int prevLowRate;
    int prevInternalSampleRate;
    int prev_packet_ms;
    int prev_voiced[SMPL_BASE_ENCODERS];

    int64_t base_encoder_cnt[SMPL_BASE_ENCODERS];

    uint32_t ifec_payload_size;
    uint32_t main_payload_size;

#if SMPL_DUMP_FEATURES
    int align_samples;
    FILE* fp_features_lsf;
    FILE* fp_reslpc;
    FILE* fp_clean_hp;
    FILE* fp_clean;
    FILE* fp_clean_hb;
#endif
} smpl_core_encoder;

typedef struct smpl_vad_status_ {
    SmplVadTypes vad_results_type[SMPL_MAX_FRAMES_PER_PACKET];
    float vad_results[SMPL_MAX_FRAMES_PER_PACKET];
    int VAD;
    int coded_as_active_voice;
    int frames_per_packet;
} smpl_vad_status;

typedef struct smpl_dtx_status_ {
    int sid_frame;
    int send_sid_frame;
    int hangover_ms;
    int remaining_dtx_hangover;
    int sid_interval_ms;
    int dtx_remaining_ms;

    // Old candidates
    int candidate_insert_idx;
    float energy[SMPL_DTX_NO_CANDIDATES];
    int lowRate[SMPL_DTX_NO_CANDIDATES];
    LbQuantParams LbQuantParams[SMPL_DTX_NO_CANDIDATES];
    HbQuantParams HbQuantParams[SMPL_DTX_NO_CANDIDATES];
} smpl_dtx_status;

typedef struct {
    opus_int32 AnaState[2];                         /* Analysis filterbank state: 0-8 kHz                                   */
    opus_int32 AnaState1[2];                        /* Analysis filterbank state: 0-4 kHz                                   */
    opus_int32 AnaState2[2];                        /* Analysis filterbank state: 0-2 kHz                                   */
    opus_int32 XnrgSubfr[SMPL_VAD_N_BANDS];         /* Subframe energies                                                    */
    opus_int32 NrgRatioSmth_Q8[SMPL_VAD_N_BANDS];   /* Smoothed energy level in each band                                   */
    opus_int32 HPstate[1];                          /* State of highpass filter in the lowest band                           */
    opus_int32 NL[SMPL_VAD_N_BANDS];                /* Noise energy level in each band                                      */
    opus_int32 inv_NL[SMPL_VAD_N_BANDS];            /* Inverse noise energy level in each band                              */
    opus_int32 NoiseLevelBias[SMPL_VAD_N_BANDS];    /* Noise level estimator bias/offset                                    */
    opus_int32 counter;                             /* Frame counter used in the initial phase                              */
    opus_int32 noise_lvl_update_speed;
    opus_int32 non_binariness;
    opus_int32 highpass_sharpness;
} smpl_VAD_state;


typedef struct smpl_encoder_ {
    smpl_core_encoder smpl_core_encoder_state[SMPL_ENCODER_NUM_CHANNELS];

    int prev_nChannelsInternal;
    int done_smooth_stereo_to_mono;

    // VAD/DTX
    smpl_VAD_state sVAD;
    int speech_activity_Q8;
    int input_tilt_Q15;
    smpl_dtx_status dtx_state;
    silk_resampler_state_struct resampler_api_to_vad; // Resampler from API to rate supported by VAD

    smpl_vad_status vad;

    PitchEstScratch pitchScratch;
    CelpScratch celpScratch;
} smpl_encoder;

typedef struct LpcPostfilter_ {
#if SMPL_LPC_POSTFILT_FAST
    float state_ma[SMPL_LPC_POST_IMPZ_LEN];    
#else
    float state_ma[SMPL_LPC_ORDER + 1];
#endif
    float state_ar[SMPL_LPC_ORDER];
} LpcPostfilter;

typedef struct HarmPst_ {
    float state1[2 * SMPL_HARM_POSTF_FB_DELAY];
    float LPcoefs[2 * SMPL_HARM_POSTF_FB_DELAY + 1];
    float StateComb[SMPL_MAXPITCH_LEN + SMPL_FRAME_LEN * SMPL_MAX_FRAMES_PER_PACKET + SMPL_HARM_POSTF_DELAY];
    int prev_lag;
    int prev_did_filter;
} HarmPst;

typedef struct HpPst_ {
    float state_lo_emph1[1];
    float state_lo_emph2[1];
    float state_hp[4];
    float lag_old;
    float x_old[SMPL_FRAME_LEN];
    float coef_ma[3];
    float coef_ar[3];
} HpPst;

typedef struct LPCTables_ {
    PFFFT_Setup *pffft_setup;
    double Cdif[SMPL_LPC_ORDER/2][SMPL_LPC_NFFT/4];
    double Csumdiff[SMPL_LPC_ORDER/2][SMPL_LPC_NFFT/4];
    double Csumsum[SMPL_LPC_ORDER/2][SMPL_LPC_NFFT/4];    
} LPCTables;

typedef struct CelpDecoder_ {
    float acb_state[2 * SMPL_MAX_PITCH_LAG + SMPL_MAX_SF_LEN + SMPL_LTP_INTERPOL_DELAY]; // keep two pitch cycles for PLC state blending
    int pulse_pos_ix;
    NoiseGenerator noise_generator;
} CelpDecoder;

typedef struct HbDecoder_ {
    float spec_env_state[SMPL_HB_LPC_ORDER];
    float spec_env_state_temp[SMPL_HB_LPC_ORDER];
    float post_ma_state[SMPL_HB_POST_LEN - 1];
    float env_smth;
    float out_state[SMPL_TOT_POSTFILT_DELAY]; // for delaying HB to sync with LB
    opus_int32 rand_seed;
} HbDecoder;

typedef struct CngModel_ {
    float lsfs_lb[SMPL_LPC_ORDER]; // store as LSFs for interpolation
    float A_hb[SMPL_HB_LPC_ORDER + 1];
    float nrg_bands[2]; // LB / HB
    float frame_nrg; // to decide between candidates
} CngModel;

typedef struct CNG_ {
    float state_lb[SMPL_LPC_ORDER];
    float state_hb[SMPL_HB_LPC_ORDER];

    CngModel cng_candidates[SMPL_CNG_NO_CANDIDATES];
    int cng_candidates_tail;
    int num_candidates;
    int best_candidate_idx;
    
    opus_int32 rand_seed;

    float lb_delay_buf[SMPL_TOT_POSTFILT_DELAY];
    float hb_delay_buf[SMPL_TOT_POSTFILT_DELAY];

    float state_emph[1];
} CNG;

typedef struct PLC_ {
    int loss_count_subfr;
    smpl_TOC toc;
    int is_PLC_frame_prev;
    opus_int32 rand_seed;
    float smth_state;

    // Buffers
    float acb_buf[2 * SMPL_MAX_PITCH_LAG / SMPL_MIN_SF_LEN];
    float exc_nrg_buf[SMPL_MAX_PITCH_LAG / SMPL_MIN_SF_LEN];
    float lag_buf[2 * SMPL_MAX_PITCH_LAG / SMPL_LAG_SUBFRLEN];

    // Information to improve recovery
    int   loss_len_ms;
    float comf_sig_ratio;
    int   recovery_len_ms;
    float A_last[SMPL_LPC_ORDER + 1];
    int   voiced_last;
    float nrg_last;

    // Subset of paramters from LB
    int     voiced;
    int32_t last_nrgres_Q14;
    int16_t last_subfr_pulses;
    int16_t last_fcb_idx;
    float   lsf[SMPL_LPC_ORDER];
    float   A[SMPL_LPC_ORDER + 1];
    float   precise_lag;
    float   exc_attenuation;

    // Parameters from HB
    float hb_gain;
    float hb_gain_attenuation;
    float A_hb[SMPL_HB_LPC_ORDER + 1];

    CNG cng;
} PLC;

typedef struct smpl_core_decoder_ {
    // High level resampler towards API
    silk_resampler_state_struct resampler_internal_to_api;

    float filterbank_syn_state[(SMPL_FILTERBANK_A_LEN-1)*4]; // *2 for AR/MA parts, *2 again for LB/HB *2 x *2 -> *4

    ParamsDecoder param_decoder;

    float lsfq_prev[SMPL_LPC_ORDER]; // Used for conditional lsf quantization
    float lsf_prev[SMPL_LPC_ORDER];  // Used for lsf interpolation

    float hb_lsf_prev[SMPL_HB_LPC_ORDER]; // Used for lsf interpolation (HB)
    float lb_wght_mem[SMPL_LB_WGHT_LEN - 1];
    float uv_pulse_shaping_state[2];

    CelpDecoder celp_decoder;
    HbDecoder hb_decoder;

    float lpc_synth_mem[SMPL_LPC_ORDER];

#ifdef SMPL_USE_LPC_POSTFILTER
    LpcPostfilter lpc_postfilter;
#endif
#ifdef SMPL_USE_TILT_POSTFILTER
    float tilt_postfilter;
#endif
    HpPst hp_postfilter;
    HarmPst harm_postfilter;

    PLC plc;
    float prev_nrgres;

    // Full band parameters
    float filterbank_buf[2 * (SMPL_WINNEXT_WB_LEN)];
    float filterbank_ana_state[(SMPL_FILTERBANK_A_LEN - 1) * 2];
    float up_32_48_state[SMPL_FIR_N_32_48];
    int toc_fs_Hz_prev;
    uint32_t ifec_payload_size;
    uint32_t main_payload_size;


  float hp_arma2_state[(SMPL_HP_A_LEN - 1) * 2];

#if SMPL_DUMP_FEATURES
    FILE* fp_noisy;
    FILE* fp_features_period;
    FILE* fp_features_lpc;
    FILE* fp_features_gain;
    FILE* fp_features_ltp;
    FILE* fp_features_num_bits;
    FILE* fp_features_num_bits_smooth;
    FILE* fp_features_lsf;
    FILE* fp_features_offset;
    FILE* fp_features_packet_losses;
    FILE* fp_exclpc_dec;
    FILE* fp_coded_hb;
    FILE* fp_features_hb_lpc;
    FILE* fp_features_hb_gain;
    float bits_used_smth;
#endif
    int nCnt;
} smpl_core_decoder;

typedef struct smpl_decoder_ {
    smpl_core_decoder smpl_core_decoder_state[SMPL_ENCODER_NUM_CHANNELS];
    int nCnt;
} smpl_decoder;

#ifdef __cplusplus
}
#endif

#endif
