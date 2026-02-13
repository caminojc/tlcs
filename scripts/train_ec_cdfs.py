#!/usr/bin/env python3
"""Train CDF tables for entropy coding from dumped parameter indices.

Reads binary dump from TLCS encoder (TLCS_DUMP_EC env var).
Format per frame (8 pulses): 92 int16 values:
  [16 lsf_idx] [4 lag_idx] [4 acb_vq_idx] [4×8 pulse_pos] [4×8 pulse_sign] [4 fcb_gain_idx]

Current LR config: 6-bit LSF, 6-bit pitch delta, 8 pulses, 5-bit FCB gain.
Outputs C code for trained CDF tables.
"""

import sys
import numpy as np

def load_ec_dump(path, order=16, n_subfr=4, num_pulses=8):
    """Load EC parameter dump."""
    data = np.fromfile(path, dtype=np.int16)
    vals_per_frame = order + n_subfr + n_subfr + n_subfr*num_pulses + n_subfr*num_pulses + n_subfr
    n_frames = len(data) // vals_per_frame
    data = data[:n_frames * vals_per_frame].reshape(n_frames, vals_per_frame)

    idx = 0
    lsf_indices = data[:, idx:idx+order]; idx += order
    lag_indices = data[:, idx:idx+n_subfr]; idx += n_subfr
    acb_vq_indices = data[:, idx:idx+n_subfr]; idx += n_subfr
    pulse_pos = data[:, idx:idx+n_subfr*num_pulses].reshape(n_frames, n_subfr, num_pulses); idx += n_subfr*num_pulses
    pulse_sign = data[:, idx:idx+n_subfr*num_pulses].reshape(n_frames, n_subfr, num_pulses); idx += n_subfr*num_pulses
    fcb_gain = data[:, idx:idx+n_subfr]; idx += n_subfr

    return lsf_indices, lag_indices, acb_vq_indices, pulse_pos, pulse_sign, fcb_gain, n_frames

def histogram_to_cdf(counts, total_cdf=16384, min_freq=1):
    """Convert histogram counts to CDF table with minimum frequency guarantee."""
    n = len(counts)
    # Ensure every symbol has at least min_freq
    freq = np.maximum(counts, min_freq).astype(np.float64)
    # Normalize to total_cdf
    freq = freq / freq.sum() * total_cdf
    freq = np.round(freq).astype(np.int32)
    freq = np.maximum(freq, min_freq)

    # Adjust to sum to exactly total_cdf
    diff = total_cdf - freq.sum()
    if diff != 0:
        # Add/remove from the largest bins
        sorted_idx = np.argsort(-freq)
        for i in range(abs(diff)):
            idx = sorted_idx[i % len(sorted_idx)]
            freq[idx] += 1 if diff > 0 else -1
            if freq[idx] < min_freq:
                freq[idx] = min_freq

    # Build CDF
    cdf = np.zeros(n + 1, dtype=np.uint16)
    cdf[0] = 0
    for i in range(n):
        cdf[i+1] = cdf[i] + freq[i]

    # Final fix: ensure cdf[-1] == total_cdf
    cdf[-1] = total_cdf

    return cdf

def compute_entropy(counts):
    """Compute entropy in bits from histogram."""
    total = counts.sum()
    if total == 0:
        return 0.0
    probs = counts / total
    probs = probs[probs > 0]
    return -np.sum(probs * np.log2(probs))

def main():
    dump_path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/tlcs_ec_dump.bin"

    print(f"Loading EC dump from {dump_path}...")
    lsf_idx, lag_idx, acb_idx, pulse_pos, pulse_sign, fcb_gain, n_frames = load_ec_dump(dump_path)
    print(f"  {n_frames} frames")

    # ── LSF delta indices (64 symbols, 6-bit) ──
    lsf_bits = 6
    lsf_nsym = 1 << lsf_bits
    print(f"\n{'='*60}")
    print(f"LSF delta indices ({lsf_nsym} symbols, {lsf_bits}-bit):")
    lsf_flat = lsf_idx.flatten()
    lsf_counts = np.bincount(lsf_flat.astype(np.int64), minlength=lsf_nsym)[:lsf_nsym]
    lsf_entropy = compute_entropy(lsf_counts)
    print(f"  Entropy: {lsf_entropy:.2f} bits (fixed: {lsf_bits}.0 bits)")
    print(f"  Savings: {(lsf_bits - lsf_entropy)/lsf_bits*100:.1f}%")
    print(f"  Distribution peak at {np.argmax(lsf_counts)} (count={lsf_counts.max()})")
    print(f"  Top 5 symbols: {np.argsort(-lsf_counts)[:5]}")
    lsf_cdf = histogram_to_cdf(lsf_counts)

    # ── Pitch absolute (512 symbols, 9-bit) ──
    # Absolute pitch at sf=0 and sf=2 (half-frame boundaries)
    print(f"\n{'='*60}")
    print("Pitch absolute lag (512 symbols):")
    pitch_abs = np.concatenate([lag_idx[:, 0], lag_idx[:, 2]])
    abs_counts = np.bincount(pitch_abs.astype(np.int64), minlength=512)[:512]
    abs_entropy = compute_entropy(abs_counts)
    print(f"  Entropy: {abs_entropy:.2f} bits (fixed: 9.0 bits)")
    print(f"  Savings: {(9.0 - abs_entropy)/9.0*100:.1f}%")
    print(f"  Most common lag idx: {np.argmax(abs_counts)} (count={abs_counts.max()})")
    abs_cdf = histogram_to_cdf(abs_counts)

    # ── Pitch delta (64 symbols, 6-bit, centered at 32) ──
    # Codec uses absolute at sf=0,2 and delta at sf=1,3
    # sf=1 delta from sf=0, sf=3 delta from sf=2
    pitch_delta_bits = 6
    pitch_delta_nsym = 1 << pitch_delta_bits
    delta_off = pitch_delta_nsym // 2
    print(f"\n{'='*60}")
    print(f"Pitch delta ({pitch_delta_nsym} symbols, {pitch_delta_bits}-bit):")
    pitch_deltas = []
    # sf=1 relative to sf=0
    delta1 = lag_idx[:, 1] - lag_idx[:, 0]
    delta1 = np.clip(delta1, -delta_off, delta_off - 1)
    pitch_deltas.append(delta1 + delta_off)
    # sf=3 relative to sf=2
    delta3 = lag_idx[:, 3] - lag_idx[:, 2]
    delta3 = np.clip(delta3, -delta_off, delta_off - 1)
    pitch_deltas.append(delta3 + delta_off)
    pitch_delta_flat = np.concatenate(pitch_deltas)
    delta_counts = np.bincount(pitch_delta_flat.astype(np.int64), minlength=pitch_delta_nsym)[:pitch_delta_nsym]
    delta_entropy = compute_entropy(delta_counts)
    print(f"  Entropy: {delta_entropy:.2f} bits (fixed: {pitch_delta_bits}.0 bits)")
    print(f"  Savings: {(pitch_delta_bits - delta_entropy)/pitch_delta_bits*100:.1f}%")
    print(f"  Peak at symbol {np.argmax(delta_counts)} (should be ~{delta_off} for zero delta)")
    delta_cdf = histogram_to_cdf(delta_counts)

    # ── ACB VQ index (8 symbols, 3-bit) ──
    print(f"\n{'='*60}")
    print("ACB VQ index (8 symbols):")
    acb_flat = acb_idx.flatten()
    acb_counts = np.bincount(acb_flat.astype(np.int64), minlength=8)[:8]
    acb_entropy = compute_entropy(acb_counts)
    print(f"  Entropy: {acb_entropy:.2f} bits (fixed: 3.0 bits)")
    print(f"  Savings: {(3.0 - acb_entropy)/3.0*100:.1f}%")
    print(f"  Histogram: {acb_counts}")
    acb_cdf = histogram_to_cdf(acb_counts)

    # ── Pulse positions (10 symbols for 80/8=10 positions_per_track) ──
    n_pos = 10  # subfr_size / num_pulses = 80/8
    print(f"\n{'='*60}")
    print(f"Pulse positions ({n_pos} symbols):")
    pos_flat = pulse_pos.flatten()
    pos_counts = np.bincount(pos_flat.astype(np.int64), minlength=n_pos)[:n_pos]
    pos_entropy = compute_entropy(pos_counts)
    print(f"  Entropy: {pos_entropy:.2f} bits (uniform: {np.log2(n_pos):.2f} bits)")
    print(f"  Savings: {(np.log2(n_pos) - pos_entropy)/np.log2(n_pos)*100:.1f}%")
    pos_cdf = histogram_to_cdf(pos_counts)

    # ── Pulse signs (2 symbols) ──
    print(f"\n{'='*60}")
    print("Pulse signs (2 symbols):")
    sign_flat = pulse_sign.flatten()
    sign_counts = np.bincount(sign_flat.astype(np.int64), minlength=2)[:2]
    sign_entropy = compute_entropy(sign_counts)
    print(f"  Entropy: {sign_entropy:.2f} bits (uniform: 1.0 bits)")
    print(f"  Counts: negative={sign_counts[0]}, positive={sign_counts[1]}")
    sign_cdf = histogram_to_cdf(sign_counts)

    # ── FCB gain index (32 symbols, 5-bit) ──
    print(f"\n{'='*60}")
    print("FCB gain index (32 symbols):")
    gain_flat = fcb_gain.flatten()
    gain_counts = np.bincount(gain_flat.astype(np.int64), minlength=32)[:32]
    gain_entropy = compute_entropy(gain_counts)
    print(f"  Entropy: {gain_entropy:.2f} bits (fixed: 5.0 bits)")
    print(f"  Savings: {(5.0 - gain_entropy)/5.0*100:.1f}%")
    print(f"  Peak at symbol {np.argmax(gain_counts)}")
    gain_cdf = histogram_to_cdf(gain_counts)

    # ── Total entropy budget ──
    n_pulses = 8  # current config
    n_subfr = 4
    n_abs_pitch = 2   # sf=0, sf=2
    n_delta_pitch = 2  # sf=1, sf=3
    print(f"\n{'='*60}")
    total_fixed = (16*lsf_bits + n_abs_pitch*9 + n_delta_pitch*pitch_delta_bits + n_subfr*3 +
                   n_subfr*n_pulses*np.log2(n_pos) + n_subfr*n_pulses*1 + n_subfr*5)
    total_entropy = (16*lsf_entropy + n_abs_pitch*abs_entropy + n_delta_pitch*delta_entropy + n_subfr*acb_entropy +
                     n_subfr*n_pulses*pos_entropy + n_subfr*n_pulses*sign_entropy + n_subfr*gain_entropy)
    print(f"Total fixed bits/frame:   {total_fixed:.1f}")
    print(f"Total entropy bits/frame: {total_entropy:.1f}")
    print(f"Theoretical savings:      {total_fixed - total_entropy:.1f} bits ({(total_fixed - total_entropy)/total_fixed*100:.1f}%)")
    print(f"Theoretical bitrate:      {total_entropy / 0.02 / 1000:.1f} kbps")

    # ── Generate C code ──
    print(f"\n{'='*60}")
    print("Generating trained CDF tables...")

    lines = []
    lines.append("/* Auto-generated trained CDF tables for entropy coding */")
    lines.append(f"/* Trained on {n_frames} frames from speech corpus */")
    lines.append("")

    def emit_cdf(name, cdf, n_sym):
        lines.append(f"/* {name}: {n_sym} symbols */")
        lines.append(f"static const uint16_t {name}[{n_sym + 1}] = {{")
        # Print in rows of 8
        vals = [str(int(v)) for v in cdf]
        for i in range(0, len(vals), 8):
            row = ", ".join(vals[i:i+8])
            comma = "," if i + 8 < len(vals) else ""
            lines.append(f"    {row}{comma}")
        lines.append("};")
        lines.append("")

    emit_cdf("ec_cdf_lsf_delta_trained", lsf_cdf, lsf_nsym)
    emit_cdf("ec_cdf_pitch_abs_trained", abs_cdf, 512)
    emit_cdf("ec_cdf_pitch_delta_trained", delta_cdf, pitch_delta_nsym)
    emit_cdf("ec_cdf_acb_vq_trained", acb_cdf, 8)
    emit_cdf("ec_cdf_pulse_pos_trained", pos_cdf, n_pos)
    emit_cdf("ec_cdf_pulse_sign_trained", sign_cdf, 2)
    emit_cdf("ec_cdf_fcb_gain_trained", gain_cdf, 32)

    output_path = "/Users/jonathanchristensen/CLionProjects/TLCS/scripts/trained_cdfs.c"
    with open(output_path, "w") as f:
        f.write("\n".join(lines))
    print(f"Written to {output_path}")

if __name__ == "__main__":
    main()
