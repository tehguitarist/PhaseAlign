"""ANALYSE groundwork (PLAN 3.5, 8 step 6): the auto-suggest search, prototyped offline.

    source .venv/bin/activate && python prototype/analyse.py          # synthetic checks, then the user's pairs
    ... python prototype/analyse.py --synthetic                         # only the known-answer checks

Input = the track (what the plugin changes), sidechain = the reference. Positive delay = delay the input. Both are captured, the cross-spectrum of the
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
MAX_DELAY_MS = 6.0      # the search's reach; the plugin's DELAY knob reaches +-4 ms today (the user wants 6, 2026-10-07)
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


def search(sp, top=6, verbose=False, window_ms=None):
    """window_ms = (lo, hi) restricts the delay to that range (the attack lag's neighbourhood); None searches the knob's reach."""
    t0 = time.time()
    freqs = sp.freqs
    best = []    # (score, mode, wide, theta, delay, flip)
    lo_s, hi_s = (-MAX_DELAY_MS, MAX_DELAY_MS) if window_ms is None else window_ms
    lo_s, hi_s = max(lo_s, -MAX_DELAY_MS) * 1e-3 * sp.fs, min(hi_s, MAX_DELAY_MS) * 1e-3 * sp.fs
    for mode, wide, theta in settings():
        h = stage_response(mode, wide, theta, sp.fs, freqs) if mode != "none" else np.ones(len(freqs))
        r, lags = band_scores(sp, h)
        s = r.mean(0)
        inside = (lags >= lo_s) & (lags <= hi_s)
        for flip, sign in ((False, 1.0), (True, -1.0)):
            v = np.where(inside, sign * s, -9.0)
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
            if dd < lo_s - 1e-9 or dd > hi_s + 1e-9:
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


# --- the attack lag (plan R18, offline) ----------------------------------------------------------------------------------------
ATTACK_BANDS = [(35.0, 150.0, 12.0), (150.0, 600.0, 5.0), (600.0, None, 1.5)]   # low Hz, high Hz, smoothing ms
ATTACK_RUNNER_UP = 0.65     # a peak is clear when the strongest rival outside its lobe is below this fraction of it
SEARCH_MS = 40.0


def attack_features(x, fs):
    """Per band: the log of the smoothed magnitude, positive differences only (log-envelope flux)."""
    from scipy.signal import butter, sosfilt
    feats = []
    for lo, hi, ms in ATTACK_BANDS:
        sos = (butter(4, [lo, min(hi, fs * 0.45)], "bandpass", fs=fs, output="sos") if hi
               else butter(4, lo, "highpass", fs=fs, output="sos"))
        m = np.abs(sosfilt(sos, x))
        n = max(1, int(round(ms * 1e-3 * fs)))
        m = np.convolve(m, np.ones(n) / n, mode="same")
        lg = np.log(m + 1e-6)
        feats.append(np.maximum(np.diff(lg, prepend=lg[0]), 0.0))
    return feats


def attack_lag(x, y, fs):
    """The delay (ms, positive = delay the input to meet the sidechain) from the attacks, the three bands' cross-correlations
    (each normalised) averaged. Returns (lag_ms, clear, strength, runner_up_ratio)."""
    fx, fy = attack_features(x, fs), attack_features(y, fs)
    reach = int(SEARCH_MS * 1e-3 * fs)
    n = 1 << int(np.ceil(np.log2(len(x) + reach)))
    acc = np.zeros(2 * reach + 1)
    for a, b in zip(fx, fy):
        a, b = a - a.mean(), b - b.mean()
        c = np.fft.irfft(np.fft.rfft(a, n) * np.conj(np.fft.rfft(b, n)), n)    # c[m] = sum a[t+m] b[t]: positive m = a later
        c = np.concatenate([c[-reach:], c[:reach + 1]]) / math.sqrt((a * a).sum() * (b * b).sum())
        acc += c / len(fx)
    acc = acc[::-1]     # c's lag m > 0 means the input is later, so the delay that aligns is d = -m; reversed, index i is d = i - reach
    d = np.arange(-reach, reach + 1)
    k = int(np.argmax(acc))
    peak = acc[k]
    lobe = int(0.004 * fs)
    rest = np.concatenate([acc[:max(0, k - lobe)], acc[k + lobe:]])
    runner = rest.max() / peak if peak > 0 and len(rest) else 1.0
    frac = 0.0
    if 0 < k < len(acc) - 1:
        y0, y1, y2 = acc[k - 1], acc[k], acc[k + 1]
        den = y0 - 2 * y1 + y2
        frac = 0.5 * (y0 - y2) / den if den != 0 else 0.0
    return (d[k] + frac) / fs * 1e3, bool(peak > 0 and runner < ATTACK_RUNNER_UP), float(peak), float(runner)


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


def analyse(x, y, fs, top=5):
    """The delay-first search: the attack lag fixes the delay (when it is clear and inside the knob's reach), then phase and
    polarity are chosen at that delay (refined by up to +-0.3 ms, the score's own say). Falls back to the joint search.
    Returns (candidates, baseline r, attack reading, which method)."""
    sp = spectra(x, y, fs)
    lag = attack_lag(x, y, fs)
    ms, clear = lag[0], lag[1]
    if clear and abs(ms) <= MAX_DELAY_MS:
        fam, base = search(sp, top=top, window_ms=(ms - 0.3, ms + 0.3))
        return fam, base, lag, "attack"
    fam, base = search(sp, top=top)
    return fam, base, lag, "score" if not clear else "score (attack out of range)"


MIN_GAIN = 0.03      # r gained over doing nothing below which ANALYSE says there is nothing worth changing
MIN_DELAY_MS = 0.3   # a clear attack offset at least this large is worth a delay on its own
MIN_MARGIN = 0.01    # r lead over the next family below which the pick is only tentative


def verdict(fam, base, lag=None):
    """(label, gain, margin): what ANALYSE would tell the user about the leading candidate."""
    gain = fam[0].score - base
    margin = fam[0].score - fam[1].score if len(fam) > 1 else gain
    if gain < MIN_GAIN and lag is not None and lag[1] and MIN_DELAY_MS <= abs(lag[0]) <= MAX_DELAY_MS:
        # the waveforms barely correlate, but the attacks clearly sit apart: the delay alone is worth suggesting
        return f"delay only, {lag[0]:+.2f} ms (phase and polarity add nothing the score can see)", gain, margin
    if gain < MIN_GAIN:
        return "no change worth making", gain, margin
    return ("suggest" if margin >= MIN_MARGIN else "tentative: several settings score alike"), gain, margin


def write_renders(tag, a, b, fam, fs):
    """For listening: the sidechain, the input as it is, and the input through each leading candidate, each also summed with the
    sidechain (what two mics sound like together). One common gain, so levels compare."""
    import soundfile as sf
    d = OUT / "renders" / tag
    d.mkdir(parents=True, exist_ok=True)
    clips = {"sidechain": b, "input_off": a, "sum_off": a + b}
    for i, c in enumerate(fam[:3]):
        y = render(a, fs, c)
        clips[f"input_cand{i + 1}"] = y
        clips[f"sum_cand{i + 1}"] = y + b
    gain = 0.9 / max(np.abs(v).max() for v in clips.values())
    for name, v in clips.items():
        sf.write(d / f"{name}.wav", (v * gain).astype(np.float32), int(fs), subtype="FLOAT")
    (d / "candidates.txt").write_text("\n".join(f"cand{i + 1}: {c.label()} (r {c.score:+.3f})" for i, c in enumerate(fam[:3])) + "\n")


def user_pairs(report):
    fs = 48000
    report.append("## The user's stem pairs\n")
    report.append("Input = the `_a` take, sidechain = the `_b` take (as the meter tests read them; `prototype/export_pairs.py`). "
                  "Positive delay = delay the input. `joint` is the first version (score alone, whole ±4 ms); `attack-first` takes "
                  "the delay from the attack lag and chooses phase and polarity within 0.3 ms of it. `rendered` is the candidate "
                  "run through the plugin's Hi/Lo and an exact delay. The verdict is what ANALYSE would say: nothing worth changing "
                  f"below a gain of {MIN_GAIN}, tentative below a lead of {MIN_MARGIN}. Listening files: `renders/<pair>/`.\n")
    for tag in PAIRS:
        try:
            a = np.fromfile(ROOT / "captures" / f"{tag}_a.f32", np.float32).astype(np.float64)
            b = np.fromfile(ROOT / "captures" / f"{tag}_b.f32", np.float32).astype(np.float64)
        except FileNotFoundError:
            print(f"{tag}: captures/{tag}_*.f32 missing (run prototype/export_pairs.py)")
            continue
        sp = spectra(a, b, fs)
        joint, base = search(sp, top=3)
        fam, _, lag, how = analyse(a, b, fs)
        print(f"\n{tag}: baseline r {base:+.3f}; attack lag {lag[0]:+.2f} ms ({'clear' if lag[1] else 'unclear'}, "
              f"peak {lag[2]:.2f}, rival {lag[3]:.2f}); delay from {how}")
        report.append(f"### {tag}\n\nBaseline r = {base:+.3f}. Attack lag {lag[0]:+.2f} ms "
                      f"({'clear' if lag[1] else 'unclear'}; peak {lag[2]:.2f}, strongest rival {lag[3]:.2f} of it). Delay taken from: {how}.\n")
        label, gain, margin = verdict(fam, base, lag)
        print(f"  verdict: {label} (gain {gain:+.3f}, margin {margin:+.3f})")
        report.append(f"**Verdict: {label}** (gain over doing nothing {gain:+.3f}, lead over the next family {margin:+.3f}).\n")
        write_renders(tag, a, b, fam, fs)
        report.append("| version | # | setting | predicted r | rendered r |\n|---|---|---|---|---|")
        for name, lst in (("joint", joint), ("attack-first", fam)):
            for i, c in enumerate(lst[:3]):
                rend = rendered_score(a, b, fs, c)
                print(f"  {name:12s} {i + 1}. {c.label():48s} predicted {c.score:+.3f}  rendered {rend:+.3f}")
                report.append(f"| {name} | {i + 1} | {c.label()} | {c.score:+.3f} | {rend:+.3f} |")
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
