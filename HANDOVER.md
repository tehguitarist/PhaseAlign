# Handover: where things stand, and what's next

Updated 2026-10-07, at the end of the session that rebuilt the meter (R17 to R24) after the RANGE redesign. Read CLAUDE.md
first; plan sections are IMPLEMENTATION_PLAN.md. Nothing here needs code first: what's left is decisions and listening
(the meter above all).

## Ground rules (from the user)

- `.venv` for all Python; clang-format on touched C++; **ask before committing**; work on a branch and merge to master only
  when the user says; **don't push unless asked**. The sandbox blocks force-pushes: the user runs those themselves.
- The only mention of another product anywhere in the repo (docs, code, comments, **commit messages**) is the README's
  one "Inspired by phase alignment tools like ..." sentence. Grep what you push (added lines and messages). One commit
  message broke this once and history was rewritten to fix it, so check first.
- Don't change the Hi/Lo mapping (plan 2.3: the four shapes and their reference frequencies), the 4097-tap Hilbert or the
  fade and glide lengths without asking.
- The goldens must match within 1e-6. If a Python reference changes, change the C++ identically, regenerate with
  `prototype/golden.py`, and say why.
- Anything the user would hear goes to them, with numbers, before it merges. (In the 2026-10-07 session the user asked for
  each commit, merge and install to /Library explicitly, and tried builds in a DAW between rounds.)
- Keep CPU and latency as low as possible, but working matters more (user, 2026-10-06).
- Budgets per stereo frame (`PhaseAlignDspTests "chain cost per stereo frame"`, Release): Hi/Lo < 50 ns, Constant
  < 150 ns. Hi/Lo at 44.1 kHz over 50 ns on the Windows/Linux CI runners is accepted (user, 2026-10-06).
- macOS releases are separate arm64 and Intel builds, never universal (R14); local builds are arm64.
- Naming: the phase modes are LOW, HIGH and CONSTANT; the button is RANGE **out** (one section) and **in** (two
  sections), never "90/180" (R16). The scale label and readout still show the number the knob reads up to.

## State

- **master is `a0c7c81`** (plus the commit of this handover cleanup), 16 or more commits ahead of GitHub: **nothing from
  2026-10-07 is pushed** (the last pushed commit, `7a9a676`, was green in CI on all three platforms: `gh run list
  --branch master`). Master is the only branch, locally and on GitHub. No worktrees.
- The build installed in `/Library/Audio/Plug-Ins/{Components,VST3}` (when the user asks: not ~/Library) is the Release
  build of a0c7c81 (12:59 on 2026-10-07), arm64.
- Licence: GNU AGPLv3 (`LICENSE`); JUCE under its AGPLv3 option; notices in `THIRD_PARTY_NOTICES.md`.
- Build directories (gitignored): `build-ui` (tests: `cmake -B build-ui -DPA_STANDALONE=ON`, then `cmake --build build-ui
  --target PhaseAlignTests PhaseAlignDspTests` and `ctest --test-dir build-ui`), `build-release` (benchmarks),
  `build-simd` (the convolver's `Simd.h` path forced on with `-DPA_CONVOLVER_SIMD=1`, to test it off Windows).

## Done in the last two sessions (details in the plan)

**2026-10-07, the meter (plan R17 to R24; CLAUDE.md item 9):** BANDS (default), VECTORSCOPE (with STEREO) and ALIGNMENT
(CAPTURE, zoom buttons), SLOW/FAST, HOLD with the knobs previewing on the frozen picture (delay, polarity and the phase
stage's closed-form response, checked against the real Chain to 2 degrees), blue input, hideable traces, per-view CPU
(2.8% of a core down to about 0.25%), and fixes from the user's DAW use: the Logic no-sidechain copy, bands vanishing on
FAST, the vectorscope ignoring SLOW/FAST. The old FREQUENCY, TIME OFFSET and PHASE views are gone from the UI; their
analysis stays for ANALYSE. Tuned on the user's own stem pairs (`prototype/export_pairs.py`).

**2026-10-06 and 07, before that (the RANGE redesign):**

1. **RANGE redesign (plan 2.3, R15, R16).** Each (mode, range) is its own shape built from the reference unit's measured
   geometries (nothing invented): LOW out 75.1 Hz; LOW in two stacked at 150.1 Hz; HIGH out 150.1 Hz; HIGH in 75.1 Hz
   and 1502 Hz. RANGE out is unchanged from before; only RANGE in changed. Fitted against the measurements each
   shape is within about 2° rms (`prototype/range_ab.py` renders the A/B files).
2. **Names:** LOW centres the turn lower down, HIGH higher up, in both ranges (the frequency where half the full turn has
   happened at full knob: 75 / 150 Hz for LOW out / in, 150 / about 336 Hz for HIGH). With RANGE in HIGH is also more
   spread out. NARROW / WIDE was rejected: with RANGE out both modes have the same width. README "Where the turn sits".
3. **The number on the panel is the real phase.** At full knob the scale label, the readout and the measured phase agree
   in every state, tested end to end through the processor (`tests/EditorTests.cpp`, "the phase readout is the phase the
   plugin applies"). The one exception is HIGH with RANGE in, which shows 0 to 90 with an asterisk.
4. **Glide bug found by CI and fixed (plan 2.3 "Switching shape").** The glide state is a weight on each of the four
   shapes. The step test runs several seeds because the random sequences differ between standard libraries.
5. **Windows Constant 193 ns to 53 ns** (MSVC wasn't vectorising the convolver; under `_MSC_VER` only it now uses
   `Simd.h`; plan 2.6 L). pluginval passes on all three platforms and gates the Windows and Linux CI jobs.
6. **UI and docs:** minimum editor size 60% (586 px); the README is a manual with screenshots (`docs/images/`, the meter
   ones made from the desktop test's snapshots) and a phase modes section with a figure (`prototype/modes_figure.py`);
   dimming alpha 0.4 approved.
7. **Frequency-response check of the A/B sums** (`prototype/range_fr_check.py`, plot in `prototype/out/range/fr_check.png`).

## Next: needs the user (decisions and listening)

1. **The meter (plan R17 to R24): the user's verdict in a DAW is outstanding.** Things to hear or see there: the drop-up
   menu and the three views; SLOW (slow and smooth) against FAST; the vectorscope on a real stereo track and its SOURCE
   toggle; ALIGNMENT's captured hit following the DELAY and phase knobs, the 2 s hold-off, wheel and -/+ zoom; HOLD and
   the knob previews; the legend toggles; and the Logic fix (a sidechain that is a copy of the input counts as none; the
   cause is the user's finding, the fix is only tested with synthetic buffers). The meter has no tuning left that doesn't
   need their ears. Bleed between mics is out of scope (user).
2. **Listening.** Done (user, 2026-10-07): the RANGE in A/B files are good, and Constant's true rotation is useful.
   Left: M2 in a DAW (below) and P3 fade tuning (`prototype/out/p3/`), with the user's multi-mic stems.
3. **Decided (user, 2026-10-07):** HIGH with RANGE in keeps the asterisk (the tooltip explains it); the de-cramping of LOW
   with RANGE in is left; latency re-alignment per host is fine; the README no longer carries host-specific sidechain
   steps and other hosts are not going to be checked.
4. **`release.yml`** now refuses to run on a commit without a green CI run (the `version` job checks `gh run list --workflow
   ci.yml --commit $GITHUB_SHA`; the `skip_ci_check` input overrides it). Its first real run also checks the Windows and
   Linux installers' docs step.
5. **ANALYSE (auto-suggest)** is next after the meter, and **the cleanup of what it doesn't use comes after that** (user,
   2026-10-07). The button is present but does nothing. What is ready for it: the waveform (PHAT) and attack lags and
   the curve and phase measures in `meter/CorrelationAnalyser` (off unless a view asks: `Needs`), the delay, polarity
   and phase response preview (`dsp/PhaseResponse.h`, `meter/HitCapture`), and the user's stem pairs with tests.
   Findings that matter: the waveform lag is wrong for hits whose bodies differ (a kick against a kick sample read +29
   ms), the attack lag (three bands, plan R18) is right on all five pairs; the bass's attack reading is the weakest.
   **Groundwork (2026-10-07): `prototype/analyse.py`** (PLAN 8 step 6). Whole-capture cross-spectrum; every candidate
   (4 Hi/Lo shapes, Constant, polarity, delay ±4 ms at 0.1 sample) scored in closed form as the mean per-1/3-octave-band
   r; top families de-duplicated. Synthetic pairs with a known delay and rotation are all recovered, and predicted scores
   match the plugin's own Hi/Lo rendered (to 0.003). Version 2 is attack-first: `attack_lag` (a Python port of R18; its
   readings equal the C++'s on the five pairs, input = `_a`, positive = delay the input) fixes the delay when clear and
   in reach, and phase and polarity are chosen within 0.3 ms of it. Result: delays are now right (kick +1.57 ms where the
   score alone said +3.2; snare +0.74; bass, guitar, hats ~0), but **the phase and polarity choice is weakly supported on
   these pairs** (r gains of 0.00 to 0.07 over doing nothing; kick's top three settings are within 0.001 of each other
   and the kick wants Ø, which two differently-recorded kicks may simply not justify). Next: a confidence measure
   (gain over baseline, margin over the runner-up family) so ANALYSE says "no change worth making" for guitar and hats,
   and listening to the renders; then the C++ (offline, on a background thread, reusing `dsp/PhaseResponse.h`).
   Report: `prototype/out/analyse/report.md`.

## Next: needs no input

- Watch CI after any push (`gh run list --branch master`; a watcher in the background is fine) and report. The first push
  since 2026-10-06 carries all the meter work: expect the Windows and Linux numbers for the new code (nothing in it is
  platform specific, but CI has not seen it).
- P3 with the user's stems: `PhaseAlignDspTests "[p3]"` also renders the five pairs (`stem_<tag>` sources, from `captures/`);
  2026-10-07: all 45 renders stay within 1.08x of the stem's own worst step (to 20 kHz). Run it before a change to the fades.
- Re-run `tools/meter_profile.py` (Instruments) once: the meter's CPU was measured with the analyser benchmark only
  (R21), not with the screen's drawing.
- Optional CPU: the chain adds about 6 ns around the Hi/Lo stage that it didn't before, and Hi/Lo kept warm in Constant
  costs about 13 ns there (plan 2.6 J). Neither is over budget.

## Gotchas that cost time

- `HiLoStage::process` takes at most `HiLoStage::maxBlock` (32) samples per call; more aborts without a message.
- CI's random sequences differ from the Mac's (libstdc++ and MSVC against libc++), so a test that passes on one seed can
  fail elsewhere: run several seeds, and when CI fails on x86 only, look at the seed first.
- The reference unit's measurements live in `captures/` (gitignored; never name the unit in the repo). The analysis
  scripts that read them were one-offs; the findings are in plan 2.3.
- After rewriting history, `git filter-branch` leaves a backup ref under `refs/original`: delete it once the push is done.
- The hidden meter tests (`[.userpairs]`, `[.usercapture]`, `[.bandgate]`) need `captures/*_a.f32` and `*_b.f32`:
  `source .venv/bin/activate && python prototype/export_pairs.py` makes them from the user's wavs. `[.analysercost]` wants
  a Release build (`build-release`).
- `tests/EditorTests.cpp` "meter on screen" plays audio slower than real time and uses wall-clock timers (the 2 s capture
  hold-off, the 0.8 s bar hold), so its timing checks are written around that: touch a knob right before checking a
  hold-off, and don't assume 0.1 s of audio is 0.1 s of wall time.
- Restoring a file with `mv` keeps its old timestamp, so make does not rebuild it: `touch` it.
- `juce::AudioProcessor::getSampleRate()` is 0 in tests that call `prepareToPlay` directly; use the processor's own
  `sampleRate` atomic.
