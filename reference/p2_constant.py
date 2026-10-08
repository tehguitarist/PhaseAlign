"""Constant mode: a linear-phase FIR Hilbert transformer for a true constant rotation of phase.

Specifies src/dsp/ConstantRotator.h and HilbertFir.h, and renders the golden data for them (see golden.py).

    .venv/bin/python reference/p2_constant.py      # prints the rotation accuracy per length and rate

    y[n] = cos(theta) * x[n - D] + sin(theta) * (h * x)[n]

h is an odd-length (type III) Kaiser-windowed ideal Hilbert transformer centred at tap D = (N - 1) / 2: h[D + m] =
2 / (pi m) for odd m, 0 for even m. Its response is -j A(f) e^{-j 2 pi f D / fs} with A(f) real, so the Q path is
exactly 90 degrees from the I path at every frequency; the only error is A(f) != 1, at the low end (the window's length)
and near Nyquist. With A < 1 the rotation by theta comes out as atan2(A sin(theta), cos(theta)) at a level of
sqrt(cos^2 + A^2 sin^2): the angle error is worst at 45 (and 135) degrees, the level error at 90.

Lengths are given at 48 kHz (2049 / 4097 / 8193 taps: 21 / 43 / 85 ms) and scaled with the rate to keep the same
duration, so the low end is the same at every rate. The plugin uses 4097 taps at 48 kHz.
"""

import math

import numpy as np
from scipy.signal import fftconvolve, freqz
from scipy.signal.windows import kaiser

RATES = [44100, 48000, 88200, 96000, 176400, 192000]
TAPS_48K = [2049, 4097, 8193]
BETA = 6.0


# --- design ------------------------------------------------------------------------------------------------------
def taps_at(taps48, fs):
    """The tap count at fs for a length given at 48 kHz: the same duration, rounded to N = 1 (mod 4), so the centre D
    is even and the first and last taps are zero too (src/dsp/HilbertFir.h)."""
    return 4 * int(round((taps48 * fs / 48000.0 - 1.0) / 4.0)) + 1


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
def main():
    """Print the rotation error of each length at each rate: the worst angle (degrees) and level (dB) error at a few
    low frequencies, from 50 Hz to 20 kHz, and from 16 kHz to 20 kHz, with the latency."""
    print("taps@48k  rate    taps   20 Hz            50 Hz            50 Hz-20 kHz worst   16-20 kHz worst      latency")
    for taps48 in TAPS_48K:
        for fs in RATES:
            n = taps_at(taps48, fs)
            h = hilbert_fir(n)
            top = min(20000.0, 0.499 * fs)

            def at(f):
                ang, lev = rotation_errors(amplitude(h, np.array([f]), fs))
                return f"{ang[0]:5.2f}/{lev[0]:+6.2f} dB"

            ang_b, lev_b = rotation_errors(amplitude(h, np.geomspace(50.0, top, 600), fs))
            ang_h, lev_h = rotation_errors(amplitude(h, np.geomspace(16000.0, top, 100), fs))
            print(f"{taps48:<9} {fs:<7} {n:<6} {at(20.0)}  {at(50.0)}  "
                  f"{ang_b.max():6.3f}/{np.abs(lev_b).max():6.3f} dB  {ang_h.max():6.3f}/{np.abs(lev_h).max():6.3f} dB  "
                  f"{1000.0 * latency(n, fs) / fs:5.1f} ms")


if __name__ == "__main__":
    main()
