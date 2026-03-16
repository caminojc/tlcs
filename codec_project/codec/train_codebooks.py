"""
Codec VQ Codebook Training
==========================

Trains the LSP (SplitVQ) and gain (GainQuantizer) codebooks offline from a
speech corpus.  Must be run once before the codec can encode real speech with
meaningful quantization quality.

The codec works without this step (it falls back to linearly-spaced initial
codebooks) but quality will be noticeably worse.  Run this before starting
the CMA-ES optimization loop.

Usage
-----
    python -m codec.train_codebooks \
        --corpus_dir data/librispeech \
        --codebook_dir codec/codebooks \
        --config configs/default_8kbps.json \
        --max_frames 200000

Outputs
-------
    codec/codebooks/
        lsp_cb_split0.npy  ... lsp_cb_split{N}.npy   (SplitVQ per-split CBs)
        gain_cb.npy                                    (GainQuantizer CB)
        training_stats.json                            (distortion metrics)
"""
from __future__ import annotations

import argparse
import json
import logging
import os
import time
from pathlib import Path
from typing import List, Tuple

import numpy as np

logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Feature extraction helpers
# ---------------------------------------------------------------------------

def _collect_lsp_and_gains(
    wav_paths: List[str],
    config,
    max_frames: int = 200_000,
) -> Tuple[np.ndarray, np.ndarray]:
    """
    Run the pre-processing and LPC analysis stages on a corpus to collect
    raw (unquantized) LSP vectors and gain pairs.

    Returns
    -------
    lsp_data  : (N, lpc_order) float64
    gain_data : (N, 2) float64  columns = [pitch_gain, cb_gain]
    """
    from .preprocessing import HighPassFilter, PreEmphasisFilter
    from .lpc import lpc_analysis, lpc_to_lsp
    from .pitch import open_loop_pitch_estimate
    import soundfile as sf

    hp = HighPassFilter(config.highpass_cutoff_hz, config.sample_rate)
    pe = PreEmphasisFilter(config.preemphasis_coeff)

    N = config.frame_size
    P = config.lpc_order

    lsp_list: List[np.ndarray] = []
    gain_list: List[np.ndarray] = []

    n_frames = 0
    rng = np.random.RandomState(42)

    for wav_path in wav_paths:
        if n_frames >= max_frames:
            break
        try:
            audio, sr = sf.read(wav_path, dtype="float64")
        except Exception as e:
            logger.warning("Skipping %s: %s", wav_path, e)
            continue

        if audio.ndim > 1:
            audio = audio[:, 0]

        # Resample to codec sample rate if needed (simple linear interp)
        if sr != config.sample_rate:
            ratio = config.sample_rate / sr
            new_len = int(len(audio) * ratio)
            if new_len < N:
                continue
            x_old = np.linspace(0, 1, len(audio))
            x_new = np.linspace(0, 1, new_len)
            audio = np.interp(x_new, x_old, audio)

        # Pad to frame boundary
        pad = (N - len(audio) % N) % N
        if pad:
            audio = np.concatenate([audio, np.zeros(pad)])

        hp.reset()
        pe.reset()

        prev_lsp = np.linspace(0.1, np.pi - 0.1, P)

        for i in range(0, len(audio) - N + 1, N):
            if n_frames >= max_frames:
                break

            frame = audio[i: i + N]
            hp_frame = hp.process(frame)
            pe_frame = pe.process(hp_frame)

            lpc_coeffs, residual, lpc_gain = lpc_analysis(
                pe_frame, P,
                config.lpc_window_type,
                config.lpc_window_size,
            )

            lsp = _lsp_safe(lpc_coeffs, prev_lsp)
            lsp_list.append(lsp)
            prev_lsp = lsp

            # Estimate pitch gain via normalized open-loop correlation
            pitch_lag = open_loop_pitch_estimate(
                residual, config.pitch_min_lag, config.pitch_max_lag
            )
            pitch_gain = _estimate_pitch_gain(residual, pitch_lag)

            # Estimate codebook gain as RMS of residual
            cb_gain = float(np.sqrt(np.mean(residual ** 2)) + 1e-8)
            cb_gain = min(cb_gain, 3.0)  # clip outliers

            gain_list.append(np.array([pitch_gain, cb_gain]))
            n_frames += 1

    if not lsp_list:
        raise RuntimeError("No frames collected — check corpus_dir and sample rate.")

    lsp_data = np.stack(lsp_list)   # (N, P)
    gain_data = np.stack(gain_list) # (N, 2)

    logger.info(
        "Collected %d frames for codebook training  "
        "(LSP shape %s, gain shape %s)",
        n_frames, lsp_data.shape, gain_data.shape,
    )
    return lsp_data, gain_data


def _lsp_safe(lpc_coeffs: np.ndarray, fallback: np.ndarray) -> np.ndarray:
    """Convert LPC to LSP with fallback on failure."""
    try:
        from .lpc import lpc_to_lsp
        lsp = lpc_to_lsp(lpc_coeffs)
        # Enforce ordering and bounds
        lsp = np.clip(lsp, 0.01, np.pi - 0.01)
        for i in range(1, len(lsp)):
            if lsp[i] <= lsp[i - 1] + 0.05:
                lsp[i] = lsp[i - 1] + 0.05
        lsp = np.clip(lsp, 0.01, np.pi - 0.01)
        return lsp
    except Exception:
        return fallback.copy()


def _estimate_pitch_gain(residual: np.ndarray, pitch_lag: int) -> float:
    """Simple normalized cross-correlation pitch gain estimate."""
    if pitch_lag <= 0 or pitch_lag >= len(residual):
        return 0.0
    r = residual[pitch_lag:]
    r_lagged = residual[: len(r)]
    if len(r) == 0:
        return 0.0
    num = float(np.dot(r, r_lagged))
    den = float(np.sqrt(np.dot(r, r) * np.dot(r_lagged, r_lagged)) + 1e-10)
    return float(np.clip(num / den, 0.0, 1.2))


# ---------------------------------------------------------------------------
# Codebook quality metrics
# ---------------------------------------------------------------------------

def _vq_distortion(data: np.ndarray, codebook: np.ndarray) -> float:
    """Mean squared quantization distortion (normalized)."""
    dists = np.sum((data[:, np.newaxis, :] - codebook[np.newaxis, :, :]) ** 2, axis=2)
    min_dists = dists.min(axis=1)
    return float(min_dists.mean() / (np.var(data) + 1e-10))


# ---------------------------------------------------------------------------
# Main training function
# ---------------------------------------------------------------------------

def train_codebooks(
    corpus_dir: str,
    codebook_dir: str,
    config,
    max_frames: int = 200_000,
    kmeans_iter: int = 100,
    file_limit: int = 10_000,
) -> dict:
    """
    Train and save LSP and gain codebooks from a speech corpus.

    Parameters
    ----------
    corpus_dir   : directory containing .wav files (searched recursively)
    codebook_dir : where to save the trained .npy codebook files
    config       : CodecConfig instance
    max_frames   : max frames to collect (trade-off: quality vs speed)
    kmeans_iter  : K-means iterations (more = better, slower)
    file_limit   : max wav files to scan

    Returns
    -------
    dict with training statistics
    """
    from .quantization import SplitVQ, GainQuantizer

    codebook_dir = Path(codebook_dir)
    codebook_dir.mkdir(parents=True, exist_ok=True)

    # ── Collect wav paths ─────────────────────────────────────────────────
    wav_paths = sorted(Path(corpus_dir).rglob("*.wav"))[:file_limit]
    if not wav_paths:
        # Also try flac
        wav_paths = sorted(Path(corpus_dir).rglob("*.flac"))[:file_limit]
    if not wav_paths:
        raise FileNotFoundError(
            f"No .wav or .flac files found under {corpus_dir}"
        )
    logger.info("Found %d audio files for codebook training", len(wav_paths))

    # ── Extract features ──────────────────────────────────────────────────
    t0 = time.time()
    logger.info("Extracting LSP and gain features ...")
    lsp_data, gain_data = _collect_lsp_and_gains(
        [str(p) for p in wav_paths],
        config,
        max_frames=max_frames,
    )
    logger.info("Feature extraction: %.1fs", time.time() - t0)

    stats: dict = {
        "n_frames": int(lsp_data.shape[0]),
        "lsp_shape": list(lsp_data.shape),
        "gain_shape": list(gain_data.shape),
    }

    # ── Train LSP SplitVQ ─────────────────────────────────────────────────
    logger.info(
        "Training LSP SplitVQ  (splits=%d, codebook_size=%d) ...",
        config.lsp_num_splits, config.lsp_codebook_size,
    )
    t1 = time.time()
    lsp_vq = SplitVQ(
        codebook_size=config.lsp_codebook_size,
        num_splits=config.lsp_num_splits,
        dim=config.lpc_order,
    )
    lsp_vq.train(lsp_data, max_iter=kmeans_iter)
    lsp_vq.save(str(codebook_dir))
    logger.info("LSP VQ trained: %.1fs", time.time() - t1)

    # Measure distortion
    lsp_distortion = 0.0
    for s, cb in enumerate(lsp_vq.codebooks):
        offset = sum(lsp_vq.split_sizes[:s])
        sz = lsp_vq.split_sizes[s]
        sub_data = lsp_data[:, offset: offset + sz]
        lsp_distortion += _vq_distortion(sub_data, cb)
    lsp_distortion /= config.lsp_num_splits
    stats["lsp_normalized_distortion"] = round(lsp_distortion, 6)
    logger.info("LSP normalized distortion: %.4f", lsp_distortion)

    # ── Train Gain Quantizer ──────────────────────────────────────────────
    logger.info(
        "Training GainQuantizer (codebook_size=%d) ...",
        config.gain_codebook_size,
    )
    t2 = time.time()
    gain_vq = GainQuantizer(codebook_size=config.gain_codebook_size)
    gain_vq.train(gain_data, max_iter=kmeans_iter)
    gain_cb_path = str(codebook_dir / "gain_cb.npy")
    gain_vq.save(gain_cb_path)
    logger.info("Gain VQ trained: %.1fs", time.time() - t2)

    gain_distortion = _vq_distortion(gain_data, gain_vq.codebook)
    stats["gain_normalized_distortion"] = round(gain_distortion, 6)
    logger.info("Gain normalized distortion: %.4f", gain_distortion)

    # ── Save stats ────────────────────────────────────────────────────────
    stats["total_time_s"] = round(time.time() - t0, 1)
    stats_path = codebook_dir / "training_stats.json"
    with open(stats_path, "w") as f:
        json.dump(stats, f, indent=2)
    logger.info("Saved codebook training stats to %s", stats_path)
    logger.info(
        "Codebooks saved to %s  (lsp distortion=%.4f, gain distortion=%.4f)",
        codebook_dir, lsp_distortion, gain_distortion,
    )

    return stats


# ---------------------------------------------------------------------------
# Encoder/Decoder codebook loading helper
# ---------------------------------------------------------------------------

def load_codebooks_into_encoder(encoder, codebook_dir: str) -> bool:
    """
    Load trained codebooks into an already-constructed CELPEncoder.

    Returns True if loaded, False if codebooks not found (encoder keeps defaults).
    """
    cb_dir = Path(codebook_dir)
    lsp_found = (cb_dir / "lsp_cb_split0.npy").exists()
    gain_found = (cb_dir / "gain_cb.npy").exists()

    if lsp_found:
        encoder._lsp_vq.load(str(cb_dir))
        logger.info("Loaded LSP codebooks from %s", cb_dir)
    else:
        logger.warning(
            "LSP codebooks not found in %s — using default init. "
            "Run `python -m codec.train_codebooks` first.", cb_dir
        )

    if gain_found:
        encoder._gain_vq.load(str(cb_dir / "gain_cb.npy"))
        logger.info("Loaded gain codebook from %s", cb_dir)
    else:
        logger.warning(
            "Gain codebook not found in %s — using default init.", cb_dir
        )

    return lsp_found and gain_found


def load_codebooks_into_decoder(decoder, codebook_dir: str) -> bool:
    """Same as load_codebooks_into_encoder but for CELPDecoder."""
    cb_dir = Path(codebook_dir)
    lsp_found = (cb_dir / "lsp_cb_split0.npy").exists()
    gain_found = (cb_dir / "gain_cb.npy").exists()

    if lsp_found:
        decoder._lsp_vq.load(str(cb_dir))
    if gain_found:
        decoder._gain_vq.load(str(cb_dir / "gain_cb.npy"))

    return lsp_found and gain_found


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s: %(message)s",
    )

    parser = argparse.ArgumentParser(
        description="Train CELP VQ codebooks from a speech corpus.",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--corpus_dir", required=True,
        help="Root directory of .wav files (searched recursively)",
    )
    parser.add_argument(
        "--codebook_dir", default="codec/codebooks",
        help="Output directory for trained codebook .npy files",
    )
    parser.add_argument(
        "--config", default="configs/default_8kbps.json",
        help="Path to CodecConfig JSON file",
    )
    parser.add_argument(
        "--max_frames", type=int, default=200_000,
        help="Max frames to use for training (more = better quality, slower)",
    )
    parser.add_argument(
        "--kmeans_iter", type=int, default=100,
        help="K-means iterations per codebook",
    )
    parser.add_argument(
        "--file_limit", type=int, default=10_000,
        help="Max number of audio files to scan",
    )
    args = parser.parse_args()

    # Load config
    import sys
    sys.path.insert(0, str(Path(__file__).parent.parent))
    from codec.config import CodecConfig

    with open(args.config) as f:
        cfg = CodecConfig.from_dict(json.load(f))

    stats = train_codebooks(
        corpus_dir=args.corpus_dir,
        codebook_dir=args.codebook_dir,
        config=cfg,
        max_frames=args.max_frames,
        kmeans_iter=args.kmeans_iter,
        file_limit=args.file_limit,
    )

    print("\n── Codebook Training Complete ─────────────────────────────")
    print(f"  Frames used:            {stats['n_frames']:,}")
    print(f"  LSP distortion:         {stats['lsp_normalized_distortion']:.4f}")
    print(f"  Gain distortion:        {stats['gain_normalized_distortion']:.4f}")
    print(f"  Time:                   {stats['total_time_s']:.1f}s")
    print(f"  Codebooks saved to:     {args.codebook_dir}")
    print("──────────────────────────────────────────────────────────")
