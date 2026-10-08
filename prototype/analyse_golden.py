"""Golden references for ANALYSE's C++ search (src/analyse/Search.cpp): prototype/analyse.py's suggest_with_shift on fixed
pairs, which tests/dsp/AnalyseTests.cpp must reproduce.

    .venv/bin/python prototype/analyse_golden.py      # → tests/golden/analyse_*.i16 and analyse_expected.txt (committed)
                                                      #   and, if the user's pairs are there, captures/analyse_user_*.txt

Synthetic pairs, 3 s at 48 kHz, each signal int16 little-endian (value / 32768; the Python works on exactly those values):
  A: impulsive coloured noise, the input built as the sidechain 1.25 ms early and turned 120°: the attack-first path.
  B: the same sidechain, the input 6.5 ms late and turned 60°: beyond the knob, so a manual shift and then the options.
  C: steady coloured noise, the input 2 ms late and turned 70°: weak attacks, so the joint (waveform score) search.
The cases run A with both stages on, with DELAY off and with PHASE off, B and C with both on, and A's input against C's
sidechain (unrelated: nothing worth changing). Each case also pins a few closed-form scores.

The user's pairs (gitignored): the first set's captures/<tag>_a.f32 / _b.f32 (prototype/export_pairs.py) and the second
set's captures/stems/<tag>_a.f32 / _b.f32 (written here from the wavs). captures/analyse_user_expected.txt is what the
hidden test `PhaseAlignDspTests "[.useranalyse]"` compares with.
"""
import math
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import analyse as an  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
GOLDEN = ROOT / "tests" / "golden"
FS = 48000
SECONDS = 3.0


def coloured(y, fs):
    spec = np.fft.rfft(y)
    f = np.fft.rfftfreq(len(y), 1 / fs)
    spec *= 1.0 / np.sqrt(1 + (f / 800.0) ** 2) + 0.05
    return np.fft.irfft(spec, len(y))


def turned(y, fs, delay_ms, angle):
    """y delayed by delay_ms and turned by +angle (what the plugin has to undo)."""
    spec = np.fft.rfft(y)
    f = np.fft.rfftfreq(len(y), 1 / fs)
    spec *= np.exp(-2j * np.pi * f * delay_ms * 1e-3) * np.exp(1j * math.radians(angle))
    return np.fft.irfft(spec, len(y))


def impulsive(n, fs, seed):
    rng = np.random.default_rng(seed)
    env = np.zeros(n)
    for hit in np.arange(0.1, n / fs - 0.3, 0.37):
        i = int(hit * fs)
        env[i:i + int(0.25 * fs)] += np.exp(-np.arange(int(0.25 * fs)) / (0.06 * fs))
    return coloured(env * rng.standard_normal(n), fs)


def quantise(v):
    q = np.round(v / np.abs(v).max() * 0.9 * 32767).astype("<i2")
    return q, q.astype(np.float32) / np.float32(32768.0)


def label_verdict(v):
    for key, name in (("suggest", "suggest"), ("close", "close"), ("delay only", "delayOnly"), ("no change", "nothing")):
        if v.startswith(key):
            return name
    raise ValueError(v)


def mode_name(c):
    return c.mode


def describe_case(name, xname, yname, x, y, delay_on, phase_on, fs, scores=()):
    opts, base, lag, verdict, shift, message = an.suggest_with_shift(x, y, fs, delay_on, phase_on)
    lines = [f"case {name} {xname} {yname} {int(delay_on)} {int(phase_on)} {fs}",
             f"baseline {base:.12g}",
             f"attack {lag[0]:.12g} {int(lag[1])} {lag[2]:.12g} {lag[3]:.12g}",
             f"verdict {label_verdict(verdict)}",
             f"lessLowEnd {int('less low end' in verdict)}",
             f"flags {int('chance level' in verdict)} {int('weak match' in verdict)}",
             f"shift {shift}",
             f"message {message}"]
    for oname, c, gain, lf in opts:
        lf = 0.0 if np.isnan(lf) else lf
        lines.append(f"option {c.mode} {int(c.wide)} {c.theta:.6f} {c.delay_ms:.12g} {int(c.flip)} {c.score:.12g} "
                     f"{gain:.12g} {lf:.12g} {int(oname == 'delay from the attacks' or oname == 'delay only')} "
                     f"{int(oname == 'delay only')}")
    if opts and not verdict.startswith("delay only"):
        shifted = an.shift_samples(x, shift) if shift else x
        lines.append(f"chanceGain {an.chance_gain(shifted, y, fs, delay_on, phase_on):.12g}")
    if scores:
        sp = an.spectra(x, y, fs)
        for mode, wide, theta, delay, flip in scores:
            c = an.Candidate(mode, wide, theta, delay, flip, 0.0)
            v = an.score_at(sp, an.candidate_response(c, fs, sp.freqs))
            lines.append(f"score {mode} {int(wide)} {theta:.6f} {delay:.12g} {int(flip)} {v:.12g}")
    lines.append("end")
    print(f"{name}: {verdict}; shift {shift}; " + "; ".join(c.label() + f" {c.score:+.4f}" for _, c, _, _ in opts))
    return lines


SCORES = [("none", False, 0.0, 0.0, False), ("constant", True, 45.0, 1.0, True), ("lo", True, 100.0, -0.5, False),
          ("hi", False, 30.0, 2.0, False), ("hi", True, 160.0, -1.3, True), ("lo", False, 62.5, 0.7, False)]


def synthetic():
    n = int(SECONDS * FS)
    ay = impulsive(n, FS, 11)
    ax = turned(ay, FS, -1.25, 120.0)
    bx = turned(ay, FS, 6.5, 60.0)
    cy = coloured(np.random.default_rng(12).standard_normal(n), FS)
    cx = turned(cy, FS, 2.0, 70.0)
    signals = {}
    GOLDEN.mkdir(parents=True, exist_ok=True)
    for name, v in (("a_x", ax), ("a_y", ay), ("b_x", bx), ("c_x", cx), ("c_y", cy)):
        q, f = quantise(v)
        q.tofile(GOLDEN / f"analyse_{name}.i16")
        signals[name] = f.astype(np.float64)
    s = signals
    lines = []
    lines += describe_case("a_both", "a_x", "a_y", s["a_x"], s["a_y"], True, True, FS, SCORES)
    lines += describe_case("a_delay_off", "a_x", "a_y", s["a_x"], s["a_y"], False, True, FS)
    lines += describe_case("a_phase_off", "a_x", "a_y", s["a_x"], s["a_y"], True, False, FS)
    lines += describe_case("b_shift", "b_x", "a_y", s["b_x"], s["a_y"], True, True, FS, SCORES)
    lines += describe_case("c_joint", "c_x", "c_y", s["c_x"], s["c_y"], True, True, FS, SCORES)
    lines += describe_case("unrelated", "a_x", "c_y", s["a_x"], s["c_y"], True, True, FS)
    (GOLDEN / "analyse_expected.txt").write_text("\n".join(lines) + "\n")


def user_pairs():
    import soundfile as sf
    import analyse_stems as st
    pairs = []
    for tag in an.PAIRS:
        a, b = ROOT / "captures" / f"{tag}_a.f32", ROOT / "captures" / f"{tag}_b.f32"
        if a.exists() and b.exists():
            pairs.append((tag, f"captures/{tag}"))
    stems = ROOT / "captures" / "stems"
    for tag, track, ref in st.PAIRS:
        if not (stems / f"{track}.wav").exists():
            continue
        x, y = st.load(track), st.load(ref)
        n = min(len(x), len(y))
        x[:n].astype("<f4").tofile(stems / f"{tag}_a.f32")
        y[:n].astype("<f4").tofile(stems / f"{tag}_b.f32")
        pairs.append((tag, f"captures/stems/{tag}"))
    if not pairs:
        print("no user pairs in captures/ (skipped)")
        return
    lines = []
    for tag, stem in pairs:
        x = np.fromfile(ROOT / f"{stem}_a.f32", np.float32).astype(np.float64)
        y = np.fromfile(ROOT / f"{stem}_b.f32", np.float32).astype(np.float64)
        for delay_on, phase_on in ((True, True), (False, True), (True, False)):
            lines += describe_case(f"{tag}_{int(delay_on)}{int(phase_on)}", f"{stem}_a", f"{stem}_b", x, y, delay_on,
                                   phase_on, FS)
    (ROOT / "captures" / "analyse_user_expected.txt").write_text("\n".join(lines) + "\n")


def main():
    synthetic()
    if "--synthetic" not in sys.argv:
        user_pairs()


if __name__ == "__main__":
    main()
