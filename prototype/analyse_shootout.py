"""Shootout: the user's by-ear settings (2026-10-07) scored with the same metric as ANALYSE's candidates.

    source .venv/bin/activate && python prototype/analyse_shootout.py [--reach 6]

For each pair of captures/stems: baseline r, the search's best (attack-first and joint), and the user's setting scored
with the delay taken both ways (the user's sign convention is unknown). r is band-averaged per 1/3 octave (analyse.py).
"""
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyse as an  # noqa: E402
import analyse_stems as st  # noqa: E402

# tag: (mode, wide, panel angle, delay ms, polarity flipped, note)
USER = {
    "kick_sample": ("lo", False, 22.0, 72.8 / 48.0, False, "72.8 samples"),
    "kick_oh": ("lo", False, 17.9, 0.0, False, ""),
    "snare_sample": ("lo", False, 83.6, 4.0, True, "263.6 read as inverted + 83.6; delay 4 ms"),
    "snare_oh": ("lo", False, 20.3, 0.0, False, ""),
    "bass_amp": ("hi", False, 8.8, 4.6, False, ""),
    "gtr_2": ("lo", True, 102.6, 0.0, False, ""),
}


def r_of(sp, fs, mode, wide, theta, delay_ms, flip):
    h = an.stage_response(mode, wide, theta, fs, sp.freqs)
    g = (-1.0 if flip else 1.0) * h * np.exp(-2j * np.pi * sp.freqs * delay_ms * 1e-3)
    return an.score_at(sp, g)


def main():
    if "--reach" in sys.argv:
        an.MAX_DELAY_MS = float(sys.argv[sys.argv.index("--reach") + 1])
    fs = 48000
    out = [f"# Shootout (delay reach ±{an.MAX_DELAY_MS:g} ms)\n",
           "| pair | baseline | search: attack-first | search: joint | user, delay +d | user, delay −d |\n|---|---|---|---|---|---|"]
    for tag, track, ref in st.PAIRS:
        a, b = st.load(track), st.load(ref)
        n = min(len(a), len(b))
        a, b = a[:n], b[:n]
        fam, base, lag, how = an.analyse(a, b, fs)
        sp = an.spectra(a, b, fs)
        joint, _ = an.search(sp, top=1)
        mode, wide, theta, d, flip, note = USER[tag]
        plus, minus = r_of(sp, fs, mode, wide, theta, d, flip), r_of(sp, fs, mode, wide, theta, -d, flip)
        row = (f"| {tag} | {base:+.3f} | {fam[0].score:+.3f} ({fam[0].label()}; {how}) | {joint[0].score:+.3f} ({joint[0].label()}) "
               f"| {plus:+.3f} | {minus:+.3f} |")
        print(row)
        out.append(row)
    (an.OUT / "shootout.md").write_text("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
