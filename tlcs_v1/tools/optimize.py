#!/usr/bin/env python3
"""
CMA-ES optimizer for TLCS v1 codec.
Patches tlcs_config.h → rebuild → encode/decode → NISQA score.
"""
import soundfile as sf
import torch
import torchaudio
import numpy as np
import json, os, re, subprocess, sys, tempfile, time

def _load_sf(path, **kw):
    d, sr = sf.read(str(path), dtype="float32")
    if d.ndim == 1: d = d[None, :]
    else: d = d.T
    return torch.from_numpy(d), sr
torchaudio.load = _load_sf

from torchmetrics.audio.nisqa import NonIntrusiveSpeechQualityAssessment

TLCS_DIR = os.path.expanduser("~/tlcs_v1")
CONFIG_H = os.path.join(TLCS_DIR, "src", "tlcs_config.h")
BUILD = os.path.join(TLCS_DIR, "build")
DEMO = os.path.join(BUILD, "tlcs_demo")
CORPUS = os.path.expanduser("~/corpus/speech")

# Parameters to optimize
PARAMS = {
    "TLCS_LPC_BWE":                  (0.990, 0.9999, 0.9985),
    "TLCS_PREEMPH_COEFF":            (0.50, 0.90, 0.68),
    "TLCS_HARM_POSTF_STRENGTH":      (0.0, 0.95, 0.713),
    "TLCS_HARM_POSTF_FB_STRENGTH":   (0.0, 0.8, 0.4),
    "TLCS_FORMANT_PF_GAMMA_NUM":     (0.01, 0.80, 0.65),
    "TLCS_FORMANT_PF_GAMMA_DEN":     (0.01, 0.95, 0.80),
    "TLCS_FORMANT_PF_TILT":          (0.0, 0.50, 0.30),
    "TLCS_PITCH_SHARPENING_COEF":    (0.50, 0.99, 0.95),
    "TLCS_VUV_BIAS":                 (-0.50, 0.10, -0.13),
    "TLCS_PITCH_DELTAWGHT":          (0.10, 0.60, 0.30),
    "TLCS_PITCH_PREVWGHT":           (0.30, 0.95, 0.70),
}

PARAM_NAMES = list(PARAMS.keys())
BOUNDS = [(PARAMS[n][0], PARAMS[n][1]) for n in PARAM_NAMES]
DEFAULTS = [PARAMS[n][2] for n in PARAM_NAMES]

ORIGINAL_CONFIG = open(CONFIG_H).read()

def patch_config(values):
    text = ORIGINAL_CONFIG
    for name, val in zip(PARAM_NAMES, values):
        pattern = rf'(#define\s+{re.escape(name)}\s+)[-\d.]+f'
        text = re.sub(pattern, rf'\g<1>{val:.6f}f', text)
    with open(CONFIG_H, "w") as f:
        f.write(text)

def build():
    return subprocess.run(
        ["cmake", "--build", BUILD, "-j20"],
        capture_output=True, timeout=30).returncode == 0

def evaluate(values, wavs, nisqa):
    patch_config(values)
    if not build():
        return 0.0
    scores = []
    with tempfile.TemporaryDirectory() as tmp:
        for w in wavs:
            stem = os.path.basename(w).replace(".wav", "")
            out = os.path.join(tmp, f"{stem}.wav")
            r = subprocess.run([DEMO, w, out], capture_output=True, timeout=30)
            if r.returncode != 0 or not os.path.exists(out):
                continue
            audio, sr = sf.read(out, dtype="float32")
            if audio.ndim > 1: audio = audio[:, 0]
            t = torch.tensor(audio)
            if sr != 16000:
                import torchaudio.functional as AF
                t = AF.resample(t, sr, 16000)
            nisqa.update(t.unsqueeze(0))
            result = nisqa.compute()
            nisqa.reset()
            scores.append(float(result[0].item()))
    return np.mean(scores) if scores else 0.0

def main():
    import cma

    wavs = sorted(os.path.join(CORPUS, f) for f in os.listdir(CORPUS) if f.endswith(".wav"))
    print(f"TLCS v1 optimizer: {len(wavs)} files, {len(PARAM_NAMES)} params")

    device = "cuda" if torch.cuda.is_available() else "cpu"
    nisqa = NonIntrusiveSpeechQualityAssessment(16000).to(device)
    nisqa.eval()
    print(f"NISQA on {device}")

    # Baseline
    baseline = evaluate(DEFAULTS, wavs, nisqa)
    print(f"Baseline NISQA: {baseline:.4f}")

    # Normalize to [0,1]
    def to_norm(v):
        return [(val - lo) / (hi - lo) for val, (lo, hi) in zip(v, BOUNDS)]
    def from_norm(n):
        return [lo + val * (hi - lo) for val, (lo, hi) in zip(n, BOUNDS)]

    x0 = to_norm(DEFAULTS)
    es = cma.CMAEvolutionStrategy(x0, 0.2, {
        "bounds": [[0.0] * len(x0), [1.0] * len(x0)],
        "maxiter": 100,
        "popsize": 12,
        "seed": 42,
        "verbose": -1,
    })

    best_ever = baseline
    best_ever_x = DEFAULTS[:]
    log_path = os.path.expanduser("~/tlcs_v1_cmaes.jsonl")

    gen = 0
    while not es.stop():
        solutions = es.ask()
        fitnesses = []
        for i, s in enumerate(solutions):
            x = from_norm(s)
            sc = evaluate(x, wavs, nisqa)
            fitnesses.append(sc)
            if sc > best_ever:
                best_ever = sc
                best_ever_x = x[:]
            print(f"  Gen {gen} cand {i+1}/{len(solutions)}: NISQA={sc:.4f}")

        es.tell(solutions, [-f for f in fitnesses])
        mean_f = np.mean([f for f in fitnesses if f > 0])
        rec = {"gen": gen, "best": max(fitnesses), "mean": float(mean_f),
               "sigma": float(es.sigma), "best_ever": best_ever,
               "best_params": dict(zip(PARAM_NAMES, best_ever_x))}
        with open(log_path, "a") as f:
            f.write(json.dumps(rec) + "\n")
        print(f"=== Gen {gen}: best={max(fitnesses):.4f} mean={mean_f:.4f} "
              f"best_ever={best_ever:.4f} sigma={es.sigma:.4f} ===")
        gen += 1

    patch_config(best_ever_x)
    build()
    json.dump({"nisqa": best_ever, "params": dict(zip(PARAM_NAMES, best_ever_x))},
              open(os.path.expanduser("~/tlcs_v1_best.json"), "w"), indent=2)
    print(f"\nDONE. Baseline: {baseline:.4f}, Best: {best_ever:.4f}")

if __name__ == "__main__":
    main()
