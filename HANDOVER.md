# Handover: where things stand, and what's next

Updated 2026-10-07, at the end of the session that built the RANGE redesign and fixed Windows. Read CLAUDE.md first; plan
sections are IMPLEMENTATION_PLAN.md. Nothing here needs code first: what's left is decisions and listening.

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
- Anything the user would hear goes to them, with numbers, before it merges.
- Keep CPU and latency as low as possible, but working matters more (user, 2026-10-06).
- Budgets per stereo frame (`PhaseAlignDspTests "chain cost per stereo frame"`, Release): Hi/Lo < 50 ns, Constant
  < 150 ns. Hi/Lo at 44.1 kHz over 50 ns on the Windows/Linux CI runners is accepted (user, 2026-10-06).
- macOS releases are separate arm64 and Intel builds, never universal (R14); local builds are arm64.
- Naming: the phase modes are LOW, HIGH and CONSTANT; the button is RANGE **out** (one section) and **in** (two
  sections), never "90/180" (R16). The scale label and readout still show the number the knob reads up to.

## State

- **master is `f9d7e7d`** (plus this handover's commit if made), one commit ahead of GitHub unless pushed. The last
  pushed commit, `7a9a676`, is green in CI on all three platforms (`gh run list --branch master`). Master is the only branch, locally and on GitHub. No worktrees.
- Licence: GNU AGPLv3 (`LICENSE`); JUCE under its AGPLv3 option; notices in `THIRD_PARTY_NOTICES.md`.
- Build directories (gitignored): `build-ui` (tests: `cmake -B build-ui -DPA_STANDALONE=ON`, then `cmake --build build-ui
  --target PhaseAlignTests PhaseAlignDspTests` and `ctest --test-dir build-ui`), `build-release` (benchmarks),
  `build-simd` (the convolver's `Simd.h` path forced on with `-DPA_CONVOLVER_SIMD=1`, to test it off Windows).

## Done this session (details in the plan)

1. **RANGE redesign (plan 2.3, R15, R16).** Each (mode, range) is its own shape built from the reference unit's measured
   geometries (nothing invented): LOW out 75.1 Hz; LOW in two stacked at 150.1 Hz; HIGH out 150.1 Hz; HIGH in 75.1 Hz
   and 1502 Hz. RANGE out is unchanged from before; only RANGE in changed. Fitted against the measurements each
   shape is within about 2° rms (`prototype/range_ab.py` renders the A/B files).
2. **Names:** LOW centres the turn lower down, HIGH higher up, in both ranges (the frequency where half the full turn has
   happened at full knob: 75 / 150 Hz for LOW out / in, 150 / about 336 Hz for HIGH). With RANGE in HIGH is also more
   spread out. NARROW / WIDE was rejected: with RANGE out both modes have the same width. README "Where the turn sits".
3. **The number on the panel is the real phase.** At full knob the scale label, the readout and the measured phase agree
   in every state, tested end to end through the processor (`tests/EditorTests.cpp`, "the phase readout is the phase the
   plugin applies"). The one exception is HIGH with RANGE in, which shows 0 to 90 with an asterisk: its sections are far
   apart, so there is no single frequency at which the total is 180 (the real phase at 75 Hz reaches 95.7; the total
   reaches 180 at 336 Hz).
4. **Glide bug found by CI and fixed (plan 2.3 "Switching shape").** The first glide blended two shapes, so a change to a
   third shape mid-glide jumped (a step up to 26 times a sine's own). The glide state is now a weight on each of the four
   shapes. The step test runs several seeds because the random sequences differ between standard libraries.
5. **Windows Constant 193 ns to 53 ns** (MSVC wasn't vectorising the convolver; under `_MSC_VER` only it now uses
   `Simd.h`; plan 2.6 L). pluginval passes on all three platforms and gates the Windows and Linux CI jobs.
6. **UI and docs:** minimum editor size 60% (586 px); the README is a manual with screenshots (`docs/images/`) and a
   phase modes section with a figure (`prototype/modes_figure.py`); dimming alpha 0.4 approved.
7. **Frequency-response check of the A/B sums** (`prototype/range_fr_check.py`, plot in `prototype/out/range/fr_check.png`):
   measured from the WAVs it matches the prediction to 0.01 dB. An all-pass is flat alone; the dips are in the sum with
   the dry track (LOW in: 149 Hz at full knob; HIGH in: 337 Hz and broader).

## Next: needs the user (decisions and listening)

1. **The meter views (the user wants a discussion first, to understand the options and their ramifications).** What is
   built (plan 3 and `prototype/out/p4/report.md`): two views chosen by clicking labels under the screen, **FREQUENCY**
   (default; r(f) over 1/6 octave at 256 log-spaced points, processed bright with a faint fill, unprocessed dim) and
   **TIME OFFSET** (the PHAT lag function, -5 to +5 ms, the delay knob's reach shaded, a coarse search to 40 ms behind
   "TRANSIENTS OUT OF DELAY RANGE"), plus an overall bar on the right; averaging time constant max(0.3 s, 8 cycles) per
   bin; 30 Hz screen; "NO SIDECHAIN SIGNAL" after 1 s of silence. The panel art's 16 band bars were only an example
   (comb filtering of spaced mics averages away above about 1 kHz). Things to settle in the discussion: which views to
   keep and which is the default; whether to add a phase-difference view (R6 left room for it); the averaging speed
   against jitter; the overall bar's meaning; how it should feed the auto-suggest work later. The user's verdict in a
   DAW is still outstanding.
2. **Listening.** (a) The 180-range A/B files in `prototype/out/range/` (current vs new; see `report.md` and
   `fr_check.png`; only the RANGE in files differ; nothing sounding wrong means done). (b) M2 in a DAW. (c) P2: is true
   rotation useful on real material. (d) P3 fade tuning (`prototype/out/p3/`). Real multi-mic stems are still wanted.
3. **HIGH with RANGE in: leave the asterisk, or make the number exact?** Today it shows the first section's angle (0 to
   90, within 6 degrees of the real phase at 75 Hz). It could instead be made exact by changing how the angle is split
   between the two sections (a small change to the mapping, goldens and tests). Recommendation: leave it.
4. **De-cramping of LOW with RANGE in:** its two stacked sections double the error against an analog all-pass (up to
   4.6 degrees at 44.1 kHz and 3.9 at 48, against 0.8 to 2.8 for the others, all at 20 kHz at the worst knob position).
   Doubling the oversampling quarters every error but adds a halfband stage (CPU and a few samples of latency, not
   measured). Recommendation: leave it.
5. **Host checks:** latency re-alignment per host (Hi/Lo now reports a small latency too), the quick start's sidechain
   routing steps (the README keeps its "Draft" note, which the installer's readme copies, until they are checked), and
   no-sidechain detection in Logic, Live and Reaper.
6. **`release.yml`** builds with the tests off, so a release could be cut from a commit whose CI failed: run it only on a
   green commit, or add a CI-status check to it (plan M5). The first release run also checks the Windows and Linux
   installers' docs step.
7. **ANALYSE (auto-suggest)** is dead last, after the user's stem checks. The button is present but does nothing.

## Next: needs no input

- Watch CI after any push (`gh run list --branch master`; a watcher in the background is fine) and report.
- Optional CPU: the chain adds about 6 ns around the Hi/Lo stage that it didn't before, and Hi/Lo kept warm in Constant
  costs about 13 ns there (plan 2.6 J). Neither is over budget.

## Gotchas that cost time

- `HiLoStage::process` takes at most `HiLoStage::maxBlock` (32) samples per call; more aborts without a message.
- CI's random sequences differ from the Mac's (libstdc++ and MSVC against libc++), so a test that passes on one seed can
  fail elsewhere: run several seeds, and when CI fails on x86 only, look at the seed first.
- The reference unit's measurements live in `captures/` (gitignored; never name the unit in the repo). The analysis
  scripts that read them were one-offs; the findings are in plan 2.3.
- After rewriting history, `git filter-branch` leaves a backup ref under `refs/original`: delete it once the push is done.
