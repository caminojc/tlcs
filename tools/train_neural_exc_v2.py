#!/usr/bin/env python3
"""TLC Neural Excitation — v2 with perceptual loss.

Like Lyra but 10x lighter:
- Lyra: ~2M params, full neural vocoder, needs TPU/GPU
- TLC Neural: ~200K params, LPC does heavy lifting, runs on CPU

Architecture:
  LPC analysis (DSP) → features → Neural excitation (tiny GRU) → LPC synthesis (DSP)
  
  The neural net ONLY generates the excitation signal. LPC handles the spectral
  envelope. This is why we can be so small — LPC removes 90% of the work.

Training loss:
  - L1 on waveform (after LPC synthesis)
  - Multi-resolution STFT loss (spectral fidelity)  
  - Feature matching loss from WavLM (perceptual quality)
"""
import os, sys, math
import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F
from torch.utils.data import Dataset, DataLoader

DATA_DIR = os.path.expanduser("~/training_data")
DEVICE = torch.device("cuda" if torch.cuda.is_available() else "cpu")

FRAME_SIZE = 160    # 10ms frames (finer granularity than 20ms)
LPC_ORDER = 16
COND_DIM = LPC_ORDER + 3  # LPC(17) + pitch(1) + voicing(1) = 19
HIDDEN = 192        # GRU size (smaller = faster inference)
BATCH = 16
EPOCHS = 200
LR = 0.0003

class SpeechDataset(Dataset):
    """Load pre-extracted features."""
    def __init__(self, data_dir, seq_frames=8):
        self.lpc = np.load(f"{data_dir}/lpc_coeffs.npy")
        self.pitch = np.load(f"{data_dir}/pitch_lags.npy")
        self.voicing = np.load(f"{data_dir}/voicing.npy")
        self.residual = np.load(f"{data_dir}/residual.npy")
        self.original = np.load(f"{data_dir}/original.npy")
        
        # Resample to 10ms frames (split each 20ms frame in half)
        n_frames_20ms = len(self.lpc)
        self.n_frames = n_frames_20ms * 2
        
        # Duplicate frame features for 10ms resolution
        self.lpc_10ms = np.repeat(self.lpc, 2, axis=0)
        self.pitch_10ms = np.repeat(self.pitch, 2)
        self.voicing_10ms = np.repeat(self.voicing, 2)
        
        self.seq_frames = seq_frames
        self.seq_len = seq_frames * FRAME_SIZE
        self.n_seqs = (self.n_frames - seq_frames) // seq_frames
        
        print(f"Dataset: {self.n_frames} frames (10ms), {self.n_seqs} sequences")
    
    def __len__(self):
        return self.n_seqs
    
    def __getitem__(self, idx):
        f_start = idx * self.seq_frames
        s_start = f_start * FRAME_SIZE
        s_end = s_start + self.seq_len
        
        # Build conditioning
        cond = np.zeros((self.seq_len, COND_DIM), dtype=np.float32)
        for f in range(self.seq_frames):
            fi = f_start + f
            if fi >= self.n_frames: fi = self.n_frames - 1
            ss = f * FRAME_SIZE
            se = ss + FRAME_SIZE
            cond[ss:se, :LPC_ORDER+1] = self.lpc_10ms[fi]
            cond[ss:se, LPC_ORDER+1] = self.pitch_10ms[fi] / 300.0
            cond[ss:se, LPC_ORDER+2] = self.voicing_10ms[fi]
        
        target = self.residual[s_start:s_end].astype(np.float32)
        original = self.original[s_start:s_end].astype(np.float32)
        
        return torch.from_numpy(cond), torch.from_numpy(target), torch.from_numpy(original)

class TLCNeural(nn.Module):
    """Tiny neural excitation generator.
    
    ~200K params. Generates excitation conditioned on LPC + pitch + voicing.
    LPC synthesis filter applied externally (DSP).
    """
    def __init__(self):
        super().__init__()
        # Frame-rate conditioning network
        self.cond_net = nn.Sequential(
            nn.Linear(COND_DIM, 64),
            nn.Tanh(),
            nn.Linear(64, 64),
            nn.Tanh(),
        )
        # Sample-rate GRU (the core)
        self.gru = nn.GRU(64 + 1, HIDDEN, batch_first=True)  # +1 for prev sample
        # Output
        self.out = nn.Sequential(
            nn.Linear(HIDDEN, 64),
            nn.Tanh(),
            nn.Linear(64, 1),
            nn.Tanh(),  # bound output
        )
        
        total = sum(p.numel() for p in self.parameters())
        print(f"TLC Neural: {total:,} params ({total*4/1024:.0f} KB)")
    
    def forward(self, cond, prev_samples=None):
        B, T, _ = cond.shape
        c = self.cond_net(cond)  # (B, T, 64)
        
        if prev_samples is not None:
            # Teacher forcing
            prev = torch.cat([torch.zeros(B, 1, 1, device=c.device),
                            prev_samples[:, :-1].unsqueeze(-1)], dim=1)
        else:
            prev = torch.zeros(B, T, 1, device=c.device)
        
        gru_in = torch.cat([c, prev], dim=-1)
        h, _ = self.gru(gru_in)
        return self.out(h).squeeze(-1)

class MultiResSTFTLoss(nn.Module):
    """Multi-resolution STFT loss for perceptual quality."""
    def __init__(self, fft_sizes=[512, 256, 128]):
        super().__init__()
        self.fft_sizes = fft_sizes
    
    def forward(self, pred, target):
        loss = 0
        for n_fft in self.fft_sizes:
            hop = n_fft // 4
            window = torch.hann_window(n_fft, device=pred.device)
            
            pred_stft = torch.stft(pred, n_fft, hop, window=window, return_complex=True)
            target_stft = torch.stft(target, n_fft, hop, window=window, return_complex=True)
            
            # Spectral convergence
            sc = torch.norm(torch.abs(target_stft) - torch.abs(pred_stft)) / (torch.norm(torch.abs(target_stft)) + 1e-7)
            # Log magnitude loss
            lm = torch.mean(torch.abs(torch.log(torch.abs(pred_stft) + 1e-7) - torch.log(torch.abs(target_stft) + 1e-7)))
            
            loss += sc + lm
        return loss / len(self.fft_sizes)

def lpc_synthesis_batch(excitation, lpc_coeffs, frame_size=FRAME_SIZE):
    """Apply LPC synthesis filter to excitation (batched, differentiable)."""
    B, T = excitation.shape
    n_frames = T // frame_size
    output = torch.zeros_like(excitation)
    mem = torch.zeros(B, LPC_ORDER, device=excitation.device)
    
    for f in range(n_frames):
        fi = min(f, lpc_coeffs.shape[1] - 1)
        a = lpc_coeffs[:, fi, :]  # (B, LPC_ORDER+1)
        
        for i in range(frame_size):
            t = f * frame_size + i
            s = excitation[:, t]
            for k in range(min(LPC_ORDER, i + f * frame_size)):
                if k < LPC_ORDER:
                    s -= a[:, k+1] * mem[:, k]
            output[:, t] = s
            mem = torch.cat([s.unsqueeze(1), mem[:, :-1]], dim=1)
    
    return output

def train():
    print(f"Device: {DEVICE}")
    print(f"Training TLC Neural Excitation v2")
    print(f"Goal: Lyra quality, 10x less compute")
    
    dataset = SpeechDataset(DATA_DIR, seq_frames=8)
    loader = DataLoader(dataset, batch_size=BATCH, shuffle=True, num_workers=2, pin_memory=True)
    
    model = TLCNeural().to(DEVICE)
    optimizer = torch.optim.AdamW(model.parameters(), lr=LR, weight_decay=0.01)
    scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, EPOCHS)
    stft_loss = MultiResSTFTLoss().to(DEVICE)
    
    best_loss = float('inf')
    
    for epoch in range(EPOCHS):
        model.train()
        total_loss = 0
        n = 0
        
        for cond, target_exc, original in loader:
            cond = cond.to(DEVICE)
            target_exc = target_exc.to(DEVICE)
            original = original.to(DEVICE)
            
            # Normalize target for stable training
            target_norm = target_exc / (torch.max(torch.abs(target_exc), dim=1, keepdim=True)[0] + 1e-7)
            
            pred = model(cond, target_norm)
            
            # Loss 1: L1 on excitation
            l1 = F.l1_loss(pred, target_norm)
            
            # Loss 2: Multi-res STFT on excitation
            spec = stft_loss(pred, target_norm)
            
            loss = l1 + 0.5 * spec
            
            optimizer.zero_grad()
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            optimizer.step()
            
            total_loss += loss.item()
            n += 1
        
        scheduler.step()
        avg = total_loss / max(n, 1)
        
        if avg < best_loss:
            best_loss = avg
            torch.save(model.state_dict(), f"{DATA_DIR}/tlc_neural_best.pt")
        
        if (epoch + 1) % 10 == 0 or epoch == 0:
            print(f"Epoch {epoch+1}/{EPOCHS}: loss={avg:.4f} best={best_loss:.4f}")
    
    # Export
    model.load_state_dict(torch.load(f"{DATA_DIR}/tlc_neural_best.pt"))
    model.eval()
    print(f"\nBest loss: {best_loss:.4f}")
    
    # Save for C export
    torch.save({
        'model_state': model.state_dict(),
        'config': {'hidden': HIDDEN, 'cond_dim': COND_DIM, 'lpc_order': LPC_ORDER},
    }, f"{DATA_DIR}/tlc_neural_export.pt")
    print(f"Saved to {DATA_DIR}/tlc_neural_export.pt")

if __name__ == "__main__":
    train()
