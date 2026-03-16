"""
Speech corpus management for codec evaluation and evaluator training.

Supports: NISQA corpus format, LibriSpeech, VCTK, or any flat directory of wavs.
"""

from __future__ import annotations

import csv
import hashlib
import os
import random
from collections import Counter
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import numpy as np
import soundfile as sf

from dataclasses import dataclass


@dataclass
class SpeechSample:
    """A single speech sample with optional MOS label."""
    path: str
    mos_score: Optional[float] = None
    condition: Optional[str] = None
    db: Optional[str] = None

    @property
    def has_mos(self) -> bool:
        return self.mos_score is not None


class SpeechCorpus:
    """
    Manages a corpus of speech files for codec evaluation and evaluator training.

    Supports: NISQA corpus format, LibriSpeech, VCTK, or any flat directory of wavs.
    """

    def __init__(self, root_dir: str, format: str = "auto"):
        """
        Parameters
        ----------
        root_dir : str
            Root directory of the corpus.
        format : str
            One of "auto", "nisqa", "flat". If "auto", detects based on
            presence of CSV files in root_dir.
        """
        self.root_dir = Path(root_dir)
        if not self.root_dir.exists():
            raise FileNotFoundError(f"Corpus directory not found: {root_dir}")

        self.samples: List[SpeechSample] = []

        if format == "auto":
            format = self._detect_format()

        if format == "nisqa":
            self.samples = self._load_nisqa()
        else:
            self.samples = self._load_flat()

    def _detect_format(self) -> str:
        """Auto-detect corpus format from directory contents."""
        csv_files = list(self.root_dir.glob("*.csv"))
        for csv_file in csv_files:
            try:
                with open(csv_file, "r") as f:
                    header = f.readline().lower()
                    if "mos" in header and ("filepath" in header or "deg" in header):
                        return "nisqa"
            except Exception:
                continue
        return "flat"

    def _load_nisqa(self) -> List[SpeechSample]:
        """
        Load NISQA corpus format.

        Expected CSV columns: filepath_deg (or deg), mos, db, con
        The CSV may use various column names; we search for likely matches.
        """
        samples = []
        csv_files = list(self.root_dir.glob("*.csv"))
        if not csv_files:
            raise FileNotFoundError(f"No CSV files found in {self.root_dir}")

        for csv_path in csv_files:
            with open(csv_path, "r", newline="") as f:
                reader = csv.DictReader(f)
                field_names = [fn.strip().lower() for fn in reader.fieldnames or []]

                # Find the audio path column
                path_col = None
                for candidate in ["filepath_deg", "deg", "filepath", "path", "file"]:
                    for fn in reader.fieldnames or []:
                        if fn.strip().lower() == candidate:
                            path_col = fn
                            break
                    if path_col:
                        break

                # Find MOS column
                mos_col = None
                for candidate in ["mos", "mos_score", "mean_mos"]:
                    for fn in reader.fieldnames or []:
                        if fn.strip().lower() == candidate:
                            mos_col = fn
                            break
                    if mos_col:
                        break

                if path_col is None or mos_col is None:
                    continue  # Skip CSV files that don't match

                # Find optional columns
                db_col = self._find_column(reader.fieldnames, ["db", "database"])
                con_col = self._find_column(reader.fieldnames, ["con", "condition"])

                # Re-read from start (reader already consumed header)
                f.seek(0)
                reader = csv.DictReader(f)

                for row in reader:
                    audio_path = row[path_col].strip()
                    # Resolve relative paths against corpus root
                    if not os.path.isabs(audio_path):
                        audio_path = str(self.root_dir / audio_path)

                    if not os.path.isfile(audio_path):
                        continue

                    try:
                        mos = float(row[mos_col].strip())
                    except (ValueError, KeyError):
                        continue

                    db = row.get(db_col, "").strip() if db_col else None
                    condition = row.get(con_col, "").strip() if con_col else None

                    samples.append(SpeechSample(
                        path=audio_path,
                        mos_score=mos,
                        condition=condition or None,
                        db=db or None,
                    ))

        return samples

    @staticmethod
    def _find_column(
        fieldnames: Optional[List[str]], candidates: List[str]
    ) -> Optional[str]:
        """Find first matching column name from candidates."""
        if not fieldnames:
            return None
        for candidate in candidates:
            for fn in fieldnames:
                if fn.strip().lower() == candidate:
                    return fn
        return None

    def _load_flat(self, pattern: str = "**/*.wav") -> List[SpeechSample]:
        """Load from a flat directory of wav files (no MOS labels)."""
        samples = []
        for wav_path in sorted(self.root_dir.glob(pattern)):
            if wav_path.is_file():
                samples.append(SpeechSample(path=str(wav_path)))
        return samples

    def train_val_split(
        self,
        val_ratio: float = 0.1,
        seed: int = 42,
    ) -> Tuple[List[SpeechSample], List[SpeechSample]]:
        """
        Deterministic train/val split.

        Splits are shuffled but deterministic given the same seed.
        """
        indices = list(range(len(self.samples)))
        rng = random.Random(seed)
        rng.shuffle(indices)

        val_size = max(1, int(len(indices) * val_ratio))
        val_indices = set(indices[:val_size])

        train = [s for i, s in enumerate(self.samples) if i not in val_indices]
        val = [s for i, s in enumerate(self.samples) if i in val_indices]
        return train, val

    def get_paths(self) -> List[str]:
        """Return all audio file paths in the corpus."""
        return [s.path for s in self.samples]

    def get_labeled_samples(self) -> List[SpeechSample]:
        """Return only samples that have MOS labels."""
        return [s for s in self.samples if s.has_mos]

    def stats(self) -> Dict:
        """
        Corpus statistics.

        Returns dict with: num_files, total_duration_hours, sample_rates,
        mos_distribution (if labels present).
        """
        num_files = len(self.samples)
        total_duration_s = 0.0
        sample_rates: Counter = Counter()
        mos_values: List[float] = []
        errors = 0

        for sample in self.samples:
            try:
                info = sf.info(sample.path)
                total_duration_s += info.duration
                sample_rates[info.samplerate] += 1
            except Exception:
                errors += 1

            if sample.mos_score is not None:
                mos_values.append(sample.mos_score)

        result: Dict = {
            "num_files": num_files,
            "total_duration_hours": total_duration_s / 3600.0,
            "sample_rates": dict(sample_rates),
            "read_errors": errors,
        }

        if mos_values:
            arr = np.array(mos_values)
            result["mos_distribution"] = {
                "count": len(arr),
                "mean": float(arr.mean()),
                "std": float(arr.std()),
                "min": float(arr.min()),
                "max": float(arr.max()),
                "median": float(np.median(arr)),
            }

        return result

    def subset(
        self,
        n: int,
        seed: int = 42,
        require_mos: bool = False,
    ) -> List[SpeechSample]:
        """Return a random subset of n samples."""
        pool = self.get_labeled_samples() if require_mos else self.samples
        rng = random.Random(seed)
        n = min(n, len(pool))
        return rng.sample(pool, n)

    def __len__(self) -> int:
        return len(self.samples)

    def __repr__(self) -> str:
        labeled = sum(1 for s in self.samples if s.has_mos)
        return (
            f"SpeechCorpus(root='{self.root_dir}', "
            f"files={len(self.samples)}, labeled={labeled})"
        )
