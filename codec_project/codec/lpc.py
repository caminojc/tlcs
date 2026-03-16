"""
LPC analysis and synthesis for the CELP codec.

- Levinson-Durbin algorithm (pure numpy)
- LPC <-> LSP conversion
- LSP interpolation for subframe smoothing
- All-pole synthesis filter
"""
from __future__ import annotations

import numpy as np


# ═══════════════════════════════════════════════════════════════════════════
# Bandwidth Expansion
# ═══════════════════════════════════════════════════════════════════════════

def apply_bandwidth_expansion(lpc: np.ndarray, gamma: float = 0.9985) -> np.ndarray:
    """
    Multiply lpc[k] by gamma^k for stability.
    gamma close to 1.0 = slight spectral flattening.
    SMPL default: 0.9985. Optimizer range: [0.990, 0.9999].
    """
    result = lpc.copy()
    for k in range(1, len(result)):
        result[k] *= gamma ** k
    return result


# ═══════════════════════════════════════════════════════════════════════════
# Windows
# ═══════════════════════════════════════════════════════════════════════════

def _make_window(window_type: str, length: int) -> np.ndarray:
    if window_type == "hamming":
        return np.hamming(length)
    elif window_type == "hanning":
        return np.hanning(length)
    elif window_type == "blackman":
        return np.blackman(length)
    else:
        raise ValueError(f"Unknown window: {window_type}")


# ═══════════════════════════════════════════════════════════════════════════
# Autocorrelation + Levinson-Durbin
# ═══════════════════════════════════════════════════════════════════════════

def _autocorrelation(signal: np.ndarray, order: int) -> np.ndarray:
    """Biased autocorrelation of *signal* for lags 0 … order."""
    n = len(signal)
    r = np.zeros(order + 1)
    for k in range(order + 1):
        r[k] = np.dot(signal[:n - k], signal[k:])
    # Bandwidth expansion (lag windowing) for stability
    bw = 60.0  # Hz equivalent bandwidth expansion
    for k in range(order + 1):
        r[k] *= np.exp(-0.5 * (2.0 * np.pi * bw * k / 8000.0) ** 2)
    return r


def _levinson_durbin(r: np.ndarray, order: int):
    """
    Levinson-Durbin recursion.

    Returns
    -------
    a : ndarray, shape (order + 1,) — LPC coefficients (a[0] = 1.0)
    e : float — prediction error energy
    """
    a = np.zeros(order + 1)
    a[0] = 1.0
    e = r[0]
    if e <= 0:
        return a, 1e-10

    for i in range(1, order + 1):
        lam = -np.dot(a[1:i], r[i - 1:0:-1]) - r[i]
        ki = lam / e
        # Clamp reflection coefficient for stability
        ki = np.clip(ki, -0.9999, 0.9999)
        a_new = a.copy()
        for j in range(1, i):
            a_new[j] = a[j] + ki * a[i - j]
        a_new[i] = ki
        a = a_new
        e = e * (1.0 - ki * ki)
        if e <= 0:
            e = 1e-10
    return a, e


def lpc_analysis(
    frame: np.ndarray,
    order: int,
    window_type: str = "hamming",
    window_size: int | None = None,
) -> tuple[np.ndarray, np.ndarray, float]:
    """
    Compute LPC coefficients, residual, and prediction gain.

    Parameters
    ----------
    frame : 1-D float array — speech frame (pre-emphasised)
    order : int — LPC order
    window_type : str — window function name
    window_size : int or None — analysis window length (None = len(frame))

    Returns
    -------
    lpc_coeffs : (order + 1,) — LPC filter [1, a1, a2, …]
    residual   : same length as *frame* — prediction residual
    gain       : float — sqrt of prediction error energy
    """
    if window_size is None:
        window_size = len(frame)

    # Apply analysis window (left-aligned if window > frame)
    n = min(window_size, len(frame))
    windowed = np.zeros(window_size)
    windowed[:n] = frame[:n] * _make_window(window_type, n)

    r = _autocorrelation(windowed, order)
    lpc_coeffs, error_energy = _levinson_durbin(r, order)
    gain = np.sqrt(max(error_energy, 1e-10))

    # Apply bandwidth expansion for stability
    lpc_coeffs = apply_bandwidth_expansion(lpc_coeffs)

    # Compute residual: e[n] = sum_k a[k]*x[n-k]
    residual = np.zeros(len(frame))
    for n_idx in range(len(frame)):
        val = 0.0
        for k in range(order + 1):
            if n_idx - k >= 0:
                val += lpc_coeffs[k] * frame[n_idx - k]
        residual[n_idx] = val

    return lpc_coeffs, residual, gain


# ═══════════════════════════════════════════════════════════════════════════
# LPC ↔ LSP (Line Spectral Pair) conversion
# ═══════════════════════════════════════════════════════════════════════════

def lpc_to_lsp(lpc_coeffs: np.ndarray) -> np.ndarray:
    """
    Convert LPC coefficients to Line Spectral Pair frequencies.

    Uses numpy root-finding on P(z) = A(z) + z^{-(p+1)}A(z^{-1}) and
    Q(z) = A(z) - z^{-(p+1)}A(z^{-1}).  Returns LSP frequencies in (0, pi).
    """
    order = len(lpc_coeffs) - 1
    a = lpc_coeffs.copy()

    # Extend a with a trailing 0 for the symmetric construction
    a_ext = np.zeros(order + 2)
    a_ext[:order + 1] = a

    # P(z) coefficients: a[i] + a[order+1-i]
    p = np.zeros(order + 2)
    q = np.zeros(order + 2)
    for i in range(order + 2):
        p[i] = a_ext[i] + a_ext[order + 1 - i]
        q[i] = a_ext[i] - a_ext[order + 1 - i]

    # Find roots of P and Q as polynomials (in z^{-1} power series)
    # np.roots expects highest power first, but our coefficients are
    # in ascending powers of z^{-1}, so reverse them.
    p_roots_all = np.roots(p[::-1])
    q_roots_all = np.roots(q[::-1])

    # Extract roots on the unit circle (|root| ≈ 1) and take their angles
    def _unit_circle_angles(roots):
        angles = []
        for r in roots:
            if abs(abs(r) - 1.0) < 0.02:  # close to unit circle
                angle = np.angle(r)
                if angle < 0:
                    angle += 2 * np.pi
                if 0.0 < angle < np.pi:
                    angles.append(float(angle))
        return sorted(angles)

    p_angles = _unit_circle_angles(p_roots_all)
    q_angles = _unit_circle_angles(q_roots_all)

    # Merge and sort
    lsp_freqs = sorted(p_angles + q_angles)

    if len(lsp_freqs) < order:
        # Fallback: evenly spaced
        lsp_freqs = np.linspace(0.05, np.pi - 0.05, order).tolist()
    lsp_freqs = lsp_freqs[:order]

    # Enforce strict ordering with minimum gap
    min_gap = 0.005
    for i in range(1, len(lsp_freqs)):
        if lsp_freqs[i] <= lsp_freqs[i - 1] + min_gap:
            lsp_freqs[i] = lsp_freqs[i - 1] + min_gap
    for i in range(len(lsp_freqs)):
        lsp_freqs[i] = max(0.001, min(lsp_freqs[i], np.pi - 0.001))

    return np.array(lsp_freqs)


def lsp_to_lpc(lsp_freqs: np.ndarray) -> np.ndarray:
    """
    Convert LSP frequencies back to LPC coefficients.

    Reconstructs P(z) and Q(z) from the LSP roots, then recovers
    A(z) = 0.5 * (P(z) + Q(z)).  Even-indexed LSPs are P roots,
    odd-indexed are Q roots.
    """
    order = len(lsp_freqs)

    # Even-indexed LSPs belong to P, odd to Q
    p_freqs = lsp_freqs[0::2]
    q_freqs = lsp_freqs[1::2]

    # Build P'(z) from its roots: product of (1 - 2*cos(w)*z^{-1} + z^{-2})
    p_prime = np.array([1.0])
    for f in p_freqs:
        factor = np.array([1.0, -2.0 * np.cos(f), 1.0])
        p_prime = np.convolve(p_prime, factor)

    # Build Q'(z) from its roots
    q_prime = np.array([1.0])
    for f in q_freqs:
        factor = np.array([1.0, -2.0 * np.cos(f), 1.0])
        q_prime = np.convolve(q_prime, factor)

    # Reconvolve: P(z) = (1+z^{-1})*P'(z),  Q(z) = (1-z^{-1})*Q'(z)
    p_poly = np.convolve(p_prime, [1.0, 1.0])
    q_poly = np.convolve(q_prime, [1.0, -1.0])

    # A(z) = 0.5 * (P(z) + Q(z))
    max_len = max(len(p_poly), len(q_poly))
    p_padded = np.zeros(max_len)
    q_padded = np.zeros(max_len)
    p_padded[:len(p_poly)] = p_poly
    q_padded[:len(q_poly)] = q_poly

    a = 0.5 * (p_padded + q_padded)
    if abs(a[0]) > 1e-12:
        a /= a[0]
    return a[:order + 1]


def lsp_interpolate(
    lsp_prev: np.ndarray,
    lsp_curr: np.ndarray,
    alpha: float,
) -> np.ndarray:
    """
    Linear interpolation between LSP vectors.

    alpha = 0 → lsp_prev, alpha = 1 → lsp_curr.
    """
    return (1.0 - alpha) * lsp_prev + alpha * lsp_curr


# ═══════════════════════════════════════════════════════════════════════════
# LPC synthesis filter
# ═══════════════════════════════════════════════════════════════════════════

def lpc_synthesis_filter(
    excitation: np.ndarray,
    lpc_coeffs: np.ndarray,
    state: np.ndarray | None = None,
) -> tuple[np.ndarray, np.ndarray]:
    """
    All-pole synthesis: s[n] = exc[n] - sum_{k=1}^{P} a[k]*s[n-k]

    Parameters
    ----------
    excitation : 1-D array
    lpc_coeffs : (order+1,) — a[0]=1
    state      : (order,) or None — filter memory from previous subframe

    Returns
    -------
    speech    : same length as excitation
    new_state : (order,) — updated filter memory
    """
    order = len(lpc_coeffs) - 1
    if state is None:
        state = np.zeros(order)

    n = len(excitation)
    speech = np.zeros(n)
    mem = state.copy()

    for i in range(n):
        val = excitation[i]
        for k in range(order):
            val -= lpc_coeffs[k + 1] * mem[k]
        speech[i] = val
        # Shift memory
        mem = np.roll(mem, 1)
        mem[0] = speech[i]

    return speech, mem


def lpc_zero_state_response(
    lpc_coeffs: np.ndarray,
    state: np.ndarray,
    length: int,
) -> np.ndarray:
    """
    Zero-input response of the synthesis filter (ringing from past state).
    """
    excitation = np.zeros(length)
    response, _ = lpc_synthesis_filter(excitation, lpc_coeffs, state)
    return response


def compute_impulse_response(
    lpc_coeffs: np.ndarray,
    length: int,
) -> np.ndarray:
    """Impulse response of 1/A(z) for the given LPC coefficients."""
    impulse = np.zeros(length)
    impulse[0] = 1.0
    h, _ = lpc_synthesis_filter(impulse, lpc_coeffs)
    return h
