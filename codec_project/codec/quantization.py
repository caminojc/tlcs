"""
Vector quantization for the CELP codec.

- SplitVQ: split vector quantization for LSP parameters
- GainQuantizer: joint pitch + codebook gain VQ

Codebooks are plain numpy arrays, trained offline via K-means,
saved/loaded as .npy files.  Zero ML at runtime.
"""
from __future__ import annotations

import os
from pathlib import Path
from typing import List, Tuple

import numpy as np


# ═══════════════════════════════════════════════════════════════════════════
# Split Vector Quantizer (LSPs)
# ═══════════════════════════════════════════════════════════════════════════

class SplitVQ:
    """
    Split vector quantizer for LSP parameters.

    The LSP vector of dimension *dim* is split into *num_splits* sub-vectors,
    each independently quantised against a codebook of size *codebook_size*.
    """

    def __init__(
        self,
        codebook_size: int = 64,
        num_splits: int = 5,
        dim: int = 10,
    ):
        self.codebook_size = codebook_size
        self.num_splits = num_splits
        self.dim = dim
        # Sizes of each split (handle uneven division)
        base, extra = divmod(dim, num_splits)
        self.split_sizes: List[int] = [base + (1 if i < extra else 0)
                                        for i in range(num_splits)]
        # Codebooks: one per split, each shape (codebook_size, split_dim)
        self.codebooks: List[np.ndarray] = [
            self._init_codebook(sz) for sz in self.split_sizes
        ]

    def _init_codebook(self, split_dim: int) -> np.ndarray:
        """Initialise a codebook with linearly spaced entries in [0, pi]."""
        cb = np.zeros((self.codebook_size, split_dim))
        for i in range(self.codebook_size):
            lo = np.pi * i / self.codebook_size
            hi = np.pi * (i + 1) / self.codebook_size
            cb[i] = np.linspace(lo, hi, split_dim)
        return cb

    # ── training ──────────────────────────────────────────────────────────

    def train(self, data: np.ndarray, max_iter: int = 50) -> None:
        """
        Train codebooks from data using K-means.

        Parameters
        ----------
        data : (N, dim) — training LSP vectors
        """
        splits = self._split(data)
        for s, sub_data in enumerate(splits):
            self.codebooks[s] = _kmeans(
                sub_data, self.codebook_size, max_iter=max_iter
            )

    # ── quantize / dequantize ─────────────────────────────────────────────

    def quantize(
        self, lsp: np.ndarray
    ) -> Tuple[List[int], np.ndarray]:
        """
        Quantize an LSP vector.

        Returns (indices, quantized_lsp).
        """
        parts = self._split_single(lsp)
        indices = []
        q_parts = []
        for s, part in enumerate(parts):
            idx = self._nearest(self.codebooks[s], part)
            indices.append(idx)
            q_parts.append(self.codebooks[s][idx])
        return indices, np.concatenate(q_parts)

    def dequantize(self, indices: List[int]) -> np.ndarray:
        """Reconstruct LSP from codebook indices."""
        parts = [self.codebooks[s][idx] for s, idx in enumerate(indices)]
        return np.concatenate(parts)

    # ── persistence ───────────────────────────────────────────────────────

    def save(self, directory: str) -> None:
        os.makedirs(directory, exist_ok=True)
        for s, cb in enumerate(self.codebooks):
            np.save(os.path.join(directory, f"lsp_cb_split{s}.npy"), cb)

    def load(self, directory: str) -> None:
        for s in range(self.num_splits):
            path = os.path.join(directory, f"lsp_cb_split{s}.npy")
            if os.path.exists(path):
                self.codebooks[s] = np.load(path)

    # ── internals ─────────────────────────────────────────────────────────

    def _split_single(self, vec: np.ndarray) -> List[np.ndarray]:
        parts = []
        offset = 0
        for sz in self.split_sizes:
            parts.append(vec[offset:offset + sz])
            offset += sz
        return parts

    def _split(self, data: np.ndarray) -> List[np.ndarray]:
        """Split (N, dim) into a list of (N, split_dim) arrays."""
        parts = []
        offset = 0
        for sz in self.split_sizes:
            parts.append(data[:, offset:offset + sz])
            offset += sz
        return parts

    @staticmethod
    def _nearest(codebook: np.ndarray, vec: np.ndarray) -> int:
        dists = np.sum((codebook - vec[np.newaxis, :]) ** 2, axis=1)
        return int(np.argmin(dists))


# ═══════════════════════════════════════════════════════════════════════════
# Gain Quantizer
# ═══════════════════════════════════════════════════════════════════════════

class GainQuantizer:
    """
    Joint pitch-gain + codebook-gain vector quantizer.

    Each codebook entry is a 2-D vector (pitch_gain, cb_gain).
    Pitch gain is typically in [0, 1.2], codebook gain in a wider range.
    """

    def __init__(self, codebook_size: int = 64):
        self.codebook_size = codebook_size
        # Rectangular grid: few pitch-gain levels (usually 0 or ~1) ×
        # many codebook-gain levels (needs fine resolution).
        # Use 8 pitch-gain levels × 8 cb-gain levels = 64.
        n_pg = 8
        n_cg = codebook_size // n_pg
        if n_cg < 2:
            n_cg = 2
        pg = np.linspace(0.0, 1.2, n_pg)
        # Log-spaced codebook gains: dense near small values where speech
        # codebook gains typically fall, sparse at high values.
        cg = np.geomspace(0.02, 3.0, n_cg)
        grid = np.array(np.meshgrid(pg, cg)).T.reshape(-1, 2)
        # Trim or pad to exact codebook_size
        if len(grid) >= codebook_size:
            self.codebook = grid[:codebook_size].copy()
        else:
            extra = codebook_size - len(grid)
            rng = np.random.RandomState(0)
            pad = rng.rand(extra, 2) * np.array([1.2, 3.0])
            self.codebook = np.vstack([grid, pad])

    def train(self, data: np.ndarray, max_iter: int = 50) -> None:
        """Train from (N, 2) gain pairs."""
        self.codebook = _kmeans(data, self.codebook_size, max_iter=max_iter)

    def quantize(
        self, pitch_gain: float, cb_gain: float
    ) -> Tuple[int, float, float]:
        """
        Quantize a (pitch_gain, cb_gain) pair.

        Returns (index, quantized_pitch_gain, quantized_cb_gain).
        """
        vec = np.array([pitch_gain, cb_gain])
        dists = np.sum((self.codebook - vec[np.newaxis, :]) ** 2, axis=1)
        idx = int(np.argmin(dists))
        return idx, float(self.codebook[idx, 0]), float(self.codebook[idx, 1])

    def dequantize(self, index: int) -> Tuple[float, float]:
        """Returns (pitch_gain, cb_gain)."""
        entry = self.codebook[index]
        return float(entry[0]), float(entry[1])

    def save(self, path: str) -> None:
        np.save(path, self.codebook)

    def load(self, path: str) -> None:
        if os.path.exists(path):
            self.codebook = np.load(path)


# ═══════════════════════════════════════════════════════════════════════════
# K-means (offline training helper — no sklearn dependency)
# ═══════════════════════════════════════════════════════════════════════════

def _kmeans(
    data: np.ndarray,
    k: int,
    max_iter: int = 50,
) -> np.ndarray:
    """
    Simple K-means clustering.  Returns centroids of shape (k, dim).
    """
    n, d = data.shape
    if n <= k:
        # Not enough data — pad with data mean
        centroids = np.zeros((k, d))
        centroids[:n] = data
        centroids[n:] = data.mean(axis=0)
        return centroids

    # Init: random selection
    rng = np.random.RandomState(42)
    indices = rng.choice(n, size=k, replace=False)
    centroids = data[indices].copy()

    for _ in range(max_iter):
        # Assign
        dists = np.sum(
            (data[:, np.newaxis, :] - centroids[np.newaxis, :, :]) ** 2,
            axis=2,
        )
        labels = np.argmin(dists, axis=1)

        # Update
        new_centroids = np.zeros_like(centroids)
        for c in range(k):
            members = data[labels == c]
            if len(members) > 0:
                new_centroids[c] = members.mean(axis=0)
            else:
                new_centroids[c] = centroids[c]

        if np.allclose(centroids, new_centroids, atol=1e-6):
            break
        centroids = new_centroids

    return centroids
