# TLCS Progress — 2026-03-15 (evening)

## Latest: SMPL-style gain architecture implemented
- Phi-based FCB search (SMPL num/den recurrence)
- dB-stepped gain quantizer (replaces joint VQ)
- SNR: 1.4 → **2.0 dB**
- NISQA: 0.72 → **0.78** (target: >2.0)
- Correlation: 0.53 → **0.62**
- RMS ratio: **0.49** (was 0.04 before gain fixes)

## NISQA Score: 0.78 (target: >2.0)

## What's Working
- Bit budget: 160 bits exact (32 LSP + 4×32 subframe)
- LPC analysis-synthesis: 10.1 dB SNR (pipeline verified)
- Phi-based Algebraic CB with SMPL-style num/den search
- dB-stepped FCB gain quantizer (4-bit, -80 to 0 dB)
- Perceptual weighting W(z) = A(z/γ1)/A(z/γ2)
- Harmonic + formant postfilter, pitch sharpening, shaped noise fill

## What's Still Broken
- NISQA 0.78 < 2.0 target
- 3 pulses still sparse — SMPL uses 8+ at same bitrate via entropy coding
- Encoder stays in weighted domain but decoder uses gains directly — may still have domain mismatch in gain application

## DGX Status
- SMPL steroids optimizer: running (24 params, 200 gens)

## Repo
https://github.com/caminojc/tlcs (branch: lr-v2-experiment)
