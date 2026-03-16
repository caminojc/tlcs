"""
Data pipeline, corpus tooling, and end-to-end orchestration for the
CELP codec optimization system.

This module imports from both codec and evaluator — it is the
integration layer that ties them together.
"""

from pipeline.corpus import SpeechCorpus, SpeechSample
from pipeline.preprocess import normalize_corpus

__all__ = ["SpeechCorpus", "SpeechSample", "normalize_corpus"]
