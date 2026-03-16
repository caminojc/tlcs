# TLCS Progress — 2026-03-15 (late night)

## BREAKTHROUGH: LSP root finder fixed
- Root cause: P polynomial deconvolution used SUBTRACT instead of ADD
- All frames now get 16 real LSP roots spanning 332-7371 Hz
- Previously returned uniform fallback → same flat LPC for every frame
- This was THE bug causing scratchy/metallic output

## Current: 3.25 dB SNR — recognizable speech!
- 8 pulses / 160-sample subframes / Phi-based search
- dB-stepped gain / absolute VQ with correct codebooks
- LSP VQ: 4 splits × 256 entries × 4 dims = 32 bits

## Quality Breakdown
| Config | SNR |
|--------|-----|
| LPC analysis-synthesis (perfect) | 10.1 dB |
| + Pitch (no VQ) | 11.2 dB |
| + Absolute VQ (correct codebooks) | **3.25 dB** ← HERE |
| Pre-LSP-fix (uniform fallback) | 0.03 dB |

## Next: Close the 8 dB VQ gap
The LSP VQ has 37.6 dB SNR on the LSP vectors — the error is small.
But small LSP errors amplify to large LPC coefficient errors.
Need: weighted LSP distance (emphasize closely-spaced LSP pairs)
or switch to LSF domain quantization.

## DGX
- SMPL steroids: gen 31/200, best 3.81

## Repo: https://github.com/caminojc/tlcs
