# TLCS Progress — 2026-03-16 02:30

## Current: NISQA 1.2, "sounds like speech but scratchy and low-fi"
Perceptual weighting on target only. 8 pulses, bare CELP. User says "a 2."
MLOW at same bitrate = 4.

## Session Achievements
- **LSP root finder fixed** (was returning uniform fallback every frame = THE scratchy bug)
- **SILK-style LSP conversion** ported (62 dB roundtrip, was -1.1 dB)
- **8-pulse architecture** (2×160 subframes, was 3 pulses in 4×80)
- **Perceptual weighting on target** working (NISQA 0.78 → 1.3)
- **Gain domain partially solved**: h = 1/A(z) unweighted, W(z) on target only

## The Remaining Gap (1.2 → 4.0 NISQA)
**Perceptual weighting must be on BOTH h and target** — that's how MLOW does it.
The trick is how MLOW handles gains so they work in both domains.

MLOW's approach (from source analysis):
- Encoder searches using h_weighted = W(z) * 1/A(z) impulse response
- Target is also weighted
- Gains are computed in weighted domain
- But the DECODER applies gains to excitation that goes through 1/A(z) only
- The gain SCALING accounts for the energy ratio between weighted and unweighted IR

**This is the exact question**: how does gain_weighted translate to gain_unweighted?
Answer is in `smpl_celp.c` and `smpl_celp_util.c`.

## Architecture
- 16 kHz, 2×160 subframes, 8 pulses, 8000 bps (20 bytes/frame)
- SILK-style LSP (cosine table + ordering + stability check)
- Phi-based FCB search (SMPL num/den recurrence)
- dB-stepped gain quantizer

## Repo
https://github.com/caminojc/tlcs (branch: lr-v2-experiment)
