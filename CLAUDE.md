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
job, better. That includes the mic and preamp models in the user's stem names: they live in the gitignored `captures/`.

## Current status (update as work progresses)

As of 2026-10-08 everything is on master and pushed (GitHub master at the merge of `analyse-refinements`: ANALYSE and its
refinements from the user's newer stems, larger text, a finished README); CI's first look at `src/analyse` on Windows and Linux
is the next thing to check. **Start a session with `HANDOVER.md`**: what is next, what waits for the user (their DAW verdicts
and CI; the blind listening test is done), and the ground rules. The plan's R-table (IMPLEMENTATION_PLAN.md) records every decision.

1. **What is built.** The delay (−4 to +4 ms in steps of 0.1 sample, windowed-sinc kernels; latency = the reach plus the
   lookahead while on; plan 2.1a, R12), polarity, the phase stage (HIGH and LOW all-pass shapes in both RANGEs, run
   oversampled between linear-phase halfbands so they match analog to 20 kHz at every rate, R13, R15, R16; CONSTANT, a
   4097-tap Hilbert FIR, a true rotation, about 43 ms of latency), host bypass, section dimming (alpha 0.4, approved),
   the tooltips switch (R25), the meter (BANDS, VECTORSCOPE, ALIGNMENT; R17 to R24) and **ANALYSE** (R26: capture, a
   search golden-tested against `prototype/analyse.py`, the options screen with ORIGINAL, before/after previews,
   warnings for chance-level and weak matches, a manual-shift advice only for a significant gain with a best effort as the track
   stands, a progress bar). Licence: GNU AGPLv3.
2. **Hi/Lo shapes:** each (mode, range) is its own shape from the reference unit's captured geometries (LOW out 75.1 Hz,
   LOW in two stacked at 150.1 Hz, HIGH out 150.1 Hz, HIGH in 75.1 Hz + 1502 Hz); the modes are named for where the
   middle of the turn sits (plan 2.3). HIGH with RANGE in shows the first section's angle, 0-90, with an asterisk. The
   user approved the RANGE A/B renders (2026-10-07). Don't change the mapping without asking. The reference unit's
   captures are in `captures/` (gitignored); never name it in the repo (see Naming above).
3. **Python references** (`prototype/`, all through the venv) and their C++ goldens, within 1e-6 (DSP) or 1e-9
   (ANALYSE's scores): `hilo.py` and `p2_constant.py` → `golden.py` → `tests/golden/`; `analyse.py` (the ANALYSE spec) →
   `analyse_golden.py` → `tests/golden/analyse_*` and, from the user's pairs, `captures/analyse_user_expected.txt`.
   Regenerate whenever a reference changes, and change the C++ identically. Also: `analyse_stems.py`, `analyse_stems2.py` and
   `analyse_stems3.py` (every pair of the newer stem sets, results in `prototype/out/analyse/`), `build_blind3.py`,
   `blind3_page.html`, `blind3_decode.py` and `analyse_blind3_metrics.py` (the blind listening test, its decoder and the measures
   scored against its results; the key is open now that the results are back), `hit_timing.py` (onset timing per hit, a candidate witness for percussive pairs),
   `analyse_shootout.py` (the earlier blind listening sets), `analyse_metrics.py` (scoring measures against the user's verdicts),
   `export_pairs.py` (raw copies of the user's pairs for the hidden tests), `p3_report.py`, `p4_meter.py`,
   `subsample.py`, `hf_check.py`, `oversampling.py`, `decramp_ab.py`, `range_ab.py`, `modes_figure.py`.
4. **Build and test** (see also HANDOVER "Gotchas"):
   - Art or `ui/ui-info.csv` changed: `tools/build_assets.sh` (regenerates `src/ui/Layout.h` and `assets/images/`),
     then re-run CMake. Adding a control = CSV rows + a row in the editor's table (`toggleSpecs` in PluginEditor.cpp).
   - `cmake -B build-ui -DPA_STANDALONE=ON && cmake --build build-ui --target PhaseAlignTests PhaseAlignDspTests`, then
     `ctest --test-dir build-ui`. `PhaseAlignDspTests` is the light runner (src/dsp, src/meter, src/analyse; juce_dsp
     only; counts allocations). Snapshots: `PA_SNAPSHOT_DIR=<dir> build-ui/PhaseAlignTests_artefacts/Debug/PhaseAlignTests
     "[snapshot]"` (the editor and ANALYSE's screens); on screen: `"[desktop]"`.
   - Release builds (`build-release`) for benchmarks: `PhaseAlignDspTests "chain cost per stereo frame"`, `"[bench]"`,
     `"[.analysercost]"`, `"[.analysecost]"`. Hidden tests on the user's pairs: `"[.useranalyse]"`, `"[.usercapture]"`,
     `"[.bandgate]"` (they need `captures/`, see HANDOVER).
   - CI runs on master pushes, PRs and manual dispatch: tests, the DSP benchmark and pluginval on all three platforms
     (pluginval gates the Windows and Linux jobs); `release.yml` refuses a commit without a green CI run.
   - Installing (when the user asks): `/Library/Audio/Plug-Ins/{Components,VST3}`, not ~/Library.
5. **Waiting for the user:** their DAW verdict on the meter and ANALYSE; the CI result. The blind listening test is done (2026-10-09,
   plan R27): ANALYSE stays as it is. Details: HANDOVER.

## Key documents

- `PLAN.md`: the design spec (what the plugin does and why).
- `IMPLEMENTATION_PLAN.md`: how it gets built, in milestone order, plus deviations from PLAN.md that need sign-off.
- `HANDOVER.md`: where things stand, what waits for the user, the next work, ground rules and gotchas. Read it first.
- `ui/ui-info.csv`: UI artwork positions and sizes. **x/y are the centre of each image**, in a 1954x1224
  design space. The asset pipeline renders every image at 2.5x of the 782x490 default size, which is exactly its
  slot in the design space (1954x1224 for `ui/plugin-base.png`), and never enlarges a master. The user may replace
  the upscaled 3908x2448 base with the original 1954x1224 one; either works.
