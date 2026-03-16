# TLCS Progress — 2026-03-15 (late night)

## Key Finding: Quality Breakdown by Component
| Component | SNR | Delta | Status |
|-----------|-----|-------|--------|
| LPC analysis-synthesis (perfect) | 10.1 dB | — | Verified |
| + Pitch (no VQ) | **11.2 dB** | +1.1 | Working well |
| + LSP VQ (4×256 split) | **4.2 dB** | -7.0 | **BOTTLENECK** |
| + Gain VQ + bitstream | **2.0 dB** | -2.2 | Lossy |
| Full codec with postfilters | **0.81 NISQA** | — | All features enabled |

**The LSP VQ is destroying 7 dB.** This is the #1 fix needed.

## Architecture (current)
- 2 × 160-sample subframes, 8 pulses each
- Phi-based SMPL-style FCB search
- dB-stepped gain quantizer
- 160 bits = 20 bytes = 8000 bps

## What Sounds Good
- `/tmp/pitch_pulse.wav` — raw LPC + pitch + 8 pulses, 11.2 dB SNR
- This proves the CELP pipeline works when VQ doesn't ruin it

## Next Step
**Implement predictive LSP VQ** — quantize (lsp_current - lsp_previous) instead of absolute LSPs. This typically saves 3-5 dB of VQ distortion because consecutive frames have similar LPC. SMPL does this with its MA-predicted LSF coding.

Alternative: reduce LPC order to 10 (narrowband-style) — each split covers 2.5 LSPs with 256 entries, much better resolution. But loses wideband quality.

## DGX
- SMPL steroids optimizer still running

## Repo
https://github.com/caminojc/tlcs (branch: lr-v2-experiment)
