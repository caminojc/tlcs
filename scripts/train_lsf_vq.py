#!/usr/bin/env python3
"""Train split-VQ codebooks for LSF delta quantization.

Reads binary LSF dump from the TLCS encoder (TLCS_DUMP_LSF env var).
Format: per frame, 48 float32 values: [16 raw_lsf] [16 lsf_pred] [16 delta]

Trains split-VQ codebooks on the delta vectors.
Outputs C code for the codebook arrays.
"""

import sys
import numpy as np
from sklearn.cluster import MiniBatchKMeans

def load_lsf_dump(path, order=16):
    """Load LSF dump binary file."""
    data = np.fromfile(path, dtype=np.float32)
    floats_per_frame = order * 3  # raw, pred, delta
    n_frames = len(data) // floats_per_frame
    data = data[:n_frames * floats_per_frame].reshape(n_frames, floats_per_frame)

    raw_lsf = data[:, :order]
    lsf_pred = data[:, order:2*order]
    delta = data[:, 2*order:3*order]

    return raw_lsf, lsf_pred, delta, n_frames

def train_split_vq(deltas, splits, bits_per_split):
    """Train split-VQ codebooks.

    splits: list of (start, end) index pairs for splitting the vector
    bits_per_split: list of bit counts for each split
    """
    codebooks = []
    total_distortion = 0.0

    for (start, end), bits in zip(splits, bits_per_split):
        n_entries = 1 << bits
        sub_data = deltas[:, start:end]
        dim = end - start

        print(f"  Training split [{start}:{end}] (dim={dim}): {n_entries} entries ({bits} bits)")

        kmeans = MiniBatchKMeans(
            n_clusters=n_entries,
            random_state=42,
            batch_size=min(4096, len(sub_data)),
            n_init=3,
            max_iter=300,
        )
        kmeans.fit(sub_data)

        # Compute distortion
        labels = kmeans.predict(sub_data)
        recon = kmeans.cluster_centers_[labels]
        mse = np.mean((sub_data - recon) ** 2)
        max_err = np.max(np.abs(sub_data - recon))

        print(f"    MSE={mse:.6f}, max_err={max_err:.4f} rad")

        codebooks.append(kmeans.cluster_centers_)
        total_distortion += mse * dim

    total_dim = sum(end - start for start, end in splits)
    print(f"  Total MSE per coefficient: {total_distortion/total_dim:.6f}")

    return codebooks

def evaluate_scalar_baseline(deltas, bits, range_val):
    """Evaluate current scalar quantization for comparison."""
    levels = 1 << bits
    step = 2.0 * range_val / levels

    clipped = np.clip(deltas, -range_val, range_val)
    indices = np.round((clipped + range_val) / step).astype(int)
    indices = np.clip(indices, 0, levels - 1)
    recon = indices * step - range_val

    mse = np.mean((deltas - recon) ** 2)
    max_err = np.max(np.abs(deltas - recon))

    return mse, max_err

def generate_c_code(codebooks, splits, bits_per_split, order=16):
    """Generate C code for VQ codebooks."""
    lines = []
    lines.append("/* Auto-generated LSF split-VQ codebooks */")
    lines.append(f"/* Order={order}, splits={len(splits)} */")
    lines.append("")
    lines.append("#include <stdint.h>")
    lines.append("")

    total_bits = sum(bits_per_split)
    lines.append(f"#define LSF_VQ_NUM_SPLITS  {len(splits)}")
    lines.append(f"#define LSF_VQ_TOTAL_BITS  {total_bits}")
    lines.append(f"#define LSF_VQ_ORDER       {order}")
    lines.append("")

    for idx, ((start, end), bits, cb) in enumerate(zip(splits, bits_per_split, codebooks)):
        dim = end - start
        n_entries = 1 << bits
        lines.append(f"/* Split {idx}: LSF[{start}:{end}], dim={dim}, {bits} bits ({n_entries} entries) */")
        lines.append(f"#define LSF_VQ_SPLIT{idx}_START  {start}")
        lines.append(f"#define LSF_VQ_SPLIT{idx}_DIM    {dim}")
        lines.append(f"#define LSF_VQ_SPLIT{idx}_BITS   {bits}")
        lines.append(f"#define LSF_VQ_SPLIT{idx}_SIZE   {n_entries}")
        lines.append("")

        # Quantize to int16 with scale factor for compact storage
        # Find range
        cb_min = cb.min()
        cb_max = cb.max()
        abs_max = max(abs(cb_min), abs(cb_max))
        # Use float storage for simplicity (can optimize later)

        lines.append(f"static const float lsf_vq_cb{idx}[{n_entries}][{dim}] = {{")
        for i in range(n_entries):
            vals = ", ".join(f"{v:10.6f}" for v in cb[i])
            comma = "," if i < n_entries - 1 else ""
            lines.append(f"    {{ {vals} }}{comma}")
        lines.append("};")
        lines.append("")

    # Split configuration
    lines.append("static const int lsf_vq_split_start[] = {" +
                 ", ".join(str(s) for s, e in splits) + "};")
    lines.append("static const int lsf_vq_split_dim[] = {" +
                 ", ".join(str(e-s) for s, e in splits) + "};")
    lines.append("static const int lsf_vq_split_bits[] = {" +
                 ", ".join(str(b) for b in bits_per_split) + "};")

    return "\n".join(lines)


def main():
    dump_path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/tlcs_lsf_dump.bin"

    print(f"Loading LSF dump from {dump_path}...")
    raw_lsf, lsf_pred, deltas, n_frames = load_lsf_dump(dump_path)
    print(f"  {n_frames} frames, order=16")

    # Statistics
    print(f"\nDelta statistics:")
    print(f"  Mean: {np.mean(deltas, axis=0)}")
    print(f"  Std:  {np.std(deltas, axis=0)}")
    print(f"  Min:  {np.min(deltas, axis=0)}")
    print(f"  Max:  {np.max(deltas, axis=0)}")
    print(f"  Overall std: {np.std(deltas):.4f}")
    print(f"  Values in [-0.8, 0.8]: {np.mean(np.abs(deltas) <= 0.8)*100:.1f}%")

    # Baseline: current scalar 5-bit quantization
    print(f"\nBaseline scalar quantization (5-bit, +-0.8 rad):")
    scalar_mse, scalar_max = evaluate_scalar_baseline(deltas, 5, 0.8)
    print(f"  MSE={scalar_mse:.6f}, max_err={scalar_max:.4f} rad")

    # Try different split-VQ configurations
    configs = [
        ("2-split [0:8][8:16], 8+8=16 bits", [(0,8), (8,16)], [8, 8]),
        ("2-split [0:8][8:16], 9+9=18 bits", [(0,8), (8,16)], [9, 9]),
        ("2-split [0:8][8:16], 10+10=20 bits", [(0,8), (8,16)], [10, 10]),
        ("4-split [0:4][4:8][8:12][12:16], 7+7+7+7=28 bits", [(0,4),(4,8),(8,12),(12,16)], [7,7,7,7]),
        ("4-split [0:4][4:8][8:12][12:16], 8+8+8+8=32 bits", [(0,4),(4,8),(8,12),(12,16)], [8,8,8,8]),
    ]

    best_config = None
    best_mse = float('inf')
    best_codebooks = None

    for name, splits, bits in configs:
        total_bits = sum(bits)
        print(f"\n{'='*60}")
        print(f"Config: {name}")
        print(f"  Total bits: {total_bits} (saves {80 - total_bits} bits vs scalar 80)")

        codebooks = train_split_vq(deltas, splits, bits)

        # Evaluate total distortion
        total_mse = 0.0
        for (start, end), cb in zip(splits, codebooks):
            sub_data = deltas[:, start:end]
            # Find nearest codebook entry
            dists = np.sum((sub_data[:, np.newaxis, :] - cb[np.newaxis, :, :]) ** 2, axis=2)
            best_idx = np.argmin(dists, axis=1)
            recon = cb[best_idx]
            total_mse += np.mean((sub_data - recon) ** 2) * (end - start)

        avg_mse = total_mse / 16
        improvement = (scalar_mse - avg_mse) / scalar_mse * 100
        print(f"  Average MSE: {avg_mse:.6f} ({improvement:+.1f}% vs scalar)")

        if avg_mse < best_mse:
            best_mse = avg_mse
            best_config = (name, splits, bits)
            best_codebooks = codebooks

    # Generate C code for best config
    print(f"\n{'='*60}")
    print(f"Best config: {best_config[0]}")
    print(f"Generating C code...")

    c_code = generate_c_code(best_codebooks, best_config[1], best_config[2])

    output_path = "/Users/jonathanchristensen/CLionProjects/TLCS/src/codec/tlcs_lsf_vq_codebook.h"
    with open(output_path, "w") as f:
        f.write(c_code)
    print(f"Written to {output_path}")

    # Also output the recommended split for integration
    print(f"\nRecommended integration:")
    print(f"  Splits: {best_config[1]}")
    print(f"  Bits:   {best_config[2]}")
    print(f"  Total:  {sum(best_config[2])} bits (saves {80 - sum(best_config[2])} from scalar 80)")

if __name__ == "__main__":
    main()
