"""
Helper script to download public MOS-labeled datasets for evaluator training
and unlabeled speech corpora for codec optimization.

Datasets:
- NISQA corpus (MOS-labeled, ~2GB) — for evaluator training
- VoiceMOS Challenge 2022 (MOS-labeled) — requires manual registration
- LibriSpeech clean-100 (unlabeled, ~6GB) — for codec optimization corpus
"""

from __future__ import annotations

import hashlib
import logging
import os
import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path
from typing import Optional
from urllib.request import urlretrieve
from urllib.error import URLError

logger = logging.getLogger(__name__)


def _download_file(
    url: str,
    dest_path: str,
    description: str = "",
    expected_sha256: Optional[str] = None,
) -> bool:
    """Download a file with progress reporting."""
    dest = Path(dest_path)
    if dest.exists():
        logger.info("Already downloaded: %s", dest_path)
        if expected_sha256 and _sha256(dest_path) != expected_sha256:
            logger.warning("Checksum mismatch for %s — re-downloading", dest_path)
            dest.unlink()
        else:
            return True

    dest.parent.mkdir(parents=True, exist_ok=True)
    desc = description or url.split("/")[-1]
    logger.info("Downloading %s from %s ...", desc, url)

    try:
        # Try wget first (better progress and resume support)
        result = subprocess.run(
            ["wget", "-q", "--show-progress", "-O", dest_path, url],
            capture_output=True,
        )
        if result.returncode == 0:
            logger.info("Downloaded %s", desc)
            return True
    except FileNotFoundError:
        pass

    try:
        # Fallback to urllib
        def _progress(block_num, block_size, total_size):
            if total_size > 0:
                pct = min(100, block_num * block_size * 100 // total_size)
                print(f"\r  {desc}: {pct}%", end="", flush=True)

        urlretrieve(url, dest_path, reporthook=_progress)
        print()  # newline after progress
        logger.info("Downloaded %s", desc)
        return True
    except (URLError, OSError) as e:
        logger.error("Failed to download %s: %s", url, e)
        if dest.exists():
            dest.unlink()
        return False


def _sha256(path: str) -> str:
    """Compute SHA-256 hash of a file."""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(8192), b""):
            h.update(chunk)
    return h.hexdigest()


def _extract(archive_path: str, dest_dir: str) -> None:
    """Extract tar.gz, tar.bz2, or zip archive."""
    logger.info("Extracting %s to %s ...", archive_path, dest_dir)
    Path(dest_dir).mkdir(parents=True, exist_ok=True)

    if archive_path.endswith((".tar.gz", ".tgz")):
        with tarfile.open(archive_path, "r:gz") as tar:
            tar.extractall(dest_dir)
    elif archive_path.endswith(".tar.bz2"):
        with tarfile.open(archive_path, "r:bz2") as tar:
            tar.extractall(dest_dir)
    elif archive_path.endswith(".zip"):
        with zipfile.ZipFile(archive_path, "r") as zf:
            zf.extractall(dest_dir)
    else:
        raise ValueError(f"Unknown archive format: {archive_path}")


# ---------------------------------------------------------------------------
# NISQA corpus
# ---------------------------------------------------------------------------

NISQA_URLS = {
    "NISQA_TRAIN_SIM": "https://zenodo.org/records/6126981/files/NISQA_TRAIN_SIM.tar.gz",
    "NISQA_TRAIN_LIVE": "https://zenodo.org/records/6126981/files/NISQA_TRAIN_LIVE.tar.gz",
    "NISQA_VAL_SIM": "https://zenodo.org/records/6126981/files/NISQA_VAL_SIM.tar.gz",
    "NISQA_VAL_LIVE": "https://zenodo.org/records/6126981/files/NISQA_VAL_LIVE.tar.gz",
    "NISQA_TEST_P501": "https://zenodo.org/records/6126981/files/NISQA_TEST_P501.tar.gz",
    "NISQA_TEST_FOR": "https://zenodo.org/records/6126981/files/NISQA_TEST_FOR.tar.gz",
    "NISQA_TEST_LIVETALK": "https://zenodo.org/records/6126981/files/NISQA_TEST_LIVETALK.tar.gz",
}


def download_nisqa_corpus(
    dest_dir: str,
    subsets: Optional[list] = None,
) -> bool:
    """
    Download NISQA corpus from Zenodo.

    Parameters
    ----------
    dest_dir : str
        Destination directory.
    subsets : list of str or None
        Which subsets to download. None = all.
        Options: NISQA_TRAIN_SIM, NISQA_TRAIN_LIVE, NISQA_VAL_SIM, etc.

    Returns
    -------
    bool
        True if all downloads succeeded.
    """
    dest = Path(dest_dir)
    dest.mkdir(parents=True, exist_ok=True)

    urls = NISQA_URLS
    if subsets:
        urls = {k: v for k, v in urls.items() if k in subsets}

    success = True
    for name, url in urls.items():
        archive_name = url.split("/")[-1]
        archive_path = str(dest / archive_name)
        extracted_dir = dest / name

        if extracted_dir.exists() and any(extracted_dir.iterdir()):
            logger.info("Already extracted: %s", extracted_dir)
            continue

        if not _download_file(url, archive_path, description=name):
            success = False
            continue

        try:
            _extract(archive_path, str(dest))
            # Clean up archive
            os.remove(archive_path)
        except Exception as e:
            logger.error("Failed to extract %s: %s", archive_path, e)
            success = False

    return success


# ---------------------------------------------------------------------------
# VoiceMOS Challenge 2022
# ---------------------------------------------------------------------------

def download_voicemos_2022(dest_dir: str) -> None:
    """
    VoiceMOS Challenge 2022 data requires registration.

    This function prints instructions for manual download.
    """
    print(
        "\n"
        "=" * 70 + "\n"
        "VoiceMOS Challenge 2022 Data Download\n"
        "=" * 70 + "\n"
        "\n"
        "The VoiceMOS Challenge 2022 dataset requires manual registration.\n"
        "\n"
        "Steps:\n"
        "  1. Visit: https://voicemos-challenge-2022.github.io/\n"
        "  2. Register and agree to the data usage terms\n"
        "  3. Download the training data (main track)\n"
        f"  4. Extract to: {dest_dir}/voicemos2022/\n"
        "\n"
        "Expected structure:\n"
        f"  {dest_dir}/voicemos2022/DATA/wav/\n"
        f"  {dest_dir}/voicemos2022/DATA/sets/\n"
        "\n"
        "The CSV files contain MOS labels in the format:\n"
        "  system_id, utterance_id, mean_mos\n"
        "\n"
        "=" * 70
    )


# ---------------------------------------------------------------------------
# LibriSpeech clean-100
# ---------------------------------------------------------------------------

LIBRISPEECH_URL = (
    "https://www.openslr.org/resources/12/train-clean-100.tar.gz"
)


def download_librispeech_clean100(dest_dir: str) -> bool:
    """
    Download LibriSpeech train-clean-100 (~6GB).

    This is 100 hours of clean read English speech. No MOS labels — used
    only for codec optimization (encode → decode → evaluate cycle).
    """
    dest = Path(dest_dir)
    dest.mkdir(parents=True, exist_ok=True)

    extracted_marker = dest / "LibriSpeech" / "train-clean-100"
    if extracted_marker.exists() and any(extracted_marker.iterdir()):
        logger.info("LibriSpeech train-clean-100 already present at %s", dest)
        return True

    archive_path = str(dest / "train-clean-100.tar.gz")
    if not _download_file(
        LIBRISPEECH_URL, archive_path,
        description="LibriSpeech train-clean-100 (~6GB)",
    ):
        return False

    try:
        _extract(archive_path, str(dest))
        os.remove(archive_path)
        logger.info("LibriSpeech extracted to %s", dest)

        # Convert FLAC to WAV for simpler codec processing
        flac_dir = dest / "LibriSpeech" / "train-clean-100"
        _convert_flac_to_wav(flac_dir)

        return True
    except Exception as e:
        logger.error("Failed to extract LibriSpeech: %s", e)
        return False


def _convert_flac_to_wav(root_dir: Path) -> None:
    """Convert FLAC files to WAV (in-place, keeping originals)."""
    try:
        import soundfile as sf
    except ImportError:
        logger.warning("soundfile not installed — skipping FLAC→WAV conversion")
        return

    flac_files = list(root_dir.rglob("*.flac"))
    if not flac_files:
        return

    logger.info("Converting %d FLAC files to WAV ...", len(flac_files))
    for flac_path in flac_files:
        wav_path = flac_path.with_suffix(".wav")
        if wav_path.exists():
            continue
        try:
            data, sr = sf.read(str(flac_path))
            sf.write(str(wav_path), data, sr, subtype="PCM_16")
        except Exception as e:
            logger.warning("Failed to convert %s: %s", flac_path, e)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

if __name__ == "__main__":
    import argparse

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s: %(message)s",
    )

    parser = argparse.ArgumentParser(
        description="Download speech datasets for codec optimization."
    )
    parser.add_argument(
        "--dest_dir", default="data", help="Root data directory"
    )
    parser.add_argument(
        "--nisqa", action="store_true", help="Download NISQA corpus"
    )
    parser.add_argument(
        "--nisqa_subsets", nargs="*", default=None,
        help="NISQA subsets to download (default: all)"
    )
    parser.add_argument(
        "--voicemos", action="store_true",
        help="Print VoiceMOS 2022 download instructions"
    )
    parser.add_argument(
        "--librispeech", action="store_true",
        help="Download LibriSpeech train-clean-100"
    )
    parser.add_argument(
        "--all", action="store_true", help="Download everything"
    )

    args = parser.parse_args()
    dest = args.dest_dir

    if args.all or args.nisqa:
        download_nisqa_corpus(
            os.path.join(dest, "nisqa"),
            subsets=args.nisqa_subsets,
        )

    if args.all or args.voicemos:
        download_voicemos_2022(dest)

    if args.all or args.librispeech:
        download_librispeech_clean100(os.path.join(dest, "librispeech"))

    if not any([args.all, args.nisqa, args.voicemos, args.librispeech]):
        parser.print_help()
