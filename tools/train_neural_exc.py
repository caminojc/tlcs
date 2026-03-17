#!/usr/bin/env python3
"""Train a tiny neural excitation generator (LPCNet-style).

Architecture:
  Input: LPC coefficients (17) + pitch lag (1) + voicing (1) + previous sample (1) = 20
  GRU-A: 384 units (main recurrence)  
  GRU-B: 16 units (fine detail)
  Output: mu-law sample (256 classes) or direct float

Total params: ~600K

The model generates excitation samples that, when filtered through LPC synthesis,
produce natural-sounding speech. Trained on original LPC residuals as targets.
"""
import os, sys
import numpy as np
import torch
import torch.nn as nn
import torch.optim as optim
from torch.utils.data import Dataset, DataLoader

DATA_DIR = os.path.expanduser("~/training_data")
DEVICE = torch.device("cuda" if torch.cuda.is_available() else "cpu")

# Hyperparams
FRAME_SIZE = 320
LPC_ORDER = 16
GRU_A_SIZE = 256   # smaller than LPCNet's 384 for speed
GRU_B_SIZE = 16
BATCH_SIZE = 32
EPOCHS = 100
LR = 0.001
SAMPLE_RATE = 16000

class ExcitationDataset(Dataset):
    """Dataset: per-frame conditioning → per-sample excitation targets."""
    def __init__(self, data_dir, seq_len=FRAME_SIZE * 4):
        self.lpc = np.load(f"{data_dir}/lpc_coeffs.npy")      # (N_frames, 17)
        self.pitch = np.load(f"{data_dir}/pitch_lags.npy")     # (N_frames,)
        self.voicing = np.load(f"{data_dir}/voicing.npy")      # (N_frames,)
        self.residual = np.load(f"{data_dir}/residual.npy")    # (N_samples,)
        self.original = np.load(f"{data_dir}/original.npy")    # (N_samples,)
        
        self.n_frames = len(self.lpc)
        self.seq_len = seq_len
        self.seq_frames = seq_len // FRAME_SIZE
        
        # How many sequences we can make
        self.n_seqs = (self.n_frames - self.seq_frames) // self.seq_frames
        print(f"Dataset: {self.n_frames} frames, {self.n_seqs} sequences of {self.seq_frames} frames")
    
    def __len__(self):
        return self.n_seqs
    
    def __getitem__(self, idx):
        f_start = idx * self.seq_frames
        f_end = f_start + self.seq_frames
        s_start = f_start * FRAME_SIZE
        s_end = f_end * FRAME_SIZE
        
        # Conditioning: repeat per-frame features to per-sample
        cond = np.zeros((self.seq_len, LPC_ORDER + 3), dtype=np.float32)
        for f in range(self.seq_frames):
            fi = f_start + f
            ss = f * FRAME_SIZE
            se = ss + FRAME_SIZE
            cond[ss:se, :LPC_ORDER+1] = self.lpc[fi]
            cond[ss:se, LPC_ORDER+1] = self.pitch[fi] / 300.0  # normalize
            cond[ss:se, LPC_ORDER+2] = self.voicing[fi]
        
        # Target: original residual
        target = self.residual[s_start:s_end].copy()
        
        return (
            torch.from_numpy(cond),
            torch.from_numpy(target),
        )

class NeuralExcitation(nn.Module):
    """Tiny neural excitation generator.
    
    Generates LPC excitation conditioned on:
    - LPC coefficients (spectral envelope)
    - Pitch lag (fundamental frequency)
    - Voicing strength
    
    Architecture inspired by LPCNet but smaller:
    - Conditioning FC: maps frame features to GRU input
    - GRU-A: main temporal modeling (256 units)
    - GRU-B: fine sample-level detail (16 units)
    - Output FC: produces excitation sample
    """
    def __init__(self, cond_dim=LPC_ORDER+3, gru_a=GRU_A_SIZE, gru_b=GRU_B_SIZE):
        super().__init__()
        self.cond_fc = nn.Linear(cond_dim, 64)
        self.gru_a = nn.GRU(64 + 1, gru_a, batch_first=True)  # +1 for prev sample
        self.gru_b = nn.GRU(gru_a, gru_b, batch_first=True)
        self.out_fc = nn.Sequential(
            nn.Linear(gru_b, 64),
            nn.Tanh(),
            nn.Linear(64, 1),
        )
        
        total = sum(p.numel() for p in self.parameters())
        print(f"Model params: {total:,} ({total*4/1024:.0f} KB float32)")
    
    def forward(self, cond, target=None):
        """
        cond: (B, T, cond_dim) per-sample conditioning
        target: (B, T) target excitation (for teacher forcing)
        Returns: (B, T) predicted excitation
        """
        B, T, _ = cond.shape
        
        # Condition projection
        c = torch.relu(self.cond_fc(cond))  # (B, T, 64)
        
        # Autoregressive generation with teacher forcing during training
        if target is not None:
            # Teacher forcing: use target as previous sample input
            prev = torch.zeros(B, 1, 1, device=cond.device)
            prev_samples = torch.cat([prev, target[:, :-1].unsqueeze(-1)], dim=1)  # (B, T, 1)
            
            gru_input = torch.cat([c, prev_samples], dim=-1)  # (B, T, 65)
            ha, _ = self.gru_a(gru_input)   # (B, T, gru_a)
            hb, _ = self.gru_b(ha)          # (B, T, gru_b)
            out = self.out_fc(hb).squeeze(-1)  # (B, T)
            return out
        else:
            # Autoregressive inference (slower)
            ha_state = None
            hb_state = None
            prev = torch.zeros(B, 1, device=cond.device)
            outputs = []
            
            for t in range(T):
                ct = c[:, t:t+1, :]  # (B, 1, 64)
                inp = torch.cat([ct, prev.unsqueeze(-1)], dim=-1)  # (B, 1, 65)
                ha_out, ha_state = self.gru_a(inp, ha_state)
                hb_out, hb_state = self.gru_b(ha_out, hb_state)
                sample = self.out_fc(hb_out).squeeze(-1).squeeze(-1)  # (B,)
                outputs.append(sample)
                prev = sample
            
            return torch.stack(outputs, dim=1)  # (B, T)

def train():
    print(f"Device: {DEVICE}")
    
    # Load data
    dataset = ExcitationDataset(DATA_DIR)
    loader = DataLoader(dataset, batch_size=BATCH_SIZE, shuffle=True, num_workers=2)
    
    # Model
    model = NeuralExcitation().to(DEVICE)
    optimizer = optim.Adam(model.parameters(), lr=LR)
    scheduler = optim.lr_scheduler.CosineAnnealingLR(optimizer, EPOCHS)
    
    # Training loop
    best_loss = float('inf')
    for epoch in range(EPOCHS):
        model.train()
        total_loss = 0
        n_batches = 0
        
        for cond, target in loader:
            cond = cond.to(DEVICE)
            target = target.to(DEVICE)
            
            pred = model(cond, target)
            
            # L1 + spectral loss
            l1_loss = torch.mean(torch.abs(pred - target))
            
            # Simple spectral loss (FFT magnitude)
            pred_fft = torch.fft.rfft(pred, dim=-1)
            target_fft = torch.fft.rfft(target, dim=-1)
            spec_loss = torch.mean(torch.abs(
                torch.log(torch.abs(pred_fft) + 1e-7) - 
                torch.log(torch.abs(target_fft) + 1e-7)
            ))
            
            loss = l1_loss + 0.5 * spec_loss
            
            optimizer.zero_grad()
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            optimizer.step()
            
            total_loss += loss.item()
            n_batches += 1
        
        scheduler.step()
        avg_loss = total_loss / max(n_batches, 1)
        
        if avg_loss < best_loss:
            best_loss = avg_loss
            torch.save(model.state_dict(), f"{DATA_DIR}/neural_exc_best.pt")
        
        if (epoch + 1) % 10 == 0 or epoch == 0:
            print(f"Epoch {epoch+1}/{EPOCHS}: loss={avg_loss:.4f} (best={best_loss:.4f}), lr={scheduler.get_last_lr()[0]:.6f}")
    
    print(f"\nTraining complete. Best loss: {best_loss:.4f}")
    print(f"Model saved to {DATA_DIR}/neural_exc_best.pt")
    
    # Export to ONNX for C inference
    model.load_state_dict(torch.load(f"{DATA_DIR}/neural_exc_best.pt"))
    model.eval()
    dummy_cond = torch.randn(1, FRAME_SIZE, LPC_ORDER + 3).to(DEVICE)
    dummy_target = torch.randn(1, FRAME_SIZE).to(DEVICE)
    torch.onnx.export(model, (dummy_cond, dummy_target),
                      f"{DATA_DIR}/neural_exc.onnx",
                      input_names=['cond', 'target'],
                      output_names=['excitation'],
                      dynamic_axes={'cond': {1: 'time'}, 'target': {1: 'time'}})
    print(f"ONNX model exported to {DATA_DIR}/neural_exc.onnx")

if __name__ == "__main__":
    train()
