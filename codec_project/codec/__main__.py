"""
CLI entry point for the CELP codec.

Usage:
    python -m codec.encoder input.wav output.bin [--config config.json]
    python -m codec.decoder input.bin output.wav [--config config.json]
    python -m codec input.wav output.wav          # roundtrip test
"""
from __future__ import annotations

import argparse
import json
import sys
import os

import numpy as np


def main():
    parser = argparse.ArgumentParser(
        description="CELP/ACELP codec — encode, decode, or roundtrip"
    )
    sub = parser.add_subparsers(dest="command", help="Command")

    # Encode
    enc = sub.add_parser("encode", help="Encode WAV to bitstream")
    enc.add_argument("input", help="Input WAV file")
    enc.add_argument("output", help="Output bitstream file")
    enc.add_argument("--config", help="JSON config file", default=None)

    # Decode
    dec = sub.add_parser("decode", help="Decode bitstream to WAV")
    dec.add_argument("input", help="Input bitstream file")
    dec.add_argument("output", help="Output WAV file")
    dec.add_argument("--config", help="JSON config file", default=None)

    # Roundtrip
    rt = sub.add_parser("roundtrip", help="Encode then decode (test)")
    rt.add_argument("input", help="Input WAV file")
    rt.add_argument("output", help="Output WAV file")
    rt.add_argument("--config", help="JSON config file", default=None)
    rt.add_argument("--bitstream", help="Intermediate bitstream path",
                    default="/tmp/celp_roundtrip.bin")

    args = parser.parse_args()

    if args.command is None:
        parser.print_help()
        sys.exit(1)

    from .config import CodecConfig
    from .encoder import CELPEncoder
    from .decoder import CELPDecoder

    # Load config
    if args.config:
        with open(args.config) as f:
            config = CodecConfig.from_dict(json.load(f))
    else:
        config = CodecConfig()

    if args.command == "encode":
        encoder = CELPEncoder(config)
        encoder.encode_file(args.input, args.output)
        print(f"Encoded {args.input} -> {args.output}")

    elif args.command == "decode":
        decoder = CELPDecoder(config)
        decoder.decode_file(args.input, args.output)
        print(f"Decoded {args.input} -> {args.output}")

    elif args.command == "roundtrip":
        encoder = CELPEncoder(config)
        decoder = CELPDecoder(config)
        encoder.encode_file(args.input, args.bitstream)
        decoder.decode_file(args.bitstream, args.output)

        # Compute SNR
        import soundfile as sf
        orig, _ = sf.read(args.input, dtype="float64")
        if orig.ndim > 1:
            orig = orig[:, 0]
        recon, _ = sf.read(args.output, dtype="float64")
        min_len = min(len(orig), len(recon))
        orig = orig[:min_len]
        recon = recon[:min_len]

        noise = orig - recon
        sig_power = np.sum(orig ** 2) + 1e-10
        noise_power = np.sum(noise ** 2) + 1e-10
        snr = 10 * np.log10(sig_power / noise_power)
        print(f"Roundtrip: {args.input} -> {args.output}")
        print(f"  SNR: {snr:.2f} dB")
        print(f"  Bitstream size: {os.path.getsize(args.bitstream)} bytes")
        bpf = config.compute_bits_per_frame()
        actual_bps = bpf['total'] / (config.frame_size_ms / 1000.0)
        print(f"  Effective bitrate: {actual_bps:.0f} bps")


if __name__ == "__main__":
    main()
