"""
Audio preprocessing for the CELP codec optimization pipeline.

Handles resampling, loudness normalization, silence removal,
and batch PESQ computation for baselines.
"""

from __future__ import annotations

import logging
import os
from pathlib import Path
from typing import List, Optional, Tuple

import numpy as np
import soundfile as sf

logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Loudness / normalization utilities
# ---------------------------------------------------------------------------

def _rms(signal: np.ndarray) -> float:
    """Root mean square of a signal."""
    return float(np.sqrt(np.mean(signal ** 2)))


def _lufs_approx(signal: np.ndarray, sr: int) -> float:
    """
    Approximate integrated LUFS using RMS (simplified).

    A proper LUFS measurement uses ITU-R BS.1770 K-weighting, but for
    relative normalization within a pipeline this RMS-based approximation
    is sufficient and avoids heavy dependencies.
    """
    rms_val = _rms(signal)
    if rms_val < 1e-10:
        return -100.0
    # LUFS ≈ -0.691 + 20*log10(rms) for full-scale sinusoid reference
    return -0.691 + 20.0 * np.log10(rms_val)


def _normalize_loudness(
    signal: np.ndarray,
    sr: int,
    target_lufs: float = -23.0,
) -> np.ndarray:
    """Scale signal to approximate target LUFS."""
    current = _lufs_approx(signal, sr)
    if current < -80.0:
        # Signal is essentially silence
        return signal
    gain_db = target_lufs - current
    gain_linear = 10.0 ** (gain_db / 20.0)
    scaled = signal * gain_linear
    # Soft-clip to prevent overflow
    peak = np.max(np.abs(scaled))
    if peak > 0.99:
        scaled = scaled * (0.99 / peak)
    return scaled


def _strip_silence(
    signal: np.ndarray,
    sr: int,
    threshold_db: float = -40.0,
    frame_ms: float = 10.0,
) -> np.ndarray:
    """Strip leading and trailing silence from a signal."""
    frame_len = int(sr * frame_ms / 1000.0)
    threshold_linear = 10.0 ** (threshold_db / 20.0)

    num_frames = len(signal) // frame_len
    if num_frames == 0:
        return signal

    # Compute per-frame energy
    energies = np.array([
        _rms(signal[i * frame_len : (i + 1) * frame_len])
        for i in range(num_frames)
    ])

    # Find first and last frame above threshold
    active = np.where(energies > threshold_linear)[0]
    if len(active) == 0:
        return signal  # All silence — return as-is

    start_sample = active[0] * frame_len
    end_sample = min((active[-1] + 1) * frame_len, len(signal))
    return signal[start_sample:end_sample]


# ---------------------------------------------------------------------------
# Main preprocessing function
# ---------------------------------------------------------------------------

def normalize_corpus(
    input_dir: str,
    output_dir: str,
    target_sr: int = 8000,
    target_duration_s: Optional[float] = None,
    normalize_loudness: bool = True,
    target_lufs: float = -23.0,
    remove_silence: bool = True,
    min_duration_s: float = 1.0,
) -> dict:
    """
    Preprocess a corpus for use in codec optimization.

    Outputs clean, normalized wavs ready for encode/decode cycles.

    Parameters
    ----------
    input_dir : str
        Directory with source wav files (searched recursively).
    output_dir : str
        Destination for preprocessed wavs (flat structure).
    target_sr : int
        Target sample rate. Files are resampled if needed.
    target_duration_s : float or None
        If set, truncate/pad files to this duration. None keeps original.
    normalize_loudness : bool
        If True, normalize all files to target_lufs.
    target_lufs : float
        Target loudness in LUFS (approximate).
    remove_silence : bool
        Strip leading/trailing silence.
    min_duration_s : float
        Discard files shorter than this (after silence removal).

    Returns
    -------
    dict
        Statistics: processed, skipped, errors, total_duration_s.
    """
    input_path = Path(input_dir)
    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)

    stats = {"processed": 0, "skipped": 0, "errors": 0, "total_duration_s": 0.0}

    wav_files = sorted(
        list(input_path.glob("**/*.wav"))
        + list(input_path.glob("**/*.flac"))
        + list(input_path.glob("**/*.mp3"))
    )

    if not wav_files:
        logger.warning("No audio files found in %s", input_dir)
        return stats

    for wav_file in wav_files:
        try:
            signal, sr = sf.read(str(wav_file), dtype="float64")

            # Mix to mono
            if signal.ndim > 1:
                signal = signal.mean(axis=1)

            # Resample if needed
            if sr != target_sr:
                signal = _resample(signal, sr, target_sr)
                sr = target_sr

            # Remove silence
            if remove_silence:
                signal = _strip_silence(signal, sr)

            # Check minimum duration
            duration = len(signal) / sr
            if duration < min_duration_s:
                stats["skipped"] += 1
                continue

            # Truncate or pad to target duration
            if target_duration_s is not None:
                target_len = int(target_sr * target_duration_s)
                if len(signal) > target_len:
                    signal = signal[:target_len]
                elif len(signal) < target_len:
                    signal = np.pad(signal, (0, target_len - len(signal)))

            # Normalize loudness
            if normalize_loudness:
                signal = _normalize_loudness(signal, sr, target_lufs)

            # Write output (flat naming: stem is preserved, collisions get suffix)
            out_name = wav_file.stem + ".wav"
            out_path = output_path / out_name
            if out_path.exists():
                out_name = f"{wav_file.stem}_{stats['processed']:04d}.wav"
                out_path = output_path / out_name

            sf.write(str(out_path), signal, sr, subtype="PCM_16")
            stats["processed"] += 1
            stats["total_duration_s"] += len(signal) / sr

        except Exception as e:
            logger.warning("Error processing %s: %s", wav_file, e)
            stats["errors"] += 1

    logger.info(
        "Preprocessing complete: %d processed, %d skipped, %d errors",
        stats["processed"], stats["skipped"], stats["errors"],
    )
    return stats


def _resample(signal: np.ndarray, orig_sr: int, target_sr: int) -> np.ndarray:
    """Resample using scipy (polyphase if possible, else linear interpolation)."""
    if orig_sr == target_sr:
        return signal
    try:
        from scipy.signal import resample_poly
        import math
        gcd = math.gcd(orig_sr, target_sr)
        up = target_sr // gcd
        down = orig_sr // gcd
        return resample_poly(signal, up, down)
    except ImportError:
        # Fallback: linear interpolation
        target_len = int(len(signal) * target_sr / orig_sr)
        x_old = np.linspace(0, 1, len(signal))
        x_new = np.linspace(0, 1, target_len)
        return np.interp(x_new, x_old, signal)


# ---------------------------------------------------------------------------
# PESQ baseline computation
# ---------------------------------------------------------------------------

def compute_pesq_scores(
    degraded_paths: List[str],
    reference_paths: List[str],
    mode: str = "wb",
) -> List[Optional[float]]:
    """
    Batch PESQ computation for baseline comparison.

    Parameters
    ----------
    degraded_paths : list of str
        Paths to degraded (codec output) wav files.
    reference_paths : list of str
        Paths to reference (original) wav files.
    mode : str
        "wb" for wideband (16kHz) or "nb" for narrowband (8kHz).

    Returns
    -------
    list of float or None
        PESQ scores. None for files that failed.
    """
    try:
        from pesq import pesq
    except ImportError:
        logger.error(
            "pesq package not installed. Run: pip install pesq"
        )
        return [None] * len(degraded_paths)

    sr = 16000 if mode == "wb" else 8000
    scores: List[Optional[float]] = []

    for ref_path, deg_path in zip(reference_paths, degraded_paths):
        try:
            ref, ref_sr = sf.read(ref_path, dtype="float64")
            deg, deg_sr = sf.read(deg_path, dtype="float64")

            # Mix to mono
            if ref.ndim > 1:
                ref = ref.mean(axis=1)
            if deg.ndim > 1:
                deg = deg.mean(axis=1)

            # Resample if needed
            if ref_sr != sr:
                ref = _resample(ref, ref_sr, sr)
            if deg_sr != sr:
                deg = _resample(deg, deg_sr, sr)

            # Align lengths
            min_len = min(len(ref), len(deg))
            ref = ref[:min_len]
            deg = deg[:min_len]

            # Scale to int16 range for PESQ
            ref_16 = (ref * 32768).astype(np.float32)
            deg_16 = (deg * 32768).astype(np.float32)

            score = pesq(sr, ref_16, deg_16, mode)
            scores.append(float(score))
        except Exception as e:
            logger.warning("PESQ failed for %s: %s", deg_path, e)
            scores.append(None)

    return scores
