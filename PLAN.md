# Phase Align: Project Plan

**Status:** v0.7 and record and compare (section 3.5) are built; this is the original design spec, and how it was built,
with every deviation, is in IMPLEMENTATION_PLAN.md (where things stand: HANDOVER.md). **Milestones:** v0.7 is the core feature set (sections 3.1 to 3.4). Record and compare (section 3.5) is planned as the next milestone, and v0.7 is structured to support it (section 4.9). **Framework:** JUCE (C++), AU and VST3, macOS first **Goal:** a low-CPU phase alignment utility. This is a utility, not an analogue emulation.

---

## 1. Goals and non-goals

**Goals**

- Continuously variable phase adjustment to fix partial phase cancellation between two combined sources.
- Add a sample-accurate delay with convenient units.
- Add a correlation meter that compares the processed signal with a sidechain signal, so the user can see whether alignment is improving.
- Keep CPU and latency as low as possible. Bypassed stages should cost almost nothing.
- Offer a record-and-compare feature that suggests settings. Suggestions only; the user decides by ear.

**Non-goals**

- Matching analogue character, ICs or colouration.
- Oversampling (see the decisions log, section 5).

---

## 2. Background: all-pass phase adjustment

- An all-pass filter changes phase but not magnitude, so it can turn one source's phase against another's without
  changing its tone on its own.
- A first-order all-pass runs from 0° at DC to 180° at Nyquist (digital) and reaches 90° at its corner frequency, so
  its angle is frequency dependent. Two in series reach up to 360°.
- Where the sections' corners sit sets which part of the spectrum turns most: the Hi and Lo modes (4.3).
- A pure delay shifts every frequency by the same time, which is what aligns two mics at different distances; a phase
  rotation fixes what a delay can't (a DI against an amp, a mic's own phase response).
- Phase processing only matters when two signals are combined.

---

## 3. Feature specification (v1)

### 3.1 Delay section

| Item | Spec |
| --- | --- |
| Delay knob | 0 to 4 ms (decided; longer offsets are pushed manually in the DAW). The effective delay is always rounded to whole samples. |
| Unit selector (3-way) | ms / samples / cm |
| Distance conversion | 343 m/s (tooltip states this); 4 ms is about 137 cm |
| Bypass | Independent; saves CPU when active |
| Value readout | Shows value in the selected unit; double-click to type a value |
| Fine adjust | Shift-drag, mouse wheel, arrow keys nudge by one sample |

The parameter is stored in **ms**, so sessions are portable across sample rates. The display converts to samples or cm. Typed values convert back to ms. The delay actually applied is rounded to whole samples at the current sample rate, and the readout should show that effective value (proposed). Note that JUCE sliders reset on double-click by default, so this must be overridden to give edit-on-double-click.

### 3.2 Phase section

| Item | Spec |
| --- | --- |
| Phase knob | 0 to 90° or 0 to 180°, set by RANGE (HIGH with RANGE in shows 0 to 90° with an asterisk, see 2.3) |
| Mode switch (3-way) | Hi / Lo / Constant |
| Polarity flip button | 180° inversion; with the knob this covers the full 360° |
| Bypass | Independent |
| Value readout | Degrees; double-click to edit |

The RANGE button is the hardware's 90°/180° switch: 90 runs one all-pass section over the knob, 180 two (section 4.3).

### 3.3 Correlation meter

- Compares the processed signal with the sidechain input.
- Also shows the unprocessed signal against the sidechain, so "better or worse" is the visible difference.
- Idle state when no sidechain is connected.
- Meter on/off button. When off (or when the editor is closed), the meter analysis and drawing are skipped entirely, to save CPU. Capture for auto-suggest (section 3.5) is independent of the meter.
- Design detail in section 6.

### 3.4 General

- Stereo linked (same processing on both channels).
- Mono and stereo layouts, with a sidechain bus.
- Resizable UI via a corner drag handle; the last size is remembered (section 7).
- Tooltips on every control.
- Zero reported latency.

### 3.5 Record and compare (auto-suggest)

**As built (2026-10-08): IMPLEMENTATION_PLAN R26.** The main differences from what follows: the search scores every
candidate in closed form from one cross-spectrum of the capture (no GCC-PHAT, no offline renders), takes the delay from the
attacks when they are clear, searches only the stages switched on, and advises a manual shift in samples only for a significant gain beyond ±4 ms (the smallest that gets most of it, with a best effort for the track as it stands alongside);
the capture ends when the transport stops (10 s minimum, 30 s recommended); clicking an option applies it, with ORIGINAL
to go back.

Planned as the milestone after v0.7. The output is a **suggestion only**: the setting with the highest measured correlation is not necessarily the one that sounds best, so the user decides by ear.

**Interaction** (a single ANALYSE button plus the screen; details are proposed and to be finalised in the UI session)

1. Press ANALYSE to start capturing while a representative section plays. The plugin records the input and sidechain into a buffer (about 10 s, mono sum for analysis). No CPU cost when not capturing.
2. Press again, or let the capture time out, to analyse. This runs on a background thread, never the audio thread. The screen shows the capturing and processing states.
3. The screen lists the candidates. Choosing one applies it (as a single host-undoable change); the user can audition others or discard.

**Search scope**

- Always a full search: delay, mode (Hi/Lo/Constant), angle and flip. There is no scope button; the screen presents the best candidates across modes and the user picks.
- Whether the delay search should be optional is still an open question (section 9). It is currently assumed to be included.

**Method**

- Delay: cross-correlation (GCC-PHAT) at whole-sample lags within the plugin's 0 to 4 ms range. If the strongest peak lies outside the range, report that rather than silently clamping.
- Phase: run the plugin's own DSP offline over a grid of angles (and flip) for each allowed mode, and score each against the sidechain by correlation, band-weighted so low-frequency energy doesn't dominate. Constant mode has an approximate closed form: `θ ≈ atan2(corr(sidechain, Q), corr(sidechain, I))`.
- Output: the top few candidates with a confidence indicator, not a single answer. The meter shows before and after.

**Caveats**

- Works best for one source captured by two mics (kick in/out, DI vs amp, snare top/bottom). A sidechain containing other instruments pollutes the result.
- Delay and phase partly substitute for each other, and periodic material gives several near-equal peaks. Prefer the smallest plausible delay and show confidence.
- Correlation is a proxy for "sounds good", not the same thing.

UI states for this feature are described in section 7.

---

## 4. DSP design

### 4.1 Signal path

Polarity flip, phase section and delay are all linear and time-invariant, so their order does not change the result. Suggested order: flip, phase, delay.

### 4.2 Delay

- Delay line sized for the maximum delay at the highest supported sample rate (4 ms at 192 kHz is 768 samples).
- Whole-sample delay only: the target is `round(ms × sampleRate / 1000)`. No interpolation, so the shift is exact, with no filtering, and it is cheaper than a fractional delay. Sub-sample resolution is deliberately given up (decision 12).
- A step change in a whole-sample delay can click, so crossfade between the old and new delay taps over a few milliseconds when the value changes (proposed approach; to confirm in prototyping).
- Not reported to the host as latency.

### 4.3 Phase section: Hi and Lo modes

- One or two first-order all-pass sections with a variable corner frequency (TPT or equivalent, chosen for stability under modulation).
- The RANGE button picks 90 or 180: at 90 one section runs over the knob's whole travel; at 180 two sections share it. The
  knob keeps its position when RANGE is pressed. Each mode and range is its own shape, built from the reference unit's
  measured geometries (IMPLEMENTATION_PLAN 2.3, R15).
- A change of range or mode must be seamless: the sections' corners glide from the old shape to the new one. Both
  sections start at their "no effect" corner setting at 0°.
- Hi and Lo differ in where the sections sit: the corner frequencies and the knob-to-angle mapping are in IMPLEMENTATION_PLAN 2.3.
- Corner-frequency changes are smoothed.
- Prewarp the coefficients to reduce cramping near Nyquist (section 4.5).

### 4.4 Phase section: Constant mode

- A Hilbert-style pair: two all-pass IIR networks whose outputs differ by about 90° over a wide band, giving I and Q.
- Rotation by θ: `y = cos(θ)·I + sin(θ)·Q`. The knob only changes the mix, so there is no coefficient modulation, no zipper noise and no stability concern.
- Reference design: Olli Niemitalo's 8-section pair, about ±0.7° of 90° over a wide band, one multiply per section, effectively no latency.
- Caveats: accuracy tapers at both band edges. There is large low-frequency group delay. The usable band edges scale with sample rate, so coefficient sets should be designed per sample rate.
- Use double precision for filter states (poles are very close to 1).

### 4.5 Accuracy target and cramping

- Magnitude is flat by construction (all-pass).
- The real target is phase accuracy. A digital all-pass reaches 180° at Nyquist, where the analogue prototype only approaches it.
- Prewarping pins the match at one frequency, so some deviation remains near Nyquist at 44.1 and 48 kHz. At 88.2 kHz and above this largely disappears.
- Define a tolerance after measuring (provisional idea: within a few degrees up to roughly 18 to 20 kHz at 44.1 kHz). Measured 2026-10-06 against analog sections: `prototype/out/hf/report.md`, IMPLEMENTATION_PLAN 2.3.

### 4.6 Bypass behaviour

- Crossfade over about 5 to 10 ms on toggle, then skip the processing entirely once the fade completes.
- Clear or reset filter and delay state when a stage is re-enabled, to avoid clicks from stale audio.
- Use `juce::ScopedNoDenormals`.

### 4.7 Mode switching

- Hi, Lo and Constant use different structures, so switching needs a crossfade of about 10 to 20 ms. Both run briefly during the fade. Only the active mode runs otherwise.

### 4.8 Latency and CPU

- Reported latency: zero. The delay is an intentional effect, not compensation.
- CPU: a few multiply-adds per sample per channel in the active phase mode, plus the delay line.
- No oversampling.

### 4.9 Offline-callable DSP (for auto-suggest)

Structure the processing chain (flip, phase, delay) as code that can run on a plain buffer with its own state objects, independent of the audio callback. The analyser in section 3.5 then reuses the exact same DSP instead of a second implementation. This costs nothing in v0.7.

---

## 5. Decisions log

1. Constant mode plus Hi/Lo modes, behind one 3-way switch.
2. 90°/180° switch removed; the knob position handles it (to be verified against measurements).
3. Delay units: ms, samples, cm (not metres).
4. Oversampling dropped. Reason: nothing in the chain is nonlinear, IIR oversampling filters add their own phase shift, and FIR versions add latency. Cramping is handled by prewarping and per-rate coefficients.
5. Tooltips on every control.
6. Resizable UI via a corner drag handle, with the last size remembered (stored in plugin state).
7. Permanent, versioned parameter IDs from the first release.
8. Delay range stays at 4 ms. An extended range (about 20 m) was considered and rejected; longer offsets are pushed manually in the DAW.
9. Record and compare is in the plan as suggestions only. Placement as the milestone after v0.7 is a proposal.
10. Auto-suggest uses a single ANALYSE button and always searches all modes; the screen presents the candidates and the user picks. The separate full auto / current mode choice was dropped.
11. UI direction: hardware-style look with a "screen" for the meters (section 7).
12. The delay is always rounded to whole samples. No snap control and no fractional interpolation. (This is my reading of "let's round it"; revisit if sub-sample alignment proves necessary at high frequencies.)
13. FLIP is a dedicated button.
14. Meter off stops the meter analysis and drawing, to save CPU.

---

## 6. Correlation meter design (proposed, not final)

- Pearson correlation `r = Σxy / sqrt(Σx² · Σy²)` with exponential averaging (roughly 0.3 to 1 s). Guard against divide-by-zero on silence.
- Broadband correlation hides frequency-dependent problems, so the proposed display is about 6 to 8 band-split correlation bars plus a smoothed overall reading, for both processed and unprocessed signals against the sidechain.
- Running sums are cheap enough for the audio thread. Anything heavier (for example an FFT phase curve) belongs on the GUI side, fed by a lock-free FIFO (`juce::AbstractFifo`).
- Treat absolute values with care. Two different mics on one source never reach +1, so the difference between processed and unprocessed matters more than the number.
- Sidechain handling: unconnected shows an idle state; stereo sidechain is summed to mono for analysis.
- Band count: the plan proposed 6 to 8 bands, while the concept image shows 14. About 10 octave bands with fixed centres is a proposed compromise (low bands settle slowly because their cycles are long).
- The exact visual treatment (bars, history trace, phase curve) is still to be decided once the prototype exists.

---

## 7. UI plan

- **Row 1 (controls):** delay knob, then its unit selector and bypass; then phase knob, then its Hi/Lo/Constant switch, flip button and bypass.
- **Row 2:** meters on the screen, with the ANALYSE button beside it and a meter on/off button.
- **UI size:** a corner drag handle; the last size is remembered.
- Readouts on each knob, editable by double-click.
- Resizing: lock the aspect ratio, lay out relative to size, support HiDPI, save the size in plugin state.
- Tooltips: one `TooltipWindow`, with `setTooltip()` on each control stating what it does and its units.

**Intended presentation** (final look to be settled in a separate prototyping session using image-generated concepts)

- Physical-hardware look, with a "screen" area holding the meters.
- The suggestion feature is driven by a single console-style square illuminated ANALYSE button with text overlaid on the lit face. Capture, processing and the choice of candidates are handled on the screen.
- The screen shows the feature's state: idle; armed and capturing (level and elapsed time); processing (progress); results (candidates with confidence); and a no-sidechain state.

**Notes from reviewing the first concept image** (dark panel, two LED-ring knobs, green LED readouts, toggle switches, meter screen, two lit square buttons; carry these into the UI session)

- The layout fits the plan: control row on top, screen below, buttons at the right.
- Missing from the concept: FLIP and the meter on/off button. The concept's second lit button (labelled SCOPE) is no longer needed, since the scope choice was dropped; the blue button could become meter on/off.
- The delay readout needs a unit suffix (ms / smp / cm) that follows the unit switch.
- The concept puts a phase symbol at the end of the phase knob's scale, which could be mistaken for the FLIP button. Use a different icon there and reserve the symbol for FLIP.
- Bypass LEDs: lit green reads as "active" while the button says BYPASS. Either light them when bypassed, or label them as IN.
- The screen needs a second marker per bar (for example an outline or tick) for the unprocessed signal, and a processed/unprocessed pair on the overall bar.
- Bar centres must match the axis labels; the concept's labels skip from 3.2k to 8k.
- Build in JUCE by drawing meters, LED rings, readouts and text in code, with raster images only for panel texture, knob caps, toggles and button faces, so the UI scales cleanly. Redraw only the meter area, at about 30 fps.
- Choose an original name and tagline if released, and check them against existing products and trademarks.

---

## 8. Prototyping and measurement plan

1. **Python prototype first.** Implement all three phase modes and the delay offline before any C++.
2. **Hi/Lo design:** corner-frequency ranges for Hi and Lo, the knob-to-angle mapping, and behaviour near Nyquist
   (IMPLEMENTATION_PLAN 2.3).
3. **Measure magnitude** too, to confirm flatness.
4. **Validate the range and mode changes** (glides between shapes) for continuity.
5. **Design Constant-mode coefficients** for 44.1, 48, 88.2, 96 and 192 kHz and plot the achieved 90° accuracy.
6. Prototype the auto-suggest search in Python: GCC-PHAT delay estimate, then grid search over angle and flip in each mode, using recorded or synthetic signal pairs.
7. Port the working structures to JUCE.

---

## 9. Open questions

1. **90° switch.** *Resolved (user, 2026-10-06, IMPLEMENTATION_PLAN R15):* the RANGE button selects one section (90) or
   two (180) over the knob's whole travel; each mode and range is its own shape from the reference unit's measured
   geometries, named as its manual describes them. Extending Hi/Lo below their lowest corners is closed (not done).
2. **Decoupling angle and width (unverified idea).** Two cascaded first-order all-passes might allow independent angle
   and width. Parked; not needed for v0.7 (Hi and Lo are two fixed spreads: sections coincident vs 20× apart).
3. **Meter visual style.** Bars, history, phase curve, or a combination.
4. **Auto-suggest delay search.** Currently assumed to be always included. Confirm, or make it optional.
5. **Auto-suggest milestone.** Proposed as the step after v0.7; confirm.
6. **Hardware-style UI.** Details to be settled in the prototyping session.
7. **Delay rounding.** The delay is rounded to whole samples (decision 12). Revisit if sub-sample alignment proves necessary.

---

## 10. Testing and release checklist

- Impulse test for delay accuracy.
- Sine sweeps for phase angle versus frequency in each mode, at 44.1, 48, 96 and 192 kHz.
- Click tests for mode switching, bypass toggling and delay changes.
- Auto-suggest: test on synthetic pairs with a known delay and phase rotation, and confirm the search recovers them.
- `auval` and `pluginval` early and often.
- Universal binary (Apple Silicon and Intel).
- If distributing: check the JUCE licence tier and plan for macOS notarisation.

---

## 11. Deferred (not in v1)

- Audition mode: sum processed signal and sidechain inside the plugin, with a sidechain trim.
- Meter ballistics controls (adjustable averaging, reset and hold).
- Phase-versus-frequency curve of the plugin's own processing.
- Delay advance (for example ±2 ms around a fixed reported latency).
- FFT-based cross-spectrum phase display.