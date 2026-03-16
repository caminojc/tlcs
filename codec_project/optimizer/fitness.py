"""
Fitness function for black-box codec parameter optimization.

Evaluates a CodecConfig by:
  1. Encoding + decoding a batch of speech samples through the CELP codec
  2. Scoring the decoded outputs with a frozen perceptual evaluator
  3. Combining MOS score with bitrate and complexity penalties

The evaluator is injected at construction time and NEVER modified.
"""

from __future__ import annotations

import logging
import math
import os
import shutil
import tempfile
import traceback
from multiprocessing import Pool
from pathlib import Path
from typing import Any, Dict, List, Optional, Protocol, Tuple

import numpy as np

import sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from codec.config import CodecConfig
from codec.encoder import CELPEncoder
from codec.decoder import CELPDecoder

logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Protocols — the evaluator is injected, not imported
# ---------------------------------------------------------------------------

class FrozenEvaluator(Protocol):
    """
    A frozen perceptual speech quality evaluator.

    Accepts a list of (reference_wav, degraded_wav) path pairs and returns
    a list of predicted MOS scores (floats in roughly [1, 5]).
    The evaluator's weights are NEVER updated during optimisation.
    """

    def score(
        self, pairs: List[Tuple[str, str]]
    ) -> List[float]:
        ...


# ---------------------------------------------------------------------------
# Real codec wrapper — uses CELPEncoder + CELPDecoder
# ---------------------------------------------------------------------------

class CELPCodecWrapper:
    """
    Wraps the CELP encoder and decoder to provide a simple
    encode_decode(input_wav, output_wav, config) API.
    """

    def encode_decode(
        self, input_wav: str, output_wav: str, config: CodecConfig
    ) -> str:
        """
        Encode *input_wav* using *config*, then decode back to *output_wav*.
        Returns the path to the decoded output.
        """
        # Use a temporary file for the intermediate bitstream
        bitstream_path = output_wav + ".celp"
        try:
            encoder = CELPEncoder(config)
            encoder.encode_file(input_wav, bitstream_path)

            decoder = CELPDecoder(config)
            decoder.decode_file(bitstream_path, output_wav)
        finally:
            # Clean up intermediate bitstream
            if os.path.exists(bitstream_path):
                os.remove(bitstream_path)

        return output_wav


# ---------------------------------------------------------------------------
# Complexity estimator (heuristic, no actual profiling)
# ---------------------------------------------------------------------------

def _estimate_complexity(config: CodecConfig) -> float:
    """
    Heuristic estimate of per-frame CPU cost, normalised so the default
    config scores ~1.0.  Higher = more expensive.

    The factors mirror the dominant cost centres in a typical CELP encoder:
      - LPC analysis: O(lpc_order^2 * window_samples)
      - Adaptive codebook search: O(acb_num_pulses * subframe)
      - Codebook lookups scale with codebook size
      - Fractional-pitch interpolation adds a constant multiplier
    """
    # Base LPC cost (autocorrelation + Levinson-Durbin)
    lpc_cost = (config.lpc_order ** 2) * (config.lpc_window_size_ms / 30.0)

    # Adaptive codebook search
    acb_cost = config.acb_num_pulses * 10.0

    # Fixed codebook search (gain + LSP lookups)
    cb_cost = math.log2(config.gain_codebook_size) + math.log2(config.lsp_codebook_size)

    # Pitch interpolation multiplier
    pitch_mult = {"integer": 1.0, "half": 1.3, "third": 1.6}.get(
        config.pitch_fractional, 1.0
    )

    raw = (lpc_cost + acb_cost + cb_cost) * pitch_mult

    # Normalise so default config ≈ 1.0
    default_raw = (10**2 * 1.0 + 4 * 10.0 + math.log2(64) + math.log2(64)) * 1.6
    return raw / default_raw


def _estimate_bits_per_frame(config: CodecConfig) -> float:
    """
    Uses the codec's own bit budget computation for accuracy.
    Falls back to a rough heuristic if that fails.
    """
    try:
        bpf = config.compute_bits_per_frame()
        return float(bpf["total"])
    except Exception:
        # Rough fallback
        lsp_bits = math.log2(config.lsp_codebook_size) * config.lpc_order / 2.0
        pitch_bits = 8.0
        if config.pitch_fractional == "half":
            pitch_bits += 1.0
        elif config.pitch_fractional == "third":
            pitch_bits += 2.0
        gain_bits = config.pitch_gain_bits + math.log2(config.gain_codebook_size)
        pulse_bits = config.acb_num_pulses * (1 + 5)
        return lsp_bits + pitch_bits + gain_bits + pulse_bits


# ---------------------------------------------------------------------------
# FitnessFunction
# ---------------------------------------------------------------------------

class FitnessFunction:
    """
    Black-box fitness function for codec parameter optimisation.

    fitness = mean_mos
              - lambda_bitrate * bitrate_penalty
              - lambda_complexity * complexity_penalty

    bitrate_penalty = max(0, actual_bpf - target_bpf) / target_bpf
    complexity_penalty = estimated relative CPU ops per frame
    """

    def __init__(
        self,
        evaluator: FrozenEvaluator,
        codec: CELPCodecWrapper | None = None,
        speech_corpus: List[str] = (),
        target_bitrate: int = 8000,
        lambda_bitrate: float = 0.5,
        lambda_complexity: float = 0.1,
        num_samples: int = 20,
        tmp_dir: str = "/tmp/codec_eval",
        seed: int = 42,
    ):
        self._evaluator = evaluator
        self._codec = codec or CELPCodecWrapper()
        self._corpus = list(speech_corpus)
        self._target_bitrate = target_bitrate
        self._lambda_br = lambda_bitrate
        self._lambda_cx = lambda_complexity
        self._num_samples = min(num_samples, max(len(self._corpus), 1))
        self._tmp_dir = tmp_dir
        self._rng = np.random.RandomState(seed)
        self._eval_count = 0

        os.makedirs(self._tmp_dir, exist_ok=True)

    # -- public API ---------------------------------------------------------

    def evaluate(self, config: CodecConfig) -> float:
        """
        Evaluate a single CodecConfig.  Returns a scalar fitness value
        (higher is better).  Returns -inf for invalid / crashing configs.
        """
        self._eval_count += 1
        try:
            # Validate before spending CPU on encode/decode
            config.validate()
            return self._evaluate_impl(config)
        except Exception:
            logger.warning(
                "Evaluation failed for config %s:\n%s",
                config.summary(),
                traceback.format_exc(),
            )
            return float("-inf")

    def evaluate_batch(
        self,
        configs: List[CodecConfig],
        n_workers: int | None = None,
    ) -> List[float]:
        """Evaluate a batch of configs sequentially."""
        return [self.evaluate(cfg) for cfg in configs]

    @property
    def eval_count(self) -> int:
        return self._eval_count

    # -- internal -----------------------------------------------------------

    def _evaluate_impl(self, config: CodecConfig) -> float:
        # 1. Select a random subset of the corpus
        if not self._corpus:
            # No corpus — return fitness based purely on penalties
            est_bpf = _estimate_bits_per_frame(config)
            target_bpf = self._target_bitrate / 50.0
            bitrate_penalty = max(0.0, est_bpf - target_bpf) / target_bpf
            complexity_penalty = _estimate_complexity(config)
            return 3.0 - self._lambda_br * bitrate_penalty - self._lambda_cx * complexity_penalty

        indices = self._rng.choice(
            len(self._corpus), size=self._num_samples, replace=False
        )
        selected = [self._corpus[i] for i in indices]

        # 2. Encode + decode each sample
        eval_dir = tempfile.mkdtemp(dir=self._tmp_dir, prefix="eval_")
        try:
            pairs: List[Tuple[str, str]] = []
            for wav_path in selected:
                out_path = os.path.join(
                    eval_dir, Path(wav_path).stem + "_decoded.wav"
                )
                decoded = self._codec.encode_decode(wav_path, out_path, config)
                pairs.append((wav_path, decoded))

            # 3. Score with frozen evaluator
            mos_scores = self._evaluator.score(pairs)
            mean_mos = float(np.mean(mos_scores))

            # 4. Penalty terms
            est_bpf = _estimate_bits_per_frame(config)
            # Convert target bitrate (bps) to bits-per-frame
            frames_per_sec = 1000.0 / config.frame_size_ms
            target_bpf = self._target_bitrate / frames_per_sec
            bitrate_penalty = max(0.0, est_bpf - target_bpf) / target_bpf

            complexity_penalty = _estimate_complexity(config)

            fitness = (
                mean_mos
                - self._lambda_br * bitrate_penalty
                - self._lambda_cx * complexity_penalty
            )
            return fitness

        finally:
            # Clean up decoded files
            shutil.rmtree(eval_dir, ignore_errors=True)

    def _worker_evaluate(
        self, config: CodecConfig, worker_tmp: str
    ) -> float:
        """Entry point for each multiprocessing worker."""
        os.makedirs(worker_tmp, exist_ok=True)
        old_tmp = self._tmp_dir
        self._tmp_dir = worker_tmp
        try:
            return self.evaluate(config)
        finally:
            self._tmp_dir = old_tmp
            shutil.rmtree(worker_tmp, ignore_errors=True)
