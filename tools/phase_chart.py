#!/usr/bin/env python3
"""The README's second phase figure: the phase of the plugin's output relative to its input, as an analyser would show it
(signed, wrapped to +-180 degrees, a lag negative), for each mode and range at 25, 50, 75 and 100% of the knob's travel.
The same six panels as the first figure (tools/phase_modes_figure.py). HIGH and LOW come from the golden-tested reference
(reference/hilo.py, the oversampled sections at 48 kHz); CONSTANT is a true rotation, the same angle at every frequency.

    .venv/bin/pip install -r tools/requirements.txt
    .venv/bin/python tools/phase_chart.py       # writes docs/images/phase-chart.png
"""

import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "reference"))
import hilo  # noqa: E402

OUT = ROOT / "docs" / "images" / "phase-chart.png"
FS = 48000
KNOBS = (0.25, 0.5, 0.75, 1.0)
COLOURS = ("#9bc2e6", "#5b9bd5", "#2e75b6", "#0b3d6e")
f = np.geomspace(20, 20000, 1200)


def wrapped(lag_deg):
    """The phase for a lag, wrapped to (-180, 180]; a NaN where it wraps so the line isn't drawn across the jump."""
    p = (-np.asarray(lag_deg, float) + 180.0) % 360.0 - 180.0
    p = np.where(p == -180.0, 180.0, p)
    p[1:][np.abs(np.diff(p)) > 180.0] = np.nan
    return p


# (title, mode, wide, reference frequencies to mark)
PANELS = [[("LOW, RANGE out", "lo", False, (75.1,)), ("HIGH, RANGE out", "hi", False, (150.1,)), ("CONSTANT, RANGE out", None, False, ())],
          [("LOW, RANGE in", "lo", True, (150.1,)), ("HIGH, RANGE in", "hi", True, (75.1, 1502.0)), ("CONSTANT, RANGE in", None, True, ())]]

fig, axes = plt.subplots(2, 3, figsize=(11.5, 6.8), sharex=True, sharey=True)
for r, row in enumerate(PANELS):
    for c, (title, mode, wide, refs) in enumerate(row):
        ax = axes[r, c]
        ax.axhline(0, color="#222222", lw=1.5, zorder=2)
        top = 180.0 if wide else 90.0
        for knob, colour in zip(KNOBS, COLOURS):
            theta = knob * top
            if mode is None:
                y = np.full_like(f, wrapped([theta, theta])[0])  # a true rotation: the same angle at every frequency
            else:
                y = wrapped(hilo.lag_oversampled(mode, theta, f, FS, wide))
            ax.semilogx(f, y, color=colour, lw=1.8, label=f"{int(knob * 100)}%  ({theta:g}°)", zorder=3)
        for ref in refs:
            ax.axvline(ref, color="#999999", lw=0.8, ls=":")
        ax.set_title(title, fontsize=11)
        ax.set_ylim(-195, 195)
        ax.set_yticks(range(-180, 181, 90))
        ax.set_yticklabels([f"{v}°" for v in range(-180, 181, 90)])
        ax.set_xlim(20, 20000)
        ax.set_xticks([20, 100, 1000, 10000])
        ax.set_xticklabels(["20", "100", "1k", "10k"])
        ax.minorticks_off()
        ax.grid(True, alpha=0.25)
        if c == 0:
            ax.set_ylabel("phase, output against input")
        if r == 1:
            ax.set_xlabel("frequency (Hz)")
        # the empty corner of each panel: the wrapped curves fill the top right and the bottom left of RANGE in
        where = dict(loc="upper right") if not wide else dict(loc="center right", bbox_to_anchor=(1, 0.74)) if mode is None else dict(loc="lower right")
        ax.legend(fontsize=7.5, title="knob (panel angle)", title_fontsize=7.5, framealpha=0.95, **where)
axes[0, 0].text(22, 6, "0°: no change", fontsize=8, color="#222222", va="bottom")
fig.suptitle("Phase of the output against the input, as an analyser shows it, at 25, 50, 75 and 100% of the knob "
             "(dotted: the sections' reference frequencies)", fontsize=10.5)
fig.text(0.5, 0.005, "A lag is negative phase. Where a curve reaches ±180° it wraps round (the turn itself is continuous). "
         "Ø (polarity flip) is a flat 180°, the top and bottom edges.", ha="center", fontsize=8, color="#555555")
fig.tight_layout(rect=(0, 0.025, 1, 0.96))
OUT.parent.mkdir(parents=True, exist_ok=True)
fig.savefig(OUT, dpi=120)
print(OUT)
