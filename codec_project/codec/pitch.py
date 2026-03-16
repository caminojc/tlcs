"""
Pitch (adaptive codebook) analysis for the CELP codec.

- Open-loop pitch estimation via autocorrelation
- Closed-loop pitch refinement with impulse-response weighting
- Fractional pitch interpolation (sinc-based)
"""
from __future__ import annotations

import numpy as np


# ═══════════════════════════════════════════════════════════════════════════
# Sinc interpolation table for fractional pitch
# ═══════════════════════════════════════════════════════════════════════════

_SINC_LEN = 11  # taps on each side


def _sinc_table(resolution: int) -> np.ndarray:
    """
    Pre-compute windowed-sinc coefficients for fractional delays.

    resolution: number of sub-sample phases (2 for half, 3 for third, …)
    Returns shape (resolution, 2*_SINC_LEN + 1).
    """
    taps = 2 * _SINC_LEN + 1
    table = np.zeros((resolution, taps))
    for phase in range(resolution):
        frac = phase / resolution
        for k in range(-_SINC_LEN, _SINC_LEN + 1):
            x = k - frac
            # Hamming-windowed sinc
            sinc_val = np.sinc(x)
            win = 0.54 + 0.46 * np.cos(np.pi * (k - frac) / (_SINC_LEN + 1))
            table[phase, k + _SINC_LEN] = sinc_val * win
    return table


# Pre-built tables
_TABLE_HALF = _sinc_table(2)
_TABLE_THIRD = _sinc_table(3)


# ═══════════════════════════════════════════════════════════════════════════
# Fractional pitch interpolation
# ═══════════════════════════════════════════════════════════════════════════

def fractional_pitch_interpolate(
    signal: np.ndarray,
    lag: float,
    fractional_mode: str = "third",
) -> np.ndarray:
    """
    Extract a pitch-period excitation from *signal* at fractional *lag*.

    Parameters
    ----------
    signal : past excitation buffer (at least lag + _SINC_LEN samples long)
    lag    : pitch lag in samples (may be fractional)
    fractional_mode : "integer", "half", or "third"

    Returns
    -------
    Excitation vector of length = subframe size (caller slices).
    """
    if fractional_mode == "integer":
        int_lag = int(round(lag))
        start = len(signal) - int_lag
        if start < 0:
            out = np.zeros(int_lag)
            out[-start:] = signal[:int_lag + start]
            return out
        return signal[start:start + int(lag)]

    int_lag = int(np.floor(lag))
    frac = lag - int_lag

    if fractional_mode == "half":
        table = _TABLE_HALF
        resolution = 2
    else:  # "third"
        table = _TABLE_THIRD
        resolution = 3

    phase = int(round(frac * resolution)) % resolution
    coeffs = table[phase]

    # Build the interpolated sample at each position
    n = len(signal)
    start = n - int_lag
    out_len = int_lag if int_lag > 0 else 1
    out = np.zeros(out_len)
    for i in range(out_len):
        pos = start + i
        val = 0.0
        for k in range(-_SINC_LEN, _SINC_LEN + 1):
            idx = pos + k
            if 0 <= idx < n:
                val += coeffs[k + _SINC_LEN] * signal[idx]
        out[i] = val
    return out


# ═══════════════════════════════════════════════════════════════════════════
# Open-loop pitch estimation
# ═══════════════════════════════════════════════════════════════════════════

def open_loop_pitch_estimate(
    residual: np.ndarray,
    min_lag: int,
    max_lag: int,
) -> int:
    """
    Open-loop pitch estimate via normalized autocorrelation.

    Returns the integer lag with highest correlation.
    """
    n = len(residual)
    best_lag = min_lag
    best_corr = -1e30

    for lag in range(min_lag, min(max_lag + 1, n)):
        # Cross-correlation at this lag
        seg = residual[:n - lag]
        ref = residual[lag:]
        minlen = min(len(seg), len(ref))
        seg = seg[:minlen]
        ref = ref[:minlen]

        energy = np.dot(ref, ref)
        if energy < 1e-10:
            continue
        corr = np.dot(seg, ref) / np.sqrt(np.dot(seg, seg) * energy + 1e-10)
        if corr > best_corr:
            best_corr = corr
            best_lag = lag

    return best_lag


# ═══════════════════════════════════════════════════════════════════════════
# Closed-loop pitch search
# ═══════════════════════════════════════════════════════════════════════════

def closed_loop_pitch_search(
    target: np.ndarray,
    impulse_response: np.ndarray,
    prev_excitation: np.ndarray,
    min_lag: int,
    max_lag: int,
    fractional: str = "third",
) -> tuple[float, float]:
    """
    Closed-loop (analysis-by-synthesis) pitch search.

    Finds the lag that minimises the weighted error:
        || target - gain * H * v(lag) ||^2
    where H is the impulse-response convolution matrix and v(lag)
    is the adaptive codebook vector.

    Parameters
    ----------
    target          : (subframe_size,) — target signal (after removing
                      zero-state response)
    impulse_response: (subframe_size,) — truncated impulse response of W(z)/A(z)
    prev_excitation : past excitation buffer
    min_lag, max_lag: search range (integer)
    fractional      : "integer", "half", or "third"

    Returns
    -------
    optimal_lag  : float (may be fractional)
    pitch_gain   : float
    """
    subframe_size = len(target)

    # ── Integer search ────────────────────────────────────────────────
    best_lag = min_lag
    best_corr = -1e30
    best_energy = 1.0

    for lag in range(min_lag, min(max_lag + 1, len(prev_excitation))):
        v = _build_acb_vector(prev_excitation, lag, subframe_size)
        # Filter through impulse response
        fv = _convolve_truncated(v, impulse_response, subframe_size)

        corr = np.dot(target, fv)
        energy = np.dot(fv, fv) + 1e-10

        # Maximise corr^2 / energy (equivalent to minimising error)
        score = corr * corr / energy
        if score > best_corr:
            best_corr = score
            best_lag = lag
            best_energy = energy

    # ── Fractional refinement ─────────────────────────────────────────
    if fractional == "integer":
        v = _build_acb_vector(prev_excitation, best_lag, subframe_size)
        fv = _convolve_truncated(v, impulse_response, subframe_size)
        gain = np.dot(target, fv) / (np.dot(fv, fv) + 1e-10)
        gain = np.clip(gain, 0.0, 1.2)
        return float(best_lag), float(gain)

    if fractional == "half":
        fracs = [-0.5, 0.0, 0.5]
    else:  # "third"
        fracs = [-2 / 3, -1 / 3, 0.0, 1 / 3, 2 / 3]

    best_frac_lag = float(best_lag)
    best_frac_score = -1e30

    for df in fracs:
        frac_lag = best_lag + df
        if frac_lag < min_lag or frac_lag > max_lag:
            continue
        v = _build_acb_vector_frac(prev_excitation, frac_lag, subframe_size, fractional)
        fv = _convolve_truncated(v, impulse_response, subframe_size)
        corr = np.dot(target, fv)
        energy = np.dot(fv, fv) + 1e-10
        score = corr * corr / energy
        if score > best_frac_score:
            best_frac_score = score
            best_frac_lag = frac_lag

    # Compute final gain
    v = _build_acb_vector_frac(prev_excitation, best_frac_lag, subframe_size, fractional)
    fv = _convolve_truncated(v, impulse_response, subframe_size)
    gain = np.dot(target, fv) / (np.dot(fv, fv) + 1e-10)
    gain = np.clip(gain, 0.0, 1.2)

    return float(best_frac_lag), float(gain)


# ═══════════════════════════════════════════════════════════════════════════
# Helpers
# ═══════════════════════════════════════════════════════════════════════════

def _build_acb_vector(
    prev_excitation: np.ndarray,
    lag: int,
    length: int,
) -> np.ndarray:
    """Build adaptive codebook vector by pitch-repeating past excitation."""
    n = len(prev_excitation)
    v = np.zeros(length)
    for i in range(length):
        idx = n - lag + i
        # Wrap for pitch-period repetition
        while idx < 0:
            idx += lag
        if idx < n:
            v[i] = prev_excitation[idx]
        else:
            # Beyond buffer — use wrap
            wrap_idx = (idx - n) % lag
            src = n - lag + wrap_idx
            if 0 <= src < n:
                v[i] = prev_excitation[src]
    return v


def _build_acb_vector_frac(
    prev_excitation: np.ndarray,
    frac_lag: float,
    length: int,
    fractional_mode: str,
) -> np.ndarray:
    """Build ACB vector with fractional-lag interpolation."""
    int_lag = int(round(frac_lag))
    if fractional_mode == "integer" or abs(frac_lag - int_lag) < 1e-6:
        return _build_acb_vector(prev_excitation, int_lag, length)

    n = len(prev_excitation)
    v = np.zeros(length)

    if fractional_mode == "half":
        table = _TABLE_HALF
        resolution = 2
    else:
        table = _TABLE_THIRD
        resolution = 3

    int_part = int(np.floor(frac_lag))
    frac_part = frac_lag - int_part
    phase = int(round(frac_part * resolution)) % resolution
    coeffs = table[phase]

    for i in range(length):
        pos = n - int_part + i
        val = 0.0
        for k in range(-_SINC_LEN, _SINC_LEN + 1):
            idx = pos + k
            if 0 <= idx < n:
                val += coeffs[k + _SINC_LEN] * prev_excitation[idx]
        v[i] = val
    return v


def _convolve_truncated(
    x: np.ndarray,
    h: np.ndarray,
    length: int,
) -> np.ndarray:
    """Causal convolution of x with h, truncated to *length* samples."""
    result = np.zeros(length)
    for i in range(length):
        for k in range(i + 1):
            if k < len(x) and (i - k) < len(h):
                result[i] += x[k] * h[i - k]
    return result


def build_adaptive_codebook_excitation(
    prev_excitation: np.ndarray,
    lag: float,
    subframe_size: int,
    fractional_mode: str = "third",
) -> np.ndarray:
    """Public API: build the adaptive codebook contribution."""
    return _build_acb_vector_frac(prev_excitation, lag, subframe_size, fractional_mode)
