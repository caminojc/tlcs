"""
Perceptual speech quality evaluator for CELP codec optimisation.

Exports
-------
FrozenEvaluator
    Read-only scoring API loaded from a frozen artifact.  This is the only
    class the codec optimiser should use.

PerceptualEvaluator
    The underlying WavLM + MOS-head model.  Used for training and validation;
    NOT for direct use during optimisation.
"""

from evaluator.evaluator_api import FrozenEvaluator
from evaluator.model import PerceptualEvaluator

__all__ = ["FrozenEvaluator", "PerceptualEvaluator"]
