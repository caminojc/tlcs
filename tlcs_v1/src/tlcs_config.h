/* TLCS v1 configuration — CMA-ES optimized defaults from SMPL research.
 * Every float is an optimizer target. DO NOT HAND TUNE. */
#ifndef TLCS_CONFIG_H
#define TLCS_CONFIG_H

/* ── Core ──────────────────────────────────────────────── */
#define TLCS_SAMPLE_RATE        16000
#define TLCS_FRAME_MS           20
#define TLCS_FRAME_SIZE         (TLCS_SAMPLE_RATE / 1000 * TLCS_FRAME_MS)  /* 320 */
#define TLCS_SUBFRAME_MS        5
#define TLCS_SUBFRAME_SIZE      (TLCS_SAMPLE_RATE / 1000 * TLCS_SUBFRAME_MS) /* 80 */
#define TLCS_NUM_SUBFRAMES      (TLCS_FRAME_SIZE / TLCS_SUBFRAME_SIZE)     /* 4 */
#define TLCS_BITRATE_BPS        8000
#define TLCS_BITS_PER_FRAME     (TLCS_BITRATE_BPS * TLCS_FRAME_MS / 1000) /* 160 */
#define TLCS_BYTES_PER_FRAME    (TLCS_BITS_PER_FRAME / 8)                 /* 20 */

/* ── LPC ───────────────────────────────────────────────── */
#define TLCS_LPC_ORDER          16
#define TLCS_LPC_BWE            0.9999f   /* CMA-ES: minimal expansion */

/* ── LSP quantization ──────────────────────────────────── */
#define TLCS_LSP_NUM_SPLITS     4
#define TLCS_LSP_CB_BITS        8       /* 256 entries per split */
#define TLCS_LSP_CB_SIZE        (1 << TLCS_LSP_CB_BITS)
#define TLCS_LSP_TOTAL_BITS     (TLCS_LSP_NUM_SPLITS * TLCS_LSP_CB_BITS)  /* 32 */

/* ── Pitch ─────────────────────────────────────────────── */
#define TLCS_PITCH_MIN_LAG      32      /* ~500 Hz @ 16 kHz */
#define TLCS_PITCH_MAX_LAG      159     /* ~100 Hz — 7-bit range = 128 lags */
#define TLCS_PITCH_LAG_RANGE    (TLCS_PITCH_MAX_LAG - TLCS_PITCH_MIN_LAG + 1) /* 128 */
#define TLCS_PITCH_LAG_BITS     7       /* ceil(log2(128)) */
#define TLCS_PITCH_FRAC_BITS    1       /* half-sample fractional */
#define TLCS_PITCH_GAIN_BITS    3       /* 8 levels, 0 to 1.2 */
#define TLCS_PITCH_BITS_PER_SUB (TLCS_PITCH_LAG_BITS + TLCS_PITCH_FRAC_BITS + TLCS_PITCH_GAIN_BITS) /* 11 */

#define TLCS_PITCH_DELTAWGHT    0.1439f   /* CMA-ES */
#define TLCS_PITCH_PREVWGHT     0.7981f   /* CMA-ES */

/* ── VUV decision ──────────────────────────────────────── */
#define TLCS_VUV_BIAS           -0.1038f  /* CMA-ES */
#define TLCS_VUV_HYST           0.05f

/* ── Algebraic codebook ────────────────────────────────── */
#define TLCS_ACB_NUM_PULSES     3       /* 3 pulses (was 2) */
#define TLCS_ACB_NUM_TRACKS     5       /* 5 tracks: 80/5=16 positions, 4 bits */
#define TLCS_ACB_POS_PER_TRACK  (TLCS_SUBFRAME_SIZE / TLCS_ACB_NUM_TRACKS) /* 16 */
#define TLCS_ACB_POS_BITS       4       /* ceil(log2(16)) */
#define TLCS_ACB_BITS_PER_SUB   (TLCS_ACB_NUM_PULSES * (TLCS_ACB_POS_BITS + 1) + 1) /* 3×5+1=16 */
/* The +1 is a VUV flag bit — free voicing info per subframe */

/* ── Gain quantization ─────────────────────────────────── */
#define TLCS_GAIN_CB_BITS       5       /* 32 entries (was 64) */
#define TLCS_GAIN_CB_SIZE       (1 << TLCS_GAIN_CB_BITS)
#define TLCS_GAIN_BITS_PER_SUB  TLCS_GAIN_CB_BITS       /* 5 */

/* ── Pre/de-emphasis ───────────────────────────────────── */
#define TLCS_PREEMPH_COEFF      0.68f

/* ── Perceptual masking ────────────────────────────────── */
#define TLCS_PERC_MASK_SMTH     0.11f
#define TLCS_PERC_MEL_FC_HZ     320.0f

/* ── Harmonic postfilter ───────────────────────────────── */
#define TLCS_HARM_POSTF_STRENGTH     0.6438f   /* CMA-ES */
#define TLCS_HARM_POSTF_FB_STRENGTH  0.4734f   /* CMA-ES */
#define TLCS_HARM_POSTF_CUTOFF_HZ    4000.0f

/* ── Formant postfilter ────────────────────────────────── */
#define TLCS_FORMANT_PF_GAMMA_NUM    0.65f
#define TLCS_FORMANT_PF_GAMMA_DEN    0.80f
#define TLCS_FORMANT_PF_TILT         0.30f

/* ── Perceptual weighting filter ───────────────────────── */
#define TLCS_PERC_GAMMA1    0.94f
#define TLCS_PERC_GAMMA2    0.60f

/* ── Pitch sharpening ──────────────────────────────────── */
#define TLCS_PITCH_SHARPENING_COEF   0.9881f   /* CMA-ES */

/* ── Shaped noise fill (decoder) ──────────────────────── */
#define TLCS_NOISE_V_GAIN    0.35f
#define TLCS_NOISE_UV_GAIN   0.80f

/* ── Rate control ──────────────────────────────────────── */
#define TLCS_RATE_CONT_GAIN     0.05f

/* ── Bit budget verification ───────────────────────────── */
/* Per subframe: pitch(11) + fcb(16) + gain(5) = 32 bits
 * Per frame:    lsp(32) + 4×32 = 160 bits = 20 bytes     */
#if (TLCS_LSP_TOTAL_BITS + TLCS_NUM_SUBFRAMES * (TLCS_PITCH_BITS_PER_SUB + TLCS_ACB_BITS_PER_SUB + TLCS_GAIN_BITS_PER_SUB)) != TLCS_BITS_PER_FRAME
#error "Bit budget mismatch!"
#endif

#endif /* TLCS_CONFIG_H */
