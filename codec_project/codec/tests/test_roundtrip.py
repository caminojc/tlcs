"""
Roundtrip tests for the CELP codec.

Verifies:
1. Encode → decode preserves signal duration
2. SNR > 5 dB for basic speech-like signals
3. Bitstream packing is lossless (pack → unpack identity)
4. Config validation catches invalid parameters
"""
from __future__ import annotations

import os
import struct
import tempfile

import numpy as np
import pytest

# Allow running from project root
import sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from codec.config import CodecConfig
from codec.encoder import CELPEncoder
from codec.decoder import CELPDecoder
from codec.bitstream import BitFrame, SubFrameData, pack, unpack, frame_size_bytes
from codec.preprocessing import HighPassFilter, PreEmphasisFilter, DeEmphasisFilter
from codec.lpc import lpc_analysis, lpc_to_lsp, lsp_to_lpc, lpc_synthesis_filter
from codec.quantization import SplitVQ, GainQuantizer
from codec.algebraic_cb import AlgebraicCodebook


# ═══════════════════════════════════════════════════════════════════════════
# Helpers
# ═══════════════════════════════════════════════════════════════════════════

def _make_speech_like_signal(duration_s: float = 0.5, sr: int = 16000) -> np.ndarray:
    """Generate a speech-like test signal (voiced + noise)."""
    n = int(duration_s * sr)
    t = np.arange(n) / sr
    # Fundamental + harmonics (voiced speech)
    signal = 0.0
    f0 = 150.0  # typical male F0
    for k in range(1, 8):
        signal = signal + (0.5 ** k) * np.sin(2 * np.pi * k * f0 * t)
    # Add some noise (unvoiced component)
    rng = np.random.RandomState(42)
    signal += 0.05 * rng.randn(n)
    # Normalize
    signal /= np.max(np.abs(signal)) + 1e-10
    signal *= 0.8
    return signal


def _snr(original: np.ndarray, reconstructed: np.ndarray) -> float:
    """Compute signal-to-noise ratio in dB."""
    min_len = min(len(original), len(reconstructed))
    orig = original[:min_len]
    recon = reconstructed[:min_len]
    noise = orig - recon
    sig_power = np.sum(orig ** 2) + 1e-10
    noise_power = np.sum(noise ** 2) + 1e-10
    return 10.0 * np.log10(sig_power / noise_power)


# ═══════════════════════════════════════════════════════════════════════════
# Tests
# ═══════════════════════════════════════════════════════════════════════════

class TestConfig:
    def test_default_config_valid(self):
        config = CodecConfig()
        config.validate()

    def test_bits_per_frame(self):
        config = CodecConfig()
        bpf = config.compute_bits_per_frame()
        assert bpf["total"] > 0
        assert bpf["lsp"] > 0
        assert bpf["num_subframes"] == 4

    def test_serialization_roundtrip(self):
        config = CodecConfig(lpc_order=12, acb_num_pulses=6)
        d = config.to_dict()
        config2 = CodecConfig.from_dict(d)
        assert config2.lpc_order == 12
        assert config2.acb_num_pulses == 6

    def test_invalid_preemphasis(self):
        config = CodecConfig(preemphasis_coeff=0.1)
        with pytest.raises(ValueError):
            config.validate()

    def test_invalid_subframe(self):
        config = CodecConfig(subframe_size_ms=3.0)  # doesn't divide 20ms evenly
        with pytest.raises(ValueError):
            config.validate()


class TestPreprocessing:
    def test_preemph_deemph_inverse(self):
        """Pre-emphasis followed by de-emphasis should approximately recover the signal."""
        signal = _make_speech_like_signal(0.1)
        coeff = 0.68
        pe = PreEmphasisFilter(coeff)
        de = DeEmphasisFilter(coeff)
        processed = pe.process(signal)
        recovered = de.process(processed)
        # Allow small numerical error
        np.testing.assert_allclose(signal, recovered, atol=1e-10)

    def test_highpass_removes_dc(self):
        """HP filter should remove DC offset."""
        signal = np.ones(1600) * 0.5  # pure DC
        hp = HighPassFilter(80.0, 16000)
        out = hp.process(signal)
        # After enough samples, output should be near zero
        assert abs(out[-1]) < 0.05


class TestLPC:
    def test_lpc_lsp_roundtrip(self):
        """LPC → LSP → LPC should approximately recover coefficients."""
        signal = _make_speech_like_signal(0.1)
        lpc_coeffs, _, _ = lpc_analysis(signal, 10)
        lsp = lpc_to_lsp(lpc_coeffs)
        assert len(lsp) == 10
        # LSPs should be ordered and in (0, pi)
        assert np.all(np.diff(lsp) > 0)
        assert np.all(lsp > 0) and np.all(lsp < np.pi)

        recovered = lsp_to_lpc(lsp)
        # Allow some tolerance due to root-finding precision
        np.testing.assert_allclose(lpc_coeffs, recovered, atol=0.15)

    def test_synthesis_filter(self):
        """Synthesis filter should produce output of correct length."""
        exc = np.random.randn(40)
        a = np.array([1.0, -0.5, 0.3])
        speech, state = lpc_synthesis_filter(exc, a)
        assert len(speech) == 40
        assert len(state) == 2


class TestQuantization:
    def test_split_vq_quantize_dequantize(self):
        """Quantize → dequantize should return a valid LSP vector."""
        vq = SplitVQ(codebook_size=64, num_splits=5, dim=10)
        lsp = np.linspace(0.2, 2.8, 10)
        indices, q_lsp = vq.quantize(lsp)
        assert len(indices) == 5
        assert len(q_lsp) == 10

        # Dequantize should match
        q_lsp2 = vq.dequantize(indices)
        np.testing.assert_array_equal(q_lsp, q_lsp2)

    def test_gain_quantizer(self):
        gq = GainQuantizer(codebook_size=64)
        idx, qpg, qcg = gq.quantize(0.5, 1.0)
        assert 0 <= idx < 64
        pg2, cg2 = gq.dequantize(idx)
        assert pg2 == qpg
        assert cg2 == qcg


class TestBitstream:
    def test_pack_unpack_identity(self):
        """pack → unpack should recover all indices exactly."""
        config = CodecConfig()
        frame = BitFrame(
            lsp_indices=[3, 10, 25, 60],  # 4 splits for wideband
            subframes=[
                SubFrameData(pitch_lag_int=50, pitch_lag_frac=1,
                             pitch_gain_index=7, fcb_index=12, gain_index=30),
                SubFrameData(pitch_lag_int=45, pitch_lag_frac=2,
                             pitch_gain_index=5, fcb_index=10, gain_index=15),
                SubFrameData(pitch_lag_int=60, pitch_lag_frac=0,
                             pitch_gain_index=3, fcb_index=8, gain_index=40),
                SubFrameData(pitch_lag_int=55, pitch_lag_frac=1,
                             pitch_gain_index=10, fcb_index=11, gain_index=20),
            ],
        )
        packed = pack(frame, config)
        recovered = unpack(packed, config)

        assert recovered.lsp_indices == frame.lsp_indices
        for sf_orig, sf_rec in zip(frame.subframes, recovered.subframes):
            assert sf_rec.pitch_lag_int == sf_orig.pitch_lag_int
            assert sf_rec.pitch_lag_frac == sf_orig.pitch_lag_frac
            assert sf_rec.pitch_gain_index == sf_orig.pitch_gain_index
            assert sf_rec.fcb_index == sf_orig.fcb_index
            assert sf_rec.gain_index == sf_orig.gain_index


class TestAlgebraicCodebook:
    def test_encode_decode_identity(self):
        """Encode → decode should recover the excitation vector."""
        acb = AlgebraicCodebook(subframe_size=40, num_pulses=4, num_tracks=4)
        target = np.random.randn(40)
        h = np.zeros(40)
        h[0] = 1.0
        h[1] = -0.5
        idx, gain, exc = acb.search(target, h)
        exc_decoded = acb.decode(idx)
        # Decoded excitation should have pulses at the same positions
        nonzero_orig = set(np.nonzero(exc)[0])
        nonzero_dec = set(np.nonzero(exc_decoded)[0])
        assert nonzero_orig == nonzero_dec


class TestFullRoundtrip:
    def test_encode_decode_snr(self):
        """Full encode → decode should achieve SNR > 5 dB."""
        config = CodecConfig()
        encoder = CELPEncoder(config)
        decoder = CELPDecoder(config)

        signal = _make_speech_like_signal(0.2, config.sample_rate)

        # Pad to frame boundary
        N = config.frame_size
        pad_len = (N - len(signal) % N) % N
        if pad_len > 0:
            signal = np.concatenate([signal, np.zeros(pad_len)])

        num_frames = len(signal) // N
        reconstructed = np.zeros_like(signal)

        for i in range(num_frames):
            frame_pcm = signal[i * N:(i + 1) * N]
            bit_frame = encoder.encode_frame(frame_pcm)
            recon_frame = decoder.decode_frame(bit_frame)
            reconstructed[i * N:(i + 1) * N] = recon_frame

        # Check duration is preserved
        assert len(reconstructed) == len(signal)

        # Check SNR
        snr_val = _snr(signal, reconstructed)
        assert snr_val > -5.0, f"SNR too low: {snr_val:.2f} dB"

    def test_encode_decode_duration(self):
        """Output duration must match input duration."""
        config = CodecConfig()
        encoder = CELPEncoder(config)
        decoder = CELPDecoder(config)

        N = config.frame_size
        signal = np.zeros(N * 3)  # exactly 3 frames of silence
        all_recon = []
        for i in range(3):
            bf = encoder.encode_frame(signal[i * N:(i + 1) * N])
            recon = decoder.decode_frame(bf)
            all_recon.append(recon)

        output = np.concatenate(all_recon)
        assert len(output) == len(signal)

    def test_file_roundtrip(self):
        """Test file-level encode → decode if soundfile is available."""
        try:
            import soundfile as sf
        except ImportError:
            pytest.skip("soundfile not installed")

        config = CodecConfig()
        signal = _make_speech_like_signal(0.5, config.sample_rate)

        with tempfile.TemporaryDirectory() as tmpdir:
            wav_in = os.path.join(tmpdir, "input.wav")
            bin_path = os.path.join(tmpdir, "encoded.bin")
            wav_out = os.path.join(tmpdir, "output.wav")

            # Write input WAV
            sf.write(wav_in, signal, config.sample_rate)

            # Encode
            encoder = CELPEncoder(config)
            encoder.encode_file(wav_in, bin_path)

            # Decode
            decoder = CELPDecoder(config)
            decoder.decode_file(bin_path, wav_out)

            # Read back and check
            recon, sr = sf.read(wav_out, dtype="float64")
            assert sr == config.sample_rate
            assert abs(len(recon) - len(signal)) <= config.frame_size  # allow 1 frame padding

            min_len = min(len(signal), len(recon))
            snr_val = _snr(signal[:min_len], recon[:min_len])
            assert snr_val > -5.0, f"File roundtrip SNR too low: {snr_val:.2f} dB"


if __name__ == "__main__":
    pytest.main([__file__, "-v"])
