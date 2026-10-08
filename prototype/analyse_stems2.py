"""ANALYSE on the third set of stems (captures/stems2/, gitignored): a full kit, bass DI and amp, two guitars.

    source .venv/bin/activate && python prototype/analyse_stems2.py [--jobs 6]

Input = the track the plugin sits on, sidechain = the reference (kick: Drum Kick, snare: Drum Snare Top, bass and guitar
against the bass). Dual-mono files are taken as mono; true stereo files are analysed as L, R and their sum, since one setting
has to serve both sides. Each pair is run on the whole capture, its first 15 s (bass and guitar: the constant part) and its
last 15 s (the stabs). The whole-capture run also reads the score against the delay out to +-WIDE_MS, for the best phase at
each lag, over all bands and over the bands under 300 Hz only, to see where a manual shift would pay (rooms: the user,
2026-10-08, wants the smallest shift that gets most of the gain, not the full alignment). Results: prototype/out/analyse/stems2/.
"""
import argparse
import json
import sys
from multiprocessing import Pool
from pathlib import Path

import numpy as np
import soundfile as sf

sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyse as an  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "captures" / "stems2"
OUT = an.OUT / "stems2"
FS = 48000
WIDE_MS = 40.0
KICK, SNARE = "Drum Kick", "Drum Snare Top"
DRUM_INPUTS = ["Drum {}".format(n) for n in ("OHs", "Room Close", "Room Far", "Room Mono", "Bleed")]
PAIRS = ([("kick", KICK, "Drum Kick Sample")] + [("kick", KICK, n) for n in DRUM_INPUTS]
         + [("snare", SNARE, n) for n in ("Drum Snare Sample", "Drum Snare Bottom")] + [("snare", SNARE, n) for n in DRUM_INPUTS]
         + [("bass", "Bass DI", "Bass Amp"), ("gtr", "GTR 1", "GTR 2")]
         + [("bass-gtr", b, g) for b in ("Bass DI", "Bass Amp") for g in ("GTR 1", "GTR 2")])
SLICES = {"whole": (0.0, 99.0), "first15": (0.0, 15.0), "last15": (17.0, 32.0)}


def load(name):
    """{'': mono} for dual-mono files, else {'L', 'R', 'sum'}."""
    x, fs = sf.read(SRC / f"{name}.wav", dtype="float64")
    assert fs == FS, (name, fs)
    if x.ndim == 1:
        return {"": x}
    if np.corrcoef(x[:, 0], x[:, 1])[0, 1] > 0.9995:
        return {"": x.mean(1)}
    return {" L": x[:, 0], " R": x[:, 1], " sum": x.mean(1)}


def cut(x, span):
    return x[int(span[0] * FS):int(span[1] * FS)]


def curve(x, y, reach_ms=WIDE_MS, fs=FS):
    """Best score at every lag over every setting and polarity (all bands, and the bands under LF_HZ), with the setting."""
    sp = an.spectra(x, y, fs)
    freqs = sp.freqs
    lf = np.array([f < an.LF_HZ for f in sp.centres])
    best = {"all": None, "lf": None}
    nlag = None
    for mode, wide, theta in an.settings(True):
        h = an.stage_response(mode, wide, theta, fs, freqs) if mode != "none" else np.ones(len(freqs))
        r, lags = an.band_scores(sp, h, reach_ms)
        for key, s in (("all", r.mean(0)), ("lf", r[lf].mean(0) if lf.any() else r.mean(0))):
            for flip, sign in ((False, 1.0), (True, -1.0)):
                v = sign * s
                if best[key] is None:
                    nlag = len(lags)
                    best[key] = [np.full(nlag, -9.0), [None] * nlag]
                m = v > best[key][0]
                best[key][0] = np.where(m, v, best[key][0])
                for j in np.nonzero(m)[0]:
                    best[key][1][j] = (mode, wide, theta, flip)
    zero = int(np.argmin(np.abs(lags)))
    base = {k: float(an.score_at(sp, np.ones(len(freqs)))) for k in ("all",)}
    out = {}
    for key in ("all", "lf"):
        v, who = best[key]
        # r with nothing applied, at lag 0, for the same bands
        edges = [e for e, f in zip(sp.edges, sp.centres) if (f < an.LF_HZ or key == "all")]
        r0 = np.mean([np.real(sp.sxy[a:b].sum()) / np.sqrt(sp.sxx[a:b].sum() * sp.syy[a:b].sum()) for a, b in edges])
        peaks = [i for i in range(1, nlag - 1) if v[i] >= v[i - 1] and v[i] > v[i + 1]]
        peaks.sort(key=lambda i: -v[i])
        chosen = []
        for i in peaks:
            if all(abs(lags[i] - lags[j]) > 0.5 * fs * 1e-3 for j in chosen):
                chosen.append(i)
            if len(chosen) == 10:
                break
        inside = np.nonzero(np.abs(lags) <= an.MAX_DELAY_MS * fs * 1e-3)[0]
        k = inside[int(np.argmax(v[inside]))]
        out[key] = {"baseline": float(r0), "inrange_best": float(v[k]),
                    "inrange_peak": {"ms": float(lags[k] / fs * 1e3), "score": float(v[k]), "setting": list(who[k])},
                    "peaks": [{"ms": float(lags[i] / fs * 1e3), "score": float(v[i]), "setting": list(who[i])} for i in chosen]}
    return out


def work(job):
    tag, ref, inp, side, slice_name = job
    a = load(inp)[side]
    b = load(ref)[""]
    span = SLICES[slice_name]
    a, b = cut(a, span), cut(b, span)
    n = min(len(a), len(b))
    a, b = a[:n], b[:n]
    opts, base, lag, verdict, shift, message = an.suggest_with_shift(a, b, FS)
    row = {"tag": tag, "ref": ref, "input": inp + side, "slice": slice_name, "baseline": base, "attack_ms": lag[0],
           "attack_clear": lag[1], "attack_peak": lag[2], "verdict": verdict, "shift_samples": shift, "message": message,
           "options": [{"label": c.label(), "r": c.score, "gain": g, "low_end": lf} for _, c, g, lf in opts]}
    if slice_name == "whole":
        row["curve"] = curve(a, b)
    return row


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int, default=6)
    args = ap.parse_args()
    jobs = []
    for tag, ref, inp in PAIRS:
        for side in load(inp):
            for s in SLICES:
                jobs.append((tag, ref, inp, side, s))
    print(f"{len(jobs)} runs", flush=True)
    OUT.mkdir(parents=True, exist_ok=True)
    with Pool(args.jobs) as pool:
        rows = []
        for i, r in enumerate(pool.imap_unordered(work, jobs)):
            rows.append(r)
            print(f"{i + 1}/{len(jobs)} {r['ref']} <- {r['input']} [{r['slice']}]: {r['verdict']}", flush=True)
    rows.sort(key=lambda r: (r["tag"], r["ref"], r["input"], list(SLICES).index(r["slice"])))
    (OUT / "results.json").write_text(json.dumps(rows, indent=1))
    print(f"wrote {OUT / 'results.json'}")


if __name__ == "__main__":
    main()
