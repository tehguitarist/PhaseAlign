# Developing Phase Align

How the code is laid out, how to build and test it, and the rules that keep it correct. For what the plugin does and why it
sounds the way it does, read the [README](../README.md) (the "How it works" section covers the signal flow, the phase
modes, the meter and ANALYSE).

## Layout

| Path | What is there |
|---|---|
| `src/dsp` | The processing chain (polarity, phase, delay): plain C++ with no JUCE and no allocation, so it runs the same in the plugin and the tests. `HiLoStage` (HIGH and LOW), `ConstantRotator` (CONSTANT), `DelayStage`, `Chain`. |
| `src/meter` | The correlation meter's audio-side capture and its GUI-side analyser (BANDS, VECTORSCOPE, ALIGNMENT). |
| `src/analyse` | ANALYSE: the capture FIFO, the session that gates and triggers it, and the search. |
| `src/params`, `src/ui`, `src/Plugin*` | Parameters, the editor and its components, the processor. |
| `tests/` | Catch2 tests; `tests/dsp` holds the light runner, `tests/golden` the reference data. |
| `reference/` | Python reference implementations (the specification of the DSP and of ANALYSE) and the generators for the committed test data and tables. |
| `ui/`, `assets/`, `tools/` | The artwork masters, the baked assets that are embedded, and the scripts that produce them. |
| `installer/` | The macOS, Windows and Linux packaging. |
| `libs/` | JUCE and PFFFT, as submodules. |

## Building

```bash
git clone --recurse-submodules https://github.com/tehguitarist/PhaseAlign.git
cd PhaseAlign
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 3
```

Artefacts land in `build/PhaseAlign_artefacts/Release/{AU,VST3}` (AU on macOS only). Options:

- `-DPA_STANDALONE=ON` also builds a standalone app, for looking at the UI without a host.
- `-DPA_BUILD_TESTS=OFF` skips the tests (they roughly double a clean build).
- `-DPA_GENERIC_FFT_TESTS=ON` builds the DSP tests with PFFFT on macOS too, to see what Windows and Linux get.

macOS ships two separate builds, arm64 and x86_64, never a universal binary.

## Testing

```bash
cmake -B build-test -DPA_STANDALONE=ON
cmake --build build-test --target PhaseAlignTests PhaseAlignDspTests
ctest --test-dir build-test
```

- `PhaseAlignDspTests` is the light runner: `src/dsp`, `src/meter` and `src/analyse` with `juce_dsp` only. It counts
  allocations, and the audio path must make none.
- `PhaseAlignTests` is everything else: parameters, state, the processor through its real entry points, the editor, ANALYSE's
  flow, robustness (mono, sample rates, block sizes).
- Tests tagged hidden (`[.]`) don't run under `ctest`. Run them by name or tag with the test binary:
  `PhaseAlignDspTests "chain cost per stereo frame"` (the budget below), `"[bench]"`, `"[.analysercost]"` and `"[.analysecost]"`
  (build with `-DCMAKE_BUILD_TYPE=Release` for these), `PA_SNAPSHOT_DIR=<dir> PhaseAlignTests "[snapshot]"` (renders the editor
  and ANALYSE's screens to PNGs), and `PhaseAlignTests "[desktop]"` (on-screen checks that need a display).
- `pluginval` is run in CI against the VST3 on all three platforms, and `auval` against the AU on macOS.
- CI (`.github/workflows/ci.yml`) builds and tests on macOS, Windows and Linux for every push to master and every pull
  request. Random tests use different generators on different standard libraries, so a failure on one platform only is
  worth looking at the seed first.

CPU budgets per stereo frame (Release, the DSP benchmark): HIGH and LOW under 50 ns, CONSTANT under 150 ns.

## The reference implementations

`reference/` holds the Python that specifies the numerical parts. The C++ must match it:

| Reference | C++ | Test data |
|---|---|---|
| `hilo.py` (HIGH and LOW, run oversampled) | `HiLoStage`, `AllpassCascade`, `Oversampler`, `PhaseMapping` | `golden.py` writes `tests/golden/hilo_*`; within 1e-6 |
| `p2_constant.py` (CONSTANT) | `ConstantRotator`, `HilbertFir`, `PartitionedConvolver` | `golden.py` writes `tests/golden/constant_*`; within 1e-6 |
| `analyse.py` (ANALYSE's search) | `src/analyse/Search` | `analyse_golden.py` writes `tests/golden/analyse_*`; scores within 1e-9 |
| `oversampling.py`, `subsample.py`, `fractional_tables.py` | generate `src/dsp/HalfbandTables.h` and `FractionalKernelTables.h` | don't edit those headers by hand |

If a reference changes, change the C++ identically and regenerate the goldens. All Python goes through the project virtual
environment:

```bash
python3 -m venv .venv && .venv/bin/pip install -r reference/requirements.txt
.venv/bin/python reference/golden.py
.venv/bin/python reference/analyse_golden.py
```

## Rules that matter

- **Parameter IDs are permanent** (`src/params/Parameters.h`). Never rename or renumber one; saved sessions depend on them.
  Add new parameters with new IDs.
- **The HIGH and LOW mapping is fixed**: the four shapes and their reference frequencies (README, "All-pass phase") come from
  an analog reference and are what the readout means. Changing them changes what every saved session sounds like.
- **Nothing allocates, locks or waits on the audio thread.** The meter's and ANALYSE's capture are lock-free single-producer
  single-consumer buffers; analysis runs on the GUI side or its own thread.
- **Latency is reported to the host** and is fixed per configuration (the delay on or off, CONSTANT or not, the sample rate),
  never per knob position. When it changes, the audio fades out and back in.
- `HiLoStage::process` takes at most `HiLoStage::maxBlock` (32) samples per call; the chain splits blocks.
- `juce::AudioProcessor::getSampleRate()` is 0 in tests that call `prepareToPlay` directly; use the processor's own `sampleRate`.
- The editor's on-screen tests play audio slower than real time and use wall-clock timers; ANALYSE's flow tests drive the
  session with `tickForTesting()` and `waitForAnalysisForTesting()` instead of its timer.
- When filtering build output, search for `error` (a compile error looks like `file.cpp:12:3: error:`), or a failed compile
  leaves the tests running a stale binary.

## ANALYSE's constants

The search (`src/analyse/Search.h`) is tuned by a handful of constants whose comments give what each means. They were set from
measurements on real multi-mic recordings and checked against blind listening tests:

- A candidate is scored by the mean 1/3-octave-band correlation *r* (40 Hz to 16 kHz) of the sum with the sidechain. Other
  measures (broadband *r*, gain in dB, low-end weighted, time-domain peak and RMS) ordered candidates no better.
- An option must gain at least `MIN_GAIN` (0.03 in *r*) over doing nothing; two options are shown only when the second is
  within `MIN_MARGIN` (0.01) of the first.
- Warnings, never rejections: "the audio seems unrelated" when the best option doesn't beat a chance test (the same search
  with the sidechain circularly shifted) by `CHANCE_MARGIN` (0.02), skipped when *r* is at least `CHANCE_SKIP_R` (0.30); "a weak
  match" when *r* stays under `WEAK_MATCH` (0.12).
- A manual shift (in samples, beyond the knob's ±4 ms) is advised only when the best score beyond the reach beats the best inside
  it by `SHIFT_GAIN` (0.04; 0.02 when the best delay sits at the knob's edge) and then is the smallest shift that gets
  `SHIFT_FRACTION` (0.75) of the way.
- When listening, how well the algorithm helps depends on the source: polarity advice agrees with listeners about 90% of the time
  when the pick's correlation is above 0.25, and is a coin flip below 0.05. Percussive pairs (kick against its room and overhead mics
  especially) are the weakest case: leaving them alone is often right. ANALYSE suggests; the ear decides.

## The artwork

`ui/` holds the masters and `ui/ui-info.csv` the layout (x and y are the **centre** of each image, in a 1954x1224 design space).
After changing either, run `tools/build_assets.sh` (needs ImageMagick, pngquant and the venv): it regenerates `src/ui/Layout.h`
and the embedded images in `assets/images/`. Adding a control means rows in the CSV and an entry in `toggleSpecs` in
`src/PluginEditor.cpp`.

## Releasing

The **Release** workflow (`.github/workflows/release.yml`, run manually from the Actions tab or
`gh workflow run release.yml`) refuses a commit without a green CI run. It builds macOS (arm64 and Intel), Windows and Linux,
signs and notarizes the macOS builds, and publishes a draft GitHub release. The version comes from
`project(PhaseAlign VERSION ...)` in `CMakeLists.txt`. The installers stage the README's quick start and the licences
(`installer/stage_docs.sh`).
