# Phase Align: notes for Claude sessions

## Python environment (read first)

All Python work (DSP prototypes, measurement scripts, asset processing) uses the project venv at `.venv/`.
Shell state does not persist between commands, so **every** Python command must go through the venv:

```bash
source .venv/bin/activate && python prototype/some_script.py
```

or call `.venv/bin/python` directly. Never use the system `python3` or `pip` for project work.

- Dependencies live in `prototype/requirements.txt`. Add new packages there, then run
  `.venv/bin/pip install -r prototype/requirements.txt`.
- If `.venv/` is missing (fresh clone), recreate it: `python3 -m venv .venv && .venv/bin/pip install -r prototype/requirements.txt`.
- Generated plots and renders go in `prototype/out/` (gitignored).

## Tools available on this machine

`magick` (ImageMagick) and `pngquant` are installed, and pluginval (Homebrew cask:
`/Applications/pluginval.app/Contents/MacOS/pluginval`).

**Naming (user, 2026-10-06):** the only mention of another product anywhere in the repo (docs, code, comments, commit
messages) is the README's "Inspired by phase alignment tools like the Little Labs IBP". Not a clone: it does the same
job, better.

## Current status (update as work progresses)

1. **Hi/Lo design (P1 + P5, 2026-10-05):** `prototype/hilo.py` (Hi/Lo prototype, the reference for the C++). Mapping
   in IMPLEMENTATION_PLAN 2.3. Open, decided after listening to the stems (don't implement yet): what the RANGE button
   does in the DSP (`phaseRange`, default 90; `phase` is the knob position, angle = phase x range), extending Hi/Lo
   below their lowest corners. The knob's whole travel is usable (no dead zones; checked 2026-10-06).
2. **Prototypes** (`prototype/`, all through the venv): `hilo.py` (P1), `p2_constant.py` (P2), `p4_meter.py` (P4),
   `subsample.py` (fractional delay), `hf_check.py` (high frequencies, knob travel), `p3_report.py` (P3), `golden.py`
   (the C++ goldens), `fractional_tables.py` (the low-delay kernel tables).
3. **UI build: M0 + M3 DONE (2026-10-05, branch `ui-build`).** Details and choices to confirm in IMPLEMENTATION_PLAN
   M0/M3. Workflow:
   - Art or `ui/ui-info.csv` changed: `tools/build_assets.sh` (regenerates `src/ui/Layout.h` and `assets/images/`),
     then re-run CMake. Adding a control = CSV rows + a row in the editor's table (`toggleSpecs` in PluginEditor.cpp).
   - Build + test: `cmake -B build-ui -DPA_STANDALONE=ON && cmake --build build-ui --target PhaseAlignTests PhaseAlignDspTests`
     then `ctest --test-dir build-ui`. `PhaseAlignDspTests` is the light runner (src/dsp + src/meter, juce_dsp only;
     counts allocations). Visual check: `PA_SNAPSHOT_DIR=<dir> build-ui/PhaseAlignTests_artefacts/Debug/PhaseAlignTests "[snapshot]"`;
     on screen (idle repaints, and the meter end to end with `meter_*.png` snapshots): `... "[desktop]"`.
     Benchmark: `PhaseAlignDspTests "[bench]"` (build Release).
   - pluginval runs in CI only (macOS job); it has never run (2026-10-06: the commits adding it and ctest were never
     pushed; only 4 early CI runs exist). CI runs on master pushes, PRs and manual dispatch.
4. **M2 first part + P4 + M4 DONE (2026-10-06, committed on `ui-build`, not pushed).** Delay, polarity, host
   bypass and the meter really work; the phase stage is an identity placeholder. One crossfade length for
   delay/polarity/stage on-off: 50 ms (`dsp::crossfadeMs`). The meter is a frequency curve + PHAT
   time-offset view (IMPLEMENTATION_PLAN 3, `prototype/out/p4/report.md`). The editor opens at 80% (782x490); assets
   are 1:1 with the 1954x1224 design space. Pending from the user: verdict on the meter views in a DAW; whether to
   lower the minimum editor size (733 px now) to 586.
5. **NEXT (user, 2026-10-06), in this order unless the user says otherwise:**
   1. **Delay -4 to +4 ms: DONE (2026-10-06, merged to master).** As built in IMPLEMENTATION_PLAN 2.1a: while on,
      the latency is the reach in whole samples plus the interpolation lookahead; `Chain` fades out, switches and fades
      in (10 ms each) on delayOn changes; bypass keeps the latency; meter streams are latency-aligned. Steps of
      **0.1 sample** (user, 2026-10-06; R12, replaces whole samples): windowed-sinc kernels, 48/24/8 taps by rate,
      23/11/3 samples of lookahead. The user's answers are recorded in the plan (delayOn not automatable; "TRANSIENTS
      OUT OF DELAY RANGE" replaces the other-track hint; polarity flip applies and shows regardless of phase on/off).
      **Section dimming (plan 4.5): DONE (2026-10-06, merged to master):** `DesignComponent::setDimmed`,
      `PhaseAlignEditor::updateDimming`, `design::dimmedAlpha`, tests/DimmingTests.cpp; pending the user's verdict on
      the alpha.
   2. **P2 Constant mode: DONE (2026-10-06).** `prototype/p2_constant.py`; the user chose **4097 taps** (at 48 kHz,
      scaled with the rate). Sub-sample study: `prototype/subsample.py`.
   3. **C++ port: DONE (2026-10-06, merged to master).** Hi/Lo (`AllpassCascade`, current P5 mapping) and Constant
      (`HilbertFir`, zero-latency `PartitionedConvolver`, `ConstantRotator`; L = D − 1, about 43 ms), golden-tested
      against the Python references (`prototype/golden.py` → `tests/golden/`, within 1e-6). As built and measured:
      IMPLEMENTATION_PLAN 2.3, 2.4, 2.6. Left: **the user's listening in a DAW** (M2 done-when). Don't change the
      Hi/Lo mapping without asking; regenerate the goldens (`golden.py`) whenever a reference changes.
   4. **Efficiency**: benchmarks done (plan 2.6, all within budget on macOS) and vDSP confirmed. Left: Windows/Linux
      (no vDSP) benchmarks, PFFFT only if those are over budget.
   5. **DSP and hardening pass (2026-10-06, branch `dsp-hardening`, pushed; worktree `.claude/worktrees/dsp-hardening`).**
      Before/after table in plan 2.6: Hi/Lo idle 10.9 → 4.0 ns, Constant 48 kHz 61.9 → 30 ns, 192 kHz 97 → 39 ns.
      With the generic FFT that Windows/Linux use, measured here with `-DPA_GENERIC_FFT_TESTS=ON`: 97 → 61 ns and
      148 → 103 ns. `src/dsp/RealFft.h` is the FFT interface (PFFFT would go there). Also built:
      - **Burst fix (user-approved):** the Hi/Lo sections run in direct form I, and the cascade stays warm in
        Constant. The old TPT state burst when the knob left 0°/90°, peaking at 4 on ±0.5 noise. Goldens regenerated;
        `hilo.py` changed identically.
      - Low-delay fractional kernels exist but are **off: the user kept Kaiser**.
      - Robustness tests (`tests/RobustnessTests.cpp`); the plugin runner counts allocations per thread.
      - P3 renders and report: `PhaseAlignDspTests "[p3]"`, then `prototype/p3_report.py` → `prototype/out/p3/`.
      - Meter: a coarse ±40 ms search, so far peaks read TRANSIENTS OUT OF DELAY RANGE.
      - DELAY tooltip latency; the `-192.0 samp` readout fix; the quick-start draft in the README.
      - High frequencies and knob travel: `prototype/hf_check.py`.
      - CI: the DSP benchmark on every OS, pluginval logs, `workflow_dispatch`, `_USE_MATH_DEFINES` for MSVC.
      - pluginval at strictness 10 passes locally on the VST3.

      Next: **`HANDOVER.md`** (de-cramping Hi/Lo's top octave first; the 44.1 kHz delay's roll-off above 20 kHz is
      accepted, user 2026-10-06). Benchmarks: `PhaseAlignDspTests "chain cost per
      stereo frame"` (Release); `[dump]` writes renders for comparing two builds bit for bit.
   - The stem analysis and auto-suggest (ANALYSE) work is dead last. Real multi-mic stem pairs are still wanted for the
     P2/P3 listening.
   - Installing (when the user asks): `/Library/Audio/Plug-Ins/{Components,VST3}`, not ~/Library.

## Key documents

- `PLAN.md`: the design spec (what the plugin does and why).
- `IMPLEMENTATION_PLAN.md`: how it gets built, in milestone order, plus deviations from PLAN.md that need sign-off.
- `HANDOVER.md`: the next work that needs no input from the user, in order (de-cramping Hi/Lo first).
- `ui/ui-info.csv`: UI artwork positions and sizes. **x/y are the centre of each image**, in a 1954x1224
  design space. The asset pipeline renders every image at 2.5x of the 782x490 default size, which is exactly its
  slot in the design space (1954x1224 for `ui/plugin-base.png`), and never enlarges a master. The user may replace
  the upscaled 3908x2448 base with the original 1954x1224 one; either works.
