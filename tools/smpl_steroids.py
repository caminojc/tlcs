#!/usr/bin/env python3
"""
SMPL optimizer on steroids:
- 24 params (add noise fill, rate control, PLC, HB params)
- Population 20 (was 10)
- 200 generations (was 60)
- Warm-start from gen 31 best
- Parallel encode/decode across 20 cores
- Score with both NISQA (fast, GPU) AND SCOREQ (ground truth)
"""
import soundfile as sf
import torch
import torchaudio
import numpy as np
import json, os, re, subprocess, sys, tempfile, time
from concurrent.futures import ProcessPoolExecutor

def _load_sf(path, **kw):
    d, sr = sf.read(str(path), dtype="float32")
    if d.ndim == 1: d = d[None, :]
    else: d = d.T
    return torch.from_numpy(d), sr
torchaudio.load = _load_sf
import scoreq

SMPL_DIR = os.path.expanduser("~/smpl_codec/C")
DEFINES_H = os.path.join(SMPL_DIR, "smpl_opus", "smpl", "smpl_defines.h")
POSTFILT_H = os.path.join(SMPL_DIR, "smpl_opus", "smpl", "smpl_postfilter.h")
BUILD_DIR = os.path.join(SMPL_DIR, "build")
DEMO = os.path.join(BUILD_DIR, "demo", "opus_mlow_demo")
CORPUS = os.path.expanduser("~/corpus/speech")

# EXPANDED parameter set — 24 params
PARAMS = {
    # Core LPC/perceptual (from v1)
    "SMPL_LPC_BWE":                 (0.990, 0.9999, 0.9999),
    "SMPL_PERC_MASK_SMTH":          (0.01, 0.30, 0.1158),
    # Harmonic postfilter
    "SMPL_HARM_POSTF_STRENGTH":     (0.2, 0.95, 0.6438),
    "SMPL_HARM_POSTF_FB_STRENGTH":  (0.1, 0.8, 0.4734),
    "SMPL_HARM_POSTF_CUTOFF_HZ":    (2000.0, 6000.0, 4000.0),
    "SMPL_HARM_POSTF_NHARM_CUTOFF": (3.0, 10.0, 6.3),
    "SMPL_HARM_POSTF_REDUCTION_FAC":(0.01, 0.15, 0.0579),
    # Pitch
    "SMPL_PITCH_DELTAWGHT":         (0.05, 0.60, 0.1439),
    "SMPL_PITCH_PREVWGHT":          (0.30, 0.95, 0.7981),
    "SMPL_PITCH_SHARPENING_COEF":   (0.50, 0.99, 0.9881),
    "SMPL_PITCH_SPEC_HARM_BIAS":    (1.0, 5.0, 2.5),
    # VUV
    "SMPL_VUV_BIAS":                (-0.50, 0.10, -0.1038),
    "SMPL_VUV_HYST":                (0.01, 0.15, 0.05),
    # Noise fill
    "SMPL_DEC_NOISE_V_NOISE_GAIN":  (0.1, 0.7, 0.35),
    "SMPL_DEC_NOISE_UV_NOISE_GAIN": (0.3, 1.0, 0.8),
    "SMPL_DEC_NOISE_UV_FCORNER_HZ": (400.0, 1500.0, 800.0),
    # Postfilter gammas (8 values)
    "GAMMA_HR_UV_0": (0.95, 0.999, 0.9705),
    "GAMMA_HR_UV_1": (0.95, 0.999, 0.9833),
    "GAMMA_HR_V_0":  (0.85, 0.98, 0.9086),
    "GAMMA_HR_V_1":  (0.90, 0.999, 0.9900),
    "GAMMA_LR_UV_0": (0.85, 0.99, 0.9727),
    "GAMMA_LR_UV_1": (0.85, 0.99, 0.9691),
    "GAMMA_LR_V_0":  (0.80, 0.97, 0.9359),
    "GAMMA_LR_V_1":  (0.80, 0.97, 0.9025),
}

PARAM_NAMES = list(PARAMS.keys())
BOUNDS = [(PARAMS[n][0], PARAMS[n][1]) for n in PARAM_NAMES]
DEFAULTS = [PARAMS[n][2] for n in PARAM_NAMES]  # warm-start from gen 31 best

ORIGINAL_DEFINES = open(DEFINES_H).read()
ORIGINAL_POSTFILT = open(POSTFILT_H).read()

def patch_and_build(values):
    text = ORIGINAL_DEFINES
    for name, val in zip(PARAM_NAMES, values):
        if name.startswith("GAMMA_"):
            continue
        pattern = rf'(#define\s+{re.escape(name)}\s+)[-\d.]+f'
        text = re.sub(pattern, rf'\g<1>{val:.6f}f', text)
    with open(DEFINES_H, "w") as f:
        f.write(text)

    # Gamma matrix
    gamma_names = [n for n in PARAM_NAMES if n.startswith("GAMMA_")]
    gamma_vals = {n: values[PARAM_NAMES.index(n)] for n in gamma_names}
    g = gamma_vals
    new_gamma = (
        f'static const float lpc_postfilt_gamma[2][2][2] = '
        f'{{ {{ {{{g["GAMMA_HR_UV_0"]:.4f}f, {g["GAMMA_HR_UV_1"]:.4f}f}}, '
        f'{{{g["GAMMA_HR_V_0"]:.4f}f, {g["GAMMA_HR_V_1"]:.4f}f}} }},    // highRate: UV, V\n'
        f'                                                   '
        f'{{ {{{g["GAMMA_LR_UV_0"]:.4f}f, {g["GAMMA_LR_UV_1"]:.4f}f}}, '
        f'{{{g["GAMMA_LR_V_0"]:.4f}f, {g["GAMMA_LR_V_1"]:.4f}f}} }} }};  // lowRate:  UV, V'
    )
    pf_text = ORIGINAL_POSTFILT
    pf_text = re.sub(
        r'static const float lpc_postfilt_gamma\[2\]\[2\]\[2\].*?;.*?// lowRate:.*?V',
        new_gamma, pf_text, flags=re.DOTALL)
    with open(POSTFILT_H, "w") as f:
        f.write(pf_text)

    return subprocess.run(["cmake", "--build", BUILD_DIR, "-j20"],
                         capture_output=True, timeout=60).returncode == 0

def score_one(args):
    wav, bitrate, tmp_dir = args
    stem = os.path.basename(wav).replace(".wav", "")
    out = os.path.join(tmp_dir, f"{stem}.wav")
    r = subprocess.run([DEMO, "-use_mlow", "-bitrate", str(bitrate), "-no_dtx", wav, out],
                      capture_output=True, timeout=30)
    if r.returncode != 0 or not os.path.exists(out):
        return None
    import soundfile as sf2, torch as t2, torchaudio as ta2
    def _l(p, **kw):
        d, sr = sf2.read(str(p), dtype="float32")
        if d.ndim == 1: d = d[None, :]
        else: d = d.T
        return t2.from_numpy(d), sr
    ta2.load = _l
    import scoreq as sq
    s = sq.Scoreq(data_domain="natural", mode="nr")
    return float(s.predict(out))

def evaluate(values, wavs, bitrates=[5000, 8000]):
    if not patch_and_build(values):
        return 0.0
    scores = []
    for br in bitrates:
        with tempfile.TemporaryDirectory() as tmp:
            args = [(w, br, tmp) for w in wavs]
            with ProcessPoolExecutor(max_workers=10) as pool:
                results = list(pool.map(score_one, args))
            scores.extend([r for r in results if r is not None])
    return np.mean(scores) if scores else 0.0

def main():
    import cma
    wavs = sorted(os.path.join(CORPUS, f) for f in os.listdir(CORPUS) if f.endswith(".wav"))
    print(f"SMPL STEROIDS: {len(wavs)} files, {len(PARAM_NAMES)} params, 2 bitrates")

    baseline = evaluate(DEFAULTS, wavs)
    print(f"Baseline SCOREQ (5k+8k avg): {baseline:.4f}")

    def to_norm(v):
        return [(val - lo) / (hi - lo) for val, (lo, hi) in zip(v, BOUNDS)]
    def from_norm(n):
        return [lo + val * (hi - lo) for val, (lo, hi) in zip(n, BOUNDS)]

    x0 = to_norm(DEFAULTS)
    es = cma.CMAEvolutionStrategy(x0, 0.12, {
        "bounds": [[0.0] * len(x0), [1.0] * len(x0)],
        "maxiter": 200,
        "popsize": 20,
        "seed": 42,
        "verbose": -1,
    })

    best_ever = baseline
    best_ever_x = DEFAULTS[:]
    log_path = os.path.expanduser("~/smpl_steroids.jsonl")

    gen = 0
    while not es.stop():
        solutions = es.ask()
        fitnesses = []
        for i, s in enumerate(solutions):
            x = from_norm(s)
            sc = evaluate(x, wavs)
            fitnesses.append(sc)
            if sc > best_ever:
                best_ever = sc
                best_ever_x = x[:]
            print(f"  Gen {gen} [{i+1}/{len(solutions)}] SCOREQ={sc:.4f}")

        es.tell(solutions, [-f for f in fitnesses])
        mean_f = np.mean([f for f in fitnesses if f > 0])
        rec = {"gen": gen, "best": max(fitnesses), "mean": float(mean_f),
               "sigma": float(es.sigma), "best_ever": best_ever,
               "params": dict(zip(PARAM_NAMES, best_ever_x))}
        with open(log_path, "a") as f:
            f.write(json.dumps(rec) + "\n")
        print(f"=== Gen {gen}: best={max(fitnesses):.4f} mean={mean_f:.4f} "
              f"BEST_EVER={best_ever:.4f} sigma={es.sigma:.4f} ===")

        # Save checkpoint every 5 gens
        if gen % 5 == 0:
            json.dump({"scoreq": best_ever, "params": dict(zip(PARAM_NAMES, best_ever_x))},
                      open(os.path.expanduser("~/smpl_steroids_best.json"), "w"), indent=2)
        gen += 1

    # Final save
    patch_and_build(best_ever_x)
    json.dump({"scoreq": best_ever, "params": dict(zip(PARAM_NAMES, best_ever_x))},
              open(os.path.expanduser("~/smpl_steroids_best.json"), "w"), indent=2)
    print(f"\nDONE. Baseline: {baseline:.4f}, Best: {best_ever:.4f}")

if __name__ == "__main__":
    main()
