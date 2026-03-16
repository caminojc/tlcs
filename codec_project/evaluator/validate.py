"""
Standalone validation script for the perceptual evaluator.

Loads a trained checkpoint, evaluates on a held-out test set, and reports
correlation metrics against ground-truth MOS (and optionally PESQ / POLQA).

The evaluator is considered **validated** when SRCC > 0.85 on the test set.
Only a validated evaluator may be frozen for use as the optimiser oracle.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import matplotlib

matplotlib.use("Agg")  # non-interactive backend
import matplotlib.pyplot as plt
import numpy as np
import torch
from scipy.stats import pearsonr, spearmanr

from evaluator.model import PerceptualEvaluator
from evaluator.train import MOSDataset


PASS_THRESHOLD: float = 0.85


def load_model(checkpoint: str, device: torch.device) -> PerceptualEvaluator:
    """Load a PerceptualEvaluator from a training checkpoint."""
    ckpt = torch.load(checkpoint, map_location=device, weights_only=False)
    backbone_name = ckpt.get("backbone_name", "microsoft/wavlm-base-plus")
    model = PerceptualEvaluator(backbone_name=backbone_name, freeze_backbone=True)
    model.load_state_dict(ckpt["model_state_dict"])
    model.to(device)
    model.eval()
    return model


def evaluate_on_dataset(
    model: PerceptualEvaluator,
    dataset: MOSDataset,
    device: torch.device,
    batch_size: int = 16,
) -> Tuple[List[float], List[float]]:
    """Return (predicted_mos, ground_truth_mos) lists."""
    from torch.utils.data import DataLoader
    from evaluator.train import collate_fn

    loader = DataLoader(
        dataset, batch_size=batch_size, shuffle=False,
        collate_fn=collate_fn, num_workers=2,
    )
    all_preds: List[float] = []
    all_targets: List[float] = []

    with torch.no_grad():
        for wavs, targets in loader:
            wavs = wavs.to(device)
            preds = model(wavs).squeeze()
            if preds.dim() == 0:
                preds = preds.unsqueeze(0)
            all_preds.extend(preds.cpu().tolist())
            all_targets.extend(targets.tolist())

    return all_preds, all_targets


def compute_metrics(
    preds: List[float], targets: List[float],
) -> Dict[str, float]:
    """Compute SRCC, Pearson R, and RMSE."""
    preds_np = np.array(preds)
    targets_np = np.array(targets)
    srcc, srcc_p = spearmanr(preds_np, targets_np)
    pearson, pearson_p = pearsonr(preds_np, targets_np)
    rmse = float(np.sqrt(np.mean((preds_np - targets_np) ** 2)))
    return {
        "srcc": round(float(srcc), 4),
        "srcc_pvalue": float(srcc_p),
        "pearson_r": round(float(pearson), 4),
        "pearson_pvalue": float(pearson_p),
        "rmse": round(rmse, 4),
        "n_samples": len(preds),
    }


def compute_pesq_correlation(
    dataset: MOSDataset, preds: List[float],
) -> Optional[Dict[str, float]]:
    """Compute SRCC of predictions vs PESQ scores, if pesq is installed."""
    try:
        from pesq import pesq as pesq_fn
    except ImportError:
        print("pesq not installed — skipping PESQ correlation")
        return None

    import torchaudio

    pesq_scores: List[float] = []
    pred_subset: List[float] = []

    for i, (path, _mos) in enumerate(dataset.entries):
        try:
            wav, sr = torchaudio.load(path)
            if wav.size(0) > 1:
                wav = wav.mean(dim=0, keepdim=True)
            wav = wav.squeeze(0)
            if sr != 16000:
                wav = torchaudio.functional.resample(wav, sr, 16000)
            # PESQ needs a reference — without it we cannot compute.
            # Skip gracefully.
            break
        except Exception:
            continue
    print("PESQ correlation requires reference audio — skipping (degraded-only dataset)")
    return None


def compute_polqa_correlation(
    dataset: MOSDataset, preds: List[float],
) -> Optional[Dict[str, float]]:
    """Compute SRCC vs POLQA if available. Skips gracefully if not."""
    try:
        import polqa  # noqa: F401
    except ImportError:
        print("polqa not installed — skipping POLQA correlation")
        return None
    print("POLQA correlation requires reference audio — skipping")
    return None


def scatter_plot(
    preds: List[float],
    targets: List[float],
    metrics: Dict[str, float],
    output_path: str,
) -> None:
    """Save a scatter plot of predicted vs ground-truth MOS."""
    fig, ax = plt.subplots(figsize=(7, 7))
    ax.scatter(targets, preds, alpha=0.4, s=12, edgecolors="none")
    ax.plot([1, 5], [1, 5], "r--", linewidth=1, label="ideal")
    ax.set_xlabel("Ground-Truth MOS")
    ax.set_ylabel("Predicted MOS")
    ax.set_title(
        f"Evaluator Validation\n"
        f"SRCC={metrics['srcc']:.3f}  |  Pearson={metrics['pearson_r']:.3f}  |  "
        f"RMSE={metrics['rmse']:.3f}"
    )
    ax.set_xlim(0.8, 5.2)
    ax.set_ylim(0.8, 5.2)
    ax.set_aspect("equal")
    ax.legend()
    ax.grid(True, alpha=0.3)
    fig.tight_layout()
    fig.savefig(output_path, dpi=150)
    plt.close(fig)
    print(f"Scatter plot saved to {output_path}")


def validate(args: argparse.Namespace) -> Dict:
    """Run full validation and return results dict."""
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")

    # Load model.
    model = load_model(args.checkpoint, device)
    assert model.backbone_is_frozen(), "Backbone must be frozen"

    # Load test data.
    csv_path = args.test_csv
    if csv_path is None:
        from evaluator.train import find_csv
        csv_path = find_csv(args.data_dir)
    dataset = MOSDataset(csv_path, args.data_dir)
    print(f"Test set: {len(dataset)} samples")

    # Evaluate.
    preds, targets = evaluate_on_dataset(model, dataset, device)
    metrics = compute_metrics(preds, targets)

    # Optional external metric correlations.
    pesq_corr = compute_pesq_correlation(dataset, preds)
    polqa_corr = compute_polqa_correlation(dataset, preds)

    # Scatter plot.
    output_dir = Path(args.checkpoint).parent.parent
    scatter_path = str(output_dir / "validation_scatter.png")
    scatter_plot(preds, targets, metrics, scatter_path)

    # Pass / fail.
    passed = metrics["srcc"] >= PASS_THRESHOLD
    status = "PASS" if passed else "FAIL"
    print(f"\n{'=' * 50}")
    print(f"Validation {status}: SRCC = {metrics['srcc']:.4f} (threshold = {PASS_THRESHOLD})")
    print(f"  Pearson R = {metrics['pearson_r']:.4f}")
    print(f"  RMSE      = {metrics['rmse']:.4f}")
    print(f"  Samples   = {metrics['n_samples']}")
    print(f"{'=' * 50}")

    results = {
        "status": status,
        "metrics": metrics,
        "pesq_correlation": pesq_corr,
        "polqa_correlation": polqa_corr,
        "checkpoint": args.checkpoint,
        "threshold": PASS_THRESHOLD,
    }

    results_path = str(output_dir / "validation_results.json")
    with open(results_path, "w") as f:
        json.dump(results, f, indent=2)
    print(f"Results saved to {results_path}")

    return results


def main() -> None:
    parser = argparse.ArgumentParser(description="Validate the perceptual evaluator.")
    parser.add_argument(
        "--checkpoint", type=str, required=True,
        help="Path to trained checkpoint (best_mos_head.pt).",
    )
    parser.add_argument(
        "--test_csv", type=str, default=None,
        help="Path to test CSV. If not given, auto-detects in --data_dir.",
    )
    parser.add_argument(
        "--data_dir", type=str, required=True,
        help="Directory containing test wav files.",
    )
    validate(parser.parse_args())


if __name__ == "__main__":
    main()
