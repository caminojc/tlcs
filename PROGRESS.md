# TLCS Progress — Updated 2026-03-15

## What Was Built

### TCX Engine (lr-v2-experiment, working)
- Hybrid LP+TCX at 5k/9.6k/25k bitrates
- SCOREQ: 3.36 (5k), 3.60 (9.6k), 4.04 (25k)
- ALFE through F1+F2, perceptual distortion weighting, adaptive rc_mult
- 42/42 tests passing

### CELP v1 Engine (tlcs_v1/, work in progress)
- 16 kHz wideband, 8 kbps, 160 bits/frame exact
- LPC-16 + split VQ (4×256) + fractional pitch
- 3-pulse algebraic CB (5 tracks) — upgraded from 2 pulses
- Perceptual weighting W(z), harmonic PF, pitch sharpening, shaped noise fill
- All constants in `tlcs_v1/src/tlcs_config.h`
- **NISQA: ~0.67 (broken — see failures below)**

### Optimizer Tooling
- CMA-ES optimizer on DGX (24 params, pop 20, 200 gens)
- SMPL CELP at gen 31: **SCOREQ 3.979** (stock 3.88, EVS 4.17)
- Steroids optimizer running with expanded param set
- Blind listening test framework (50 configs, A/B, MOS rating)

### Human Preference Data
- 68 blind ratings collected across 3 rounds
- Key finding: less postfilter = more natural (PF_DEN 0.48 preferred over 0.80)
- SCOREQ diverges from human perception by ~1.5 MOS points on TLCS

## Current Scores

| Codec | SCOREQ | Human MOS | Bitrate |
|-------|--------|-----------|---------|
| EVS 9.6k | 4.17 | 4.0 | 9.6 kbps |
| SMPL 8k (CMA-ES opt) | **3.98** | — | 8.5 kbps |
| TLCS TCX 25k | 4.04 | — | 24.3 kbps |
| SMPL 8k (stock) | 3.88 | 3.7 | 8.5 kbps |
| TLCS TCX 9.6k | 3.60 | 2.0 | 11.4 kbps |
| SMPL 5k (stock) | 3.24 | 3.3 | 6.0 kbps |
| TLCS TCX 5k | 3.36 | 1.7 | 8.7 kbps |
| **TLCS CELP v1** | **~0.67** | **unintelligible** | 8.0 kbps |

## What Failed and Why

### TLCS CELP v1: 0.67 NISQA (should be >2.5)
**Root cause:** Gain calibration mismatch between encoder and decoder.
- Encoder computes gains in perceptually-weighted domain (~0.003)
- Decoder synthesizes in unweighted domain
- LPC synthesis filter has gain ~20000 but FCB excitation is near-zero
- Fix applied (unweighted gain recomputation) but still only 1.4 dB SNR
- LPC analysis-synthesis alone gives 10.1 dB → proves LPC path works
- **The 3 pulses in 80 samples are structurally insufficient for energy**

### Proxy evaluators diverge from SCOREQ
- NISQA optimized params degraded SCOREQ by 0.03
- WavLM trained evaluator (SRCC 0.95) overfitted on postfilter — SCOREQ regressed 0.20
- Human ratings diverge from SCOREQ by 1.5 MOS on TLCS TCX

### Time-domain LTP broke MDCT overlap-add
- Reverted. Needs MDCT-domain approach.

### Neural post-filter degraded SCOREQ
- L1+spectral loss: -0.77 SCOREQ
- SCOREQ-optimized spectral filter: +0.005 (negligible)

## Exact Next Step

**Fix TLCS CELP v1 gain to produce intelligible speech (NISQA > 2.0).**

The gain from `tlcs_acb_search()` is ~0.003 but the synthesis filter needs ~0.05-0.5 to produce audible output. The issue is the analysis-by-synthesis loop operates in the weighted domain where gains are tiny, but the decoder needs unweighted gains.

Specific fix needed in `tlcs_v1/src/tlcs_encoder.c`:
1. After FCB search, recompute gain using unweighted impulse response (already attempted but not working)
2. OR: scale gain by `sqrt(target_energy / filtered_excitation_energy)` as an energy normalization step
3. Verify with: encode → decode → measure RMS ratio (should be 0.8-1.2, currently 0.04)

Once NISQA > 2.0, run the DGX optimizer on TLCS v1 params.

## DGX Status
- Steroids optimizer: running, 24 params, ~gen 5 of 200
- SMPL best so far: SCOREQ 3.979 (16 optimized params saved)

## Repo
https://github.com/caminojc/tlcs (branch: lr-v2-experiment)
