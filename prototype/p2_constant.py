"""P2: Constant mode (IMPLEMENTATION_PLAN 2.4): a linear-phase FIR Hilbert transformer for a true constant rotation.

    .venv/bin/python prototype/p2_constant.py      # accuracy tables, plots and listening renders → prototype/out/p2/

    y[n] = cos(theta) * x[n - D] + sin(theta) * (h * x)[n]

h is an odd-length (type III) Kaiser-windowed ideal Hilbert transformer centred at tap D = (N - 1) / 2: h[D + m] =
2 / (pi m) for odd m, 0 for even m. Its response is -j A(f) e^{-j 2 pi f D / fs} with A(f) real, so the Q path is
exactly 90° from the I path at every frequency; the only error is A(f) != 1, at the low end (the window's length) and
near Nyquist. With A < 1 the rotation by theta comes out as atan2(A sin(theta), cos(theta)) at a level of
sqrt(cos^2 + A^2 sin^2): the angle error is worst at 45° (and 135°), the level error at 90°.

Lengths are given at 48 kHz (2049 / 4097 / 8193 taps: 21 / 43 / 85 ms) and scaled with the rate to keep the same
duration, so the low end is the same at every rate. In the plugin the I path is delayed by L = D + B to match the
partitioned convolver's block B (256, or 512 at 96 kHz and up); offline here, the I path is just delayed by D.

Outputs (prototype/out/p2/):
  report.md            tables: error at the low end and the top per rate and length, latency per rate
  accuracy.png         A(f) as level error at 90° and angle error at 45°, per length, every rate
  kernel.png           the Q path's impulse response per length (what a rotated transient spreads into)
  renders/<pair>/      synthetic two-mic pairs, rotated, for listening (see report.md for the guide)
"""

import math
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import soundfile as sf
from scipy.signal import butter, fftconvolve, freqz, lfilter, sosfilt
from scipy.signal.windows import kaiser

OUT = Path(__file__).resolve().parent / "out" / "p2"
RATES = [44100, 48000, 88200, 96000, 176400, 192000]
TAPS_48K = [2049, 4097, 8193]
BETA = 6.0
RENDER_FS = 48000
ANGLES = [0, 45, 90, 135, 180]


# --- design ------------------------------------------------------------------------------------------------------
def taps_at(taps48, fs):
    """The tap count at fs for a length given at 48 kHz: the same duration, rounded to N = 1 (mod 4), so the centre D
    is even and the first and last taps are zero too (src/dsp/HilbertFir.h)."""
    return 4 * int(round((taps48 * fs / 48000.0 - 1.0) / 4.0)) + 1


def block_size(fs):
    return 512 if fs >= 96000 else 256


def latency(num_taps, fs):
    """The plugin's latency L = D - 1: the zero first tap is dropped and the convolver applies the first partition
    directly, so it adds no block latency (src/dsp/PartitionedConvolver.h). fs is unused but kept in the API."""
    return (num_taps - 1) // 2 - 1


def hilbert_fir(num_taps, beta=BETA):
    """Kaiser-windowed ideal Hilbert transformer, odd length (type III): every even offset from the centre is 0."""
    assert num_taps % 2 == 1
    d = (num_taps - 1) // 2
    m = np.arange(num_taps) - d
    h = np.zeros(num_taps)
    odd = m % 2 != 0
    h[odd] = 2.0 / (math.pi * m[odd])
    return h * kaiser(num_taps, beta)


def amplitude(h, freqs, fs):
    """A(f): the Hilbert response with its linear phase and -j removed (real; 1 is ideal)."""
    d = (len(h) - 1) // 2
    _, resp = freqz(h, worN=freqs, fs=fs)
    a = resp * np.exp(2j * math.pi * freqs * d / fs) / -1j
    assert np.max(np.abs(a.imag)) < 1e-6, "type III: the Q path must be exactly 90°"
    return a.real


def rotation_errors(a):
    """Worst angle error (degrees, over theta) and worst level error (dB, at 90°) for amplitude a."""
    angle = np.degrees(np.abs(np.arctan(np.abs(a)) - math.pi / 4))  # worst at 45°: atan2(A sin, cos) - 45°
    level = 20 * np.log10(np.maximum(np.abs(a), 1e-12))  # worst at 90°: the output is the Q path alone
    return angle, level


def rotate(x, h, theta_deg):
    """The rotation offline: cos * x delayed by D + sin * (h * x), trimmed back to x's timing (the latency removed)."""
    d = (len(h) - 1) // 2
    q = fftconvolve(x, h)[d : d + len(x)]
    t = math.radians(theta_deg)
    return math.cos(t) * x + math.sin(t) * q


class ConstantRotator:
    """The sample-processing reference for src/dsp/ConstantRotator.h (golden-tested, tests/golden/):
    y[n] = cos(theta) x[n - L] + sin(theta) (h * x)[n - (L - D)], L = D - 1.

    The angle is a linear ramp of fixed length (smooth_ms) on a grid of `sub` samples: at the start of each cell it
    moves on by one cell, and cos/sin are interpolated per sample from their values before the move to those after.
    Exact at 0, 90 and 180 degrees; settled at 0 or 180 the output is +-x[n - L] exactly. Offline: the convolution is
    exact (no partitions), so this checks the convolver too.
    """

    def __init__(self, fs, theta=0.0, taps48=4097, smooth_ms=20.0, sub=32):
        self.fs, self.sub = fs, sub
        self.h = hilbert_fir(taps_at(taps48, fs))
        self.d = (len(self.h) - 1) // 2
        self.latency = latency(len(self.h), fs)
        self.theta = self.target = float(theta)
        self.step = 0.0
        self.smooth_n = max(1, round(smooth_ms * 1e-3 * fs))
        self.schedule = []  # (sample, theta), applied before that sample

    @staticmethod
    def trig(deg):
        if deg == 0.0:
            return 1.0, 0.0
        if deg == 180.0:
            return -1.0, 0.0
        if deg == 90.0:
            return 0.0, 1.0
        r = math.radians(deg)
        return math.cos(r), math.sin(r)

    def render(self, x, schedule):
        """x: (channels, n). schedule: [(sample, theta)], in order. Returns (channels, n)."""
        x = np.atleast_2d(np.asarray(x, dtype=np.float64))
        n = x.shape[1]
        lat, shift = self.latency, self.latency - self.d
        dry = np.zeros_like(x)
        dry[:, lat:] = x[:, : n - lat]
        q = np.zeros_like(x)
        for ch in range(x.shape[0]):
            full = fftconvolve(x[ch], self.h)[: n + 1]
            if shift >= 0:
                q[ch, shift:] = full[: n - shift]
            else:  # q[n] = (h * x)[n + 1]
                q[ch, :] = full[-shift: n - shift]
        y = np.empty_like(x)
        events = list(schedule)
        for cell in range(0, n, self.sub):
            while events and events[0][0] <= cell:
                t = float(events.pop(0)[1])
                if t != self.target:
                    self.target = t
                    self.step = (self.target - self.theta) / self.smooth_n
            c0, s0 = self.trig(self.theta)
            if self.theta != self.target:
                self.theta += self.step * self.sub
                if (self.step > 0) == (self.theta >= self.target):
                    self.theta = self.target
            c1, s1 = self.trig(self.theta)
            m = min(self.sub, n - cell)
            sl = slice(cell, cell + m)
            if s0 == 0.0 and s1 == 0.0:
                y[:, sl] = (-1.0 if c0 < 0 else 1.0) * dry[:, sl]
                continue
            t = (np.arange(m) + 1) / self.sub
            c, s_ = c0 + (c1 - c0) * t, s0 + (s1 - s0) * t
            y[:, sl] = c * dry[:, sl] + s_ * q[:, sl]
        return y


# --- accuracy ----------------------------------------------------------------------------------------------------
def accuracy():
    rows = []
    fig, axes = plt.subplots(len(TAPS_48K), 2, figsize=(13, 3.3 * len(TAPS_48K)), sharex=True)
    colours = plt.cm.viridis(np.linspace(0, 0.9, len(RATES)))
    for r, taps48 in enumerate(TAPS_48K):
        for fs, colour in zip(RATES, colours):
            n = taps_at(taps48, fs)
            h = hilbert_fir(n)
            top = min(20000.0, 0.499 * fs)
            freqs = np.geomspace(10.0, min(24000.0, 0.499 * fs), 1200)
            a = amplitude(h, freqs, fs)
            angle, level = rotation_errors(a)
            axes[r, 0].semilogx(freqs, level, color=colour, label=f"{fs / 1000:g} kHz ({n} taps)")
            axes[r, 1].semilogx(freqs, angle, color=colour)

            at = lambda f: amplitude(h, np.array([f]), fs)[0]  # noqa: E731
            pts = {f: rotation_errors(np.array([at(f)])) for f in (20.0, 25.0, 30.0, 40.0, 50.0)}
            band = np.geomspace(50.0, top, 600)
            ang_b, lev_b = rotation_errors(amplitude(h, band, fs))
            hi = np.geomspace(16000.0, top, 100)
            ang_hi, lev_hi = rotation_errors(amplitude(h, hi, fs))
            d = (n - 1) // 2
            rows.append(
                dict(
                    taps48=taps48,
                    fs=fs,
                    taps=n,
                    d_ms=1000.0 * d / fs,
                    latency_ms=1000.0 * latency(n, fs) / fs,
                    pts=pts,
                    band_angle=ang_b.max(),
                    band_level=np.abs(lev_b).max(),
                    hi_angle=ang_hi.max(),
                    hi_level=np.abs(lev_hi).max(),
                )
            )
        axes[r, 0].set_ylabel(f"{taps48} taps @48k\nlevel error at 90° (dB)")
        axes[r, 1].set_ylabel("angle error at 45° (°)")
        axes[r, 0].set_ylim(-6, 0.5)
        axes[r, 1].set_ylim(0, 20)
        for ax in axes[r]:
            ax.axvspan(20, 20000, color="0.93", zorder=-1)
            ax.grid(True, which="both", alpha=0.3)
        axes[r, 0].legend(fontsize=7, loc="lower right")
    for ax in axes[-1]:
        ax.set_xlabel("frequency (Hz)")
        ax.set_xlim(10, 24000)
    fig.suptitle("P2: FIR Hilbert rotation error by length (Kaiser β = 6; tap counts scaled with the rate)")
    fig.tight_layout()
    fig.savefig(OUT / "accuracy.png", dpi=110)
    plt.close(fig)
    return rows


def kernel_plot():
    fs = 48000
    fig, ax = plt.subplots(figsize=(12, 4))
    for taps48, colour in zip(TAPS_48K, ["tab:red", "tab:blue", "tab:green"]):
        h = hilbert_fir(taps48)
        d = (len(h) - 1) // 2
        t = (np.arange(len(h)) - d) / fs * 1000.0
        ax.plot(t, h, color=colour, lw=0.9, label=f"{taps48} taps (±{1000 * d / fs:.0f} ms)")
    ax.set_xlim(-45, 45)
    ax.set_ylim(-0.15, 0.15)
    ax.set_xlabel("time from the transient (ms)")
    ax.set_ylabel("Q path impulse response")
    ax.set_title("A 90° rotation of an impulse: the Hilbert kernel 2/(πn), symmetric, so half of it comes before")
    ax.grid(alpha=0.3)
    ax.legend()
    fig.tight_layout()
    fig.savefig(OUT / "kernel.png", dpi=110)
    plt.close(fig)


# --- synthetic pairs ---------------------------------------------------------------------------------------------
def allpass1(x, fc, fs):
    """First-order all-pass (frequency-dependent phase, flat level): stands in for mic, speaker and bleed phase."""
    t = math.tan(math.pi * fc / fs)
    c = (t - 1) / (t + 1)
    return lfilter([c, 1.0], [1.0, c], x)


def frac_delay(x, delay_samples):
    """Delay by a (possibly fractional) number of samples, in the frequency domain (the mic spacing)."""
    n = len(x)
    nfft = 1 << (n + int(delay_samples) + 64).bit_length()
    spec = np.fft.rfft(x, nfft)
    f = np.fft.rfftfreq(nfft)
    y = np.fft.irfft(spec * np.exp(-2j * math.pi * f * delay_samples), nfft)
    return y[:n]


def env(n, fs, attack_ms, decay_ms):
    t = np.arange(n) / fs
    a = np.minimum(t / (attack_ms / 1000.0), 1.0)
    return a * np.exp(-t / (decay_ms / 1000.0))


def pair_kick(fs, rng):
    """Kick in (beater + shell) and out (more low end, later, with the port's phase)."""

    def one(rng):
        n = int(0.6 * fs)
        t = np.arange(n) / fs
        f = 48 + 110 * np.exp(-t / 0.03)
        body = np.sin(2 * math.pi * np.cumsum(f) / fs) * env(n, fs, 1, 180)
        click = rng.standard_normal(n) * env(n, fs, 0.1, 4)
        click = sosfilt(butter(2, 2500, "high", fs=fs, output="sos"), click)
        return 0.6 * body + 0.5 * click, body

    seq_rng = np.random.default_rng(1)
    n_total = int(4.0 * fs)
    inside, outside = np.zeros(n_total), np.zeros(n_total)
    t = 0.25
    while t + 0.6 < 4.0:
        b, c = one(seq_rng)
        i = int(t * fs)
        inside[i : i + len(b)] += b
        outside[i : i + len(b)] += 1.3 * c + 0.15 * (b - 0.6 * c)
        t += 0.5
    outside = sosfilt(butter(2, 900, "low", fs=fs, output="sos"), outside)
    outside = allpass1(allpass1(outside, 70, fs), 140, fs)
    outside = frac_delay(outside, 0.0011 * fs)  # about 38 cm further away
    return inside, outside


def pair_snare(fs, rng):
    """Snare top (stick + body) and bottom (wires: brighter, inverted by facing up, a little later)."""
    seq_rng = np.random.default_rng(2)
    n_total = int(4.0 * fs)
    top, bottom = np.zeros(n_total), np.zeros(n_total)
    t = 0.25
    while t + 0.4 < 4.0:
        n = int(0.4 * fs)
        tt = np.arange(n) / fs
        body = (np.sin(2 * math.pi * 185 * tt) + 0.6 * np.sin(2 * math.pi * 330 * tt)) * env(n, fs, 0.5, 60)
        wires = seq_rng.standard_normal(n) * env(n, fs, 0.5, 110)
        wires = sosfilt(butter(2, [1500, 9000], "band", fs=fs, output="sos"), wires)
        stick = seq_rng.standard_normal(n) * env(n, fs, 0.05, 3)
        i = int(t * fs)
        top[i : i + n] += 0.8 * body + 0.35 * wires + 0.4 * stick
        bottom[i : i + n] += 0.5 * body + 1.0 * wires
        t += 0.5
    bottom = -allpass1(bottom, 400, fs)
    bottom = frac_delay(bottom, 0.0006 * fs)
    return top, bottom


def pair_bass(fs, rng):
    """Bass DI and amp: the amp is band-limited with the cabinet's phase and a little later (latency, distance)."""
    notes = [41.2, 41.2, 55.0, 49.0, 41.2, 61.7, 55.0, 49.0]
    n_total = int(4.0 * fs)
    di = np.zeros(n_total)
    step = int(0.45 * fs)
    for k, f in enumerate(notes):
        i = int(0.2 * fs) + k * step
        n = min(step, n_total - i)
        if n <= 0:
            break
        tt = np.arange(n) / fs
        saw = sum(np.sin(2 * math.pi * f * h * tt) / h for h in range(1, 30) if f * h < 8000)
        di[i : i + n] += 0.35 * saw * env(n, fs, 3, 900)
    amp = sosfilt(butter(4, 2500, "low", fs=fs, output="sos"), di)
    amp = sosfilt(butter(2, 60, "high", fs=fs, output="sos"), amp)
    amp = allpass1(amp, 120, fs)
    amp = frac_delay(amp, 0.0015 * fs)
    return di, amp


def pair_guitar(fs, rng):
    """Acoustic-ish guitar close and room: the room mic is 3 ms later, with early reflections and air loss."""
    seq_rng = np.random.default_rng(4)
    n_total = int(4.0 * fs)
    close = np.zeros(n_total)
    for k, f in enumerate([110.0, 146.8, 196.0, 246.9, 196.0, 146.8, 164.8, 220.0]):
        period = int(round(fs / f))
        n = int(1.2 * fs)
        buf = seq_rng.uniform(-1, 1, period)
        out = np.zeros(n)
        for i in range(n):  # Karplus-Strong
            out[i] = buf[i % period]
            buf[i % period] = 0.5 * (buf[i % period] + buf[(i + 1) % period]) * 0.996
        i0 = int(0.2 * fs) + k * int(0.45 * fs)
        m = min(n, n_total - i0)
        close[i0 : i0 + m] += 0.3 * out[:m]
    room = frac_delay(close, 0.003 * fs)
    for delay_ms, gain in [(5.1, 0.45), (7.9, -0.3), (11.3, 0.25)]:
        room += gain * frac_delay(close, delay_ms / 1000.0 * fs)
    room = sosfilt(butter(1, 6000, "low", fs=fs, output="sos"), room)
    return close, room


PAIRS = {"kick_in_out": pair_kick, "snare_top_bottom": pair_snare, "bass_di_amp": pair_bass, "guitar_close_room": pair_guitar}


def best_alignment(a, b):
    """The whole-sample shift of b (within the delay knob's -4..+4 ms) that lines it up with a: the PHAT peak."""
    n = len(a)
    nfft = 1 << (2 * n).bit_length()
    cross = np.fft.rfft(a, nfft) * np.conj(np.fft.rfft(b, nfft))
    lag_fn = np.fft.irfft(cross / np.maximum(np.abs(cross), 1e-12), nfft)
    max_lag = int(0.004 * RENDER_FS)
    lags = np.concatenate([np.arange(0, max_lag + 1), np.arange(-max_lag, 0)])
    vals = np.concatenate([lag_fn[: max_lag + 1], lag_fn[-max_lag:]])
    lag = int(lags[np.argmax(np.abs(vals))])
    return lag


def shift(x, k):
    y = np.zeros_like(x)
    if k >= 0:
        y[k:] = x[: len(x) - k]
    else:
        y[:k] = x[-k:]
    return y


def renders():
    fs = RENDER_FS
    rng = np.random.default_rng(0)  # the pair functions seed their own sequences; rng is unused but kept in the API
    root = OUT / "renders"
    lines = []
    for name, make in PAIRS.items():
        a, b = make(fs, rng)  # a: the reference (stays), b: the track that gets Phase Align
        # The plugin is on b; with the -4..+4 ms delay it can move b either way. Align it to a by the PHAT peak first.
        lag = best_alignment(a, b)  # positive: b is earlier, so it is delayed
        b_aligned = shift(b, lag)
        peak = max(np.max(np.abs(a)), np.max(np.abs(b_aligned)))
        a, b_aligned, b = a / peak * 0.45, b_aligned / peak * 0.45, b / peak * 0.45

        d = root / name
        d.mkdir(parents=True, exist_ok=True)
        for f in d.glob("*.wav"):
            f.unlink()
        sf.write(d / "00_sum_unaligned.wav", a + b, fs, subtype="FLOAT")
        sf.write(d / "01_sum_delay_only.wav", a + b_aligned, fs, subtype="FLOAT")

        results, sums = [], {}
        for taps48 in TAPS_48K:
            h = hilbert_fir(taps48)
            dlen = (len(h) - 1) // 2
            q = fftconvolve(b_aligned, h)[dlen : dlen + len(b_aligned)]
            # 0 to 360: the knob's 0 to 180 plus the polarity flip.
            theta = math.degrees(math.atan2(np.dot(a, q), np.dot(a, b_aligned))) % 360.0
            y = rotate(b_aligned, h, theta)
            rms_gain = 20 * math.log10(np.std(a + y) / np.std(a + b_aligned))
            corr_before = np.dot(a, b_aligned) / math.sqrt(np.dot(a, a) * np.dot(b_aligned, b_aligned))
            corr_after = np.dot(a, y) / math.sqrt(np.dot(a, a) * np.dot(y, y))
            sums[taps48] = a + y
            results.append([taps48, theta, rms_gain, corr_before, corr_after])
            sf.write(d / f"02_sum_delay_and_best_rotation_{theta:05.1f}deg_{taps48}taps.wav", a + y, fs, subtype="FLOAT")
            sf.write(d / f"04_track_alone_rotated_90deg_{taps48}taps.wav", rotate(b_aligned, h, 90.0), fs, subtype="FLOAT")
        # How far each length's best-rotation sum is from the longest one's: an objective hint for the listening.
        longest = sums[TAPS_48K[-1]]
        for r in results:
            r.append(20 * math.log10(np.std(sums[r[0]] - longest) / np.std(longest) + 1e-30))
        h = hilbert_fir(4097)
        for ang in ANGLES:
            sf.write(d / f"03_sum_rotated_{ang:03d}deg_4097taps.wav", a + rotate(b_aligned, h, ang), fs, subtype="FLOAT")
        sf.write(d / "04_track_alone_dry.wav", b_aligned, fs, subtype="FLOAT")
        lines.append((name, lag, results))
    return lines


# --- report ------------------------------------------------------------------------------------------------------
def report(rows, render_lines):
    md = ["# P2: Constant mode, FIR Hilbert length", "", "Generated by `prototype/p2_constant.py`. Plan 2.4.", ""]
    md += [
        "## Accuracy",
        "",
        "Angle error is the worst over θ (at 45°/135°); level error is at 90° (where the output is the Q path alone).",
        "Tap counts scale with the rate (same duration), so the low end is the same at every rate.",
        "",
        "| taps @48k | rate | taps | 20 Hz | 25 Hz | 30 Hz | 40 Hz | 50 Hz | 50 Hz–20 kHz worst | 16–20 kHz worst | latency L = D − 1 |",
        "|---|---|---|---|---|---|---|---|---|---|---|",
    ]
    for r in rows:
        cell = lambda f: f"{r['pts'][f][0][0]:.2f}° / {r['pts'][f][1][0]:+.2f} dB"  # noqa: E731
        md.append(
            f"| {r['taps48']} | {r['fs'] / 1000:g} kHz | {r['taps']} | {cell(20.0)} | {cell(25.0)} | {cell(30.0)} | "
            f"{cell(40.0)} | {cell(50.0)} | {r['band_angle']:.3f}° / {r['band_level']:.3f} dB | "
            f"{r['hi_angle']:.3f}° / {r['hi_level']:.3f} dB | {r['latency_ms']:.1f} ms |"
        )
    md += ["", "![accuracy](accuracy.png)", "", "![kernel](kernel.png)", ""]
    md += ["## Renders (48 kHz, synthetic pairs)", ""]
    md += [
        "Each pair is a reference mic `a` and the track `b` that gets Phase Align. `b` is first aligned to `a` by the",
        "PHAT peak (what the delay knob does), then rotated. The best angle is the closed form",
        "θ = atan2(corr(a, Q), corr(a, I)) (plan v0.8).",
        "",
        "| pair | delay applied | taps | best θ | r before → after rotation | level of a + b vs delay only | "
        f"difference from {TAPS_48K[-1]} taps |",
        "|---|---|---|---|---|---|---|",
    ]
    for name, lag, results in render_lines:
        for taps48, theta, gain, cb, ca, diff in results:
            vs = "(reference)" if taps48 == TAPS_48K[-1] else f"{diff:.1f} dB"
            md.append(
                f"| {name} | {lag:+d} samples ({1000 * lag / RENDER_FS:+.2f} ms) | {taps48} | {theta:.1f}° | "
                f"{cb:+.3f} → {ca:+.3f} | {gain:+.2f} dB | {vs} |"
            )
    md += [
        "",
        "Files in `renders/<pair>/`:",
        "",
        "- `00_sum_unaligned.wav`: a + b as recorded.",
        "- `01_sum_delay_only.wav`: a + b after the delay alone (what the plugin does today).",
        "- `02_sum_delay_and_best_rotation_*`: plus the best rotation, one per length. **Compare the three lengths:**",
        "  any difference is in the lowest octave (below about 50 Hz).",
        "- `03_sum_rotated_*deg_4097taps.wav`: the knob swept 0/45/90/135/180°, to hear what rotation does to the sum.",
        "- `04_track_alone_*`: the track alone, dry and rotated 90° per length. **Listen for the pre-wiggle** on the kick",
        "  and snare: a true rotation spreads a transient symmetrically in time (kernel.png).",
        "",
        "Synthetic material can show the mechanism but not whether rotation helps on real recordings; that still needs",
        "real multi-mic stems (plan section 7).",
    ]
    (OUT / "report.md").write_text("\n".join(md) + "\n")


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    rows = accuracy()
    kernel_plot()
    render_lines = renders()
    report(rows, render_lines)
    print((OUT / "report.md").read_text())


if __name__ == "__main__":
    main()
