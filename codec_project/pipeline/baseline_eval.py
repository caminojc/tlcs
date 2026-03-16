"""
Baseline evaluation: run a codec config through encode→decode→score cycle
and produce a comprehensive quality report.
"""

from __future__ import annotations

import json
import logging
import os
import tempfile
from pathlib import Path
from typing import Dict, List, Optional

import numpy as np
import soundfile as sf

logger = logging.getLogger(__name__)


def run_baseline_evaluation(
    corpus_dir: str,
    codec_config,
    evaluator=None,
    output_dir: str = "eval_output",
    compare_pesq: bool = True,
    max_files: Optional[int] = None,
) -> Dict:
    """
    Run full evaluation of a codec config.

    Steps:
    1. Encode and decode all (or max_files) wav files in corpus_dir
    2. Score with evaluator (FrozenEvaluator / PerceptualEvaluator)
    3. Compute PESQ scores if compare_pesq=True
    4. Generate report: mean MOS, MOS distribution, correlation with PESQ
    5. Save report to output_dir/eval_report.json
    6. Save scatter plot: our_mos vs pesq (if matplotlib available)

    Parameters
    ----------
    corpus_dir : str
        Directory of wav files to evaluate.
    codec_config : CodecConfig
        Codec configuration to evaluate.
    evaluator : optional
        Perceptual evaluator with a .score(path) method. If None, only
        PESQ is computed.
    output_dir : str
        Where to save report and decoded files.
    compare_pesq : bool
        Whether to compute PESQ scores.
    max_files : int or None
        Limit number of files to evaluate. None = all.

    Returns
    -------
    dict
        Evaluation report.
    """
    from codec.config import CodecConfig as FullCodecConfig
    from codec.encoder import CELPEncoder
    from codec.decoder import CELPDecoder

    output_path = Path(output_dir)
    output_path.mkdir(parents=True, exist_ok=True)
    decoded_dir = output_path / "decoded"
    decoded_dir.mkdir(exist_ok=True)

    # Collect wav files
    corpus_path = Path(corpus_dir)
    wav_files = sorted(
        list(corpus_path.glob("**/*.wav"))
        + list(corpus_path.glob("**/*.flac"))
    )
    if max_files is not None:
        wav_files = wav_files[:max_files]

    if not wav_files:
        logger.error("No audio files found in %s", corpus_dir)
        return {"error": "No audio files found"}

    # Ensure we have a full codec config
    if isinstance(codec_config, dict):
        config = FullCodecConfig.from_dict(codec_config)
    elif hasattr(codec_config, "sample_rate"):
        config = codec_config
    else:
        # Optimizer's CodecConfig — convert by merging with defaults
        config = _optimizer_config_to_codec_config(codec_config)

    config.validate()

    # Initialize codec
    encoder = CELPEncoder(config)
    decoder = CELPDecoder(config)

    # Encode → decode all files
    reference_paths: List[str] = []
    degraded_paths: List[str] = []
    eval_mos_scores: List[Optional[float]] = []
    bitstream_sizes: List[int] = []
    durations: List[float] = []
    encode_errors = 0

    logger.info("Evaluating %d files with config: %s", len(wav_files), config)

    for wav_file in wav_files:
        try:
            out_name = wav_file.stem + "_decoded.wav"
            decoded_path = str(decoded_dir / out_name)
            bitstream_path = str(decoded_dir / (wav_file.stem + ".bin"))

            # Encode
            encoder.encode_file(str(wav_file), bitstream_path)

            # Decode
            decoder.decode_file(bitstream_path, decoded_path)

            # Track file sizes
            bs_size = os.path.getsize(bitstream_path)
            bitstream_sizes.append(bs_size)

            info = sf.info(str(wav_file))
            durations.append(info.duration)

            reference_paths.append(str(wav_file))
            degraded_paths.append(decoded_path)

            # Score with evaluator
            if evaluator is not None:
                try:
                    mos = evaluator.score(decoded_path)
                    eval_mos_scores.append(mos)
                except Exception as e:
                    logger.warning("Evaluator failed on %s: %s", decoded_path, e)
                    eval_mos_scores.append(None)
            else:
                eval_mos_scores.append(None)

        except Exception as e:
            logger.warning("Codec failed on %s: %s", wav_file, e)
            encode_errors += 1

    # PESQ scores
    pesq_scores: List[Optional[float]] = [None] * len(reference_paths)
    if compare_pesq and reference_paths:
        try:
            from pipeline.preprocess import compute_pesq_scores
            pesq_scores = compute_pesq_scores(
                degraded_paths, reference_paths, mode="nb"
            )
        except Exception as e:
            logger.warning("PESQ computation failed: %s", e)

    # Compile report
    report = _compile_report(
        config=config,
        reference_paths=reference_paths,
        degraded_paths=degraded_paths,
        eval_mos_scores=eval_mos_scores,
        pesq_scores=pesq_scores,
        bitstream_sizes=bitstream_sizes,
        durations=durations,
        encode_errors=encode_errors,
    )

    # Save report
    report_path = output_path / "eval_report.json"
    with open(report_path, "w") as f:
        json.dump(report, f, indent=2, default=str)
    logger.info("Report saved to %s", report_path)

    # Generate scatter plot
    _try_plot_scatter(
        eval_mos_scores, pesq_scores, output_path / "mos_vs_pesq.png"
    )

    return report


def _optimizer_config_to_codec_config(opt_config):
    """
    Convert an optimizer CodecConfig (subset of params) to a full
    codec.config.CodecConfig by merging onto defaults.
    """
    from codec.config import CodecConfig as FullCodecConfig
    full = FullCodecConfig()
    opt_dict = opt_config.to_dict() if hasattr(opt_config, "to_dict") else {}
    for key, val in opt_dict.items():
        if hasattr(full, key):
            setattr(full, key, val)
    return full


def _compile_report(
    config,
    reference_paths: List[str],
    degraded_paths: List[str],
    eval_mos_scores: List[Optional[float]],
    pesq_scores: List[Optional[float]],
    bitstream_sizes: List[int],
    durations: List[float],
    encode_errors: int,
) -> Dict:
    """Build the evaluation report dict."""
    report: Dict = {
        "config": config.to_dict(),
        "bits_per_frame": config.compute_bits_per_frame(),
        "num_files": len(reference_paths),
        "encode_errors": encode_errors,
    }

    # Bitrate statistics
    if bitstream_sizes and durations:
        actual_bitrates = [
            bs * 8 / dur
            for bs, dur in zip(bitstream_sizes, durations)
            if dur > 0
        ]
        if actual_bitrates:
            report["bitrate"] = {
                "mean_bps": float(np.mean(actual_bitrates)),
                "std_bps": float(np.std(actual_bitrates)),
                "min_bps": float(np.min(actual_bitrates)),
                "max_bps": float(np.max(actual_bitrates)),
            }

    # Evaluator MOS statistics
    valid_mos = [m for m in eval_mos_scores if m is not None]
    if valid_mos:
        arr = np.array(valid_mos)
        report["evaluator_mos"] = {
            "count": len(arr),
            "mean": float(arr.mean()),
            "std": float(arr.std()),
            "min": float(arr.min()),
            "max": float(arr.max()),
            "median": float(np.median(arr)),
        }

    # PESQ statistics
    valid_pesq = [p for p in pesq_scores if p is not None]
    if valid_pesq:
        arr = np.array(valid_pesq)
        report["pesq"] = {
            "count": len(arr),
            "mean": float(arr.mean()),
            "std": float(arr.std()),
            "min": float(arr.min()),
            "max": float(arr.max()),
            "median": float(np.median(arr)),
        }

    # Correlation between evaluator MOS and PESQ
    if valid_mos and valid_pesq:
        # Align by index (only where both are valid)
        paired_mos = []
        paired_pesq = []
        for m, p in zip(eval_mos_scores, pesq_scores):
            if m is not None and p is not None:
                paired_mos.append(m)
                paired_pesq.append(p)

        if len(paired_mos) >= 3:
            from scipy.stats import spearmanr, pearsonr
            srcc, srcc_p = spearmanr(paired_mos, paired_pesq)
            plcc, plcc_p = pearsonr(paired_mos, paired_pesq)
            report["correlation"] = {
                "srcc": float(srcc),
                "srcc_pvalue": float(srcc_p),
                "plcc": float(plcc),
                "plcc_pvalue": float(plcc_p),
                "num_pairs": len(paired_mos),
            }

    # Per-file details
    report["per_file"] = []
    for i, (ref, deg) in enumerate(zip(reference_paths, degraded_paths)):
        entry = {"reference": ref, "degraded": deg}
        if i < len(eval_mos_scores) and eval_mos_scores[i] is not None:
            entry["evaluator_mos"] = eval_mos_scores[i]
        if i < len(pesq_scores) and pesq_scores[i] is not None:
            entry["pesq"] = pesq_scores[i]
        report["per_file"].append(entry)

    return report


def _try_plot_scatter(
    mos_scores: List[Optional[float]],
    pesq_scores: List[Optional[float]],
    output_path,
) -> None:
    """Generate scatter plot of evaluator MOS vs PESQ if possible."""
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        logger.info("matplotlib not available — skipping scatter plot")
        return

    paired_mos = []
    paired_pesq = []
    for m, p in zip(mos_scores, pesq_scores):
        if m is not None and p is not None:
            paired_mos.append(m)
            paired_pesq.append(p)

    if len(paired_mos) < 2:
        return

    fig, ax = plt.subplots(figsize=(8, 6))
    ax.scatter(paired_pesq, paired_mos, alpha=0.6, s=20)
    ax.set_xlabel("PESQ Score")
    ax.set_ylabel("Evaluator MOS")
    ax.set_title("Evaluator MOS vs PESQ")
    ax.grid(True, alpha=0.3)

    # Add correlation line
    z = np.polyfit(paired_pesq, paired_mos, 1)
    p = np.poly1d(z)
    x_line = np.linspace(min(paired_pesq), max(paired_pesq), 100)
    ax.plot(x_line, p(x_line), "r--", alpha=0.5, label="Linear fit")
    ax.legend()

    fig.tight_layout()
    fig.savefig(str(output_path), dpi=150)
    plt.close(fig)
    logger.info("Scatter plot saved to %s", output_path)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    import argparse
    logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")

    parser = argparse.ArgumentParser(
        description="Run baseline evaluation of a codec config."
    )
    parser.add_argument("--corpus_dir", required=True, help="Directory of wav files")
    parser.add_argument("--config", required=True, help="Path to codec config JSON")
    parser.add_argument(
        "--evaluator_checkpoint", default=None,
        help="Path to frozen evaluator checkpoint (.pt)"
    )
    parser.add_argument("--output_dir", default="eval_output", help="Output directory")
    parser.add_argument("--no_pesq", action="store_true", help="Skip PESQ computation")
    parser.add_argument("--max_files", type=int, default=None, help="Max files to eval")

    args = parser.parse_args()

    # Load codec config
    from codec.config import CodecConfig
    config = CodecConfig.from_dict(json.load(open(args.config)))

    # Load evaluator if checkpoint provided
    evaluator = None
    if args.evaluator_checkpoint:
        import torch
        from evaluator.model import PerceptualEvaluator
        evaluator = PerceptualEvaluator()
        state = torch.load(args.evaluator_checkpoint, map_location="cpu")
        evaluator.load_state_dict(state)
        evaluator.eval()

    report = run_baseline_evaluation(
        corpus_dir=args.corpus_dir,
        codec_config=config,
        evaluator=evaluator,
        output_dir=args.output_dir,
        compare_pesq=not args.no_pesq,
        max_files=args.max_files,
    )

    # Print summary
    print("\n=== Evaluation Report ===")
    print(f"Files evaluated: {report.get('num_files', 0)}")
    print(f"Encode errors:   {report.get('encode_errors', 0)}")
    if "evaluator_mos" in report:
        m = report["evaluator_mos"]
        print(f"Evaluator MOS:   {m['mean']:.3f} ± {m['std']:.3f}")
    if "pesq" in report:
        p = report["pesq"]
        print(f"PESQ:            {p['mean']:.3f} ± {p['std']:.3f}")
    if "correlation" in report:
        c = report["correlation"]
        print(f"SRCC(MOS,PESQ):  {c['srcc']:.4f}")
    if "bitrate" in report:
        b = report["bitrate"]
        print(f"Actual bitrate:  {b['mean_bps']:.0f} bps")
    print(f"\nFull report: {args.output_dir}/eval_report.json")
