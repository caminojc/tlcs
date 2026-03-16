"""
Freeze a validated evaluator into a production-ready artifact.

This script is the **gating step** between training and deployment.  It:

1. Loads a training checkpoint.
2. Runs the validation suite (SRCC > 0.85 required).
3. If validation passes, writes ``frozen_evaluator.pt`` — the sole artifact
   consumed by the codec optimiser.
4. If validation fails, exits with an error.  A sub-threshold evaluator must
   **never** be used as the optimiser oracle because its scoring surface is
   unreliable and would drive the optimiser toward meaningless configurations.

The frozen artifact bundles the full model state, backbone name, validation
SRCC, and a ``frozen_at`` timestamp so downstream consumers can verify
provenance.
"""

from __future__ import annotations

import argparse
import sys
from datetime import datetime, timezone
from pathlib import Path

import torch

from evaluator.model import PerceptualEvaluator
from evaluator.validate import PASS_THRESHOLD, load_model


def freeze(args: argparse.Namespace) -> None:
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")

    # ── Load and validate ─────────────────────────────────────────────
    print(f"Loading checkpoint: {args.checkpoint}")
    ckpt = torch.load(args.checkpoint, map_location=device, weights_only=False)

    # Run validation if test data is available.
    val_srcc: float
    if args.data_dir is not None:
        print("Running validation...")
        from evaluator.validate import validate as run_validation

        val_args = argparse.Namespace(
            checkpoint=args.checkpoint,
            test_csv=args.test_csv,
            data_dir=args.data_dir,
        )
        results = run_validation(val_args)
        val_srcc = results["metrics"]["srcc"]
    else:
        # Use the SRCC recorded during training.
        val_srcc = ckpt.get("val_srcc", 0.0)
        print(f"No --data_dir provided; using training SRCC = {val_srcc:.4f}")

    if val_srcc < PASS_THRESHOLD:
        print(
            f"\nFAILED: SRCC {val_srcc:.4f} < {PASS_THRESHOLD} threshold.\n"
            f"The evaluator does NOT meet quality requirements.\n"
            f"Frozen artifact will NOT be produced."
        )
        sys.exit(1)

    # ── Produce frozen artifact ───────────────────────────────────────
    model = load_model(args.checkpoint, device)
    assert model.backbone_is_frozen(), "Backbone must be frozen"

    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)

    frozen_artifact = {
        "model_state_dict": model.state_dict(),
        "backbone_name": model.backbone_name,
        "validation_srcc": val_srcc,
        "frozen_at": datetime.now(timezone.utc).isoformat(),
        "is_frozen_artifact": True,
        "source_checkpoint": str(args.checkpoint),
    }

    torch.save(frozen_artifact, output_path)
    print(f"\nFrozen evaluator saved to {output_path}")
    print(f"  backbone:        {model.backbone_name}")
    print(f"  validation SRCC: {val_srcc:.4f}")
    print(f"  frozen at:       {frozen_artifact['frozen_at']}")


def main() -> None:
    parser = argparse.ArgumentParser(
        description=(
            "Produce a frozen evaluator artifact. "
            "Exits with error if validation SRCC < 0.85."
        ),
    )
    parser.add_argument(
        "--checkpoint", type=str, required=True,
        help="Path to trained checkpoint (best_mos_head.pt).",
    )
    parser.add_argument(
        "--data_dir", type=str, default=None,
        help="Test data directory. If provided, re-runs validation.",
    )
    parser.add_argument(
        "--test_csv", type=str, default=None,
        help="Path to test CSV (used with --data_dir).",
    )
    parser.add_argument(
        "--output", type=str, default="evaluator/frozen_evaluator.pt",
        help="Output path for the frozen artifact.",
    )
    freeze(parser.parse_args())


if __name__ == "__main__":
    main()
