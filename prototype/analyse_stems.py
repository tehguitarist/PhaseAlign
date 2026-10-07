"""ANALYSE on the user's second set of stems (captures/stems/, gitignored), each a track aligned to a sidechain.

    source .venv/bin/activate && python prototype/analyse_stems.py

Input = the track the plugin sits on, sidechain = the reference (the user's naming, 2026-10-07). Stereo files are summed to
mono; each pair is trimmed to its shorter length from the start. Renders for listening go to prototype/out/analyse/stems/<pair>/
(sidechain, input_off, sum_off, and input_candN / sum_candN for the top three), one common gain per pair.
"""
import sys
from pathlib import Path

import numpy as np
import soundfile as sf

sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyse as an  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
PAIRS = [("snare_sample", "Snare Sample", "Snare Top"), ("snare_oh", "Snare OH", "Snare Top"),
         ("kick_sample", "Kick Sample", "Kick"), ("kick_oh", "Kick OH", "Kick"),
         ("bass_amp", "Bass Amp", "Bass DI"), ("gtr_2", "GTR 2", "GTR 1")]


def load(name):
    x, fs = sf.read(ROOT / "captures" / "stems" / f"{name}.wav")
    assert fs == 48000, (name, fs)
    return (x.mean(1) if x.ndim > 1 else x).astype(np.float64)


def main():
    fs = 48000
    lines = ["# ANALYSE on the second stem set\n", "Input = the track, sidechain = the reference; positive delay = delay the input.\n"]
    for tag, track, ref in PAIRS:
        a, b = load(track), load(ref)
        n = min(len(a), len(b))
        a, b = a[:n], b[:n]
        fam, base, lag, how = an.analyse(a, b, fs)
        sp = an.spectra(a, b, fs)
        joint, _ = an.search(sp, top=3)
        label, gain, margin = an.verdict(fam, base, lag)
        print(f"\n{tag} ({track} -> {ref}, {n / fs:.1f} s): baseline r {base:+.3f}; attack lag {lag[0]:+.2f} ms "
              f"({'clear' if lag[1] else 'unclear'}, peak {lag[2]:.2f}, rival {lag[3]:.2f}); delay from {how}")
        print(f"  verdict: {label} (gain {gain:+.3f}, margin {margin:+.3f})")
        lines += [f"## {tag}: {track} → {ref}\n", f"Baseline r {base:+.3f}. Attack lag {lag[0]:+.2f} ms "
                  f"({'clear' if lag[1] else 'unclear'}; peak {lag[2]:.2f}, rival {lag[3]:.2f}). Delay from {how}. "
                  f"**Verdict: {label}** (gain {gain:+.3f}, margin {margin:+.3f}).\n",
                  "| version | # | setting | predicted r | rendered r |\n|---|---|---|---|---|"]
        for name, lst in (("joint", joint), ("attack-first", fam)):
            for i, c in enumerate(lst[:3]):
                rend = an.rendered_score(a, b, fs, c)
                print(f"  {name:12s} {i + 1}. {c.label():48s} predicted {c.score:+.3f}  rendered {rend:+.3f}")
                lines.append(f"| {name} | {i + 1} | {c.label()} | {c.score:+.3f} | {rend:+.3f} |")
        lines.append("")
        d = an.OUT / "stems" / tag
        d.mkdir(parents=True, exist_ok=True)
        clips = {"sidechain": b, "input_off": a, "sum_off": a + b}
        for i, c in enumerate(fam[:3]):
            y = an.render(a, fs, c)
            clips[f"input_cand{i + 1}"], clips[f"sum_cand{i + 1}"] = y, y + b
        g = 0.9 / max(np.abs(v).max() for v in clips.values())
        for k, v in clips.items():
            sf.write(d / f"{k}.wav", (v * g).astype(np.float32), fs, subtype="FLOAT")
        (d / "candidates.txt").write_text("\n".join(f"cand{i + 1}: {c.label()} (r {c.score:+.3f})" for i, c in enumerate(fam[:3])) + "\n")
    (an.OUT / "stems_report.md").write_text("\n".join(lines) + "\n")
    print(f"\nwrote {an.OUT / 'stems_report.md'}")


if __name__ == "__main__":
    main()
