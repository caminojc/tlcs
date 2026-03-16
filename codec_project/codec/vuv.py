"""
Voiced/Unvoiced decision for the CELP codec.

Based on SMPL voicing parameters:
  SMPL_VUV_BIAS = -0.13  (tends toward unvoiced — avoids buzzy artifacts)
  SMPL_VUV_HYST = 0.05   (smooths transitions)
"""
from __future__ import annotations


def voicing_decision(
    pitch_corr: float,
    prev_voiced: bool,
    bias: float = -0.13,
    hysteresis: float = 0.05,
) -> bool:
    """
    Returns True if frame is voiced.

    Parameters
    ----------
    pitch_corr : normalized pitch correlation from pitch search [0, 1]
    prev_voiced : previous frame voicing state
    bias : negative = conservative (avoids buzzy artifacts on unvoiced)
    hysteresis : smooths transitions between voiced/unvoiced
    """
    threshold = 0.5 + bias
    if prev_voiced:
        threshold -= hysteresis  # harder to switch away from voiced
    return float(pitch_corr) > threshold
