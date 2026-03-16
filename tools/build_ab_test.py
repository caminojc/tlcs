#!/usr/bin/env python3
"""
Build A/B preference test: encode one reference file with 50 configs,
generate an HTML page with pairwise comparisons.
"""
import json
import os
import subprocess
import sys
import shutil

PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TUNE_H = os.path.join(PROJECT, "src", "codec", "tlcs_tune.h")
BUILD = os.path.join(PROJECT, "build")
ENC = os.path.join(BUILD, "tools", "tlcs_enc")
DEC = os.path.join(BUILD, "tools", "tlcs_dec")
AB_DIR = os.path.join(PROJECT, "eval", "ab_test")
AUDIO_DIR = os.path.join(AB_DIR, "audio")

# Use a clear voiced file for comparison
REF_WAV = "/Users/jonathanchristensen/CLionProjects/TLC/benchmark/corpus/speech/908-157963-0014.wav"
REF_WAV2 = "/Users/jonathanchristensen/CLionProjects/TLC/benchmark/corpus/speech/237-134500-0022.wav"
BITRATE = 9600

def write_tune(params):
    lines = ["#ifndef TLCS_TUNE_H", "#define TLCS_TUNE_H", ""]
    for k, v in params.items():
        lines.append(f"#define {k:<25s} {v:.6f}f")
    lines += ["", "#endif", ""]
    with open(TUNE_H, "w") as f:
        f.write("\n".join(lines))

def build():
    r = subprocess.run(
        ["cmake", "--build", BUILD],
        capture_output=True, timeout=30,
        env={**os.environ, "PATH": "/Applications/CLion.app/Contents/bin/cmake/mac/aarch64/bin:" + os.environ["PATH"]}
    )
    return r.returncode == 0

def encode_decode(wav_in, wav_out, bitrate):
    tmp = wav_out + ".tlcs"
    r1 = subprocess.run([ENC, wav_in, tmp, str(bitrate)], capture_output=True, timeout=10)
    if r1.returncode != 0:
        return False
    r2 = subprocess.run([DEC, tmp, wav_out], capture_output=True, timeout=10)
    os.remove(tmp)
    return r2.returncode == 0

def main():
    configs_path = os.path.join(PROJECT, "tools", "ab_configs.json")
    if not os.path.exists(configs_path):
        print(f"ERROR: {configs_path} not found. Run config generator first.")
        sys.exit(1)

    with open(configs_path) as f:
        configs = json.load(f)

    os.makedirs(AUDIO_DIR, exist_ok=True)

    # Copy reference
    shutil.copy2(REF_WAV, os.path.join(AUDIO_DIR, "original_m1.wav"))
    shutil.copy2(REF_WAV2, os.path.join(AUDIO_DIR, "original_f1.wav"))

    # Encode each config
    print(f"Encoding {len(configs)} variants...")
    for cfg in configs:
        cid = cfg["id"]
        params = {k: v for k, v in cfg.items() if k != "id"}
        write_tune(params)
        if not build():
            print(f"  Config {cid}: BUILD FAILED")
            continue
        # Encode both reference files
        ok1 = encode_decode(REF_WAV, os.path.join(AUDIO_DIR, f"v{cid}_m1.wav"), BITRATE)
        ok2 = encode_decode(REF_WAV2, os.path.join(AUDIO_DIR, f"v{cid}_f1.wav"), BITRATE)
        status = "OK" if (ok1 and ok2) else "PARTIAL"
        print(f"  Config {cid}: {status}")

    # Generate HTML
    html = """<!DOCTYPE html>
<html lang="en"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>TLCS A/B Preference Test</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,system-ui,sans-serif;background:#0a0a0a;color:#e0e0e0;padding:1.5rem;max-width:900px;margin:0 auto}
h1{font-size:1.8rem;margin-bottom:0.3rem;color:#fff}
.sub{color:#888;margin-bottom:1.5rem;font-size:0.9rem}
.progress{background:#222;border-radius:8px;padding:0.8rem 1rem;margin-bottom:1.5rem;font-size:0.9rem}
.progress .bar{height:6px;background:#333;border-radius:3px;margin-top:0.5rem}
.progress .fill{height:6px;background:#60a5fa;border-radius:3px;transition:width 0.3s}
.pair{background:#111;border-radius:10px;padding:1.2rem;margin-bottom:1rem;display:none}
.pair.active{display:block}
.pair-header{font-size:0.85rem;color:#888;margin-bottom:0.8rem}
.ref-row{margin-bottom:1rem;padding-bottom:0.8rem;border-bottom:1px solid #222}
.ref-row .label{font-size:0.8rem;color:#60a5fa;margin-bottom:0.3rem}
.choices{display:grid;grid-template-columns:1fr 1fr;gap:1rem}
.choice{background:#1a1a1a;border:2px solid #333;border-radius:8px;padding:1rem;cursor:pointer;transition:all 0.2s}
.choice:hover{border-color:#555}
.choice.selected-a{border-color:#4ade80;background:#1a2a1a}
.choice.selected-b{border-color:#f97316;background:#2a1a1a}
.choice .label{font-weight:600;font-size:1.1rem;margin-bottom:0.5rem;text-align:center}
.choice audio{width:100%}
.btn-row{display:flex;gap:1rem;margin-top:1rem;justify-content:center}
.btn{padding:0.6rem 2rem;border-radius:6px;border:none;font-size:1rem;cursor:pointer;font-weight:600}
.btn-next{background:#60a5fa;color:#000}
.btn-next:disabled{background:#333;color:#666;cursor:not-allowed}
.btn-skip{background:#333;color:#aaa}
.btn-done{background:#4ade80;color:#000;font-size:1.2rem;padding:0.8rem 3rem}
.results{display:none;background:#111;border-radius:10px;padding:1.5rem;margin-top:1rem}
.results h2{margin-bottom:1rem}
#dl-btn{background:#60a5fa;color:#000;padding:0.6rem 2rem;border:none;border-radius:6px;cursor:pointer;font-weight:600;margin-top:1rem}
</style>
</head><body>
<h1>TLCS A/B Preference Test</h1>
<p class="sub">Listen to the original, then pick which coded version (A or B) sounds more natural. Use headphones if possible.</p>

<div class="progress">
  <span id="prog-text">Pair 1 / --</span>
  <div class="bar"><div class="fill" id="prog-fill" style="width:0%"></div></div>
</div>

<div id="pairs-container"></div>

<div class="results" id="results">
  <h2>Done! Thank you.</h2>
  <p id="results-text"></p>
  <button id="dl-btn" onclick="downloadResults()">Download Results JSON</button>
</div>

<script>
"""
    # Generate pairs: each pair compares two random configs on the same reference file
    import random
    random.seed(42)
    pairs = []
    config_ids = [c["id"] for c in configs]

    # Round 1: compare each config against config 0 (near-best) on M1 voice
    for cid in config_ids[1:25]:
        pairs.append({"a": 0, "b": cid, "ref": "m1"})

    # Round 2: top configs against each other on F1 voice
    for i in range(0, min(24, len(config_ids)-1), 2):
        pairs.append({"a": config_ids[i], "b": config_ids[i+1], "ref": "f1"})

    # Shuffle
    random.shuffle(pairs)
    # Randomize A/B order per pair
    for p in pairs:
        if random.random() > 0.5:
            p["a"], p["b"] = p["b"], p["a"]

    html += f"const PAIRS = {json.dumps(pairs)};\n"
    html += f"const CONFIGS = {json.dumps(configs)};\n"
    html += """
let currentPair = 0;
let results = [];

function renderPair(idx) {
  const container = document.getElementById('pairs-container');
  container.innerHTML = '';
  if (idx >= PAIRS.length) { showResults(); return; }

  const p = PAIRS[idx];
  const ref = p.ref;
  document.getElementById('prog-text').textContent = `Pair ${idx+1} / ${PAIRS.length}`;
  document.getElementById('prog-fill').style.width = `${(idx/PAIRS.length)*100}%`;

  const div = document.createElement('div');
  div.className = 'pair active';
  div.innerHTML = `
    <div class="pair-header">Comparison ${idx+1} of ${PAIRS.length}</div>
    <div class="ref-row">
      <div class="label">Original (reference)</div>
      <audio controls preload="auto"><source src="audio/original_${ref}.wav" type="audio/wav"></audio>
    </div>
    <div class="choices">
      <div class="choice" id="choice-a" onclick="selectChoice('a')">
        <div class="label">A</div>
        <audio controls preload="auto"><source src="audio/v${p.a}_${ref}.wav" type="audio/wav"></audio>
      </div>
      <div class="choice" id="choice-b" onclick="selectChoice('b')">
        <div class="label">B</div>
        <audio controls preload="auto"><source src="audio/v${p.b}_${ref}.wav" type="audio/wav"></audio>
      </div>
    </div>
    <div class="btn-row">
      <button class="btn btn-skip" onclick="skipPair()">Skip</button>
      <button class="btn btn-next" id="btn-next" onclick="nextPair()" disabled>Next &rarr;</button>
    </div>
  `;
  container.appendChild(div);
}

let currentChoice = null;
function selectChoice(c) {
  currentChoice = c;
  document.getElementById('choice-a').className = 'choice' + (c === 'a' ? ' selected-a' : '');
  document.getElementById('choice-b').className = 'choice' + (c === 'b' ? ' selected-b' : '');
  document.getElementById('btn-next').disabled = false;
}

function nextPair() {
  if (!currentChoice) return;
  const p = PAIRS[currentPair];
  const winner = currentChoice === 'a' ? p.a : p.b;
  const loser = currentChoice === 'a' ? p.b : p.a;
  results.push({pair: currentPair, winner: winner, loser: loser, ref: p.ref, choice: currentChoice});
  currentChoice = null;
  currentPair++;
  renderPair(currentPair);
}

function skipPair() {
  currentChoice = null;
  currentPair++;
  renderPair(currentPair);
}

function showResults() {
  document.getElementById('pairs-container').style.display = 'none';
  document.getElementById('prog-fill').style.width = '100%';
  document.getElementById('prog-text').textContent = `Done! ${results.length} comparisons`;

  // Count wins per config
  const wins = {};
  const losses = {};
  results.forEach(r => {
    wins[r.winner] = (wins[r.winner] || 0) + 1;
    losses[r.loser] = (losses[r.loser] || 0) + 1;
  });

  let text = '<table style="width:100%;font-size:0.9rem;margin-top:1rem">';
  text += '<tr><th>Config ID</th><th>Wins</th><th>Losses</th><th>Win Rate</th></tr>';
  const allIds = [...new Set([...Object.keys(wins), ...Object.keys(losses)])].sort((a,b) => (wins[b]||0) - (wins[a]||0));
  allIds.forEach(id => {
    const w = wins[id] || 0;
    const l = losses[id] || 0;
    const rate = w + l > 0 ? (w / (w + l) * 100).toFixed(0) : '-';
    text += `<tr><td>${id}</td><td>${w}</td><td>${l}</td><td>${rate}%</td></tr>`;
  });
  text += '</table>';

  const res = document.getElementById('results');
  res.style.display = 'block';
  document.getElementById('results-text').innerHTML = text;
}

function downloadResults() {
  const data = {
    comparisons: results,
    configs: CONFIGS,
    timestamp: new Date().toISOString()
  };
  const blob = new Blob([JSON.stringify(data, null, 2)], {type: 'application/json'});
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = 'ab_preferences.json';
  a.click();
}

renderPair(0);
</script>
</body></html>"""

    html_path = os.path.join(AB_DIR, "index.html")
    with open(html_path, "w") as f:
        f.write(html)
    print(f"\nA/B test page: file://{html_path}")
    print(f"  {len(pairs)} comparisons, {len(configs)} configs")
    print(f"  Audio in: {AUDIO_DIR}")

if __name__ == "__main__":
    main()
