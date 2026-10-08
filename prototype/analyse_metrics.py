"""Which scoring agrees with the user's blind listening? Each measure scores the candidates the user compared, and agrees
with a verdict when it ranks the preferred one higher.

    source .venv/bin/activate && python prototype/analyse_metrics.py

Verdicts (PREFS): blind set 1 (prototype/out/analyse/blind/key.txt) and the first renders (renders/<pair>/candidates.txt),
from the user's listening recorded in HANDOVER.md. 2026-10-08: the current measure (mean band r) agreed on 14 of 21, as
well as any variant; add the new stems' verdicts here before changing the measure."""
import re
import sys
from pathlib import Path
import numpy as np
ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyse as an, analyse_stems as st
fs = 48000

def parse(label):
    flip = "Ø" in label
    d = float(re.search(r"delay ([+-][0-9.]+) ms", label).group(1))
    if label.startswith("phase off"):
        return an.Candidate("none", False, 0.0, d, flip, 0)
    m = re.match(r"CONSTANT ([0-9.]+)°", label)
    if m:
        return an.Candidate("constant", True, float(m.group(1)), d, flip, 0)
    m = re.match(r"(LO|HI) (in|out) ([0-9.]+)°", label)
    return an.Candidate(m.group(1).lower(), m.group(2) == "in", float(m.group(3)), d, flip, 0)

key = {}
for line in (ROOT / "prototype/out/analyse/blind/key.txt").read_text().splitlines():
    m = re.match(r"(\w+) ([A-D]): [^:]+: (.*)", line)
    if m:
        key.setdefault(m.group(1), {})[m.group(2)] = parse(m.group(3))
for tag in ("kick", "snare"):
    for line in (ROOT / f"prototype/out/analyse/renders/{tag}/candidates.txt").read_text().splitlines():
        m = re.match(r"cand(\d): (.*) \(r", line)
        key.setdefault("set1_" + tag, {})[m.group(1)] = parse(m.group(2))

# (pair, better, worse): the user's verdicts
PREFS = [("snare_sample", "C", "A"), ("snare_sample", "C", "B"), ("snare_sample", "B", "A"),
         ("snare_oh", "C", "A"), ("snare_oh", "C", "B"),
         ("kick_sample", "C", "D"), ("kick_sample", "C", "A"), ("kick_sample", "C", "B"),
         ("gtr_2", "C", "A"), ("gtr_2", "A", "B"), ("gtr_2", "B", "D"), ("gtr_2", "C", "B"), ("gtr_2", "C", "D"), ("gtr_2", "A", "D"),
         ("bass_amp", "C", "A"), ("bass_amp", "A", "B"), ("bass_amp", "C", "B"),
         ("set1_kick", "1", "2"), ("set1_kick", "1", "3"),
         ("set1_snare", "3", "1"), ("set1_snare", "3", "2")]

def load(tag):
    if tag.startswith("set1_"):
        t = tag[5:]
        return (np.fromfile(ROOT/"captures"/f"{t}_a.f32", np.float32).astype(float), np.fromfile(ROOT/"captures"/f"{t}_b.f32", np.float32).astype(float))
    return (np.fromfile(ROOT/"captures/stems"/f"{tag}_a.f32", np.float32).astype(float), np.fromfile(ROOT/"captures/stems"/f"{tag}_b.f32", np.float32).astype(float))

def band_terms(sp, c):
    g = an.candidate_response(c, fs, sp.freqs)
    out = []
    for a, b in sp.edges:
        cross = np.real((g[a:b] * sp.sxy[a:b]).sum()); px = sp.sxx[a:b].sum(); py = sp.syy[a:b].sum()
        out.append((cross, px, py))
    return np.array(out), np.array(sp.centres)

def metrics(sp, c):
    t, f = band_terms(sp, c)
    cross, px, py = t[:, 0], t[:, 1], t[:, 2]
    r = cross / np.sqrt(px * py)
    gain_db = 10 * np.log10((px + py + 2 * cross) / (px + py))
    loud = (px + py) ** 0.3
    return {
        "current (mean band r)": r.mean(),
        "sum gain dB": gain_db.mean(),
        "sum gain dB, loudness-weighted": (gain_db * loud).sum() / loud.sum(),
        "band r, lows x2": np.average(r, weights=np.where(f < 500, 2.0, 1.0)),
        "broadband r (meter)": cross.sum() / np.sqrt(px.sum() * py.sum()),
    }

cache = {}
right = {}
detail = []
for pair, better, worse in PREFS:
    if pair not in cache:
        x, y = load(pair); n = min(len(x), len(y)); cache[pair] = an.spectra(x[:n], y[:n], fs)
    sp = cache[pair]
    mb, mw = metrics(sp, key[pair][better]), metrics(sp, key[pair][worse])
    for name in mb:
        ok = mb[name] > mw[name]
        right.setdefault(name, []).append(ok)
    detail.append((pair, better, worse, {k: mb[k] > mw[k] for k in mb}))
print(f"{len(PREFS)} preferences from the user's listening\n")
for name, oks in right.items():
    print(f"  {name:32s} agrees on {sum(oks):2d} of {len(oks)}")
print()
names = list(right)
print("per preference (1 = agrees):", " | ".join(n.split(' (')[0][:14] for n in names))
for pair, b, w, d in detail:
    print(f"  {pair:13s} {b}>{w}:  " + "  ".join(str(int(d[n])) for n in names))
