"""Which scoring agrees with the user's blind3 ranking? Every option the user heard is scored on the excerpt they heard, by several
measures, and a measure agrees on a pair of options when it orders them the way the user ranked them.

    source .venv/bin/activate && python prototype/analyse_blind3_metrics.py path/to/blind3-results-DATE.json

Needs captures/ and the key (prototype/out/analyse/blind3/key.json); opened only after the listening. The excerpts are cut as
build_blind3.py cut them (run_kit_test, run_song_test, run_early_test), on the mono analysis signals. Pairs are within a test, ties
and the repeat left out. Reports: all pairs, pairs without the flipped control, and the polarity pairs (the leading option against its
own polarity-inverted twin). Writes prototype/out/analyse/blind3/metrics.md."""
import json
import re
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyse as an  # noqa: E402
import analyse_stems2 as s2  # noqa: E402
import analyse_stems3 as s3  # noqa: E402
import build_blind3 as bb  # noqa: E402

FS = bb.FS
OUT = an.OUT / "blind3"


def parse(desc):
    """(Candidate, shift in samples) from a key description; None for the repeat."""
    if desc.startswith("repeat"):
        return None
    desc = desc.split(": ", 1)[-1]
    shift = 0
    m = re.search(r" after a manual shift of ([+-]\d+) samples", desc)
    if m:
        shift = int(m.group(1))
        desc = desc[:m.start()]
    if desc == "nothing applied":
        return an.Candidate("none", False, 0.0, 0.0, False, 0.0), 0
    flip = "Ø" in desc
    d = float(re.search(r"delay ([+-][0-9.]+) ms", desc).group(1))
    if desc.startswith("phase off"):
        return an.Candidate("none", False, 0.0, d, flip, 0), shift
    m = re.match(r"CONSTANT ([0-9.]+)°", desc)
    if m:
        return an.Candidate("constant", True, float(m.group(1)), d, flip, 0), shift
    m = re.match(r"(LO|HI) (in|out) ([0-9.]+)°", desc)
    return an.Candidate(m.group(1).lower(), m.group(2) == "in", float(m.group(3)), d, flip, 0), shift


def active_block(x, y, fs, per_s, lead_s=0.0, step=50, tail=0):
    """Start (in blocks) and block size of the span of per_s seconds where both play most, as the builder chose it."""
    b = int(s3.BLOCK_S * fs)
    nb = len(x) // b
    db = lambda v: 20 * np.log10(np.sqrt((v[:nb * b].reshape(nb, b) ** 2).mean(1)) + 1e-12)  # noqa: E731
    active = ((db(x) > s3.ACTIVE_DB) & (db(y) > s3.ACTIVE_DB)).astype(float)
    per = min(nb, int(per_s / s3.BLOCK_S))
    csum = np.concatenate([[0.0], np.cumsum(active)])
    starts = np.arange(int(lead_s * 100), nb - per - tail + (1 if not tail else 0), step)
    return int(starts[int(np.argmax(csum[starts + per] - csum[starts]))]), b, per


def heard(kind, ref, inp):
    """(x, y, fs): the mono input and sidechain over the excerpt the user heard."""
    if kind.startswith("s3-"):
        ref_ch, fs = s3.load_channels(*ref)
        in_ch, _ = s3.load_channels(*inp)
        n = min(len(ref_ch[0]), len(in_ch[0]))
        y, x = np.mean([c[:n] for c in ref_ch], axis=0), np.mean([c[:n] for c in in_ch], axis=0)
        s, b, per = active_block(x, y, fs, bb.EXCERPT_S, bb.LEAD_S, 100, 50)
        a = int(s * b)
        return x[a:a + int(bb.EXCERPT_S * fs)], y[a:a + int(bb.EXCERPT_S * fs)], fs
    if kind == "early":
        folder = bb.ROOT / "captures" / ("stems" if ref == "set2" else "")
        x = np.fromfile(folder / f"{inp}_a.f32", np.float32).astype(np.float64)
        y = np.fromfile(folder / f"{inp}_b.f32", np.float32).astype(np.float64)
        n = min(len(x), len(y))
        x, y = x[:n], y[:n]
        s, b, per = active_block(x, y, FS, bb.EXCERPT_S)
        return x[s * b:(s + per) * b], y[s * b:(s + per) * b], FS
    y = s2.load(ref)[""]
    tr = s2.load(inp)
    x = tr[" sum"] if "" not in tr else tr[""]
    n = min(len(x), len(y))
    window = bb.DRUM_WINDOW if kind in ("kick", "snare") else bb.BAND_WINDOW
    return bb.excerpt(x[:n, None], window, FS)[:, 0], bb.excerpt(y[:n, None], window, FS)[:, 0], FS


def band_r(sp, c, shift, fs):
    cc = an.Candidate(c.mode, c.wide, c.theta, c.delay_ms + shift / fs * 1e3, c.flip, 0)
    g = an.candidate_response(cc, fs, sp.freqs)
    cross, px, py, f = [], [], [], np.array(sp.centres)
    for a, b in sp.edges:
        cross.append(np.real((g[a:b] * sp.sxy[a:b]).sum()))
        px.append(sp.sxx[a:b].sum())
        py.append(sp.syy[a:b].sum())
    cross, px, py = map(np.array, (cross, px, py))
    return cross, px, py, f


def measures(sp, c, shift, fs):
    cross, px, py, f = band_r(sp, c, shift, fs)
    r = cross / np.sqrt(px * py)
    gain = 10 * np.log10((px + py + 2 * cross) / (px + py))
    loud = (px + py) ** 0.3
    return {
        "mean band r (current)": r.mean(),
        "band r, lows x2": np.average(r, weights=np.where(f < 500, 2.0, 1.0)),
        "mean r below 300 Hz": r[f < 300].mean(),
        "broadband r": cross.sum() / np.sqrt(px.sum() * py.sum()),
        "sum gain dB": gain.mean(),
        "sum gain dB, loudness-weighted": (gain * loud).sum() / loud.sum(),
        "energy-weighted r (= broadband gain)": 10 * np.log10(((px + py + 2 * cross).sum()) / (px + py).sum()),
    }


def main():
    res = json.loads(Path(sys.argv[1]).read_text())
    key = json.loads((OUT / "key.json").read_text())
    if res.get("build") != key["build"]:
        sys.exit("build mismatch")
    jobs = {f"{k}|{r}<-{i}" if not k.startswith("s3-") else f"{k}|{' '.join(r)}<-{' '.join(i)}": (k, r, i)
            for k, r, i, _ in bb.TESTS}
    jobs.update({f"early|{r}:{i}": (k, r, i) for k, r, i, _ in bb.TESTS if k == "early"})
    names = None
    tally = {"all": {}, "no flipped": {}, "polarity": {}}
    per_test, lines = [], []
    for t in res["tests"]:
        k = key["tests"][t["id"]]
        ranks = {o: r for o, r in t["ranks"].items() if r}
        if not ranks:
            continue
        x, y, fs = heard(*jobs[k["test"]])
        n = min(len(x), len(y))
        sp = an.spectra(x[:n], y[:n], fs)
        sc = {}
        for o in ranks:
            p = parse(k["options"][o]["desc"])
            if p:
                sc[o] = measures(sp, p[0], p[1], fs)
        names = names or list(next(iter(sc.values())))
        kinds = {o: k["options"][o]["kind"] for o in sc}
        lead = next((o for o in sc if kinds[o] in ("pick1", "paper")), None)
        flip = next((o for o in sc if kinds[o] == "flipped"), None)
        ok_t = {m: [0, 0] for m in names}
        opts = [o for o in sc if o in ranks]
        for i, a in enumerate(opts):
            for b in opts[i + 1:]:
                if ranks[a] == ranks[b]:
                    continue
                better, worse = (a, b) if ranks[a] < ranks[b] else (b, a)
                groups = ["all"]
                if "flipped" not in (kinds[a], kinds[b]):
                    groups.append("no flipped")
                if {a, b} == {lead, flip}:
                    groups.append("polarity")
                for m in names:
                    ok = sc[better][m] > sc[worse][m]
                    for g in groups:
                        tally[g].setdefault(m, []).append(ok)
                    ok_t[m][0] += ok
                    ok_t[m][1] += 1
        per_test.append((t["id"], k["test"], ok_t))
    lines.append("# Blind3 verdicts against the scoring measures\n")
    for g, d in tally.items():
        lines.append(f"## Pairs: {g} ({len(next(iter(d.values())))} pairs)\n\n| measure | agrees |\n|---|---|")
        for m in names:
            lines.append(f"| {m} | {np.mean(d[m]):.0%} ({sum(d[m])} of {len(d[m])}) |")
        lines.append("")
    lines.append("## Per test (share of the user's ordered pairs each measure reproduces)\n")
    lines.append("| test | " + " | ".join(m.split(" (")[0][:16] for m in names) + " |\n|---|" + "---|" * len(names))
    for tid, name, ok_t in per_test:
        lines.append(f"| {tid} {name[:40]} | " + " | ".join(f"{ok_t[m][0] / max(ok_t[m][1], 1):.0%}" for m in names) + " |")
    text = "\n".join(lines)
    (OUT / "metrics.md").write_text(text)
    print(text)


if __name__ == "__main__":
    main()
