"""
Pre-processing filters for the CELP codec.

- 2nd-order IIR high-pass filter (DC removal / low-freq noise)
- 1st-order FIR pre-emphasis filter
Both preserve state across frames.
"""
from __future__ import annotations

import numpy as np


class HighPassFilter:
    """
    2nd-order Butterworth high-pass filter (direct-form II transposed).

    Coefficients are computed once from the cutoff and sample rate.
    """

    def __init__(self, cutoff_hz: float, sample_rate: int):
        self.cutoff_hz = cutoff_hz
        self.sample_rate = sample_rate
        # Bilinear-transform 2nd-order Butterworth HPF
        wc = 2.0 * np.pi * cutoff_hz / sample_rate
        k = np.tan(wc / 2.0)
        k2 = k * k
        sqrt2 = np.sqrt(2.0)
        norm = 1.0 / (1.0 + sqrt2 * k + k2)

        self.b0 = norm
        self.b1 = -2.0 * norm
        self.b2 = norm
        self.a1 = 2.0 * (k2 - 1.0) * norm
        self.a2 = (1.0 - sqrt2 * k + k2) * norm

        # State (direct-form II transposed)
        self._z1 = 0.0
        self._z2 = 0.0

    def process(self, frame: np.ndarray) -> np.ndarray:
        """Filter a frame of samples in-place order."""
        out = np.empty_like(frame, dtype=np.float64)
        for i in range(len(frame)):
            x = float(frame[i])
            y = self.b0 * x + self._z1
            self._z1 = self.b1 * x - self.a1 * y + self._z2
            self._z2 = self.b2 * x - self.a2 * y
            out[i] = y
        return out

    def reset(self) -> None:
        self._z1 = 0.0
        self._z2 = 0.0


class PreEmphasisFilter:
    """
    1st-order FIR pre-emphasis: y[n] = x[n] - coeff * x[n-1].
    """

    def __init__(self, coeff: float):
        self.coeff = coeff
        self._prev = 0.0

    def process(self, frame: np.ndarray) -> np.ndarray:
        out = np.empty_like(frame, dtype=np.float64)
        prev = self._prev
        for i in range(len(frame)):
            x = float(frame[i])
            out[i] = x - self.coeff * prev
            prev = x
        self._prev = prev
        return out

    def reset(self) -> None:
        self._prev = 0.0


class DeEmphasisFilter:
    """
    Inverse of pre-emphasis: y[n] = x[n] + coeff * y[n-1].
    Used in the decoder to undo pre-emphasis.
    """

    def __init__(self, coeff: float):
        self.coeff = coeff
        self._prev = 0.0

    def process(self, frame: np.ndarray) -> np.ndarray:
        out = np.empty_like(frame, dtype=np.float64)
        prev = self._prev
        for i in range(len(frame)):
            out[i] = float(frame[i]) + self.coeff * prev
            prev = out[i]
        self._prev = prev
        return out

    def reset(self) -> None:
        self._prev = 0.0
