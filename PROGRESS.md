# TLCS Progress — 2026-03-16 06:00

## Current Best: NISQA 1.67 avg
User: "pretty good" — closing in on MLOW quality.

## Architecture (commit a0e2a0f)
- 2-basis ACB (MLOW-style: basis0 + basis1 neighbor interpolation)
- Joint (g0,g1,gc) gain search over 8-entry ACB codebook × dB-stepped FCB
- 2-pass FCB: redo with quantized ACB gains if better
- W(z) on both h and target (MLOW gain approach)
- SILK-style LSP conversion (62 dB roundtrip)
- 8 pulses Phi-based, 2×160 subframes
- Formant PF 0.65/0.80, harmonic PF 0.64, noise fill v=0.20/uv=0.40

## DGX Status
- SMPL steroids: STOPPED at gen 70, best 3.82 (converged)
- **TLCS optimizer: RUNNING, gen 1, 100 gens, 12 candidates, full DGX**
  Searching: LPC_BWE, PREEMPH, HARM_POSTF, FORMANT_PF, NOISE, PITCH params

## What's Left
DGX optimizer will find best params for this architecture.
After that: delayed-decision search is the next code change.

## Repo
https://github.com/caminojc/tlcs
