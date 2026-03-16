#!/usr/bin/env python3
"""
SCOREQ-based parameter optimization for TLCS TCX codec.

Uses Optuna (TPE sampler) to search the TUNE_ parameter space.
Two phases:
  Phase 1: Decoder-only params (fast — encode once, decode many times)
  Phase 2: Encoder+decoder params (slower — re-encode each trial)

All TUNE_ params are passed via environment variables to avoid recompilation.

Usage:
  python3 optimize_params.py --phase 1 --n-trials 200
  python3 optimize_params.py --phase 2 --n-trials 100
  python3 optimize_params.py --phase both --n-trials 200
  python3 optimize_params.py --phase 1 --n-trials 500 --full-corpus
"""

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from concurrent.futures import ProcessPoolExecutor, as_completed

import numpy as np
import optuna

# Paths
TLCS_ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = TLCS_ROOT / "build"
ENC = BUILD_DIR / "tools" / "tlcs_enc"
DEC = BUILD_DIR / "tools" / "tlcs_dec"
CORPUS = Path("/Users/jonathanchristensen/CLionProjects/TLC/benchmark/corpus/speech")
SCOREQ_PYTHON = Path("/Users/jonathanchristensen/CLionProjects/TLC/benchmark/.venv/bin/python3")

# Quick set (balanced M/F, avoid files that fail)
QUICK_FILES = [
    "908-157963-0014.wav",   # M1
    "1284-1180-0032.wav",    # F2
    "1580-141083-0007.wav",  # M3
    "3575-170457-0021.wav",  # M4
    "5639-40744-0005.wav",   # F3
]

# Full 20-file corpus
FULL_FILES = sorted([f.name for f in CORPUS.glob("*.wav")])

BITRATE = 9600


def encode_file(src, dst, bitrate, env):
    """Encode a single file."""
    subprocess.run(
        [str(ENC), str(src), str(dst), str(bitrate)],
        env=env, capture_output=True, check=True
    )


def decode_file(tlcs_path, wav_path, env):
    """Decode a single file."""
    subprocess.run(
        [str(DEC), str(tlcs_path), str(wav_path)],
        env=env, capture_output=True, check=True
    )


def score_file_scoreq(wav_path):
    """Compute SCOREQ for a single decoded file."""
    result = subprocess.run(
        [str(SCOREQ_PYTHON), "-c",
         f"import scoreq; s=scoreq.Scoreq(data_domain='natural',mode='nr'); print(float(s.predict('{wav_path}')))"],
        capture_output=True, text=True, timeout=60
    )
    if result.returncode != 0:
        return 1.0  # bad score on failure
    # SCOREQ prints init messages before the score; grab last line
    lines = [l.strip() for l in result.stdout.strip().split('\n') if l.strip()]
    return float(lines[-1])


def encode_files(test_files, env_overrides=None, tmpdir=None):
    """Encode test files. Returns dict of {name: tlcs_path}."""
    encoded = {}
    env = os.environ.copy()
    if env_overrides:
        env.update({k: str(v) for k, v in env_overrides.items()})
    for f in test_files:
        src = CORPUS / f
        if not src.exists():
            continue
        name = f.replace(".wav", "")
        dst = Path(tmpdir) / f"{name}.tlcs"
        try:
            encode_file(src, dst, BITRATE, env)
            encoded[name] = dst
        except subprocess.CalledProcessError:
            pass
    return encoded


def decode_and_score(test_files, encoded_files, env_overrides=None, tmpdir=None):
    """Decode files and compute mean SCOREQ. Returns (mean, per_file_dict)."""
    env = os.environ.copy()
    if env_overrides:
        env.update({k: str(v) for k, v in env_overrides.items()})

    scores = {}
    for name, tlcs_path in encoded_files.items():
        dec_wav = Path(tmpdir) / f"{name}_dec.wav"
        try:
            decode_file(tlcs_path, dec_wav, env)
            score = score_file_scoreq(str(dec_wav))
            scores[name] = score
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
            scores[name] = 1.0
    if not scores:
        return 1.0, {}
    mean = np.mean(list(scores.values()))
    return mean, scores


def phase1_objective(trial, test_files, encoded_files, tmpdir):
    """Optimize decoder-only TUNE_ parameters (post-filters + noise fill + SBR)."""
    params = {
        # Noise fill
        "TUNE_NF_CODED": trial.suggest_float("nf_coded", 0.02, 0.20),
        "TUNE_NF_UNCODED_HI": trial.suggest_float("nf_uncoded_hi", 0.3, 1.0),
        "TUNE_NF_UNCODED_SLOPE": trial.suggest_float("nf_uncoded_slope", 0.0, 0.6),
        # SBR
        "TUNE_SBR_BASE_MULT": trial.suggest_float("sbr_base_mult", 0.4, 1.0),
        "TUNE_SBR_DEPTH_FRAC": trial.suggest_float("sbr_depth_frac", 0.2, 0.8),
        # Post-filter
        "TUNE_PF_NUM_LR": trial.suggest_float("pf_num_lr", 0.45, 0.85),
        "TUNE_PF_DEN_LR": trial.suggest_float("pf_den_lr", 0.60, 0.98),
        "TUNE_PF_TILT_LR": trial.suggest_float("pf_tilt_lr", 0.0, 0.55),
    }
    # Constraint: gamma_n < gamma_d
    if params["TUNE_PF_NUM_LR"] >= params["TUNE_PF_DEN_LR"]:
        return 1.0  # invalid

    mean_scoreq, per_file = decode_and_score(test_files, encoded_files, params, tmpdir)
    for k, v in per_file.items():
        trial.set_user_attr(k, v)
    return mean_scoreq


def phase2_objective(trial, test_files, tmpdir):
    """Optimize encoder TUNE_ params (ALFE) + decoder params jointly."""
    enc_params = {
        "TUNE_ALFE_BOOST": trial.suggest_float("alfe_boost", 1.0, 2.5),
    }
    dec_params = {
        "TUNE_ALFE_BOOST": enc_params["TUNE_ALFE_BOOST"],  # must match
        "TUNE_NF_CODED": trial.suggest_float("nf_coded", 0.02, 0.20),
        "TUNE_NF_UNCODED_HI": trial.suggest_float("nf_uncoded_hi", 0.3, 1.0),
        "TUNE_NF_UNCODED_SLOPE": trial.suggest_float("nf_uncoded_slope", 0.0, 0.6),
        "TUNE_SBR_BASE_MULT": trial.suggest_float("sbr_base_mult", 0.4, 1.0),
        "TUNE_SBR_DEPTH_FRAC": trial.suggest_float("sbr_depth_frac", 0.2, 0.8),
        "TUNE_PF_NUM_LR": trial.suggest_float("pf_num_lr", 0.45, 0.85),
        "TUNE_PF_DEN_LR": trial.suggest_float("pf_den_lr", 0.60, 0.98),
        "TUNE_PF_TILT_LR": trial.suggest_float("pf_tilt_lr", 0.0, 0.55),
    }
    if dec_params["TUNE_PF_NUM_LR"] >= dec_params["TUNE_PF_DEN_LR"]:
        return 1.0

    encoded = encode_files(test_files, enc_params, tmpdir)
    all_params = {**enc_params, **dec_params}
    mean_scoreq, per_file = decode_and_score(test_files, encoded, all_params, tmpdir)
    for k, v in per_file.items():
        trial.set_user_attr(k, v)
    return mean_scoreq


def run_phase1(n_trials, test_files):
    print("=" * 60)
    print(f"Phase 1: Decoder-only optimization ({len(test_files)} files, SCOREQ)")
    print("=" * 60)

    with tempfile.TemporaryDirectory() as tmpdir:
        print("Encoding test files with default encoder params...")
        encoded = encode_files(test_files, tmpdir=tmpdir)
        print(f"  Encoded {len(encoded)} files")

        baseline_mean, baseline_scores = decode_and_score(
            test_files, encoded, tmpdir=tmpdir)
        print(f"  Baseline SCOREQ: {baseline_mean:.3f}")
        for k, v in sorted(baseline_scores.items(), key=lambda x: x[1]):
            print(f"    {k}: {v:.3f}")

        study = optuna.create_study(
            direction="maximize",
            sampler=optuna.samplers.TPESampler(seed=42),
            study_name="tlcs_tcx_decoder_scoreq"
        )

        # Seed with current defaults
        study.enqueue_trial({
            "nf_coded": 0.08,
            "nf_uncoded_hi": 0.70,
            "nf_uncoded_slope": 0.30,
            "sbr_base_mult": 0.90,
            "sbr_depth_frac": 0.50,
            "pf_num_lr": 0.65,
            "pf_den_lr": 0.80,
            "pf_tilt_lr": 0.30,
        })

        def objective(trial):
            return phase1_objective(trial, test_files, encoded, tmpdir)

        study.optimize(objective, n_trials=n_trials, show_progress_bar=True)

        print("\n" + "=" * 60)
        print("Phase 1 Results")
        print("=" * 60)
        print(f"Best SCOREQ: {study.best_value:.4f} (baseline: {baseline_mean:.4f}, "
              f"delta: {study.best_value - baseline_mean:+.4f})")
        print("Best params:")
        for k, v in sorted(study.best_params.items()):
            print(f"  {k}: {v:.4f}")
        print("Per-file scores:")
        attrs = sorted(study.best_trial.user_attrs.items(), key=lambda x: x[1])
        for k, v in attrs:
            print(f"  {k}: {v:.3f}")

        # Print tune.h update
        param_map = {
            "nf_coded": "TUNE_NF_CODED",
            "nf_uncoded_hi": "TUNE_NF_UNCODED_HI",
            "nf_uncoded_slope": "TUNE_NF_UNCODED_SLOPE",
            "sbr_base_mult": "TUNE_SBR_BASE_MULT",
            "sbr_depth_frac": "TUNE_SBR_DEPTH_FRAC",
            "pf_num_lr": "TUNE_PF_NUM_LR",
            "pf_den_lr": "TUNE_PF_DEN_LR",
            "pf_tilt_lr": "TUNE_PF_TILT_LR",
        }
        print("\nSuggested tune.h defaults:")
        for short_name, tune_name in sorted(param_map.items()):
            val = study.best_params.get(short_name)
            if val is not None:
                print(f"  {tune_name}: {val:.4f}")

    return study


def run_phase2(n_trials, test_files, phase1_best=None):
    print("\n" + "=" * 60)
    print(f"Phase 2: Joint encoder+decoder optimization ({len(test_files)} files, SCOREQ)")
    print("=" * 60)

    with tempfile.TemporaryDirectory() as tmpdir:
        study = optuna.create_study(
            direction="maximize",
            sampler=optuna.samplers.TPESampler(seed=42),
            study_name="tlcs_tcx_joint_scoreq"
        )

        # Seed: current optimized defaults
        seed = {
            "alfe_boost": 1.60,
            "nf_coded": 0.08,
            "nf_uncoded_hi": 0.70,
            "nf_uncoded_slope": 0.30,
            "sbr_base_mult": 0.90,
            "sbr_depth_frac": 0.50,
            "pf_num_lr": 0.65,
            "pf_den_lr": 0.80,
            "pf_tilt_lr": 0.30,
        }
        if phase1_best:
            seed.update(phase1_best)
        study.enqueue_trial(seed)

        def objective(trial):
            return phase2_objective(trial, test_files, tmpdir)

        study.optimize(objective, n_trials=n_trials, show_progress_bar=True)

        print("\n" + "=" * 60)
        print("Phase 2 Results")
        print("=" * 60)
        print(f"Best SCOREQ: {study.best_value:.4f}")
        print("Best params:")
        for k, v in sorted(study.best_params.items()):
            print(f"  {k}: {v:.4f}")
        print("Per-file scores:")
        attrs = sorted(study.best_trial.user_attrs.items(), key=lambda x: x[1])
        for k, v in attrs:
            print(f"  {k}: {v:.3f}")

    return study


def main():
    parser = argparse.ArgumentParser(description="TLCS TCX SCOREQ parameter optimizer")
    parser.add_argument("--phase", choices=["1", "2", "both"], default="both")
    parser.add_argument("--n-trials", type=int, default=200)
    parser.add_argument("--full-corpus", action="store_true",
                        help="Use all 20 speech files (slower but more robust)")
    parser.add_argument("--bitrate", type=int, default=9600)
    args = parser.parse_args()

    global BITRATE
    BITRATE = args.bitrate

    test_files = FULL_FILES if args.full_corpus else QUICK_FILES
    print(f"Using {len(test_files)} test files, bitrate={BITRATE}")

    optuna.logging.set_verbosity(optuna.logging.WARNING)

    phase1_best = None
    if args.phase in ("1", "both"):
        study1 = run_phase1(args.n_trials, test_files)
        phase1_best = study1.best_params

    if args.phase in ("2", "both"):
        run_phase2(args.n_trials, test_files, phase1_best)


if __name__ == "__main__":
    main()
