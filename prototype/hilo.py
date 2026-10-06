"""P1: Hi/Lo phase modes (IMPLEMENTATION_PLAN 2.3), with the P5 knob mapping.

    .venv/bin/python prototype/hilo.py        # P1 checks and plots → prototype/out/p1/

Two first-order all-pass sections in series, each parametrised by k = 1/tan(pi*fc/fs). The phase lag of one
section at f is 2*atan(k*tan(pi*f/fs)); k -> 0 is identity.

The knob angle theta (0-180°) is split into theta1 (section 1's lag at f1) and theta2 (section 2's lag at f2), so
k_i = tan(theta_i/2) / tan(pi*f_i/fs). The mapping is defined in Hz, so it is the same at every sample rate.

The mapping on one 0-180° knob:
  * theta 0-90: one section, its corner swept down to f1 (150.1 Hz Hi, 75.1 Hz Lo) at 90°, from identity at 0°.
  * Hi, theta 90-180: glides from "section 1 at f1" into two ganged sections at the same corner (HI_BLEND_END
    onwards). The readout is the true lag at 150.1 Hz.
  * Lo, theta 90-180: holds section 1 at f1 and sweeps section 2 down to f2 = 20 x 75.1 Hz, so more angle never means
    less phase at any frequency.
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
    f1: float               # section 1 reference: theta1 is its lag here (Hz)
    f2: float               # section 2 reference (Hz)
    blend_end: float | None  # Hi only: theta where the split reaches the ganged pair (theta1 = theta2)


HI_BLEND_END = 125.0  # smallest round value that keeps the lag non-decreasing in theta at every frequency (P1 check 3)
MODES = {
    "hi": Mode(f1=150.1, f2=150.1, blend_end=HI_BLEND_END),
    "lo": Mode(f1=75.1, f2=20.0 * 75.1, blend_end=None),
}


def split(mode, theta):
    """theta (degrees) -> (theta1, theta2): section 1's lag at f1 and section 2's lag at f2."""
    m = MODES[mode]
    theta = min(max(theta, 0.0), 180.0)
    if theta <= 90.0:
        return theta, 0.0
    if m.blend_end is None:
        return 90.0, theta - 90.0
    s = min((theta - 90.0) / (m.blend_end - 90.0), 1.0)
    s = s * s * (3.0 - 2.0 * s)
    t1 = 90.0 - s * (90.0 - theta / 2.0)
    return t1, theta - t1


def k_pair(mode, theta, fs):
    m = MODES[mode]
    t1, t2 = split(mode, theta)
    return (max(math.tan(math.radians(t1) / 2) / math.tan(math.pi * m.f1 / fs), K_MIN),
            max(math.tan(math.radians(t2) / 2) / math.tan(math.pi * m.f2 / fs), K_MIN))


def section_lag(k, f, fs):
    return 2 * np.degrees(np.arctan(k * np.tan(np.pi * np.asarray(f) / fs)))


def lag(mode, theta, f, fs):
    """Phase lag in degrees of the TPT cascade at frequencies f."""
    k1, k2 = k_pair(mode, theta, fs)
    return section_lag(k1, f, fs) + section_lag(k2, f, fs)


def analog_lag(mode, theta, f):
    """The same mapping as an analog cascade (what the digital one approaches at high rates)."""
    m = MODES[mode]
    out = np.zeros_like(np.asarray(f, dtype=float))
    for t, fr in zip(split(mode, theta), (m.f1, m.f2)):
        out += 2 * np.degrees(np.arctan(math.tan(math.radians(t) / 2) * np.asarray(f) / fr))
    return out


# ----------------------------------------------------------------------------------------------------------------
# Sample processing (the reference for the C++ port and the golden tests)
# ----------------------------------------------------------------------------------------------------------------

class HiLo:
    """Stereo-linked Hi/Lo cascade with angle smoothing and the Hi<->Lo k-glide (R3). The reference for
    src/dsp/AllpassCascade.h (golden-tested, tests/golden/).

    TPT section: v = (x - s)*G, lp = v + s, s' = lp + v, y = 2*lp - x, with G = 1/(1 + k).

    Coefficients run on a fixed grid of `sub` samples: at the start of each cell the angle (a linear ramp of fixed
    length) and the glide move on by one cell, and each section's G is interpolated linearly per sample from its value
    before the move to its value after it, so a corner sweeping down from near Nyquist doesn't step. Switching back to
    the previous mode during a glide reverses it from where it is.
    """

    def __init__(self, fs, mode="hi", theta=0.0, smooth_ms=30.0, glide_ms=30.0, sub=32):
        self.fs, self.sub = fs, sub
        self.mode, self.prev_mode = mode, mode
        self.theta = self.target = float(theta)
        self.step = 0.0
        self.smooth_n = max(1, round(smooth_ms * 1e-3 * fs))
        self.glide_n = max(1, round(glide_ms * 1e-3 * fs))
        self.glide = 1.0  # 0 → previous mode's k, 1 → current mode's k
        self.s = None
        self.until = 0  # samples left in the current cell
        self.g0 = self.g1 = None

    def set(self, theta=None, mode=None):
        if theta is not None and float(theta) != self.target:
            self.target = float(theta)
            self.step = (self.target - self.theta) / self.smooth_n  # linear ramp of fixed length
        if mode is not None and mode != self.mode:
            if self.glide < 1.0 and mode == self.prev_mode:  # back again mid-glide: reverse from where it is
                self.prev_mode, self.mode, self.glide = self.mode, self.prev_mode, 1.0 - self.glide
            else:
                self.prev_mode, self.mode, self.glide = self.mode, mode, 0.0

    def _settled(self):
        return self.theta == self.target and self.glide >= 1.0

    def _g(self):
        k1, k2 = k_pair(self.mode, self.theta, self.fs)
        if self.glide < 1.0:
            o1, o2 = k_pair(self.prev_mode, self.theta, self.fs)
            k1 = math.exp((1.0 - self.glide) * math.log(o1) + self.glide * math.log(k1))
            k2 = math.exp((1.0 - self.glide) * math.log(o2) + self.glide * math.log(k2))
        return np.array([1.0 / (1.0 + k1), 1.0 / (1.0 + k2)])

    def _advance(self):
        if self.theta != self.target:
            self.theta += self.step * self.sub
            if (self.step > 0) == (self.theta >= self.target):
                self.theta = self.target
        if self.glide < 1.0:
            self.glide = min(1.0, self.glide + self.sub / self.glide_n)

    def process(self, x):
        x = np.atleast_2d(np.asarray(x, dtype=np.float64))
        if self.s is None:
            self.s = np.zeros((2, x.shape[0]))
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
                self.s[np.abs(self.s) < 1e-20] = 0.0
                self.until = self.sub
            m = min(n - i, self.until)
            done = self.sub - self.until
            seg = x[:, i:i + m].copy()
            if np.array_equal(self.g0, self.g1):  # static: a plain first-order filter per section
                for k in range(2):
                    G = self.g0[k]
                    c = 2.0 * G - 1.0
                    zi = (2.0 * (1.0 - G) * self.s[k])[:, None]
                    seg, zf = lfilter([c, 1.0], [1.0, c], seg, axis=-1, zi=zi)
                    self.s[k] = zf[:, 0] / (2.0 * (1.0 - G))
            else:
                for j in range(m):
                    t = (done + j + 1) / self.sub
                    v_in = seg[:, j]
                    for k in range(2):
                        G = self.g0[k] + (self.g1[k] - self.g0[k]) * t
                        v = (v_in - self.s[k]) * G
                        lp = v + self.s[k]
                        self.s[k] = lp + v
                        v_in = 2.0 * lp - v_in
                    seg[:, j] = v_in
            y[:, i:i + m] = seg
            self.until -= m
            i += m
        return y


# ----------------------------------------------------------------------------------------------------------------
# P1 checks
# ----------------------------------------------------------------------------------------------------------------

RATES = (44100, 48000, 96000, 192000)


def main():
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    out = Path(__file__).resolve().parent / "out" / "p1"
    out.mkdir(parents=True, exist_ok=True)
    rep = ["# P1: Hi/Lo prototype checks\n", "Generated by `prototype/hilo.py`.\n",
           f"Mapping: Hi f1 = f2 = {MODES['hi'].f1} Hz, ganged from θ = {HI_BLEND_END:.0f}°; "
           f"Lo f1 = {MODES['lo'].f1} Hz, f2 = {MODES['lo'].f2:.0f} Hz. K_MIN = {K_MIN:.3g}.\n"]

    # 1. Phase vs frequency at every rate.
    thetas = np.arange(0, 181, 15)
    for mode in MODES:
        fig, axes = plt.subplots(2, 2, figsize=(12, 8), sharey=True)
        for ax, fs in zip(axes.flat, RATES):
            f = np.geomspace(10, 0.499 * fs, 500)
            for t in thetas:
                ax.semilogx(f, lag(mode, t, f, fs), lw=1.4 if t in (90, 180) else 0.8, label=f"{t}°")
                ax.semilogx(f, analog_lag(mode, t, f), "k:", lw=0.5)
            ax.axvline(20000, color="k", lw=0.5)
            ax.set_title(f"{mode} @ {fs} Hz (dotted: analog)")
            ax.grid(True, which="both", alpha=0.3)
        axes[0, 0].legend(fontsize=6, ncol=2)
        for ax in axes[:, 0]:
            ax.set_ylabel("phase lag (°)")
        fig.tight_layout()
        fig.savefig(out / f"phase_{mode}.png", dpi=110)
        plt.close(fig)

    # 2. Continuity at the 90° handover, and readout accuracy.
    rep.append("## Handover at 90° (10 Hz–20 kHz)\n")
    rep.append("| mode | rate | jump across 90° ± 0.001° | largest lag change per 0.1° step, θ 89–91° | "
               "same, θ 0–2° (section 1 entering) |")
    rep.append("|---|---|---|---|---|")

    def steepest(mode, f, fs, t0, t1):
        return max(np.max(np.abs(lag(mode, t + 0.1, f, fs) - lag(mode, t, f, fs))) for t in np.arange(t0, t1, 0.1))

    for mode in MODES:
        for fs in RATES:
            f = np.geomspace(10, 20000, 2000)
            jump = np.max(np.abs(lag(mode, 90.001, f, fs) - lag(mode, 89.999, f, fs)))
            rep.append(f"| {mode} | {fs} | {jump:.4f}° | {steepest(mode, f, fs, 89.0, 91.0):.1f}° | "
                       f"{steepest(mode, f, fs, 0.0, 2.0):.1f}° |")
    rep.append("\nBoth sections always run, so the handover is continuous by construction; there is no engage event. "
               "Just past 90° the second section's corner sweeps down from far above the audio band, so the top "
               "octave moves quickly per degree. That is the same thing that happens as section 1 enters near 0°. "
               "The 30 ms angle smoothing covers it.")

    rep.append("\n## Readout (Hi: true lag at f1; Lo: section lags at f1 and f2)\n")
    worst = {}
    for mode in MODES:
        m = MODES[mode]
        for fs in RATES:
            for t in np.arange(0, 180.01, 0.5):
                k1, k2 = k_pair(mode, t, fs)
                t1, t2 = split(mode, t)
                if mode == "hi":
                    e = abs(section_lag(k1, m.f1, fs) + section_lag(k2, m.f1, fs) - t)
                else:
                    e = max(abs(section_lag(k1, m.f1, fs) - t1), abs(section_lag(k2, m.f2, fs) - t2))
                worst[mode] = max(worst.get(mode, 0.0), float(e))
    rep.append(f"- Worst readout error, all rates, θ 0–180°: Hi {worst['hi']:.3f}°, Lo {worst['lo']:.3f}° "
               f"(only the K_MIN floor near 0° contributes).")
    f_lo = np.array([MODES["lo"].f1])
    rep.append(f"- Lo's true lag at {MODES['lo'].f1} Hz runs 0–90° over θ 0–90°, then only to "
               f"{lag('lo', 180, f_lo, 48000)[0]:.1f}° at θ = 180° (the extra phase is higher up), so the Lo readout "
               f"past 90° is 90° + section 2's lag at {MODES['lo'].f2:.0f} Hz.")

    # 3. Monotonic: more knob never means less phase at any frequency.
    rep.append("\n## Monotonic in θ\n")
    fa = np.geomspace(1, 1e6, 3000)
    for mode in MODES:
        P = np.array([analog_lag(mode, t, fa) for t in np.arange(0, 180.001, 0.05)])
        rep.append(f"- {mode}: smallest lag change per 0.05° step over 1 Hz–1 MHz (analog, so it holds at every "
                   f"rate): {np.min(np.diff(P, axis=0)):+.5f}°")

    # 4. The sample processor equals the closed form (static), and the Hi<->Lo glide stays all-pass.
    rep.append("\n## Sample processor\n")
    fs = 48000
    for mode, t in (("hi", 37.0), ("hi", 140.0), ("lo", 110.0)):
        p = HiLo(fs, mode, t)
        n = 1 << 16
        imp = np.zeros(n)
        imp[0] = 1.0
        h = p.process(imp)[0]
        H = np.fft.rfft(h)
        f = np.fft.rfftfreq(n, 1 / fs)
        sel = (f > 10) & (f < 0.45 * fs)
        meas = -np.degrees(np.unwrap(np.angle(H)))[sel]
        rep.append(f"- {mode} θ={t:.0f}°: impulse response vs closed form: max phase error "
                   f"{np.max(np.abs(meas - lag(mode, t, f[sel], fs))):.2e}°, magnitude "
                   f"{np.max(np.abs(20 * np.log10(np.abs(H[sel])))):.2e} dB")
    # Glide: settle a long noise burst through a Hi->Lo switch; the output power must match the input's.
    rng = np.random.default_rng(0)
    x = rng.standard_normal(fs)
    p = HiLo(fs, "hi", 120.0)
    p.process(x[: fs // 2])
    p.set(mode="lo")
    y = p.process(x[fs // 2:])
    rep.append(f"- Hi→Lo glide at θ=120° on white noise: output/input RMS over the glide "
               f"{np.sqrt(np.mean(y[0, :2048] ** 2) / np.mean(x[fs // 2: fs // 2 + 2048] ** 2)):.4f} "
               f"(each section is all-pass at every k, so the glide is all-pass throughout).")

    (out / "report.md").write_text("\n".join(rep) + "\n")
    print("\n".join(rep))


if __name__ == "__main__":
    main()
