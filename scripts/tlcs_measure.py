#!/usr/bin/env python3
"""
TLCS Quality Diagnostic Measurement Script

Runs tlcs_diag on a corpus of WAV files, measures PESQ/SNR for each
component, and prints an actionable diagnostic report.

Usage:
    python3 scripts/tlcs_measure.py <corpus_dir> [--build-dir build/]

Expects:
    - tlcs_diag binary in build/tools/
    - pesq package in TLC benchmark venv
    - Corpus dir containing 16-bit mono WAV files at 16kHz
"""

import argparse
import csv
import glob
import os
import subprocess
import sys
import tempfile

import numpy as np

# PESQ via TLC benchmark venv
PESQ_PYTHON = "/Users/jonathanchristensen/CLionProjects/TLC/benchmark/.venv/bin/python3"


def compute_snr(ref, deg):
    """Compute segmental SNR (dB) between reference and degraded signals."""
    signal_power = np.sum(ref.astype(np.float64) ** 2)
    noise = ref.astype(np.float64) - deg.astype(np.float64)
    noise_power = np.sum(noise ** 2)
    if noise_power < 1e-10:
        return 99.0
    return 10.0 * np.log10(signal_power / noise_power)


def read_wav_samples(path):
    """Read 16-bit mono WAV, return numpy int16 array."""
    import wave
    with wave.open(path, 'rb') as wf:
        assert wf.getsampwidth() == 2
        assert wf.getnchannels() == 1
        n = wf.getnframes()
        raw = wf.readframes(n)
    return np.frombuffer(raw, dtype=np.int16)


def compute_pesq(ref_path, deg_path, sr=16000):
    """Compute PESQ-WB using TLC benchmark venv."""
    code = f"""
import sys
try:
    from pesq import pesq
    import numpy as np
    import wave

    def read_wav(p):
        with wave.open(p, 'rb') as wf:
            n = wf.getnframes()
            raw = wf.readframes(n)
        return np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0

    ref = read_wav('{ref_path}')
    deg = read_wav('{deg_path}')
    n = min(len(ref), len(deg))
    ref = ref[:n]
    deg = deg[:n]
    score = pesq({sr}, ref, deg, 'wb')
    print(f"{{score:.4f}}")
except Exception as e:
    print(f"ERROR: {{e}}", file=sys.stderr)
    print("-1.0")
"""
    result = subprocess.run(
        [PESQ_PYTHON, "-c", code],
        capture_output=True, text=True, timeout=30
    )
    try:
        return float(result.stdout.strip())
    except ValueError:
        sys.stderr.write(f"PESQ error: {result.stderr}\n")
        return -1.0


def analyze_csv(csv_path):
    """Parse metrics CSV and compute summary statistics."""
    stats = {
        'lsf_sd': [],
        'voicing': [],
        'pitch_frac': [],
        'cb_frac': [],
        'error_frac': [],
        'pitch_gain_sat': 0,
        'cb_gain_sat': 0,
        'total_frames': 0,
    }

    with open(csv_path, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            stats['total_frames'] += 1
            stats['lsf_sd'].append(float(row['lsf_sd_db']))
            stats['voicing'].append(float(row['voicing']))
            stats['pitch_frac'].append(float(row['pitch_frac']))
            stats['cb_frac'].append(float(row['cb_frac']))
            stats['error_frac'].append(float(row['error_frac']))

            # Check pitch gain saturation (quantized = max)
            for i in range(4):
                pg = float(row[f'pitch_gain{i}'])
                if pg >= 1.19:  # near PITCH_GAIN_MAX=1.2
                    stats['pitch_gain_sat'] += 1

            # Check CB gain saturation
            for i in range(4):
                cg = abs(float(row[f'cb_gain{i}']))
                cg_uq = abs(float(row[f'cb_gain_uq{i}']))
                if cg_uq > 0 and cg / cg_uq < 0.5:
                    stats['cb_gain_sat'] += 1

    return stats


def print_report(file_results, aggregate_stats):
    """Print diagnostic report with actionable thresholds."""
    print("=" * 72)
    print("TLCS QUALITY DIAGNOSTIC REPORT")
    print("=" * 72)

    # Per-file results
    print("\n--- Per-File PESQ-WB ---")
    print(f"{'File':<30} {'Full':>6} {'LPC-UQ':>7} {'Pitch':>6} {'CB':>6} {'SNR':>6}")
    print("-" * 72)

    pesq_full_all = []
    pesq_lpc_all = []
    pesq_pitch_all = []
    pesq_cb_all = []
    snr_all = []

    for r in file_results:
        print(f"{r['name']:<30} {r['pesq_full']:>6.3f} {r['pesq_lpc']:>7.3f} "
              f"{r['pesq_pitch']:>6.3f} {r['pesq_cb']:>6.3f} {r['snr']:>6.1f}")
        pesq_full_all.append(r['pesq_full'])
        pesq_lpc_all.append(r['pesq_lpc'])
        pesq_pitch_all.append(r['pesq_pitch'])
        pesq_cb_all.append(r['pesq_cb'])
        snr_all.append(r['snr'])

    print("-" * 72)
    print(f"{'MEAN':<30} {np.mean(pesq_full_all):>6.3f} {np.mean(pesq_lpc_all):>7.3f} "
          f"{np.mean(pesq_pitch_all):>6.3f} {np.mean(pesq_cb_all):>6.3f} {np.mean(snr_all):>6.1f}")

    # LSF quality impact
    lpc_delta = np.mean(pesq_lpc_all) - np.mean(pesq_full_all)
    print(f"\n--- LSF Quantization Impact ---")
    print(f"  PESQ gain from unquantized LPC: {lpc_delta:+.3f}")
    print(f"  Mean LSF spectral distortion:   {np.mean(aggregate_stats['lsf_sd']):.2f} dB")
    if np.mean(aggregate_stats['lsf_sd']) > 1.0:
        print("  ** FLAG: LSF SD > 1.0 dB — scalar quantizer is a bottleneck")
    elif np.mean(aggregate_stats['lsf_sd']) > 0.5:
        print("  * NOTE: LSF SD moderately high — VQ could help")
    else:
        print("  OK: LSF quantization is adequate")

    # Energy decomposition
    print(f"\n--- Excitation Energy Decomposition ---")
    mean_pitch = np.mean(aggregate_stats['pitch_frac']) * 100
    mean_cb = np.mean(aggregate_stats['cb_frac']) * 100
    mean_err = np.mean(aggregate_stats['error_frac']) * 100
    print(f"  Pitch contribution:   {mean_pitch:.1f}%")
    print(f"  Codebook contribution: {mean_cb:.1f}%")
    print(f"  Uncaptured error:     {mean_err:.1f}%")

    # Voicing analysis
    voiced_frames = sum(1 for v in aggregate_stats['voicing'] if v > 0.6)
    total = len(aggregate_stats['voicing'])
    voiced_pct = 100.0 * voiced_frames / total if total > 0 else 0
    voiced_pitch_frac = []
    unvoiced_pitch_frac = []
    for v, pf in zip(aggregate_stats['voicing'], aggregate_stats['pitch_frac']):
        if v > 0.6:
            voiced_pitch_frac.append(pf)
        else:
            unvoiced_pitch_frac.append(pf)

    print(f"\n--- Voicing Analysis ---")
    print(f"  Voiced frames (voicing > 0.6): {voiced_pct:.0f}%")
    if voiced_pitch_frac:
        print(f"  Pitch capture on voiced:       {np.mean(voiced_pitch_frac)*100:.1f}%")
        if np.mean(voiced_pitch_frac) < 0.6:
            print("  ** FLAG: Pitch capture < 60% on voiced frames — pitch search issue")
    if unvoiced_pitch_frac:
        print(f"  Pitch capture on unvoiced:     {np.mean(unvoiced_pitch_frac)*100:.1f}%")

    if mean_err > 12:
        print(f"\n  ** FLAG: Uncaptured error > 12% — codebook capacity insufficient")

    # Gain saturation
    total_subfr = aggregate_stats['total_frames'] * 4
    pg_sat_pct = 100.0 * aggregate_stats['pitch_gain_sat'] / total_subfr if total_subfr > 0 else 0
    cg_sat_pct = 100.0 * aggregate_stats['cb_gain_sat'] / total_subfr if total_subfr > 0 else 0
    print(f"\n--- Gain Saturation ---")
    print(f"  Pitch gain saturated: {pg_sat_pct:.1f}% of subframes")
    print(f"  CB gain mismatch:     {cg_sat_pct:.1f}% of subframes")
    if pg_sat_pct > 1:
        print("  * NOTE: Pitch gain saturating — consider wider range")
    if cg_sat_pct > 5:
        print("  ** FLAG: CB gain quantization error > 5% — increase gain bits")

    print("\n" + "=" * 72)


def main():
    parser = argparse.ArgumentParser(description="TLCS Quality Diagnostics")
    parser.add_argument("corpus_dir", help="Directory of input WAV files")
    parser.add_argument("--build-dir", default="build", help="Build directory")
    parser.add_argument("--single", help="Process a single file instead of corpus")
    args = parser.parse_args()

    diag_bin = os.path.join(args.build_dir, "tools", "tlcs_diag")
    if not os.path.exists(diag_bin):
        print(f"Error: {diag_bin} not found. Build with cmake first.", file=sys.stderr)
        sys.exit(1)

    # Find WAV files
    if args.single:
        wav_files = [args.single]
    else:
        wav_files = sorted(glob.glob(os.path.join(args.corpus_dir, "*.wav")))
    if not wav_files:
        print(f"No WAV files found in {args.corpus_dir}", file=sys.stderr)
        sys.exit(1)

    print(f"Processing {len(wav_files)} files...")

    file_results = []
    all_stats = {
        'lsf_sd': [],
        'voicing': [],
        'pitch_frac': [],
        'cb_frac': [],
        'error_frac': [],
        'pitch_gain_sat': 0,
        'cb_gain_sat': 0,
        'total_frames': 0,
    }

    with tempfile.TemporaryDirectory() as tmpdir:
        for wav_path in wav_files:
            name = os.path.basename(wav_path)
            prefix = os.path.join(tmpdir, os.path.splitext(name)[0])

            print(f"  {name}...", end="", flush=True)

            # Run diagnostic tool
            result = subprocess.run(
                [diag_bin, wav_path, prefix],
                capture_output=True, text=True, timeout=120
            )
            if result.returncode != 0:
                print(f" FAILED: {result.stderr.strip()}")
                continue

            # Measure PESQ for each component
            pesq_full  = compute_pesq(wav_path, prefix + "_full_recon.wav")
            pesq_lpc   = compute_pesq(wav_path, prefix + "_lpc_resynth.wav")
            pesq_pitch = compute_pesq(wav_path, prefix + "_pitch_only.wav")
            pesq_cb    = compute_pesq(wav_path, prefix + "_cb_only.wav")

            # Measure SNR
            ref = read_wav_samples(wav_path)
            deg = read_wav_samples(prefix + "_full_recon.wav")
            n = min(len(ref), len(deg))
            snr = compute_snr(ref[:n], deg[:n])

            file_results.append({
                'name': name,
                'pesq_full': pesq_full,
                'pesq_lpc': pesq_lpc,
                'pesq_pitch': pesq_pitch,
                'pesq_cb': pesq_cb,
                'snr': snr,
            })

            # Parse CSV metrics
            csv_path = prefix + "_metrics.csv"
            if os.path.exists(csv_path):
                stats = analyze_csv(csv_path)
                all_stats['lsf_sd'].extend(stats['lsf_sd'])
                all_stats['voicing'].extend(stats['voicing'])
                all_stats['pitch_frac'].extend(stats['pitch_frac'])
                all_stats['cb_frac'].extend(stats['cb_frac'])
                all_stats['error_frac'].extend(stats['error_frac'])
                all_stats['pitch_gain_sat'] += stats['pitch_gain_sat']
                all_stats['cb_gain_sat'] += stats['cb_gain_sat']
                all_stats['total_frames'] += stats['total_frames']

            print(f" PESQ={pesq_full:.3f} SNR={snr:.1f}dB")

    if not file_results:
        print("No files processed successfully.", file=sys.stderr)
        sys.exit(1)

    print_report(file_results, all_stats)


if __name__ == "__main__":
    main()
