# TLCS Progress — 2026-03-16 02:00

## Current: 7.8 dB SNR, "sounds like a 2" per user listening
Bare CELP without postfilters or perceptual weighting. Intelligible but rough.
MLOW at same bitrate sounds like a 4.

## The Gap: Perceptual Weighting + Postfilters
When enabled, perceptual weighting (gamma1=0.94, gamma2=0.60) drops SNR from 7.8 to 0.2 dB.
Postfilters (harmonic, formant) also degrade output.
MLOW doesn't have this problem because its gain domain is consistent throughout.

**Root cause:** TLCS encoder searches in weighted domain but decoder synthesizes in unweighted domain. The dB-stepped gains computed from weighted inner products don't produce correct excitation levels in unweighted synthesis. MLOW stays in one domain throughout — gains are computed and applied in the same perceptual space.

## What Works (commit c05c85f)
- SILK-style LSP conversion: 62 dB roundtrip (was -1.1 dB — THIS was the scratchy bug)
- 8-pulse Phi-based FCB search (SMPL-style num/den recurrence)
- dB-stepped gain quantizer
- 2×160 subframes, delta pitch encoding
- 8000 bps exact (20 bytes/frame)
- Trained codebooks from C codec's own LPC output

## What's Broken
- Perceptual weighting crashes SNR (gain domain mismatch)
- Postfilters destabilize output
- No VUV-adaptive coding
- Some WAV files with non-standard headers crash the demo

## DGX
- SMPL steroids optimizer: gen 31+, best 3.81 SCOREQ

## Critical Next Step
Study MLOW's gain computation in `smpl_celp_util.c` lines 492-520 (decode) 
and `smpl_celp.c` (encode). Understand how it keeps gains consistent between 
encoder search domain and decoder synthesis domain. Mirror that exactly.

The specific question: does MLOW apply the FCB gain to the WEIGHTED 
excitation and then deweight at synthesis? Or does it compute gains that 
already account for the weighting filter's frequency response?

## Repo
https://github.com/caminojc/tlcs (branch: lr-v2-experiment)
