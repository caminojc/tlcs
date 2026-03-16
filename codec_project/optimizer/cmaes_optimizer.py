"""
CMA-ES optimizer for CELP codec DSP parameter tuning.

Uses Hansen's reference CMA-ES implementation (``cma`` package) to search
the normalised [0,1]^d parameter space defined by SearchSpaceEncoder.
The fitness function is treated as a pure black box.
"""

from __future__ import annotations

import json
import logging
import os
import pickle
import time
from dataclasses import asdict
from pathlib import Path
from typing import Any, Dict, List, Optional

import cma
import numpy as np

import sys as _sys
import os as _os
_sys.path.insert(0, _os.path.join(_os.path.dirname(__file__), ".."))

from optimizer.fitness import FitnessFunction
from optimizer.search_space import SearchSpaceEncoder
from codec.config import CodecConfig

logger = logging.getLogger(__name__)


class CodecCMAESOptimizer:
    """
    CMA-ES over CodecConfig parameter space.

    Parameters
    ----------
    fitness_fn : FitnessFunction
        Black-box fitness function (higher = better).
    search_space : SearchSpaceEncoder
        Encoder/decoder between CodecConfig and flat vectors.
    initial_config : CodecConfig, optional
        Starting point.  Uses defaults if *None*.
    sigma0 : float
        Initial global step size (relative to the [0,1] normalised space).
    population_size : int, optional
        *None* → CMA-ES default ``4 + floor(3 * ln(dim))``.
    max_generations : int
        Hard cap on the number of generations.
    checkpoint_dir : str
        Directory for periodic checkpoints.
    log_dir : str
        Directory for the JSONL optimisation log.
    """

    def __init__(
        self,
        fitness_fn: FitnessFunction,
        search_space: SearchSpaceEncoder,
        initial_config: Optional[CodecConfig] = None,
        sigma0: float = 0.3,
        population_size: Optional[int] = None,
        max_generations: int = 200,
        checkpoint_dir: str = "optimizer/checkpoints",
        log_dir: str = "optimizer/logs",
    ):
        self._fitness_fn = fitness_fn
        self._space = search_space
        self._sigma0 = sigma0
        self._pop_size = population_size
        self._max_gen = max_generations
        self._ckpt_dir = checkpoint_dir
        self._log_dir = log_dir

        # Encode the initial point
        init_cfg = initial_config or CodecConfig.defaults()
        self._x0 = self._space.encode(init_cfg)

        # Best result tracking
        self._best_fitness = float("-inf")
        self._best_config: CodecConfig = init_cfg
        self._best_vector: np.ndarray = self._x0.copy()

        os.makedirs(self._ckpt_dir, exist_ok=True)
        os.makedirs(self._log_dir, exist_ok=True)

    # ------------------------------------------------------------------
    # Main loop
    # ------------------------------------------------------------------

    def run(self) -> CodecConfig:
        """
        Execute the CMA-ES optimisation loop and return the best
        CodecConfig found.
        """
        dim = self._space.dim()
        lower, upper = self._space.bounds()
        per_dim_sigma = self._space.sigma_per_dim(self._sigma0)

        opts: Dict[str, Any] = {
            "bounds": [lower.tolist(), upper.tolist()],
            "CMA_stds": per_dim_sigma.tolist(),
            "maxiter": self._max_gen,
            "verbose": -1,  # suppress CMA-ES stdout
            "seed": 42,
        }
        if self._pop_size is not None:
            opts["popsize"] = self._pop_size

        es = cma.CMAEvolutionStrategy(self._x0.tolist(), self._sigma0, opts)

        gen = 0
        while not es.stop():
            # 1. Sample candidate solutions
            solutions = es.ask()

            # 2. Decode to configs
            configs = [self._space.decode(np.array(s)) for s in solutions]

            # 3. Evaluate fitness (parallel)
            fitnesses = self._fitness_fn.evaluate_batch(configs)

            # 4. CMA-ES minimises — negate fitness
            es.tell(solutions, [-f for f in fitnesses])

            # 5. Track best
            gen_best_idx = int(np.argmax(fitnesses))
            gen_best_fit = fitnesses[gen_best_idx]
            gen_best_cfg = configs[gen_best_idx]
            gen_mean_fit = float(np.mean([f for f in fitnesses if f > float("-inf")]))

            if gen_best_fit > self._best_fitness:
                self._best_fitness = gen_best_fit
                self._best_config = gen_best_cfg
                self._best_vector = np.array(solutions[gen_best_idx])

            # 6. Logging
            self._log_generation(
                gen=gen,
                best_fitness=self._best_fitness,
                mean_fitness=gen_mean_fit,
                gen_best_fitness=gen_best_fit,
                best_config=self._best_config,
                sigma=es.sigma,
            )

            # 7. Checkpoint every 10 generations
            if gen % 10 == 0:
                self._checkpoint(gen, es, self._best_config)

            gen += 1

        # Final checkpoint
        self._checkpoint(gen, es, self._best_config)

        logger.info(
            "CMA-ES finished after %d generations. Best fitness: %.4f",
            gen,
            self._best_fitness,
        )
        logger.info("Best config: %s", self._best_config.summary())

        return self._best_config

    # ------------------------------------------------------------------
    # Resume from checkpoint
    # ------------------------------------------------------------------

    def resume(self, checkpoint_path: str) -> CodecConfig:
        """
        Resume optimisation from a previously saved checkpoint.
        """
        with open(checkpoint_path, "rb") as f:
            ckpt = pickle.load(f)

        self._best_fitness = ckpt["best_fitness"]
        self._best_config = ckpt["best_config"]
        self._best_vector = ckpt["best_vector"]
        start_gen = ckpt["generation"] + 1

        # Reconstruct CMA-ES state
        dim = self._space.dim()
        lower, upper = self._space.bounds()
        per_dim_sigma = self._space.sigma_per_dim(self._sigma0)

        opts: Dict[str, Any] = {
            "bounds": [lower.tolist(), upper.tolist()],
            "CMA_stds": per_dim_sigma.tolist(),
            "maxiter": self._max_gen,
            "verbose": -1,
            "seed": 42,
        }
        if self._pop_size is not None:
            opts["popsize"] = self._pop_size

        es = cma.CMAEvolutionStrategy(
            self._best_vector.tolist(), self._sigma0, opts
        )
        # Inject saved CMA-ES internal state if available
        if "cma_state" in ckpt and ckpt["cma_state"] is not None:
            try:
                es.inject([self._best_vector.tolist()], force=True)
            except Exception:
                logger.warning("Could not fully restore CMA-ES state; continuing from best vector.")

        gen = start_gen
        while not es.stop():
            solutions = es.ask()
            configs = [self._space.decode(np.array(s)) for s in solutions]
            fitnesses = self._fitness_fn.evaluate_batch(configs)
            es.tell(solutions, [-f for f in fitnesses])

            gen_best_idx = int(np.argmax(fitnesses))
            gen_best_fit = fitnesses[gen_best_idx]
            gen_best_cfg = configs[gen_best_idx]
            gen_mean_fit = float(np.mean([f for f in fitnesses if f > float("-inf")]))

            if gen_best_fit > self._best_fitness:
                self._best_fitness = gen_best_fit
                self._best_config = gen_best_cfg
                self._best_vector = np.array(solutions[gen_best_idx])

            self._log_generation(
                gen=gen,
                best_fitness=self._best_fitness,
                mean_fitness=gen_mean_fit,
                gen_best_fitness=gen_best_fit,
                best_config=self._best_config,
                sigma=es.sigma,
            )

            if gen % 10 == 0:
                self._checkpoint(gen, es, self._best_config)

            gen += 1

        self._checkpoint(gen, es, self._best_config)
        logger.info(
            "CMA-ES resumed and finished after %d total generations. Best fitness: %.4f",
            gen,
            self._best_fitness,
        )
        return self._best_config

    # ------------------------------------------------------------------
    # Logging
    # ------------------------------------------------------------------

    def _log_generation(
        self,
        gen: int,
        best_fitness: float,
        mean_fitness: float,
        gen_best_fitness: float,
        best_config: CodecConfig,
        sigma: float,
    ) -> None:
        """Append one JSON line to the optimisation log."""
        record = {
            "generation": gen,
            "best_fitness": best_fitness,
            "gen_best_fitness": gen_best_fitness,
            "mean_fitness": mean_fitness,
            "sigma": sigma,
            "best_config": best_config.to_dict(),
            "config_summary": best_config.summary(),
            "timestamp": time.time(),
            "total_evals": self._fitness_fn.eval_count,
        }
        log_path = os.path.join(self._log_dir, "optimization_log.jsonl")
        with open(log_path, "a") as f:
            f.write(json.dumps(record) + "\n")

    # ------------------------------------------------------------------
    # Checkpointing
    # ------------------------------------------------------------------

    def _checkpoint(
        self,
        gen: int,
        es: cma.CMAEvolutionStrategy,
        best_config: CodecConfig,
    ) -> None:
        """Save CMA-ES state + best config to a pickle checkpoint."""
        ckpt = {
            "generation": gen,
            "best_fitness": self._best_fitness,
            "best_config": best_config,
            "best_vector": self._best_vector,
            "sigma": es.sigma,
            "cma_state": None,  # CMA-ES internal state serialisation
        }
        # Attempt to serialise the full CMA-ES object
        try:
            ckpt["cma_state"] = pickle.dumps(es)
        except Exception:
            logger.warning("Could not serialise full CMA-ES state for gen %d", gen)

        path = os.path.join(self._ckpt_dir, f"gen_{gen:04d}.pkl")
        with open(path, "wb") as f:
            pickle.dump(ckpt, f)
        logger.debug("Checkpoint saved: %s", path)
