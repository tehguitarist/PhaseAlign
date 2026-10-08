"""Build the blind listening test from the user's stems (captures/stems2/: a kit, bass, guitars; captures/stems3/: one song, several mics
per source), as one page.

    source .venv/bin/activate && python prototype/build_blind3.py [--jobs 8]

Writes the page to captures/stems2/blind/ (index.html, one opaque .js data file per test, gitignored with captures/) and the
KEY to prototype/out/analyse/blind3/key.json. The page holds no labels, settings or scores, only opaque ids: the user ranks
by ear and sends back results.json; prototype/blind3_decode.py joins it with the key. Nothing in the page names an option's
kind, so open the key only after the listening. The page and the key carry the same build id, and the page sets aside answers
saved by another build.

Each test is one sidechain and one track. An option is that track through a setting, summed with the sidechain, which is how the
plugin is used (the groundwork's finding: judge by the sum). The options per test (duplicates dropped, at most five besides OFF and
the flipped one):
  off                   nothing applied
  pick 1 / pick 2       what ANALYSE shows today (suggest_with_shift on the capture, its manual shift applied)
  noshift               when a shift is advised: the best phase for the track where it is (what ANALYSE now also offers as the
                        row that works without the shift; "small" if its gain is under the usual minimum)
  paper                 best all-band score inside the knob's reach, whatever ANALYSE shows
  lf                    best score under 300 Hz inside the reach
  alt                   the next-best distinct in-range peak of the all-band score
  delay-only            best delay and polarity with the phase stage off
  hit-timing            kick and snare pairs: the delay from where the hits start (prototype/hit_timing.py: the first rise of the
                        high-passed envelope, hit by hit) when that reading is tight, with the best phase and polarity within 0.3 ms
                        of it; a candidate for the ear, not part of ANALYSE
  shift small / lf      beyond the reach, if the score there beats the reach by a margin: the smallest shift that gets most of
                        the gain (the user's room-mic point: a few ms nearer is harmless, the full alignment is not), and the same
                        for the low-end score
  first / last          stems2 bass and guitar: ANALYSE's pick on the sustained half and on the stabs
  win a / win b         stems3: ANALYSE's pick on the song's first and last captures, where it differs from the one used
  flipped (outlier)     the leading option with the polarity inverted
  repeat (some tests)   the leading option a second time under another id, to see how consistent the listening is
Stereo tracks are analysed as their sum and every option is applied to both sides; dual-mono files are mono. For the song (part 3,
44.1 kHz) the picks come from the 30 s of active audio (10 ms blocks where both play above -60 dBFS) around the 10 s excerpt, as the
plugin's capture would hold it, and the excerpt is rendered from the song with 2 s of lead-in.
"""
import argparse
import base64
import hashlib
import json
import sys
from multiprocessing import Pool
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyse as an  # noqa: E402
import analyse_stems2 as s2  # noqa: E402
import analyse_stems3 as s3  # noqa: E402
import hit_timing as ht  # noqa: E402

FS = 48000
ROOT = Path(__file__).resolve().parent.parent
WEB = ROOT / "captures" / "stems2" / "blind"
KEY = an.OUT / "blind3" / "key.json"
KICK, SNARE = "Drum Kick", "Drum Snare Top"
DRUM_WINDOW = [(4.0, 14.0)]
BAND_WINDOW = [(6.0, 12.0), (20.0, 26.0)]       # sustained half, then the stabs
XFADE = 0.03
EDGE = 0.003            # every clip's own fade in and out, so the loop point doesn't click
MAX_OPTIONS = 6
REPEAT_IN = {"kick|Drum Kick Sample", "snare|Drum Snare Bottom", "bass|Bass Amp"} | set(s3.config()["repeat"])
EXCERPT_S, LEAD_S = 10.0, 2.0

# (kind, sidechain, track, part): part 1 is the core set, 2 the optional rest of the kit, 3 the song, 4 the user's earlier stems from
# the first two rounds (all optional beyond part 1)
TESTS = ([("kick", KICK, n, 1) for n in ("Drum Kick Sample", "Drum OHs", "Drum Room Close", "Drum Room Far", "Drum Bleed")]
         + [("kick", KICK, "Drum Room Mono", 2)]
         + [("snare", SNARE, n, 1) for n in ("Drum Snare Sample", "Drum Snare Bottom", "Drum OHs", "Drum Room Close")]
         + [("snare", SNARE, n, 2) for n in ("Drum Room Far", "Drum Room Mono", "Drum Bleed")]
         + [("bass", "Bass DI", "Bass Amp", 1), ("gtr", "GTR 1", "GTR 2", 1)]
         + [("bass-gtr", b, g, 2) for b in ("Bass DI", "Bass Amp") for g in ("GTR 1", "GTR 2")]
         # the song (captures/stems3/pairs.json: no mic model is named in the repo)
         + [(k, tuple(r), tuple(i), p) for k, r, i, p in s3.config()["tests"]]
         # the first two rounds of the user's stems (48 kHz pairs: input = the track, sidechain = the reference)
         + [("early", "set1", tag, 4) for tag in ("kick", "snare", "bass", "guitar", "hats")]
         + [("early", "set2", tag, 4) for tag in ("kick_sample", "kick_oh", "snare_sample", "snare_oh", "bass_amp", "gtr_2")])


def cand_key(c, shift=0, fs=FS):
    d = c.delay_ms + shift / fs * 1e3
    return (c.mode, c.wide, round(c.theta / 7.5), c.flip, round(d / (0.2 if abs(d) <= 4.5 else 0.5)))


def is_off(c, shift, fs=FS):
    return c.mode == "none" and not c.flip and abs(c.delay_ms + shift / fs * 1e3) < 0.1


def setting(p, ms=None):
    mode, wide, theta, flip = p["setting"]
    return an.Candidate(mode, bool(wide), float(theta), p["ms"] if ms is None else ms, bool(flip), p["score"])


def describe(c, shift):
    s = c.label()
    return s + (f" after a manual shift of {shift:+d} samples" if shift else "")


def build_options(x, y, fs, percussive=False):
    """[(kind, description, Candidate, shift in samples)] for one test; x, y are the analysis signals."""
    opts, seen = [], set()

    def add(kind, c, shift=0):
        k = cand_key(c, shift, fs)
        if k in seen or is_off(c, shift, fs):
            return False
        seen.add(k)
        opts.append((kind, describe(c, shift), c, shift))
        return True

    sp = an.spectra(x, y, fs)
    cv = s2.curve(x, y, fs=fs)
    picks, base, lag, verdict, shift, message = an.suggest_with_shift(x, y, fs)
    meta = {"baseline": base, "verdict": verdict, "message": message, "attack_ms": lag[0], "attack_clear": bool(lag[1])}
    for i, (_, c, gain, lf) in enumerate(picks):
        add(f"pick{i + 1}", c, shift)
    if shift:       # a shift is advised: the best this track can do where it is (the user, 2026-10-08), to hear against the shifted picks
        for _, c, gain, lf, small in an.as_is_options(x, y, fs)[:1]:
            add("noshift-small" if small else "noshift", c, 0)
    ap, lp = cv["all"]["inrange_peak"], cv["lf"]["inrange_peak"]
    add("paper", setting(ap))
    add("lf", setting(lp))
    d1, _ = an.search(sp, top=1, phase_on=False)
    if d1:
        add("delay-only", d1[0])
    beyond = [p for p in cv["all"]["peaks"] if abs(p["ms"]) > an.MAX_DELAY_MS]
    if beyond and beyond[0]["score"] - cv["all"]["inrange_best"] >= 0.02:
        gap = beyond[0]["score"] - cv["all"]["inrange_best"]
        near = [p for p in beyond if p["score"] - cv["all"]["inrange_best"] >= 0.6 * gap]
        add("shift-small", setting(min(near, key=lambda p: abs(p["ms"]))))
    beyond = [p for p in cv["lf"]["peaks"] if abs(p["ms"]) > an.MAX_DELAY_MS]
    if beyond and beyond[0]["score"] - cv["lf"]["inrange_best"] >= 0.08:
        gap = beyond[0]["score"] - cv["lf"]["inrange_best"]
        near = [p for p in beyond if p["score"] - cv["lf"]["inrange_best"] >= 0.6 * gap]
        add("shift-lf", setting(min(near, key=lambda p: abs(p["ms"]))))
    alt = [q for q in cv["all"]["peaks"] if abs(q["ms"]) <= an.MAX_DELAY_MS and q["score"] > cv["all"]["baseline"]]
    for q in alt[:3]:
        if add("alt", setting(q)):
            break
    if percussive:
        r = ht.hit_timing(x, y, fs)
        if r is not None:
            meta["hit_timing"] = {"lag_ms": r[0], "spread_ms": r[1], "hits": r[2]}
            if r[1] <= ht.TIGHT_MS and abs(r[0]) <= an.MAX_DELAY_MS:
                fam, _ = an.search(sp, top=1, window_ms=(r[0] - 0.3, r[0] + 0.3))
                if fam:
                    add("hit-timing", fam[0])
    meta["curve"] = {k: {"baseline": v["baseline"], "inrange_best": v["inrange_best"]} for k, v in cv.items()}
    return opts, meta, add


def edge_fade(a, fs):
    n = int(EDGE * fs)
    w = 0.5 - 0.5 * np.cos(np.pi * np.arange(n) / n)
    a = a.copy()
    a[:n] *= w[:, None]
    a[-n:] *= w[::-1][:, None]
    return a


def excerpt(sig, window, fs):
    """The window(s) of sig (samples x channels), joined with a raised-cosine crossfade."""
    parts = [sig[int(a * fs):int(b * fs)] for a, b in window]
    out = parts[0]
    xf = int(XFADE * fs)
    for p in parts[1:]:
        w = (0.5 - 0.5 * np.cos(np.pi * np.arange(xf) / xf))[:, None]
        out = np.concatenate([out[:-xf], out[-xf:] * (1 - w) + p[:xf] * w, p[xf:]])
    return out


def to_ch(a, n):
    return a if a.shape[1] == n else np.repeat(a[:, :1], n, 1)


def finalize(kind, inp_name, opts, meta, baseline, in_ch, ref_ch, fs, seed, window, cut=None, build_extra=None):
    """Turn the options into clips: the track through each, summed with the sidechain, the excerpt cut, the gains worked out."""
    rng = np.random.default_rng(seed)
    priority = ["pick1", "noshift", "noshift-small", "paper", "shift-small", "lf", "shift-lf", "slice-first", "slice-last", "win-a", "win-b", "hit-timing", "pick2", "alt",
                "delay-only"]
    opts = sorted(opts, key=lambda o: priority.index(o[0]))
    keep = opts[:MAX_OPTIONS]
    off = ("off", "nothing applied", an.Candidate("none", False, 0.0, 0.0, False, baseline), 0)
    lead = keep[0] if keep else off
    c = lead[2]
    flipped = an.Candidate(c.mode, c.wide, c.theta, c.delay_ms, not c.flip, c.score)
    final = [off] + keep + [("flipped", "leading option, polarity inverted: " + describe(flipped, lead[3]), flipped, lead[3])]
    if f"{kind}|{inp_name}" in REPEAT_IN:
        final.append(("repeat", "repeat of " + lead[0], lead[2], lead[3]))
    nch = max(len(in_ch), len(ref_ch))
    sc = to_ch(np.stack(ref_ch, 1), nch)
    clips = []
    for okind, desc, cand, shift in final:
        outs = []
        for ch in in_ch:
            xx = an.shift_samples(ch, shift) if shift else ch
            outs.append(an.render(xx, fs, cand) if (not is_off(cand, shift, fs) or cand.flip) else xx)
        mix = to_ch(np.stack(outs, 1), nch) + sc
        clips.append((okind, desc, edge_fade(cut(mix) if cut else excerpt(mix, window, fs), fs)))
    ref_clip = edge_fade(cut(sc) if cut else excerpt(sc, window, fs), fs)
    peak = max(np.abs(c_[2]).max() for c_ in clips)
    g = 0.89 / peak
    rms = [np.sqrt(np.mean((c_[2] * g) ** 2)) for c_ in clips]
    target = min(rms)
    ids = [hashlib.sha1(f"{seed}-{i}".encode()).hexdigest()[:6] for i in range(len(clips))]
    order = rng.permutation(len(clips))
    ref_rms = np.sqrt(np.mean((ref_clip * g) ** 2))
    m_ref = min(target / ref_rms, 0.95 / max(np.abs(ref_clip * g).max(), 1e-9))
    data = {"ch": nch, "rate": fs, "ref": {"m": float(m_ref), "d": b64(ref_clip * g)}, "order": [ids[i] for i in order],
            "clips": {ids[i]: {"m": float(target / rms[i]), "d": b64(clips[i][2] * g)} for i in range(len(clips))}}
    options = {ids[i]: {"kind": clips[i][0], "desc": clips[i][1]} for i in range(len(clips))}
    return data, options


def run_test(job):
    idx, (kind, ref, inp, part), seed = job
    if kind.startswith("s3-"):
        return run_song_test(idx, kind, ref, inp, part, seed)
    if kind == "early":
        return run_early_test(idx, kind, ref, inp, part, seed)
    return run_kit_test(idx, kind, ref, inp, part, seed)


def run_kit_test(idx, kind, ref, inp, part, seed):
    y = s2.load(ref)[""]
    tr = s2.load(inp)
    stereo = "" not in tr
    chans = [tr[" L"], tr[" R"]] if stereo else [tr[""]]
    x = tr[" sum"] if stereo else tr[""]
    n = min(len(x), len(y), *(len(c) for c in chans))
    x, y, chans = x[:n], y[:n], [c[:n] for c in chans]
    opts, meta, add = build_options(x, y, FS, percussive=kind in ("kick", "snare"))
    if kind in ("bass", "gtr"):
        for name, span in (("first", s2.SLICES["first15"]), ("last", s2.SLICES["last15"])):
            xa, ya = s2.cut(x, span), s2.cut(y, span)
            m = min(len(xa), len(ya))
            picks, _, _, _, shift, _ = an.suggest_with_shift(xa[:m], ya[:m], FS)
            if picks:
                add(f"slice-{name}", picks[0][1], shift)
    window = DRUM_WINDOW if kind in ("kick", "snare") else BAND_WINDOW
    data, options = finalize(kind, inp, opts, meta, meta["baseline"], chans, [y], FS, seed, window)
    key = {"test": f"{kind}|{ref}<-{inp}", "part": part, "meta": meta, "options": options}
    return idx, data, key


def run_song_test(idx, kind, ref, inp, part, seed):
    ref_ch, fs = s3.load_channels(*ref)
    in_ch, _ = s3.load_channels(*inp)
    n = min(len(ref_ch[0]), len(in_ch[0]))
    ref_ch, in_ch = [c[:n] for c in ref_ch], [c[:n] for c in in_ch]
    y, x = np.mean(ref_ch, axis=0), np.mean(in_ch, axis=0)
    # the 10 s excerpt: where both play most (10 ms blocks above -60 dBFS), with room for the lead-in
    b = int(s3.BLOCK_S * fs)
    nb = n // b
    db = lambda v: 20 * np.log10(np.sqrt((v[:nb * b].reshape(nb, b) ** 2).mean(1)) + 1e-12)  # noqa: E731
    active = ((db(x) > s3.ACTIVE_DB) & (db(y) > s3.ACTIVE_DB)).astype(float)
    per = int(EXCERPT_S / s3.BLOCK_S)
    csum = np.concatenate([[0.0], np.cumsum(active)])
    starts = np.arange(int(LEAD_S * 100), nb - per - 50, 100)
    t0 = float(starts[int(np.argmax(csum[starts + per] - csum[starts]))] * s3.BLOCK_S)
    wins = s3.gated_windows(x, y, fs)
    mid = t0 + EXCERPT_S / 2
    w = min(range(len(wins)), key=lambda i: 0 if wins[i][4] <= mid <= wins[i][5] else min(abs(mid - wins[i][4]), abs(mid - wins[i][5])))
    _, secs, xa, ya, f0, f1 = wins[w]
    opts, meta, add = build_options(xa, ya, fs, percussive=kind in ("s3-kick", "s3-snare"))
    for name, i in (("a", 0), ("b", len(wins) - 1)):
        if i != w:
            picks, _, _, _, shift, _ = an.suggest_with_shift(wins[i][2], wins[i][3], fs)
            if picks:
                add(f"win-{name}", picks[0][1], shift)
    meta.update(excerpt_from_s=t0, analysis_window=w, analysis_seconds=secs, analysis_span_s=[f0, f1], windows=len(wins), fs=fs)
    a, z = int((t0 - LEAD_S) * fs), int((t0 + EXCERPT_S + 0.5) * fs)
    seg_in, seg_ref = [c[a:z] for c in in_ch], [c[a:z] for c in ref_ch]
    lo, hi = int(LEAD_S * fs), int((LEAD_S + EXCERPT_S) * fs)
    data, options = finalize(kind, " ".join(inp), opts, meta, meta["baseline"], seg_in, seg_ref, fs, seed, None, cut=lambda m: m[lo:hi])
    key = {"test": f"{kind}|{' '.join(ref)}<-{' '.join(inp)}", "part": part, "meta": meta, "options": options}
    return idx, data, key


def run_early_test(idx, kind, ref, inp, part, seed):
    """One of the user's first two rounds of pairs (captures/<tag>_a.f32 and _b.f32; set 2 under captures/stems/)."""
    folder = ROOT / "captures" / ("stems" if ref == "set2" else "")
    x = np.fromfile(folder / f"{inp}_a.f32", np.float32).astype(np.float64)
    y = np.fromfile(folder / f"{inp}_b.f32", np.float32).astype(np.float64)
    n = min(len(x), len(y))
    x, y = x[:n], y[:n]
    opts, meta, add = build_options(x, y, FS, percussive=any(t in inp for t in ("kick", "snare")))
    # the excerpt: up to 10 s where both play most
    b = int(s3.BLOCK_S * FS)
    nb = n // b
    db = lambda v: 20 * np.log10(np.sqrt((v[:nb * b].reshape(nb, b) ** 2).mean(1)) + 1e-12)  # noqa: E731
    active = ((db(x) > s3.ACTIVE_DB) & (db(y) > s3.ACTIVE_DB)).astype(float)
    per = min(nb, int(EXCERPT_S / s3.BLOCK_S))
    csum = np.concatenate([[0.0], np.cumsum(active)])
    starts = np.arange(0, nb - per + 1, 50)
    lo = int(starts[int(np.argmax(csum[starts + per] - csum[starts]))] * b)
    hi = lo + per * b
    data, options = finalize(kind, f"{ref}:{inp}", opts, meta, meta["baseline"], [x], [y], FS, seed, None, cut=lambda m: m[lo:hi])
    meta.update(excerpt_from_s=lo / FS, seconds=n / FS)
    key = {"test": f"{kind}|{ref}:{inp}", "part": part, "meta": meta, "options": options}
    return idx, data, key


def b64(a):
    return base64.b64encode(np.clip(np.round(a * 32767), -32768, 32767).astype("<i2").tobytes()).decode()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int, default=8)
    args = ap.parse_args()
    rng = np.random.default_rng(2026)
    # part 1 first, then 2, then 3, shuffled inside each part (the first 19 keep their places when tests are added at the end)
    order = sorted(range(len(TESTS)), key=lambda i: (TESTS[i][3], rng.random()))
    jobs = [(i, TESTS[i], 31000 + i) for i in order]
    WEB.mkdir(parents=True, exist_ok=True)
    KEY.parent.mkdir(parents=True, exist_ok=True)
    for old in WEB.glob("*.js"):
        old.unlink()
    manifest, keys, build = [], {}, hashlib.sha1()
    with Pool(args.jobs) as pool:
        results = {i: (d, k) for i, d, k in pool.imap_unordered(run_test, jobs)}
    for pos, i in enumerate(order):
        data, key = results[i]
        tid = f"T{pos + 1:02d}"
        fname = hashlib.sha1(f"blind3-{tid}".encode()).hexdigest()[:10] + ".js"
        text = f'window.__load("{tid}",{json.dumps(data, separators=(",", ":"))});\n'
        (WEB / fname).write_text(text)
        build.update(text.encode())
        manifest.append({"id": tid, "file": fname, "part": key["part"], "ch": data["ch"], "options": data["order"]})
        keys[tid] = key
        print(tid, f"{len(key['options'])} versions, {(WEB / fname).stat().st_size / 1e6:.1f} MB", flush=True)   # nothing that gives the blind away
    build_id = build.hexdigest()[:12]
    KEY.write_text(json.dumps({"build": build_id, "tests": keys}, indent=1))
    html = (Path(__file__).resolve().parent / "blind3_page.html").read_text()
    html = html.replace("/*MANIFEST*/[]", json.dumps(manifest)).replace('/*BUILD*/""', json.dumps(build_id))
    (WEB / "index.html").write_text(html)
    print(f"build {build_id}\npage: {WEB / 'index.html'}\nkey:  {KEY}")


if __name__ == "__main__":
    main()
