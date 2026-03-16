"""
CELP/ACELP Speech Codec — Pure DSP, fully parameterized.

All DSP hyperparameters are exposed via CodecConfig for external
optimization. Zero ML at runtime.
"""
from .config import CodecConfig
from .encoder import CELPEncoder
from .decoder import CELPDecoder
from .bitstream import BitFrame

__all__ = ["CodecConfig", "CELPEncoder", "CELPDecoder", "BitFrame"]
