# TLCS Progress — 2026-03-15 (night)

## Latest: 2-subframe / 8-pulse architecture
- Switched from 4×80-sample to 2×160-sample subframes
- 8 pulses per subframe (was 3) — 2.7× more excitation density
- Delta pitch encoding for SF1 (saves 3 bits → more FCB bits)
- NISQA: **0.81** (was 0.78 with 3 pulses)
- SNR: 2.0 dB | Correlation: 0.61 | RMS ratio: 0.63

## NISQA: 0.81 (target: >2.0)

## What's Working
- 2×160 subframes, 8 pulses each, 8 tracks
- Phi-based SMPL-style FCB search with num/den recurrence
- dB-stepped FCB gain (4-bit, -80 to 0 dB)
- Delta pitch (SF1 = SF0 ± 8 samples, 4-bit)
- Perceptual weighting, harmonic PF, pitch sharpening, shaped noise
- 20 bytes/frame = 8000 bps exact

## Still Broken
- NISQA 0.81 < 2.0 — gain domain still not right
- Weighted-domain search gains don't map cleanly to decoder reconstruction
- Need to validate: is the decoder applying gains in the same domain as encoder computed them?

## DGX
- SMPL steroids: running (24 params, 200 gens)

## Repo
https://github.com/caminojc/tlcs
