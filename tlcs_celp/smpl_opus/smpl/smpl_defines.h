#ifndef SMPL_DEFINES_H
#define SMPL_DEFINES_H

#define SMPL_max(x, y) (((x)>(y))?(x):(y))
#define SMPL_min(x, y) (((x)<(y))?(x):(y))

#define SMPL_abs(x) ((x) < 0 ? -(x) : (x))

#define SMPL_MAX_RATE_BPS 30000
#define SMPL_MIN_RATE_BPS 3000

#define SMPL_MAX_SF_LEN   (16*10)
#define SMPL_MIN_SF_LEN   (16*5)
#define SMPL_FRAME_LEN    (16*20)
#define SMPL_HB_SF_LEN    ((int)(16*5))
#define SMPL_MAX_N_SUBFR  (SMPL_FRAME_LEN / SMPL_MIN_SF_LEN)
#define SMPL_MAX_HB_SUBFR (SMPL_FRAME_LEN / SMPL_HB_SF_LEN)

#define SMPL_LPC_ORDER    16
#define SMPL_MAX_L_RESP   (32+1)

#define SMPL_ARR_LEN(x)   (sizeof(x) / sizeof(x[0]))

#define SMPL_TRUE         1
#define SMPL_FALSE        0

#define SMPL_PI 3.1415926535897f
#define SMPL_E  2.7182818284590f

#define SMPL_USE_POWF_FAST                 1              // comment out to use powf() instead of smpl_fast_powf()

#define SMPL_USE_SPEC_LSW_WEIGHT           1              // Comment out to always use Laoira lsf weights

#define SMPL_ENC_HP_FCORNER_3DB_HZ         35

#define SMPL_MAX_FRAMES_PER_PACKET         6
#define SMPL_MAX_FB_PACKET_SIZE_IN_SAMPLES (48 * 120)     // 120ms
#define SMPL_MAX_WB_PACKET_SIZE_IN_SAMPLES (16 * 120)     // 120ms
#define SMPL_MAX_WB_FRAME_SIZE_IN_SAMPLES  (16 * 20)      // 20ms
#define SMPL_WINNEXT_WB_LEN                (16 * 2)       // 2ms analysis window look ahead
#define SMPL_WINNEXT_WB_LONG_LEN           (16 * 4)       // 4ms analysis window look ahead when more frames are still coming within the same packet
#define SMPL_WINONES_LPC_WB_LEN            120            // 7.5ms analysis segment length which is scaled by 1
#define SMPL_WINPREV_LPC_WB_LEN            (16 * 4)       // 4ms analysis window before current frame
#define SMPL_LPC_BUF_MEM_LEN               (SMPL_WINPREV_LPC_WB_LEN + SMPL_WINNEXT_WB_LEN + 16)
#define SMPL_LPC_REG                       5e-7f
#define SMPL_LPC_BWE                       0.9999f         // Bandwidth expansion of LPC coefficients
#define SMPL_PERC_REG                      1e-3f
#define SMPL_PERC_MASK_SMTH                0.1158f          // higher -> more smoothing
#define SMPL_PERC_MEL_FC_HZ                320.0f

#define SMPL_LPC_NFFT                      512
#define SMPL_LPC_WIN1_10MS_LEN             120
#define SMPL_LPC_WIN1_20MS_LEN             264
#define SMPL_PERC_WIN1_10MS_LEN            192
#define SMPL_PERC_WIN1_20MS_LEN            352
#define SMPL_WIN3_SHORT_LEN                SMPL_WINNEXT_WB_LEN
#define SMPL_WIN3_LONG_LEN                 SMPL_WINNEXT_WB_LONG_LEN
#define SMPL_PERC_RESP_LEN                 (16 * 2)       // 2ms
#define SMPL_PERC_EMPH_V_LEN               (2)
#define SMPL_WINPREV_PERC_LEN              (16 * 12)      // 12ms analysis window before current frame (8/12/16)

#define SMPL_DEFAULT_HANGOVER_MS           60
#define SMPL_DEFAULT_SID_INTERVAL_MS       400
#define SMPL_SPEECH_ACTIVITY_DTX_THRES     (0.05f)

#define SMPL_PITCH_FS_KHZ                  16
#define SMPL_PITCH_STAGE_1_FS_KHZ          8
#define SMPL_PITCH_COARSE_FS_KHZ           16
#define SMPL_PITCH_DOWNSAMP_STAGES         1 //LOG2(SMPL_PITCH_FS_KHZ/SMPL_PITCH_STAGE_1_FS_KHZ)
#define SMPL_PITCH_UPSAMP_STAGES           (SMPL_PITCH_DOWNSAMP_STAGES + 1)
#define SMPL_PITCH_TOT_INTERP_DELAY        6
#define SMPL_PITCH_NUM_SUBFRAMES           8
#define SMPL_MINPITCH_MS                   2
#define SMPL_MAXPITCH_MS                   20
#define SMPL_PITCH_NUMLAGS_COARSE          (SMPL_PITCH_COARSE_FS_KHZ * (SMPL_MAXPITCH_MS - SMPL_MINPITCH_MS))
#define SMPL_MINPITCH_LEN                  (SMPL_MINPITCH_MS * SMPL_PITCH_FS_KHZ)
#define SMPL_MAXPITCH_LEN                  (SMPL_MAXPITCH_MS * SMPL_PITCH_FS_KHZ)
#define SMPL_MINPITCH_STAGE1               ((SMPL_MINPITCH_MS * SMPL_PITCH_STAGE_1_FS_KHZ) - SMPL_PITCH_TOT_INTERP_DELAY)
#define SMPL_MAXPITCH_STAGE1               ((SMPL_MAXPITCH_MS * SMPL_PITCH_STAGE_1_FS_KHZ) + SMPL_PITCH_TOT_INTERP_DELAY)
#define SMPL_PITCH_DELTAWGHT               0.1439f
#define SMPL_PITCH_SHORTWGHT1              0.04f
//#define SMPL_PITCH_SHORTWGHT2              1.0f
#define SMPL_PITCH_SPEC_HARM_BIAS          2.5f
#define SMPL_PITCH_SIGM_SCALE              10.0f
#define SMPL_PITCH_PREVWGHT                0.7981f
#define SMPL_PITCH_PREVWGHT_SPAN           0.15f
#define SMPL_PITCH_RATEWGHT_LR             0.028f
#define SMPL_PITCH_RATEWGHT_HR             0.022f
#define SMPL_PITCH_SEARCH_MULT_FRAC        100
#define SMPL_LAG_SUBFRLEN                  40 // 2.5ms
#define SMPL_PITCH_LAG_SUBFRLEN_STAGE1     ((SMPL_PITCH_STAGE_1_FS_KHZ*SMPL_LAG_SUBFRLEN)/SMPL_PITCH_FS_KHZ)

#define SMPL_PITCHBLOCK_MS                  2
#define SMPL_MIN_PITCH_LAG                  (SMPL_MINPITCH_MS*16)
#define SMPL_MAX_PITCH_LAG                  (SMPL_MAXPITCH_MS*16)
#define MAX_NUM_SUBFR                       4
#define SMPL_MAX_PITCH_LEN                  320
#define SMPL_PITCH_LOOKAHEAD_LEN            7
#define SMPL_PITCH_TOT_INTERPOL_DELAY_LEN   12
#define SMPL_MAX_LTP_BUF_LEN                659
#define SMPL_F_LEN                          257
#define SMPL_VUV_BIAS                       -0.1038f
#define SMPL_VUV_HYST                       0.05f

#define SMPL_HARM_POSTF_FB_DELAY            8
#define SMPL_HARM_POSTF_DELAY               (SMPL_HARM_POSTF_LAG_SUBFR_LEN)
#if !SMPL_DUMP_FEATURES
#define SMPL_USE_LPC_POSTFILTER             1  // uncomment to turn on LPC postfilter
#define SMPL_USE_TILT_POSTFILTER            1  // uncomment to turn on tilt postfilter
#endif
#define SMPL_TOT_POSTFILT_DELAY             (SMPL_HARM_POSTF_FB_DELAY + SMPL_HARM_POSTF_DELAY)

#define SMPL_LPC_POST_IMPZ_LEN              32
#define SMPL_LPC_POSTFILT_FAST              0

#define SMPL_PITCH_SHARPENING_COEF          0.9881f  // only used for lowRate

#define SMPL_HARM_POSTF_LAG_SUBFR_LEN       (SMPL_FRAME_LEN / SMPL_PITCH_NUM_SUBFRAMES)
#define SMPL_HARM_POSTF_FB_STRENGTH         0.4734f
#define SMPL_HARM_POSTF_STRENGTH            0.6438f
#define SMPL_HARM_POSTF_CUTOFF_HZ           4000.0f
#define SMPL_HARM_POSTF_NHARM_CUTOFF        6.3f
#define SMPL_HARM_POSTF_REDUCTION_FAC       0.0579f
#define SMPL_HARM_POSTF_OLA                 0.0f

#define SMPL_HP_POSTF_EMPH_COEF             3000.0f
#define SMPL_HP_POSTF_EMPH_GAIN             20.0f
#define SMPL_HP_POSTF_TRANSITION_SPEED      2.0f
#define	SMPL_HP_POSTF_FCORNER_3DB_HZ        50.0f

#define SMPL_FCB_SRV_MAX                    4

#define SMPL_RATE_CONT_SCALE                26.0f    // higher -> more pulses; only matters initially, before the rate control loop takes over
#define SMPL_RATE_CONT_CLAMP_MAX            0.9f
#define SMPL_RATE_CONT_CLAMP_MIN            -0.3f
#define SMPL_RATE_CONT_GAIN                 0.05f    // scale on controller updates

#define SMPL_DTX_NO_CANDIDATES              3 // Number of old candiates for DTX to keep
#define SMPL_CNG_NO_CANDIDATES              3 // Number of old candiates for CNG to keep
#define SMPL_VAD_N_BANDS                    4
#define SMPL_LB                             0
#define SMPL_HB                             1

#define SMPL_PLC_BWE_V                      0.995f
#define SMPL_PLC_BWE_UV                     0.95f
#define SMPL_PLC_LAG_DRIFT                  1.01f
#define SMPL_PLC_EXC_ATTEN                  0.90f
#define SMPL_PLC_ACB_MIN                    0.7f
#define SMPL_PLC_ACB_MAX                    0.95f
#define SMPL_PLC_BLEND_SHAPE                -0.2f * SMPL_LAG_SUBFRLEN
#define SMPL_PLC_BLEND_OFFSET               75.0f / SMPL_LAG_SUBFRLEN
#define SMPL_PLC_INTERPOL_SHAPE             0.025f
#define SMPL_PLC_INTERPOL_OFFSET            2.5f
#define SMPL_COMFORT_SIGNAL_THRESHOLD       100.0f
#define SMPL_PLC_HB_ATTEN                   0.8f
#define SMPL_PLC_EARLY_ATTEN_MS             40
#define SMPL_PLC_EARLY_ATTEN_POW            3.0f
#define SMPL_PLC_EARLY_ATTEN_GAIN           0.99f
#define SMPL_PLC_LATE_ATTEN_SCALE           1.0f
#define SMPL_PLC_LATE_ATTEN_SHAPE           0.5f
#define SMPL_PLC_LATE_ATTEN_OFFSET          10.0f
// The following three commented lines pass Meta's integration tests when
// replacing the three above lines. This results in longer decay times and
// changes the quality of the current PLC.
// #define SMPL_PLC_LATE_ATTEN_SCALE           0.5f
// #define SMPL_PLC_LATE_ATTEN_SHAPE           0.5f
// #define SMPL_PLC_LATE_ATTEN_OFFSET          13.0f
#define SMPL_RECOVER_MIN_BWE                0.95f
#define SMPL_RECOVER_LEN_MS                 60
#define SMPL_PLC_INJECT_SMTH                0.999f
#define SMPL_PLC_INJECT_GAIN                0.8f

#define SMPL_SWITCH_TO_STEREO_BITRATE_THR   22000
#define SMPL_STEREO_BITRATE_THR             18000
#define SMPL_STEREO_SMOOTHING_BITRATE_THR   14000

typedef enum SmplFrameTypes_ {
    BACKGROUND_NOISE = 0,
    UNVOICED = 1,
    VOICED = 2,
    SMPL_SID = 3,
    NO_TRANSMISSION = 4
} SmplFrameTypes;

typedef enum SmplVadTypes_ {
    INACTIVE = 0,
    HANGOVER = 1,
    ACTIVE = 2
} SmplVadTypes;

typedef enum SmplLpcPostFilterMode_ {
    SMPL_LPC_PSTF_MODE_WB_OFF_SWB_ON = 0,
    SMPL_LPC_PSTF_MODE_WB_ON_SWB_ON = 1,
    SMPL_LPC_PSTF_MODE_WB_ON_SWB_OFF = 2,
    SMPL_LPC_PSTF_MODE_WB_OFF_SWB_OFF = 3,
} SmplLpcPostFilterMode;

#define SMPL_DEC_NOISE_V_NOISE_GAIN         0.35f
#define SMPL_DEC_NOISE_UV_NOISE_GAIN        0.8f
#define SMPL_DEC_NOISE_UV_FCORNER_HZ        800.0f
#define SMPL_ENV_SMTH_COEF_V                0.95f
#define SMPL_ENV_SMTH_COEF_UV               0.995f
#define SMPL_ENV_SMTH_COEF_UV_V             0.99f
#define SMPL_NOISE_CORR_ORDER               2
#define SMPL_NOISE_DCT_ORDER                16

#define SMPL_HB_LPC_ORDER                   4
#define SMPL_HB_LPC_REG                     1e-3f
#define SMPL_HB_LPC_BWE                     0.98f
#define SMPL_HB_SMTH_COEF_V                 0.95f
#define SMPL_HB_SMTH_COEF_UV                0.994f
#define SMPL_HB_POST_ORDER                  2
#define SMPL_HB_RATIO_LIMIT                 7.141147671186561f // defined to be the 0.9995 quantile of all ratios
#define SMPL_UP_2X_MAX_LEN                  2 * (SMPL_MAX_WB_PACKET_SIZE_IN_SAMPLES * 32000 / 16000)
#define SMPL_UP_2X_STATE_LEN                2

#define SMPL_CELP_FS_KHZ                    16
#define SMPL_V_GAIN_MIN_DB                  -100.0f
#define SMPL_V_GAIN_MAX_DB                  0.0f
#define SMPL_V_GAIN_STEP_DB                 3.0f
#define SMPL_UV_GAIN_MIN_DB                 -90.0f
#define SMPL_UV_GAIN_MAX_DB                 0.0f
#define SMPL_UV_GAIN_STEP_DB                1.0f
#define SMPL_RATE_ACB_SCALE                 0.9f

#define SMPL_CELP_IDX_FEC                   0
#define SMPL_CELP_IDX_MAIN                  1
#define SMPL_CELP_MAX_RATES                 (SMPL_CELP_IDX_MAIN + 1)
#define SMPL_CELP_MAX_NUMSURV               8

#define SMPL_MAX_PULSES_PER_SF              40

#define SMPL_FLAG_DECODE_NORMAL              0
#define SMPL_FLAG_PACKET_LOST                1
#define SMPL_FLAG_DECODE_LBRR                2

#define SMPL_BASE_ENCODER_1                  0
#define SMPL_BASE_ENCODER_2                  1
#define SMPL_BASE_ENCODERS                   SMPL_BASE_ENCODER_2 + 1

#define SMPL_NON_FLAT_STATE_LEN 5
#define SMPL_NON_FLAT_SUBFR_LEN 16
#define SMPL_UV_NONFLATNESS_PER_SF 1       // Incomment to run flatness per subframe
#define SMPL_UV_NONFLATNESS_THR 0.5f
#define SMPL_UV_NONFLATNESS_SA           // Incomment to make flatness threshold depend on speech activity
#define SMPL_NON_FLAT_NRGS_LEN ((SMPL_FRAME_LEN / SMPL_NON_FLAT_SUBFR_LEN) + SMPL_NON_FLAT_STATE_LEN)

#endif
