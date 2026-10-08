# Handover: where things stand, and what's next

Updated 2026-10-08, after building ANALYSE into the plugin (plan R26, branch `analyse`); before that, 2026-10-07, the
session that rebuilt the meter (R17 to R24) after the RANGE redesign. Read CLAUDE.md
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

- **master is `4e3965b`**; GitHub has master up to `8f906f0`, so the 14 commits since (the ANALYSE groundwork) are not
  pushed (CI: `gh run list --branch master`). **ANALYSE is on the branch `analyse`** (from
  master, not merged; merge when the user says). No worktrees.
- The build installed in `/Library/Audio/Plug-Ins/{Components,VST3}` (when the user asks: not ~/Library) is the Release
  build of the `analyse` branch (`cb3df41`: ANALYSE with the warnings and the progress bar, 2026-10-08), arm64; auval
  passes.
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
5. **ANALYSE (auto-suggest)**: built (2026-10-08), see "ANALYSE: built into the plugin" below. What the groundwork
   (`prototype/analyse.py`, 2026-10-07/08) found that still matters:
   - **Judge by the sum with the sidechain, not the track alone** (user's listening, `prototype/out/analyse/renders/`).
   - **Blind shootout** (`analyse_shootout.py --blind`, the second stem set): the search's pick was best on 5 of 6; the bass
     was the exception (off was best: the pick had less low end, hence the "less low end" flag rather than a reject).
     All the kick and snare winners were Ø plus a rotation.
   - **Decided (user):** the delay stays ±4 ms (a bigger offset is a manual shift in samples); only the stages switched on
     are searched, polarity always; up to two options, two only when within 0.01; less low end is a flag, not a reject.
   - **Untested by ear:** snare sample after its −284 sample shift (HI in 180°, −1.05 ms, where the ear liked CONSTANT 50°
     at the whole −5.94 ms), and the bass's suggestions. The second blind set (`analyse_shootout.py --blind2`,
     `prototype/out/analyse/blind2/`, key in `key.txt`) is waiting for the user's listening.
   The full history is in git (this file before 2026-10-08) and plan R26.

## ANALYSE: built into the plugin (2026-10-08, branch `analyse`, not merged; plan R26)

**What was built** (details and numbers in plan R26; README "ANALYSE"): the search ported to `src/analyse/Search`
(golden-tested against `prototype/analyse.py`, which stays the spec: `prototype/analyse_golden.py` writes
`tests/golden/analyse_*`, and, from the user's pairs, `captures/analyse_user_expected.txt` for the hidden
`PhaseAlignDspTests "[.useranalyse]"`; regenerate both whenever analyse.py changes), the capture and background thread
(`src/analyse/Session`, `CaptureFifo`, owned by the processor), the screen (`src/ui/AnalyseScreen`), applying an option as
one gesture over the changed parameters, and the tests (`tests/dsp/AnalyseTests.cpp`, `tests/AnalyseFlowTests.cpp`;
`PA_SNAPSHOT_DIR=... PhaseAlignTests "ANALYSE snapshots"` draws the screen's states). The user's choices (2026-10-08): a 10 s
minimum and 30 s recommended (first 5 and 10; raised once the chance gains below were measured), the last 30 s kept; the transport stopping analyses, ANALYSE NOW without a transport, a second
press goes back to the meter; clicking an option applies it and ORIGINAL goes back; the preview is BANDS with an
ALIGNMENT toggle.

**Changed in the spec while building (both in analyse.py and the C++, goldens regenerated):** the 0.1-sample refinement
sits on the knob's own grid, and options are merged when they would sound alike on the capture (band-mean correlation of
one's output with the other's, every bin weighted by the track's spectrum, at least cos 20°, for polarity and phase with
delays within 0.5 ms, or for the whole response). The remaining "near neighbour" second options are real alternatives:
snare OH's CONSTANT 5° Ø and LO in 10° Ø are identical to 2 kHz and opposite at 5 to 8 kHz (the same body, another top). On the
user's pairs: kick OH (both scopes with phase) is now one clear suggestion instead of LO in 140° vs 160°; the second
option changed for snare sample (both), snare OH and guitar 2; the first options are unchanged everywhere.

**Next (needs the user):** the verdict in a DAW (the flow, the screen at the real size, applying and A/B by ear, the
undo behaviour of the seven-parameter gesture in their host, the Logic no-sidechain case). Then the refinement, in this
order unless the user says otherwise:
1. **Done (2026-10-08): the chance test and the weak-match warning, and a minimum attack peak** (plan R26). The
   constants (`CHANCE_MARGIN` 0.02, `WEAK_MATCH` 0.12, `ATTACK_MIN_PEAK` 0.15) separate the user's 19 pairs with small
   margins; check them on new stems. Background: on unrelated material the best gain over doing nothing is +0.044 to +0.049 at 5 s (noise
   against noise, and mismatched pairs of the user's stems), over `MIN_GAIN` 0.03; about +0.02 at 10 s and +0.005 at
   20 s; the minimum is now 10 s (user). The user's call: warn, don't hide (guitar and bass playing one part may still
   want lining up).
2. **Waiting for the user's new stems (user, 2026-10-08: "wait for more samples for a really comprehensive check before
   modifying too much").** Then, together: the thresholds (`ATTACK_STRONG`, `MIN_GAIN`, `MIN_MARGIN`, `ATTACK_MIN_PEAK`,
   `CHANCE_MARGIN`, `WEAK_MATCH`), the second blind set (`prototype/out/analyse/blind2/`), and these, measured or proposed
   on 2026-10-08:
   - **The scoring measure is as good as any tried.** `prototype/analyse_metrics.py` scores the candidates the user
     compared by ear with five measures: the current one (mean band r) agrees with 14 of 21 verdicts, a loudness-weighted
     sum gain also 14, the others 12 or 13. The misses are transient timing (handled by the attack-first/joint switch),
     the bass's low end (the "less low end" flag) and near-ties within 0.003 (the two-option display). Add the new
     verdicts to its `PREFS` before changing the measure.
   - **A finer angle grid gains nothing:** refining the best option to 0.5 degree adds at most 0.0001 r on every pair.
   - **To hear (a proposed third blind set, sums with the sidechain):** the set 1 kick, where ANALYSE now picks LO in 72.5
     Ø at +3.19 ms (its attack peak, 0.27, is under `ATTACK_STRONG`, so the waveform score sets the delay) but the user
     preferred +1.57 ms in the first listening and the attack reading says +1.28 ms (+3.19 was never heard); snare sample
     after its shift against CONSTANT 50 at -5.94 ms; the bass amp's pick against off.
   - **Optional, the user's call:** a note when the best setting swings between parts of the capture (split into thirds).
     ANALYSE keeps giving the averaged best option, which is what the user wants.
3. **Done (2026-10-08): `meter/CorrelationAnalyser` cleaned up** (the user's standing request): the curve, phase angles,
   PHAT lag and attack lag (and their tests: the time and phase views', the kick sample's, `[.userpairs]`,
   `[.attacktrace]`) are gone; it keeps the averages, the six bands, the overall pair, the held preview and the
   sidechain silence timer. ANALYSE's search has its own attack lag (the Python's whole-capture form).

Known behaviour to mention if asked: an option whose phase is "off" switches PHASE off, so the next ANALYSE doesn't search
the phase (the screen's header says what is searched).

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
