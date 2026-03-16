"""
Adaptive post-filter for the CELP decoder.

- Formant postfilter: H(z) = A(z/γ₁) / A(z/γ₂)   (γ₁ < γ₂)
- Tilt compensation:  1 - μ·z⁻¹
- Automatic gain control (AGC) to match original loudness
"""
from __future__ import annotations

import numpy as np


class PostFilter:
    """
    Combined formant postfilter + tilt compensation + AGC.
    """

    def __init__(self):
        # Persistent state for filters and AGC
        self._formant_state_num: np.ndarray | None = None
        self._formant_state_den: np.ndarray | None = None
        self._tilt_prev_in: float = 0.0
        self._agc_gain: float = 1.0

    def process(
        self,
        frame: np.ndarray,
        lpc_coeffs: np.ndarray,
        tilt_coeff: float = 0.3,
        formant_coeff: float = 0.5,
        enabled: bool = True,
    ) -> np.ndarray:
        """
        Apply postfilter to one subframe of decoded speech.

        Parameters
        ----------
        frame         : (N,) — decoded speech subframe
        lpc_coeffs    : (order+1,) — LPC coefficients (a[0]=1)
        tilt_coeff    : μ for tilt compensation (higher = more de-tilt)
        formant_coeff : controls spectral shaping strength
        enabled       : bypass if False
        """
        if not enabled:
            return frame.copy()

        order = len(lpc_coeffs) - 1

        # Gamma values for formant postfilter
        gamma_num = 0.65  # numerator (zeros) — slightly inside unit circle
        gamma_den = 0.75 + formant_coeff * 0.2  # denominator (poles) — further in
        # Ensure gamma_num < gamma_den for spectral peak enhancement
        if gamma_num >= gamma_den:
            gamma_den = gamma_num + 0.05

        # Build weighted LPC polynomials
        a_num = _weight_lpc(lpc_coeffs, gamma_num)
        a_den = _weight_lpc(lpc_coeffs, gamma_den)

        # Initialize states if needed
        if self._formant_state_num is None or len(self._formant_state_num) != order:
            self._formant_state_num = np.zeros(order)
            self._formant_state_den = np.zeros(order)

        # ── Formant filter: y = A(z/γ₁) / A(z/γ₂) applied to frame ──
        # First apply numerator (FIR): A(z/γ₁) — analysis
        n = len(frame)
        intermediate = np.zeros(n)
        mem_num = self._formant_state_num.copy()
        for i in range(n):
            val = frame[i]
            for k in range(order):
                val += a_num[k + 1] * mem_num[k]
            intermediate[i] = val
            mem_num = np.roll(mem_num, 1)
            mem_num[0] = frame[i]
        self._formant_state_num = mem_num

        # Then apply denominator (IIR): 1/A(z/γ₂) — synthesis
        filtered = np.zeros(n)
        mem_den = self._formant_state_den.copy()
        for i in range(n):
            val = intermediate[i]
            for k in range(order):
                val -= a_den[k + 1] * mem_den[k]
            filtered[i] = val
            mem_den = np.roll(mem_den, 1)
            mem_den[0] = filtered[i]
        self._formant_state_den = mem_den

        # ── Tilt compensation: H_tilt(z) = 1 - μ·z⁻¹ ────────────────
        tilt_out = np.zeros(n)
        prev_in = self._tilt_prev_in
        for i in range(n):
            tilt_out[i] = filtered[i] - tilt_coeff * prev_in
            prev_in = filtered[i]
        self._tilt_prev_in = prev_in

        # ── AGC ──────────────────────────────────────────────────────
        # Match the energy of the output to the input
        input_energy = np.dot(frame, frame)
        output_energy = np.dot(tilt_out, tilt_out)

        if output_energy > 1e-10:
            target_gain = np.sqrt(input_energy / output_energy)
        else:
            target_gain = 1.0

        # Smooth gain to avoid clicks
        agc_alpha = 0.9
        result = np.zeros(n)
        g = self._agc_gain
        for i in range(n):
            g = agc_alpha * g + (1.0 - agc_alpha) * target_gain
            result[i] = tilt_out[i] * g
        self._agc_gain = g

        return result

    def reset(self) -> None:
        self._formant_state_num = None
        self._formant_state_den = None
        self._tilt_prev_in = 0.0
        self._agc_gain = 1.0


class HarmonicPostFilter:
    """
    Pitch-based harmonic postfilter — enhances voiced speech naturalness.

    H(z) = 1 / (1 - g_h * z^(-T))

    where T = pitch lag, g_h = harmonic postfilter gain.
    Based on SMPL_HARM_POSTF_STRENGTH = 0.713.
    Applied after formant postfilter, before de-emphasis.
    """

    def __init__(self, max_lag: int = 231):
        self._buffer = np.zeros(max_lag + 320)  # history buffer
        self._buf_pos = max_lag
        self.strength = 0.713       # SMPL default — optimizer tunes this
        self.fb_strength = 0.4      # feedback strength

    def process(self, frame: np.ndarray, pitch_lag: int,
                voiced: bool = True) -> np.ndarray:
        """Apply harmonic postfilter to one subframe."""
        if not voiced or pitch_lag <= 0:
            return frame.copy()

        n = len(frame)
        out = np.zeros(n)
        strength = self.strength
        fb = self.fb_strength

        for i in range(n):
            # Pitch-delayed sample from history
            delay_idx = self._buf_pos + i - pitch_lag
            if delay_idx >= 0 and delay_idx < len(self._buffer):
                pitch_val = self._buffer[delay_idx]
            else:
                pitch_val = 0.0

            # Comb filter: enhance pitch harmonics
            out[i] = frame[i] + strength * pitch_val

            # Update history with feedback
            self._buffer[self._buf_pos + i] = frame[i] + fb * pitch_val

        # Shift buffer
        shift = n
        self._buffer[:len(self._buffer) - shift] = self._buffer[shift:]
        self._buffer[len(self._buffer) - shift:] = 0.0

        # AGC: match energy
        in_energy = np.dot(frame, frame)
        out_energy = np.dot(out, out)
        if out_energy > 1e-10:
            out *= np.sqrt(in_energy / out_energy)

        return out

    def reset(self) -> None:
        self._buffer[:] = 0.0


def _weight_lpc(a: np.ndarray, gamma: float) -> np.ndarray:
    """Scale LPC coefficients: a_weighted[k] = a[k] * gamma^k."""
    weighted = a.copy()
    for k in range(1, len(a)):
        weighted[k] *= gamma ** k
    return weighted
