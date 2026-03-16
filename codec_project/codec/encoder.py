"""
CELP Encoder — analysis-by-synthesis speech encoder.

Signal flow per frame:
1. Pre-processing (HP filter + pre-emphasis)
2. LPC analysis (Levinson-Durbin)
3. LPC → LSP → quantize
4. Per subframe:
   a. Interpolate LSP, convert back to LPC
   b. Compute target (weighted speech minus zero-state response)
   c. Adaptive codebook (pitch) search
   d. Remove adaptive CB contribution from target
   e. Algebraic codebook search
   f. Quantize gains
   g. Update excitation memory
5. Pack bitstream
"""
from __future__ import annotations

import os
import struct
from typing import BinaryIO

import numpy as np

from .config import CodecConfig
from .preprocessing import HighPassFilter, PreEmphasisFilter
from .lpc import (
    lpc_analysis, lpc_to_lsp, lsp_to_lpc, lsp_interpolate,
    lpc_synthesis_filter, compute_impulse_response, lpc_zero_state_response,
)
from .quantization import SplitVQ, GainQuantizer
from .pitch import (
    open_loop_pitch_estimate, closed_loop_pitch_search,
    build_adaptive_codebook_excitation,
)
from .algebraic_cb import AlgebraicCodebook
from .bitstream import BitFrame, SubFrameData, pack, frame_size_bytes


class CELPEncoder:
    """Full CELP encoder with configurable parameters."""

    def __init__(self, config: CodecConfig, codebook_dir: str = "codec/codebooks"):
        config.validate()
        self.config = config
        self._frame_size = config.frame_size
        self._subframe_size = config.subframe_size
        self._num_subframes = config.num_subframes
        self._order = config.lpc_order

        # Pre-processing
        self._hp_filter = HighPassFilter(config.highpass_cutoff_hz, config.sample_rate)
        self._preemph = PreEmphasisFilter(config.preemphasis_coeff)

        # Quantizers
        self._lsp_vq = SplitVQ(
            codebook_size=config.lsp_codebook_size,
            num_splits=config.lsp_num_splits,
            dim=config.lpc_order,
        )
        self._gain_vq = GainQuantizer(codebook_size=config.gain_codebook_size)

        # Auto-load trained codebooks if available
        from .train_codebooks import load_codebooks_into_encoder
        self._codebooks_trained = load_codebooks_into_encoder(self, codebook_dir)

        # Algebraic codebook
        self._acb = AlgebraicCodebook(
            subframe_size=self._subframe_size,
            num_pulses=config.acb_num_pulses,
            num_tracks=config.acb_num_tracks,
        )

        # Encoder state
        self._prev_lsp = np.linspace(0.1, np.pi - 0.1, self._order)
        self._excitation_buf = np.zeros(config.pitch_max_lag + self._frame_size + 160)
        self._synth_state = np.zeros(self._order)  # synthesis filter memory

    def encode_frame(self, pcm_frame: np.ndarray) -> BitFrame:
        """
        Encode one frame of PCM samples.

        Parameters
        ----------
        pcm_frame : (frame_size,) float64 — raw PCM

        Returns
        -------
        BitFrame with all quantized indices
        """
        cfg = self.config
        N = self._frame_size
        Nsub = self._subframe_size
        P = self._order

        # ── 1. Pre-processing ─────────────────────────────────────────
        hp_frame = self._hp_filter.process(pcm_frame.astype(np.float64))
        pe_frame = self._preemph.process(hp_frame)

        # ── 2. LPC analysis ───────────────────────────────────────────
        lpc_coeffs, residual, lpc_gain = lpc_analysis(
            pe_frame, P, cfg.lpc_window_type, cfg.lpc_window_size
        )

        # ── 3. LPC → LSP → quantize ──────────────────────────────────
        lsp_curr = lpc_to_lsp(lpc_coeffs)
        # Stability check: ensure LSPs are ordered and in (0, pi)
        lsp_curr = _stabilize_lsp(lsp_curr)

        lsp_indices, lsp_q = self._lsp_vq.quantize(lsp_curr)
        lsp_q = _stabilize_lsp(lsp_q)

        # ── 4. Open-loop pitch estimate ───────────────────────────────
        ol_pitch = open_loop_pitch_estimate(
            residual, cfg.pitch_min_lag, cfg.pitch_max_lag
        )

        # ── 5. Subframe processing ────────────────────────────────────
        bit_frame = BitFrame(lsp_indices=lsp_indices, subframes=[])
        synth_state = self._synth_state.copy()

        for sf_idx in range(self._num_subframes):
            sf_start = sf_idx * Nsub
            sf_end = sf_start + Nsub
            target_speech = pe_frame[sf_start:sf_end]

            # Interpolate LSP for this subframe
            alpha = (sf_idx + 1) / self._num_subframes
            lsp_interp = lsp_interpolate(self._prev_lsp, lsp_q, alpha)
            lsp_interp = _stabilize_lsp(lsp_interp)
            lpc_sub = lsp_to_lpc(lsp_interp)

            # Impulse response of 1/A(z)
            h = compute_impulse_response(lpc_sub, Nsub)

            # Zero-state response (ringing from previous subframe)
            zsr = lpc_zero_state_response(lpc_sub, synth_state, Nsub)

            # Target for codebook search
            target = target_speech - zsr

            # ── Adaptive codebook search ──────────────────────────────
            # Widen search range for better pitch tracking
            search_min = max(cfg.pitch_min_lag, ol_pitch - 10)
            search_max = min(cfg.pitch_max_lag, ol_pitch + 10)

            pitch_lag, pitch_gain = closed_loop_pitch_search(
                target, h, self._excitation_buf,
                search_min, search_max, cfg.pitch_fractional,
            )

            # Build adaptive codebook excitation
            acb_exc = build_adaptive_codebook_excitation(
                self._excitation_buf, pitch_lag, Nsub, cfg.pitch_fractional
            )

            # Remove adaptive CB contribution from target
            acb_filtered = np.zeros(Nsub)
            for i in range(Nsub):
                for k in range(i + 1):
                    acb_filtered[i] += pitch_gain * acb_exc[k] * h[i - k]
            target2 = target - acb_filtered

            # ── Algebraic codebook search ─────────────────────────────
            fcb_index, cb_gain, fcb_exc = self._acb.search(target2, h)

            # ── Gain quantization ─────────────────────────────────────
            gain_idx, q_pitch_gain, q_cb_gain = self._gain_vq.quantize(
                pitch_gain, cb_gain
            )

            # ── Update excitation buffer ──────────────────────────────
            total_exc = q_pitch_gain * acb_exc + q_cb_gain * fcb_exc
            n_buf = len(self._excitation_buf)
            self._excitation_buf[:n_buf - Nsub] = self._excitation_buf[Nsub:]
            self._excitation_buf[n_buf - Nsub:] = total_exc

            # Update synthesis filter state
            _, synth_state = lpc_synthesis_filter(total_exc, lpc_sub, synth_state)

            # ── Encode pitch lag ──────────────────────────────────────
            int_lag = int(round(pitch_lag))
            frac_part = pitch_lag - int(np.floor(pitch_lag))
            lag_index = int_lag - cfg.pitch_min_lag
            lag_index = max(0, min(lag_index, cfg.pitch_max_lag - cfg.pitch_min_lag))

            if cfg.pitch_fractional == "half":
                frac_index = int(round(frac_part * 2)) % 2
            elif cfg.pitch_fractional == "third":
                frac_index = int(round(frac_part * 3)) % 3
            else:
                frac_index = 0

            # ── Pack subframe data ────────────────────────────────────
            sf_data = SubFrameData(
                pitch_lag_int=lag_index,
                pitch_lag_frac=frac_index,
                pitch_gain_index=min(q_pitch_gain_to_index(q_pitch_gain, cfg.pitch_gain_bits),
                                     (1 << cfg.pitch_gain_bits) - 1),
                fcb_index=fcb_index,
                gain_index=gain_idx,
            )
            bit_frame.subframes.append(sf_data)

            # Update open-loop estimate for next subframe
            ol_pitch = int_lag

        # Save state for next frame
        self._prev_lsp = lsp_q.copy()
        self._synth_state = synth_state.copy()

        return bit_frame

    def encode_file(self, input_wav: str, output_bitstream: str) -> None:
        """Encode a WAV file to a bitstream file."""
        import soundfile as sf

        audio, sr = sf.read(input_wav, dtype="float64")
        if audio.ndim > 1:
            audio = audio[:, 0]  # mono
        if sr != self.config.sample_rate:
            # Simple resampling via linear interpolation
            ratio = self.config.sample_rate / sr
            new_len = int(len(audio) * ratio)
            x_old = np.linspace(0, 1, len(audio))
            x_new = np.linspace(0, 1, new_len)
            audio = np.interp(x_new, x_old, audio)

        # Pad to frame boundary
        N = self._frame_size
        pad_len = (N - len(audio) % N) % N
        if pad_len > 0:
            audio = np.concatenate([audio, np.zeros(pad_len)])

        num_frames = len(audio) // N
        fsb = frame_size_bytes(self.config)

        with open(output_bitstream, "wb") as f:
            # Header: magic + config hash + frame count
            f.write(b"CELP")
            f.write(struct.pack("<I", num_frames))
            f.write(struct.pack("<I", fsb))

            for i in range(num_frames):
                frame_pcm = audio[i * N:(i + 1) * N]
                bit_frame = self.encode_frame(frame_pcm)
                packed = pack(bit_frame, self.config)
                # Pad or trim to exact frame size
                packed = packed[:fsb].ljust(fsb, b"\x00")
                f.write(packed)

    def reset(self) -> None:
        """Reset encoder state (for processing a new file)."""
        self._hp_filter.reset()
        self._preemph.reset()
        self._prev_lsp = np.linspace(0.1, np.pi - 0.1, self._order)
        self._excitation_buf[:] = 0.0
        self._synth_state[:] = 0.0


def _stabilize_lsp(lsp: np.ndarray, min_gap: float = 0.05) -> np.ndarray:
    """Ensure LSP frequencies are strictly increasing and in (0, pi)."""
    lsp = np.clip(lsp, 0.01, np.pi - 0.01)
    for i in range(1, len(lsp)):
        if lsp[i] <= lsp[i - 1] + min_gap:
            lsp[i] = lsp[i - 1] + min_gap
    # Re-clip in case pushing caused overflow
    lsp = np.clip(lsp, 0.01, np.pi - 0.01)
    return lsp


def q_pitch_gain_to_index(gain: float, bits: int) -> int:
    """Uniform scalar quantize pitch gain to index."""
    levels = 1 << bits
    # Map gain from [0, 1.2] to [0, levels-1]
    idx = int(round(gain / 1.2 * (levels - 1)))
    return max(0, min(idx, levels - 1))
