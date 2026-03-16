# TLCS Progress — Updated 2026-03-15 (late session)

## Latest Changes
- Joint gain optimization in unweighted domain (2×2 normal equations)
- RMS ratio: 0.04 → **0.47** (12× improvement in level matching)
- NISQA: 0.67 → **0.72** (still below 2.0 target)
- Removed energy compensation hack from FCB search
- 3 pulses / 5 tracks / 160 bits exact

## Current NISQA Score: 0.72 (target: >2.0)

## What Works
- Bit budget: 160 bits exact ✓
- LPC analysis-synthesis: 10.1 dB SNR ✓  
- Joint gain: computes gains in unweighted domain ✓
- RMS ratio: 0.47 (was 0.04) ✓
- All DSP features enabled: perc weighting, harmonic PF, pitch sharpening, noise fill

## What's Still Broken
- NISQA 0.72 < 2.0 target
- Correlation 0.53 (low — FCB positions found in weighted domain may be suboptimal for unweighted reconstruction)
- SNR 1.4 dB (should be 5-10 for intelligible CELP)

## Exact Next Step
Investigate why weighted-domain FCB pulse positions produce poor unweighted reconstruction. Consider:
1. The weighted IR has very different shape than unweighted IR — positions optimal for one are poor for the other
2. May need to search in unweighted domain directly and accept worse perceptual shaping
3. Or use a two-pass approach: search weighted, refine positions in unweighted

## DGX Status
- SMPL steroids optimizer: running (24 params, gen ~5 of 200)
- Best SMPL params: SCOREQ 3.979 (stock 3.88, EVS 4.17)

## Repo
https://github.com/caminojc/tlcs (branch: lr-v2-experiment)
