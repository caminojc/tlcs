# TLCS Progress — 2026-03-16 03:00

## Current Best: NISQA 1.53 avg (M1: 1.31, F2: 1.25, M2: 2.03)
User says: "better but not like MLOW — still scratchy muffled"
MLOW at same bitrate: NISQA ~3.8

## Session Journey (NISQA)
0.67 → 0.78 (LSP fix) → 1.3 (MLOW gain approach) → 1.53 (postfilters)

## What's Working
- SILK-style LSP conversion (62 dB roundtrip)
- MLOW-style gain: W(z) on both h and target, gain direct from search
- 8-pulse Phi-based FCB search
- Harmonic PF (0.64) + formant PF (0.65/0.80) + tilt (0.20)
- Light noise fill (v=0.20, uv=0.40)
- 8000 bps, 2×160 subframes

## Best Config (tlcs_config.h)
- PERC_GAMMA1=0.94, PERC_GAMMA2=0.60
- HARM_POSTF_STRENGTH=0.64, FB=0.47
- FORMANT_PF_NUM=0.65, DEN=0.80, TILT=0.20
- NOISE_V=0.20, NOISE_UV=0.40
- PITCH_SHARPENING=0.0 (hurts female voice)

## Gap to MLOW (1.53 → 3.8)
Parameter tuning is at its ceiling. The remaining 2.3 NISQA points need:

1. **Delayed-decision codebook search** — MLOW uses Viterbi-like path tracking
   with 30-130 survivors. Our greedy 8-pulse search places pulses suboptimally.
   File: `smpl_celp.c` lines 283-430 (calc_gains_v with rate-distortion).

2. **Variable pulse count** — MLOW adapts pulses per subframe via rate control.
   Voiced frames get more pulses, silence gets fewer. 
   File: `smpl_bitrate_controller.c` (subfr_importance weighting).

3. **Joint ACB+FCB gain optimization** — MLOW solves a 3×3 system (2 ACB gains + 
   1 FCB gain) jointly. We use separate scalar quantization.
   File: `smpl_celp.c` lines 286-343 (Phi_all, dall, joint RD search).

4. **Better pitch search** — MLOW uses 8 pitch subframes per frame with 
   block-based tracking. We use 2 subframes with simple closed-loop.

## DGX
- SMPL steroids: gen 31+, best 3.81 (24 params, 200 gens)

## Repo
https://github.com/caminojc/tlcs (branch: lr-v2-experiment, commit f755bae)
