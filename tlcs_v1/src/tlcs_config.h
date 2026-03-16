/* TLCS v1 configuration — 2-subframe, 8-pulse CELP at 8 kbps.
 * Every float is an optimizer target. DO NOT HAND TUNE. */
#ifndef TLCS_CONFIG_H
#define TLCS_CONFIG_H

/* ── Core ──────────────────────────────────────────────── */
#define TLCS_SAMPLE_RATE        16000
#define TLCS_FRAME_MS           20
#define TLCS_FRAME_SIZE         (TLCS_SAMPLE_RATE / 1000 * TLCS_FRAME_MS)  /* 320 */
#define TLCS_SUBFRAME_MS        10      /* 10ms subframes (was 5ms) */
#define TLCS_SUBFRAME_SIZE      (TLCS_SAMPLE_RATE / 1000 * TLCS_SUBFRAME_MS) /* 160 */
#define TLCS_NUM_SUBFRAMES      (TLCS_FRAME_SIZE / TLCS_SUBFRAME_SIZE)     /* 2 */
#define TLCS_BITRATE_BPS        8000
#define TLCS_BITS_PER_FRAME     (TLCS_BITRATE_BPS * TLCS_FRAME_MS / 1000) /* 160 */
#define TLCS_BYTES_PER_FRAME    (TLCS_BITS_PER_FRAME / 8)                 /* 20 */

/* ── LPC ───────────────────────────────────────────────── */
#define TLCS_LPC_ORDER          16
#define TLCS_LPC_BWE            0.9999f   /* CMA-ES */

/* ── LSP quantization ──────────────────────────────────── */
#define TLCS_LSP_NUM_SPLITS     4
#define TLCS_LSP_CB_BITS        8       /* 256 entries per split */
#define TLCS_LSP_CB_SIZE        (1 << TLCS_LSP_CB_BITS)
#define TLCS_LSP_TOTAL_BITS     (TLCS_LSP_NUM_SPLITS * TLCS_LSP_CB_BITS)  /* 32 */

/* ── Pitch ─────────────────────────────────────────────── */
#define TLCS_PITCH_MIN_LAG      32      /* ~500 Hz @ 16 kHz */
#define TLCS_PITCH_MAX_LAG      159     /* ~100 Hz */
#define TLCS_PITCH_LAG_RANGE    (TLCS_PITCH_MAX_LAG - TLCS_PITCH_MIN_LAG + 1)
#define TLCS_PITCH_LAG_BITS     7       /* full lag for SF0 */
#define TLCS_PITCH_DELTA_BITS   4       /* delta lag for SF1 (±8) */
#define TLCS_PITCH_FRAC_BITS    1       /* half-sample */
#define TLCS_PITCH_GAIN_BITS    3       /* 8 levels */
/* SF0: full lag(7) + frac(1) + gain(3) = 11 bits */
#define TLCS_PITCH_BITS_SF0     (TLCS_PITCH_LAG_BITS + TLCS_PITCH_FRAC_BITS + TLCS_PITCH_GAIN_BITS)
/* SF1: delta lag(4) + frac(1) + gain(3) = 8 bits */
#define TLCS_PITCH_BITS_SF1     (TLCS_PITCH_DELTA_BITS + TLCS_PITCH_FRAC_BITS + TLCS_PITCH_GAIN_BITS)
#define TLCS_PITCH_TOTAL_BITS   (TLCS_PITCH_BITS_SF0 + TLCS_PITCH_BITS_SF1) /* 19 */

#define TLCS_PITCH_DELTAWGHT    0.1439f   /* CMA-ES */
#define TLCS_PITCH_PREVWGHT     0.7981f   /* CMA-ES */

/* ── VUV decision ──────────────────────────────────────── */
#define TLCS_VUV_BIAS           -0.1038f  /* CMA-ES */
#define TLCS_VUV_HYST           0.05f

/* ── Algebraic codebook — 8 pulses in 160 samples ──────── */
#define TLCS_ACB_NUM_PULSES     8       /* 8 pulses! (was 3) */
#define TLCS_ACB_NUM_TRACKS     8       /* 8 tracks: 160/8=20 pos, 5 bits */
#define TLCS_ACB_POS_PER_TRACK  (TLCS_SUBFRAME_SIZE / TLCS_ACB_NUM_TRACKS) /* 20 */
#define TLCS_ACB_POS_BITS       5       /* ceil(log2(20)) */
#define TLCS_ACB_BITS_PER_SUB   (TLCS_ACB_NUM_PULSES * (TLCS_ACB_POS_BITS + 1)) /* 8×6=48 */

/* ── Gain quantization — dB-stepped (SMPL-style) ─────── */
#define TLCS_FCB_GAIN_BITS      4       /* 4-bit dB gain per subframe */
#define TLCS_GAIN_BITS_PER_SUB  TLCS_FCB_GAIN_BITS  /* 4 */
#define TLCS_V_GAIN_MIN_DB      -80.0f
#define TLCS_V_GAIN_MAX_DB        0.0f
#define TLCS_V_GAIN_STEPS         16    /* 4 bits */
#define TLCS_V_GAIN_STEP_DB       ((TLCS_V_GAIN_MAX_DB - TLCS_V_GAIN_MIN_DB) / (TLCS_V_GAIN_STEPS - 1))
#define TLCS_UV_GAIN_MIN_DB     -60.0f
#define TLCS_UV_GAIN_MAX_DB       0.0f
#define TLCS_UV_GAIN_STEPS        16
#define TLCS_UV_GAIN_STEP_DB      ((TLCS_UV_GAIN_MAX_DB - TLCS_UV_GAIN_MIN_DB) / (TLCS_UV_GAIN_STEPS - 1))
/* Legacy */
#define TLCS_GAIN_CB_BITS       5
#define TLCS_GAIN_CB_SIZE       (1 << TLCS_GAIN_CB_BITS)

/* ── Pre/de-emphasis ───────────────────────────────────── */
#define TLCS_PREEMPH_COEFF      0.68f

/* ── Perceptual masking ────────────────────────────────── */
#define TLCS_PERC_MASK_SMTH     0.11f
#define TLCS_PERC_MEL_FC_HZ     320.0f

/* ── Harmonic postfilter ───────────────────────────────── */
#define TLCS_HARM_POSTF_STRENGTH     0.55f
#define TLCS_HARM_POSTF_FB_STRENGTH  0.4734f
#define TLCS_HARM_POSTF_CUTOFF_HZ    4000.0f

/* ── Formant postfilter ────────────────────────────────── */
#define TLCS_FORMANT_PF_GAMMA_NUM    0.65f
#define TLCS_FORMANT_PF_GAMMA_DEN    0.80f
#define TLCS_FORMANT_PF_TILT         0.30f

/* ── Perceptual weighting filter ───────────────────────── */
#define TLCS_PERC_GAMMA1    0.94f
#define TLCS_PERC_GAMMA2    0.60f

/* ── Pitch sharpening ──────────────────────────────────── */
#define TLCS_PITCH_SHARPENING_COEF   0.0f

/* ── Shaped noise fill (decoder) ──────────────────────── */
#define TLCS_NOISE_V_GAIN    0.15f
#define TLCS_NOISE_UV_GAIN   0.40f

/* ── Rate control ──────────────────────────────────────── */
#define TLCS_RATE_CONT_GAIN     0.05f

/* ── Bit budget verification ───────────────────────────── */
/* LSP(32) + pitch(11+8=19) + 2×(FCB(48) + gain(4)) = 32+19+104 = 155
 * Plus pitch_gain for SF0 is in pitch bits. Spare: 5 bits for VUV/future.
 * Actually: 32 + 19 + 2*(48+4) = 32+19+104 = 155. 5 spare bits. */
#define TLCS_SPARE_BITS  (TLCS_BITS_PER_FRAME - TLCS_LSP_TOTAL_BITS - TLCS_PITCH_TOTAL_BITS - TLCS_NUM_SUBFRAMES * (TLCS_ACB_BITS_PER_SUB + TLCS_GAIN_BITS_PER_SUB))

#endif /* TLCS_CONFIG_H */
