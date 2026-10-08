# Handover: where things stand, and what's next

Updated 2026-10-08, at the end of the session that refined ANALYSE on the user's newer stem sets (plan R26, last paragraphs).
Read CLAUDE.md first; plan sections and R-numbers are IMPLEMENTATION_PLAN.md. The plugin is feature-complete for now:
what's left is the user's listening (the blind test below), their DAW verdicts, a merge and a push.

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
- **ANALYSE's search follows the data** (user, 2026-10-08: the new stems are in and refinements from them are welcome, then
  the whole blind test is redone on the one algorithm). Refinements still go in both the Python spec and the C++, with goldens
  regenerated; a change after the blind test was built means rebuilding it (`prototype/build_blind3.py`) so every version
  the user hears is from the same algorithm. Don't name any mic or preamp model in the repo: the stem sets' names live in
  `captures/stems3/pairs.json` (gitignored).
- Anything the user would hear goes to them, with numbers, before it merges. The user asks for each commit, merge and
  install to /Library explicitly, and tries builds in a DAW between rounds.
- Keep CPU and latency as low as possible, but working matters more (user, 2026-10-06).
- Budgets per stereo frame (`PhaseAlignDspTests "chain cost per stereo frame"`, Release): Hi/Lo < 50 ns, Constant
  < 150 ns. Hi/Lo at 44.1 kHz over 50 ns on the Windows/Linux CI runners is accepted (user, 2026-10-06).
- macOS releases are separate arm64 and Intel builds, never universal (R14); local builds are arm64.
- Naming: the phase modes are LOW, HIGH and CONSTANT; the button is RANGE **out** (one section) and **in** (two
  sections), never "90/180" (R16). The scale label and readout still show the number the knob reads up to.

## State

- **Master has ANALYSE** (merged 2026-10-08); the refinements from the newer stems are committed on the branch
  `analyse-refinements`, **not merged and not pushed** (the user merges and pushes when they say). GitHub has master up to
  `8f906f0` (green in CI on all three platforms, with all the meter work). No worktrees.
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
3. **The blind listening test** (see "The newer stem sets and the blind test"): their `results.json`, then the decode, the
   disagreements between ear and score, and a confirmation round before any change goes in.
4. **Listening:** M2 in a DAW (the delay and phase as built) and P3 fade tuning (`prototype/out/p3/`) on multi-mic stems;
   the second blind set (`prototype/out/analyse/blind2/`, key in `key.txt`, opened after listening).
5. **A push** when the user says: master's ANALYSE work goes to GitHub, and CI's first look at it (below).

**Decided (user):** HIGH with RANGE in keeps the asterisk (the tooltip explains it); the de-cramping of LOW with RANGE in
is left; latency re-alignment per host is fine; no host-specific sidechain steps in the README, and other hosts are not
going to be checked; the RANGE in A/B renders are good and Constant's true rotation is useful (2026-10-07).

## The newer stem sets and the blind test

Four sets of the user's stems are in `captures/` (gitignored): the first two rounds (VST drums, bass, guitar, hats: 11 pairs,
`captures/*_a.f32` and `captures/stems/`), `captures/stems2/` (a kit with rooms, bleed, samples, bass DI and amp, two guitars;
48 kHz, 32 s) and `captures/stems3/` (one song, 4 min 12 s at 44.1 kHz, several mics per source; names SOURCE-MIC-PREAMP, `.L` and
`.R` combined; which track is whose sidechain is in `captures/stems3/pairs.json`).

- `prototype/analyse_stems2.py` and `analyse_stems3.py` run every pair (stems2: whole, first and last 15 s, curves to 40 ms;
  stems3: consecutive windows of 30 s of active audio, as the plugin's capture would hold it; `--only NAME` re-runs one
  sidechain). Results in `prototype/out/analyse/stems2/` and `stems3/` (`results.json`).
- `prototype/build_blind3.py` builds the blind page (`captures/stems2/blind/index.html`, opaque ids, build id in the page and
  the key) and the key (`prototype/out/analyse/blind3/key.json`: **don't open it, and don't print option kinds, before the
  user's `results.json` is back**). 45 tests in four parts (the kit, more of the kit, the song, the earlier stems), up to 9
  versions each, every version the track summed with the sidechain (REF plays the sidechain alone), a polarity-flipped control in
  every test and hidden repeats in some. Options: off, ANALYSE's picks, the best on paper, the best under 300 Hz, delay only,
  the next peak, the smallest shift beyond the reach, the best as it is when a shift is advised (`noshift`), the other windows'
  picks, and `hit-timing` (kick and snare pairs: the delay from `prototype/hit_timing.py`, a candidate for the ear, not part of
  ANALYSE). A rebuild changes the build id and the page sets aside answers saved by the old one.
- `prototype/blind3_decode.py results.json` joins the results with the key (refuses another build): per test the ranked options with
  their settings and comments, then the pooled mean rank by option kind, the controls (the flipped version should be last, a
  repeat should rank like what it repeats) and each part.
- What the data showed (2026-10-08, plan R26): attack readings and waveform scores both slip whole cycles on tonal drum bodies (the
  snare sample's attack read -5.9 ms and its score peaks sit at -17 to -20 ms, though the user hears no flam: within 100
  samples); the first rise of the high-passed envelope per hit (`hit_timing.py`) put the kick and snare samples at -29 to +47 samples
  with a spread under 0.2 ms; overheads sit 4 to 7 ms from the kick and snare (the user: up to 2.5 m); the chance flag misfired on
  strong matches; the shift advice misfired on a note's period and on cycle slips. All fixed in the search except the hit-timing
  witness, which waits for the ears.
- **Still open (the user's call after listening):** promote `hit_timing` to the delay authority for percussive pairs (and an
  explicit percussive-against-sustained measure from it); a low-end-weighted score for rooms; a note when the pick swings between
  windows (AC guitar mics, overheads); the weak-match flags on the delay-only path (bass against guitar, attacks agreeing in time
  but not in phase); a periodic-peak guard for sustained notes (a bass note repeated gives equal peaks one period apart).

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
  sound alike on the capture; up to two options, two only when within 0.01; a manual shift in samples only for a significant
  gain beyond ±4 ms, the smallest that gets most of it, with the clear-attack rules and the lower gain at the knob's edge (2026-10-08,
  plan R26: `shift_advice` / `shiftAdvice`); with a shift advised, also the best as it is (`optionsAsIs`, small ones kept); only the
  stages switched on are searched, polarity always. 0.03 to 0.7 s in Release for 3 to 27 s of audio. Golden-tested: `prototype/analyse_golden.py` writes `tests/golden/analyse_*` (six synthetic cases, committed)
  and `captures/analyse_user_expected.txt` (the user's 11 pairs in three scopes, for the hidden `"[.useranalyse]"`).
- **Robustness** (2026-10-08, from cross-instrument pairs of the user's stems): an attack reading is clear only with a
  peak of at least 0.15; a chance test (the search again with the sidechain circularly shifted by 0.37 and 0.61 of the
  capture, the smaller gain kept) flags options that don't beat it by 0.02 as "the audio seems unrelated"; options whose
  r stays under 0.12 are flagged "a weak match" (the chance flag is not raised, and its test not run, when the best option's r is 0.30 or
  more). Warnings, not rejections (user: guitar and bass playing one part may
  still want lining up). Kick against snare, bass against kick, hats against snare: nothing worth changing.
- **The capture and the session** (`src/analyse/Session`, `CaptureFifo`, in the processor, so they outlive the editor):
  10 ms stretches where both play above -60 dBFS and differ, while the transport plays; a 10 s minimum, 30 s
  recommended and kept (user); the transport stopping analyses; the search on its own thread, cancellable, reporting
  its progress.
- **The screen** (`src/ui/AnalyseScreen`, in the meter's place while the button is lit): capturing (what is searched,
  the seconds against the minimum, the levels; CLEAR, ANALYSE NOW), analysing (cycling dots and a bar, at least 0.6 s),
  results (up to two options and ORIGINAL; with a shift advised the rows that work as it is first, then those marked AFTER THE SHIFT,
  small improvements flagged; a click applies one as one gesture; a BANDS or ALIGNMENT before/after preview over the capture;
  AGAIN); the screen's text is large (20 and 17 design units). A second press of the button goes back to the meter, leaving what is applied.
- Known behaviour to mention if asked: an option whose phase is "off" switches PHASE off, so the next ANALYSE doesn't
  search the phase (the screen's header says what is searched).

## ANALYSE: the check for when the listening is back

1. **Decode** (`prototype/blind3_decode.py`): do the flipped controls come last and the repeats agree? Then, per option kind, which
   wins where (the best on paper, the best under 300 Hz, ANALYSE's pick, delay only, `hit-timing`, `noshift` against the shifted
   picks, the other windows' picks), and where the ear and the score disagree (add the verdicts to `analyse_metrics.py`'s `PREFS`
   before changing the measure).
2. **The thresholds**, first guesses from about 60 pairs, some with small margins: `ATTACK_STRONG` 0.35, `ATTACK_MIN_PEAK` 0.15
   (genuine pairs 0.17 to 0.88, unrelated 0.01 to 0.12; the snare sample passes at 0.17), `MIN_GAIN` 0.03, `SMALL_GAIN` 0.01,
   `MIN_MARGIN` 0.01, `SHIFT_GAIN` 0.04, `SHIFT_GAIN_EDGE` 0.02 and `SHIFT_FRACTION` 0.75 (genuine gains +0.04 to +0.084, unrelated up
   to +0.032), `CHANCE_MARGIN` 0.02 and `CHANCE_SKIP_R` 0.30, `WEAK_MATCH` 0.12. The listening can't test warnings: they rest on the numbers.
3. **The scoring measure:** `prototype/analyse_metrics.py` scores the candidates the user compared by ear with five measures; the current
   one (mean band r) agreed with 14 of 21 verdicts, as well as any. The misses were transient timing (the attack-first/joint switch),
   the bass's low end (the "less low end" flag) and near-ties within 0.003. A finer angle grid gains nothing (at most 0.0001 r at 0.5°).
4. **Then** a short confirmation set built from whatever changes, before they go in, as the user asked for each round.

What the groundwork's listening found, for context: judge candidates by the sum with the sidechain, not the track alone;
in the first blind shootout the search's pick was best on 5 of 6 pairs (the bass the exception, where off was best:
hence "less low end"); all the kick and snare winners were Ø plus a rotation. The full history is in git (this file
before 2026-10-08) and plan R26.

## Gotchas that cost time

- When filtering build output, grep for `error` (not ` error `): `file.cpp:12:3: error: ...` has a colon after it, and a missed compile
  error means the tests run a stale binary and pass. Check the tail of the build, or `ctest` from a clean build, before trusting a pass.
- Never start Python's `multiprocessing` from a heredoc (`python - <<E`): the spawn start method re-imports `__main__` and forks
  endlessly. Put the code in a file with an `if __name__ == "__main__":` guard.

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
