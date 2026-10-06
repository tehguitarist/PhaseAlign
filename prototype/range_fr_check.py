"""Frequency-response check of the RANGE A/B renders (prototype/range_ab.py): the level of each 50/50 sum against the pink
noise it was made from, measured from the WAVs with Welch's method, overlaid on the closed-form prediction in report.md.

    .venv/bin/python prototype/range_fr_check.py <path to the old hilo.py>   # → prototype/out/range/fr_check.png

An all-pass is flat on its own; the dips are in the sum with the dry track, (1 + H) / 2, which is what the WAVs are.
"""

import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import soundfile as sf
from scipy.signal import csd, welch

import range_ab
from oversampling import response

OUT = Path(__file__).resolve().parent / "out" / "range"
FS = range_ab.FS


def measured_db(tag):
    x, _ = sf.read(OUT / "dry_pink.wav", dtype="float64")
    y, _ = sf.read(OUT / f"{tag}.wav", dtype="float64")
    x, y = x[:, 0], y[:, 0]
    f, pxx = welch(x, FS, nperseg=16384)
    _, pxy = csd(x, y, FS, nperseg=16384)
    return f, 20 * np.log10(np.abs(pxy / pxx))  # |H| of the sum; its latency only adds delay


def main():
    old = range_ab.load_old(sys.argv[1])
    fig, axes = plt.subplots(2, 2, figsize=(12, 7), sharex=True, sharey=True)
    rows = []
    for r, mode in enumerate(("lo", "hi")):
        for c, knob in enumerate((0.5, 1.0)):
            ax = axes[r, c]
            for variant, colour, ls in (("current", "#c0504d", "--"), ("new", "#2e75b6", "-")):
                tag = f"{mode.upper()}_180_k{int(knob * 100)}_pink_{variant}"
                f, db = measured_db(tag)
                sel = (f >= 30) & (f <= 18000)
                ax.semilogx(f[sel], db[sel], color=colour, ls=ls, lw=1.1, alpha=0.9, label=f"{variant}, measured")
                _, lagf = range_ab.setting(old, variant, mode, True, knob)
                pred = 20 * np.log10(np.abs(1 + response(FS, f[sel]) * np.exp(-1j * np.radians(lagf(f[sel])))) / 2)
                ax.semilogx(f[sel], pred, color="k", ls=":", lw=1.0, label="predicted" if variant == "new" else None)
                dev = np.abs(db[sel] - pred)
                deep = np.argmin(pred)
                rows.append((tag, f[sel][deep], pred[deep], float(np.median(dev)), float(np.percentile(dev, 95))))
            ax.set_title(f"{'HI' if mode == 'hi' else 'LO'}, range in, knob {int(knob * 100)}%  (files {mode.upper()}_180_k{int(knob * 100)})", fontsize=10)
            ax.grid(True, which="both", alpha=0.25)
            ax.set_ylim(-30, 3)
            if c == 0:
                ax.set_ylabel("sum with the dry track (dB)")
            if r == 1:
                ax.set_xlabel("frequency (Hz)")
            ax.legend(fontsize=7.5, loc="lower right")
    fig.suptitle("RANGE in: level of the processed + dry sum, measured from the WAVs (pink noise) against the prediction", fontsize=11)
    fig.tight_layout(rect=(0, 0, 1, 0.96))
    fig.savefig(OUT / "fr_check.png", dpi=110)
    print(f"{'file':34s} {'deepest predicted dip':>26s}   measured vs predicted: median / 95th pct (dB)")
    for tag, fd, dd, med, p95 in rows:
        print(f"{tag:34s} {dd:7.1f} dB at {fd:7.0f} Hz        {med:5.2f} / {p95:5.2f}")


if __name__ == "__main__":
    main()
