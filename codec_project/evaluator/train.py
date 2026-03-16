"""
Training script for the MOS regression head.

The WavLM backbone is frozen — only the head parameters are optimised.
Supports NISQA and VoiceMOS Challenge data formats.

Loss
----
MSE + 0.1 * pairwise ranking loss.  The ranking term encourages the head to
preserve the ordinal relationship between samples even when the absolute MOS
prediction is off, which improves Spearman rank correlation (SRCC).
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import random
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F
from scipy.stats import spearmanr
from torch.optim import AdamW
from torch.optim.lr_scheduler import CosineAnnealingLR
from torch.utils.data import DataLoader, Dataset

from evaluator.model import PerceptualEvaluator


# ──────────────────────────────────────────────────────────────────────
# Dataset
# ──────────────────────────────────────────────────────────────────────


class MOSDataset(Dataset):
    """
    Reads a CSV with columns ``filepath`` and ``mos``.

    Supports two directory layouts:
    * NISQA: ``data_dir/wav/<filename>.wav``, CSV at ``data_dir/NISQA_*.csv``
    * VoiceMOS: ``data_dir/<filename>.wav``, CSV at ``data_dir/*.csv``

    Audio is loaded, resampled to 16 kHz, and cropped/padded to *max_seconds*.
    """

    TARGET_SR: int = 16_000

    def __init__(
        self,
        csv_path: str,
        data_dir: str,
        max_seconds: float = 6.0,
    ) -> None:
        self.data_dir = Path(data_dir)
        self.max_len = int(max_seconds * self.TARGET_SR)
        self.entries: List[Tuple[str, float]] = []
        self._load_csv(csv_path)

    def _load_csv(self, csv_path: str) -> None:
        import torchaudio  # local import to keep module-level import light

        with open(csv_path, newline="") as f:
            reader = csv.DictReader(f)
            headers = [h.strip().lower() for h in reader.fieldnames]

            # Determine column names (handle varying header conventions).
            filepath_col = next(
                (h for h in reader.fieldnames if h.strip().lower() in
                 ("filepath", "filename", "file", "wav", "audio_path", "deg_wav")),
                reader.fieldnames[0],
            )
            mos_col = next(
                (h for h in reader.fieldnames if h.strip().lower() in
                 ("mos", "mean_mos", "mos_score", "score", "quality")),
                reader.fieldnames[-1],
            )

            for row in reader:
                rel_path = row[filepath_col].strip()
                mos = float(row[mos_col].strip())
                # Try multiple path resolutions.
                candidates = [
                    self.data_dir / rel_path,
                    self.data_dir / "wav" / rel_path,
                    self.data_dir / "wav" / Path(rel_path).name,
                    Path(rel_path),
                ]
                resolved = next((c for c in candidates if c.exists()), None)
                if resolved is not None:
                    self.entries.append((str(resolved), mos))

    def __len__(self) -> int:
        return len(self.entries)

    def __getitem__(self, idx: int) -> Tuple[torch.Tensor, float]:
        import torchaudio

        path, mos = self.entries[idx]
        wav, sr = torchaudio.load(path)
        if wav.size(0) > 1:
            wav = wav.mean(dim=0, keepdim=True)
        wav = wav.squeeze(0)
        if sr != self.TARGET_SR:
            wav = torchaudio.functional.resample(wav, orig_freq=sr, new_freq=self.TARGET_SR)
        # Crop or pad.
        if wav.size(0) > self.max_len:
            start = random.randint(0, wav.size(0) - self.max_len)
            wav = wav[start : start + self.max_len]
        elif wav.size(0) < self.max_len:
            pad = torch.zeros(self.max_len - wav.size(0))
            wav = torch.cat([wav, pad])
        return wav, mos


def collate_fn(
    batch: List[Tuple[torch.Tensor, float]],
) -> Tuple[torch.Tensor, torch.Tensor]:
    wavs, scores = zip(*batch)
    return torch.stack(wavs), torch.tensor(scores, dtype=torch.float32)


# ──────────────────────────────────────────────────────────────────────
# Losses
# ──────────────────────────────────────────────────────────────────────


def pairwise_ranking_loss(preds: torch.Tensor, targets: torch.Tensor) -> torch.Tensor:
    """
    Differentiable pairwise ranking loss.

    For every pair (i, j) in the batch where target_i > target_j, we want
    pred_i > pred_j.  Penalises violations via a margin-based hinge loss.
    """
    diff_pred = preds.unsqueeze(0) - preds.unsqueeze(1)    # (B, B)
    diff_target = targets.unsqueeze(0) - targets.unsqueeze(1)  # (B, B)
    sign = torch.sign(diff_target)
    loss = torch.clamp(0.5 - sign * diff_pred, min=0.0)
    # Only count non-trivial pairs (i != j).
    mask = 1.0 - torch.eye(preds.size(0), device=preds.device)
    n_pairs = mask.sum().clamp(min=1.0)
    return (loss * mask).sum() / n_pairs


# ──────────────────────────────────────────────────────────────────────
# Training loop
# ──────────────────────────────────────────────────────────────────────


def find_csv(data_dir: str) -> str:
    """Locate the first CSV file in *data_dir*."""
    p = Path(data_dir)
    candidates = list(p.glob("*.csv"))
    if not candidates:
        raise FileNotFoundError(f"No CSV files found in {data_dir}")
    return str(candidates[0])


def train(args: argparse.Namespace) -> None:
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    print(f"Training on {device}")

    # ── Data ──────────────────────────────────────────────────────────
    csv_path = find_csv(args.data_dir)
    full_dataset = MOSDataset(csv_path, args.data_dir)
    n_val = max(1, int(len(full_dataset) * args.val_split))
    n_train = len(full_dataset) - n_val
    train_ds, val_ds = torch.utils.data.random_split(full_dataset, [n_train, n_val])
    print(f"Dataset: {n_train} train, {n_val} val samples from {csv_path}")

    train_loader = DataLoader(
        train_ds, batch_size=args.batch_size, shuffle=True,
        collate_fn=collate_fn, num_workers=2, pin_memory=True,
    )
    val_loader = DataLoader(
        val_ds, batch_size=args.batch_size, shuffle=False,
        collate_fn=collate_fn, num_workers=2, pin_memory=True,
    )

    # ── Model ─────────────────────────────────────────────────────────
    model = PerceptualEvaluator(freeze_backbone=True).to(device)
    assert model.backbone_is_frozen(), "Backbone must be frozen during head training"
    print(
        f"Trainable params: "
        f"{sum(p.numel() for p in model.parameters() if p.requires_grad):,} / "
        f"{sum(p.numel() for p in model.parameters()):,}"
    )

    # ── Optimiser ─────────────────────────────────────────────────────
    optimizer = AdamW(
        [p for p in model.parameters() if p.requires_grad],
        lr=args.lr,
        weight_decay=1e-4,
    )
    scheduler = CosineAnnealingLR(optimizer, T_max=args.epochs)

    # ── Training ──────────────────────────────────────────────────────
    checkpoint_dir = Path(args.checkpoint_dir)
    checkpoint_dir.mkdir(parents=True, exist_ok=True)
    best_srcc = -1.0
    patience_counter = 0
    log_lines: List[Dict] = []

    for epoch in range(1, args.epochs + 1):
        model.head.train()
        train_losses = []

        for wavs, targets in train_loader:
            wavs, targets = wavs.to(device), targets.to(device)
            preds = model(wavs).squeeze()
            if preds.dim() == 0:
                preds = preds.unsqueeze(0)

            mse = F.mse_loss(preds, targets)
            rank = pairwise_ranking_loss(preds, targets)
            loss = mse + 0.1 * rank

            optimizer.zero_grad()
            loss.backward()
            optimizer.step()
            train_losses.append(loss.item())

        scheduler.step()

        # ── Validation ────────────────────────────────────────────────
        model.eval()
        all_preds, all_targets = [], []
        val_mse_sum = 0.0

        with torch.no_grad():
            for wavs, targets in val_loader:
                wavs, targets = wavs.to(device), targets.to(device)
                preds = model(wavs).squeeze()
                if preds.dim() == 0:
                    preds = preds.unsqueeze(0)
                val_mse_sum += F.mse_loss(preds, targets, reduction="sum").item()
                all_preds.extend(preds.cpu().tolist())
                all_targets.extend(targets.cpu().tolist())

        val_mse = val_mse_sum / max(len(all_preds), 1)
        srcc, _ = spearmanr(all_preds, all_targets) if len(all_preds) > 2 else (0.0, 1.0)
        mean_train_loss = np.mean(train_losses)

        log_entry = {
            "epoch": epoch,
            "train_loss": round(float(mean_train_loss), 5),
            "val_mse": round(float(val_mse), 5),
            "val_srcc": round(float(srcc), 4),
            "lr": round(scheduler.get_last_lr()[0], 7),
        }
        log_lines.append(log_entry)
        print(
            f"Epoch {epoch:3d}/{args.epochs} | "
            f"train_loss={mean_train_loss:.4f} | "
            f"val_mse={val_mse:.4f} | "
            f"val_srcc={srcc:.4f}"
        )

        # ── Checkpointing / early stopping ────────────────────────────
        if srcc > best_srcc:
            best_srcc = srcc
            patience_counter = 0
            ckpt_path = checkpoint_dir / "best_mos_head.pt"
            torch.save(
                {
                    "epoch": epoch,
                    "model_state_dict": model.state_dict(),
                    "head_state_dict": model.head.state_dict(),
                    "backbone_name": model.backbone_name,
                    "val_srcc": srcc,
                    "val_mse": val_mse,
                },
                ckpt_path,
            )
            print(f"  -> New best SRCC={srcc:.4f}, saved to {ckpt_path}")
        else:
            patience_counter += 1
            if patience_counter >= 10:
                print(f"Early stopping at epoch {epoch} (patience=10)")
                break

    # ── Save training log ─────────────────────────────────────────────
    log_path = checkpoint_dir / "training_log.jsonl"
    with open(log_path, "w") as f:
        for entry in log_lines:
            f.write(json.dumps(entry) + "\n")
    print(f"Training log saved to {log_path}")
    print(f"Best validation SRCC: {best_srcc:.4f}")

    # ── Optional: correlation with PESQ ───────────────────────────────
    try:
        from pesq import pesq as pesq_fn
        print("\nComputing correlation with PESQ on held-out set...")
        # This requires reference audio which isn't always available.
        print("(Skipped — reference audio paths not available in default CSV format)")
    except ImportError:
        print("\npesq not installed — skipping PESQ correlation analysis")


# ──────────────────────────────────────────────────────────────────────
# CLI
# ──────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Train the MOS regression head (backbone frozen)."
    )
    parser.add_argument(
        "--data_dir", type=str, required=True,
        help="Directory containing CSV + wav files (NISQA or VoiceMOS format).",
    )
    parser.add_argument("--epochs", type=int, default=50)
    parser.add_argument("--batch_size", type=int, default=16)
    parser.add_argument("--lr", type=float, default=1e-4)
    parser.add_argument(
        "--checkpoint_dir", type=str, default="evaluator/checkpoints",
        help="Directory to save checkpoints.",
    )
    parser.add_argument("--val_split", type=float, default=0.15)
    train(parser.parse_args())


if __name__ == "__main__":
    main()
