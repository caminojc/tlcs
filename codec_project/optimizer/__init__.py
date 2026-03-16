"""
Black-box optimization loop for CELP codec DSP parameter tuning.

Uses CMA-ES (or Bayesian optimization) to search the codec's parameter space
with a frozen perceptual evaluator as the oracle fitness function.
"""

from optimizer.search_space import (
    ContinuousParam,
    IntParam,
    CategoricalParam,
    SearchSpaceEncoder,
    SEARCH_SPACE,
)
from codec.config import CodecConfig
from optimizer.fitness import FitnessFunction, CELPCodecWrapper
from optimizer.cmaes_optimizer import CodecCMAESOptimizer
from optimizer.bayesian_optimizer import CodecBayesianOptimizer

__all__ = [
    "ContinuousParam",
    "IntParam",
    "CategoricalParam",
    "CodecConfig",
    "SearchSpaceEncoder",
    "SEARCH_SPACE",
    "FitnessFunction",
    "CELPCodecWrapper",
    "CodecCMAESOptimizer",
    "CodecBayesianOptimizer",
]
