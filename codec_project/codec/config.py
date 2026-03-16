"""
CodecConfig: fully parameterized configuration for the CELP/ACELP codec.

Every meaningful DSP hyperparameter is exposed so the optimizer can search
the parameter space. Zero ML — pure DSP at runtime.
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field, asdict, fields
from typing import Dict, Any


@dataclass
class CodecConfig:
    """Tunable CELP codec configuration."""

    # ── Core ──────────────────────────────────────────────────────────────
    sample_rate: int = 16000         # 16 kHz wideband
    frame_size_ms: float = 20.0      # frame duration
    subframe_size_ms: float = 5.0    # subframe duration (4 subframes of 80 samples)
    bitrate_bps: int = 8000          # target bitrate

    # ── Pre-processing ────────────────────────────────────────────────────
    highpass_cutoff_hz: float = 20.0
    preemphasis_coeff: float = 0.68  # [0.5, 0.9]

    # ── LPC ───────────────────────────────────────────────────────────────
    lpc_order: int = 16              # 16 for wideband
    lpc_window_type: str = "hamming" # "hamming", "hanning", "blackman"
    lpc_window_size_ms: float = 30.0
    lpc_bwe: float = 0.9985         # bandwidth expansion (SMPL default)

    # ── LSP quantization ──────────────────────────────────────────────────
    lsp_num_splits: int = 4          # 4 splits × 8 bits = 32 bits
    lsp_codebook_size: int = 256     # 8 bits per split

    # ── Pitch search ──────────────────────────────────────────────────────
    pitch_min_lag: int = 32          # samples  (~500 Hz @ 16 kHz)
    pitch_max_lag: int = 231         # samples  (~69 Hz  @ 16 kHz)
    pitch_fractional: str = "third"  # "integer", "half", "third"
    pitch_gain_bits: int = 4
    pitch_delta_weight: float = 0.3  # SMPL_PITCH_DELTAWGHT
    pitch_prev_weight: float = 0.7   # SMPL_PITCH_PREVWGHT

    # ── Algebraic codebook (ISPP) ─────────────────────────────────────────
    acb_num_pulses: int = 2          # 2 pulses at 8kbps (12 bits/subfr)
    acb_num_tracks: int = 4
    acb_gain_bits: int = 4

    # ── Joint gain quantization ───────────────────────────────────────────
    gain_codebook_size: int = 64

    # ── VUV decision ──────────────────────────────────────────────────────
    vuv_bias: float = -0.13          # SMPL_VUV_BIAS
    vuv_hysteresis: float = 0.05     # SMPL_VUV_HYST

    # ── Post-filter ───────────────────────────────────────────────────────
    postfilter_enabled: bool = True
    postfilter_tilt_coeff: float = 0.3
    postfilter_formant_coeff: float = 0.5

    # ── Derived (computed, not stored) ────────────────────────────────────

    @property
    def frame_size(self) -> int:
        """Frame size in samples."""
        return int(self.sample_rate * self.frame_size_ms / 1000.0)

    @property
    def subframe_size(self) -> int:
        """Subframe size in samples."""
        return int(self.sample_rate * self.subframe_size_ms / 1000.0)

    @property
    def num_subframes(self) -> int:
        return self.frame_size // self.subframe_size

    @property
    def lpc_window_size(self) -> int:
        """LPC analysis window in samples."""
        return int(self.sample_rate * self.lpc_window_size_ms / 1000.0)

    # ── Serialization ─────────────────────────────────────────────────────

    def to_dict(self) -> Dict[str, Any]:
        return asdict(self)

    @classmethod
    def from_dict(cls, d: Dict[str, Any]) -> "CodecConfig":
        valid = {f.name for f in fields(cls)}
        return cls(**{k: v for k, v in d.items() if k in valid})

    # ── Validation ────────────────────────────────────────────────────────

    def validate(self) -> None:
        """Raise ValueError if the config is internally inconsistent."""
        # Frame / subframe geometry
        if self.frame_size % self.subframe_size != 0:
            raise ValueError(
                f"subframe_size ({self.subframe_size}) must divide "
                f"frame_size ({self.frame_size}) evenly"
            )
        # Track count must divide subframe
        if self.subframe_size % self.acb_num_tracks != 0:
            raise ValueError(
                f"acb_num_tracks ({self.acb_num_tracks}) must divide "
                f"subframe_size ({self.subframe_size})"
            )
        # Sanity ranges
        if not 0.5 <= self.preemphasis_coeff <= 0.9:
            raise ValueError("preemphasis_coeff must be in [0.5, 0.9]")
        if not 8 <= self.lpc_order <= 16:
            raise ValueError("lpc_order must be in [8, 16]")
        if self.lpc_window_type not in ("hamming", "hanning", "blackman"):
            raise ValueError(f"Unknown lpc_window_type: {self.lpc_window_type}")
        if self.pitch_fractional not in ("integer", "half", "third"):
            raise ValueError(f"Unknown pitch_fractional: {self.pitch_fractional}")
        if self.acb_num_pulses < 1:
            raise ValueError("acb_num_pulses must be >= 1")
        # Bitrate achievability (soft check)
        bpf = self.compute_bits_per_frame()
        actual_bps = bpf["total"] / (self.frame_size_ms / 1000.0)
        if actual_bps > self.bitrate_bps * 1.5:
            raise ValueError(
                f"Actual bitrate ({actual_bps:.0f} bps) exceeds "
                f"target ({self.bitrate_bps} bps) by >50%"
            )

    # ── Bit budget ────────────────────────────────────────────────────────

    def compute_bits_per_frame(self) -> Dict[str, int]:
        """Bit allocation breakdown for one frame."""
        nsub = self.num_subframes

        # LSP: lsp_num_splits codebook indices per frame
        lsp_bits = self.lsp_num_splits * int(math.log2(self.lsp_codebook_size))

        # Per subframe: pitch lag + fractional + pitch gain
        pitch_lag_bits = int(math.ceil(math.log2(
            self.pitch_max_lag - self.pitch_min_lag + 1
        )))
        frac_bits = {"integer": 0, "half": 1, "third": 2}[self.pitch_fractional]
        pitch_bits_per_sub = pitch_lag_bits + frac_bits + self.pitch_gain_bits

        # FCB: each pulse needs position (within track) + sign (1 bit)
        positions_per_track = self.subframe_size // self.acb_num_tracks
        pos_bits = int(math.ceil(math.log2(max(positions_per_track, 1))))
        fcb_bits_per_sub = self.acb_num_pulses * (pos_bits + 1)

        # Gain codebook
        gain_bits_per_sub = int(math.ceil(math.log2(self.gain_codebook_size)))

        sub_bits = pitch_bits_per_sub + fcb_bits_per_sub + gain_bits_per_sub
        total = lsp_bits + nsub * sub_bits

        return {
            "lsp": lsp_bits,
            "pitch_per_sub": pitch_bits_per_sub,
            "fcb_per_sub": fcb_bits_per_sub,
            "gain_per_sub": gain_bits_per_sub,
            "sub_total": sub_bits,
            "num_subframes": nsub,
            "total": total,
        }

    @classmethod
    def defaults(cls) -> "CodecConfig":
        """Return a CodecConfig with all default values (16 kHz, 8 kbps)."""
        return cls()

    @classmethod
    def tlcs_8kbps(cls) -> "CodecConfig":
        """TLCS v1 wideband 8 kbps preset."""
        return cls(sample_rate=16000, bitrate_bps=8000, lpc_order=16,
                   pitch_min_lag=32, pitch_max_lag=231,
                   lsp_num_splits=4, lsp_codebook_size=256, acb_num_pulses=2)

    @classmethod
    def tlcs_4kbps(cls) -> "CodecConfig":
        """TLCS v1 wideband 4 kbps preset."""
        return cls(sample_rate=16000, bitrate_bps=4000, lpc_order=16,
                   pitch_min_lag=32, pitch_max_lag=231,
                   lsp_num_splits=4, lsp_codebook_size=64, acb_num_pulses=1,
                   pitch_gain_bits=3, gain_codebook_size=32)

    def summary(self) -> str:
        """One-line human-readable summary for logging."""
        bpf = self.compute_bits_per_frame()
        actual_bps = bpf["total"] / (self.frame_size_ms / 1000.0)
        return (
            f"lpc={self.lpc_order} preemph={self.preemphasis_coeff:.2f} "
            f"pulses={self.acb_num_pulses} pitch={self.pitch_fractional} "
            f"window={self.lpc_window_type} "
            f"lsp_cb={self.lsp_codebook_size} gain_cb={self.gain_codebook_size} "
            f"bps={actual_bps:.0f}"
        )

    def __repr__(self) -> str:
        bpf = self.compute_bits_per_frame()
        actual_bps = bpf["total"] / (self.frame_size_ms / 1000.0)
        return (
            f"CodecConfig(bitrate={actual_bps:.0f}bps, "
            f"frame={self.frame_size_ms}ms, lpc_order={self.lpc_order}, "
            f"pulses={self.acb_num_pulses})"
        )
