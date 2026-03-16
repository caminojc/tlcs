#!/usr/bin/env python3
"""Frame-level quality analysis of TLCS codec at 9600 bps."""

import subprocess
import json
import os
import glob
import numpy as np
import soundfile as sf

# Monkey-patch torchaudio (not needed here, we use soundfile directly)

CORPUS = os.path.expanduser("~/corpus/speech")
ENC = os.path.expanduser("~/tlcs/build/tools/tlcs_enc")
DEC = os.path.expanduser("~/tlcs/build/tools/tlcs_dec")
TMPDIR = "/tmp/tlcs_frame_analysis"
BITRATE = 9600
SR = 16000
FRAME_LEN = 320  # 20ms at 16kHz
NFFT = 512
HOP = FRAME_LEN

os.makedirs(TMPDIR, exist_ok=True)


def encode_decode(wav_path):
    """Encode and decode a WAV file, return (orig, decoded) as float32 arrays."""
    base = os.path.splitext(os.path.basename(wav_path))[0]
    tlcs_path = os.path.join(TMPDIR, base + ".tlcs")
    dec_path = os.path.join(TMPDIR, base + "_dec.wav")

    subprocess.run([ENC, wav_path, tlcs_path, str(BITRATE)],
                   capture_output=True, check=True)
    subprocess.run([DEC, tlcs_path, dec_path],
                   capture_output=True, check=True)

    orig, sr1 = sf.read(wav_path, dtype="float32")
    dec, sr2 = sf.read(dec_path, dtype="float32")

    # Trim to same length
    n = min(len(orig), len(dec))
    return orig[:n], dec[:n]


def frame_energy_db(frame):
    return 10.0 * np.log10(np.mean(frame ** 2) + 1e-10)


def seg_snr(orig, dec):
    sig_pow = np.sum(orig ** 2)
    err_pow = np.sum((orig - dec) ** 2)
    return 10.0 * np.log10(sig_pow / (err_pow + 1e-10) + 1e-10)


def spectral_distortion(orig, dec, nfft=NFFT):
    """L2 distance between log-magnitude spectra."""
    win = np.hanning(len(orig))
    O = np.fft.rfft(orig * win, n=nfft)
    D = np.fft.rfft(dec * win, n=nfft)
    log_orig = np.log(np.abs(O) + 1e-10)
    log_dec = np.log(np.abs(D) + 1e-10)
    return np.sqrt(np.mean((log_orig - log_dec) ** 2))


def zero_crossing_rate(frame):
    signs = np.sign(frame)
    return np.sum(np.abs(np.diff(signs)) > 0) / (len(frame) - 1)


def voicing_strength(frame):
    """Max autocorrelation in lag 20-200 normalized by lag-0."""
    if np.sum(frame ** 2) < 1e-12:
        return 0.0
    ac = np.correlate(frame, frame, mode="full")
    ac = ac[len(frame) - 1:]  # positive lags only
    if ac[0] < 1e-12:
        return 0.0
    # Lags 20-200 correspond to ~80-800 Hz at 16kHz
    lo, hi = 20, min(200, len(ac) - 1)
    if lo >= len(ac):
        return 0.0
    return np.max(ac[lo:hi + 1]) / ac[0]


def band_spectral_distortion(orig, dec, nfft=NFFT, sr=SR):
    """Spectral distortion per frequency band."""
    win = np.hanning(len(orig))
    O = np.fft.rfft(orig * win, n=nfft)
    D = np.fft.rfft(dec * win, n=nfft)
    log_orig = np.log(np.abs(O) + 1e-10)
    log_dec = np.log(np.abs(D) + 1e-10)
    diff2 = (log_orig - log_dec) ** 2

    freqs = np.fft.rfftfreq(nfft, 1.0 / sr)
    bands = {
        "0-1kHz": (0, 1000),
        "1-2kHz": (1000, 2000),
        "2-4kHz": (2000, 4000),
        "4-8kHz": (4000, 8000),
    }
    result = {}
    for name, (lo, hi) in bands.items():
        mask = (freqs >= lo) & (freqs < hi)
        if np.any(mask):
            result[name] = float(np.sqrt(np.mean(diff2[mask])))
        else:
            result[name] = 0.0
    return result


def classify_frame(energy_db, voicing):
    if energy_db <= -30:
        return "silence"
    elif voicing > 0.5:
        return "voiced"
    else:
        return "unvoiced"


def main():
    wav_files = sorted(glob.glob(os.path.join(CORPUS, "*.wav")))
    print(f"Found {len(wav_files)} WAV files")

    all_frames = []  # list of dicts per frame

    for i, wav_path in enumerate(wav_files):
        fname = os.path.basename(wav_path)
        print(f"  [{i+1}/{len(wav_files)}] {fname}", flush=True)

        orig, dec = encode_decode(wav_path)
        n_frames = len(orig) // FRAME_LEN

        for f in range(n_frames):
            s = f * FRAME_LEN
            e = s + FRAME_LEN
            o_frame = orig[s:e]
            d_frame = dec[s:e]

            energy = frame_energy_db(o_frame)
            snr = seg_snr(o_frame, d_frame)
            sd = spectral_distortion(o_frame, d_frame)
            zcr = zero_crossing_rate(o_frame)
            voicing = voicing_strength(o_frame)
            ftype = classify_frame(energy, voicing)
            band_sd = band_spectral_distortion(o_frame, d_frame)

            all_frames.append({
                "file": fname,
                "frame": f,
                "energy_db": float(energy),
                "snr": float(snr),
                "spectral_dist": float(sd),
                "zcr": float(zcr),
                "voicing": float(voicing),
                "type": ftype,
                "band_sd": band_sd,
            })

    print(f"\nTotal frames analyzed: {len(all_frames)}")

    # --- Statistics by frame type ---
    types = ["voiced", "unvoiced", "silence"]
    type_stats = {}
    for t in types:
        frames = [f for f in all_frames if f["type"] == t]
        if not frames:
            type_stats[t] = {"count": 0, "pct": 0, "mean_snr": 0, "mean_sd": 0}
            continue
        snrs = [f["snr"] for f in frames]
        sds = [f["spectral_dist"] for f in frames]
        type_stats[t] = {
            "count": len(frames),
            "pct": round(100.0 * len(frames) / len(all_frames), 1),
            "mean_snr": round(float(np.mean(snrs)), 2),
            "std_snr": round(float(np.std(snrs)), 2),
            "median_snr": round(float(np.median(snrs)), 2),
            "mean_sd": round(float(np.mean(sds)), 3),
            "std_sd": round(float(np.std(sds)), 3),
        }

    # --- Worst 10% frames ---
    sorted_by_snr = sorted(all_frames, key=lambda f: f["snr"])
    n_worst = max(1, len(all_frames) // 10)
    worst_frames = sorted_by_snr[:n_worst]
    worst_type_counts = {}
    for f in worst_frames:
        worst_type_counts[f["type"]] = worst_type_counts.get(f["type"], 0) + 1
    worst_snrs = [f["snr"] for f in worst_frames]
    worst_stats = {
        "count": n_worst,
        "type_distribution": worst_type_counts,
        "mean_snr": round(float(np.mean(worst_snrs)), 2),
        "min_snr": round(float(np.min(worst_snrs)), 2),
        "max_snr": round(float(np.max(worst_snrs)), 2),
    }

    # --- Frequency band analysis by frame type ---
    band_names = ["0-1kHz", "1-2kHz", "2-4kHz", "4-8kHz"]
    band_stats = {}
    for t in ["voiced", "unvoiced"]:
        frames = [f for f in all_frames if f["type"] == t]
        if not frames:
            continue
        band_stats[t] = {}
        for b in band_names:
            vals = [f["band_sd"][b] for f in frames]
            band_stats[t][b] = {
                "mean": round(float(np.mean(vals)), 3),
                "std": round(float(np.std(vals)), 3),
            }

    # --- Find worst band ---
    worst_band = None
    worst_band_val = -1
    for t in ["voiced", "unvoiced"]:
        if t not in band_stats:
            continue
        for b in band_names:
            v = band_stats[t][b]["mean"]
            if v > worst_band_val:
                worst_band_val = v
                worst_band = (t, b)

    # --- Assemble results ---
    results = {
        "total_frames": len(all_frames),
        "bitrate": BITRATE,
        "frame_type_stats": type_stats,
        "worst_10pct": worst_stats,
        "band_analysis": band_stats,
        "worst_band": {"type": worst_band[0], "band": worst_band[1],
                       "mean_sd": worst_band_val} if worst_band else None,
    }

    out_path = os.path.expanduser("~/frame_analysis.json")
    with open(out_path, "w") as f:
        json.dump(results, f, indent=2)
    print(f"\nResults saved to {out_path}")

    # --- Print summary ---
    print("\n" + "=" * 70)
    print("FRAME-LEVEL QUALITY ANALYSIS — TLCS @ 9600 bps")
    print("=" * 70)

    print("\n--- Frame Type Distribution ---")
    for t in types:
        s = type_stats[t]
        print(f"  {t:10s}: {s['count']:5d} frames ({s['pct']:5.1f}%)")

    print("\n--- Mean Segmental SNR by Frame Type ---")
    for t in types:
        s = type_stats[t]
        if s["count"] > 0:
            print(f"  {t:10s}: {s['mean_snr']:7.2f} dB  (std={s['std_snr']:.2f}, median={s['median_snr']:.2f})")

    print("\n--- Mean Spectral Distortion by Frame Type ---")
    for t in types:
        s = type_stats[t]
        if s["count"] > 0:
            print(f"  {t:10s}: {s['mean_sd']:7.3f}  (std={s['std_sd']:.3f})")

    print("\n--- Worst 10% Frames ---")
    print(f"  Count: {worst_stats['count']}")
    print(f"  SNR range: [{worst_stats['min_snr']:.2f}, {worst_stats['max_snr']:.2f}] dB")
    print(f"  Mean SNR: {worst_stats['mean_snr']:.2f} dB")
    print(f"  Type breakdown:")
    for t, c in sorted(worst_stats["type_distribution"].items(), key=lambda x: -x[1]):
        pct = 100.0 * c / worst_stats["count"]
        print(f"    {t:10s}: {c:4d} ({pct:5.1f}%)")

    print("\n--- Spectral Distortion by Frequency Band ---")
    print(f"  {'Band':10s} {'Voiced':>12s} {'Unvoiced':>12s}")
    print(f"  {'-'*10} {'-'*12} {'-'*12}")
    for b in band_names:
        v_val = band_stats.get("voiced", {}).get(b, {}).get("mean", 0)
        u_val = band_stats.get("unvoiced", {}).get(b, {}).get("mean", 0)
        marker = " <-- WORST" if worst_band and worst_band[1] == b else ""
        print(f"  {b:10s} {v_val:12.3f} {u_val:12.3f}{marker}")

    if worst_band:
        print(f"\n  Highest distortion: {worst_band[1]} for {worst_band[0]} frames (mean SD = {worst_band_val:.3f})")

    print("\n" + "=" * 70)
    print("KEY FINDINGS:")
    # Identify which type has worst SNR among active frames
    active_types = [t for t in ["voiced", "unvoiced"] if type_stats[t]["count"] > 0]
    if active_types:
        worst_type = min(active_types, key=lambda t: type_stats[t]["mean_snr"])
        print(f"  - Worst quality frame type: {worst_type} (mean SNR = {type_stats[worst_type]['mean_snr']:.2f} dB)")
    if worst_band:
        print(f"  - Worst frequency band: {worst_band[1]} ({worst_band[0]}, mean SD = {worst_band_val:.3f})")
    # Dominant type in worst 10%
    if worst_stats["type_distribution"]:
        dom_type = max(worst_stats["type_distribution"].items(), key=lambda x: x[1])
        dom_pct = 100.0 * dom_type[1] / worst_stats["count"]
        print(f"  - Worst 10% dominated by: {dom_type[0]} ({dom_pct:.1f}%)")
    print("=" * 70)


if __name__ == "__main__":
    main()
