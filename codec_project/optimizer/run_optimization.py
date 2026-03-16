#!/usr/bin/env python3
"""
CELP codec parameter optimisation using torchmetrics NISQA evaluator.

Usage:
  python optimizer/run_optimization.py \
      --corpus_dir /path/to/wavs \
      --target_bitrate 8000 \
      --max_generations 200 \
      --output_config optimizer/logs/best_config.json
"""
from __future__ import annotations

import argparse
import glob
import json
import logging
import os
import sys
import time
from pathlib import Path
from typing import List, Tuple

import numpy as np

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))


# ---------------------------------------------------------------------------
# NISQA evaluator adapter
# ---------------------------------------------------------------------------

class NISQAEvaluator:
    """Wraps torchmetrics NISQA to satisfy the FitnessFunction Protocol."""

    def __init__(self):
        import torch
        import torchaudio
        from torchmetrics.audio.nisqa import NonIntrusiveSpeechQualityAssessment
        self._nisqa = NonIntrusiveSpeechQualityAssessment(fs=16000)
        self._nisqa.eval()
        for p in self._nisqa.parameters():
            p.requires_grad = False

    def score(self, pairs: List[Tuple[str, str]]) -> List[float]:
        """Score degraded files from (ref, deg) pairs."""
        import torch
        import soundfile as sf
        import torchaudio.functional as AF

        scores = []
        for _ref, deg in pairs:
            audio, sr = sf.read(deg, dtype='float32')
            if audio.ndim > 1:
                audio = audio[:, 0]
            t = torch.tensor(audio)
            if sr != 16000:
                t = AF.resample(t, sr, 16000)
            self._nisqa.update(t.unsqueeze(0))
            result = self._nisqa.compute()
            self._nisqa.reset()
            scores.append(float(result[0].item()))
        return scores


# ---------------------------------------------------------------------------
# Progress display
# ---------------------------------------------------------------------------

def _print_header():
    print(
        f"{'Gen':>5} | {'Best MOS':>9} | {'Mean MOS':>9} | "
        f"{'Sigma':>8} | {'Evals':>7} | Best Config Summary"
    )
    print("-" * 100)


def _print_generation_line(log_entry: dict):
    print(
        f"{log_entry.get('generation', '?'):>5} | "
        f"{log_entry['best_fitness']:>9.4f} | "
        f"{log_entry.get('mean_fitness', 0):>9.4f} | "
        f"{log_entry.get('sigma', 0):>8.5f} | "
        f"{log_entry.get('total_evals', 0):>7} | "
        f"{log_entry.get('config_summary', '')}"
    )


class _LogTailer:
    def __init__(self, log_path: str):
        self._path = log_path
        self._pos = 0
        _print_header()

    def update(self):
        if not os.path.exists(self._path):
            return
        with open(self._path, "r") as f:
            f.seek(self._pos)
            for line in f:
                line = line.strip()
                if line:
                    try:
                        record = json.loads(line)
                        _print_generation_line(record)
                    except json.JSONDecodeError:
                        pass
            self._pos = f.tell()


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def parse_args(argv=None):
    p = argparse.ArgumentParser(
        description="CELP codec parameter optimisation (NISQA evaluator)",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("--corpus_dir", required=True, help="WAV files directory")
    p.add_argument("--target_bitrate", type=int, default=8000, choices=[4000, 8000])
    p.add_argument("--optimizer", default="cmaes", choices=["cmaes", "bayesian"])
    p.add_argument("--max_generations", type=int, default=200)
    p.add_argument("--population_size", type=int, default=None)
    p.add_argument("--num_samples", type=int, default=5, help="Speech samples per eval")
    p.add_argument("--lambda_bitrate", type=float, default=0.5)
    p.add_argument("--lambda_complexity", type=float, default=0.1)
    p.add_argument("--sigma0", type=float, default=0.3)
    p.add_argument("--resume", type=str, default=None)
    p.add_argument("--output_config", default="optimizer/best_config.json")
    p.add_argument("--checkpoint_dir", default="optimizer/checkpoints")
    p.add_argument("--log_dir", default="optimizer/logs")
    p.add_argument("--tmp_dir", default="/tmp/codec_eval")
    return p.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
    )
    logger = logging.getLogger("optimizer")

    wav_files = sorted(glob.glob(os.path.join(args.corpus_dir, "**", "*.wav"), recursive=True))
    if not wav_files:
        logger.error("No WAV files found in %s", args.corpus_dir)
        sys.exit(1)
    logger.info("Found %d WAV files in %s", len(wav_files), args.corpus_dir)

    logger.info("Initializing NISQA evaluator...")
    evaluator = NISQAEvaluator()
    logger.info("NISQA evaluator ready")

    from optimizer.fitness import CELPCodecWrapper, FitnessFunction

    fitness_fn = FitnessFunction(
        evaluator=evaluator,
        codec=CELPCodecWrapper(),
        speech_corpus=wav_files,
        target_bitrate=args.target_bitrate,
        lambda_bitrate=args.lambda_bitrate,
        lambda_complexity=args.lambda_complexity,
        num_samples=args.num_samples,
        tmp_dir=args.tmp_dir,
    )

    if args.optimizer == "cmaes":
        from optimizer.cmaes_optimizer import CodecCMAESOptimizer
        from optimizer.search_space import SearchSpaceEncoder

        space = SearchSpaceEncoder()

        opt = CodecCMAESOptimizer(
            fitness_fn=fitness_fn,
            search_space=space,
            sigma0=args.sigma0,
            population_size=args.population_size,
            max_generations=args.max_generations,
            checkpoint_dir=args.checkpoint_dir,
            log_dir=args.log_dir,
        )

        log_file = os.path.join(args.log_dir, "optimization_log.jsonl")
        tailer = _LogTailer(log_file)

        if args.resume:
            logger.info("Resuming from checkpoint %s", args.resume)
            best_config = opt.resume(args.resume)
        else:
            best_config = opt.run()

        tailer.update()

    elif args.optimizer == "bayesian":
        from optimizer.bayesian_optimizer import CodecBayesianOptimizer

        opt = CodecBayesianOptimizer(
            fitness_fn=fitness_fn,
            n_calls=args.max_generations,
            n_initial_points=min(20, args.max_generations // 3),
            log_dir=args.log_dir,
            checkpoint_dir=args.checkpoint_dir,
        )

        log_file = os.path.join(args.log_dir, "bayesian_log.jsonl")
        tailer = _LogTailer(log_file)
        best_config = opt.run()
        tailer.update()

    os.makedirs(os.path.dirname(args.output_config) or ".", exist_ok=True)
    with open(args.output_config, "w") as f:
        json.dump(best_config.to_dict(), f, indent=2)

    print("\n" + "=" * 60)
    print("OPTIMISATION COMPLETE")
    print("=" * 60)
    print(f"Best config saved to: {args.output_config}")
    print(f"Best config: {best_config.summary()}")
    print(f"Total fitness evaluations: {fitness_fn.eval_count}")
    print("=" * 60)


if __name__ == "__main__":
    main()
