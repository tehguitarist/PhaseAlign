"""The README's figure of the phase modes: lag against frequency for each mode and range, at 25, 50, 75 and 100% of the
knob's travel, from the golden-tested reference (prototype/hilo.py, as built: the oversampled sections at 48 kHz).

    .venv/bin/python prototype/modes_figure.py       # → docs/images/phase-modes.png
"""

from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

import hilo

OUT = Path(__file__).resolve().parent.parent / "docs" / "images" / "phase-modes.png"
FS = 48000
KNOBS = (0.25, 0.5, 0.75, 1.0)
COLOURS = ("#9bc2e6", "#5b9bd5", "#2e75b6", "#0b3d6e")
f = np.geomspace(20, 20000, 500)

# (title, mode, wide, reference frequencies to mark)
PANELS = [[("LOW, RANGE out", "lo", False, (75.1,)), ("HIGH, RANGE out", "hi", False, (150.1,)), ("CONSTANT, RANGE out", None, False, ())],
          [("LOW, RANGE in", "lo", True, (150.1,)), ("HIGH, RANGE in", "hi", True, (75.1, 1502.0)), ("CONSTANT, RANGE in", None, True, ())]]

fig, axes = plt.subplots(2, 3, figsize=(11.5, 6.6), sharex=True, sharey="row")
for r, row in enumerate(PANELS):
    for c, (title, mode, wide, refs) in enumerate(row):
        ax = axes[r, c]
        top = 180.0 if wide else 90.0
        for knob, colour in zip(KNOBS, COLOURS):
            theta = knob * top
            if mode is None:
                y = np.full_like(f, theta)  # a true rotation: the same angle at every frequency
            else:
                y = hilo.lag_oversampled(mode, theta, f, FS, wide)
            ax.semilogx(f, y, color=colour, lw=1.8, label=f"{int(knob * 100)}%  ({theta:g}°)")
        for ref in refs:
            ax.axvline(ref, color="#999999", lw=0.8, ls=":")
        ax.set_title(title, fontsize=11)
        ax.set_ylim(0, 380 if wide else 200)
        ax.set_yticks(range(0, 381 if wide else 201, 90 if wide else 45) if False else (list(range(0, 361, 90)) if wide else list(range(0, 181, 45))))
        ax.grid(True, which="both", alpha=0.25)
        if c == 0:
            ax.set_ylabel("phase turned (°)")
        if r == 1:
            ax.set_xlabel("frequency (Hz)")
        ax.set_xticks([20, 100, 1000, 10000])
        ax.set_xticklabels(["20", "100", "1k", "10k"])
        ax.legend(fontsize=7.5, title="knob (panel angle)", title_fontsize=7.5, loc="upper left" if mode is not None else "upper right")
fig.suptitle("Phase turned by each mode and range, at 25, 50, 75 and 100% of the knob (dotted: the sections' reference frequencies)",
             fontsize=10.5)
fig.tight_layout(rect=(0, 0, 1, 0.96))
OUT.parent.mkdir(parents=True, exist_ok=True)
fig.savefig(OUT, dpi=120)
print(OUT)
