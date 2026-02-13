#!/usr/bin/env python3
"""Train split-VQ codebooks at specific sizes for LR and VLR modes.

The key insight: the current codebooks were trained at 256 entries but
LR mode only uses entries 0-127 and VLR uses 0-63. Since KMeans cluster
indices are arbitrary, this gives a RANDOM subset rather than an optimal
codebook at the target size.

This script trains SEPARATE codebooks at the exact sizes needed:
- 128 entries per split for LR (7-bit mode)
- 64 entries per split for VLR (6-bit mode)

Outputs C code with both codebook sets.
"""

import sys
import numpy as np
from sklearn.cluster import MiniBatchKMeans

def load_lsf_dump(path, order=16):
    data = np.fromfile(path, dtype=np.float32)
    floats_per_frame = order * 3
    n_frames = len(data) // floats_per_frame
    data = data[:n_frames * floats_per_frame].reshape(n_frames, floats_per_frame)
    raw_lsf = data[:, :order]
    lsf_pred = data[:, order:2*order]
    delta = data[:, 2*order:3*order]
    return raw_lsf, lsf_pred, delta, n_frames

def train_split_vq(deltas, n_entries, splits=None):
    if splits is None:
        splits = [(0,4), (4,8), (8,12), (12,16)]
    codebooks = []
    for start, end in splits:
        sub_data = deltas[:, start:end]
        dim = end - start
        kmeans = MiniBatchKMeans(
            n_clusters=n_entries,
            random_state=42,
            batch_size=min(4096, len(sub_data)),
            n_init=5,
            max_iter=500,
        )
        kmeans.fit(sub_data)
        labels = kmeans.predict(sub_data)
        recon = kmeans.cluster_centers_[labels]
        mse = np.mean((sub_data - recon) ** 2)
        max_err = np.max(np.abs(sub_data - recon))
        print(f"  Split [{start}:{end}]: {n_entries} entries, MSE={mse:.6f}, max_err={max_err:.4f}")
        codebooks.append(kmeans.cluster_centers_)
    return codebooks

def evaluate_current(deltas, codebook_path):
    """Evaluate current codebook quality at different truncation levels."""
    # Load current codebooks from C source (we'll just retrain and compare)
    pass

def generate_c_tables(cb_128, cb_64):
    """Generate C code with separate codebook tables for LR (128) and VLR (64)."""
    lines = []
    lines.append("/* Auto-generated LSF split-VQ codebooks */")
    lines.append("/* Separate codebooks trained at target sizes for LR and VLR */")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("")
    lines.append("#define LSF_VQ_NUM_SPLITS  4")
    lines.append("#define LSF_VQ_SPLIT_DIM   4")
    lines.append("#define LSF_VQ_SPLIT_SIZE  128  /* max entries (LR) */")
    lines.append("#define LSF_VQ_ORDER       16")
    lines.append("")

    # LR codebooks (128 entries)
    lines.append("/* ══════════════════════════════════════════════════════════════")
    lines.append(" *  LR codebooks: 128 entries per split (7-bit mode)")
    lines.append(" * ══════════════════════════════════════════════════════════════ */")
    lines.append("")
    for idx, cb in enumerate(cb_128):
        n = len(cb)
        lines.append(f"const float lsf_vq_cb{idx}[{n}][4] = {{")
        for i in range(n):
            vals = ", ".join(f"{v:10.6f}f" for v in cb[i])
            comma = "," if i < n - 1 else ""
            lines.append(f"    {{ {vals} }}{comma}")
        lines.append("};")
        lines.append("")

    # VLR codebooks (64 entries)
    lines.append("/* ══════════════════════════════════════════════════════════════")
    lines.append(" *  VLR codebooks: 64 entries per split (6-bit mode)")
    lines.append(" * ══════════════════════════════════════════════════════════════ */")
    lines.append("")
    for idx, cb in enumerate(cb_64):
        n = len(cb)
        lines.append(f"const float lsf_vq_cb_vlr{idx}[{n}][4] = {{")
        for i in range(n):
            vals = ", ".join(f"{v:10.6f}f" for v in cb[i])
            comma = "," if i < n - 1 else ""
            lines.append(f"    {{ {vals} }}{comma}")
        lines.append("};")
        lines.append("")

    return "\n".join(lines)


def main():
    dump_path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/tlcs_lsf_dump.bin"

    print(f"Loading LSF dump from {dump_path}...")
    raw_lsf, lsf_pred, deltas, n_frames = load_lsf_dump(dump_path)
    print(f"  {n_frames} frames")

    print(f"\nDelta statistics:")
    print(f"  Overall std: {np.std(deltas):.4f}")
    print(f"  Per-coeff std: {np.std(deltas, axis=0)}")

    # First: evaluate the CURRENT codebook quality (truncated)
    # by training at 256 and measuring how truncation to 128/64 degrades quality
    print(f"\n{'='*60}")
    print("Training 256-entry codebook (to measure truncation effect):")
    cb_256 = train_split_vq(deltas, 256)

    # Evaluate at full 256
    total_mse_256 = 0.0
    for s, (start, end) in enumerate([(0,4),(4,8),(8,12),(12,16)]):
        sub = deltas[:, start:end]
        dists = np.sum((sub[:, None, :] - cb_256[s][None, :, :]) ** 2, axis=2)
        best_idx = np.argmin(dists, axis=1)
        recon = cb_256[s][best_idx]
        total_mse_256 += np.mean((sub - recon) ** 2) * 4
    print(f"  Full 256-entry MSE/coeff: {total_mse_256/16:.6f}")

    # Evaluate truncated to 128 (what LR currently does)
    total_mse_trunc128 = 0.0
    for s, (start, end) in enumerate([(0,4),(4,8),(8,12),(12,16)]):
        sub = deltas[:, start:end]
        dists = np.sum((sub[:, None, :] - cb_256[s][:128][None, :, :]) ** 2, axis=2)
        best_idx = np.argmin(dists, axis=1)
        recon = cb_256[s][:128][best_idx]
        total_mse_trunc128 += np.mean((sub - recon) ** 2) * 4
    print(f"  Truncated-to-128 MSE/coeff: {total_mse_trunc128/16:.6f} (CURRENT LR)")

    # Evaluate truncated to 64 (what VLR currently does)
    total_mse_trunc64 = 0.0
    for s, (start, end) in enumerate([(0,4),(4,8),(8,12),(12,16)]):
        sub = deltas[:, start:end]
        dists = np.sum((sub[:, None, :] - cb_256[s][:64][None, :, :]) ** 2, axis=2)
        best_idx = np.argmin(dists, axis=1)
        recon = cb_256[s][:64][best_idx]
        total_mse_trunc64 += np.mean((sub - recon) ** 2) * 4
    print(f"  Truncated-to-64 MSE/coeff:  {total_mse_trunc64/16:.6f} (CURRENT VLR)")

    # Now train at correct sizes
    print(f"\n{'='*60}")
    print("Training 128-entry codebook (for LR):")
    cb_128 = train_split_vq(deltas, 128)

    total_mse_128 = 0.0
    for s, (start, end) in enumerate([(0,4),(4,8),(8,12),(12,16)]):
        sub = deltas[:, start:end]
        dists = np.sum((sub[:, None, :] - cb_128[s][None, :, :]) ** 2, axis=2)
        best_idx = np.argmin(dists, axis=1)
        recon = cb_128[s][best_idx]
        total_mse_128 += np.mean((sub - recon) ** 2) * 4
    print(f"  Proper 128-entry MSE/coeff: {total_mse_128/16:.6f}")
    improvement_128 = (total_mse_trunc128 - total_mse_128) / total_mse_trunc128 * 100
    print(f"  Improvement vs truncated: {improvement_128:+.1f}%")

    print(f"\n{'='*60}")
    print("Training 64-entry codebook (for VLR):")
    cb_64 = train_split_vq(deltas, 64)

    total_mse_64 = 0.0
    for s, (start, end) in enumerate([(0,4),(4,8),(8,12),(12,16)]):
        sub = deltas[:, start:end]
        dists = np.sum((sub[:, None, :] - cb_64[s][None, :, :]) ** 2, axis=2)
        best_idx = np.argmin(dists, axis=1)
        recon = cb_64[s][best_idx]
        total_mse_64 += np.mean((sub - recon) ** 2) * 4
    print(f"  Proper 64-entry MSE/coeff:  {total_mse_64/16:.6f}")
    improvement_64 = (total_mse_trunc64 - total_mse_64) / total_mse_trunc64 * 100
    print(f"  Improvement vs truncated: {improvement_64:+.1f}%")

    # Generate C code
    print(f"\n{'='*60}")
    print("Generating C code...")
    c_code = generate_c_tables(cb_128, cb_64)

    output_path = "/Users/jonathanchristensen/CLionProjects/TLCS/src/codec/tlcs_lsf_vq_tables.c"
    with open(output_path, "w") as f:
        f.write(c_code)
    print(f"Written to {output_path}")

    print(f"\nSummary:")
    print(f"  LR (128 entries):  MSE {total_mse_trunc128/16:.6f} → {total_mse_128/16:.6f} ({improvement_128:+.1f}%)")
    print(f"  VLR (64 entries):  MSE {total_mse_trunc64/16:.6f} → {total_mse_64/16:.6f} ({improvement_64:+.1f}%)")

if __name__ == "__main__":
    main()
