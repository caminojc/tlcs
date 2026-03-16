"""
CELP Decoder — synthesis-based speech decoder.

Signal flow per frame:
1. Unpack bitstream
2. Dequantize LSPs
3. Per subframe:
   a. Interpolate LSP → LPC
   b. Dequantize gains
   c. Build adaptive codebook excitation from pitch lag
   d. Build fixed codebook excitation from FCB index
   e. Combine excitations: exc = gp * acb + gc * fcb
   f. Synthesize speech through 1/A(z)
4. Post-processing (postfilter + de-emphasis)
"""
from __future__ import annotations

import os
import struct
from typing import BinaryIO

import numpy as np

from .config import CodecConfig
from .preprocessing import DeEmphasisFilter
from .lpc import (
    lsp_to_lpc, lsp_interpolate,
    lpc_synthesis_filter,
)
from .quantization import SplitVQ, GainQuantizer
from .pitch import build_adaptive_codebook_excitation
from .algebraic_cb import AlgebraicCodebook
from .postfilter import PostFilter, HarmonicPostFilter
from .bitstream import BitFrame, SubFrameData, unpack, frame_size_bytes


class CELPDecoder:
    """Full CELP decoder with configurable parameters."""

    def __init__(self, config: CodecConfig, codebook_dir: str = "codec/codebooks"):
        config.validate()
        self.config = config
        self._frame_size = config.frame_size
        self._subframe_size = config.subframe_size
        self._num_subframes = config.num_subframes
        self._order = config.lpc_order

        # Quantizers (must match encoder)
        self._lsp_vq = SplitVQ(
            codebook_size=config.lsp_codebook_size,
            num_splits=config.lsp_num_splits,
            dim=config.lpc_order,
        )
        self._gain_vq = GainQuantizer(codebook_size=config.gain_codebook_size)

        # Auto-load trained codebooks if available
        from .train_codebooks import load_codebooks_into_decoder
        self._codebooks_trained = load_codebooks_into_decoder(self, codebook_dir)

        # Algebraic codebook
        self._acb = AlgebraicCodebook(
            subframe_size=self._subframe_size,
            num_pulses=config.acb_num_pulses,
            num_tracks=config.acb_num_tracks,
        )

        # Post-processing
        self._postfilter = PostFilter()
        self._harm_postfilter = HarmonicPostFilter(max_lag=config.pitch_max_lag)
        self._deemph = DeEmphasisFilter(config.preemphasis_coeff)

        # Decoder state
        self._prev_lsp = np.linspace(0.1, np.pi - 0.1, self._order)
        self._excitation_buf = np.zeros(config.pitch_max_lag + self._frame_size + 160)
        self._synth_state = np.zeros(self._order)

    def decode_frame(self, bit_frame: BitFrame) -> np.ndarray:
        """
        Decode one frame from quantized indices.

        Returns
        -------
        pcm : (frame_size,) float64 — decoded speech
        """
        cfg = self.config
        N = self._frame_size
        Nsub = self._subframe_size
        P = self._order

        # ── 1. Dequantize LSPs ────────────────────────────────────────
        lsp_q = self._lsp_vq.dequantize(bit_frame.lsp_indices)
        lsp_q = _stabilize_lsp(lsp_q)

        # ── 2. Subframe synthesis ─────────────────────────────────────
        output = np.zeros(N)
        synth_state = self._synth_state.copy()

        for sf_idx in range(self._num_subframes):
            sf_data = bit_frame.subframes[sf_idx]

            # Interpolate LSP
            alpha = (sf_idx + 1) / self._num_subframes
            lsp_interp = lsp_interpolate(self._prev_lsp, lsp_q, alpha)
            lsp_interp = _stabilize_lsp(lsp_interp)
            lpc_sub = lsp_to_lpc(lsp_interp)

            # Decode pitch lag
            int_lag = sf_data.pitch_lag_int + cfg.pitch_min_lag
            if cfg.pitch_fractional == "half":
                frac = sf_data.pitch_lag_frac / 2.0
            elif cfg.pitch_fractional == "third":
                frac = sf_data.pitch_lag_frac / 3.0
            else:
                frac = 0.0
            pitch_lag = int_lag + frac

            # Dequantize gains
            q_pg, q_cg = self._gain_vq.dequantize(sf_data.gain_index)

            # Build adaptive codebook excitation
            acb_exc = build_adaptive_codebook_excitation(
                self._excitation_buf, pitch_lag, Nsub, cfg.pitch_fractional
            )

            # Build fixed codebook excitation
            fcb_exc = self._acb.decode(sf_data.fcb_index)

            # Combine excitations
            total_exc = q_pg * acb_exc + q_cg * fcb_exc

            # Update excitation buffer
            n_buf = len(self._excitation_buf)
            self._excitation_buf[:n_buf - Nsub] = self._excitation_buf[Nsub:]
            self._excitation_buf[n_buf - Nsub:] = total_exc

            # Synthesis filter
            speech, synth_state = lpc_synthesis_filter(
                total_exc, lpc_sub, synth_state
            )

            # Harmonic postfilter (pitch-based, for naturalness)
            speech = self._harm_postfilter.process(
                speech, int(round(pitch_lag)), voiced=True
            )

            # Formant postfilter
            if cfg.postfilter_enabled:
                speech = self._postfilter.process(
                    speech, lpc_sub,
                    tilt_coeff=cfg.postfilter_tilt_coeff,
                    formant_coeff=cfg.postfilter_formant_coeff,
                )

            output[sf_idx * Nsub:(sf_idx + 1) * Nsub] = speech

        # ── 3. De-emphasis ────────────────────────────────────────────
        output = self._deemph.process(output)

        # Save state
        self._prev_lsp = lsp_q.copy()
        self._synth_state = synth_state.copy()

        return output

    def decode_file(self, input_bitstream: str, output_wav: str) -> None:
        """Decode a bitstream file to WAV."""
        import soundfile as sf

        fsb = frame_size_bytes(self.config)

        with open(input_bitstream, "rb") as f:
            # Read header
            magic = f.read(4)
            if magic != b"CELP":
                raise ValueError(f"Invalid bitstream magic: {magic!r}")
            num_frames = struct.unpack("<I", f.read(4))[0]
            stored_fsb = struct.unpack("<I", f.read(4))[0]

            all_pcm = []
            for _ in range(num_frames):
                data = f.read(stored_fsb)
                if len(data) < stored_fsb:
                    break
                bit_frame = unpack(data, self.config)
                pcm = self.decode_frame(bit_frame)
                all_pcm.append(pcm)

        audio = np.concatenate(all_pcm) if all_pcm else np.array([])

        # Clip to [-1, 1] for WAV output
        audio = np.clip(audio, -1.0, 1.0)
        sf.write(output_wav, audio, self.config.sample_rate)

    def reset(self) -> None:
        """Reset decoder state."""
        self._postfilter.reset()
        self._deemph.reset()
        self._prev_lsp = np.linspace(0.1, np.pi - 0.1, self._order)
        self._excitation_buf[:] = 0.0
        self._synth_state[:] = 0.0


def _stabilize_lsp(lsp: np.ndarray, min_gap: float = 0.05) -> np.ndarray:
    """Ensure LSP frequencies are strictly increasing and in (0, pi)."""
    lsp = np.clip(lsp, 0.01, np.pi - 0.01)
    for i in range(1, len(lsp)):
        if lsp[i] <= lsp[i - 1] + min_gap:
            lsp[i] = lsp[i - 1] + min_gap
    lsp = np.clip(lsp, 0.01, np.pi - 0.01)
    return lsp
