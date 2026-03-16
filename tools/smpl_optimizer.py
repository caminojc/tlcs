#!/usr/bin/env python3
"""
CMA-ES optimizer for SMPL codec parameters.
Patches smpl_defines.h → recompiles → encode/decode → SCOREQ score.
Runs on DGX with parallel evaluation.
"""
import soundfile as sf
import torch
import torchaudio
import numpy as np
import json, os, re, subprocess, sys, tempfile, time
from concurrent.futures import ProcessPoolExecutor, as_completed

# Monkey-patch torchaudio for scoreq
def _load_sf(path, **kw):
    d, sr = sf.read(str(path), dtype="float32")
    if d.ndim == 1: d = d[None, :]
    else: d = d.T
    return torch.from_numpy(d), sr
torchaudio.load = _load_sf
import scoreq

SMPL_DIR = os.path.expanduser("~/smpl_codec/C")
DEFINES_H = os.path.join(SMPL_DIR, "smpl_opus", "smpl", "smpl_defines.h")
BUILD_DIR = os.path.join(SMPL_DIR, "build")
DEMO = os.path.join(BUILD_DIR, "demo", "opus_mlow_demo")
CORPUS = os.path.expanduser("~/corpus/speech")

# Parameters to optimize and their ranges
PARAMS = {
    "SMPL_LPC_BWE":                 (0.990, 0.9999, 0.9985),
    "SMPL_PERC_MASK_SMTH":          (0.01, 0.30, 0.11),
    "SMPL_HARM_POSTF_STRENGTH":     (0.3, 0.95, 0.713),
    "SMPL_HARM_POSTF_FB_STRENGTH":  (0.1, 0.8, 0.4),
    "SMPL_PITCH_DELTAWGHT":         (0.1, 0.6, 0.3),
    "SMPL_PITCH_PREVWGHT":          (0.3, 0.95, 0.7),
    "SMPL_VUV_BIAS":                (-0.5, 0.1, -0.13),
    "SMPL_PITCH_SHARPENING_COEF":   (0.5, 0.99, 0.95),
}

# Postfilter gamma matrix [2][2][2] — flatten to 8 values
GAMMA_PARAMS = {
    "GAMMA_HR_UV_0": (0.95, 0.999, 0.985),
    "GAMMA_HR_UV_1": (0.95, 0.999, 0.99),
    "GAMMA_HR_V_0":  (0.85, 0.98, 0.94),
    "GAMMA_HR_V_1":  (0.90, 0.99, 0.965),
    "GAMMA_LR_UV_0": (0.85, 0.98, 0.95),
    "GAMMA_LR_UV_1": (0.90, 0.99, 0.97),
    "GAMMA_LR_V_0":  (0.80, 0.95, 0.90),
    "GAMMA_LR_V_1":  (0.85, 0.98, 0.95),
}

ALL_PARAMS = {**PARAMS, **GAMMA_PARAMS}
PARAM_NAMES = list(ALL_PARAMS.keys())
BOUNDS = [(ALL_PARAMS[n][0], ALL_PARAMS[n][1]) for n in PARAM_NAMES]
DEFAULTS = [ALL_PARAMS[n][2] for n in PARAM_NAMES]


def read_defines():
    """Read the original defines file."""
    with open(DEFINES_H) as f:
        return f.read()


ORIGINAL_DEFINES = read_defines()


def patch_defines(values):
    """Patch smpl_defines.h with new parameter values."""
    text = ORIGINAL_DEFINES

    for name, val in zip(PARAM_NAMES, values):
        if name.startswith("GAMMA_"):
            continue  # handled separately
        pattern = rf'(#define\s+{re.escape(name)}\s+)[-\d.]+f?'
        replacement = rf'\g<1>{val:.6f}f'
        text = re.sub(pattern, replacement, text)

    # Patch gamma matrix
    gamma = [values[PARAM_NAMES.index(f"GAMMA_{x}")] for x in
             ["HR_UV_0","HR_UV_1","HR_V_0","HR_V_1","LR_UV_0","LR_UV_1","LR_V_0","LR_V_1"]]

    with open(DEFINES_H, "w") as f:
        f.write(text)

    # Patch postfilter gamma in smpl_postfilter.h
    pf_h = os.path.join(SMPL_DIR, "smpl_opus", "smpl", "smpl_postfilter.h")
    with open(pf_h) as f:
        pf_text = f.read()
    new_gamma = (f'static const float lpc_postfilt_gamma[2][2][2] = '
                 f'{{ {{ {{{gamma[0]:.4f}f, {gamma[1]:.4f}f}}, {{{gamma[2]:.4f}f, {gamma[3]:.4f}f}} }},    // highRate: UV, V\n'
                 f'                                                   {{ {{{gamma[4]:.4f}f, {gamma[5]:.4f}f}}, {{{gamma[6]:.4f}f, {gamma[7]:.4f}f}} }} }};  // lowRate:  UV, V')
    pf_text = re.sub(
        r'static const float lpc_postfilt_gamma\[2\]\[2\]\[2\].*?;.*?// lowRate:.*?V',
        new_gamma, pf_text, flags=re.DOTALL)
    with open(pf_h, "w") as f:
        f.write(pf_text)


def build():
    """Incremental build — fast after first compile."""
    r = subprocess.run(
        ["cmake", "--build", BUILD_DIR, "-j20"],
        capture_output=True, timeout=60)
    return r.returncode == 0


def score_file(wav_path, bitrate, tmp_dir):
    """Encode, decode, score one file."""
    stem = os.path.basename(wav_path).replace(".wav", "")
    out_path = os.path.join(tmp_dir, f"{stem}_dec.wav")
    r = subprocess.run(
        [DEMO, "-use_mlow", "-bitrate", str(bitrate), "-no_dtx",
         wav_path, out_path],
        capture_output=True, timeout=30)
    if r.returncode != 0 or not os.path.exists(out_path):
        return None
    # Score
    import soundfile as sf2
    import torch as t2
    import torchaudio as ta2
    def _lsf(p, **kw):
        d, sr = sf2.read(str(p), dtype="float32")
        if d.ndim == 1: d = d[None, :]
        else: d = d.T
        return t2.from_numpy(d), sr
    ta2.load = _lsf
    import scoreq as sq
    s = sq.Scoreq(data_domain="natural", mode="nr")
    return float(s.predict(out_path))


def evaluate(values, wavs, bitrate=8000):
    """Patch, build, encode/decode all files, score."""
    patch_defines(values)
    if not build():
        return 0.0

    scores = []
    with tempfile.TemporaryDirectory() as tmp:
        for w in wavs:
            sc = score_file(w, bitrate, tmp)
            if sc is not None:
                scores.append(sc)
    return np.mean(scores) if scores else 0.0


def main():
    import cma

    wavs = sorted(os.path.join(CORPUS, f) for f in os.listdir(CORPUS) if f.endswith(".wav"))
    print(f"SMPL CMA-ES optimizer: {len(wavs)} files, {len(PARAM_NAMES)} params")

    # Baseline
    baseline = evaluate(DEFAULTS, wavs, 8000)
    print(f"Baseline SCOREQ: {baseline:.4f}")

    # Normalize to [0,1]
    def to_norm(values):
        return [(v - lo) / (hi - lo) for v, (lo, hi) in zip(values, BOUNDS)]

    def from_norm(normed):
        return [lo + n * (hi - lo) for n, (lo, hi) in zip(normed, BOUNDS)]

    x0 = to_norm(DEFAULTS)

    es = cma.CMAEvolutionStrategy(x0, 0.15, {
        "bounds": [[0.0] * len(x0), [1.0] * len(x0)],
        "maxiter": 60,
        "popsize": 10,
        "seed": 42,
        "verbose": -1,
    })

    best_ever = baseline
    best_ever_x = DEFAULTS[:]
    log_path = os.path.expanduser("~/smpl_cmaes.jsonl")

    gen = 0
    while not es.stop():
        solutions = es.ask()
        fitnesses = []

        for i, s in enumerate(solutions):
            x = from_norm(s)
            sc = evaluate(x, wavs, 8000)
            fitnesses.append(sc)
            if sc > best_ever:
                best_ever = sc
                best_ever_x = x[:]
            print(f"  Gen {gen} cand {i+1}/{len(solutions)}: SCOREQ={sc:.4f}")

        es.tell(solutions, [-f for f in fitnesses])
        mean_f = np.mean([f for f in fitnesses if f > 0])

        rec = {
            "gen": gen,
            "best_scoreq": max(fitnesses),
            "mean_scoreq": float(mean_f),
            "sigma": float(es.sigma),
            "best_ever_scoreq": best_ever,
            "best_ever_params": dict(zip(PARAM_NAMES, best_ever_x)),
        }
        with open(log_path, "a") as f:
            f.write(json.dumps(rec) + "\n")
        print(f"=== Gen {gen}: best={max(fitnesses):.4f} mean={mean_f:.4f} "
              f"best_ever={best_ever:.4f} sigma={es.sigma:.4f} ===")
        gen += 1

    # Restore best and save
    patch_defines(best_ever_x)
    build()
    final = evaluate(best_ever_x, wavs, 8000)

    result = {
        "baseline": baseline,
        "best": final,
        "params": dict(zip(PARAM_NAMES, best_ever_x)),
    }
    json.dump(result, open(os.path.expanduser("~/smpl_best_params.json"), "w"), indent=2)

    print(f"\nDONE. Baseline: {baseline:.4f}, Best: {final:.4f} ({final-baseline:+.4f})")
    print(json.dumps(result["params"], indent=2))


if __name__ == "__main__":
    main()
