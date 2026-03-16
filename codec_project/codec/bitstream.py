"""
Bitstream packing/unpacking for the CELP codec.

BitFrame holds all quantised indices for one frame.
pack() / unpack() convert between BitFrame and raw bytes.
"""
from __future__ import annotations

import math
import struct
from dataclasses import dataclass, field
from typing import List, Tuple

import numpy as np

from .config import CodecConfig


@dataclass
class SubFrameData:
    """Quantized data for a single subframe."""
    pitch_lag_int: int = 0
    pitch_lag_frac: int = 0      # fractional part index
    pitch_gain_index: int = 0
    fcb_index: int = 0
    gain_index: int = 0


@dataclass
class BitFrame:
    """All quantized indices for one codec frame."""
    lsp_indices: List[int] = field(default_factory=list)
    subframes: List[SubFrameData] = field(default_factory=list)


class BitPacker:
    """Bit-level packer: accumulates bits and outputs bytes."""

    def __init__(self):
        self._bits: List[int] = []

    def write(self, value: int, num_bits: int) -> None:
        """Write *num_bits* from *value* (MSB first)."""
        for i in range(num_bits - 1, -1, -1):
            self._bits.append((value >> i) & 1)

    def flush(self) -> bytes:
        """Pad to byte boundary and return bytes."""
        # Pad
        while len(self._bits) % 8 != 0:
            self._bits.append(0)
        out = bytearray()
        for i in range(0, len(self._bits), 8):
            byte = 0
            for b in range(8):
                byte = (byte << 1) | self._bits[i + b]
            out.append(byte)
        return bytes(out)

    @property
    def num_bits(self) -> int:
        return len(self._bits)


class BitUnpacker:
    """Bit-level unpacker: reads bits from a byte buffer."""

    def __init__(self, data: bytes):
        self._bits: List[int] = []
        for byte in data:
            for i in range(7, -1, -1):
                self._bits.append((byte >> i) & 1)
        self._pos = 0

    def read(self, num_bits: int) -> int:
        """Read *num_bits* and return as unsigned integer."""
        value = 0
        for _ in range(num_bits):
            if self._pos < len(self._bits):
                value = (value << 1) | self._bits[self._pos]
                self._pos += 1
            else:
                value <<= 1
        return value

    @property
    def remaining(self) -> int:
        return len(self._bits) - self._pos


def _bit_widths(config: CodecConfig) -> dict:
    """Compute bit-widths for each field based on config."""
    lsp_bits = int(math.ceil(math.log2(max(config.lsp_codebook_size, 2))))
    pitch_lag_range = config.pitch_max_lag - config.pitch_min_lag + 1
    pitch_lag_bits = int(math.ceil(math.log2(max(pitch_lag_range, 2))))
    frac_bits = {"integer": 0, "half": 1, "third": 2}[config.pitch_fractional]
    positions_per_track = config.subframe_size // max(config.acb_num_tracks, 1)
    pos_bits = int(math.ceil(math.log2(max(positions_per_track, 2))))
    fcb_bits = config.acb_num_pulses * (pos_bits + 1)
    gain_bits = int(math.ceil(math.log2(max(config.gain_codebook_size, 2))))

    return {
        "lsp_per_split": lsp_bits,
        "pitch_lag": pitch_lag_bits,
        "pitch_frac": frac_bits,
        "pitch_gain": config.pitch_gain_bits,
        "fcb": fcb_bits,
        "gain": gain_bits,
    }


def pack(frame: BitFrame, config: CodecConfig) -> bytes:
    """Pack a BitFrame into bytes."""
    bw = _bit_widths(config)
    packer = BitPacker()

    # LSP indices
    for idx in frame.lsp_indices:
        packer.write(idx, bw["lsp_per_split"])

    # Subframes
    for sf in frame.subframes:
        packer.write(sf.pitch_lag_int, bw["pitch_lag"])
        if bw["pitch_frac"] > 0:
            packer.write(sf.pitch_lag_frac, bw["pitch_frac"])
        packer.write(sf.pitch_gain_index, bw["pitch_gain"])
        packer.write(sf.fcb_index, bw["fcb"])
        packer.write(sf.gain_index, bw["gain"])

    return packer.flush()


def unpack(data: bytes, config: CodecConfig) -> BitFrame:
    """Unpack bytes into a BitFrame."""
    bw = _bit_widths(config)
    unpacker = BitUnpacker(data)

    frame = BitFrame()

    # LSP indices
    for _ in range(config.lsp_num_splits):
        frame.lsp_indices.append(unpacker.read(bw["lsp_per_split"]))

    # Subframes
    for _ in range(config.num_subframes):
        sf = SubFrameData()
        sf.pitch_lag_int = unpacker.read(bw["pitch_lag"])
        if bw["pitch_frac"] > 0:
            sf.pitch_lag_frac = unpacker.read(bw["pitch_frac"])
        sf.pitch_gain_index = unpacker.read(bw["pitch_gain"])
        sf.fcb_index = unpacker.read(bw["fcb"])
        sf.gain_index = unpacker.read(bw["gain"])
        frame.subframes.append(sf)

    return frame


def frame_size_bytes(config: CodecConfig) -> int:
    """Number of bytes per packed frame."""
    bw = _bit_widths(config)
    total_bits = config.lsp_num_splits * bw["lsp_per_split"]
    sub_bits = (bw["pitch_lag"] + bw["pitch_frac"] + bw["pitch_gain"]
                + bw["fcb"] + bw["gain"])
    total_bits += config.num_subframes * sub_bits
    return math.ceil(total_bits / 8)
