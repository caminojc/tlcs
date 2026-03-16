"""
Defines the CELP codec DSP parameter search space and encoding/decoding
between CodecConfig objects and flat numpy vectors for CMA-ES.
"""
from __future__ import annotations

import dataclasses
import sys
import os
from dataclasses import dataclass
from typing import Any, Dict, List, Optional, Tuple, Union

import numpy as np

# Ensure codec package is importable
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from codec.config import CodecConfig


# ---------------------------------------------------------------------------
# Parameter descriptors
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class ContinuousParam:
    """A continuous floating-point parameter."""
    low: float
    high: float
    default: float

    def __post_init__(self):
        if not (self.low <= self.default <= self.high):
            raise ValueError(
                f"Default {self.default} not in [{self.low}, {self.high}]"
            )


@dataclass(frozen=True)
class IntParam:
    """An integer parameter — optionally constrained to a discrete set of choices."""
    low: Optional[int] = None
    high: Optional[int] = None
    default: int = 0
    choices: Optional[List[int]] = None

    def __post_init__(self):
        if self.choices is not None:
            if self.default not in self.choices:
                raise ValueError(
                    f"Default {self.default} not in choices {self.choices}"
                )
        else:
            if self.low is None or self.high is None:
                raise ValueError("IntParam needs low/high or choices")
            if not (self.low <= self.default <= self.high):
                raise ValueError(
                    f"Default {self.default} not in [{self.low}, {self.high}]"
                )

    @property
    def effective_low(self) -> int:
        return min(self.choices) if self.choices else self.low

    @property
    def effective_high(self) -> int:
        return max(self.choices) if self.choices else self.high


@dataclass(frozen=True)
class CategoricalParam:
    """A categorical parameter — encoded as one-hot for CMA-ES."""
    choices: List[str]
    default: str

    def __post_init__(self):
        if self.default not in self.choices:
            raise ValueError(
                f"Default '{self.default}' not in {self.choices}"
            )


ParamSpec = Union[ContinuousParam, IntParam, CategoricalParam]


# ---------------------------------------------------------------------------
# Canonical search space definition
# ---------------------------------------------------------------------------

SEARCH_SPACE: Dict[str, ParamSpec] = {
    # Continuous parameters (CMA-ES native)
    "preemphasis_coeff":        ContinuousParam(low=0.5,  high=0.9,  default=0.68),
    "lpc_window_size_ms":       ContinuousParam(low=20.0, high=40.0, default=30.0),
    "postfilter_tilt_coeff":    ContinuousParam(low=0.1,  high=0.5,  default=0.3),
    "postfilter_formant_coeff": ContinuousParam(low=0.3,  high=0.8,  default=0.5),

    # Integer parameters (rounded from continuous CMA-ES)
    # lpc_order pinned to 10 (codebooks trained for dim=10) — not in search space
    "acb_num_pulses":       IntParam(low=2,  high=8,  default=4),
    "pitch_gain_bits":      IntParam(low=3,  high=6,  default=4),
    "gain_codebook_size":   IntParam(choices=[32, 64, 128], default=64),
    # lsp_codebook_size pinned to 64 (codebooks trained for size=64) — not in search space

    # Categorical parameters (one-hot encoded for CMA-ES, then decoded)
    "lpc_window_type":  CategoricalParam(choices=["hamming", "hanning", "blackman"], default="hamming"),
    "pitch_fractional": CategoricalParam(choices=["integer", "half", "third"], default="third"),
}


# ---------------------------------------------------------------------------
# SearchSpaceEncoder — maps CodecConfig <-> flat numpy vector for CMA-ES
# ---------------------------------------------------------------------------

class SearchSpaceEncoder:
    """
    Encodes a CodecConfig into a flat numpy vector (all values normalised to
    [0, 1]) and decodes back.  Categorical parameters use one-hot encoding;
    integer choice parameters use one-hot; range integers are treated as
    continuous and rounded on decode.

    The normalised representation is what CMA-ES operates on.
    """

    def __init__(self, space: Dict[str, ParamSpec] | None = None):
        self.space = space or SEARCH_SPACE
        # Build an ordered list of (name, spec) for deterministic ordering
        self._params: List[Tuple[str, ParamSpec]] = list(self.space.items())
        # Pre-compute the segment layout: (start_idx, length) per parameter
        self._segments: List[Tuple[int, int]] = []
        idx = 0
        for _name, spec in self._params:
            length = self._spec_dim(spec)
            self._segments.append((idx, length))
            idx += length
        self._dim = idx

    # -- public API ---------------------------------------------------------

    def dim(self) -> int:
        """Total dimensionality of the encoded vector."""
        return self._dim

    def bounds(self) -> Tuple[np.ndarray, np.ndarray]:
        """Per-dimension lower and upper bounds (all in [0, 1])."""
        lower = np.zeros(self._dim)
        upper = np.ones(self._dim)
        return lower, upper

    def sigma_per_dim(self, sigma0: float = 0.3) -> np.ndarray:
        """
        Initial per-dimension sigma for CMA-ES, scaled by the parameter
        range.  Since everything is normalised to [0,1], sigma0 is used
        directly, but one-hot dims get a smaller sigma.
        """
        sigmas = np.full(self._dim, sigma0)
        for (_name, spec), (start, length) in zip(self._params, self._segments):
            if isinstance(spec, CategoricalParam):
                sigmas[start : start + length] = sigma0 * 0.5
            elif isinstance(spec, IntParam) and spec.choices is not None:
                sigmas[start : start + length] = sigma0 * 0.5
        return sigmas

    def encode(self, config: CodecConfig) -> np.ndarray:
        """Convert a CodecConfig to a normalised numpy vector."""
        vec = np.zeros(self._dim)
        cfg_dict = config.to_dict()
        for (name, spec), (start, length) in zip(self._params, self._segments):
            val = cfg_dict.get(name, self._default_for(spec))
            vec[start : start + length] = self._encode_param(spec, val)
        return vec

    def decode(self, vector: np.ndarray) -> CodecConfig:
        """Convert a normalised numpy vector back to a CodecConfig."""
        vector = np.asarray(vector)
        cfg_dict: Dict[str, Any] = {}
        for (name, spec), (start, length) in zip(self._params, self._segments):
            segment = vector[start : start + length]
            cfg_dict[name] = self._decode_param(spec, segment)
        # Merge with full CodecConfig defaults
        base = CodecConfig()
        base_dict = base.to_dict()
        base_dict.update(cfg_dict)
        return CodecConfig.from_dict(base_dict)

    def default_vector(self) -> np.ndarray:
        """Encode the default CodecConfig."""
        return self.encode(CodecConfig())

    def param_names(self) -> List[str]:
        """Human-readable names for each dimension (for logging)."""
        names = []
        for (name, spec), (start, length) in zip(self._params, self._segments):
            if length == 1:
                names.append(name)
            else:
                if isinstance(spec, CategoricalParam):
                    for c in spec.choices:
                        names.append(f"{name}={c}")
                elif isinstance(spec, IntParam) and spec.choices is not None:
                    for c in spec.choices:
                        names.append(f"{name}={c}")
        return names

    # -- encoding helpers ---------------------------------------------------

    @staticmethod
    def _spec_dim(spec: ParamSpec) -> int:
        if isinstance(spec, ContinuousParam):
            return 1
        elif isinstance(spec, IntParam):
            if spec.choices is not None:
                return len(spec.choices)  # one-hot
            return 1  # continuous, rounded on decode
        elif isinstance(spec, CategoricalParam):
            return len(spec.choices)  # one-hot
        raise TypeError(f"Unknown param spec type: {type(spec)}")

    @staticmethod
    def _default_for(spec: ParamSpec) -> Any:
        return spec.default

    @staticmethod
    def _encode_param(spec: ParamSpec, value: Any) -> np.ndarray:
        if isinstance(spec, ContinuousParam):
            normed = (value - spec.low) / (spec.high - spec.low)
            return np.array([np.clip(normed, 0.0, 1.0)])

        elif isinstance(spec, IntParam):
            if spec.choices is not None:
                oh = np.zeros(len(spec.choices))
                idx = spec.choices.index(value) if value in spec.choices else 0
                oh[idx] = 1.0
                return oh
            normed = (value - spec.low) / (spec.high - spec.low)
            return np.array([np.clip(normed, 0.0, 1.0)])

        elif isinstance(spec, CategoricalParam):
            oh = np.zeros(len(spec.choices))
            idx = spec.choices.index(value) if value in spec.choices else 0
            oh[idx] = 1.0
            return oh

        raise TypeError(f"Unknown param spec type: {type(spec)}")

    @staticmethod
    def _decode_param(spec: ParamSpec, segment: np.ndarray) -> Any:
        if isinstance(spec, ContinuousParam):
            normed = float(np.clip(segment[0], 0.0, 1.0))
            return spec.low + normed * (spec.high - spec.low)

        elif isinstance(spec, IntParam):
            if spec.choices is not None:
                idx = int(np.argmax(segment))
                return spec.choices[idx]
            normed = float(np.clip(segment[0], 0.0, 1.0))
            raw = spec.low + normed * (spec.high - spec.low)
            return int(round(raw))

        elif isinstance(spec, CategoricalParam):
            idx = int(np.argmax(segment))
            return spec.choices[idx]

        raise TypeError(f"Unknown param spec type: {type(spec)}")
