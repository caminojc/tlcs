"""
Perceptual speech quality evaluator using a frozen WavLM backbone.

The WavLM backbone is ALWAYS frozen. Only the lightweight MOS regression head
is trainable. This design ensures the evaluator learns a stable mapping from
WavLM's pre-trained speech representations to MOS scores without corrupting
the backbone's generalisation ability.

When used as the oracle fitness function for codec optimisation, the entire
model (backbone + head) is frozen so the optimisation target never shifts.
"""

from __future__ import annotations

import math
from typing import List, Optional

import torch
import torch.nn as nn
import torchaudio
from transformers import WavLMModel


class MOSHead(nn.Module):
    """Small MLP that maps WavLM hidden states to a scalar MOS prediction."""

    def __init__(self, input_dim: int = 768, dropout: float = 0.1) -> None:
        super().__init__()
        self.net = nn.Sequential(
            nn.Linear(input_dim, 256),
            nn.GELU(),
            nn.Dropout(dropout),
            nn.Linear(256, 64),
            nn.GELU(),
            nn.Linear(64, 1),
            nn.Sigmoid(),  # output in [0, 1], scaled to [1, 5] below
        )

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        """Map pooled hidden states to MOS in [1.0, 5.0]."""
        raw = self.net(x)  # (B, 1) in [0, 1]
        return raw * 4.0 + 1.0  # scale to [1.0, 5.0]


class PerceptualEvaluator(nn.Module):
    """
    WavLM-based perceptual speech quality evaluator.

    Architecture
    ------------
    - **Backbone**: ``microsoft/wavlm-base-plus`` (frozen — never updated).
    - **Head**: small MLP producing a scalar MOS in [1.0, 5.0].

    Why the backbone is frozen
    --------------------------
    The backbone provides a rich, general-purpose speech representation learnt
    from large-scale self-supervised pre-training.  Freezing it:
      1. Preserves generalisation — fine-tuning on small MOS datasets would
         overfit and degrade the representation quality.
      2. Guarantees oracle integrity — when the evaluator is used as the fitness
         function for codec parameter optimisation, the scoring surface must be
         fixed so the optimiser converges to a genuine optimum rather than
         chasing a shifting target.
    """

    BACKBONE_SAMPLE_RATE: int = 16_000
    WINDOW_SECONDS: float = 3.0
    OVERLAP_RATIO: float = 0.5

    def __init__(
        self,
        backbone_name: str = "microsoft/wavlm-base-plus",
        freeze_backbone: bool = True,
    ) -> None:
        super().__init__()
        self.backbone_name = backbone_name
        self.backbone = WavLMModel.from_pretrained(backbone_name)
        self.head = MOSHead(input_dim=self.backbone.config.hidden_size)

        if freeze_backbone:
            self._freeze_backbone()

    # ------------------------------------------------------------------
    # Backbone freezing
    # ------------------------------------------------------------------

    def _freeze_backbone(self) -> None:
        """Freeze every parameter in the backbone — no gradients, ever."""
        for param in self.backbone.parameters():
            param.requires_grad = False
        self.backbone.eval()

    def backbone_is_frozen(self) -> bool:
        return all(not p.requires_grad for p in self.backbone.parameters())

    # ------------------------------------------------------------------
    # Forward pass
    # ------------------------------------------------------------------

    def forward(
        self,
        waveform: torch.Tensor,
        sample_rate: int = 16_000,
    ) -> torch.Tensor:
        """
        Score a waveform.

        Parameters
        ----------
        waveform : torch.Tensor
            Shape ``(batch, samples)`` or ``(samples,)``.
        sample_rate : int
            Native sample rate of *waveform*. Resampled to 16 kHz internally.

        Returns
        -------
        torch.Tensor
            Scalar MOS prediction(s), shape ``(batch,)`` or ``()``.
        """
        if waveform.dim() == 1:
            waveform = waveform.unsqueeze(0)

        # Resample to 16 kHz if necessary.
        if sample_rate != self.BACKBONE_SAMPLE_RATE:
            waveform = torchaudio.functional.resample(
                waveform, orig_freq=sample_rate, new_freq=self.BACKBONE_SAMPLE_RATE
            )

        device = next(self.parameters()).device
        waveform = waveform.to(device)

        # Chunk into overlapping windows and mean-pool scores.
        scores = []
        for i in range(waveform.size(0)):
            chunks = self._chunk(waveform[i])
            chunk_scores = self._score_chunks(chunks)
            scores.append(chunk_scores.mean())

        return torch.stack(scores).squeeze()

    def _chunk(self, wav: torch.Tensor) -> torch.Tensor:
        """Split a 1-D waveform into 3-second windows with 50 % overlap."""
        window_len = int(self.WINDOW_SECONDS * self.BACKBONE_SAMPLE_RATE)
        hop = int(window_len * (1.0 - self.OVERLAP_RATIO))
        length = wav.size(0)

        if length <= window_len:
            # Pad short audio to window length.
            padded = torch.zeros(window_len, device=wav.device, dtype=wav.dtype)
            padded[:length] = wav
            return padded.unsqueeze(0)

        chunks = []
        start = 0
        while start + window_len <= length:
            chunks.append(wav[start : start + window_len])
            start += hop
        # Include the tail if there's a meaningful remainder.
        remainder = length - start
        if remainder > self.BACKBONE_SAMPLE_RATE:  # > 1 second
            padded = torch.zeros(window_len, device=wav.device, dtype=wav.dtype)
            padded[:remainder] = wav[start:]
            chunks.append(padded)

        return torch.stack(chunks)

    def _score_chunks(self, chunks: torch.Tensor) -> torch.Tensor:
        """Score a batch of waveform chunks through backbone + head."""
        with torch.no_grad() if self.backbone_is_frozen() else torch.enable_grad():
            hidden = self.backbone(chunks).last_hidden_state  # (N, T, D)
        pooled = hidden.mean(dim=1)  # (N, D) — mean-pool over time
        return self.head(pooled).squeeze(-1)  # (N,)

    # ------------------------------------------------------------------
    # Convenience wrappers
    # ------------------------------------------------------------------

    def score(self, audio_path: str) -> float:
        """Load a wav file and return a scalar MOS prediction."""
        waveform, sr = torchaudio.load(audio_path)
        # Mix to mono.
        if waveform.size(0) > 1:
            waveform = waveform.mean(dim=0, keepdim=True)
        waveform = waveform.squeeze(0)
        with torch.no_grad():
            return self.forward(waveform, sample_rate=sr).item()

    def score_batch(self, audio_paths: List[str]) -> List[float]:
        """Score multiple wav files. Returns list of MOS floats."""
        results: List[float] = []
        for path in audio_paths:
            results.append(self.score(path))
        return results
