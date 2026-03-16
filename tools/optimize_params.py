#!/usr/bin/env python3
"""
Bayesian parameter optimization for TLCS LR codec.

Uses Optuna (TPE sampler) to search the parameter space with PESQ as
the objective function. Two phases:
  Phase 1: Decoder-only params (fast — encode once, decode many times)
  Phase 2: Encoder+decoder params (slower — re-encode each trial)

Usage:
  python3 optimize_params.py --phase 1 --n-trials 100
  python3 optimize_params.py --phase 2 --n-trials 50
  python3 optimize_params.py --phase both --n-trials 100
  python3 optimize_params.py --phase 2 --n-trials 200 --full-corpus
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
from pesq import pesq
import soundfile as sf

# Paths
TLCS_ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = TLCS_ROOT / "build"
ENC = BUILD_DIR / "tools" / "tlcs_enc"
DEC = BUILD_DIR / "tools" / "tlcs_dec"
CORPUS = Path("/Users/jonathanchristensen/CLionProjects/TLC/benchmark/corpus/speech")

# 3-file quick set (balanced M/F)
QUICK_FILES = [
    "908-157963-0014.wav",   # M1
    "237-134500-0022.wav",   # F1
    "1284-1180-0032.wav",    # F2
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


def score_file(ref_path, dec_path):
    """Compute PESQ for a single file pair."""
    ref, sr = sf.read(str(ref_path))
    deg, _ = sf.read(str(dec_path))
    mn = min(len(ref), len(deg))
    return pesq(sr, ref[:mn], deg[:mn], 'wb')


def encode_files(test_files, env_overrides=None, tmpdir=None):
    """Encode test files. Returns dict of {name: tlcs_path}."""
    encoded = {}
    env = os.environ.copy()
    if env_overrides:
        env.update({k: str(v) for k, v in env_overrides.items()})
    for f in test_files:
        src = CORPUS / f
        name = f.replace(".wav", "")
        dst = Path(tmpdir) / f"{name}.tlcs"
        encode_file(src, dst, BITRATE, env)
        encoded[name] = dst
    return encoded


def decode_and_score(test_files, encoded_files, env_overrides=None, tmpdir=None):
    """Decode files and compute mean PESQ. Returns (mean, per_file_dict)."""
    env = os.environ.copy()
    if env_overrides:
        env.update({k: str(v) for k, v in env_overrides.items()})

    scores = {}
    for name, tlcs_path in encoded_files.items():
        dec_wav = Path(tmpdir) / f"{name}_dec.wav"
        decode_file(tlcs_path, dec_wav, env)
        ref_file = CORPUS / f"{name}.wav"
        score = score_file(ref_file, dec_wav)
        scores[name] = score
    mean = np.mean(list(scores.values()))
    return mean, scores


def phase1_objective(trial, test_files, encoded_files, tmpdir):
    """Optimize decoder-only parameters (post-filters + noise fill/tilt)."""
    params = {
        "TLCS_HARM_STRENGTH": trial.suggest_float("harm_strength", 0.0, 0.60),
        "TLCS_FPF_GN": trial.suggest_float("fpf_gn", 0.65, 0.95),
        "TLCS_FPF_GD": trial.suggest_float("fpf_gd", 0.80, 0.99),
        "TLCS_FPF_TILT": trial.suggest_float("fpf_tilt", 0.0, 0.60),
        "TLCS_BASS_ALPHA": trial.suggest_float("bass_alpha", 0.0, 0.30),
        "TLCS_NF_SCALE": trial.suggest_float("nf_scale", 0.0, 1.5),
        "TLCS_TILT_ALPHA": trial.suggest_float("tilt_alpha", 0.0, 0.25),
    }
    # Constraint: gamma_n < gamma_d
    if params["TLCS_FPF_GN"] >= params["TLCS_FPF_GD"]:
        return 0.0  # invalid

    mean_pesq, per_file = decode_and_score(test_files, encoded_files, params, tmpdir)
    for k, v in per_file.items():
        trial.set_user_attr(k, v)
    return mean_pesq


def phase2_objective(trial, test_files, tmpdir):
    """Optimize encoder + decoder parameters jointly."""
    enc_params = {
        "TLCS_PREEMPH": trial.suggest_float("preemph", 0.0, 0.70),
        "TLCS_GAMMA1": trial.suggest_float("gamma1", 0.85, 0.99),
        "TLCS_GAMMA2": trial.suggest_float("gamma2", 0.30, 0.70),
        "TLCS_BWE": trial.suggest_float("bwe", 0.988, 0.999),
        "TLCS_PITCH_BLEND_LR": trial.suggest_float("pitch_blend_lr", 0.0, 0.35),
        "TLCS_PITCH_BLEND_MAX_LR": trial.suggest_float("pitch_blend_max_lr", 0.0, 0.35),
        "TLCS_TILT_LR": trial.suggest_float("tilt_lr", -0.40, 0.10),
    }
    dec_params = {
        "TLCS_PREEMPH": enc_params["TLCS_PREEMPH"],
        "TLCS_HARM_STRENGTH": trial.suggest_float("harm_strength", 0.0, 0.60),
        "TLCS_FPF_GN": trial.suggest_float("fpf_gn", 0.65, 0.95),
        "TLCS_FPF_GD": trial.suggest_float("fpf_gd", 0.80, 0.99),
        "TLCS_FPF_TILT": trial.suggest_float("fpf_tilt", 0.0, 0.60),
        "TLCS_BASS_ALPHA": trial.suggest_float("bass_alpha", 0.0, 0.30),
        "TLCS_NF_SCALE": trial.suggest_float("nf_scale", 0.0, 1.5),
        "TLCS_TILT_ALPHA": trial.suggest_float("tilt_alpha", 0.0, 0.25),
        # Pitch blend must match encoder
        "TLCS_PITCH_BLEND_LR": enc_params["TLCS_PITCH_BLEND_LR"],
        "TLCS_PITCH_BLEND_MAX_LR": enc_params["TLCS_PITCH_BLEND_MAX_LR"],
    }
    if dec_params["TLCS_FPF_GN"] >= dec_params["TLCS_FPF_GD"]:
        return 0.0

    encoded = encode_files(test_files, enc_params, tmpdir)
    all_params = {**enc_params, **dec_params}
    mean_pesq, per_file = decode_and_score(test_files, encoded, all_params, tmpdir)
    for k, v in per_file.items():
        trial.set_user_attr(k, v)
    return mean_pesq


def run_phase1(n_trials, test_files):
    print("=" * 60)
    print(f"Phase 1: Decoder-only optimization ({len(test_files)} files)")
    print("=" * 60)

    with tempfile.TemporaryDirectory() as tmpdir:
        print("Encoding test files with default encoder params...")
        encoded = encode_files(test_files, tmpdir=tmpdir)
        print(f"  Encoded {len(encoded)} files")

        baseline_mean, baseline_scores = decode_and_score(
            test_files, encoded, tmpdir=tmpdir)
        print(f"  Baseline: mean={baseline_mean:.3f} "
              + " ".join(f"{k}={v:.3f}" for k, v in
                         sorted(baseline_scores.items(), key=lambda x: x[1])))

        study = optuna.create_study(
            direction="maximize",
            sampler=optuna.samplers.TPESampler(seed=42),
            study_name="tlcs_lr_decoder_opt"
        )

        # Seed with current defaults
        study.enqueue_trial({
            "harm_strength": 0.175,
            "fpf_gn": 0.848,
            "fpf_gd": 0.885,
            "fpf_tilt": 0.254,
            "bass_alpha": 0.181,
            "nf_scale": 0.0,
            "tilt_alpha": 0.0,
        })

        def objective(trial):
            return phase1_objective(trial, test_files, encoded, tmpdir)

        study.optimize(objective, n_trials=n_trials, show_progress_bar=True)

        print("\n" + "=" * 60)
        print("Phase 1 Results")
        print("=" * 60)
        print(f"Best PESQ: {study.best_value:.4f} (baseline: {baseline_mean:.4f}, "
              f"delta: {study.best_value - baseline_mean:+.4f})")
        print("Best params:")
        for k, v in study.best_params.items():
            print(f"  {k}: {v:.4f}")
        print("Per-file scores (sorted):")
        attrs = sorted(study.best_trial.user_attrs.items(), key=lambda x: x[1])
        for k, v in attrs:
            print(f"  {k}: {v:.3f}")

    return study


def run_phase2(n_trials, test_files, phase1_best=None):
    print("\n" + "=" * 60)
    print(f"Phase 2: Joint encoder+decoder optimization ({len(test_files)} files)")
    print("=" * 60)

    with tempfile.TemporaryDirectory() as tmpdir:
        study = optuna.create_study(
            direction="maximize",
            sampler=optuna.samplers.TPESampler(seed=42),
            study_name="tlcs_lr_joint_opt"
        )

        # Seed: current optimized defaults
        seed = {
            "preemph": 0.061,
            "gamma1": 0.967,
            "gamma2": 0.373,
            "bwe": 0.993,
            "pitch_blend_lr": 0.299,
            "pitch_blend_max_lr": 0.184,
            "tilt_lr": 0.019,
            "harm_strength": 0.098,
            "fpf_gn": 0.933,
            "fpf_gd": 0.945,
            "fpf_tilt": 0.180,
            "bass_alpha": 0.224,
            "nf_scale": 0.0,
            "tilt_alpha": 0.0,
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
        print(f"Best PESQ: {study.best_value:.4f}")
        print("Best params:")
        for k, v in study.best_params.items():
            print(f"  {k}: {v:.4f}")
        print("Per-file scores (sorted):")
        attrs = sorted(study.best_trial.user_attrs.items(), key=lambda x: x[1])
        for k, v in attrs:
            print(f"  {k}: {v:.3f}")

    return study


def main():
    parser = argparse.ArgumentParser(description="TLCS LR parameter optimizer")
    parser.add_argument("--phase", choices=["1", "2", "both"], default="both")
    parser.add_argument("--n-trials", type=int, default=100)
    parser.add_argument("--full-corpus", action="store_true",
                        help="Use all 20 speech files (slower but more robust)")
    args = parser.parse_args()

    test_files = FULL_FILES if args.full_corpus else QUICK_FILES
    print(f"Using {len(test_files)} test files")

    optuna.logging.set_verbosity(optuna.logging.WARNING)

    phase1_best = None
    if args.phase in ("1", "both"):
        study1 = run_phase1(args.n_trials, test_files)
        phase1_best = study1.best_params

    if args.phase in ("2", "both"):
        run_phase2(args.n_trials, test_files, phase1_best)


if __name__ == "__main__":
    main()
