# TLCS — Speech Codec

Production speech codec targeting VoIP/RTC at 4–8 kbps.

## Design Principles

- **Zero ML at runtime** — pure DSP encode/decode, no neural inference
- **ML guides, DSP runs** — ML used offline for parameter optimization and quality evaluation
- **Human preference is ground truth** — not PESQ, SCOREQ, or VISQOL

## Architecture

### TCX Engine (`src/codec/`) — Proven, SCOREQ 3.60 at 9.6k
Hybrid LP+TCX transform codec on `lr-v2-experiment` branch.
- MDCT-based spectral quantization with range coding
- ALFE (Adaptive Low-Frequency Emphasis) through F1+F2
- Perceptual distortion weighting in step selection
- Adaptive SBR (Spectral Band Replication)
- Formant postfilter

### CELP v1 Engine (`tlcs_v1/`) — Work in Progress
Ground-up wideband CELP codec, 16 kHz, 8 kbps.
- LPC order 16, split-VQ (4×256), bandwidth expansion
- Fractional pitch (1/3 sample), 2-pulse algebraic codebook
- Perceptual weighting filter W(z) = A(z/γ₁) / A(z/γ₂)
- Harmonic + formant postfilter, pitch sharpening, shaped noise fill
- All tunable constants in `tlcs_v1/src/tlcs_config.h`

### Python Skeleton (`codec_project/codec/`) — Reference
Pure-Python CELP implementation for prototyping. 16/16 tests passing.

### Optimizer Tooling (`tools/`)
- `smpl_steroids.py` — CMA-ES optimization with SCOREQ (24 params, DGX GPU)
- `gpu_sweep.py` — parameter sweep with NISQA/SCOREQ
- Blind listening test builders for human preference data

## Build

```bash
# TCX engine
export PATH="/Applications/CLion.app/Contents/bin/cmake/mac/aarch64/bin:$PATH"
cmake -B build && cmake --build build
./build/tests/test_scaffold  # 42 tests

# CELP v1
cd tlcs_v1
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/tlcs_demo input.wav output.wav
```

## Benchmark Results (2026-03-15)

| Codec | SCOREQ | Bitrate |
|-------|--------|---------|
| EVS 9.6k | 4.17 | 9.6 kbps |
| SMPL 8k (CMA-ES optimized) | **3.98** | 8.5 kbps |
| **TLCS TCX 25k** | **4.04** | 24.3 kbps |
| SMPL 8k (stock) | 3.88 | 8.5 kbps |
| **TLCS TCX 9.6k** | **3.60** | 11.4 kbps |
| **TLCS TCX 5k** | **3.36** | 8.7 kbps |
| SMPL 5k | 3.24 | 6.0 kbps |

## DGX Optimization

CMA-ES running on NVIDIA GB10 DGX (20 cores, CUDA 13.0):
- 24 parameters searched simultaneously
- Population 20, 200 generations
- SCOREQ ground truth on 20-file corpus at 5k + 8k bitrates
- Warm-started from gen-31 best (SCOREQ 3.979)

## License

Proprietary. All rights reserved.
