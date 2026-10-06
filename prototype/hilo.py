"""P1: Hi/Lo phase modes (IMPLEMENTATION_PLAN 2.3), with the range-aware knob mapping (2026-10-06).

    .venv/bin/python prototype/hilo.py        # P1 checks and plots → prototype/out/p1/

Two first-order all-pass sections in series, each parametrised by k = 1/tan(pi*fc/fs). The phase lag of one
section at f is 2*atan(k*tan(pi*f/fs)); k -> 0 is identity.

The knob's travel sets phi, the angle of section 1 (its lag at its reference f1), from 0 to 90 degrees. The RANGE
button (`wide`) selects the 90 range (one section, theta = phi) or the 180 range (two sections, theta2 = phi, so the
panel angle theta = 2 phi runs 0-180). k_i = tan(theta_i / 2) / tan(pi * f_i / fs), so the mapping is in Hz and is the
same at every sample rate.

Each (mode, range) combination is its own "shape" with its own section references, taken from the captures of the
reference unit (75.1 Hz, 150.1 Hz and 20 x 75.1 = 1502 Hz; nothing invented). The modes are named as the manual
describes the 180 range: HIGH is the wide setting (one section on the lows, one on the highs), LOW the narrow one (both
sections on the lows, stacked):

  | | 90 range | 180 range |
  |---|---|---|
  | LOW | one section at 75.1 Hz | two stacked sections at 150.1 Hz |
  | HIGH | one section at 150.1 Hz | 75.1 Hz and 1502 Hz |

Toggling RANGE or the mode changes shape; the k of each section glides geometrically to the new shape's value (R3), from
wherever it is when the change arrives (the glide state is a weight on each of the four shapes, so a change in the
middle of a glide, to any shape, still never steps), so there is no step. The panel angle: LOW 180 reads exactly 0-180 (the stacked pair's lag at 150.1 Hz);
HIGH 180 shows phi (0-90) with an asterisk, because its lag at 75.1 Hz only reaches about 96 degrees.

More knob or a wider range never means less phase at any frequency (both thetas only grow).
"""

import math
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from scipy.signal import lfilter

# k is never below this: lag at 20 kHz under 0.25° at 44.1 kHz (plan 2.3). At k = 0 the pole would sit on z = -1.
K_MIN = math.tan(math.radians(0.125)) / math.tan(math.pi * 20000 / 44100)


@dataclass(frozen=True)
class Mode:
    f1: float  # section 1 reference: theta1 is its lag here (Hz)
    f2: float  # section 2 reference (Hz)


SHAPE_ORDER = [("lo", False), ("lo", True), ("hi", False), ("hi", True)]
SHAPES = {  # (mode, wide) -> section references
    ("lo", False): Mode(f1=75.1, f2=75.1),
    ("lo", True): Mode(f1=150.1, f2=150.1),
    ("hi", False): Mode(f1=150.1, f2=150.1),
    ("hi", True): Mode(f1=75.1, f2=20.0 * 75.1),
}


def knob_angles(theta, wide):
    """Panel angle theta (0-90 in the 90 range, 0-180 in the 180 range) -> (phi, w): section 1's angle and the
    weight of section 2 (the targets the processor ramps)."""
    theta = min(max(theta, 0.0), 180.0 if wide else 90.0)
    return (theta / 2.0, 1.0) if wide else (theta, 0.0)


def split(mode, theta, wide):
    """theta (degrees) -> (theta1, theta2): section 1's lag at f1 and section 2's lag at f2."""
    phi, w = knob_angles(theta, wide)
    return phi, w * phi


def angles(shape, phi):
    """A shape's section angles at phi: section 2 carries the same angle in the 180 range and none in the 90 range."""
    return phi, (phi if shape[1] else 0.0)


def k_angles(shape, t1, t2, fs):
    m = SHAPES[shape]
    return (max(math.tan(math.radians(t1) / 2) / math.tan(math.pi * m.f1 / fs), K_MIN),
            max(math.tan(math.radians(t2) / 2) / math.tan(math.pi * m.f2 / fs), K_MIN))


def k_pair(mode, theta, fs, wide):
    return k_angles((mode, wide), *split(mode, theta, wide), fs)


def section_lag(k, f, fs):
    return 2 * np.degrees(np.arctan(k * np.tan(np.pi * np.asarray(f) / fs)))


def lag(mode, theta, f, fs, wide):
    """Phase lag in degrees of the TPT cascade at frequencies f."""
    k1, k2 = k_pair(mode, theta, fs, wide)
    return section_lag(k1, f, fs) + section_lag(k2, f, fs)


def analog_lag(mode, theta, f, wide):
    """The same mapping as an analog cascade (what the digital one approaches at high rates)."""
    m = SHAPES[(mode, wide)]
    out = np.zeros_like(np.asarray(f, dtype=float))
    for t, fr in zip(split(mode, theta, wide), (m.f1, m.f2)):
        out += 2 * np.degrees(np.arctan(math.tan(math.radians(t) / 2) * np.asarray(f) / fr))
    return out


# ----------------------------------------------------------------------------------------------------------------
# Sample processing (the reference for the C++ port and the golden tests)
# ----------------------------------------------------------------------------------------------------------------

class HiLo:
    """Stereo-linked Hi/Lo cascade with angle smoothing and the Hi<->Lo k-glide (R3). The reference for
    src/dsp/AllpassCascade.h (golden-tested, tests/golden/).

    Each section is the all-pass of a TPT one-pole with G = 1/(1 + k): H(z) = (-p + z^-1)/(1 - p z^-1), p = 1 - 2G,
    run in direct form I (y = -p x + x1 + p y1; the state is the last input and output). Until 2026-10-06 it ran as the
    TPT structure, whose state at k = kMin holds a near-lossless Nyquist resonance that came out as a burst when the
    knob left 0 or 90 degrees; the transfer function is the same, so static settings are unchanged.

    The panel angle theta and the range (`wide`) set the target of phi (section 1's angle), which moves on a linear
    ramp of fixed length; the shape (mode, wide) glides as described above, so a knob move, a mode switch or a RANGE
    toggle never steps (and the knob keeps its position through a RANGE toggle: phi is unchanged).

    Coefficients run on a fixed grid of `sub` samples: at the start of each cell the ramp and the glide
    move on by one cell, and each section's G is interpolated linearly per sample from its value
    before the move to its value after it, so a corner sweeping down from near Nyquist doesn't step.

    The glide is a weight on each of the four shapes (summing to 1). A shape change sets the target weights to that
    shape alone and moves every weight to its target on a linear ramp of fixed length (glide_ms) from where it is, and each
    section's k is exp(sum of weight x ln k of that shape at the current phi). So a change that arrives mid-glide, back
    to the previous shape or on to a third one, continues smoothly from the current blend (until 2026-10-06 the glide
    was between just two shapes: a third shape mid-glide jumped to the previous shape's k, a click).
    """

    def __init__(self, fs, mode="hi", theta=0.0, wide=False, smooth_ms=30.0, glide_ms=30.0, sub=32):
        self.fs, self.sub = fs, sub
        self.panel = float(theta)
        self.shape = (mode, bool(wide))
        self.phi = self.phi_target = knob_angles(self.panel, bool(wide))[0]
        self.phi_step = 0.0
        self.smooth_n = max(1, round(smooth_ms * 1e-3 * fs))
        self.glide_n = max(1, round(glide_ms * 1e-3 * fs))
        self.w = [1.0 if sh == self.shape else 0.0 for sh in SHAPE_ORDER]  # glide weights, one per shape
        self.w_target = list(self.w)
        self.w_step = [0.0] * len(SHAPE_ORDER)
        self.x1 = None  # per section: last input and last output (direct form I)
        self.y1 = None
        self.until = 0  # samples left in the current cell
        self.g0 = self.g1 = None

    def set(self, theta=None, mode=None, wide=None):
        if theta is not None:
            self.panel = float(theta)
        shape = (self.shape[0] if mode is None else mode, self.shape[1] if wide is None else bool(wide))
        if theta is not None or shape != self.shape:
            phi_t = knob_angles(self.panel, shape[1])[0]
            if phi_t != self.phi_target:
                self.phi_target = phi_t
                self.phi_step = (phi_t - self.phi) / self.smooth_n  # a linear ramp of fixed length
        if shape != self.shape:
            self.shape = shape
            self.w_target = [1.0 if sh == shape else 0.0 for sh in SHAPE_ORDER]
            self.w_step = [(t - w) / self.glide_n for t, w in zip(self.w_target, self.w)]  # linear ramps of fixed length

    def _settled(self):
        return self.phi == self.phi_target and self.w == self.w_target

    def _g(self):
        ln1 = ln2 = 0.0
        for wi, sh in zip(self.w, SHAPE_ORDER):
            if wi != 0.0:
                a1, a2 = k_angles(sh, *angles(sh, self.phi), self.fs)
                ln1 += wi * math.log(a1)
                ln2 += wi * math.log(a2)
        return np.array([1.0 / (1.0 + math.exp(ln1)), 1.0 / (1.0 + math.exp(ln2))])

    def _advance(self):
        if self.phi != self.phi_target:
            self.phi += self.phi_step * self.sub
            if (self.phi_step > 0) == (self.phi >= self.phi_target):
                self.phi = self.phi_target
        for i, (step, target) in enumerate(zip(self.w_step, self.w_target)):
            if self.w[i] != target:
                self.w[i] += step * self.sub
                if (step > 0) == (self.w[i] >= target):
                    self.w[i] = target

    def process(self, x):
        x = np.atleast_2d(np.asarray(x, dtype=np.float64))
        if self.x1 is None:
            self.x1 = np.zeros((2, x.shape[0]))
            self.y1 = np.zeros((2, x.shape[0]))
        y = np.empty_like(x)
        n = x.shape[1]
        i = 0
        while i < n:
            if self.until == 0:  # a new cell
                self.g0 = self._g()
                if not self._settled():
                    self._advance()
                    self.g1 = self._g()
                else:
                    self.g1 = self.g0
                for v in (self.x1, self.y1):
                    v[np.abs(v) < 1e-20] = 0.0
                self.until = self.sub
            m = min(n - i, self.until)
            done = self.sub - self.until
            seg = x[:, i:i + m].copy()
            if np.array_equal(self.g0, self.g1):  # static: a plain first-order filter per section
                for k in range(2):
                    p = 1.0 - 2.0 * self.g0[k]
                    zi = (self.x1[k] + p * self.y1[k])[:, None]  # transposed form's state from the last x and y
                    out = lfilter([-p, 1.0], [1.0, -p], seg, axis=-1, zi=zi)[0]
                    self.x1[k], self.y1[k] = seg[:, -1], out[:, -1]
                    seg = out
            else:
                for j in range(m):
                    t = (done + j + 1) / self.sub
                    v_in = seg[:, j]
                    for k in range(2):
                        G = self.g0[k] + (self.g1[k] - self.g0[k]) * t
                        p = 1.0 - 2.0 * G
                        out = self.x1[k] - p * v_in + p * self.y1[k]
                        self.x1[k], self.y1[k] = v_in, out
                        v_in = out
                    seg[:, j] = v_in
            y[:, i:i + m] = seg
            self.until -= m
            i += m
        return y


class HiLoOversampled:
    """Hi/Lo as built since the de-cramping (2026-10-06): HiLo at M times the rate between the halfbands of
    prototype/oversampling.py, so the sections are within 2.5 degrees of analog ones to 20 kHz at every rate, for a
    latency of `latency` base samples. The coefficient grid stays 32 base samples long (32 M at the oversampled rate),
    so the angle and glide move as they did before. The reference for src/dsp/HiLoStage.h (golden-tested)."""

    def __init__(self, fs, mode="hi", theta=0.0, wide=False):
        from oversampling import Oversampler

        self.os = Oversampler(fs)
        self.factor, self.latency = self.os.factor, self.os.latency
        self.core = HiLo(fs * self.factor, mode, theta, wide, sub=32 * self.factor)

    def set(self, theta=None, mode=None, wide=None):
        self.core.set(theta=theta, mode=mode, wide=wide)

    def process(self, x):
        x = np.atleast_2d(np.asarray(x, dtype=np.float64))
        return self.os.down(self.core.process(self.os.up(x)))


def lag_oversampled(mode, theta, f, fs, wide):
    """Lag in degrees of HiLoOversampled at f, without its latency: the sections' at the oversampled rate (the
    halfbands are linear phase, so they add only the delay)."""
    from oversampling import plan

    return lag(mode, theta, f, fs * plan(fs)[0], wide)


# ----------------------------------------------------------------------------------------------------------------
# P1 checks
# ----------------------------------------------------------------------------------------------------------------

RATES = (44100, 48000, 96000, 192000)


MODE_NAMES = ("hi", "lo")


def checks(out, plt):
    """P1 checks; returns report lines."""
    rep = ["Section references (Hz), by mode and range: " + "; ".join(
        f"{n.upper()} {'180' if w else '90'}: {SHAPES[(n, w)].f1} and {SHAPES[(n, w)].f2:.0f}"
        for n in MODE_NAMES for w in (False, True)) + f". K_MIN = {K_MIN:.3g}.\n"]
    ranges = (("90", False, np.arange(0, 91, 15)), ("180", True, np.arange(0, 181, 30)))

    # 1. Phase vs frequency at every rate.
    for mode in MODE_NAMES:
        for rname, wide, thetas in ranges:
            fig, axes = plt.subplots(2, 2, figsize=(12, 8), sharey=True)
            for ax, fs in zip(axes.flat, RATES):
                f = np.geomspace(10, 0.499 * fs, 500)
                for t in thetas:
                    ax.semilogx(f, lag(mode, t, f, fs, wide), lw=1.2, label=f"{t}°")
                    ax.semilogx(f, analog_lag(mode, t, f, wide), "k:", lw=0.5)
                ax.axvline(20000, color="k", lw=0.5)
                ax.set_title(f"{mode} range {rname} @ {fs} Hz (dotted: analog)")
                ax.grid(True, which="both", alpha=0.3)
            axes[0, 0].legend(fontsize=6, ncol=2)
            for ax in axes[:, 0]:
                ax.set_ylabel("phase lag (°)")
            fig.tight_layout()
            fig.savefig(out / f"phase_{mode}_{rname}.png", dpi=110)
            plt.close(fig)

    # 2. Entering from 0°: the steepest change in lag per 0.1° of panel angle (the top octave moves fast there).
    rep.append("### Steepest lag change per 0.1° of panel angle, 10 Hz–20 kHz\n")
    rep.append("| mode | range | rate | θ 0–2° | whole travel |")
    rep.append("|---|---|---|---|---|")

    def steepest(mode, wide, f, fs, t0, t1):
        return max(np.max(np.abs(lag(mode, t + 0.1, f, fs, wide) - lag(mode, t, f, fs, wide)))
                   for t in np.arange(t0, t1, 0.1))

    for mode in MODE_NAMES:
        for rname, wide, thetas in ranges:
            for fs in RATES:
                f = np.geomspace(10, 20000, 800)
                top = float(thetas[-1])
                rep.append(f"| {mode} | {rname} | {fs} | {steepest(mode, wide, f, fs, 0.0, 2.0):.1f}° | "
                           f"{steepest(mode, wide, f, fs, 0.0, top - 0.1):.1f}° |")

    # 3. Readout: the true lag at the first reference against the panel angle.
    rep.append("\n### Readout: true lag at the first section's reference vs the panel angle\n")
    rep.append("| mode | range | at θ = max | worst error over travel, all rates |")
    rep.append("|---|---|---|---|")
    for mode in MODE_NAMES:
        for rname, wide, thetas in ranges:
            m = SHAPES[(mode, wide)]
            worst, atmax = 0.0, 0.0
            for fs in RATES:
                for t in np.arange(0, float(thetas[-1]) + 0.01, 0.5):
                    e = abs(lag(mode, t, np.array([m.f1]), fs, wide)[0] - t)
                    worst = max(worst, float(e))
                atmax = float(lag(mode, float(thetas[-1]), np.array([m.f1]), 48000, wide)[0])
            rep.append(f"| {mode} | {rname} | {atmax:.1f}° | {worst:.1f}° |")
    rep.append("\nWhere the error is large the panel shows the first section's angle phi (with an asterisk), "
               "not the lag at the first reference; the stacked pair reads exactly.")

    # 4. Monotonic: more knob, or the wider range, never means less phase at any frequency.
    rep.append("\n### Monotonic\n")
    fa = np.geomspace(1, 1e6, 3000)
    for mode in MODE_NAMES:
        for rname, wide, thetas in ranges:
            P = np.array([analog_lag(mode, t, fa, wide) for t in np.arange(0, float(thetas[-1]) + 0.001, 0.05)])
            rep.append(f"- {mode} range {rname}: smallest lag change per 0.05° step over 1 Hz–1 MHz (analog, so it "
                       f"holds at every rate): {np.min(np.diff(P, axis=0)):+.5f}°")
        # Each range is its own shape; the 180 range is not guaranteed to have more lag at every frequency than the 90
        # range at the same knob position (LOW 180's stacked pair sits at 150 Hz, LOW 90's single section at 75 Hz).
        d = min(np.min(analog_lag(mode, 2 * kn, fa, True) - analog_lag(mode, kn, fa, False)) for kn in (10, 45, 90))
        rep.append(f"- {mode}: same knob position, range 180 minus range 90, smallest over 1 Hz–1 MHz: {d:+.2f}° "
                   f"(a shape change, smoothed by the glide)")

    # 5. The sample processor equals the closed form (static), and glides and range toggles stay all-pass.
    rep.append("\n### Sample processor\n")
    fs = 48000
    for mode, t, wide in (("hi", 37.0, False), ("hi", 140.0, True), ("lo", 110.0, True), ("lo", 60.0, False)):
        p = HiLo(fs, mode, t, wide)
        n = 1 << 16
        imp = np.zeros(n)
        imp[0] = 1.0
        h = p.process(imp)[0]
        H = np.fft.rfft(h)
        f = np.fft.rfftfreq(n, 1 / fs)
        sel = (f > 10) & (f < 0.45 * fs)
        meas = -np.degrees(np.unwrap(np.angle(H)))[sel]
        rep.append(f"- {mode} θ={t:.0f}° range {'180' if wide else '90'}: impulse response vs closed form: max phase "
                   f"error {np.max(np.abs(meas - lag(mode, t, f[sel], fs, wide))):.2e}°, magnitude "
                   f"{np.max(np.abs(20 * np.log10(np.abs(H[sel])))):.2e} dB")
    rng = np.random.default_rng(0)
    x = rng.standard_normal(fs)

    def power(y, x_):
        return np.sqrt(np.mean(y ** 2) / np.mean(x_ ** 2))

    p = HiLo(fs, "hi", 120.0, True)
    p.process(x[: fs // 2])
    p.set(mode="lo")
    y = p.process(x[fs // 2:])
    rep.append(f"- Hi→Lo glide at θ=120° (range 180) on white noise: output/input RMS over the glide "
               f"{power(y[0, :2048], x[fs // 2: fs // 2 + 2048]):.4f}")
    for mode, wide0, theta0 in (("lo", True, 160.0), ("hi", False, 80.0), ("hi", True, 170.0)):
        p = HiLo(fs, mode, theta0, wide0)
        p.process(x[: fs // 2])
        p.set(wide=not wide0, theta=theta0 / 2 if wide0 else theta0 * 2)  # the knob keeps its position
        y = p.process(x[fs // 2:])
        rep.append(f"- {mode.upper()} RANGE {'180→90' if wide0 else '90→180'} toggle at knob {theta0 / (180 if wide0 else 90):.2f} "
                   f"on white noise: output/input RMS over the glide {power(y[0, :2048], x[fs // 2: fs // 2 + 2048]):.4f}; "
                   f"largest sample-to-sample step vs the same noise unprocessed "
                   f"{np.max(np.abs(np.diff(y[0]))) / np.max(np.abs(np.diff(x[fs // 2:]))):.2f}x")
    return rep


def main():
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    out = Path(__file__).resolve().parent / "out" / "p1"
    out.mkdir(parents=True, exist_ok=True)
    rep = ["# P1: Hi/Lo prototype checks\n", "Generated by `prototype/hilo.py`.\n"]
    rep += checks(out, plt)
    (out / "report.md").write_text("\n".join(rep) + "\n")
    print("\n".join(rep))


if __name__ == "__main__":
    main()
