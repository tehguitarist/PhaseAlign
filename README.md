# Phase Align

![Build](https://github.com/tehguitarist/PhaseAlign/actions/workflows/ci.yml/badge.svg?branch=master)
[![License](https://img.shields.io/badge/License-AGPLv3-blue.svg)](https://opensource.org/license/agpl-v3)
[![Downloads](https://img.shields.io/github/downloads/tehguitarist/PhaseAlign/total)](https://somsubhra.github.io/github-release-stats/?username=tehguitarist&repository=PhaseAlign&page=1&per_page=30)

A low-CPU phase alignment utility plugin (AU and VST3) built with JUCE: variable all-pass or constant phase rotation,
a −4 to +4 ms delay in 0.1-sample steps, polarity, and a correlation meter against a sidechain input. Inspired by phase
alignment tools like the Little Labs IBP. See [PLAN.md](PLAN.md) for the design and
[IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md) for how it is built.

**[⬇ Download the latest release](https://github.com/tehguitarist/PhaseAlign/releases/latest)**

## Quick start

*Draft (2026-10-06); host routing steps still to be checked in each host.*

Phase Align lines one track up with another (two mics on one source, a DI and an amp, a close and a room mic). Put
it on one of the pair and feed the other into its **sidechain**; the meter then shows how well they agree.

1. **Insert Phase Align on one track** of the pair and route the **other track to its sidechain**:
   - Logic: the plugin window's *Side Chain* menu → the other track.
   - Live: in the device, set the sidechain input (*Audio From*) to the other track.
   - Reaper: give the track 4 channels, send the other track to channels 3/4, and map them to the sidechain inputs in
     the plugin's pin connector.
   - Cubase/Nuendo: enable the plugin's side-chain button and add a side-chain send from the other track.
   - Studio One and Bitwig: pick the other track in the plugin's sidechain selector.
2. **Time first.** Switch to the meter's **TIME OFFSET** view. The INPUT reading is how far apart the two tracks are
   (positive: the sidechain is later). Turn **DELAY** on and set the knob to that reading; the OUTPUT peak moves to
   0 ms. The delay reaches −4 to +4 ms, so it can move this track either way. Beyond that the screen says **TRANSIENTS
   OUT OF DELAY RANGE**: move a clip in the DAW first.
3. **Then phase.** Switch to the **FREQUENCY** view and turn **PHASE** on. The bright curve (processed) should sit at
   +1 across the band. **HIGH** and **LOW** rotate like an all-pass (no latency), centred higher or an octave lower;
   **CONSTANT** turns every frequency by the same angle. **RANGE** sets the knob's travel to 90° or 180°.
4. **Polarity (Ø)** inverts the track if the curve sits near −1 everywhere.
5. Check by ear: solo the pair and listen for the low end filling in.

**Latency.** The delay adds about 4.5 ms of latency while it is on, so it can go negative; Constant adds about 43 ms.
Both are reported to the host, which compensates. Switching either fades the audio out and back in over about 20 ms;
some hosts only re-align at the next transport start.

## Build

```bash
git clone --recurse-submodules https://github.com/tehguitarist/PhaseAlign.git
cd PhaseAlign
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 3
```

Artefacts land in `build/PhaseAlign_artefacts/Release/{AU,VST3}`.

## Release

Run the **Release** workflow manually (Actions tab or `gh workflow run release.yml`). It builds macOS (arm64 and
Intel), Windows and Linux, signs and notarizes the macOS builds, and publishes a draft GitHub Release. The version
comes from `project(PhaseAlign VERSION ...)` in `CMakeLists.txt`.
