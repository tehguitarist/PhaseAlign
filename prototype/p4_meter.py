"""P4: the correlation meter (IMPLEMENTATION_PLAN 3), simulated on synthetic pairs with a known relationship.

    .venv/bin/python prototype/p4_meter.py        # tables, plots and display mock-ups -> prototype/out/p4/

The analyser is the one the plan specifies for the GUI thread: Hann-windowed FFT frames (N = 8192 at 48 kHz, 50%
overlap); per band, the sums Sxy = sum Re(X Y*), Sxx = sum |X|^2, Syy = sum |Y|^2 over the band's bins; each sum
smoothed by an exponential average with a per-band time constant max(tau0, cycles / fc); r = Sxy / sqrt(Sxx Syy).
The three streams are the processed output x, the unprocessed input z and the sidechain y.

The questions: how many bands, how much averaging, how big an FFT, and what the screen should show. Every case
has a closed-form expected r per band (from the transfer functions and the source spectrum), so bias and jitter
are measured against the truth, not against another estimate.
"""

import math
from dataclasses import dataclass
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

import hilo

OUT = Path(__file__).resolve().parent / "out" / "p4"
FS = 48000
EPS = 1e-30


# ----------------------------------------------------------------------------------------------------------------
# Sources
# ----------------------------------------------------------------------------------------------------------------

def pink_shape(f):
    """Power spectrum of the pink source: 1/f, 2nd-order high-pass at 20 Hz, gentle roll-off above 16 kHz."""
    f = np.maximum(np.abs(f), 1e-3)
    hp = (f / 20.0) ** 4 / (1 + (f / 20.0) ** 4)
    lp = 1 / (1 + (f / 16000.0) ** 4)
    return hp * lp / f


def pink(n, rng, level_db=-20.0):
    spec = np.fft.rfft(rng.standard_normal(n)) * np.sqrt(pink_shape(np.fft.rfftfreq(n, 1 / FS)))
    x = np.fft.irfft(spec, n)
    return x * 10 ** (level_db / 20) / np.sqrt(np.mean(x ** 2))


def drums(n, rng):
    """Kick on every beat, snare on 2 and 4, closed hats on eighths, 120 bpm: intermittent, transient energy."""
    x = np.zeros(n)
    t = np.arange(int(0.6 * FS)) / FS
    beat = FS // 2
    for start in range(0, n, beat):
        bar_pos = (start // beat) % 4
        # kick: 110 -> 48 Hz sweep, 0.3 s decay
        f = 48 + 62 * np.exp(-t / 0.04)
        kick = np.sin(2 * np.pi * np.cumsum(f) / FS) * np.exp(-t / 0.3)
        seg = slice(start, min(start + len(t), n))
        x[seg] += 0.8 * kick[: seg.stop - seg.start]
        if bar_pos in (1, 3):  # snare: 190 Hz body + band noise
            body = np.sin(2 * np.pi * 190 * t) * np.exp(-t / 0.08)
            noise = np.fft.irfft(np.fft.rfft(rng.standard_normal(len(t)))
                                 * ((np.fft.rfftfreq(len(t), 1 / FS) > 1500) & (np.fft.rfftfreq(len(t), 1 / FS) < 9000)),
                                 len(t)) * np.exp(-t / 0.12)
            x[seg] += (0.4 * body + 1.5 * noise)[: seg.stop - seg.start]
        for h in (0, beat // 2):  # hats
            hs = start + h
            if hs >= n:
                continue
            th = t[: int(0.05 * FS)]
            hat = np.diff(rng.standard_normal(len(th) + 1)) * np.exp(-th / 0.015)
            x[hs: hs + len(th)] += 0.15 * hat[: max(0, min(len(th), n - hs))]
    return x * 10 ** (-6 / 20) / np.max(np.abs(x))


# ----------------------------------------------------------------------------------------------------------------
# Pairs: transfer functions applied in the frequency domain (signals are long, edges trimmed)
# ----------------------------------------------------------------------------------------------------------------

def tf_delay(ms):
    return lambda f: np.exp(-2j * np.pi * f * ms / 1000.0)


def tf_rotation(deg):
    return lambda f: np.exp(-1j * np.radians(deg)) * np.ones_like(f, dtype=complex)


def tf_hi(theta):
    return lambda f: np.exp(-1j * np.radians(hilo.lag("hi", theta, np.minimum(f, FS / 2 - 1), FS)))


def tf_product(*tfs):
    def h(f):
        out = np.ones_like(f, dtype=complex)
        for t in tfs:
            out = out * t(f)
        return out
    return h


def tf_mic_colour(f):
    """A different mic: presence lift and a softer top, zero phase."""
    f = np.maximum(f, 1e-3)
    presence = 1 + 0.6 * np.exp(-0.5 * (np.log2(f / 4000) / 0.6) ** 2)
    return presence / np.sqrt(1 + (f / 12000) ** 2)


def apply(x, tf):
    f = np.fft.rfftfreq(len(x), 1 / FS)
    return np.fft.irfft(np.fft.rfft(x) * tf(f), len(x))


@dataclass
class Case:
    name: str
    description: str
    main: callable          # transfer function from the source to this track (the earlier one)
    side: callable          # source -> sidechain (the later one)
    plugin: callable        # what the plugin does when "aligned" (the processed stream)
    side_noise_db: float = -200.0  # independent noise on the sidechain, dB relative to the source


CASES = [
    Case("delay", "sidechain 1.00 ms later; plugin delay 1.00 ms",
         lambda f: np.ones_like(f, dtype=complex), tf_delay(1.0), tf_delay(1.0)),
    Case("polarity", "sidechain inverted; plugin polarity on",
         lambda f: np.ones_like(f, dtype=complex), tf_rotation(180), tf_rotation(180)),
    Case("rotation60", "sidechain rotated 60 deg (constant); plugin Constant 60 deg",
         lambda f: np.ones_like(f, dtype=complex), tf_rotation(60), tf_rotation(60)),
    Case("hi90", "sidechain through Hi 90 deg; plugin Hi 90 deg",
         lambda f: np.ones_like(f, dtype=complex), tf_hi(90), tf_hi(90)),
    Case("micpair", "two mics: 0.85 ms apart, different colour, own reflections, noise -30 dB; plugin delay 0.85 ms",
         lambda f: 1 + 0.30 * tf_delay(6.1)(f),
         lambda f: tf_mic_colour(f) * (tf_delay(0.85)(f) + 0.35 * tf_delay(4.3)(f)),
         tf_delay(0.85), side_noise_db=-30.0),
]


def render(case, source, rng):
    main = apply(source, case.main)
    side = apply(source, case.side)
    if case.side_noise_db > -150:
        side = side + pink(len(source), rng, 20 * np.log10(np.sqrt(np.mean(side ** 2)) + EPS) + case.side_noise_db)
    processed = apply(main, case.plugin)
    return processed, main, side


# ----------------------------------------------------------------------------------------------------------------
# Bands
# ----------------------------------------------------------------------------------------------------------------

@dataclass
class Layout:
    name: str
    centres: np.ndarray
    fraction: float  # octaves per band

    def edges(self):
        half = 2 ** (self.fraction / 2)
        return self.centres / half, self.centres * half


def iso_series(step, lo=20.0, hi=20000.0):
    """ISO R10 (1/3-octave) preferred centres, every `step`-th one, from 20 Hz."""
    base = [10 ** (i / 10) for i in range(13, 44)]  # 20 Hz .. 20 kHz, exact base-10 thirds
    return np.array(base[::step])


LAYOUTS = [
    Layout("octave (10)", np.array([31.5, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000.0]), 1.0),
    Layout("2/3 octave (16)", iso_series(2), 2 / 3),
    Layout("1/3 octave (31)", iso_series(1), 1 / 3),
]


def label(fc):
    if fc >= 1000:
        v = fc / 1000
        return (f"{v:.0f}k" if abs(v - round(v)) < 0.05 else f"{v:.2g}k")
    return f"{fc:.0f}"


# ----------------------------------------------------------------------------------------------------------------
# The analyser
# ----------------------------------------------------------------------------------------------------------------

@dataclass
class Settings:
    n_fft: int = 8192
    tau0: float = 0.3      # s
    cycles: float = 8.0    # time constant floor in cycles of the band centre
    gate_db: float = -70.0


def frames(x, n, hop):
    count = 1 + (len(x) - n) // hop
    idx = np.arange(n)[None, :] + hop * np.arange(count)[:, None]
    w = np.hanning(n + 1)[:-1]
    return np.fft.rfft(x[idx] * w, axis=1), w


def ema(values, alpha):
    """Exponential average along axis 0; alpha is per column."""
    out = np.empty_like(values)
    acc = np.zeros(values.shape[1:], dtype=values.dtype)
    for i, v in enumerate(values):
        acc = acc + alpha * (v - acc)
        out[i] = acc
    return out


def band_sums(X, Y, freqs, lo, hi):
    """Per frame and band: sum Re(X Y*), sum |X|^2, sum |Y|^2."""
    masks = np.stack([(freqs >= a) & (freqs < b) for a, b in zip(lo, hi)], axis=1).astype(float)  # bins x bands
    return (np.real(X * np.conj(Y)) @ masks, (np.abs(X) ** 2) @ masks, (np.abs(Y) ** 2) @ masks, masks)


def analyse(x, z, y, layout, s: Settings):
    """Band and overall r for processed x and unprocessed z against sidechain y, per frame."""
    n = s.n_fft
    hop = n // 2
    X, w = frames(x, n, hop)
    Z, _ = frames(z, n, hop)
    Y, _ = frames(y, n, hop)
    freqs = np.fft.rfftfreq(n, 1 / FS)
    lo, hi = layout.edges()
    lo = np.append(lo, 20.0)
    hi = np.append(hi, min(20000.0, FS / 2))
    centres = np.append(layout.centres, 1000.0)
    tau = np.maximum(s.tau0, s.cycles / centres)
    tau[-1] = s.tau0
    alpha = 1 - np.exp(-(hop / FS) / tau)

    sxy, sxx, syy, masks = band_sums(X, Y, freqs, lo, hi)
    szy, szz, _, _ = band_sums(Z, Y, freqs, lo, hi)
    sxy, sxx, syy, szy, szz = (ema(v, alpha) for v in (sxy, sxx, syy, szy, szz))
    r_proc = sxy / np.sqrt(sxx * syy + EPS)
    r_unproc = szy / np.sqrt(szz * syy + EPS)

    # Gate: band mean-square level of either signal (Parseval, one-sided, Hann power) below gate_db.
    to_ms = 2.0 / (n * np.sum(w ** 2))
    level = 10 * np.log10(np.minimum(sxx, syy) * to_ms + EPS)
    gated = level < s.gate_db
    times = (np.arange(len(X)) * hop + n) / FS
    return dict(t=times, proc=r_proc[:, :-1], unproc=r_unproc[:, :-1], overall_proc=r_proc[:, -1],
                overall_unproc=r_unproc[:, -1], gated=gated[:, :-1], bins=masks.sum(axis=0)[:-1])


def expected(case, layout, n_fft, unprocessed=False):
    """Long-term r per band from the transfer functions and the pink source spectrum (same bins as the analyser)."""
    freqs = np.fft.rfftfreq(n_fft * 8, 1 / FS)[1:]  # finer grid than the analyser; same band edges
    P = pink_shape(freqs)
    hm = case.main(freqs) * (1 if unprocessed else case.plugin(freqs))
    hs = case.side(freqs)
    noise = 10 ** (case.side_noise_db / 10) * P * np.mean(np.abs(hs) ** 2 * P) / np.mean(P)
    lo, hi = layout.edges()
    lo = np.append(lo, 20.0)
    hi = np.append(hi, min(20000.0, FS / 2))
    out = []
    for a, b in zip(lo, hi):
        m = (freqs >= a) & (freqs < b)
        num = np.sum(np.real(hm[m] * np.conj(hs[m])) * P[m])
        den = np.sqrt(np.sum(np.abs(hm[m]) ** 2 * P[m]) * np.sum(np.abs(hs[m]) ** 2 * P[m] + noise[m]))
        out.append(num / (den + EPS))
    return np.array(out[:-1]), out[-1]


# ----------------------------------------------------------------------------------------------------------------
# Experiments
# ----------------------------------------------------------------------------------------------------------------

DUR = 30.0
SETTLE = 3.0  # s ignored at the start


def accuracy_table(layout, s, seeds=(1, 2, 3)):
    """Bias and jitter of r per band on pink noise, for every case and both streams (processed = aligned,
    unprocessed = as recorded). Bands with no FFT bin are left out (reported separately).
    Returns rows and the per-band worst jitter."""
    rows = []
    jitter_by_band = np.zeros(len(layout.centres))
    for case in CASES:
        stats = {k: [] for k in ("bias", "jit", "worst")}
        for seed in seeds:
            rng = np.random.default_rng(seed)
            src = pink(int(DUR * FS), rng)
            x, z, y = render(case, src, rng)
            res = analyse(x, z, y, layout, s)
            keep = res["t"] > SETTLE
            empty = res["bins"] == 0
            for stream, unprocessed in (("proc", False), ("unproc", True)):
                exp_b, _ = expected(case, layout, s.n_fft, unprocessed)
                r = res[stream][keep]
                err = r - exp_b
                err[:, empty] = np.nan
                stats["bias"].append(np.nanmean(err, axis=0))
                stats["jit"].append(np.where(empty, np.nan, np.std(r, axis=0)))
                stats["worst"].append(np.nanmax(np.abs(err), axis=0))
        bias = np.nanmax(np.abs(np.array(stats["bias"])), axis=0)
        jit = np.nanmax(np.array(stats["jit"]), axis=0)
        worst = np.nanmax(np.array(stats["worst"]), axis=0)
        jitter_by_band = np.fmax(jitter_by_band, jit)
        rows.append((case.name, np.nanmax(bias), np.nanmax(jit), np.nanmax(worst),
                     layout.centres[np.nanargmax(worst)]))
    return rows, jitter_by_band


def settle_times(layout, s):
    """Polarity step on a pure pair: r goes +1 -> -1 at t0. Time from t0 until r < -0.8 per band."""
    rng = np.random.default_rng(7)
    n = int(12 * FS)
    src = pink(n, rng)
    t0 = 6.0
    side = src.copy()
    side[int(t0 * FS):] *= -1
    res = analyse(src, src, side, layout, s)
    out = []
    for b in range(len(layout.centres)):
        after = np.where((res["t"] > t0) & (res["proc"][:, b] < -0.8))[0]
        out.append(res["t"][after[0]] - t0 if len(after) else np.nan)
    after = np.where((res["t"] > t0) & (res["overall_proc"] < -0.8))[0]
    return np.array(out), (res["t"][after[0]] - t0 if len(after) else np.nan)


def drums_table(layout, s):
    """Mic pair on drums: how often each band is gated and how much r jitters while not gated."""
    rng = np.random.default_rng(11)
    src = drums(int(DUR * FS), rng)
    case = CASES[-1]
    x, z, y = render(case, src, rng)
    res = analyse(x, z, y, layout, s)
    keep = res["t"] > SETTLE
    gated = res["gated"][keep]
    proc = res["proc"][keep]
    jit = np.array([np.std(proc[~gated[:, b], b]) if np.any(~gated[:, b]) else np.nan
                    for b in range(proc.shape[1])])
    return gated.mean(axis=0), jit, res


# ----------------------------------------------------------------------------------------------------------------
# Display mock-ups
# ----------------------------------------------------------------------------------------------------------------

GREEN = "#3cf04e"
DIM = "#1d6e26"
GRID = "#3a3a38"
TEXT = "#d0d0cc"


def screen_axes(fig, rect):
    ax = fig.add_axes(rect)
    ax.set_facecolor("#030302")
    for sp in ax.spines.values():
        sp.set_color(GRID)
    ax.tick_params(colors=TEXT, labelsize=8)
    return ax


def mock_bars(ax, layout, proc, unproc, overall, overall_un, title):
    n = len(layout.centres)
    xs = np.arange(n)
    ax.axhline(0, color=TEXT, lw=1)
    for v in (0.5, -0.5):
        ax.axhline(v, color=GRID, lw=0.6, ls=":")
    ax.bar(xs, proc, width=0.5, color=GREEN)
    ax.scatter(xs, unproc, marker="_", s=260, color="#c8ffcc", linewidths=2, zorder=3)
    ax.bar([n + 0.8], [overall], width=0.7, color=GREEN)
    ax.scatter([n + 0.8], [overall_un], marker="_", s=500, color="#c8ffcc", linewidths=2, zorder=3)
    ax.axvline(n + 0.05, color=TEXT, lw=1.5)
    ax.set_xticks(list(xs) + [n + 0.8], [label(c) for c in layout.centres] + ["ALL"])
    ax.set_ylim(-1.05, 1.05)
    ax.set_xlim(-0.7, n + 1.5)
    ax.set_yticks([-1, 0, 1], ["-1", "0", "+1"])
    ax.set_title(title, color=TEXT, fontsize=9, family="monospace", loc="left")


def per_bin_trace(x, y, s, smooth_oct=1 / 6):
    """r(f): per-bin cross-spectra, averaged over time (tau0) and smoothed over 1/6 octave."""
    n = s.n_fft
    X, _ = frames(x, n, n // 2)
    Y, _ = frames(y, n, n // 2)
    sxy = np.mean(np.real(X * np.conj(Y))[-20:], axis=0)
    sxx = np.mean(np.abs(X[-20:]) ** 2, axis=0)
    syy = np.mean(np.abs(Y[-20:]) ** 2, axis=0)
    freqs = np.fft.rfftfreq(n, 1 / FS)
    fc = np.geomspace(20, 20000, 300)
    half = 2 ** (smooth_oct / 2)
    r = []
    for c in fc:
        m = (freqs >= c / half) & (freqs < c * half)
        if not np.any(m):  # below the bin spacing: nearest bin
            m = np.abs(freqs - c) == np.min(np.abs(freqs - c))
        r.append(np.sum(sxy[m]) / np.sqrt(np.sum(sxx[m]) * np.sum(syy[m]) + EPS))
    return fc, np.array(r)


def lag_function(x, y, s, max_ms=5.0):
    """Normalised cross-correlation of x and y against lag (positive: y is later), from averaged cross-spectra."""
    n = s.n_fft
    X, _ = frames(x, n, n // 2)
    Y, _ = frames(y, n, n // 2)
    cross = np.mean((np.conj(X) * Y)[-20:], axis=0)
    freqs = np.fft.rfftfreq(n, 1 / FS)
    band = (freqs >= 20) & (freqs <= 20000)
    cc = np.fft.irfft(cross * band, n)
    norm = np.sqrt(np.sum(np.mean(np.abs(X[-20:]) ** 2, axis=0) * band)
                   * np.sum(np.mean(np.abs(Y[-20:]) ** 2, axis=0) * band)) / (n / 2)
    lags = np.arange(-n // 2, n // 2)
    cc = np.roll(cc, n // 2) / (norm + EPS)
    keep = np.abs(lags) <= max_ms * FS / 1000
    return lags[keep] / FS * 1000, cc[keep]


def phat_peak(x, y, s, max_ms=5.0):
    """PHAT-weighted cross-correlation (every bin's magnitude set to 1): a pure delay is a unit spike at its lag,
    whatever the spectrum. Returns (lag ms, signed peak value, largest value more than 0.1 ms from the peak)."""
    n = s.n_fft
    X, _ = frames(x, n, n // 2)
    Y, _ = frames(y, n, n // 2)
    cross = np.mean((np.conj(X) * Y)[-20:], axis=0)
    freqs = np.fft.rfftfreq(n, 1 / FS)
    band = (freqs >= 20) & (freqs <= 20000)
    w = cross / (np.abs(cross) + EPS) * band
    cc = np.roll(np.fft.irfft(w, n), n // 2) * n / (2 * np.sum(band))  # 1.0 for a pure delay
    lags = (np.arange(n) - n // 2) / FS * 1000
    keep = np.abs(lags) <= max_ms
    cc, lags = cc[keep], lags[keep]
    i = int(np.argmax(np.abs(cc)))
    return lags[i], cc[i], np.max(np.abs(cc[np.abs(lags - lags[i]) > 0.1]))


EXTRA_CASES = [
    Case("rot60+delay2", "sidechain 2 ms later and rotated 60 deg",
         lambda f: np.ones_like(f, dtype=complex), tf_product(tf_rotation(60), tf_delay(2.0)), tf_delay(2.0)),
    Case("lo150", "sidechain through Lo 150 deg", lambda f: np.ones_like(f, dtype=complex),
         lambda f: np.exp(-1j * np.radians(hilo.lag("lo", 150, np.minimum(f, FS / 2 - 1), FS))),
         lambda f: np.ones_like(f, dtype=complex)),
]


def lag_table(s):
    rows = ["| case | plain peak (ms) | PHAT peak (ms) | PHAT value | next highest |", "|---|---|---|---|---|"]
    for case in CASES + EXTRA_CASES:
        rng = np.random.default_rng(3)
        src = pink(int(8 * FS), rng)
        _, z, y = render(case, src, rng)
        lags, cc = lag_function(z, y, s)
        lag, val, nxt = phat_peak(z, y, s)
        rows.append(f"| {case.name} | {lags[np.argmax(cc)]:+.3f} | {lag:+.3f} | {val:+.2f} | {nxt:.2f} |")
    return rows


def mockups(s, case, filename):
    rng = np.random.default_rng(5)
    src = pink(int(12 * FS), rng)
    # Unaligned: the plugin is still at 0 ms; aligned: delay set to the mic spacing.
    x_al, z, y = render(case, src, rng)
    layout = LAYOUTS[1]

    res = analyse(x_al, z, y, layout, s)
    fig = plt.figure(figsize=(14, 9.5), facecolor="#111")
    # A: bars
    ax = screen_axes(fig, [0.05, 0.69, 0.42, 0.26])
    mock_bars(ax, layout, res["proc"][-1], res["unproc"][-1], res["overall_proc"][-1], res["overall_unproc"][-1],
              "A. BANDS (2/3 oct): bar = processed (aligned), tick = unprocessed")
    ax = screen_axes(fig, [0.54, 0.69, 0.42, 0.26])
    lay10 = LAYOUTS[0]
    res10 = analyse(x_al, z, y, lay10, s)
    mock_bars(ax, lay10, res10["proc"][-1], res10["unproc"][-1], res10["overall_proc"][-1],
              res10["overall_unproc"][-1], "A'. BANDS (octave): the same pair")

    # B: continuous trace
    ax = screen_axes(fig, [0.05, 0.37, 0.91, 0.24])
    fc, rp = per_bin_trace(x_al, y, s)
    _, ru = per_bin_trace(z, y, s)
    ax.semilogx(fc, ru, color=DIM, lw=2, label="unprocessed")
    ax.semilogx(fc, rp, color=GREEN, lw=2, label="processed")
    ax.fill_between(fc, 0, rp, color=GREEN, alpha=0.12)
    ax.axhline(0, color=TEXT, lw=1)
    ax.set_ylim(-1.05, 1.05)
    ax.set_xlim(20, 20000)
    ax.set_xticks([20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000],
                  ["20", "50", "100", "200", "500", "1k", "2k", "5k", "10k", "20k"])
    ax.set_yticks([-1, 0, 1], ["-1", "0", "+1"])
    ax.legend(loc="lower left", facecolor="#030302", labelcolor=TEXT, fontsize=8, edgecolor=GRID)
    ax.set_title("B. CURVE: correlation vs frequency (1/6-octave smoothed)", color=TEXT, fontsize=9,
                 family="monospace", loc="left")

    # C: lag view
    ax = screen_axes(fig, [0.05, 0.05, 0.91, 0.24])
    lags, cu = lag_function(z, y, s)
    _, cp = lag_function(x_al, y, s)
    ax.plot(lags, cu, color=DIM, lw=2, label="unprocessed")
    ax.plot(lags, cp, color=GREEN, lw=2, label="processed")
    peak = lags[np.argmax(cu)]
    ax.axvline(0, color=TEXT, lw=1)
    ax.axhline(0, color=GRID, lw=0.6)
    ax.annotate(f"SIDECHAIN {peak:+.2f} ms LATER\nSET DELAY {peak:.2f} ms", (peak, cu[np.argmax(cu)]), (2.2, 0.55),
                color=GREEN, family="monospace", fontsize=9, arrowprops=dict(arrowstyle="->", color=GREEN))
    ax.set_xlim(-5, 5)
    ax.set_ylim(-1.05, 1.05)
    ax.set_yticks([-1, 0, 1], ["-1", "0", "+1"])
    ax.set_xlabel("lag (ms)  [-: this track is later than the sidechain]", color=TEXT, fontsize=8)
    ax.legend(loc="lower left", facecolor="#030302", labelcolor=TEXT, fontsize=8, edgecolor=GRID)
    ax.set_title("C. LAG: cross-correlation vs time offset, +-5 ms", color=TEXT, fontsize=9, family="monospace",
                 loc="left")
    fig.suptitle(f"{case.name}: {case.description}", color=TEXT, family="monospace", fontsize=10)
    fig.savefig(OUT / filename, dpi=110)
    plt.close(fig)
    return peak


def plot_jitter(results, s):
    fig, ax = plt.subplots(figsize=(9, 4.5))
    for layout, jit in results:
        ax.semilogx(layout.centres, jit, "o-", label=layout.name)
    ax.set_xlabel("band centre (Hz)")
    ax.set_ylabel("std of r over time (worst case)")
    ax.set_title(f"P4 jitter, pink noise, N={s.n_fft}, tau=max({s.tau0} s, {s.cycles:g} cycles)")
    ax.grid(True, which="both", alpha=0.3)
    ax.legend()
    fig.tight_layout()
    fig.savefig(OUT / "jitter_by_band.png", dpi=110)
    plt.close(fig)


def plot_drums(res, layout):
    fig, axes = plt.subplots(2, 1, figsize=(10, 6), sharex=True)
    for b in (2, 5, 9, 13):
        r = np.where(res["gated"][:, b], np.nan, res["proc"][:, b])
        axes[0].plot(res["t"], r, label=f"{label(layout.centres[b])} Hz")
    axes[0].set_ylabel("processed r (gated = gap)")
    axes[0].legend(fontsize=8)
    axes[0].set_ylim(-1, 1.05)
    axes[1].plot(res["t"], res["overall_proc"], label="overall processed")
    axes[1].plot(res["t"], res["overall_unproc"], label="overall unprocessed")
    axes[1].set_ylabel("r")
    axes[1].set_xlabel("time (s)")
    axes[1].legend(fontsize=8)
    for a in axes:
        a.grid(alpha=0.3)
    fig.suptitle("P4: mic pair on drums, 2/3-octave bands")
    fig.tight_layout()
    fig.savefig(OUT / "drums_timeline.png", dpi=110)
    plt.close(fig)


RECOMMENDATION = """## Findings and recommendation

**Accuracy is not what limits the design.** With N = 8192 (scaled with the rate), Hann, 50% overlap and
tau = max(0.3 s, 8 cycles), every case reads its closed-form value within 0.04 mean error at any band width from
octave to 2/3 octave, with jitter of 0.04 or less (well under the 0.1 segment step). 1/3 octave is too fine at this FFT
size: the lowest bands get zero or one bin. Longer averaging buys little: tau0 0.5 s cuts the worst jitter from 0.042
to 0.032 but takes the settle time from 0.75 s to 1.25 s. FFT size barely matters for accuracy (4096 to 16384). So:
**keep N = 8192 at 44.1/48 kHz (5.9 Hz bins, needed for the low end of a curve), tau0 = 0.3 s, 8 cycles.**

**What to show.** The mock-ups make the case:

- **Band bars average the comb away.** Two mics 0.85 ms apart cancel at about 590 Hz, 1.8 kHz, 2.9 kHz and so on.
  2/3-octave bands catch the first notch (-0.75 at 500 Hz) but read about 0 above 2 kHz, where the real picture is
  alternating +0.8 / -0.8. Octave bands hide even more. Bars are a coarse version of the next view.
- **A correlation curve (r against frequency, 1/6-octave smoothed) shows exactly what is happening**: where the pair
  cancels, and that alignment lifts the whole curve to about +0.9. For a phase rotation it shows the crossover
  (Hi 90: +1 in the lows, 0 at about 150 Hz, -1 above 1 kHz), which is precisely what the phase knob fixes.
- **A lag view answers "how much delay?" directly**: a spike at the mic spacing (0.854 ms found for 0.85 ms), on any
  material. It must be **PHAT-weighted**: the plain cross-correlation turns a pure phase rotation into a broad hump at
  about +0.9 ms (and Lo 150 into +1.7 ms), which would tell the user to set a delay that isn't there. PHAT keeps rotations at 0 ms (with the sign
  showing an inversion) and finds 2.021 ms for 2 ms plus a 60 deg rotation.

**Recommendation: two meter views, chosen on the screen, plus the overall pair on the right in both.**

1. **FREQUENCY** (default): processed r(f) as a bright line with a faint fill to zero, unprocessed as a dim line,
   20 Hz to 20 kHz on a log axis. Gated stretches (either signal below -70 dBFS) are not drawn.
2. **TIME**: the PHAT lag function from -5 to +5 ms for processed (bright) and unprocessed (dim), with the delay knob's
   0 to 4 ms span marked and the unprocessed peak's lag printed. A negative peak means this track is the later one,
   so the screen says to put the plugin on the other track (the v0.8 rule, applied live).

Overall: unweighted broadband r, processed bar and unprocessed tick, as in the art. All three views come from the same
per-bin averaged cross-spectra, so bars could come back as a third view at no analysis cost.
"""

RECOMMENDATION = RECOMMENDATION.split("\n")


# ----------------------------------------------------------------------------------------------------------------

def main():
    OUT.mkdir(parents=True, exist_ok=True)
    lines = ["# P4: correlation meter on synthetic pairs", "",
             "Generated by `prototype/p4_meter.py`. 48 kHz, pink-noise source unless stated; 30 s per run, first 3 s"
             " ignored, 3 seeds. Both streams are scored: *processed* (the plugin set to align the pair) and *unprocessed*"
             " (as recorded, so misaligned). *Bias* is the mean error against the closed-form r for the band (same band"
             " edges); *jitter* is the standard deviation of r over time; *worst error* is the largest instantaneous"
             " error. Each is the worst over bands, streams and seeds.", ""]

    lines += ["## Cases", ""]
    for c in CASES:
        lines.append(f"- **{c.name}**: {c.description}")
    lines.append("")

    # 1. Layouts at the plan's averaging.
    base = Settings()
    lines += [f"## 1. Band layouts (N = {base.n_fft}, tau = max({base.tau0} s, {base.cycles:g} cycles))", ""]
    jitter_results = []
    for layout in LAYOUTS:
        rows, jit = accuracy_table(layout, base)
        jitter_results.append((layout, jit))
        lines += [f"### {layout.name}", "", "| case | max abs bias | worst-band jitter | worst error | at band |",
                  "|---|---|---|---|---|"]
        for name, bias, j, worst, at in rows:
            lines.append(f"| {name} | {bias:.3f} | {j:.3f} | {worst:.3f} | {label(at)} |")
        lines.append("")
    plot_jitter(jitter_results, base)

    # Bins per band at 48 kHz.
    lines += ["Bins per band at N = 8192, 48 kHz (lowest five bands):", ""]
    freqs = np.fft.rfftfreq(base.n_fft, 1 / FS)
    for layout in LAYOUTS:
        lo, hi = layout.edges()
        counts = [int(np.sum((freqs >= a) & (freqs < b))) for a, b in zip(lo, hi)]
        lines.append(f"- {layout.name}: {counts[:5]}" + (f" ({counts.count(0)} bands have no bin at all)"
                                                          if 0 in counts else ""))
    lines.append("")

    # 2. Averaging and FFT size, 2/3 octave.
    layout = LAYOUTS[1]
    lines += ["## 2. Averaging and FFT size (2/3 octave; jitter is the worst over all cases)", "",
              "| N | tau0 (s) | cycles | worst-band jitter | jitter at 20 / 200 / 2k Hz | settle 20 Hz | settle 200 Hz |"
              " settle 2k | settle overall |", "|---|---|---|---|---|---|---|---|---|"]
    variants = [Settings(8192, 0.3, 8), Settings(8192, 0.5, 8), Settings(8192, 1.0, 8), Settings(8192, 0.3, 16),
                Settings(4096, 0.3, 8), Settings(16384, 0.3, 8), Settings(8192, 0.5, 16)]
    idx = [int(np.argmin(np.abs(layout.centres - f))) for f in (20, 200, 2000)]
    settle_rows = {}
    for v in variants:
        rows, jit = accuracy_table(layout, v, seeds=(1, 2))
        micpair_jit = jit  # worst over cases per band
        st, so = settle_times(layout, v)
        settle_rows[(v.n_fft, v.tau0, v.cycles)] = st
        lines.append(f"| {v.n_fft} | {v.tau0} | {v.cycles:g} | {np.max(micpair_jit):.3f} | "
                     f"{' / '.join(f'{micpair_jit[i]:.3f}' for i in idx)} | "
                     f"{st[idx[0]]:.2f} s | {st[idx[1]]:.2f} s | {st[idx[2]]:.2f} s | {so:.2f} s |")
    lines.append("")
    lines += ["*Settle*: a polarity step on an otherwise perfect pair (r from +1 to -1); the time until r < -0.8.", ""]

    # 3. Drums.
    gated, jit, res = drums_table(layout, base)
    plot_drums(res, layout)
    lines += ["## 3. Drums (micpair transfer functions, 2/3 octave, plan averaging)", "",
              "| band | " + " | ".join(label(c) for c in layout.centres) + " |",
              "|---|" + "---|" * len(layout.centres),
              "| gated | " + " | ".join(f"{g:.0%}" for g in gated) + " |",
              "| jitter | " + " | ".join("-" if np.isnan(j) else f"{j:.2f}" for j in jit) + " |", ""]

    # 4. Mock-ups.
    peak = mockups(base, CASES[-1], "mockups_micpair.png")
    mockups(base, CASES[3], "mockups_hi90.png")
    lines += ["## 4. Display mock-ups", "", "`mockups_micpair.png`: the micpair case (0.85 ms apart) before and after"
              f" setting the delay. The lag view's peak finds the spacing as {peak:.2f} ms. `mockups_hi90.png`: a"
              " pure phase rotation (Hi 90), before and after.", ""]

    lines += ["## 5. Lag detection (unprocessed vs sidechain, pink noise)", "",
              "The plain cross-correlation peak is what the C mock-ups mark. PHAT sets every bin's magnitude to 1"
              " first, so a delay is a unit spike whatever the spectrum, and a phase rotation stays at 0 ms.", ""]
    lines += lag_table(base) + [""]
    lines += RECOMMENDATION
    (OUT / "report.md").write_text("\n".join(lines))
    print("\n".join(lines))


if __name__ == "__main__":
    main()
