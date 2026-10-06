"""Golden references for the C++ port (IMPLEMENTATION_PLAN M2): the Python prototypes render fixed scripts, and
tests/dsp/GoldenTests.cpp must match them sample by sample.

    .venv/bin/python prototype/golden.py      # → tests/golden/*.f32 (committed)

Hi/Lo renders through HiLoOversampled (the sections oversampled between halfbands, since 2026-10-06). Each case is two
files of little-endian float32, channel-major (all of channel 0, then channel 1): <case>_in.f32 (seeded
noise) and <case>_out.f32 (the prototype's output). The scripts change settings only on the 32-sample coefficient grid,
which is where both implementations act on them. Keep the case list in step with GoldenTests.cpp.
"""

from pathlib import Path

import numpy as np

from hilo import HiLoOversampled
from p2_constant import ConstantRotator

OUT = Path(__file__).resolve().parent.parent / "tests" / "golden"

# Hi/Lo: (sample, theta or None, mode or None). Covers a sweep through 90°, a Hi→Lo glide reversed half way, and a
# glide while the angle moves.
HILO_SCRIPT = [(0, 0.0, "hi"), (1024, 150.0, None), (4096, None, "lo"), (4096 + 320, None, "hi"),
               (6144, 30.0, None), (6400, None, "lo"), (7168, 180.0, None)]
# Constant: (sample, theta). Through 90 and 180 (exact there), and back to 0.
CONSTANT_SCRIPT = [(0, 0.0), (2048, 90.0), (6144, 180.0), (9216, 37.0), (12288, 0.0)]


def noise(n, seed):
    return np.random.default_rng(seed).uniform(-0.5, 0.5, (2, n)).astype(np.float32)


def save(name, x, y):
    OUT.mkdir(parents=True, exist_ok=True)
    np.asarray(x, dtype="<f4").tofile(OUT / f"{name}_in.f32")
    np.asarray(y, dtype="<f4").tofile(OUT / f"{name}_out.f32")


def hilo_case(fs, n=8192):
    x = noise(n, 1)
    p = HiLoOversampled(fs, "hi", 0.0)
    y = np.zeros((2, n))
    events = HILO_SCRIPT[1:] + [(n, None, None)]
    pos = 0
    for at, theta, mode in events:
        y[:, pos:at] = p.process(x[:, pos:at].astype(np.float64))
        p.set(theta=theta, mode=mode)
        pos = at
    save(f"hilo_{fs}", x, y)


def constant_case(fs, n=16384):
    x = noise(n, 2)
    y = ConstantRotator(fs, 0.0).render(x.astype(np.float64), CONSTANT_SCRIPT[1:])
    save(f"constant_{fs}", x, y)


def main():
    for fs in (44100, 48000, 96000, 192000):  # 4x (both outer halfbands), 2x, none
        hilo_case(fs)
    for fs in (48000, 96000):
        constant_case(fs)
    for f in sorted(OUT.glob("*.f32")):
        print(f.name, f.stat().st_size)


if __name__ == "__main__":
    main()
