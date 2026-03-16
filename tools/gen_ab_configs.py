#!/usr/bin/env python3
"""Generate 50 codec parameter configurations for A/B preference testing.

Uses Latin Hypercube Sampling with stratified groups:
  - 5 near current best (small perturbations)
  - 5 with no postfilter
  - 5 with very light SBR
  - 5 with heavy SBR
  - 30 across the full space
"""

import json
import numpy as np
from pathlib import Path

SEED = 42
rng = np.random.default_rng(SEED)

# Parameter definitions: (name, lo, hi)
PARAMS = [
    ("TUNE_ALFE_BOOST",       1.0, 2.5),
    ("TUNE_SBR_BASE_MULT",    0.2, 1.2),
    ("TUNE_SBR_DEPTH_FRAC",   0.3, 0.9),
    ("TUNE_NF_CODED",         0.02, 0.25),
    ("TUNE_NF_UNCODED_HI",    0.3, 1.0),
    ("TUNE_NF_UNCODED_SLOPE", 0.1, 0.6),
    ("TUNE_PF_NUM_LR",        0.4, 0.8),
    ("TUNE_PF_DEN_LR",        0.6, 0.9),
    ("TUNE_PF_TILT_LR",       0.1, 0.5),
]

# Current best values
CURRENT = {
    "TUNE_ALFE_BOOST":       1.50,
    "TUNE_SBR_BASE_MULT":    0.65,
    "TUNE_SBR_DEPTH_FRAC":   0.50,
    "TUNE_NF_CODED":         0.12,
    "TUNE_NF_UNCODED_HI":    0.75,
    "TUNE_NF_UNCODED_SLOPE": 0.25,
    "TUNE_PF_NUM_LR":        0.55,
    "TUNE_PF_DEN_LR":        0.75,
    "TUNE_PF_TILT_LR":       0.20,
}


def lhs(n, d, rng):
    """Latin Hypercube Sample: n points in d dimensions, each in [0,1]."""
    result = np.zeros((n, d))
    for j in range(d):
        perm = rng.permutation(n)
        for i in range(n):
            result[perm[i], j] = (i + rng.uniform()) / n
    return result


def scale(unit_vals, params):
    """Scale [0,1] values to parameter ranges."""
    cfg = {}
    for (name, lo, hi), u in zip(params, unit_vals):
        cfg[name] = round(lo + u * (hi - lo), 4)
    return cfg


def clamp_cfg(cfg):
    """Clamp all values to their valid ranges."""
    for name, lo, hi in PARAMS:
        cfg[name] = round(max(lo, min(hi, cfg[name])), 4)
    return cfg


def perturb_current(rng, sigma=0.08):
    """Small Gaussian perturbation around current best."""
    cfg = {}
    for name, lo, hi in PARAMS:
        base = CURRENT[name]
        spread = (hi - lo) * sigma
        cfg[name] = base + rng.normal() * spread
    return clamp_cfg(cfg)


configs = []

# --- Group 1: 5 near current best ---
for i in range(5):
    cfg = perturb_current(rng, sigma=0.08)
    cfg["id"] = len(configs)
    cfg["group"] = "near_best"
    configs.append(cfg)

# --- Group 2: 5 with no postfilter ---
# LHS for the 6 non-PF params, PF params fixed near zero
nopf_lhs = lhs(5, 6, rng)
nopf_params = [p for p in PARAMS if "PF_" not in p[0]]
for i in range(5):
    cfg = scale(nopf_lhs[i], nopf_params)
    cfg["TUNE_PF_NUM_LR"] = 0.01
    cfg["TUNE_PF_DEN_LR"] = 0.01
    cfg["TUNE_PF_TILT_LR"] = 0.01
    cfg["id"] = len(configs)
    cfg["group"] = "no_postfilter"
    configs.append(cfg)

# --- Group 3: 5 with very light SBR (< 0.3) ---
light_lhs = lhs(5, len(PARAMS), rng)
for i in range(5):
    cfg = scale(light_lhs[i], PARAMS)
    # Override SBR_BASE_MULT to [0.2, 0.3)
    cfg["TUNE_SBR_BASE_MULT"] = round(0.2 + light_lhs[i, 1] * 0.1, 4)
    cfg["id"] = len(configs)
    cfg["group"] = "light_sbr"
    configs.append(cfg)

# --- Group 4: 5 with heavy SBR (> 0.9) ---
heavy_lhs = lhs(5, len(PARAMS), rng)
for i in range(5):
    cfg = scale(heavy_lhs[i], PARAMS)
    # Override SBR_BASE_MULT to (0.9, 1.2]
    cfg["TUNE_SBR_BASE_MULT"] = round(0.9 + heavy_lhs[i, 1] * 0.3, 4)
    cfg["id"] = len(configs)
    cfg["group"] = "heavy_sbr"
    configs.append(cfg)

# --- Group 5: 30 across the full space ---
full_lhs = lhs(30, len(PARAMS), rng)
for i in range(30):
    cfg = scale(full_lhs[i], PARAMS)
    cfg["id"] = len(configs)
    cfg["group"] = "full_space"
    configs.append(cfg)

# Reorder keys for readability
ordered_keys = ["id", "group"] + [p[0] for p in PARAMS]
configs = [{k: c[k] for k in ordered_keys} for c in configs]

out_path = Path(__file__).parent / "ab_configs.json"
with open(out_path, "w") as f:
    json.dump(configs, f, indent=2)

print(f"Wrote {len(configs)} configs to {out_path}")
for g in ["near_best", "no_postfilter", "light_sbr", "heavy_sbr", "full_space"]:
    n = sum(1 for c in configs if c["group"] == g)
    print(f"  {g}: {n}")
