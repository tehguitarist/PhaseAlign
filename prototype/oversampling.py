"""Oversampling for Hi/Lo (IMPLEMENTATION_PLAN 2.3, de-cramping; user, 2026-10-06), and the C++ tables.

    .venv/bin/python prototype/oversampling.py      # → src/dsp/HalfbandTables.h, and prints each class's spec

A zero-latency all-pass can't be de-cramped: for any stable all-pass, tan(lag / 2) is a reactance function of the
warped frequency tan(pi f / fs), so by Foster's reactance theorem tan(lag / 2) / tan(pi f / fs) never decreases, and
the bilinear first-order section already has the least lag above its reference frequency that any all-pass with the
same lag there can have. So Hi/Lo runs its sections at M times the rate between linear-phase halfband filters, which
add a pure delay (reported as latency) and no phase: 4x below 85 kHz, 2x below 170 kHz, none from there up. At 4x the
sections are within 2.5 degrees of analog ones to 20 kHz.

Each 2x stage is a halfband FIR h of length N = 4K - 1: the even taps are dense, the centre tap (index 2K - 1) is 0.5
and the other odd taps are 0. Up: zero-stuff, filter by 2h. Down: filter by h, keep the even samples. Designed by the
one-band trick (a type II FIR g of length 2K with remez on [0, 2 fp]; h[2j] = g[j] / 2), so the zeros are exact.
Coefficients are rounded to float32, which the C++ uses, and the reference uses the same rounded values.

Latency (base-rate samples). A 2x stage delays by (N - 1) / 2 at its own rate both ways, so (N - 1) / 2 base samples
for the outer stage. The inner stage of 4x adds (N - 1) / 4, a half-integer, so the 2x stream is delayed by one more
sample on the way down, making it a whole number: L = (N1 - 1) / 2 + K2.
"""

from pathlib import Path

import numpy as np
from scipy.signal import freqz, lfilter, remez

ROOT = Path(__file__).resolve().parent.parent
DEST = ROOT / "src" / "dsp" / "HalfbandTables.h"
RIPPLE_DB = 0.03  # per stage, both ways: at most twice this end to end


def halfband(K, fp):
    """Length 4K - 1 halfband with passband edge fp (a fraction of its own rate, below 0.25), float32-rounded."""
    g = remez(2 * K, [0, 2 * fp], [1], fs=1.0)
    g = (g + g[::-1]) / 2  # exactly symmetric, so the C++ can add each pair of samples before multiplying
    h = np.zeros(4 * K - 1)
    h[0::2] = g / 2
    h[2 * K - 1] = 0.5
    return h.astype(np.float32).astype(np.float64)


# name: (K, passband edge as a fraction of the stage's output rate), each the shortest within RIPPLE_DB.
#   narrow44: 4x stage 1 below 47 kHz (passband 20 kHz at 44.1 kHz, so 0.4535 x the base rate's Nyquist)
#   narrow48: 4x stage 1 from 47 kHz (passband 20 kHz at 48 kHz)
#   wide: 4x stage 2 (passband the base Nyquist) and the single stage of 2x (passband 20 kHz at 85 kHz and up)
DESIGNS = {"narrow44": (15, 20000 / 88200), "narrow48": (8, 20000 / 96000), "wide": (3, 0.125)}
HALFBANDS = {name: halfband(K, fp) for name, (K, fp) in DESIGNS.items()}


def plan(fs):
    """fs -> (factor M, stage names outermost first, latency in base samples)."""
    if fs >= 170000:
        return 1, [], 0
    if fs >= 85000:
        return 2, ["wide"], (len(HALFBANDS["wide"]) - 1) // 2
    outer = "narrow44" if fs < 47000 else "narrow48"
    return 4, [outer, "wide"], (len(HALFBANDS[outer]) - 1) // 2 + (len(HALFBANDS["wide"]) + 1) // 4


class Oversampler:
    """Streaming up/down by plan(fs) for (channels, n) arrays; the reference for src/dsp/Oversampler.h. Call up()
    before down() for each block."""

    def __init__(self, fs):
        self.factor, names, self.latency = plan(fs)
        self.up_h = [2 * HALFBANDS[n] for n in names]
        # Down, innermost first; the outer stage of 4x takes the one extra sample of delay (module docstring).
        self.down_h = [HALFBANDS[n] for n in reversed(names)]
        if len(names) == 2:
            self.down_h[1] = np.concatenate([[0.0], self.down_h[1]])
        self.up_zi = self.down_zi = None  # sized by the first block's channels

    def up(self, x):
        if self.up_zi is None:
            self.up_zi = [np.zeros((x.shape[0], len(h) - 1)) for h in self.up_h]
            self.down_zi = [np.zeros((x.shape[0], len(h) - 1)) for h in self.down_h]
        for i, h in enumerate(self.up_h):
            s = np.zeros((x.shape[0], 2 * x.shape[1]))
            s[:, 0::2] = x
            x, self.up_zi[i] = lfilter(h, 1.0, s, axis=-1, zi=self.up_zi[i])
        return x

    def down(self, y):
        for i, h in enumerate(self.down_h):
            v, self.down_zi[i] = lfilter(h, 1.0, y, axis=-1, zi=self.down_zi[i])
            y = v[:, 0::2]
        return y


def response(fs, f):
    """The up/down pair's level (linear) at f, without the delay."""
    _, names, _ = plan(fs)
    g = np.ones_like(np.asarray(f, dtype=float))
    rate = fs
    for n in names:
        rate *= 2
        _, H = freqz(HALFBANDS[n], worN=np.asarray(f, dtype=float), fs=rate)
        g = g * np.abs(H) ** 2
    return g


def main():
    for name, (K, fp) in DESIGNS.items():
        h = HALFBANDS[name]
        w, H = freqz(h, worN=1 << 15, fs=1.0)
        pb = 20 * np.log10(np.abs(H[w <= fp]))
        sb = 20 * np.log10(np.abs(H[w >= 0.5 - fp]).max())
        print(f"{name}: N = {len(h)}, passband {fp:.4f}, ripple {pb.min():+.4f}/{pb.max():+.4f} dB, "
              f"stopband {sb:.1f} dB")
    for fs in (44100, 48000, 88200, 96000, 176400, 192000):
        M, names, L = plan(fs)
        f = np.geomspace(20, 20000, 400)
        lvl = 20 * np.log10(response(fs, f))
        print(f"{fs}: {M}x {names}, latency {L} ({L / fs * 1e3:.2f} ms), level 20 Hz-20 kHz "
              f"{lvl.min():+.4f}/{lvl.max():+.4f} dB")

    lines = ["#pragma once", "",
             "// Generated by prototype/oversampling.py; don't edit by hand. Hi/Lo's oversampling halfbands (plan 2.3):",
             "// each a length 4K - 1 FIR whose centre tap (2K - 1) is 0.5, other odd taps 0, and even taps listed here",
             "// (h[0], h[2], ..., h[4K - 2]), float32, exactly symmetric.", "// clang-format off",
             "namespace pa::dsp::halfband_tables", "{"]
    for name, h in HALFBANDS.items():
        even = h[0::2]
        lines.append(f"inline constexpr float {name}[{len(even)}] = {{")
        for i in range(0, len(even), 4):
            lines.append("    " + " ".join(f"{np.float32(v)!r}f," for v in even[i:i + 4]))
        lines.append("};")
    lines += ["} // namespace pa::dsp::halfband_tables", "// clang-format on", ""]
    DEST.write_text("\n".join(lines).replace("np.float32(", "").replace(")f,", "f,"))
    print(f"wrote {DEST.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
