"""ANALYSE on the fourth set of stems (captures/stems3/, gitignored): one song, 4 min 12 s at 44.1 kHz, several mics per source.

    source .venv/bin/activate && python prototype/analyse_stems3.py [--jobs 8] [--only NAME]

File names are SOURCE-MIC-PREAMP (the preamp or channel strip doesn't matter), the source and the mic possibly with spaces or a
dash (source BASS, mic "AMP-MICA" from BASS-AMP-MICA-PREAMP); a trailing .L / .R is a channel and the two are combined into one
stereo track (analysed as the sum, as the plugin does). Which track is the sidechain for which is in captures/stems3/pairs.json
(gitignored, like the audio, so that no mic or preamp model is named in the repo): the user's choices (2026-10-08) were the DI for
the bass and the acoustic guitar, one mic of the electric guitar, the close mic of the piano, and the top snare and inside kick mics
for the drum mics, each drum mic run against both the kick and the snare. {"pairs": [{"ref": [source, mic], "inputs": [[source, mic],
...]}], "tests": [[kind, ref, input, part]], "repeat": [kind|input]}.

The plugin keeps at most 30 s of stretches (10 ms blocks) where both play above -60 dBFS, so each pair runs on consecutive windows of
30 s of that (as many as the song holds, up to eight), to see how steady the answer is across it. Results:
prototype/out/analyse/stems3/results.json.
"""
import argparse
import json
import re
import sys
from collections import defaultdict
from multiprocessing import Pool
from pathlib import Path

import numpy as np
import soundfile as sf

sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyse as an  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "captures" / "stems3"
OUT = an.OUT / "stems3"
WINDOW_S = 30.0
BLOCK_S = 0.010
ACTIVE_DB = -60.0


def parse(path):
    """(source, mic, preamp, channel) from 'SOURCE-MIC-PREAMP[.L|.R].wav'."""
    stem = path.stem
    m = re.match(r"^(.*)\.([LR])$", stem)
    channel = m.group(2) if m else ""
    stem = m.group(1) if m else stem
    parts = stem.split("-")
    return parts[0].strip(), "-".join(parts[1:-1]).strip(), parts[-1].strip(), channel


def catalogue():
    """{(source, mic): (preamp, [paths])}: the L and R files of one mic are one entry."""
    out = defaultdict(lambda: ["", []])
    for p in sorted(SRC.glob("*.wav")):
        source, mic, preamp, channel = parse(p)
        out[(source, mic)][0] = preamp
        out[(source, mic)][1].append((channel, p))
    return out


def load(source, mic):
    """The mono analysis signal and the rate; L and R files are summed (the plugin's own mono)."""
    chans = []
    fs = None
    for channel, p in sorted(catalogue()[(source, mic)][1]):
        x, fs = sf.read(p, dtype="float64")
        chans.append(x if x.ndim == 1 else x.mean(1))
    n = min(len(c) for c in chans)
    return np.mean([c[:n] for c in chans], axis=0), fs


def load_channels(source, mic):
    """([channel arrays], rate): L and R kept apart (for rendering a stereo mic as it is), a mono file as one."""
    chans, fs = [], None
    for _, p in sorted(catalogue()[(source, mic)][1]):
        x, fs = sf.read(p, dtype="float64")
        chans.append(x if x.ndim == 1 else x.mean(1))
    n = min(len(c) for c in chans)
    return [c[:n] for c in chans], fs


def config():
    """captures/stems3/pairs.json (see the docstring)."""
    return json.loads((SRC / "pairs.json").read_text())


# [(sidechain (source, mic), [inputs (source, mic)])]
PAIRS = [(tuple(g["ref"]), [tuple(i) for i in g["inputs"]]) for g in config()["pairs"]]


def gated_windows(x, y, fs):
    """What the plugin would capture while the song plays through: the 10 ms blocks where both play above ACTIVE_DB (rms), taken in
    order and cut into windows of WINDOW_S of them (the last kept if it has at least 10 s), as (index, seconds, x, y)."""
    n = min(len(x), len(y))
    b = int(BLOCK_S * fs)
    nb = n // b
    xb, yb = x[:nb * b].reshape(nb, b), y[:nb * b].reshape(nb, b)
    db = lambda v: 20 * np.log10(np.sqrt((v ** 2).mean(1)) + 1e-12)  # noqa: E731
    sel = np.nonzero((db(xb) > ACTIVE_DB) & (db(yb) > ACTIVE_DB))[0]
    per = int(WINDOW_S / BLOCK_S)
    out = []
    for w in range(0, len(sel), per):
        part = sel[w:w + per]
        if len(part) * BLOCK_S >= 10.0:
            out.append((len(out), len(part) * BLOCK_S, xb[part].reshape(-1), yb[part].reshape(-1), part[0] * BLOCK_S, part[-1] * BLOCK_S))
    return out


def work(job):
    ref, inp, w = job
    y, fs = load(*ref)
    x, fs2 = load(*inp)
    assert fs == fs2
    wins = gated_windows(x, y, fs)
    if w >= len(wins):
        return None
    _, seconds, xa, ya, t0, t1 = wins[w]
    row = {"ref": " ".join(ref), "input": " ".join(inp), "window": w, "windows": len(wins), "seconds": seconds,
           "song_from_s": t0, "song_to_s": t1, "fs": fs}
    opts, base, lag, verdict, shift, message = an.suggest_with_shift(xa, ya, fs)
    row.update(baseline=base, attack_ms=lag[0], attack_clear=bool(lag[1]), attack_peak=lag[2], verdict=verdict,
               shift=shift, message=message,
               options=[{"label": c.label(), "r": c.score, "gain": g, "low_end": lf, "mode": c.mode, "wide": c.wide,
                         "theta": c.theta, "delay_ms": c.delay_ms, "flip": c.flip} for _, c, g, lf in opts])
    return row


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--only", help="run only the pairs whose sidechain name contains this, merging into the existing results")
    args = ap.parse_args()
    cat = catalogue()
    for key, (pre, files) in sorted(cat.items()):
        print(f"{key[0]:15s} {key[1]:10s} preamp {pre:20s} {[c or 'mono' for c, _ in files]}")
    jobs = [(ref, inp, w) for ref, inputs in PAIRS for inp in inputs for w in range(8)
            if args.only is None or args.only in " ".join(ref)]
    print(f"{len(jobs)} runs", flush=True)
    OUT.mkdir(parents=True, exist_ok=True)
    rows = []
    with Pool(args.jobs) as pool:
        for i, r in enumerate(pool.imap_unordered(work, jobs)):
            if r is None:
                continue
            rows.append(r)
            print(f"{i + 1}/{len(jobs)} {r['ref']} <- {r['input']} [{r['window']}/{r['windows']}]: {r['verdict']}", flush=True)
    if args.only and (OUT / "results.json").exists():
        mine = {(r["ref"], r["input"]) for r in rows}
        rows += [r for r in json.loads((OUT / "results.json").read_text()) if (r["ref"], r["input"]) not in mine]
    rows.sort(key=lambda r: (r["ref"], r["input"], r["window"]))
    (OUT / "results.json").write_text(json.dumps(rows, indent=1))
    print(f"wrote {OUT / 'results.json'}")


if __name__ == "__main__":
    main()
