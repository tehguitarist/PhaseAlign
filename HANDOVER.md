# Handover: where things stand, and what's next

Updated 2026-10-08, at the end of the session that built ANALYSE into the plugin (plan R26) and merged it to master.
Read CLAUDE.md first; plan sections and R-numbers are IMPLEMENTATION_PLAN.md. The plugin is feature-complete for now:
what's left is the user's verdicts, their new stems, and a push.

## Ground rules (from the user)

- `.venv` for all Python; clang-format on touched C++; **ask before committing**; work on a branch and merge to master only
  when the user says; **don't push unless asked**. The sandbox blocks force-pushes: the user runs those themselves.
- The only mention of another product anywhere in the repo (docs, code, comments, **commit messages**) is the README's
  one "Inspired by phase alignment tools like ..." sentence. Grep what you push (added lines and messages). One commit
  message broke this once and history was rewritten to fix it, so check first.
- Don't change the Hi/Lo mapping (plan 2.3: the four shapes and their reference frequencies), the 4097-tap Hilbert or the
  fade and glide lengths without asking.
- The Python references are the specs: the C++ must match its goldens (the DSP within 1e-6, ANALYSE's scores within
  1e-9). If a reference changes, change the C++ identically, regenerate (`prototype/golden.py`,
  `prototype/analyse_golden.py`), and say why.
- **Don't change ANALYSE's search until the user's new stems are in** (user, 2026-10-08: "wait for more samples for a
  really comprehensive check before modifying too much").
- Anything the user would hear goes to them, with numbers, before it merges. The user asks for each commit, merge and
  install to /Library explicitly, and tries builds in a DAW between rounds.
- Keep CPU and latency as low as possible, but working matters more (user, 2026-10-06).
- Budgets per stereo frame (`PhaseAlignDspTests "chain cost per stereo frame"`, Release): Hi/Lo < 50 ns, Constant
  < 150 ns. Hi/Lo at 44.1 kHz over 50 ns on the Windows/Linux CI runners is accepted (user, 2026-10-06).
- macOS releases are separate arm64 and Intel builds, never universal (R14); local builds are arm64.
- Naming: the phase modes are LOW, HIGH and CONSTANT; the button is RANGE **out** (one section) and **in** (two
  sections), never "90/180" (R16). The scale label and readout still show the number the knob reads up to.

## State

- **Everything is on master**; the `analyse` branch was merged on 2026-10-08 and deleted. GitHub has master up to
  `8f906f0` (green in CI on all three platforms, with all the meter work); **nothing after it is pushed**: the ANALYSE
  groundwork (prototype only) and ANALYSE itself. No worktrees.
- The build installed in `/Library/Audio/Plug-Ins/{Components,VST3}` (when the user asks: not ~/Library) is the Release
  build of `cb3df41` (ANALYSE with its warnings and progress bar), arm64; auval passes. Master differs from it only in
  ways nobody would see or hear: the option merging measured over every bin (no result changed on any pair), the
  CorrelationAnalyser cleanup, docs.
- Licence: GNU AGPLv3 (`LICENSE`); JUCE under its AGPLv3 option; notices in `THIRD_PARTY_NOTICES.md`.
- Build directories (gitignored): `build-ui` (tests: `cmake -B build-ui -DPA_STANDALONE=ON`, then `cmake --build build-ui
  --target PhaseAlignTests PhaseAlignDspTests` and `ctest --test-dir build-ui`), `build-release` (benchmarks, the
  installed plugin), `build-simd` (the convolver's `Simd.h` path forced on with `-DPA_CONVOLVER_SIMD=1`, to test it off
  Windows).

## Waiting for the user

1. **ANALYSE in a DAW** (plan R26, README "ANALYSE"): the flow (capture to 10 s, stop, the analysing bar, the options at
   the real size), applying and A/B by ear with ORIGINAL, how their host's undo treats the seven-parameter gesture (one
   step or several; JUCE can't ask for a group), the warnings on unrelated material, and Logic with no sidechain chosen
   (the capture should stay at 0 s).
2. **The meter in a DAW** (plan R17 to R24): the drop-up and the three views; SLOW against FAST; the vectorscope on a real
   stereo track and its SOURCE toggle; ALIGNMENT's captured hit following the knobs, the 2 s hold-off, wheel and -/+ zoom;
   HOLD and the knob previews; the legend toggles; the Logic fix (a sidechain that is a copy of the input counts as none;
   tested with synthetic buffers only). Bleed between mics is out of scope (user).
3. **New stems, then a comprehensive check of ANALYSE** (see "ANALYSE: the check that waits for the new stems" below).
4. **Listening:** M2 in a DAW (the delay and phase as built) and P3 fade tuning (`prototype/out/p3/`) on multi-mic stems;
   the second blind set (`prototype/out/analyse/blind2/`, key in `key.txt`, opened after listening).
5. **A push** when the user says: master's ANALYSE work goes to GitHub, and CI's first look at it (below).

**Decided (user):** HIGH with RANGE in keeps the asterisk (the tooltip explains it); the de-cramping of LOW with RANGE in
is left; latency re-alignment per host is fine; no host-specific sidechain steps in the README, and other hosts are not
going to be checked; the RANGE in A/B renders are good and Constant's true rotation is useful (2026-10-07).

## Next: needs no input

- **After the next push, watch CI** (`gh run list --branch master`; a watcher in the background is fine) and report. It
  is the first time Windows and Linux build `src/analyse` and PFFFT's double-precision engine (`pffft_double.c`, SSE2 on
  x86); nothing in it is platform specific, but CI hasn't seen it. `release.yml` refuses a commit without a green CI run
  (`skip_ci_check` overrides); its first real run also checks the Windows and Linux installers' docs step.
- Re-run `tools/meter_profile.py` (Instruments) once: the meter's CPU was measured with the analyser benchmark only (R21,
  about 0.2% of a core), not with the screen's drawing.
- P3 with the user's stems: `PhaseAlignDspTests "[p3]"` also renders the five pairs (`stem_<tag>` sources, from
  `captures/`); all 45 renders stayed within 1.08x of the stem's own worst step (to 20 kHz). Run it before a change to the
  fades.
- Optional CPU: the chain adds about 6 ns around the Hi/Lo stage that it didn't before, and Hi/Lo kept warm in Constant
  costs about 13 ns there (plan 2.6 J). Neither is over budget.

## ANALYSE: what was built (plan R26 has the details and numbers)

- **The search** (`src/analyse/Search`), a port of `prototype/analyse.py` `suggest_with_shift`, which stays the spec:
  one cross-spectrum of the whole capture, every candidate (HIGH and LOW in both RANGEs and CONSTANT in 2.5° steps,
  polarity, the delay to a quarter of a sample, refined to the knob's 0.1 sample) scored in closed form as the mean
  1/3-octave band r; the attack lag decides the delay when its peak is strong (>= 0.35); options merged when they would
  sound alike on the capture; up to two options, two only when within 0.01; a manual shift in samples when the offset is
  beyond ±4 ms; only the stages switched on are searched, polarity always. 0.03 to 0.7 s in Release for 3 to 27 s of
  audio. Golden-tested: `prototype/analyse_golden.py` writes `tests/golden/analyse_*` (six synthetic cases, committed)
  and `captures/analyse_user_expected.txt` (the user's 11 pairs in three scopes, for the hidden `"[.useranalyse]"`).
- **Robustness** (2026-10-08, from cross-instrument pairs of the user's stems): an attack reading is clear only with a
  peak of at least 0.15; a chance test (the search again with the sidechain circularly shifted by 0.37 and 0.61 of the
  capture, the smaller gain kept) flags options that don't beat it by 0.02 as "the audio seems unrelated"; options whose
  r stays under 0.12 are flagged "a weak match". Warnings, not rejections (user: guitar and bass playing one part may
  still want lining up). Kick against snare, bass against kick, hats against snare: nothing worth changing.
- **The capture and the session** (`src/analyse/Session`, `CaptureFifo`, in the processor, so they outlive the editor):
  10 ms stretches where both play above -60 dBFS and differ, while the transport plays; a 10 s minimum, 30 s
  recommended and kept (user); the transport stopping analyses; the search on its own thread, cancellable, reporting
  its progress.
- **The screen** (`src/ui/AnalyseScreen`, in the meter's place while the button is lit): capturing (what is searched,
  the seconds against the minimum, the levels; CLEAR, ANALYSE NOW), analysing (cycling dots and a bar, at least 0.6 s),
  results (up to two options and ORIGINAL; a click applies one as one gesture; a BANDS or ALIGNMENT before/after preview
  over the capture; AGAIN). A second press of the button goes back to the meter, leaving what is applied.
- Known behaviour to mention if asked: an option whose phase is "off" switches PHASE off, so the next ANALYSE doesn't
  search the phase (the screen's header says what is searched).

## ANALYSE: the check that waits for the new stems

Run all of this together once the user's new stems are in (export them like the second set: `prototype/analyse_stems.py`
lists the pairs; `prototype/analyse_golden.py` writes their raw copies and the expected results):

1. **The thresholds**, all first guesses from the user's 19 pairs, some with small margins: `ATTACK_STRONG` 0.35,
   `ATTACK_MIN_PEAK` 0.15 (genuine pairs 0.17 to 0.88, unrelated 0.01 to 0.12), `MIN_GAIN` 0.03, `MIN_MARGIN` 0.01,
   `CHANCE_MARGIN` 0.02 (genuine pairs beat the chance test by +0.037 to +0.53, unrelated ones by +0.016 and +0.007),
   `WEAK_MATCH` 0.12 (genuine pairs 0.13 or more, guitar against bass 0.075 and 0.082).
2. **The scoring measure:** `prototype/analyse_metrics.py` scores the candidates the user compared by ear with five
   measures; the current one (mean band r) agrees with 14 of 21 verdicts, as well as any (a loudness-weighted sum gain
   also 14, the others 12 or 13). The misses are transient timing (handled by the attack-first/joint switch), the bass's
   low end (the "less low end" flag) and near-ties within 0.003 (the two-option display). Add the new verdicts to its
   `PREFS` before changing the measure. A finer angle grid gains nothing (at most 0.0001 r at 0.5°).
3. **To hear** (a proposed third blind set, sums with the sidechain, with `analyse_shootout.py`): the set 1 kick, where
   ANALYSE now picks LO in 72.5° Ø at +3.19 ms (its attack peak, 0.27, is under `ATTACK_STRONG`, so the waveform score
   sets the delay) although the user preferred +1.57 ms in the first listening and the attack reading says +1.28 ms
   (+3.19 was never heard); snare sample after its −284 sample shift (HI in 180°, −1.05 ms) against CONSTANT 50° at the
   whole −5.94 ms, which the ear liked; the bass amp's pick against off.
4. **Optional, the user's call:** a note when the best setting swings between parts of the capture (split into thirds).
   ANALYSE keeps giving the averaged best option, which is what the user wants.

What the groundwork's listening found, for context: judge candidates by the sum with the sidechain, not the track alone;
in the first blind shootout the search's pick was best on 5 of 6 pairs (the bass the exception, where off was best:
hence "less low end"); all the kick and snare winners were Ø plus a rotation. The full history is in git (this file
before 2026-10-08) and plan R26.

## Gotchas that cost time

- `HiLoStage::process` takes at most `HiLoStage::maxBlock` (32) samples per call; more aborts without a message.
- CI's random sequences differ from the Mac's (libstdc++ and MSVC against libc++), so a test that passes on one seed can
  fail elsewhere: run several seeds, and when CI fails on x86 only, look at the seed first.
- The reference unit's measurements live in `captures/` (gitignored; never name the unit in the repo). The analysis
  scripts that read them were one-offs; the findings are in plan 2.3.
- After rewriting history, `git filter-branch` leaves a backup ref under `refs/original`: delete it once the push is done.
- The hidden tests on the user's pairs need files in `captures/` (gitignored): `[.usercapture]` and `[.bandgate]` want
  `captures/*_a.f32` and `*_b.f32` (`python prototype/export_pairs.py`); `[.useranalyse]` wants
  `captures/analyse_user_expected.txt` and `captures/stems/*_a.f32` (`python prototype/analyse_golden.py`, a few minutes:
  the Python search runs 33 times). `[.analysercost]` and `[.analysecost]` want a Release build (`build-release`).
- Python's output to a file is buffered: a long `analyse_golden.py` run shows nothing until it ends.
- `tests/EditorTests.cpp` "meter on screen" plays audio slower than real time and uses wall-clock timers (the 2 s capture
  hold-off, the 0.8 s bar hold), so its timing checks are written around that: touch a knob right before checking a
  hold-off, and don't assume 0.1 s of audio is 0.1 s of wall time. ANALYSE's flow tests drive the session with
  `tickForTesting()` and `waitForAnalysisForTesting()` instead of its timer.
- Restoring a file with `mv` keeps its old timestamp, so make does not rebuild it: `touch` it.
- `juce::AudioProcessor::getSampleRate()` is 0 in tests that call `prepareToPlay` directly; use the processor's own
  `sampleRate` atomic.
