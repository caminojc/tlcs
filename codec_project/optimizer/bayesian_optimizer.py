"""
Bayesian optimization (Gaussian Process) for CELP codec parameter tuning.

An alternative to CMA-ES that is more sample-efficient for small evaluation
budgets (< ~100 evaluations), at the cost of higher per-iteration overhead.

Uses scikit-optimize's ``gp_minimize`` with a mixed search space that
natively supports continuous, integer, and categorical dimensions.
"""

from __future__ import annotations

import json
import logging
import os
import time
from typing import Any, Dict, List, Optional

import numpy as np
from skopt import gp_minimize
from skopt.callbacks import CheckpointSaver
from skopt.plots import plot_convergence as _skopt_plot_convergence
from skopt.space import Categorical, Integer, Real
from skopt.utils import use_named_args

import sys as _sys
import os as _os
_sys.path.insert(0, _os.path.join(_os.path.dirname(__file__), ".."))

from optimizer.fitness import FitnessFunction
from optimizer.search_space import (
    SEARCH_SPACE,
    CategoricalParam,
    ContinuousParam,
    IntParam,
    ParamSpec,
    SearchSpaceEncoder,
)
from codec.config import CodecConfig

logger = logging.getLogger(__name__)


def _build_skopt_dimensions(
    space: Dict[str, ParamSpec],
) -> List:
    """
    Convert our search space definition into scikit-optimize dimension
    objects, preserving parameter names for ``use_named_args``.
    """
    dims = []
    for name, spec in space.items():
        if isinstance(spec, ContinuousParam):
            dims.append(Real(spec.low, spec.high, name=name, prior="uniform"))
        elif isinstance(spec, IntParam):
            if spec.choices is not None:
                dims.append(Categorical(spec.choices, name=name))
            else:
                dims.append(Integer(spec.low, spec.high, name=name))
        elif isinstance(spec, CategoricalParam):
            dims.append(Categorical(spec.choices, name=name))
        else:
            raise TypeError(f"Unknown param type: {type(spec)}")
    return dims


class CodecBayesianOptimizer:
    """
    Gaussian Process Bayesian optimization for codec tuning.

    Better than CMA-ES when evaluation budget is small (< 100 calls).
    CMA-ES is preferred for larger budgets.

    Parameters
    ----------
    fitness_fn : FitnessFunction
        Black-box fitness function (higher = better).
    search_space : dict
        Parameter space definition (defaults to ``SEARCH_SPACE``).
    n_calls : int
        Total number of fitness evaluations.
    n_initial_points : int
        Number of random evaluations before the GP model kicks in.
    log_dir : str
        Directory for the JSONL optimisation log.
    checkpoint_dir : str
        Directory for periodic checkpoints.
    """

    def __init__(
        self,
        fitness_fn: FitnessFunction,
        search_space: Dict[str, ParamSpec] | None = None,
        n_calls: int = 100,
        n_initial_points: int = 20,
        log_dir: str = "optimizer/logs",
        checkpoint_dir: str = "optimizer/checkpoints",
    ):
        self._fitness_fn = fitness_fn
        self._space = search_space or SEARCH_SPACE
        self._n_calls = n_calls
        self._n_initial = n_initial_points
        self._log_dir = log_dir
        self._ckpt_dir = checkpoint_dir

        os.makedirs(self._log_dir, exist_ok=True)
        os.makedirs(self._ckpt_dir, exist_ok=True)

        self._dimensions = _build_skopt_dimensions(self._space)
        self._param_names = [name for name in self._space]

        self._best_fitness = float("-inf")
        self._best_config: Optional[CodecConfig] = None
        self._call_idx = 0
        self._result = None  # skopt OptimizeResult, set after run()

    # ------------------------------------------------------------------
    # Main loop
    # ------------------------------------------------------------------

    def run(self) -> CodecConfig:
        """Run Bayesian optimisation and return the best CodecConfig."""

        log_path = os.path.join(self._log_dir, "bayesian_log.jsonl")
        ckpt_path = os.path.join(self._ckpt_dir, "bayesian_checkpoint.pkl")
        checkpoint_cb = CheckpointSaver(ckpt_path, compress=3)

        @use_named_args(self._dimensions)
        def objective(**params):
            config = CodecConfig.from_dict(params)
            fitness = self._fitness_fn.evaluate(config)

            # Track best
            if fitness > self._best_fitness:
                self._best_fitness = fitness
                self._best_config = config

            # Log
            record = {
                "call": self._call_idx,
                "fitness": fitness,
                "best_fitness": self._best_fitness,
                "config": config.to_dict(),
                "config_summary": config.summary(),
                "timestamp": time.time(),
            }
            with open(log_path, "a") as f:
                f.write(json.dumps(record) + "\n")

            self._call_idx += 1

            # gp_minimize *minimises*, so negate
            return -fitness

        # Default starting point
        x0 = [self._space[name].default for name in self._param_names]

        self._result = gp_minimize(
            func=objective,
            dimensions=self._dimensions,
            n_calls=self._n_calls,
            n_initial_points=self._n_initial,
            x0=x0,
            acq_func="EI",  # Expected Improvement
            noise="gaussian",
            random_state=42,
            callback=[checkpoint_cb],
            verbose=False,
        )

        logger.info(
            "Bayesian optimization finished after %d evaluations. Best fitness: %.4f",
            self._call_idx,
            self._best_fitness,
        )
        if self._best_config:
            logger.info("Best config: %s", self._best_config.summary())

        return self._best_config

    # ------------------------------------------------------------------
    # Convergence plot
    # ------------------------------------------------------------------

    def plot_convergence(self, output_path: str) -> None:
        """
        Save a convergence plot to *output_path*.

        Requires matplotlib.  The plot shows minimum observed objective
        (negated fitness) vs number of evaluations.
        """
        if self._result is None:
            raise RuntimeError("No optimisation result — call run() first.")

        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt

        fig, ax = plt.subplots(figsize=(10, 6))
        _skopt_plot_convergence(self._result, ax=ax)
        ax.set_ylabel("Negative Fitness (lower = better codec)")
        ax.set_xlabel("Number of evaluations")
        ax.set_title("Bayesian Optimization Convergence")
        fig.tight_layout()
        fig.savefig(output_path, dpi=150)
        plt.close(fig)
        logger.info("Convergence plot saved to %s", output_path)
