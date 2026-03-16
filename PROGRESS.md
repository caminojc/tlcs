# TLCS Progress — 2026-03-16 05:00

## Current Best: NISQA 1.67 avg (M1:1.21, F2:1.42, M2:2.40)
Our 8k CELP ≈ MLOW 5k quality. User: "nice but we need to close the gap"

## All-Time Best Config (commit 290b6eb, tag best-nisqa-1.67)
```
W(z): gamma1=0.94, gamma2=0.60 (on both h and target — MLOW-style)
2-basis ACB: g0*basis0 + g1*basis1, 8-entry joint codebook
Joint gain: brute-force over (g0,g1) × dB-stepped gc
Harmonic PF: 0.64, FB: 0.47
Formant PF: 0.65/0.80, tilt: 0.20
Noise: v=0.20, uv=0.40
8 pulses Phi-based, 2×160 subframes, SILK-style LSP
```

## What Didn't Help (diminishing returns)
- Stronger postfilters (SNR crashes)
- More noise fill (hurts female voice)
- Pitch sharpening (hurts female)
- Wider pitch search (codebook mismatch)
- Persistent W(z) state (less stable)
- LPC order 12 (lost detail)
- SMPL-optimized values (too aggressive for our architecture)

## Gap: 1.67 → 3.8 (MLOW)
Parameter tuning exhausted at 1.67. Remaining gap needs:
1. **Delayed-decision FCB search** with multiple survivors
2. **Variable pulse count** (some frames need 4, others 12)
3. **Better ACB gain codebook** (train from real speech, not static)

## DGX
- SMPL steroids: gen 64/200, best 3.82
- TLCS optimizer: gen 13, best 0.71

## Repo
https://github.com/caminojc/tlcs (branch: lr-v2-experiment)
