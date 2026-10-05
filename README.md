# Phase Align

A low-CPU, zero-latency phase alignment utility plugin (AU and VST3) built with JUCE: variable all-pass phase,
sample-accurate delay and a correlation meter against a sidechain input. See [PLAN.md](PLAN.md) for the design.

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
