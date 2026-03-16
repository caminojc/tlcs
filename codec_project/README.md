# CELP Codec Optimization System

Automated perceptual quality optimization for a pure-DSP CELP/ACELP speech codec
using black-box search (CMA-ES / Bayesian) guided by a WavLM-based MOS predictor.

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      pipeline/                              │
│   run_pipeline.py  ─── orchestrates stages 1→4              │
│   corpus.py        ─── NISQA / flat corpus loading          │
│   preprocess.py    ─── resample, loudness-norm, silence-rm  │
│   baseline_eval.py ─── encode→decode→score reporting        │
│   download_data.py ─── NISQA, LibriSpeech downloaders       │
└─────────┬──────────────────┬────────────────────────────────┘
          │                  │
          ▼                  ▼
┌──────────────────┐  ┌───────────────────────────────────────┐
│   evaluator/     │  │            optimizer/                  │
│                  │  │                                        │
│ model.py         │  │ search_space.py  ── param descriptors  │
│  WavLM + MOS     │  │ fitness.py       ── black-box fitness  │
│  head            │  │ cmaes_optimizer  ── CMA-ES loop        │
│                  │  │ bayesian_optim.  ── GP-based search     │
│ evaluator_api.py │  │ run_optimization ── standalone CLI      │
│  FrozenEvaluator │  │                                        │
│                  │  │ Protocols for evaluator & codec         │
│ train.py         │  │ (no hard imports from other modules)    │
│ validate.py      │  │                                        │
│ freeze.py        │  │                                        │
└──────────────────┘  └──────────────┬────────────────────────┘
                                     │
                                     ▼
                      ┌──────────────────────────┐
                      │        codec/             │
                      │                           │
                      │ config.py   ── full DSP   │
                      │ encoder.py  ── CELP enc   │
                      │ decoder.py  ── CELP dec   │
                      │ lpc.py      ── LPC anal.  │
                      │ pitch.py    ── pitch est.  │
                      │ quantization.py           │
                      │ algebraic_cb.py           │
                      │ postfilter.py             │
                      │ preprocessing.py          │
                      │ bitstream.py              │
                      └──────────────────────────┘
```

### Import Boundaries

| Module      | May import from          | Must NOT import from   |
|-------------|--------------------------|------------------------|
| `codec`     | stdlib, numpy, scipy     | evaluator, optimizer   |
| `evaluator` | stdlib, torch, torchaudio| codec, optimizer       |
| `optimizer` | codec*, evaluator*       | —                      |
| `pipeline`  | codec, evaluator, optimizer | —                   |

\* via **Protocol-based dependency injection** — no hard imports.

## Modules

### `codec/` — Pure-DSP CELP/ACELP Codec
Fully parameterised narrowband speech codec operating at 8 kHz.
Supports 4 kbps and 8 kbps bitrate targets.  No neural components at runtime.

### `evaluator/` — Perceptual Quality Predictor
WavLM backbone (frozen) with a trainable MOS-prediction head.
After training and validation (SRCC gate ≥ 0.85), the evaluator is frozen
into an immutable artifact that serves as the oracle fitness function.

### `optimizer/` — Black-Box Parameter Search
CMA-ES and Bayesian (GP-based) optimizers search the normalised [0,1]^d
parameter space.  Codec and evaluator are injected via Protocols.

### `pipeline/` — Data, Training, and Orchestration
Corpus management, preprocessing, evaluator training harness,
and the end-to-end `run_pipeline.py` that chains all four stages.

## Quickstart

```bash
# Install
pip install -e .

# Run the full pipeline (downloads data, trains evaluator, optimizes, evaluates)
python -m pipeline.run_pipeline --stage all --target_bitrate 8000

# Run individual stages
python -m pipeline.run_pipeline --stage data
python -m pipeline.run_pipeline --stage train_eval
python -m pipeline.run_pipeline --stage optimize --optimizer cmaes --max_generations 200
python -m pipeline.run_pipeline --stage evaluate

# Standalone optimization (with pre-frozen evaluator)
python -m optimizer.run_optimization \
    --evaluator_path evaluator/frozen_evaluator.pt \
    --corpus_dir data/corpus/processed \
    --target_bitrate 8000
```

## Configuration

Pre-built configs in `configs/`:

- `default_8kbps.json` — 8 kbps narrowband (default)
- `default_4kbps.json` — 4 kbps low-bitrate

## Design Principles

1. **Frozen evaluator** — the scoring surface is stationary during optimization,
   which is required for CMA-ES convergence guarantees.
2. **Protocol-based injection** — the optimizer never imports codec or evaluator
   directly; it depends on structural Protocols only.
3. **Pure DSP codec** — no neural components at codec runtime; all parameters
   are interpretable DSP knobs.
4. **Deterministic splits** — corpus splits and random seeds are fixed for
   reproducibility.
5. **Checkpoint-resume** — CMA-ES checkpoints every 10 generations; the pipeline
   skips completed stages on re-run.
