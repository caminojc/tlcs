# TLCS Progress — 2026-03-16 04:00

## Current Best: NISQA 1.6 avg — "really good speech" per user
From noise (0.67) to speech (1.6) in one session. Own C code, 8 kbps.

## Best Config
```
PERC_GAMMA1=0.94, PERC_GAMMA2=0.60 (W(z) on both h and target)
HARM_POSTF=0.64, FB=0.47
FORMANT_PF=0.65/0.80, TILT=0.20
NOISE_V=0.20, NOISE_UV=0.40
PITCH_SHARPENING=0.0
LPC_ORDER=16, 4×256 split VQ, SILK-style LSP conversion
8 pulses Phi-based, 2×160 subframes, joint gain optimization
```

## Session Breakthroughs
1. LSP root finder fixed (uniform fallback → real roots)
2. SILK-style LSP conversion (62 dB roundtrip)
3. MLOW gain approach (W(z) on both h+target, gain direct)
4. Joint pitch+FCB gain optimization (brute-force over quantized space)
5. Full-corpus codebook training on DGX

## Quality vs MLOW
Our 8k ≈ MLOW 5k quality. Gap to MLOW 8k needs:
- Delayed-decision codebook search
- Variable pulse count with rate control
- Joint 3×3 ACB+FCB gain (MLOW uses 2 ACB basis vectors)
- SMPL-optimized params don't transfer (different architecture)

## DGX Status
- SMPL steroids: gen 63/200, best 3.82 (24 params optimized)
- TLCS v1 optimizer: gen 5, exploring (NISQA ~0.7 on DGX)

## Tags
- `best-nisqa-1.5` — first good config
- `best-nisqa-1.5-v2` — with full-corpus codebooks

## Repo
https://github.com/caminojc/tlcs (branch: lr-v2-experiment, commit ceb5d2c)
