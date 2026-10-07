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
5. **ANALYSE (auto-suggest)**: built (2026-10-08), see "ANALYSE: built into the plugin" below; **the cleanup of what it
   doesn't use comes after the refinement** (user, 2026-10-07). History of the groundwork: What is ready for it: the waveform (PHAT) and attack lags and
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
   **Second stem set and blind shootout (user, 2026-10-07/08; `captures/stems/`, `prototype/analyse_stems.py`,
   `analyse_shootout.py --blind`):** the user's by-ear settings mostly scored badly on the metric, but in the blind
   listening (sums with the sidechain) the search's pick was best on 5 of 6: snare sample (CONSTANT 50°, -5.94 ms, only one
   whose transients lined up; the user's own 4 ms pick was worst, so **the delay should reach 6 ms**, user), snare OH
   (CONSTANT 5°, Ø, -1.85 ms), kick sample (attack-first LO in 62.5°, +1.21 ms; the joint pick's transients didn't line up),
   kick OH (joint, LO in 140°, Ø, -4.94 ms, tied with the user's own), guitar (joint CONSTANT 110°, -0.58 ms best; then off, then the
   user's own LO in 102.6° at 0 ms, attack-first worst). The bass was the exception: off was best (the search's -0.42 ms LO in 27.5° had less low end, the user's
   +4.6 ms HI out worst). Built from that, in `analyse.suggest`: (1) the **low-end guard** (a candidate must not lower r
   below 300 Hz: the bass is the only pair it rejects), (2) **attack-first only when the attack peak is >= 0.35**, else the
   joint search (0.39 kick sample, 0.61 snare OH vs 0.30 kick OH, 0.18 guitar: a threshold from four pairs, a first
   guess), (3) up to **two options** to choose from (user's idea: it was a toss-up by the sound wanted). Caveat: all of this
   was tuned on the same six pairs, so it needs new stems to mean anything; and the two options are near neighbours (50° vs
   70°), not real alternatives. The bass now reads "delay only, -0.37 ms", which is untested by ear.
   **Decided (user, 2026-10-08): the delay stays +-4 ms** (micro adjustments; no 6 ms extension). A bigger offset is moved
   by hand: `analyse.suggest_with_shift` says "Transient may be out of range, consider shifting +/- N samples manually if
   needed" (N in samples, as DAWs work in them; positive delays the track) when the attack reading is clear and beyond
   the reach, or the best option sits at the edge, and gives options for the signal after that shift. Two options only
   when the best two are within 0.01 r, one when there is a clear winner. Untested by ear: snare sample (-284 samples,
   then the residual search says HI in 180°, -1.05 ms, not the CONSTANT 50° the ear liked at the whole -5.94 ms) and kick
   OH (within +-4 ms the best is LO in 180°, Ø, -0.70 ms; the ear's -4.94 ms pick is out of reach and no message fires,
   because the attack peak is weak and the in-reach best isn't at the edge). The user will get more stems.
   **2026-10-08, later:** ANALYSE **respects the stage toggles** (user): `suggest(..., delay_on, phase_on)` searches only what
   is on (DELAY off: delay stays 0 and a clear attack offset is only mentioned, "DELAY is off: the transients are N samples
   apart"; PHASE off: phase stays off; polarity is always searched, the Ø button being its own control; confirmed by the user,
   whatever the button's state), so one search answers best delay, best phase, best polarity or any mix. The shift message also fires when the
   waveform score has a clearly better delay up to 10 ms out (`WIDE_REACH_MS`; this catches the kick OH, -237 samples,
   which the attack reading alone missed). The **low-end guard became a flag**: the kick OH's -4.94 ms pick lowers r below
   300 Hz by 0.05 (the user heard that: more attack, less low-end solidity) and a hard reject hid it; options are now kept
   and marked "less low end" (the bass too, -0.02). Second blind set (`analyse_shootout.py --blind2`, the ANALYSE answer
   with the manual shift applied first, plus off and the earlier best): waiting for the user's listening.
   **Listening (user, 2026-10-07; `prototype/out/analyse/renders/`): the sum tells more than the input alone.** Kick: sum 1
   (CONSTANT 130°, Ø, +1.57 ms: the score's own top pick) was best, input 3 (LO in 180°, Ø, +1.02 ms) the best alone. Snare:
   sum 3 (HI in 157.5°, Ø, +0.95 ms) was best, input 1 (LO in 125°, Ø, +0.74 ms) the best alone, so for the snare the
   score's top pick was not what the ear chose from the sum (the three scores are within 0.003). All the winners are Ø
   plus a rotation. Judge candidates by the sum with the sidechain, not the input alone. The user is getting stems that are
   further apart (bigger offsets) to test with next; the confidence thresholds stay as guesses until then.

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
sits on the knob's own grid, and options are merged when they would sound alike (band-mean correlation of one's output
with the other's at least cos 20°, for polarity and phase with delays within 0.5 ms, or for the whole response). On the
user's pairs: kick OH (both scopes with phase) is now one clear suggestion instead of LO in 140° vs 160°; the second
option changed for snare sample (both), snare OH and guitar 2; the first options are unchanged everywhere.

**Next (needs the user):** the verdict in a DAW (the flow, the screen at the real size, applying and A/B by ear, the
undo behaviour of the seven-parameter gesture in their host, the Logic no-sidechain case). Then the refinement, in this
order unless the user says otherwise:
1. **A chance-level guard.** On unrelated material the best gain over doing nothing is +0.044 to +0.049 at 5 s (noise
   against noise, and mismatched pairs of the user's stems), over `MIN_GAIN` 0.03; about +0.02 at 10 s and +0.005 at
   20 s. The minimum is now 10 s (user), which puts chance under the threshold but not by much (noise against noise
   +0.024), so the guard is still worth having.
   Proposed: a null search on the same capture with the sidechain circularly shifted by a second or so (which keeps both
   signals' spectra and destroys their relationship), and require the real gain to beat the null's by a margin (about
   0.2 s more search). Alternative: scale `MIN_GAIN` with 1/sqrt(seconds).
2. The thresholds (`ATTACK_STRONG`, `MIN_GAIN`, `MIN_MARGIN`) with the new stems, and the second blind set
   (`prototype/out/analyse/blind2/`).
3. Then **clean up what ANALYSE doesn't use in `meter/CorrelationAnalyser`** (the user's standing request): the C++ search
   uses none of it (its attack lag is the Python's whole-capture form), so the PHAT lag, the attack features, the curve
   and phase measures and their tests can go unless a view wants them.

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
