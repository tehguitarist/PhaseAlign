"""M4's Instruments check (IMPLEMENTATION_PLAN 5, M4): nothing of the meter runs while it is off, the window is hidden
or the editor is closed, confirmed with the Time Profiler rather than counters.

    .venv/bin/python tools/meter_profile.py [build-ui/PhaseAlignTests_artefacts/Debug/PhaseAlignTests]

For each condition it records PhaseAlignTests "[profile]" (tests/EditorTests.cpp: about 10 s of audio with a live
sidechain, in real time) with `xctrace`, exports the samples and counts those whose stack holds a meter function, over
the steady part of the run (from 1 s after the first processBlock sample). "metering" is the control: the meter must
show up there. Traces go to a temporary folder.
"""

import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

PHASES = ["metering", "meterOff", "hidden", "closed"]
KEYS = ["CorrelationAnalyser", "MeterScreen", "MeterCapture", "processBlock"]


def samples(xml_path):
    """(time ns, [frame names]) per sample, resolving the export's id/ref deduplication."""
    names, stacks, times, rows = {}, {}, {}, []
    for row in ET.parse(xml_path).getroot().iter("row"):
        t = row.find("sample-time")
        if t.get("id"):
            times[t.get("id")] = int(t.text)
        time = times[t.get("ref")] if t.get("ref") else int(t.text)
        bt = row.find("tagged-backtrace")
        if bt is None:
            bt = row.find("backtrace")
        if bt is None:
            continue
        if bt.get("ref"):
            frames = stacks[bt.get("ref")]
        else:
            frames = []
            for f in bt.iter("frame"):
                if f.get("id"):
                    names[f.get("id")] = f.get("name")
                frames.append(names[f.get("ref")] if f.get("ref") else f.get("name"))
            stacks[bt.get("id")] = frames
        rows.append((time, frames))
    return rows


def main():
    binary = Path(sys.argv[1] if len(sys.argv) > 1 else "build-ui/PhaseAlignTests_artefacts/Debug/PhaseAlignTests")
    out = Path(tempfile.mkdtemp(prefix="meter_profile_"))
    for phase in PHASES:
        trace, xml = out / f"{phase}.trace", out / f"{phase}.xml"
        subprocess.run(["xcrun", "xctrace", "record", "--template", "Time Profiler", "--output", str(trace),
                        "--env", f"PA_PROFILE_PHASE={phase}", "--launch", "--", str(binary.resolve()), "[profile]"],
                       check=True, capture_output=True)
        with open(xml, "w") as f:
            subprocess.run(["xcrun", "xctrace", "export", "--input", str(trace), "--xpath",
                            '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]'], check=True, stdout=f)
        rows = samples(xml)
        blocks = [t for t, fr in rows if any("processBlock" in x for x in fr)]
        start, end = min(blocks) + 1e9, max(blocks)
        steady = [fr for t, fr in rows if start <= t <= end]
        counts = {k: sum(any(k in x for x in fr) for fr in steady) for k in KEYS}
        print(f"{phase}: {len(steady)} samples over {(end - start) / 1e9:.1f} s; "
              + ", ".join(f"{k} {v}" for k, v in counts.items()))
    print(f"traces: {out}")


if __name__ == "__main__":
    main()
