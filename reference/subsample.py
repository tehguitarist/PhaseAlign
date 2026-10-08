"""Low-delay fractional-delay kernels for the delay stage, and the generator of their design data.

Specifies src/dsp/FractionalDelay.h (its kernels are tabulated by fractional_tables.py).

    .venv/bin/python reference/subsample.py      # writes reference/kernels/kernels_<rate>.txt, prints a summary
    .venv/bin/python reference/fractional_tables.py   # then writes src/dsp/FractionalKernelTables.h

A delay in steps of 0.1 sample needs an interpolator for the fractions 0.1 to 0.9. The design is a minimax kernel per
fraction by linear programming, for the least latency: the smallest lookahead H (the samples the kernel reads ahead
of its tap), then the fewest taps N, that meets the spec at every fraction. The search is bounded by what a centred
Kaiser-windowed sinc needs (H = N/2 - 1, see H_MAX). The kernels are not linear phase.
"""

import math
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

import numpy as np
from scipy.optimize import linprog

OUT = Path(__file__).resolve().parent / "kernels"

# --- the spec ----------------------------------------------------------------------------------------------------
# Spec: for every fraction 0.1 to 0.9, from 20 Hz to 20 kHz, level within 0.02 dB and phase delay
# within 0.01 samples of the nominal delay D = H + fraction, D counted from the newest sample the kernel reads.
# Kernel convention here: h[k] weights the sample k samples older than the newest one read (k = 0 .. N - 1); the C++
# `reversed()` array is h[::-1].
SPEC_RATES = [44100, 48000, 88200, 96000, 176400, 192000]
LEVEL_DB, DELAY_TOL = 0.02, 0.01
L_LO, L_HI = 10 ** (-LEVEL_DB / 20), 10 ** (LEVEL_DB / 20)
GAIN_CAP = 1.01  # |H| above 20 kHz: no boost beyond +0.09 dB, about what a pure delay does (0 dB)
POLY = 32  # sides of the polygon standing in for |H| <= GAIN_CAP
N_CAP = 64  # most taps searched
# Largest lookahead searched per rate: that of the centred windowed sinc meeting the same spec (H = N/2 - 1).
H_MAX = {44100: 23, 48000: 11, 88200: 3, 96000: 3, 176400: 3, 192000: 3}


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


def passes_at(kset, fs):
    errs = [spec_errors(t, kset["H"] + fr / 10, fs) for fr, t in zip(range(1, 10), kset["taps"])]
    return all(e[0] <= LEVEL_DB and e[1] <= DELAY_TOL for e in errs)


def write_kernels(kset, fs, path):
    lines = [f"# Fractional-delay kernels for {fs} Hz (reference/subsample.py): low-delay minimax (LP).",
             f"# lookahead H = {kset['H']}, taps N = {kset['N']}. One line per fraction: f (tenths), then N float32 taps,",
             "# oldest sample first (the C++ reversed() order: index j weights the j-th of N consecutive samples).",
             f"# Nominal delay from the newest sample read: H + f/10. Worst level {kset['level']:.4f} dB, worst delay",
             f"# {kset['delay']:.4f} samples (float32 taps, 20 Hz to 20 kHz)."]
    for fr, h in zip(range(1, 10), kset["taps"]):
        lines.append(f"{fr} " + " ".join(f"{v:.9g}" for v in np.asarray(h, np.float32)[::-1]))
    path.write_text("\n".join(lines) + "\n")



def main():
    OUT.mkdir(parents=True, exist_ok=True)
    chosen = {}
    with ProcessPoolExecutor() as pool:
        for fs in SPEC_RATES:
            low = None
            for H in range(0, H_MAX[fs] + 1):
                if kernel_set(pool, N_CAP, H, fs) is not None:
                    low = fewest_taps(pool, H, fs, H + 1, N_CAP)
                    break
            if low is None:
                print(f"{fs} Hz: no kernel set within {N_CAP} taps and lookahead {H_MAX[fs]}")
                continue
            chosen[fs] = low
            write_kernels(low, fs, OUT / f"kernels_{fs}.txt")
            print(f"{fs} Hz: lookahead H = {low['H']} ({1000 * low['H'] / fs:.3f} ms), N = {low['N']} taps, worst level "
                  f"{low['level']:.4f} dB, worst delay {low['delay']:.4f} samples, gain above 20 kHz {low['gain']:+.2f} dB",
                  flush=True)
    # A set designed for a rate holds at higher rates with the same H, since the band to 20 kHz only narrows.
    print("\nRates each set also meets the spec at:")
    print("designed for  " + " ".join(f"{fs:>7}" for fs in SPEC_RATES))
    for fs, k in chosen.items():
        print(f"{fs:<13} " + " ".join(f"{'yes' if passes_at(k, g) else 'no':>7}" for g in SPEC_RATES))


if __name__ == "__main__":
    main()
