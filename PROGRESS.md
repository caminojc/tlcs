# TLCS Progress — 2026-03-16 03:30

## Current Best: NISQA ~1.5 avg, SNR 5-7 dB
User says "better" — recognizable speech, still scratchy/muffled vs MLOW.

## Session Journey
| Change | NISQA | Key |
|--------|-------|-----|
| Start (broken LSP) | 0.67 | Unintelligible |
| LSP root finder fixed | 0.78 | Still uniform fallback |
| SILK-style LSP conversion | 0.78 | 62 dB roundtrip |
| MLOW gain: W(z) on both h+target | 1.3 | Gains work in decoder |
| Formant postfilter | 1.5 | De-muffled |
| Harmonic postfilter | 1.5-2.0 | M2 best at 2.0 |
| Best combined config | **1.53 avg** | Current |

## Config (tlcs_config.h)
```
PERC_GAMMA1=0.94, PERC_GAMMA2=0.60
HARM_POSTF=0.64, FB=0.47
FORMANT_PF=0.65/0.80, TILT=0.20
NOISE_V=0.20, NOISE_UV=0.40
PITCH_SHARPENING=0.0 (hurts female)
LPC_ORDER=16, LSP 4×256 split VQ
8 pulses, 2×160 subframes, 8000 bps
```

## What Didn't Help
- LPC order 12 (lost spectral detail, -0.3 NISQA)
- 8-split VQ (too few entries per split)
- Predictive VQ (drift/divergence)
- Iterative pulse refinement (mixed results)
- Pitch sharpening (hurts female voice)
- Stronger perceptual weighting gamma (0.92 worse than 0.94)

## Gap to MLOW (~1.5 → 3.8)
Needs architectural changes:
1. Delayed-decision codebook search (smpl_celp.c)
2. Variable pulse count with rate control (smpl_bitrate_controller.c)
3. Joint ACB+FCB gain optimization (3×3 system)
4. Better pitch search (8 subframes, block tracking)

## DGX
SMPL steroids optimizer gen 31+, best 3.81

## Repo
https://github.com/caminojc/tlcs (branch: lr-v2-experiment, commit ee07a26)
