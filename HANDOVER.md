# Handover: work that needs no input from the user

Drafted 2026-10-06 at the end of the DSP and hardening pass (branch `dsp-hardening`). Read CLAUDE.md first; plan
sections are IMPLEMENTATION_PLAN.md. Items are in order of value. Each says when it is done and where the user comes
in, if at all.

## Ground rules (from the user)

- `.venv` for all Python; clang-format on touched C++; ask before committing; never commit to master; don't push
  unless asked.
- The only mention of another product anywhere in the repo, comments and commit messages included, is the README's
  "Inspired by phase alignment tools like the Little Labs IBP". Grep what you push.
- Don't change the Hi/Lo mapping (θ → θ₁, θ₂ and the reference frequencies, plan 2.3), the 4097-tap Hilbert or the
  fade lengths without asking.
- The goldens must match within 1e-6. If a Python reference changes, change the C++ identically, regenerate with
  `prototype/golden.py`, and say why.
- Anything the user would hear goes to them, with numbers, before it merges.
- Budgets per stereo frame (`PhaseAlignDspTests "chain cost per stereo frame"`, Release): Hi/Lo < 50 ns, Constant
  < 150 ns.

## State at handover

- `dsp-hardening` is pushed with a clean history: 6 commits on origin/master, no measurement material in any of them.
- CI green on macOS, Linux and Windows (run 37396526948): ctest everywhere, auval and pluginval at strictness 10 on
  macOS.
- Local `master` still has its 14 old commits, which contain the removed material. `dsp-hardening-wip` is a local-only
  snapshot of the same tree. Neither should ever be pushed.

## 1. De-cramp Hi/Lo, so a setting sounds the same at every rate

**Problem** (`prototype/hf_check.py` → `prototype/out/hf/report.md`, plan 2.3):
- Each section is a bilinear first-order all-pass, exact at its reference frequency, so it is squeezed towards 180° at
  Nyquist.
- Against analog sections with the same lag at the reference, the worst setting (just past 0° or 90°) is off by 11° at
  5–10 kHz and 80° at 16–20 kHz at 44.1 kHz; 58° at 48 kHz, 9° at 96 kHz, 2° at 192 kHz. A typical setting is off by
  2° (Hi) / 8° (Lo) at 16–20 kHz at 44.1 kHz.
- The same knob setting's top octave differs between 44.1 and 192 kHz sessions by up to 78°.
- The level is exact everywhere (all-pass). The user said go ahead (2026-10-06).

**Target:** each section's lag within about 3° of the analog section to 16 kHz and about 10° to 20 kHz at 44.1 and
48 kHz, for every θ.

**Must keep (don't regress):**
- Exact lag at the reference frequency (the readout, ≤ 0.001° today).
- Lag non-decreasing in θ at every frequency.
- A continuous 90° handover.
- 0° = identity.
- Flat level.
- No bursts on coefficient moves (the PhaseTests broadband test: direct form I solved this for first-order sections, so
  keep the state to past samples or use a normalised lattice).
- Per-sample coefficient interpolation that stays stable. For a second-order all-pass the stable region in (a₁, a₂) is
  a triangle, which is convex, so linear interpolation between two stable sets stays stable.
- Zero latency; within budget.

**Approach:** prototype in Python first; the C++ comes after the numbers are good.

1. **Second-order all-pass per section, closed form per 32-sample cell.**
   - A(z) = (a₂ + a₁z⁻¹ + z⁻²)/(1 + a₁z⁻¹ + a₂z⁻²). Its lag at ω is linear in (a₁, a₂) once written as
     Im(e^(−jβ)·D(e^(jω))) = 0 with β = φ/2 − ω (verify the sign convention numerically).
   - So matching the analog lag exactly at the reference frequency and at one high anchor (try 15–19 kHz, or choose
     it to minimise the band error) is a 2×2 solve.
   - Check stability, monotonicity and the error over the whole θ range at 44.1, 48, 88.2, 96 and 192 kHz.
2. **Fallback:** minimax-fitted second- or third-order sections tabulated over θ per rate class, interpolated in a
   stable parametrisation (pole radius and angle).
3. **The hard part is near identity.** As θᵢ → 0 the analog section's phase vanishes everywhere, but a digital
   second-order all-pass still has to reach 360° at Nyquist, so its poles go towards the unit circle near Nyquist
   (sharp, ultrasonic). Measure what that does (ringing, numerics, the burst test). Options:
   - accept it (ultrasonic);
   - blend from the first-order section over the lowest few degrees;
   - use the first-order section while its corner is above about 20 kHz, where it isn't cramped in the audible band.
   - Don't trade away "0° = identity" without asking.

**Steps:**
1. `prototype/hilo.py`: a de-cramped variant behind a flag.
2. `hf_check.py`: a de-cramped column, plus the monotonicity and readout checks.
3. Port to `AllpassCascade` (both modes); regenerate the goldens; keep or extend PhaseTests: flat level, readout,
   monotonic, all-pass during glides, the broadband no-burst test.
4. Re-render P3 (`PhaseAlignDspTests "[p3]"`, then `prototype/p3_report.py`); benchmark.
5. Render A/B material for the user: the same settings at 44.1 and 96 kHz, old vs de-cramped, on the P3 sources and
   any real stems.

**Done when:**
- The target is met at every rate with every "must keep" holding.
- Goldens and tests pass; within budget; plan 2.3 and the 2.6 table updated.
- **Then the user listens**, and only after their OK does it merge.

## 2. Constant on Windows and Linux: PFFFT

CI's numbers (plan 2.6, run 37396526948) put Constant over budget:
- **Linux:** 119–160 ns, and 160 at 192 kHz, over 150.
- **Windows:** 232–327 ns at every rate, and one 64-sample callback at 192 kHz took 1088 µs of 333.

These are shared VMs, so pessimistic, but the gap is too big to ignore. Hi/Lo is fine everywhere.

1. **Ask the user** before fetching PFFFT's source (BSD-style licence): it's a download. Put it under `libs/` and note
   the licence where the other third-party notices go.
2. Add it as `RealFft`'s engine on non-Apple platforms, keeping the packed split format. PFFFT's ordered real
   transform is interleaved, so repack, or use its own `zconvolve_accumulate` in its internal order if that is
   faster.
3. Re-tune `ConstantRotator::blockSizesFor` for the new engine from CI's convolver benchmark. On Windows today the
   three-level 128/1024/4096 layout beats uniform 128 at 96 kHz (279 vs 525 ns).
4. Check MSVC vectorises the register-blocked loops (head, multiply-add, fractional kernel). If not, a small SSE/NEON
   helper.

**Done when:** Constant is under 150 ns and the worst 192 kHz callback is under about half its length on both CI
runners. The goldens and every test still pass with each engine, and plan 2.6 has the numbers.

## 3. CI hygiene

- GitHub moves `ubuntu-latest` to Ubuntu 26 from 2026-10-19 (an annotation on the runs). Pin `ubuntu-24.04`, or run
  once on the new image and fix what breaks (fonts for the editor tests under xvfb, package names in the apt step).
- Optional: pluginval at strictness 10 on the VST3 on Linux and Windows too (`pluginval_Linux.zip`,
  `pluginval_Windows.zip`). Linux needs xvfb.
- **Done when:** CI is green on all three after the change.

## 4. M4's Instruments check

Confirm, with a profiler rather than counters, that nothing of the meter runs when the meter is off, the editor is
closed or the window is hidden: no `CorrelationAnalyser` and no `MeterScreen` paint/timer samples.

- Use `xctrace record --template 'Time Profiler' --launch -- <binary> "[desktop]"`.
- A small hidden test may be easier: open the editor, meter off, process audio for 10 s; then closed; then hidden.
  Pass environment variables with `--env`.
- **Done when:** the M4 box in plan 5 is ticked, with the evidence noted there.

## 5. Release checklist items that need no credentials

- **Universal binary:** build `-DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"` locally, run the DSP tests natively and under
  Rosetta (`arch -x86_64 <binary>`; Rosetta is installed), and `lipo -info` the AU and VST3.
- **OFL font notices:** check each installer (`installer/macos`, `installer/windows`, `installer/linux`) ships
  `assets/fonts/licenses/`, and add them where missing.
- **Installer readme:** the README's quick-start, packaged into each installer. The host routing steps in it are still
  to be checked by the user.
- **`release.yml` review:** read it against the current tree (the tests are off there). Don't run it: it needs signing
  secrets and publishes a draft GitHub release, so it's the user's call.

## 6. Housekeeping (ask before each)

- Merge `dsp-hardening` into master: the user's decision.
- Afterwards, point local `master` at the clean history (its old commits hold the removed material) and delete
  `dsp-hardening-wip`.
- Remove the stale worktree `.claude/worktrees/dsp-negative-delay`: detached at 6452ea9, it still has the old files.
- AU in pluginval locally needs the AU installed in `/Library/Audio/Plug-Ins/Components`, which is the user's call.

## Not in this handover: needs the user

- **Listening:** M2 in a DAW, P2 (is true rotation useful on real material), P3 fade tuning (the renders and report
  are ready in `prototype/out/p3/`), and the de-cramp A/B.
- **Real multi-mic stems.**
- **Design decisions:** what RANGE does in the DSP; extending Hi/Lo below their lowest corners; the meter views; the
  dimming alpha; the minimum editor size; ANALYSE (dead last).
- **Host checks:** latency re-alignment per host, sidechain routing steps, no-sidechain detection in Logic, Live and
  Reaper.
- **Release:** a trademark search for the name; the JUCE licence tier; notarisation credentials.
