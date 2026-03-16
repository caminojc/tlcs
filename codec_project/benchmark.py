import torch, soundfile as sf, pathlib, sys, tempfile, argparse
import torchaudio.functional as AF
from torchmetrics.audio.nisqa import NonIntrusiveSpeechQualityAssessment
from codec.config import CodecConfig
from codec.encoder import CELPEncoder
from codec.decoder import CELPDecoder
import json, numpy as np

parser = argparse.ArgumentParser()
parser.add_argument('--config', default='configs/default_8kbps.json', help='CodecConfig JSON')
args = parser.parse_args()

nisqa = NonIntrusiveSpeechQualityAssessment(16000)

def mos(wav_path):
    audio, sr = sf.read(str(wav_path), dtype='float32')
    if audio.ndim > 1: audio = audio[:,0]
    t = torch.tensor(audio)
    if sr != 16000: t = AF.resample(t, sr, 16000)
    return nisqa(t)[0].item()

def codec_roundtrip(wav_path, enc, dec):
    audio, sr = sf.read(str(wav_path), dtype='float64')
    if audio.ndim > 1: audio = audio[:,0]
    with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as f:
        bs_path = f.name
    with tempfile.NamedTemporaryFile(suffix='.wav', delete=False) as f:
        out_path = f.name
    enc.reset()
    dec.reset()
    enc.encode_file(str(wav_path), bs_path)
    dec.decode_file(bs_path, out_path)
    return out_path

with open(args.config) as f:
    cfg = CodecConfig.from_dict(json.load(f))

print(f"\nConfig: {args.config}")
print(f"Config summary: {cfg.summary()}")

enc = CELPEncoder(cfg, codebook_dir='codec/codebooks')
dec = CELPDecoder(cfg, codebook_dir='codec/codebooks')

corpus = pathlib.Path('/Users/jonathanchristensen/CLionProjects/TLC/benchmark/corpus/speech/')
wavs = sorted(corpus.rglob('*.wav'))[:20]

print(f"\n{'File':<30} {'Clean MOS':>10} {'Codec MOS':>10} {'Delta':>8}")
print('-' * 62)
codec_scores = []
for w in wavs:
    clean = mos(w)
    out = codec_roundtrip(w, enc, dec)
    coded = mos(out)
    codec_scores.append(coded)
    print(f"{w.name:<30} {clean:>10.3f} {coded:>10.3f} {coded-clean:>+8.3f}")

print('-' * 62)
print(f"{'MEAN':<30} {'':>10} {np.mean(codec_scores):>10.3f}")
print(f"\nBaseline codec MOS: {np.mean(codec_scores):.3f}")
print("EVS target MOS at 8kbps: ~3.9")
