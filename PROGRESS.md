# TLCS Progress — 2026-03-16 01:00

## Current State: 3.25 dB SNR — barely intelligible speech
TLCS v1 CELP codec produces recognizable but terrible speech.
Sounds "scratchy and almost resembling speech" per user listening.

## The One Bug Left: LSP→LPC roundtrip loses 8 dB
| Config | SNR | Notes |
|--------|-----|-------|
| Raw LPC + pitch + 8 pulses (no LSP, no VQ) | **11.2 dB** | Decent speech |
| + LSP conversion only (no VQ) | **~4 dB** | LSP→LPC destroys 7 dB |
| + VQ (4×256 split) | **3.25 dB** | VQ adds only 1 dB loss |
| Previous (uniform LSP fallback) | 0.03 dB | LSP bug was THE problem |

**Root cause:** `tlcs_lsp_to_lpc()` reconstruction from LSP roots introduces 
large LPC coefficient errors. The LSP frequencies are correct (verified: 
16 roots spanning 332-7371 Hz) but the polynomial reconstruction from 
roots amplifies small numerical errors in closely-spaced LSP pairs.

## What's Been Fixed This Session
1. LSP deconvolution: P/(1+z^-1) uses ADD not SUBTRACT (was returning uniform fallback)
2. Chebyshev root finding with proper symmetric polynomial evaluation
3. 8-pulse / 2-subframe architecture (was 3-pulse / 4-subframe)
4. Phi-based SMPL-style FCB search with num/den recurrence
5. dB-stepped gain quantizer (SMPL-style)
6. Joint gain optimization in unweighted domain
7. Codebooks trained from C codec's own LPC→LSP output

## Exact Next Step
**Fix `tlcs_lsp_to_lpc()` reconstruction accuracy.**

The polynomial product `P'(z) = prod(1 - 2cos(w_k)z^{-1} + z^{-2})` 
accumulates floating-point errors for order 16 (8 quadratic factors). 
Options:
1. Use double precision for the polynomial reconstruction
2. Use the direct-form LSP→LPC from ITU-T G.729 reference code
3. Reduce LPC order to 12 (6 quadratic factors, less error accumulation)
4. Add LSP-weighted distance in VQ to penalize closely-spaced pairs

The simplest test: change `tlcs_lsp_to_lpc` to use `double` internally 
and see if roundtrip SNR improves from -1.1 dB.

## DGX Status
- SMPL steroids optimizer: gen 31/200, best 3.81

## Architecture Summary
- 16 kHz, 20ms frames (320 samples), 2 subframes × 160 samples
- LPC order 16, 4-split VQ (4×8 bits = 32 bits)
- 8-pulse algebraic CB (Phi-based, 48 bits/subframe)
- dB-stepped FCB gain (4 bits), scalar pitch gain (3 bits)
- Delta pitch for SF1 (4 bits)
- Total: 160 bits = 20 bytes = 8000 bps

## Repo
https://github.com/caminojc/tlcs (branch: lr-v2-experiment, commit 352bdd8)
