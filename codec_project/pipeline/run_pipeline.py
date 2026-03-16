"""
End-to-end pipeline runner for the CELP codec optimization system.

Orchestrates the full workflow:

Stage 1 (train_codebooks): Train LSP and gain VQ codebooks from speech corpus
Stage 2 (optimize):        Run CMA-ES optimization loop using torchmetrics NISQA evaluator
Stage 3 (evaluate):        Run baseline_eval on best config, compare to initial config
"""

from __future__ import annotations

import argparse
import json
import logging
import os
import sys
import time
from pathlib import Path

logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Stage implementations
# ---------------------------------------------------------------------------

def stage_data(
    nisqa_dir: str,
    corpus_dir: str,
    librispeech_dir: str,
    target_sr: int,
) -> None:
    """Stage 1: Data preparation."""
    from pipeline.download_data import download_nisqa_corpus, download_librispeech_clean100
    from pipeline.preprocess import normalize_corpus

    logger.info("=" * 60)
    logger.info("STAGE 1: Data Preparation")
    logger.info("=" * 60)

    # Download NISQA (for evaluator training)
    nisqa_path = Path(nisqa_dir)
    if not nisqa_path.exists() or not any(nisqa_path.iterdir()):
        logger.info("Downloading NISQA corpus ...")
        download_nisqa_corpus(
            nisqa_dir,
            subsets=["NISQA_TRAIN_SIM", "NISQA_TRAIN_LIVE",
                     "NISQA_VAL_SIM", "NISQA_VAL_LIVE"],
        )
    else:
        logger.info("NISQA corpus already present at %s", nisqa_dir)

    # Download LibriSpeech (for codec optimization)
    libri_path = Path(librispeech_dir)
    if not libri_path.exists() or not any(libri_path.iterdir()):
        logger.info("Downloading LibriSpeech clean-100 ...")
        download_librispeech_clean100(librispeech_dir)
    else:
        logger.info("LibriSpeech already present at %s", librispeech_dir)

    # Preprocess optimization corpus
    processed_dir = str(Path(corpus_dir) / "processed")
    if Path(processed_dir).exists() and list(Path(processed_dir).glob("*.wav")):
        logger.info("Processed corpus already exists at %s", processed_dir)
    else:
        logger.info("Preprocessing optimization corpus ...")
        # Look for raw wavs in LibriSpeech
        raw_dir = librispeech_dir
        ls_subdir = Path(librispeech_dir) / "LibriSpeech" / "train-clean-100"
        if ls_subdir.exists():
            raw_dir = str(ls_subdir)

        stats = normalize_corpus(
            input_dir=raw_dir,
            output_dir=processed_dir,
            target_sr=target_sr,
            normalize_loudness=True,
            remove_silence=True,
            min_duration_s=1.0,
        )
        logger.info("Preprocessing stats: %s", stats)

    logger.info("Stage 1 complete.")


def stage_train_codebooks(
    corpus_dir: str,
    codebook_dir: str = "codec/codebooks",
    config_path: str = "configs/default_8kbps.json",
    max_frames: int = 200_000,
    kmeans_iter: int = 100,
) -> None:
    """
    Stage 2: Train VQ codebooks for LSP and gain quantization.

    Must run before evaluator training or optimization — the codec needs
    trained codebooks to produce meaningful quality output for evaluation.
    """
    logger.info("=" * 60)
    logger.info("STAGE 2: Codec Codebook Training")
    logger.info("=" * 60)

    import json
    from codec.config import CodecConfig
    from codec.train_codebooks import train_codebooks

    with open(config_path) as f:
        config = CodecConfig.from_dict(json.load(f))

    stats = train_codebooks(
        corpus_dir=corpus_dir,
        codebook_dir=codebook_dir,
        config=config,
        max_frames=max_frames,
        kmeans_iter=kmeans_iter,
    )

    logger.info(
        "Stage 2 complete. LSP distortion=%.4f, Gain distortion=%.4f",
        stats["lsp_normalized_distortion"],
        stats["gain_normalized_distortion"],
    )


def stage_train_eval(
    nisqa_dir: str,
    evaluator_checkpoint_dir: str,
    srcc_threshold: float = 0.85,
) -> str:
    """
    Stage 2: Train evaluator and freeze.

    Returns path to the frozen evaluator checkpoint.
    """
    logger.info("=" * 60)
    logger.info("STAGE 2: Evaluator Training")
    logger.info("=" * 60)

    frozen_path = os.path.join(evaluator_checkpoint_dir, "frozen_evaluator.pt")
    if os.path.exists(frozen_path):
        logger.info("Frozen evaluator already exists at %s", frozen_path)
        return frozen_path

    # Check if evaluator training script exists
    train_script = os.path.join("evaluator", "train.py")
    freeze_script = os.path.join("evaluator", "freeze.py")

    import torch
    from evaluator.model import PerceptualEvaluator

    # Build corpus for training
    from pipeline.corpus import SpeechCorpus
    corpus = SpeechCorpus(nisqa_dir, format="nisqa")
    train_samples, val_samples = corpus.train_val_split(val_ratio=0.1)

    logger.info(
        "Training evaluator on %d samples, validating on %d",
        len(train_samples), len(val_samples),
    )

    # Initialize evaluator
    evaluator = PerceptualEvaluator(freeze_backbone=True)
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    evaluator = evaluator.to(device)

    # Training loop for the MOS head only
    optimizer = torch.optim.AdamW(
        evaluator.head.parameters(), lr=1e-3, weight_decay=1e-4
    )
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(
        optimizer, T_max=50
    )
    loss_fn = torch.nn.MSELoss()

    best_srcc = -1.0
    best_state = None
    patience = 10
    patience_counter = 0

    for epoch in range(50):
        evaluator.head.train()
        epoch_loss = 0.0
        n_batches = 0

        # Simple batch iteration
        import random
        random.shuffle(train_samples)
        batch_size = 8

        for i in range(0, len(train_samples), batch_size):
            batch = train_samples[i : i + batch_size]
            batch_mos = []
            batch_wavs = []

            for sample in batch:
                if sample.mos_score is None:
                    continue
                try:
                    import torchaudio
                    wav, sr = torchaudio.load(sample.path)
                    if wav.size(0) > 1:
                        wav = wav.mean(dim=0, keepdim=True)
                    wav = wav.squeeze(0)
                    batch_wavs.append((wav, sr))
                    batch_mos.append(sample.mos_score)
                except Exception:
                    continue

            if not batch_wavs:
                continue

            # Score each sample and accumulate loss
            optimizer.zero_grad()
            total_loss = torch.tensor(0.0, device=device, requires_grad=True)

            for (wav, sr), target_mos in zip(batch_wavs, batch_mos):
                pred = evaluator(wav.to(device), sample_rate=sr)
                target = torch.tensor(
                    target_mos, device=device, dtype=torch.float32
                )
                total_loss = total_loss + loss_fn(pred, target)

            avg_loss = total_loss / len(batch_wavs)
            avg_loss.backward()
            torch.nn.utils.clip_grad_norm_(evaluator.head.parameters(), 1.0)
            optimizer.step()

            epoch_loss += avg_loss.item()
            n_batches += 1

        scheduler.step()

        if n_batches == 0:
            logger.warning("Epoch %d: no valid batches", epoch)
            continue

        avg_epoch_loss = epoch_loss / n_batches

        # Validation
        evaluator.eval()
        val_preds = []
        val_targets = []

        with torch.no_grad():
            for sample in val_samples:
                if sample.mos_score is None:
                    continue
                try:
                    import torchaudio
                    wav, sr = torchaudio.load(sample.path)
                    if wav.size(0) > 1:
                        wav = wav.mean(dim=0, keepdim=True)
                    wav = wav.squeeze(0)
                    pred = evaluator(wav.to(device), sample_rate=sr).item()
                    val_preds.append(pred)
                    val_targets.append(sample.mos_score)
                except Exception:
                    continue

        if len(val_preds) >= 3:
            from scipy.stats import spearmanr
            srcc, _ = spearmanr(val_preds, val_targets)
        else:
            srcc = 0.0

        logger.info(
            "Epoch %d: loss=%.4f, val_srcc=%.4f", epoch, avg_epoch_loss, srcc
        )

        if srcc > best_srcc:
            best_srcc = srcc
            best_state = {
                k: v.cpu().clone()
                for k, v in evaluator.state_dict().items()
            }
            patience_counter = 0
        else:
            patience_counter += 1
            if patience_counter >= patience:
                logger.info("Early stopping at epoch %d", epoch)
                break

    # Check quality gate
    if best_srcc < srcc_threshold:
        logger.warning(
            "Best SRCC (%.4f) below threshold (%.4f). "
            "Proceeding anyway, but evaluator may be unreliable.",
            best_srcc, srcc_threshold,
        )

    # Freeze and save as a proper frozen artifact
    os.makedirs(evaluator_checkpoint_dir, exist_ok=True)
    if best_state is not None:
        evaluator.load_state_dict(best_state)
    evaluator.eval()

    # Freeze all parameters
    for param in evaluator.parameters():
        param.requires_grad = False

    from datetime import datetime, timezone
    frozen_artifact = {
        "model_state_dict": evaluator.state_dict(),
        "backbone_name": getattr(evaluator, "backbone_name", "microsoft/wavlm-base-plus"),
        "validation_srcc": best_srcc,
        "frozen_at": datetime.now(timezone.utc).isoformat(),
        "is_frozen_artifact": True,
        "source_checkpoint": "pipeline_train_eval",
    }

    torch.save(frozen_artifact, frozen_path)
    logger.info("Frozen evaluator saved to %s (SRCC=%.4f)", frozen_path, best_srcc)
    return frozen_path


def stage_optimize(
    corpus_dir: str,
    target_bitrate: int,
    optimizer_type: str,
    max_generations: int,
    population_size: int | None,
    output_dir: str,
) -> str:
    """
    Run CMA-ES optimization using torchmetrics NISQA evaluator.

    Returns path to the best config JSON.
    """
    logger.info("=" * 60)
    logger.info("Codec Optimization (%s)", optimizer_type)
    logger.info("=" * 60)

    import torch
    from evaluator.evaluator_api import FrozenEvaluator
    from optimizer.search_space import SearchSpaceEncoder, CodecConfig
    from optimizer.fitness import FitnessFunction

    # Create pre-trained NISQA evaluator (no artifact file needed)
    frozen_eval = FrozenEvaluator(
        device="cuda" if torch.cuda.is_available() else "cpu",
    )

    # Build evaluator adapter matching the FitnessFunction Protocol:
    #   score(pairs: List[Tuple[str, str]]) -> List[float]
    class _EvalAdapter:
        """Adapts FrozenEvaluator.score(path) to the Protocol's score(pairs)."""
        def score(self, pairs):
            return [frozen_eval.score(deg) for _ref, deg in pairs]

    # Collect speech files
    processed_dir = os.path.join(corpus_dir, "processed")
    if os.path.isdir(processed_dir):
        wav_dir = processed_dir
    else:
        wav_dir = corpus_dir

    speech_paths = sorted(
        [str(p) for p in Path(wav_dir).glob("**/*.wav")]
    )
    if not speech_paths:
        raise FileNotFoundError(f"No wav files found in {wav_dir}")

    logger.info("Using %d speech files for optimization", len(speech_paths))

    # Build fitness function (uses CELPCodecWrapper by default)
    fitness_fn = FitnessFunction(
        evaluator=_EvalAdapter(),
        speech_corpus=speech_paths,
        target_bitrate=target_bitrate,
        lambda_bitrate=0.5,
        lambda_complexity=0.1,
        num_samples=5,  # 5 files per eval for speed (pure-Python codec is slow)
    )

    # Build search space encoder
    search_space = SearchSpaceEncoder()

    # Run optimizer
    os.makedirs(output_dir, exist_ok=True)
    log_dir = os.path.join(output_dir, "logs")
    checkpoint_dir = os.path.join(output_dir, "checkpoints")
    os.makedirs(log_dir, exist_ok=True)
    os.makedirs(checkpoint_dir, exist_ok=True)

    if optimizer_type == "cmaes":
        from optimizer.cmaes_optimizer import CodecCMAESOptimizer
        opt = CodecCMAESOptimizer(
            fitness_fn=fitness_fn,
            search_space=search_space,
            sigma0=0.3,
            population_size=population_size,
            max_generations=max_generations,
            checkpoint_dir=checkpoint_dir,
            log_dir=log_dir,
        )
    elif optimizer_type == "bayesian":
        from optimizer.bayesian_optimizer import CodecBayesianOptimizer
        opt = CodecBayesianOptimizer(
            fitness_fn=fitness_fn,
            search_space=search_space,
            n_calls=max_generations,
            n_initial_points=min(20, max_generations // 5),
        )
    else:
        raise ValueError(f"Unknown optimizer type: {optimizer_type}")

    best_config = opt.run()

    # Save best config
    config_path = os.path.join(output_dir, "best_config.json")
    with open(config_path, "w") as f:
        json.dump(best_config.to_dict(), f, indent=2)

    logger.info("Best config saved to %s", config_path)
    logger.info("Best config: %s", best_config.summary())
    return config_path


def stage_evaluate(
    best_config_path: str,
    initial_config_path: str | None,
    corpus_dir: str,
    evaluator_checkpoint: str,
    output_dir: str,
) -> None:
    """Stage 4: Final evaluation and comparison."""
    logger.info("=" * 60)
    logger.info("STAGE 4: Final Evaluation")
    logger.info("=" * 60)

    import torch
    from codec.config import CodecConfig
    from evaluator.model import PerceptualEvaluator
    from pipeline.baseline_eval import run_baseline_evaluation

    # Load evaluator
    evaluator = PerceptualEvaluator(freeze_backbone=True)
    state = torch.load(evaluator_checkpoint, map_location="cpu")
    evaluator.load_state_dict(state)
    evaluator.eval()
    for param in evaluator.parameters():
        param.requires_grad = False
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    evaluator = evaluator.to(device)

    # Find wav dir
    processed_dir = os.path.join(corpus_dir, "processed")
    wav_dir = processed_dir if os.path.isdir(processed_dir) else corpus_dir

    # Evaluate best config
    with open(best_config_path) as f:
        best_config = CodecConfig.from_dict(json.load(f))

    eval_dir = os.path.join(output_dir, "final_eval_best")
    best_report = run_baseline_evaluation(
        corpus_dir=wav_dir,
        codec_config=best_config,
        evaluator=evaluator,
        output_dir=eval_dir,
        compare_pesq=True,
        max_files=100,
    )

    # Evaluate initial config for comparison
    initial_report = None
    if initial_config_path and os.path.exists(initial_config_path):
        with open(initial_config_path) as f:
            initial_config = CodecConfig.from_dict(json.load(f))

        eval_dir_init = os.path.join(output_dir, "final_eval_initial")
        initial_report = run_baseline_evaluation(
            corpus_dir=wav_dir,
            codec_config=initial_config,
            evaluator=evaluator,
            output_dir=eval_dir_init,
            compare_pesq=True,
            max_files=100,
        )

    # Print comparison
    _print_comparison(best_report, initial_report)

    # Save combined report
    combined = {
        "best_config": best_report,
        "initial_config": initial_report,
        "improvement": _compute_improvement(best_report, initial_report),
    }
    combined_path = os.path.join(output_dir, "final_report.json")
    with open(combined_path, "w") as f:
        json.dump(combined, f, indent=2, default=str)

    logger.info("Final report saved to %s", combined_path)


def _print_comparison(best_report: dict, initial_report: dict | None) -> None:
    """Print a formatted comparison table."""
    print("\n" + "=" * 60)
    print("FINAL EVALUATION RESULTS")
    print("=" * 60)

    def _fmt(report, key, subkey):
        try:
            return f"{report[key][subkey]:.3f}"
        except (KeyError, TypeError):
            return "N/A"

    headers = ["Metric", "Best Config"]
    if initial_report:
        headers.append("Initial Config")
        headers.append("Delta")

    print(f"{'Metric':<25} {'Best Config':>15}", end="")
    if initial_report:
        print(f" {'Initial Config':>15} {'Delta':>10}", end="")
    print()
    print("-" * (55 if initial_report else 40))

    metrics = [
        ("Evaluator MOS (mean)", "evaluator_mos", "mean"),
        ("Evaluator MOS (std)", "evaluator_mos", "std"),
        ("PESQ (mean)", "pesq", "mean"),
        ("PESQ (std)", "pesq", "std"),
        ("Bitrate (bps)", "bitrate", "mean_bps"),
    ]

    for label, key, subkey in metrics:
        best_val = _fmt(best_report, key, subkey)
        print(f"{label:<25} {best_val:>15}", end="")
        if initial_report:
            init_val = _fmt(initial_report, key, subkey)
            try:
                delta = (
                    best_report[key][subkey] - initial_report[key][subkey]
                )
                delta_str = f"{delta:+.3f}"
            except (KeyError, TypeError):
                delta_str = "N/A"
            print(f" {init_val:>15} {delta_str:>10}", end="")
        print()

    print("=" * (55 if initial_report else 40))


def _compute_improvement(
    best_report: dict, initial_report: dict | None
) -> dict | None:
    """Compute improvement metrics."""
    if initial_report is None:
        return None

    improvement = {}
    for key, subkey in [
        ("evaluator_mos", "mean"),
        ("pesq", "mean"),
        ("bitrate", "mean_bps"),
    ]:
        try:
            best_val = best_report[key][subkey]
            init_val = initial_report[key][subkey]
            improvement[f"{key}_{subkey}_delta"] = best_val - init_val
            if init_val != 0:
                improvement[f"{key}_{subkey}_pct"] = (
                    (best_val - init_val) / abs(init_val) * 100
                )
        except (KeyError, TypeError):
            pass

    return improvement


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main():
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)-8s %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )

    parser = argparse.ArgumentParser(
        description="End-to-end CELP codec optimization pipeline."
    )
    parser.add_argument(
        "--stage",
        default="all",
        choices=["all", "train_codebooks", "optimize", "evaluate"],
        help="Which stage(s) to run. Default: all.",
    )
    parser.add_argument("--corpus_dir", default="data/corpus")
    parser.add_argument("--nisqa_dir", default="data/nisqa")
    parser.add_argument("--librispeech_dir", default="data/librispeech")
    parser.add_argument(
        "--target_bitrate", type=int, default=8000, choices=[4000, 8000],
    )
    parser.add_argument(
        "--optimizer", default="cmaes", choices=["cmaes", "bayesian"],
    )
    parser.add_argument("--max_generations", type=int, default=200)
    parser.add_argument("--population_size", type=int, default=None)
    parser.add_argument("--output_dir", default="output")
    parser.add_argument(
        "--evaluator_checkpoint_dir", default="evaluator/checkpoints",
    )
    parser.add_argument(
        "--initial_config", default=None,
        help="Path to initial config JSON for comparison.",
    )
    parser.add_argument(
        "--resume", default=None,
        help="Path to optimizer checkpoint to resume from.",
    )

    args = parser.parse_args()

    start_time = time.time()

    # Default initial config
    if args.initial_config is None:
        bitrate_str = f"{args.target_bitrate // 1000}kbps"
        default_config = f"configs/default_{bitrate_str}.json"
        if os.path.exists(default_config):
            args.initial_config = default_config

    frozen_evaluator = os.path.join(
        args.evaluator_checkpoint_dir, "frozen_evaluator.pt"
    )

    stages = (
        ["train_codebooks", "optimize", "evaluate"]
        if args.stage == "all"
        else [args.stage]
    )

    for stage in stages:
        if stage == "train_codebooks":
            bitrate_str = f"{args.target_bitrate // 1000}kbps"
            stage_train_codebooks(
                corpus_dir=args.corpus_dir,
                codebook_dir="codec/codebooks",
                config_path=args.initial_config or f"configs/default_{bitrate_str}.json",
            )

        elif stage == "optimize":
            best_config_path = stage_optimize(
                corpus_dir=args.corpus_dir,
                target_bitrate=args.target_bitrate,
                optimizer_type=args.optimizer,
                max_generations=args.max_generations,
                population_size=args.population_size,
                output_dir=os.path.join(args.output_dir, "optimization"),
            )

        elif stage == "evaluate":
            best_config = os.path.join(
                args.output_dir, "optimization", "best_config.json"
            )
            if not os.path.exists(best_config):
                logger.error(
                    "Best config not found at %s. "
                    "Run --stage optimize first.",
                    best_config,
                )
                sys.exit(1)

            stage_evaluate(
                best_config_path=best_config,
                initial_config_path=args.initial_config,
                corpus_dir=args.corpus_dir,
                evaluator_checkpoint=frozen_evaluator,
                output_dir=os.path.join(args.output_dir, "evaluation"),
            )

    elapsed = time.time() - start_time
    logger.info("Pipeline complete in %.1f minutes", elapsed / 60)


if __name__ == "__main__":
    main()
