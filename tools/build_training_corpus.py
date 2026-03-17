#!/usr/bin/env python3
"""Build neural excitation training corpus on DGX.

For each speech file, generate:
- Original waveform (target)
- LPC coefficients per frame (conditioning)
- Pitch lag + voicing per frame (conditioning)  
- Multiple degraded versions via different codecs/bitrates (for diversity)

Output: HDF5 dataset ready for training.
"""
import os, subprocess, struct, wave, sys
import numpy as np

CORPUS = os.path.expanduser("~/corpus/speech")
TLCS = os.path.expanduser("~/tlcs")
ENC = f"{TLCS}/build/tools/tlcs_enc"
DEC = f"{TLCS}/build/tools/tlcs_dec"
OUT_DIR = os.path.expanduser("~/training_data")
os.makedirs(OUT_DIR, exist_ok=True)

SAMPLE_RATE = 16000
FRAME_SIZE = 320  # 20ms
LPC_ORDER = 16

def read_wav(path):
    """Read 16-bit mono WAV to float32 array."""
    w = wave.open(path, 'r')
    n = w.getnframes()
    raw = w.readframes(n)
    w.close()
    samples = np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0
    return samples

def write_wav(path, samples, sr=16000):
    """Write float32 array to 16-bit WAV."""
    pcm = np.clip(samples * 32768.0, -32768, 32767).astype(np.int16)
    w = wave.open(path, 'w')
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(sr)
    w.writeframes(pcm.tobytes())
    w.close()

def lpc_analysis(frame, order=16):
    """Levinson-Durbin LPC analysis."""
    n = len(frame)
    # Autocorrelation
    r = np.correlate(frame, frame, mode='full')[n-1:n+order]
    if r[0] < 1e-10:
        return np.zeros(order + 1), 0.0, 0.0
    r[0] *= 1.0001  # regularization
    
    # Levinson-Durbin
    a = np.zeros(order + 1)
    a[0] = 1.0
    err = r[0]
    for i in range(1, order + 1):
        lam = 0.0
        for j in range(1, i):
            lam += a[j] * r[i - j]
        lam = -(r[i] + lam) / err
        
        a_new = a.copy()
        for j in range(1, i):
            a_new[j] = a[j] + lam * a[i - j]
        a_new[i] = lam
        a = a_new
        err *= (1.0 - lam * lam)
        if err < 1e-10:
            break
    
    # Prediction gain
    pred_gain = r[0] / max(err, 1e-10)
    return a, err, pred_gain

def pitch_detect(frame, min_lag=20, max_lag=300):
    """Simple autocorrelation pitch detection."""
    n = len(frame)
    energy = np.sum(frame * frame)
    if energy < 1e-6:
        return 0, 0.0
    
    best_lag = 0
    best_corr = 0.0
    for lag in range(min_lag, min(max_lag, n)):
        corr = np.sum(frame[lag:] * frame[:n-lag])
        lag_e = np.sum(frame[:n-lag] * frame[:n-lag])
        if lag_e > 1e-6:
            norm_corr = corr / np.sqrt(energy * lag_e)
            if norm_corr > best_corr:
                best_corr = norm_corr
                best_lag = lag
    
    return best_lag, best_corr

def extract_features(wav_path):
    """Extract per-frame LPC + pitch features from a WAV file."""
    samples = read_wav(wav_path)
    n_frames = len(samples) // FRAME_SIZE
    
    # Pre-emphasis
    preemph = np.zeros_like(samples)
    preemph[0] = samples[0]
    for i in range(1, len(samples)):
        preemph[i] = samples[i] - 0.60 * samples[i-1]
    
    lpc_coeffs = np.zeros((n_frames, LPC_ORDER + 1), dtype=np.float32)
    pitch_lags = np.zeros(n_frames, dtype=np.int32)
    voicing = np.zeros(n_frames, dtype=np.float32)
    lpc_residual = np.zeros_like(samples)
    
    for f in range(n_frames):
        start = f * FRAME_SIZE
        frame = preemph[start:start + FRAME_SIZE]
        
        # LPC
        a, err, pg = lpc_analysis(frame, LPC_ORDER)
        lpc_coeffs[f] = a.astype(np.float32)
        
        # LPC residual (excitation)
        for i in range(FRAME_SIZE):
            idx = start + i
            res = preemph[idx]
            for k in range(1, min(LPC_ORDER + 1, idx + 1)):
                res += a[k] * preemph[idx - k]
            lpc_residual[idx] = res
        
        # Pitch
        lag, vc = pitch_detect(frame)
        pitch_lags[f] = lag
        voicing[f] = vc
    
    return {
        'samples': samples[:n_frames * FRAME_SIZE],
        'preemph': preemph[:n_frames * FRAME_SIZE],
        'residual': lpc_residual[:n_frames * FRAME_SIZE],
        'lpc': lpc_coeffs,
        'pitch': pitch_lags,
        'voicing': voicing,
        'n_frames': n_frames,
    }

def encode_decode_tlcs(wav_path, bitrate, mode='tcx'):
    """Encode/decode through TLC codec, return decoded samples."""
    import tempfile
    with tempfile.NamedTemporaryFile(suffix='.tlcs') as tf, \
         tempfile.NamedTemporaryFile(suffix='.wav') as wf:
        env = os.environ.copy()
        if mode == 'celp':
            env['TLCS_MODE'] = 'celp'
        subprocess.run([ENC, wav_path, tf.name, str(bitrate)], 
                      env=env, capture_output=True)
        subprocess.run([DEC, tf.name, wf.name],
                      env=env, capture_output=True)
        if os.path.exists(wf.name) and os.path.getsize(wf.name) > 44:
            return read_wav(wf.name)
    return None

print("=== Building Neural Excitation Training Corpus ===")
print(f"Corpus: {CORPUS}")
print(f"Output: {OUT_DIR}")

wav_files = sorted([f for f in os.listdir(CORPUS) if f.endswith('.wav')])
print(f"Found {len(wav_files)} WAV files")

all_features = []
all_targets = []  # original excitation (what we want the neural net to produce)
all_degraded = []  # various degraded excitations (for augmentation)

for wi, wf in enumerate(wav_files):
    wav_path = os.path.join(CORPUS, wf)
    print(f"[{wi+1}/{len(wav_files)}] {wf}...")
    
    # Extract features from original
    feat = extract_features(wav_path)
    all_features.append(feat)
    
    # The target: original LPC residual (perfect excitation)
    all_targets.append(feat['residual'].copy())
    
    # Generate degraded versions through our codec at various rates
    degraded = {}
    for rate in [5000, 7000, 9600, 12000]:
        for mode in ['tcx', 'celp']:
            key = f'{mode}_{rate}'
            dec_samples = encode_decode_tlcs(wav_path, rate, mode)
            if dec_samples is not None:
                # Extract the degraded excitation
                dec_feat = extract_features(os.path.join(CORPUS, wf))
                degraded[key] = dec_samples[:len(feat['samples'])]
    all_degraded.append(degraded)

# Save as numpy arrays
print("\nSaving training data...")

# Concatenate all features
all_lpc = np.concatenate([f['lpc'] for f in all_features])
all_pitch = np.concatenate([f['pitch'] for f in all_features])
all_voicing = np.concatenate([f['voicing'] for f in all_features])
all_residual = np.concatenate([f['residual'] for f in all_features])
all_samples = np.concatenate([f['samples'] for f in all_features])

np.save(f"{OUT_DIR}/lpc_coeffs.npy", all_lpc)
np.save(f"{OUT_DIR}/pitch_lags.npy", all_pitch)
np.save(f"{OUT_DIR}/voicing.npy", all_voicing)
np.save(f"{OUT_DIR}/residual.npy", all_residual)
np.save(f"{OUT_DIR}/original.npy", all_samples)

total_frames = sum(f['n_frames'] for f in all_features)
total_seconds = total_frames * FRAME_SIZE / SAMPLE_RATE
print(f"Total: {total_frames} frames ({total_seconds:.1f}s) from {len(wav_files)} files")
print(f"Saved to {OUT_DIR}/")
print("Done!")
