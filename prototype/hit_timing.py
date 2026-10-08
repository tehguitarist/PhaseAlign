"""Hit timing: where a track's hits start against a sidechain's, from the first rise of the high-passed envelope, hit by hit.

A drum hit starts with a burst of jumping broadband content before it settles into its body (the user, 2026-10-08), and the body is
periodic, so a waveform score (and the attack reading's smoothed low band) can lock onto a whole cycle of the body: the user's snare
sample scored best 5.9 ms and 17 to 20 ms away, one and three periods of its 170 Hz body, though it is within 100 samples. The first
rise of the envelope has no such repeat. For each hit found in the sidechain, the first time the envelope reaches `frac` of its local
peak is taken in both signals; the median over the hits is the offset, and the spread of the differences (interquartile range) says
how far to trust it.

The result is in the plugin's convention (positive delays the track): a track whose hits start later than the sidechain's gets a
negative one. Measured on the user's stems (2026-10-08): kick and snare samples against their mics -29 to +47 samples with a spread
under 0.2 ms; a snare bottom +42; close mics agree with the attack reading's sign; room mics and the song's busy mix have no
common onset (spread of 15 ms and more), which is the spread's job to say.

    HitTiming = (lag_ms, spread_ms, hits)  or None when there are fewer than MIN_HITS hits
"""
import numpy as np
from scipy.signal import butter, sosfilt

HP_HZ = 1500.0
SMOOTH_MS = 0.4
FRAC = 0.25
WINDOW_MS = 20.0            # how far from the sidechain's onset the track's own is looked for
THRESHOLD_DB = -30.0        # a hit is where the sidechain's envelope crosses this far under its maximum
REFRACTORY_MS = 60.0        # hits closer than this (a roll, a flam) are skipped: they smear each other
MIN_HITS = 10
TIGHT_MS = 1.0              # a spread under this counts as a clear reading


def envelope(x, fs, hp=HP_HZ, smooth_ms=SMOOTH_MS):
    sos = butter(4, hp, "highpass", fs=fs, output="sos")
    k = max(1, int(smooth_ms * 1e-3 * fs))
    return np.convolve(np.abs(sosfilt(sos, x)), np.ones(k) / k, "same")


def _first_rise(env, lo, hi, frac):
    seg = env[lo:hi]
    if len(seg) < 4 or seg.max() <= 0:
        return None
    return lo + int(np.argmax(seg >= frac * seg.max()))


def hit_timing(x, y, fs, hp=HP_HZ):
    """(lag_ms, spread_ms, hits) of the track x against the sidechain y, or None."""
    ey, ex = envelope(y, fs, hp), envelope(x, fs, hp)
    thr = ey.max() * 10 ** (THRESHOLD_DB / 20)
    gap, back, post, W = int(REFRACTORY_MS * 1e-3 * fs), int(0.015 * fs), int(0.012 * fs), int(WINDOW_MS * 1e-3 * fs)
    diffs, last = [], -10 ** 9
    for i in np.nonzero((ey[1:] >= thr) & (ey[:-1] < thr))[0] + 1:
        if i - last < gap or i < back + W + 1 or i + post + W >= len(y):
            continue
        if ey[i - back:i - 1].mean() * 3.0 >= ey[i:i + post].max():     # not a rise out of quiet: part of a roll or a tail
            continue
        last = i
        oy = _first_rise(ey, i - int(0.004 * fs), i + int(0.004 * fs), FRAC)
        ox = _first_rise(ex, i - W, i + W, FRAC)
        if oy is not None and ox is not None:
            diffs.append(ox - oy)
    if len(diffs) < MIN_HITS:
        return None
    d = np.array(diffs) / fs * 1e3
    return -float(np.median(d)), float(np.percentile(d, 75) - np.percentile(d, 25)), len(d)
