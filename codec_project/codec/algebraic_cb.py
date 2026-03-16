"""
Algebraic (fixed) codebook for the CELP codec — ISPP design.

Interleaved Single Pulse Permutation:
- Subframe is divided into *num_tracks* interleaved tracks.
- *num_pulses* are placed, one per track (cycling if pulses > tracks).
- Sequential search: place one pulse at a time, recompute gain after each.
- Pulse signs are pre-determined from the target signal polarity.
"""
from __future__ import annotations

import math
from typing import Tuple

import numpy as np


class AlgebraicCodebook:
    """ISPP algebraic codebook search and decode."""

    def __init__(
        self,
        subframe_size: int,
        num_pulses: int,
        num_tracks: int,
    ):
        self.subframe_size = subframe_size
        self.num_pulses = num_pulses
        self.num_tracks = num_tracks
        # Positions per track
        self.positions_per_track = subframe_size // num_tracks

    def search(
        self,
        target: np.ndarray,
        impulse_response: np.ndarray,
    ) -> Tuple[int, float, np.ndarray]:
        """
        Algebraic codebook search using sequential pulse placement.

        Parameters
        ----------
        target           : (subframe_size,) — target after adaptive CB removal
        impulse_response : (subframe_size,) — truncated impulse response of 1/A(z)

        Returns
        -------
        index            : packed integer encoding pulse positions + signs
        gain             : scalar codebook gain
        excitation       : (subframe_size,) — codebook excitation vector
        """
        N = self.subframe_size

        # Pre-compute the correlation matrix d(n) = <target, h_n>
        # and the auto-correlation matrix phi(i,j) = <h_i, h_j>
        # where h_n is h shifted by n (convolution column)
        d = np.zeros(N)
        for n in range(N):
            for i in range(n, N):
                d[n] += target[i] * impulse_response[i - n]

        # Determine sign from target correlation
        signs = np.sign(d)
        signs[signs == 0] = 1.0

        # Build excitation pulse by pulse (sequential greedy search)
        pulse_positions = []
        pulse_signs = []
        excitation = np.zeros(N)

        # Residual target starts as the original target
        residual_target = target.copy()

        for p in range(self.num_pulses):
            track = p % self.num_tracks

            best_pos = track  # first position in this track
            best_score = -1e30

            # Search all positions in this track
            for k in range(self.positions_per_track):
                pos = track + k * self.num_tracks
                if pos >= N:
                    break

                s = signs[pos]
                # Correlation of placing a pulse here
                corr = 0.0
                for i in range(pos, N):
                    corr += residual_target[i] * impulse_response[i - pos]
                corr *= s

                # Energy of this pulse's filtered contribution
                energy = 0.0
                for i in range(pos, N):
                    energy += impulse_response[i - pos] ** 2

                if energy < 1e-10:
                    continue

                score = corr * corr / energy
                if score > best_score:
                    best_score = score
                    best_pos = pos

            # Place the pulse
            s = signs[best_pos]
            pulse_positions.append(best_pos)
            pulse_signs.append(s)

            # Compute this pulse's optimal gain
            corr_val = 0.0
            energy_val = 0.0
            for i in range(best_pos, N):
                h_shifted = impulse_response[i - best_pos]
                corr_val += residual_target[i] * h_shifted
                energy_val += h_shifted ** 2
            pulse_gain = (s * corr_val) / (energy_val + 1e-10)

            # Update residual target (remove this pulse's contribution)
            for i in range(best_pos, N):
                residual_target[i] -= pulse_gain * s * impulse_response[i - best_pos]

            excitation[best_pos] += s

        # ── Second pass: refine each pulse with all others fixed ──
        for _refine in range(2):
            for p in range(self.num_pulses):
                track = p % self.num_tracks
                old_pos = pulse_positions[p]
                old_sign = pulse_signs[p]
                excitation[old_pos] -= old_sign

                # Filtered excitation without this pulse
                filtered_without = self._filter_excitation(excitation, impulse_response)
                fw_energy = np.dot(filtered_without, filtered_without)
                if fw_energy > 1e-10:
                    g_tmp = np.dot(target, filtered_without) / fw_energy
                    resid = target - g_tmp * filtered_without
                else:
                    resid = target.copy()

                best_pos = old_pos
                best_score = -1e30
                for k in range(self.positions_per_track):
                    pos = track + k * self.num_tracks
                    if pos >= N:
                        break
                    s = signs[pos]
                    corr = 0.0
                    for i in range(pos, N):
                        corr += resid[i] * impulse_response[i - pos]
                    corr *= s
                    energy = 0.0
                    for i in range(pos, N):
                        energy += impulse_response[i - pos] ** 2
                    if energy < 1e-10:
                        continue
                    score = corr * corr / energy
                    if score > best_score:
                        best_score = score
                        best_pos = pos

                pulse_positions[p] = best_pos
                pulse_signs[p] = signs[best_pos]
                excitation[best_pos] += signs[best_pos]

        # Compute overall gain: g = <target, H*c> / <H*c, H*c>
        filtered = self._filter_excitation(excitation, impulse_response)
        corr_total = np.dot(target, filtered)
        energy_total = np.dot(filtered, filtered) + 1e-10
        gain = corr_total / energy_total

        # Pack index
        index = self._encode_index(pulse_positions, pulse_signs)

        return index, float(gain), excitation

    def decode(self, index: int) -> np.ndarray:
        """Reconstruct excitation vector from packed index."""
        positions, signs = self._decode_index(index)
        excitation = np.zeros(self.subframe_size)
        for pos, s in zip(positions, signs):
            if 0 <= pos < self.subframe_size:
                excitation[pos] += s
        return excitation

    # ── Index encoding ────────────────────────────────────────────────────

    def _encode_index(
        self, positions: list, signs: list
    ) -> int:
        """
        Pack pulse positions and signs into a single integer.

        For each pulse: position-within-track needs ceil(log2(positions_per_track))
        bits, and sign needs 1 bit.
        """
        pos_bits = max(1, math.ceil(math.log2(max(self.positions_per_track, 2))))
        bits_per_pulse = pos_bits + 1
        index = 0
        for p in range(self.num_pulses):
            track = p % self.num_tracks
            pos = positions[p] if p < len(positions) else track
            s = signs[p] if p < len(signs) else 1.0

            pos_in_track = (pos - track) // self.num_tracks
            pos_in_track = max(0, min(pos_in_track, self.positions_per_track - 1))
            sign_bit = 0 if s >= 0 else 1

            pulse_idx = (pos_in_track << 1) | sign_bit
            index |= (pulse_idx << (p * bits_per_pulse))
        return index

    def _decode_index(
        self, index: int
    ) -> Tuple[list, list]:
        """Unpack index into positions and signs."""
        pos_bits = max(1, math.ceil(math.log2(max(self.positions_per_track, 2))))
        bits_per_pulse = pos_bits + 1
        mask = (1 << bits_per_pulse) - 1

        positions = []
        signs = []
        for p in range(self.num_pulses):
            track = p % self.num_tracks
            pulse_idx = (index >> (p * bits_per_pulse)) & mask

            sign_bit = pulse_idx & 1
            pos_in_track = pulse_idx >> 1
            pos_in_track = min(pos_in_track, self.positions_per_track - 1)

            pos = track + pos_in_track * self.num_tracks
            s = -1.0 if sign_bit else 1.0

            positions.append(pos)
            signs.append(s)

        return positions, signs

    def bits_per_subframe(self) -> int:
        """Number of bits needed to encode one subframe's FCB index."""
        pos_bits = max(1, math.ceil(math.log2(max(self.positions_per_track, 2))))
        return self.num_pulses * (pos_bits + 1)

    # ── Helpers ───────────────────────────────────────────────────────────

    @staticmethod
    def _filter_excitation(
        excitation: np.ndarray, impulse_response: np.ndarray
    ) -> np.ndarray:
        """Convolve excitation with impulse response (causal, truncated)."""
        N = len(excitation)
        result = np.zeros(N)
        for i in range(N):
            for k in range(i + 1):
                if k < N and (i - k) < len(impulse_response):
                    result[i] += excitation[k] * impulse_response[i - k]
        return result
