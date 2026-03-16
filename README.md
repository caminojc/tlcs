# TLC — Dual-Mode Speech Codec

Production speech codec with two modes for CPU/quality trade-offs. 100% original C.

## Modes

| | CELP (Max Quality) | TCX (Low CPU) |
|---|---|---|
| **How** | Time-domain pulse excitation | MDCT frequency-domain |
| **Sound** | Dry, crisp | Smooth, slight reverb |
| **Decode CPU** | 0.04% RT | 0.7% RT (20x faster) |
| **Best at** | 9.6k+ (enough bits for pulses) | 5k (beats SMPL), 25k |

## Quality (SCOREQ, 20-file corpus)

| Codec | 5k | 9.6k | 25k |
|-------|-----|------|-----|
| **TLC TCX** | **3.52** | 3.75 | 3.98 |
| **TLC CELP** | — | tuning | — |
| SMPL | 3.24 | 3.88 | — |
| EVS | — | — | 4.17 |

## Usage

```bash
cmake -B build && cmake --build build

# TCX (default)
./build/tools/tlcs_enc input.wav output.tlcs 9600
./build/tools/tlcs_dec output.tlcs decoded.wav

# CELP
TLCS_MODE=celp ./build/tools/tlcs_enc input.wav output.tlcs 9600
TLCS_MODE=celp ./build/tools/tlcs_dec output.tlcs decoded.wav
```

## Runtime Tuning

All params overridable via env vars — no recompile. See `src/codec/tlcs_tune.h`.

## Eval

- **Eval page**: https://caminojc.github.io/tlcs/
- **Blind A/B**: https://caminojc.github.io/tlcs/blind_ab/
- **All codecs**: https://caminojc.github.io/tlcs/all_codecs.html
