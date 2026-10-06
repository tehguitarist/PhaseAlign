"""Sub-sample delay: is it worth it, what resolution, and which interpolator (user question, 2026-10-06).

    .venv/bin/python prototype/subsample.py      # → prototype/out/subsample/ (report.md, interpolators.png)

Part 1, what the rounding costs: a whole-sample delay leaves up to half a sample of residual offset between the two
mics. Summing two equal, coherent signals offset by tau gives |cos(pi f tau)|, a high-frequency dip. A grid of g samples
leaves up to g/2.

Part 2, what a fractional delay costs: it needs an interpolator. Candidates, all evaluated at the worst fraction (0.5)
and at 0.1 / 0.25, from 20 Hz to 20 kHz at 44.1 and 48 kHz (the hard rates: least room above 20 kHz):
  - linear (2 taps): cheap, but a low-pass at fraction 0.5;
  - Lagrange 3rd order (4 taps);
  - Kaiser-windowed sinc (8 to 64 taps): linear phase, adds N/2 samples of latency, nearly flat if long enough;
  - Thiran all-pass (order 3): flat level by construction, delay error at the top, recursive (state makes moving it
    awkward).

Part 3, the shortest Kaiser-windowed sinc per rate (what the plugin shipped first).

Part 4, optimised kernels for the least latency (2026-10-06): a minimax design per fraction by linear programming,
centred (H = N/2 - 1) and low-delay (H free), searched for the smallest lookahead H, then the fewest taps N. Writes
kernels_<rate>.txt next to the report.
"""

import math
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from scipy.optimize import linprog
from scipy.signal import freqz

OUT = Path(__file__).resolve().parent / "out" / "subsample"
RATES = [44100, 48000, 96000]
GRIDS = [1.0, 0.5, 0.1, 0.01]
CHECK_HZ = [5000, 10000, 16000, 20000]
FRACTIONS = [0.1, 0.25, 0.5]


# --- part 1 ------------------------------------------------------------------------------------------------------
def residual_dip_db(f, tau_samples, fs):
    return 20 * math.log10(abs(math.cos(math.pi * f * tau_samples / fs)))


# --- part 2: interpolators. Each returns (taps b, denominator a, nominal delay in samples) --------------------------
def linear(frac):
    return np.array([1 - frac, frac]), np.array([1.0]), frac


def lagrange3(frac):
    d = 1.0 + frac  # centred: delay between taps 1 and 2
    b = np.ones(4)
    for k in range(4):
        for m in range(4):
            if m != k:
                b[k] *= (d - m) / (k - m)
    return b, np.array([1.0]), d


def windowed_sinc(frac, taps, beta):
    """The kernel the plugin uses (src/dsp/FractionalDelay.h): a sinc centred at taps/2 - 1 + frac, under a Kaiser
    window of half-width taps/2 centred at the same point, normalised to unity at DC."""
    d = taps // 2 - 1 + frac
    x = np.arange(taps) - d
    w = np.i0(beta * np.sqrt(np.clip(1.0 - (x / (taps / 2)) ** 2, 0.0, None))) / np.i0(beta)
    b = np.sinc(x) * w
    return b / b.sum(), np.array([1.0]), d


def thiran(frac, order=3):
    d = order - 1 + frac + 0.5  # Thiran is accurate for d near the order
    a = np.ones(order + 1)
    for k in range(1, order + 1):
        prod = 1.0
        for i in range(order + 1):
            prod *= (d - order + i) / (d - order + k + i)
        a[k] = (-1) ** k * math.comb(order, k) * prod
    return a[::-1].copy(), a, d


def errors(b, a, nominal, fs, f_lo=20.0, f_hi=20000.0):
    f = np.geomspace(f_lo, f_hi, 800)
    w, h = freqz(b, a, worN=f, fs=fs)
    level = 20 * np.log10(np.abs(h))
    phase = np.unwrap(np.angle(h))
    delay = -phase / (2 * math.pi * f / fs)  # phase delay, in samples
    return f, level, delay - nominal


def windowed_sinc_beta(taps):
    return {8: 3.0, 16: 4.0, 32: 5.0, 48: 5.5, 64: 6.0}[taps]


# --- part 4: optimised kernels for the least latency ---------------------------------------------------------------
# Spec (user, 2026-10-06): for every fraction 0.1 to 0.9, from 20 Hz to 20 kHz, level within 0.02 dB and phase delay
# within 0.01 samples of the nominal delay D = H + fraction, D counted from the newest sample the kernel reads.
# Kernel convention here: h[k] weights the sample k samples older than the newest one read (k = 0 .. N - 1); the C++
# `reversed()` array is h[::-1].
SPEC_RATES = [44100, 48000, 88200, 96000, 176400, 192000]
LEVEL_DB, DELAY_TOL = 0.02, 0.01
L_LO, L_HI = 10 ** (-LEVEL_DB / 20), 10 ** (LEVEL_DB / 20)
GAIN_CAP = 1.01  # |H| above 20 kHz: no boost beyond +0.09 dB, about what a pure delay does (0 dB)
POLY = 32  # sides of the polygon standing in for |H| <= GAIN_CAP
N_CAP = 64  # most taps searched (the shipped 44.1 kHz kernel has 48)
CURRENT = {44100: (48, 7.0), 48000: (24, 6.0), 88200: (8, 7.0), 96000: (8, 7.0), 176400: (8, 8.0), 192000: (8, 8.0)}


def spec_errors(h, D, fs):
    """Worst |level| (dB), worst |phase-delay error| and worst |group-delay error| (samples) from 20 Hz to 20 kHz, on
    a dense log + linear grid that includes both edges exactly, and the highest gain above 20 kHz (dB). The taps are
    rounded to float32 first, as the C++ stores them."""
    h = np.asarray(h, dtype=np.float32).astype(np.float64)
    f = np.unique(np.concatenate([np.geomspace(20.0, 20000.0, 4000), np.linspace(20.0, 20000.0, 4000), [20.0, 20000.0]]))
    w = 2 * np.pi * f / fs
    k = np.arange(len(h))
    resid = np.exp(-1j * np.outer(w, k - D)) @ h  # H e^{jwD}: 1 for a perfect delay
    level = 20 * np.log10(np.abs(resid))
    phase = np.angle(resid)  # small, so no unwrapping needed; phase delay = D - phase / w
    gd = np.gradient(np.unwrap(phase), w)
    wo = np.linspace(2 * np.pi * 20000.0 / fs, np.pi, 2000)
    gain = 20 * np.log10(np.abs(np.exp(-1j * np.outer(wo, k)) @ h).max())
    return np.abs(level).max(), np.abs(phase / w).max(), np.abs(gd).max(), gain


def minimax_lp(N, D, fs, extra=()):
    """Minimax kernel by linear programming: minimise t subject to, at each design frequency w in 20 Hz .. 20 kHz,
    with R = Re(H e^{jwD}) and I = Im(H e^{jwD}) (both linear in the taps),
        1 - t (1 - L_LO) <= R <= 1 + t u(w),   |I| <= t P(w),
    where P(w) = L_LO tan(0.01 w) and u(w) = sqrt(L_HI^2 - P^2) - 1. At t <= 1 these are sufficient for the spec
    (|H| >= R >= L_LO, |H| <= sqrt(R^2 + I^2) <= L_HI, |phase| <= atan(I / R) <= 0.01 w). Also sum(h) = 1 (unity at
    DC) and |H| <= GAIN_CAP above 20 kHz (a POLY-gon of half-planes). Returns (taps, t)."""
    f = np.unique(np.concatenate([np.geomspace(20.0, 20000.0, 600), np.linspace(20.0, 20000.0, 600), extra]))
    w = 2 * np.pi * f / fs
    k = np.arange(N)
    arg = np.outer(w, k - D)
    C, S = np.cos(arg), -np.sin(arg)
    P = L_LO * np.tan(DELAY_TOL * w)
    u = np.sqrt(L_HI ** 2 - P ** 2) - 1
    lo = 1 - L_LO
    one = np.ones((len(w), 1))
    # Every row scaled so its bound is t: keeps the LP well conditioned at 20 Hz, where P is about 3e-5.
    A = [np.hstack([C / u[:, None], -one]), np.hstack([-C / lo, -one]),
         np.hstack([S / P[:, None], -one]), np.hstack([-S / P[:, None], -one])]
    b = [1 / u, np.full(len(w), -1 / lo), np.zeros(len(w)), np.zeros(len(w))]
    wo = np.linspace(2 * np.pi * 20000.0 / fs, np.pi, 151)[1:]
    for th in np.arange(POLY) * 2 * np.pi / POLY:
        A.append(np.hstack([np.cos(np.outer(wo, k) + th), np.zeros((len(wo), 1))]))
        b.append(np.full(len(wo), GAIN_CAP * math.cos(math.pi / POLY)))
    c = np.zeros(N + 1)
    c[-1] = 1.0
    res = linprog(c, np.vstack(A), np.concatenate(b), np.hstack([np.ones((1, N)), [[0.0]]]), [1.0],
                  bounds=[(None, None)] * (N + 1), method="highs")
    if res.status != 0:
        return None, math.inf
    return res.x[:N], res.x[-1]


def design_fraction(args):
    """One fraction's kernel: the LP, then the real spec on float32 taps. If the dense check fails where the design
    grid said it passes, the worst frequencies join the design grid and it is solved again."""
    N, H, frac, fs = args
    D = H + frac
    extra = np.zeros(0)
    for _ in range(4):
        h, t = minimax_lp(N, D, fs, extra)
        if h is None or t > 1.0:
            return False, h, t, None
        errs = spec_errors(h, D, fs)
        if errs[0] <= LEVEL_DB and errs[1] <= DELAY_TOL:
            return True, h, t, errs
        f = np.linspace(20.0, 20000.0, 8000)
        w = 2 * np.pi * f / fs
        r = np.exp(-1j * np.outer(w, np.arange(N) - D)) @ np.asarray(h, np.float32).astype(np.float64)
        score = np.maximum(np.abs(20 * np.log10(np.abs(r))) / LEVEL_DB, np.abs(np.angle(r) / w) / DELAY_TOL)
        extra = np.concatenate([extra, f[np.argsort(score)[-40:]]])
    return False, h, t, errs


def kernel_set(pool, N, H, fs):
    """Kernels for fractions 0.1 .. 0.9 at (H, N), or None if any fraction misses the spec."""
    out = list(pool.map(design_fraction, [(N, H, fr / 10, fs) for fr in range(1, 10)]))
    if not all(o[0] for o in out):
        return None
    return {"H": H, "N": N, "taps": [o[1] for o in out], "t": max(o[2] for o in out),
            "level": max(o[3][0] for o in out), "delay": max(o[3][1] for o in out),
            "gd": max(o[3][2] for o in out), "gain": max(o[3][3] for o in out)}


def fewest_taps(pool, H, fs, n_lo, n_hi, step=1, centred=False):
    """The smallest N in [n_lo, n_hi] (multiples of step) whose kernel set passes, by bisection: adding a tap (a zero at
    the oldest end) can't make a design worse, so passing is monotonic in N. For centred designs H follows N."""
    cands = list(range(n_lo, n_hi + 1, step))
    at = lambda i: kernel_set(pool, cands[i], cands[i] // 2 - 1 if centred else H, fs)
    best = at(len(cands) - 1)
    if best is None:
        return None
    lo, hi = -1, len(cands) - 1  # cands[hi] passes; everything at or below lo fails
    while hi - lo > 1:
        mid = (lo + hi) // 2
        r = at(mid)
        if r is not None:
            hi, best = mid, r
        else:
            lo = mid
    return best


def current_kernels(fs):
    N, beta = CURRENT[fs]
    taps = [windowed_sinc(fr / 10, N, beta)[0] for fr in range(1, 10)]
    errs = [spec_errors(t, N // 2 - 1 + fr / 10, fs) for fr, t in zip(range(1, 10), taps)]
    return {"H": N // 2 - 1, "N": N, "taps": taps, "level": max(e[0] for e in errs), "delay": max(e[1] for e in errs),
            "gd": max(e[2] for e in errs), "gain": max(e[3] for e in errs)}


def passes_at(kset, fs):
    errs = [spec_errors(t, kset["H"] + fr / 10, fs) for fr, t in zip(range(1, 10), kset["taps"])]
    return all(e[0] <= LEVEL_DB and e[1] <= DELAY_TOL for e in errs)


def write_kernels(kset, fs, path):
    lines = [f"# Fractional-delay kernels for {fs} Hz (prototype/subsample.py, part 4): low-delay minimax (LP).",
             f"# lookahead H = {kset['H']}, taps N = {kset['N']}. One line per fraction: f (tenths), then N float32 taps,",
             "# oldest sample first (the C++ reversed() order: index j weights the j-th of N consecutive samples).",
             f"# Nominal delay from the newest sample read: H + f/10. Worst level {kset['level']:.4f} dB, worst delay",
             f"# {kset['delay']:.4f} samples (float32 taps, 20 Hz to 20 kHz)."]
    for fr, h in zip(range(1, 10), kset["taps"]):
        lines.append(f"{fr} " + " ".join(f"{v:.9g}" for v in np.asarray(h, np.float32)[::-1]))
    path.write_text("\n".join(lines) + "\n")


def optimised_designs():
    md = ["## 4. Optimised designs (latency)", "",
          "The smallest lookahead H (the latency the kernels add), then the fewest taps N, that meets the spec at every",
          "fraction 0.1 to 0.9: level within 0.02 dB and phase delay within 0.01 samples from 20 Hz to 20 kHz, checked",
          "on float32-rounded taps over 8000 log + linear points including both edges. Every kernel is also held to",
          f"|H| ≤ {GAIN_CAP} (+{20 * math.log10(GAIN_CAP):.2f} dB) above 20 kHz (no ultrasonic boost; a pure delay is",
          "0 dB) and unity at DC.", "",
          "- **current**: the shipped Kaiser-windowed sinc (`FractionalDelay.h`), H = N/2 − 1.",
          "- **centred minimax**: H = N/2 − 1, taps by linear programming (minimise the worst error relative to the",
          "  spec; see `minimax_lp`). Even N only.",
          f"- **low-delay minimax**: the same LP with H free (H < N ≤ {N_CAP}): H is searched from 0 up, then N by",
          "  bisection. Not linear phase.", "",
          "Margins are the worst over the nine fractions; *t* is the LP's worst error as a fraction of the spec;",
          "*gd* is the worst group-delay error (not in the spec, for information); *gain* is the highest gain above",
          "20 kHz. Cost is N multiply-adds per output sample per channel.", "",
          "| rate | design | H (latency) | N | worst level | worst delay | t | gd | gain > 20 kHz |",
          "|---|---|---|---|---|---|---|---|---|"]
    chosen = {}
    row = lambda fs, name, k: (f"| {fs / 1000:g} kHz | {name} | {k['H']} ({1000 * k['H'] / fs:.3f} ms) | {k['N']} | "
                               f"{k['level']:.4f} dB | {k['delay']:.4f} | {k.get('t', float('nan')):.2f} | "
                               f"{k['gd']:.3f} | {k['gain']:+.2f} dB |")
    with ProcessPoolExecutor() as pool:
        for fs in SPEC_RATES:
            cur = current_kernels(fs)
            md.append(row(fs, "current (Kaiser)", cur))
            centred = fewest_taps(pool, None, fs, 4, CURRENT[fs][0], step=2, centred=True)
            md.append(row(fs, "centred minimax", centred) if centred else
                      f"| {fs / 1000:g} kHz | centred minimax | none up to {CURRENT[fs][0]} taps | | | | | | |")
            low = None
            for H in range(0, cur["H"] + 1):
                if kernel_set(pool, N_CAP, H, fs) is not None:
                    low = fewest_taps(pool, H, fs, H + 1, N_CAP)
                    break
            if low is None:  # can't happen while N_CAP >= the current N, but say so rather than crash
                md.append(f"| {fs / 1000:g} kHz | low-delay minimax | none up to {N_CAP} taps | | | | | | |")
                continue
            md.append(row(fs, "**low-delay minimax**", low))
            chosen[fs] = low
            write_kernels(low, fs, OUT / f"kernels_{fs}.txt")
            print(f"part 4: {fs} Hz done (H {low['H']}, N {low['N']})", flush=True)

    md += ["", "Which rates each low-delay set also meets the spec at (a set designed for a rate holds at higher rates",
           "with the same H, since the band to 20 kHz only narrows):", "",
           "| designed for | " + " | ".join(f"{fs / 1000:g} kHz" for fs in SPEC_RATES) + " |",
           "|---|" + "---|" * len(SPEC_RATES)]
    for fs, k in chosen.items():
        md.append(f"| {fs / 1000:g} kHz (H {k['H']}, N {k['N']}) | " +
                  " | ".join("yes" if passes_at(k, g) else "no" for g in SPEC_RATES) + " |")
    md += ["", "Kernels: `kernels_<rate>.txt` (fraction, then N float32 taps, oldest sample first).", ""]
    return md


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    md = ["# Sub-sample delay: worth it, resolution, interpolator", "",
          "Generated by `prototype/subsample.py`. Question from the user, 2026-10-06.", ""]

    md += ["## 1. What the rounding costs", "",
           "Two equal, coherent signals summed with the worst residual offset a grid leaves (half a step). The dip at",
           "each frequency, in dB (0 = perfect alignment):", "",
           "| rate | grid (samples) | worst residual | " + " | ".join(f"{f / 1000:g} kHz" for f in CHECK_HZ) + " |",
           "|---|---|---|" + "---|" * len(CHECK_HZ)]
    for fs in RATES:
        for g in GRIDS:
            tau = g / 2
            cells = " | ".join(f"{residual_dip_db(f, tau, fs):.2f}" for f in CHECK_HZ)
            md.append(f"| {fs / 1000:g} kHz | {g:g} | {tau:g} samples ({1e6 * tau / fs:.1f} µs, "
                      f"{34300 * tau / fs * 10:.2f} mm) | {cells} |")
    md += ["", "Real pairs are less coherent at the top than this (different mic positions, room), so these are upper",
           "bounds on the audible effect.", ""]

    # Part 2.
    candidates = [("linear (2 taps)", lambda fr: linear(fr), 0)]
    candidates.append(("Lagrange 3rd order (4 taps)", lambda fr: lagrange3(fr), 1))
    for taps in [8, 16, 32, 48, 64]:
        candidates.append((f"windowed sinc, {taps} taps", lambda fr, t=taps: windowed_sinc(fr, t, windowed_sinc_beta(t)),
                           taps // 2 - 1))
    candidates.append(("Thiran all-pass, order 3", lambda fr: thiran(fr), 3))

    md += ["## 2. Interpolators", "",
           "Worst level error (dB) and worst delay error (samples) from 20 Hz to 20 kHz, over fractions 0.1, 0.25 and",
           "0.5. Added latency is what has to be reported on top of the 4 ms: the samples the interpolator reads",
           "ahead of its tap.", "",
           "| interpolator | added latency | 44.1 kHz level | 44.1 kHz delay | 48 kHz level | 48 kHz delay | "
           "48 kHz to 18 kHz: level |",
           "|---|---|---|---|---|---|---|"]
    fig, axes = plt.subplots(1, 2, figsize=(13, 4.5))
    for name, make, ahead in candidates:
        cells = []
        for fs in [44100, 48000]:
            lev = dly = 0.0
            for fr in FRACTIONS:
                b, a, nominal = make(fr)
                f, level, derr = errors(b, a, nominal, fs)
                lev = max(lev, np.abs(level).max())
                dly = max(dly, np.abs(derr).max())
                if fs == 44100 and fr == 0.5:
                    axes[0].semilogx(f, level, label=name)
                    axes[1].semilogx(f, derr, label=name)
            cells += [f"{lev:.3f} dB", f"{dly:.3f}"]
        lev18 = 0.0
        for fr in FRACTIONS:
            b, a, nominal = make(fr)
            _, level, _ = errors(b, a, nominal, 48000, f_hi=18000.0)
            lev18 = max(lev18, np.abs(level).max())
        md.append(f"| {name} | {ahead} samples | " + " | ".join(cells) + f" | {lev18:.3f} dB |")
    axes[0].set_ylim(-3, 0.5)
    axes[0].set_ylabel("level (dB), fraction 0.5, 44.1 kHz")
    axes[1].set_ylim(-0.3, 0.3)
    axes[1].set_ylabel("delay error (samples)")
    for ax in axes:
        ax.set_xlim(20, 22050)
        ax.grid(True, which="both", alpha=0.3)
        ax.set_xlabel("frequency (Hz)")
    axes[0].legend(fontsize=7, loc="lower left")
    fig.suptitle("Fractional-delay interpolators at the worst fraction (0.5), 44.1 kHz")
    fig.tight_layout()
    fig.savefig(OUT / "interpolators.png", dpi=110)
    plt.close(fig)
    md += ["", "![interpolators](interpolators.png)", ""]

    # Part 3: the shortest windowed sinc per rate within 0.02 dB and 0.01 samples to 20 kHz (fractions 0.1 to 0.9).
    md += ["## 3. Shortest windowed sinc per rate", "",
           "Within 0.02 dB and 0.01 samples from 20 Hz to 20 kHz at every fraction 0.1 to 0.9, searching taps in steps",
           "of 8 and β from 3 to 8. Its latency is taps/2 − 1 samples.", "",
           "| rate | taps | β | worst level | worst delay | latency |", "|---|---|---|---|---|---|"]
    for fs in [44100, 48000, 88200, 96000, 176400, 192000]:
        found = None
        for taps in range(8, 129, 8):
            for beta in np.arange(3.0, 8.01, 0.5):
                lev = dly = 0.0
                for fr in np.arange(0.1, 0.95, 0.1):
                    b, a, nominal = windowed_sinc(fr, taps, beta)
                    _, level, derr = errors(b, a, nominal, fs)
                    lev, dly = max(lev, np.abs(level).max()), max(dly, np.abs(derr).max())
                if lev <= 0.02 and dly <= 0.01 and (found is None or lev + 10 * dly < found[2] + 10 * found[3]):
                    found = (taps, beta, lev, dly)
            if found is not None:
                break
        taps, beta, lev, dly = found
        md.append(f"| {fs / 1000:g} kHz | {taps} | {beta:g} | {lev:.4f} dB | {dly:.4f} | {taps // 2 - 1} samples "
                  f"({1000 * (taps // 2 - 1) / fs:.2f} ms) |")
    md.append("")

    md += optimised_designs()

    (OUT / "report.md").write_text("\n".join(md) + "\n")
    print((OUT / "report.md").read_text())


if __name__ == "__main__":
    main()
