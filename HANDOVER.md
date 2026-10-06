# Handover: where things stand, and what's next

Updated 2026-10-06 at the end of the session that worked through the previous handover (its item-by-item briefs are in
git history, before f1b4c30). Read CLAUDE.md first; plan sections are IMPLEMENTATION_PLAN.md.

## Ground rules (from the user)

- `.venv` for all Python; clang-format on touched C++; ask before committing; work on a branch and merge to master only
  when the user says; don't push unless asked.
- The only mention of another product anywhere in the repo, comments and commit messages included, is the README's
  "Inspired by phase alignment tools like the Little Labs IBP". Grep what you push.
- Don't change the Hi/Lo mapping (θ → θ₁, θ₂ and the reference frequencies, plan 2.3), the 4097-tap Hilbert or the
  fade lengths without asking.
- The goldens must match within 1e-6. If a Python reference changes, change the C++ identically, regenerate with
  `prototype/golden.py`, and say why.
- Anything the user would hear goes to them, with numbers, before it merges.
- Keep CPU and latency as low as possible, but working matters more: do the efficient thing, not the easy expensive one
  (user, 2026-10-06).
- Budgets per stereo frame (`PhaseAlignDspTests "chain cost per stereo frame"`, Release): Hi/Lo < 50 ns, Constant
  < 150 ns. **Hi/Lo at 44.1 kHz going over 50 ns on the Windows/Linux CI runners is accepted** (user, 2026-10-06).
- macOS releases are separate arm64 and Intel builds, never universal (R14); local builds are arm64.

## State

- **master is 4 commits ahead of origin, not pushed** (d327bd5 de-cramping, 2b57341 PFFFT, 2db5ff6 docs, f1b4c30
  release prep), plus this handover's commit if made. No other branches or worktrees. CI last ran green on dad5331;
  nothing since has run in CI.
- Licence: **GNU AGPLv3** (`LICENSE`); JUCE under its AGPLv3 option; notices in `THIRD_PARTY_NOTICES.md`.

## Done in this session (details in the plan)

1. **Hi/Lo de-cramped (R13, plan 2.3, 2.6 J). Approved by ear (user, 2026-10-06).** Zero-latency de-cramping is
   impossible (Foster's reactance theorem), so Hi/Lo runs oversampled between linear-phase halfbands: latency 32
   samples at 44.1 kHz, 18 at 48, 5 at 96, 0 at 192, reported whenever Constant isn't selected. Within 2.5° of analog
   to 20 kHz at every rate. Cost on the M1: 18–23 ns (was 4).
2. **PFFFT** behind `RealFft` where vDSP isn't (plan 2.6 K): on the M1 it matches vDSP. Convolver layouts no longer
   depend on the engine.
3. **CI:** Ubuntu pinned to 24.04; pluginval on Windows/Linux as information only.
4. **M4's Instruments check** done (`tools/meter_profile.py`, plan M4).
5. **Release prep (plan M5):** both macOS architectures tested (arm64 natively, x86_64 under Rosetta); every installer
   and release zip ships `installer/stage_docs.sh`'s readme and licences; `release.yml` reviewed; tooltips audited, with
   their latencies computed from the DSP and pinned by a test.
6. Decided (user, 2026-10-06): no trademark search (open source); macOS notarisation is covered (the Apple secrets
   are set in the repo; `release.yml` signs and notarises both macOS builds and their .pkg); Windows and Linux ship
   unsigned (no credentials, none needed).

## Next: needs no input from the user

1. **Windows Constant is over budget (CI run 37420074764; user, 2026-10-06: do this very last).** CI's numbers are in
   plan 2.6 L: Windows Constant 155-214 ns against the 150 budget (macOS and Linux are within it); Windows Hi/Lo is
   over 50 ns at 44.1 and 48 kHz (44.1 kHz accepted; the 48 kHz fractional-delay 91.5 ns looks like noise but isn't
   covered by the acceptance). The Windows convolver costs 3-4x Linux's against 2-3x for the `Simd.h` code, which fits
   MSVC not vectorising its float reductions. Move the convolver's head, multiply-add and fractional-kernel loops onto
   `src/dsp/Simd.h` and measure on a Windows runner; fallback: 128/512 blocks below 80 kHz behind `_M_X64` (about 52 ns
   off 193). `blockSizesFor` was left alone (Linux within noise; the M1 prefers the current layout). pluginval passes
   on Windows and Linux at strictness 10, and `continue-on-error` is gone from that CI step.
2. **Optional CPU:** the chain adds about 6 ns around the Hi/Lo stage that it didn't before (the delayed dry path, and
   more: plan 2.6 J), and Hi/Lo kept warm in Constant costs about 13 ns there. Neither is over budget.

## Needs the user

- **A push** (and so CI on everything since dad5331).
- **`release.yml`:** it builds with the tests off, so a release could be cut from a commit whose CI failed. Run it only
  on a green commit, or add a CI-status check to it (plan M5).
- **The first release run**, which also checks the Windows and Linux installers' new docs step.
- **Host checks:** latency re-alignment per host (now also Hi/Lo's small latency), the quick start's sidechain routing
  steps (the installer readme keeps the README's "Draft" note until they're checked), no-sidechain detection in Logic,
  Live and Reaper.
- **Listening:** M2 in a DAW, P2 (is true rotation useful on real material), P3 fade tuning (`prototype/out/p3/`).
- **Real multi-mic stems.**
- **The RANGE redesign (branch `hilo-range-modes`):** listen to `prototype/out/range/` (current vs new, 180 range
  only; the 90 range is identical), then merge. Pending after that: the README/plan already describe it.
- **Design decisions:** the meter views (to discuss, "last"); ANALYSE (dead last). Decided 2026-10-06: dimming alpha
  0.4, minimum editor size 60% (586 px), RANGE/Hi-Lo as above, no extension below the lowest corners.
- AU in pluginval locally needs the AU installed in `/Library/Audio/Plug-Ins/Components`.
