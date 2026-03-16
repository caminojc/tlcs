#ifndef TLCS_TYPES_H
#define TLCS_TYPES_H

#include <stdint.h>
#include <stddef.h>

/* ── Codec configuration constants ─────────────────────────────── */

#define TLCS_MAX_SAMPLE_RATE   16000
#define TLCS_MIN_SAMPLE_RATE    8000
#define TLCS_FRAME_MS             20
#define TLCS_MAX_FRAME_SIZE     (TLCS_MAX_SAMPLE_RATE / 1000 * TLCS_FRAME_MS)  /* 320 */
#define TLCS_MIN_FRAME_SIZE     (TLCS_MIN_SAMPLE_RATE / 1000 * TLCS_FRAME_MS)  /* 160 */
#define TLCS_MAX_SUBFRAMES        8
#define TLCS_SUBFRAMES            8   /* legacy alias — prefer cfg.n_subfr */
#define TLCS_MAX_SUBFR_SIZE     160                                            /* max subframe buffer */

/* Bitrate thresholds: VLR < 8k < LR < 12k < HR */
#define TLCS_VLR_BITRATE_THRESHOLD 8000
#define TLCS_LR_BITRATE_THRESHOLD  12000

#define TLCS_LPC_ORDER_NB        10
#define TLCS_LPC_ORDER_WB        16
#define TLCS_LPC_ORDER_MAX       TLCS_LPC_ORDER_WB

#define TLCS_MAX_PITCH_LAG      300   /* ~53 Hz at 16 kHz */
#define TLCS_MIN_PITCH_LAG       20   /* ~800 Hz at 16 kHz */

/* Maximum bitstream bytes per frame */
#define TLCS_MAX_FRAME_BYTES     80

/* QMF filter bank */
#define TLCS_QMF_TAPS_MAX        32
#define TLCS_QMF_HALF_MAX        (TLCS_QMF_TAPS_MAX / 2)

/* ── Error codes ───────────────────────────────────────────────── */

typedef enum {
    TLCS_OK                  =  0,
    TLCS_ERR_INVALID_ARG     = -1,
    TLCS_ERR_BUFFER_TOO_SMALL= -2,
    TLCS_ERR_BAD_BITSTREAM   = -3,
    TLCS_ERR_INTERNAL        = -4,
    TLCS_ERR_IO              = -5
} tlcs_status;

/* ── Frame mode (2 bits in bitstream) ──────────────────────────── */

typedef enum {
    TLCS_MODE_VOICED     = 0,
    TLCS_MODE_UNVOICED   = 1,
    TLCS_MODE_TRANSITION = 2,
    TLCS_MODE_SILENCE    = 3
} tlcs_frame_mode;

/* ── Codec mode (1 bit in bitstream, HR only) ─────────────────── */

typedef enum {
    TLCS_CODEC_MODE_S    = 0,   /* CELP (speech/predictive) */
    TLCS_CODEC_MODE_T    = 1    /* TCX (transform) */
} tlcs_codec_mode;

/* ── Codec configuration ───────────────────────────────────────── */

typedef struct {
    int32_t  sample_rate;     /* 8000 or 16000 */
    int32_t  bitrate;         /* target bitrate in bps (4000–24000) */
    int32_t  frame_size;      /* samples per frame (computed from sample_rate) */
    int32_t  subfr_size;      /* samples per subframe */
    int32_t  n_subfr;         /* subframes per frame: 4 (LR) or 8 (HR) */
    int32_t  lpc_order;       /* 10 (LR/NB) or 16 (HR WB) */
    int32_t  num_pulses;      /* ACELP pulses: 2 (LR) or 10 (HR) */
    int32_t  lsf_bits;        /* bits per LSF coeff: 5 (LR) or 7 (HR) */
    int32_t  fcb_gain_bits;   /* FCB gain bits: 5 (LR) or 7 (HR) */
    int32_t  pitch_delta_bits;  /* delta pitch bits: 5 (LR) or 7 (HR) */
    int32_t  pitch_delta_offset;/* signed-to-unsigned offset: 16 (LR) or 64 (HR) */
    int32_t  use_ec;           /* 1 = entropy-coded frames (LR), 0 = fixed-width */
    int32_t  use_lsf_vq;      /* 1 = split-VQ for LSFs, 0 = scalar quantization */
} tlcs_config;

/* ── Encoder state ─────────────────────────────────────────────── */

typedef struct {
    tlcs_config cfg;

    /* Pre-emphasis */
    int16_t  preemph_mem;

    /* LPC */
    int16_t  prev_lsf[TLCS_LPC_ORDER_MAX];
    int16_t  prev_prev_lsf[TLCS_LPC_ORDER_MAX];

    /* Pitch */
    int16_t  prev_pitch_lag;
    int16_t  prev_pitch_gain;

    /* Adaptive codebook (past excitation) — float for pitch precision */
    float    exc_buf[TLCS_MAX_PITCH_LAG + TLCS_MAX_FRAME_SIZE + TLCS_MAX_SUBFR_SIZE];

    /* Innovation history buffer for cross-subframe pitch sharpening */
    float    innov_buf[TLCS_MAX_PITCH_LAG + TLCS_MAX_FRAME_SIZE];

    /* Speech history buffer for OL pitch search (pre-emphasized speech) */
    float    speech_buf[TLCS_MAX_PITCH_LAG + TLCS_MAX_FRAME_SIZE];

    /* Synthesis filter memory (float for analysis precision) */
    float    synth_mem[TLCS_LPC_ORDER_MAX];

    /* Weighted synthesis filter memory (float for ZIR precision) */
    float    wgt_synth_mem[TLCS_LPC_ORDER_MAX];

    /* Perceptual weighting filter memory (float for ZIR precision) */
    float    wgt_mem[TLCS_LPC_ORDER_MAX];

    /* Frame energy tracking */
    int32_t  frame_energy_smooth;

    /* FCB gain prediction state (persists across frames) */
    float    prev_cb_gain_mag;

    /* Frame counter */
    uint32_t frame_count;

    /* Band-split state (WB mode) */
    float    qmf_ana_mem[TLCS_QMF_TAPS_MAX];
    int16_t  hb_prev_lsf[8];

    /* TCX mode state */
    int32_t  prev_codec_mode;                       /* previous frame's codec mode */
    int32_t  mode_hold_count;                       /* hysteresis counter for mode switching */
    float    mdct_overlap[TLCS_MAX_FRAME_SIZE];     /* MDCT forward overlap (prev frame speech) */
    float    imdct_recon_overlap[TLCS_MAX_FRAME_SIZE]; /* IMDCT overlap for encoder-side reconstruction */
    uint32_t recon_noise_seed;                      /* noise seed for encoder-side reconstruction */
} tlcs_encoder;

/* ── Decoder state ─────────────────────────────────────────────── */

typedef struct {
    tlcs_config cfg;

    /* De-emphasis */
    int16_t  deemph_mem;

    /* LPC */
    int16_t  prev_lsf[TLCS_LPC_ORDER_MAX];
    int16_t  prev_prev_lsf[TLCS_LPC_ORDER_MAX];

    /* Pitch */
    int16_t  prev_pitch_lag;
    int16_t  prev_pitch_gain;

    /* Adaptive codebook (past excitation) — float for pitch precision */
    float    exc_buf[TLCS_MAX_PITCH_LAG + TLCS_MAX_FRAME_SIZE + TLCS_MAX_SUBFR_SIZE];

    /* Innovation history buffer for cross-subframe pitch sharpening */
    float    innov_buf[TLCS_MAX_PITCH_LAG + TLCS_MAX_FRAME_SIZE];

    /* Synthesis filter memory (float precision) */
    float    synth_mem[TLCS_LPC_ORDER_MAX];

    /* Formant post-filter state */
    float    pf_fir_mem[TLCS_LPC_ORDER_MAX];   /* FIR A(z/γn) memory */
    float    pf_synth_mem[TLCS_LPC_ORDER_MAX]; /* IIR 1/A(z/γd) memory */
    float    pf_tilt_mem;
    float    pf_history[TLCS_MAX_PITCH_LAG + TLCS_MAX_FRAME_SIZE];
    float    pf_lp_mem;  /* 1st-order LP filter state */

    /* Harmonic post-filter history */
    float    harm_pf_hist[TLCS_MAX_PITCH_LAG + TLCS_MAX_FRAME_SIZE];

    /* Pre-synthesis excitation tilt memory */
    float    exc_tilt_mem;

    /* Innovation filter memory (AMR-WB style periodicity enhancement) */
    float    innov_filt_mem;

    /* Noise fill state */
    uint32_t noise_seed;
    float    noise_env_state;
    float    comfort_noise_mem;

    /* UV pulse shaping filter state (SMPL-style ARMA) */
    float    uv_ma_mem;
    float    uv_ar_mem;

    /* FCB gain prediction state (persists across frames) */
    float    prev_cb_gain_mag;

    /* PLC state */
    int16_t  plc_loss_count;
    int16_t  plc_pitch_lag;
    int16_t  plc_gain_atten;  /* Q15 */

    /* Frame energy tracking */
    int32_t  frame_energy_smooth;

    /* Frame counter */
    uint32_t frame_count;

    /* Band-split state (WB mode) */
    float    qmf_lb_mem[TLCS_QMF_HALF_MAX];
    float    qmf_hb_mem[TLCS_QMF_HALF_MAX];
    int16_t  hb_prev_lsf[8];
    float    hb_synth_mem[8];
    uint32_t hb_rng_state;

    /* HP post-filter state (50 Hz Butterworth HP) */
    float    hp50_x1, hp50_x2;   /* input delay line */
    float    hp50_y1, hp50_y2;   /* output delay line */

    /* HF shelf emphasis state (3 kHz presence boost) */
    float    shelf_prev_x;

    /* TCX mode state */
    int32_t  prev_codec_mode;                       /* previous frame's codec mode */
    float    mdct_overlap[TLCS_MAX_FRAME_SIZE];     /* IMDCT overlap-add buffer */
    float    tcx_synth_mem[TLCS_LPC_ORDER_MAX];     /* LPC synthesis memory for TCX */
} tlcs_decoder;

#endif /* TLCS_TYPES_H */
