"""ANALYSE groundwork (PLAN 3.5, 8 step 6): the auto-suggest search, prototyped offline.

    source .venv/bin/activate && python prototype/analyse.py          # synthetic checks, then the user's pairs
    ... python prototype/analyse.py --synthetic                         # only the known-answer checks

Input = the track (what the plugin changes), sidechain = the reference. Both are captured, the cross-spectrum of the
whole capture is taken once (Hann 8192 at 48 kHz, 75% overlap, scaled with the rate), and every candidate setting is
scored from it in closed form: a candidate turns the input's spectrum by G(f) = s * H(f) * exp(-j 2 pi f d), where s is the
polarity (+-1), H the phase stage's response at a static setting (the same formulas as src/dsp/PhaseResponse.h) and d the
delay, and its score is the band-limited Pearson correlation with the sidechain,

    r_b = sum_{k in b} Re(G_k Sxy_k) / sqrt(Sxx_b Syy_b),      score = mean of r_b over the active bands.

Equal weight per 1/3-octave band (so low frequencies don't dominate, PLAN 3.5); a band is active when both signals have
energy within 40 dB of their own loudest band. Delay is searched jointly with the phase setting: for each (shape, angle)
one inverse FFT per band gives r_b at every lag at once (zero-padded 4x, so a quarter of a sample); polarity is the sign of
the score. Candidates are then ranked and de-duplicated into families; the top few are refined to 0.1 sample.

The search is checked against the plugin's own DSP: the best Hi/Lo candidates are rendered through
`hilo.HiLoOversampled` and the delay applied exactly (FFT shift), and the rendered score is compared with the predicted one.

Outputs: prototype/out/analyse/report.md
"""
import math
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from scipy.signal import hilbert

sys.path.insert(0, str(Path(__file__).resolve().parent))
import hilo  # noqa: E402
from oversampling import plan  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT = Path(__file__).resolve().parent / "out" / "analyse"
PAD = 4                 # zero-padding of the lag search (a quarter of a sample)
MAX_DELAY_MS = 4.0      # the plugin's DELAY knob reaches +-4 ms
BAND_LO, BAND_HI = 40.0, 16000.0
ACTIVE_DB = -40.0
STEP = 2.5              # panel degrees between angle candidates
SHAPES = [("lo", False), ("lo", True), ("hi", False), ("hi", True)]
FAMILY_DELAY_MS = 0.5   # two candidates with the same polarity, shape and a delay and angle this close are one family


# --- the cross-spectrum ----------------------------------------------------------------------------------------------
def frame_size(fs):
    return 8192 * (1 if fs < 60000 else 2 if fs < 120000 else 4)


@dataclass
class Spectra:
    fs: float
    n: int
    sxy: np.ndarray    # sum over frames of X Y*, X = input, Y = sidechain
    sxx: np.ndarray
    syy: np.ndarray
    edges: list        # band bin ranges, active ones only
    centres: list

    @property
    def freqs(self):
        return np.arange(self.n // 2 + 1) * self.fs / self.n


def spectra(x, y, fs):
    n = frame_size(fs)
    win = np.hanning(n)
    hop = n // 4
    count = (len(x) - n) // hop + 1
    sxy = np.zeros(n // 2 + 1, complex)
    sxx = np.zeros(n // 2 + 1)
    syy = np.zeros(n // 2 + 1)
    for i in range(count):
        a = np.fft.rfft(win * x[i * hop:i * hop + n])
        b = np.fft.rfft(win * y[i * hop:i * hop + n])
        sxy += a * np.conj(b)
        sxx += np.abs(a) ** 2
        syy += np.abs(b) ** 2
    freqs = np.arange(n // 2 + 1) * fs / n
    c = 2 ** (1 / 3)
    centres, edges = [], []
    f = 31.5
    while f < BAND_HI * 1.1:
        lo, hi = f / 2 ** (1 / 6), f * 2 ** (1 / 6)
        if lo >= BAND_LO * 0.9 and hi <= min(BAND_HI * 1.1, fs / 2):
            k = np.nonzero((freqs >= lo) & (freqs < hi))[0]
            if len(k) >= 2:
                centres.append(f)
                edges.append((k[0], k[-1] + 1))
        f *= c
    px = np.array([sxx[a:b].sum() for a, b in edges])
    py = np.array([syy[a:b].sum() for a, b in edges])
    keep = (px >= px.max() * 10 ** (ACTIVE_DB / 10)) & (py >= py.max() * 10 ** (ACTIVE_DB / 10))
    edges = [e for e, k in zip(edges, keep) if k]
    centres = [f for f, k in zip(centres, keep) if k]
    return Spectra(fs, n, sxy, sxx, syy, edges, centres)


# --- the phase stage's response (src/dsp/PhaseResponse.h) --------------------------------------------------------------
def stage_response(mode, wide, theta, fs, freqs):
    """H(f) of the phase stage at a static setting: mode 'lo' / 'hi' / 'constant', theta the panel angle."""
    if mode == "constant":
        return np.full(len(freqs), np.exp(-1j * math.radians(theta)))
    rate = fs * plan(fs)[0]
    phi = hilo.knob_angles(theta, wide)[0]
    k1, k2 = hilo.k_angles((mode, wide), *hilo.angles((mode, wide), phi), rate)
    z = np.exp(-2j * np.pi * freqs / rate)
    h = np.ones(len(freqs), complex)
    for k in (k1, k2):
        p = (k - 1.0) / (k + 1.0)
        h *= (-p + z) / (1.0 - p * z)
    return h


def settings():
    """Every (mode, wide, theta) the search covers: Hi/Lo in both ranges over their travel, and Constant 0 to 180."""
    out = [("none", False, 0.0)]
    for mode, wide in SHAPES:
        top = 180.0 if wide else 90.0
        out += [(mode, wide, t) for t in np.arange(STEP, top + 1e-9, STEP)]
    out += [("constant", True, t) for t in np.arange(STEP, 180.0 + 1e-9, STEP)]
    return out


# --- the search ---------------------------------------------------------------------------------------------------------
@dataclass(frozen=True)
class Candidate:
    mode: str
    wide: bool
    theta: float
    delay_ms: float
    flip: bool
    score: float

    def label(self):
        phase = "phase off" if self.mode == "none" else (
            f"CONSTANT {self.theta:.1f}°" if self.mode == "constant" else
            f"{self.mode.upper()} {'in' if self.wide else 'out'} {self.theta:.1f}°")
        return f"{phase}, {'Ø ' if self.flip else ''}delay {self.delay_ms:+.2f} ms"


def band_scores(sp, g_k):
    """Per band r_b at every lag: rows are bands, columns lags -L..+L in 1/PAD samples (index m = lag * PAD)."""
    n = sp.n * PAD
    reach = int(math.ceil(MAX_DELAY_MS * 1e-3 * sp.fs)) * PAD
    out = np.empty((len(sp.edges), 2 * reach + 1))
    lags = np.arange(-reach, reach + 1)
    for i, (a, b) in enumerate(sp.edges):
        spec = np.zeros(n // 2 + 1, complex)
        spec[a:b] = g_k[a:b] * sp.sxy[a:b]
        y = np.fft.irfft(spec, n) * (n / 2.0)       # y[m] = sum_k Re(spec_k e^{+j 2 pi k m / n}), k per N-point bin
        norm = math.sqrt(sp.sxx[a:b].sum() * sp.syy[a:b].sum())
        out[i] = y[(-lags) % n] / norm               # the index -d: e^{-j w d}
    return out, lags / PAD


def score_at(sp, g_k):
    """The score of one fully specified G (no lag search): mean over bands of Re(sum G Sxy) / sqrt(Sxx Syy)."""
    r = [np.real((g_k[a:b] * sp.sxy[a:b]).sum()) / math.sqrt(sp.sxx[a:b].sum() * sp.syy[a:b].sum()) for a, b in sp.edges]
    return float(np.mean(r))


def search(sp, top=6, verbose=False):
    t0 = time.time()
    freqs = sp.freqs
    best = []    # (score, mode, wide, theta, delay, flip)
    for mode, wide, theta in settings():
        h = stage_response(mode, wide, theta, sp.fs, freqs) if mode != "none" else np.ones(len(freqs))
        r, lags = band_scores(sp, h)
        s = r.mean(0)
        for flip, sign in ((False, 1.0), (True, -1.0)):
            v = sign * s
            for j in np.argsort(v)[::-1][:3]:     # a few peaks per setting, so families survive de-duplication
                best.append((v[j], mode, wide, theta, lags[j], flip))
    best.sort(key=lambda c: -c[0])
    # refine the leaders to 0.1 sample, then de-duplicate into families
    cands = []
    for score, mode, wide, theta, d, flip in best[:200]:
        h = stage_response(mode, wide, theta, sp.fs, freqs) if mode != "none" else np.ones(len(freqs))
        sign = -1.0 if flip else 1.0
        bestc = (-9.0, d)
        for dd in np.arange(d - 0.3, d + 0.3001, 0.1):
            if abs(dd) > MAX_DELAY_MS * 1e-3 * sp.fs:
                continue
            g = sign * h * np.exp(-2j * np.pi * freqs * dd / sp.fs)
            bestc = max(bestc, (score_at(sp, g), dd))
        cands.append(Candidate(mode, wide, theta, bestc[1] / sp.fs * 1e3, flip, bestc[0]))
    cands.sort(key=lambda c: -c.score)
    families = []
    for c in cands:
        same = lambda f: (f.flip == c.flip and (f.mode, f.wide) == (c.mode, c.wide)
                          and abs(f.delay_ms - c.delay_ms) < FAMILY_DELAY_MS and abs(f.theta - c.theta) < 20.0)
        if not any(same(f) for f in families):
            families.append(c)
        if len(families) == top:
            break
    baseline = score_at(sp, np.ones(len(freqs)))
    if verbose:
        print(f"  search {time.time() - t0:.1f} s, {len(settings())} settings, {len(sp.edges)} bands")
    return families, baseline


# --- render check against the plugin's own DSP -----------------------------------------------------------------------------
def render(x, fs, c):
    """The input through the candidate, with the plugin's Hi/Lo (or a true rotation for CONSTANT), the delay exact."""
    y = np.asarray(x, np.float64)
    if c.flip:
        y = -y
    if c.mode in ("lo", "hi"):
        stage = hilo.HiLoOversampled(fs, c.mode, c.theta, c.wide)
        out = stage.process(y)[0]
        y = np.concatenate([out[stage.latency:], np.zeros(stage.latency)])    # the latency is compensated by the host
    elif c.mode == "constant":
        th = math.radians(c.theta)
        y = math.cos(th) * y + math.sin(th) * np.imag(hilbert(y))             # exact rotation (the FIR matches it above ~50 Hz)
    n = len(y)
    spec = np.fft.rfft(y, 2 * n)
    spec *= np.exp(-2j * np.pi * np.arange(len(spec)) * (c.delay_ms * 1e-3 * fs) / (2 * n))
    return np.fft.irfft(spec, 2 * n)[:n]


def rendered_score(x, y, fs, c):
    sp = spectra(render(x, fs, c), y, fs)
    return score_at(sp, np.ones(len(sp.freqs)))


# --- synthetic known-answer pairs -------------------------------------------------------------------------------------------
def synthetic(fs, delay_ms, angle, seed):
    """A sidechain of band-limited impulsive noise, and an input that is that sidechain delayed and phase-rotated, so that the
    right answer is: delay the input by -delay_ms... built the other way round, the input is what the plugin must undo."""
    rng = np.random.default_rng(seed)
    n = fs * 8
    t = np.arange(n) / fs
    env = np.zeros(n)
    for hit in np.arange(0.2, 7.5, 0.37):
        i = int(hit * fs)
        env[i:i + int(0.25 * fs)] += np.exp(-np.arange(int(0.25 * fs)) / (0.06 * fs))
    y = env * rng.standard_normal(n)
    # coloured: a low shelf and some top, so no band is empty
    spec = np.fft.rfft(y)
    f = np.fft.rfftfreq(n, 1 / fs)
    spec *= 1.0 / np.sqrt(1 + (f / 800.0) ** 2) + 0.05
    y = np.fft.irfft(spec, n)
    x = np.fft.rfft(y)
    x *= np.exp(-2j * np.pi * f * delay_ms * 1e-3) * np.exp(+1j * math.radians(angle))   # the input lags and leads the sidechain
    x = np.fft.irfft(x, n)
    return x, y


def synthetic_checks(report):
    fs = 48000
    report.append("## Synthetic pairs (known answer)\n")
    report.append("The input is the sidechain turned by a constant rotation and a delay; the search must find the inverse "
                  "(Constant at that angle, delay of that size).\n")
    report.append("| built as | best | score | baseline r | 2nd family |\n|---|---|---|---|---|")
    ok = True
    for delay, angle in [(0.0, 0.0), (1.7, 0.0), (-2.3, 0.0), (0.0, 60.0), (1.0, 90.0), (0.0, 180.0), (-1.25, 120.0)]:
        x, y = synthetic(fs, delay, angle, 7)
        # the input's spectrum is Y e^{-j w delay} e^{+j angle}: to undo, apply e^{+j w delay} (delay -delay) and e^{-j angle}
        sp = spectra(x, y, fs)
        fam, base = search(sp, top=2)
        best = fam[0]
        want_delay = -delay
        want_theta = angle % 360.0
        ok_here = abs(best.delay_ms - want_delay) < 0.05 and (
            (best.mode == "constant" and abs(best.theta - want_theta) <= STEP) or (angle == 0.0 and best.mode == "none"))
        # 180 degrees can equally be a polarity flip
        if angle == 180.0 and best.flip and best.mode == "none":
            ok_here = abs(best.delay_ms - want_delay) < 0.05
        ok &= ok_here
        second = fam[1].label() + f" ({fam[1].score:.3f})" if len(fam) > 1 else "-"
        report.append(f"| delay {delay:+.2f} ms, rotation {angle:.0f}° | {best.label()} {'✓' if ok_here else '✗'} | {best.score:.3f} | "
                      f"{base:+.3f} | {second} |")
        print(f"synthetic {delay:+.2f} ms {angle:5.1f}°  →  {best.label():45s} {best.score:.3f}  {'ok' if ok_here else 'MISS'}")
    report.append("")
    return ok


# --- the user's pairs -----------------------------------------------------------------------------------------------------------
PAIRS = ["kick", "snare", "bass", "guitar", "hats"]


def user_pairs(report):
    fs = 48000
    report.append("## The user's stem pairs\n")
    report.append("Input = the `_b` take, sidechain = the `_a` take (`prototype/export_pairs.py`). Scores are band-averaged r; "
                  "`rendered` is the same candidate run through the plugin's Hi/Lo and an exact delay.\n")
    for tag in PAIRS:
        try:
            a = np.fromfile(ROOT / "captures" / f"{tag}_a.f32", np.float32).astype(np.float64)
            b = np.fromfile(ROOT / "captures" / f"{tag}_b.f32", np.float32).astype(np.float64)
        except FileNotFoundError:
            print(f"{tag}: captures/{tag}_*.f32 missing (run prototype/export_pairs.py)")
            continue
        sp = spectra(b, a, fs)
        fam, base = search(sp, top=5, verbose=True)
        print(f"\n{tag}: baseline r {base:+.3f}, {len(sp.edges)} active bands ({sp.centres[0]:.0f} to {sp.centres[-1]:.0f} Hz)")
        report.append(f"### {tag}\n\nBaseline (everything off): r = {base:+.3f}; {len(sp.edges)} active bands, "
                      f"{sp.centres[0]:.0f} to {sp.centres[-1]:.0f} Hz.\n")
        report.append("| # | setting | predicted r | rendered r |\n|---|---|---|---|")
        for i, c in enumerate(fam):
            rend = rendered_score(b, a, fs, c)
            print(f"  {i + 1}. {c.label():48s} predicted {c.score:+.3f}  rendered {rend:+.3f}")
            report.append(f"| {i + 1} | {c.label()} | {c.score:+.3f} | {rend:+.3f} |")
        report.append("")


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    report = ["# ANALYSE groundwork: search prototype\n", "Generated by `prototype/analyse.py`. Method in its docstring.\n"]
    ok = synthetic_checks(report)
    if "--synthetic" not in sys.argv:
        user_pairs(report)
    (OUT / "report.md").write_text("\n".join(report) + "\n")
    print(f"\nwrote {OUT / 'report.md'}; synthetic checks {'all passed' if ok else 'HAD MISSES'}")


if __name__ == "__main__":
    main()
