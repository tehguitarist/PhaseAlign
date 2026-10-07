"""Raw float32 copies of the stem pairs in captures/ (gitignored), for the hidden analysis tests.

Reads captures/<name>.wav and captures/<name>_1.wav (Kick, Snare, bass, guitar, hats), mixes to mono, trims each pair
to a common length and writes captures/<tag>_a.f32 and captures/<tag>_b.f32 (48 kHz mono float32), which
tests/dsp/MeterTests.cpp reads:

    PhaseAlignDspTests "[.userpairs]"     the attack and waveform lags (the user's own kick, snare, bass, guitar, hats)
    PhaseAlignDspTests "[.usercapture]"   hit detection and capture rendering on them
    PhaseAlignDspTests "[.bandgate]"      how often a band is gated or near zero, SLOW and FAST

The hats pair is exported realigned: the two takes' hits are 185.14 ms apart (half the 370 ms spacing, which is why a
plain cross-correlation shows equal peaks at +-185 ms), so the second is delayed by that much; the onsets then coincide.

Run through the venv:  source .venv/bin/activate && python prototype/export_pairs.py
"""
import numpy as np
import soundfile as sf

PAIRS = [("kick", "Kick", "Kick_1"), ("snare", "Snare", "Snare_1"), ("bass", "bass", "bass_1"),
         ("guitar", "guitar", "guitar_1"), ("hats", "hats", "hats_1")]
HATS_B_DELAY_MS = 185.14


def mono(path):
    x, fs = sf.read(path)
    assert fs == 48000, f"{path}: {fs} Hz (the tests assume 48 kHz)"
    return (x.mean(1) if x.ndim > 1 else x).astype(np.float32)


for tag, a_name, b_name in PAIRS:
    a, b = mono(f"captures/{a_name}.wav"), mono(f"captures/{b_name}.wav")
    n = min(len(a), len(b))
    a, b = a[:n], b[:n]
    if tag == "hats":
        shift = int(round(HATS_B_DELAY_MS * 48000 / 1000))
        b = np.concatenate([np.zeros(shift, np.float32), b])[:n]
    a.tofile(f"captures/{tag}_a.f32")
    b.tofile(f"captures/{tag}_b.f32")
    print(f"{tag}: {n} samples ({n / 48000:.1f} s)")
