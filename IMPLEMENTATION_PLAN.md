# Phase Align: Implementation Plan (v0.7, structured for v0.8 auto-suggest)

Companion to [PLAN.md](PLAN.md). PLAN.md says *what* the plugin is. This file says *how* to build it, in what
order, and lists where it deviates from PLAN.md. **Priority order throughout: sound and behaviour first, then
latency, then CPU.**

- Latency is allowed where the sound needs it, as long as it is reported to the host. The user-facing delay
  knob is never reported.
- In practice only Constant mode needs latency. Hi/Lo stay at zero (section 2.4).

Items marked **[verify]** are things I believe are right but have not confirmed. Check them before relying on them.

---

## 0. Summary of decisions

| # | Decision | Why | Changes PLAN.md? |
|---|---|---|---|
| R1 | **Constant mode uses a linear-phase FIR Hilbert transformer.** About 43 ms of reported latency, **only while Constant is selected**. | True constant rotation relative to the dry signal, so 0° equals the (delayed) dry signal. A zero-latency IIR pair smears the bass by about 2.3 ms even at 0°. | Yes (4.4, 4.8) |
| R2 | Keep the IIR filters of a bypassed stage running ("warm"). Skip only the mixing. | Clicks are avoided by design, for about 10 flops per sample (section 2.6) | Yes (4.6) |
| R3 | Hi↔Lo switches by gliding the corner frequencies instead of crossfading | The signal stays all-pass the whole way: no comb dip and no level change | Yes (4.7) |
| R4 | The knob angle is the true phase at a per-shape reference frequency (HIGH with RANGE in: the first section's angle, marked *; see 2.3 and R15). Sections run in sequence, and the second section is always running. | Changes of range or mode are continuous (glided). The readout means something. | Refines 4.3 |
| R5 | `phaseMode` is not automatable | Changing into or out of Constant changes the reported latency, and hosts re-compensate inconsistently | New |
| R6 | Meter: the audio thread only copies samples to a FIFO. The GUI does an FFT cross-spectrum and computes band correlation from it. | Lightest possible audio-thread cost, sharper bands, and it reuses code for v0.8 and the deferred phase curve | Refines 6 |
| R7 | Detect "no sidechain" from the **signal** as well as the bus layout | Logic always supplies a sidechain buffer, so the layout alone can't tell | Refines 6 |
| R8 | DSP code depends only on `juce_dsp` (for its FFT), plus a fast unit-test target in CI | The v0.8 analyser reuses the exact same code (4.9). Tests don't pull in the GUI modules. | Adds tests back to CI |
| R9 | Assets ship at 2.5× the default size and are **cached pre-scaled to physical pixels** on resize | Sharpness comes from the cache, not only from the asset resolution (section 4.2) | Agrees |
| R10 | **Delay −4 to +4 ms** (user, 2026-10-06; built 2026-10-06, branch `dsp-negative-delay`): delay on reports 4 ms of latency so the delay can be negative; delay off reports none (2.1a) | Align either track without moving clips | Yes (3.1, 3.4 "zero latency") |
| R12 | **Delay in steps of 0.1 sample** (user, 2026-10-06; built on `dsp-negative-delay`), replacing whole samples (PLAN decision 12) | Whole samples leave up to 2.4 dB of dip at 20 kHz on a coherent pair at 44.1 kHz; 0.1 sample leaves 0.02 dB (2.1a) | Yes (2.1, decision 12, open question 7) |
| R13 | **Hi/Lo runs oversampled** (4× below 85 kHz, 2× below 170 kHz) between linear-phase halfbands, and **reports a small latency whenever Constant isn't selected**: 32 samples at 44.1 kHz (0.73 ms), 18 at 48 kHz (0.38 ms), 5 at 88.2/96 kHz, none from 176.4 kHz (user, 2026-10-06; merged to master and approved by ear) | De-cramping: within 2.5° of analog sections to 20 kHz at every rate (was up to 80° at 44.1 kHz), so a setting sounds the same at every rate. Impossible at zero latency (Foster's reactance theorem, 2.3) | Yes (3.4 and 4.8 "zero latency", decision 4 "oversampling dropped") |
| R14 | **macOS ships two separate builds, not a universal binary** (user, 2026-10-06): arm64, and x86_64 labelled "Intel" in the GitHub release (`release.yml`'s `macos` and `macos-intel` jobs). Local builds are arm64 only | Each download stays half the size; only a small minority need Intel, but it is still offered | Yes (10, "universal binary") |
| R15 | **RANGE selects the Hi/Lo shape, and the modes are named for what they do** (user, 2026-10-06): RANGE out is one section (LOW 75.1 Hz, HIGH 150.1 Hz); RANGE in is two sections sharing the whole knob travel (LOW: stacked at 150.1 Hz; HIGH: 75.1 Hz and 1502 Hz), glided on a change. HIGH with RANGE in shows the first section's angle, 0–90°, with an asterisk. Replaces the single 0–180° knob mapping (P5) | Matches the reference unit's measured 90 and 180 behaviours (captures) with names that agree with its manual; the whole knob stays usable (no dead zones) | Refines 4.3 (2.3) |
| R16 | **The RANGE button is named by its position, "out" and "in", not "90" and "180"** (user, 2026-10-07): out (the default) is one section, in is two. Only some modes read 180 with it in (Low and Constant do; High shows 0 to 90 with an asterisk), so "RANGE 180" misled. The scale label still shows the number the knob reads up to (90 or 180); the tooltips, the host text of the `phaseRange` parameter ("Out" / "In"; typed "180" and "90" still read) and all the docs use out and in. The identifiers (`phaseRange`, `phaseWide`, `range180()`) are unchanged. | Names the control for what it does | Refines 4.3 (R15) |
| R17 | **Meter v2 (user, 2026-10-07):** a PHASE view (cross-spectrum angle per 1/24 octave, drawn only where coherent), a SLOW/FAST averaging toggle (fast: 0.1 s floor, 3 cycles; slow is P4's), and HOLD (manual, plus automatic while a host that reports its transport is stopped). Frozen, dragging across TIME OFFSET previews a delay (0.1-sample steps, within the knob's reach): the OUTPUT trace becomes the input as it would read with the delay knob there, on every view and the overall bar. BANDS: six segmented bands as a fourth view. TIME OFFSET weights each bin by its coherence above a per-bin noise floor (a kick against a kick sample read as noise under plain PHAT, user's DAW test 2026-10-07); \"clear\" excludes the peak's whole main lobe. FAST halves the frame (4096 at 48 kHz), tau floor 0.04 s and 2 cycles, and restarts the averages. | See what a snapshot looks like, find the delay peak by scrubbing, tell delay from rotation | Refines 3 (R6) |
| R18 | **TIME OFFSET gets an ATTACK reading** (user's DAW test with Kick.wav against Kick_1.wav, 2026-10-07; then Snare, bass, guitar and hats pairs). The two kicks start 1.23 ms apart but their sub bodies, tails and clicks differ, so the waveform lag read +29 ms and jumped around, never clear. Attack lag, built to cover any material (a snare has its energy high, a guitar stab in the mids, a bass note low): each stream split into three bands (35 to 150 Hz smoothed 12 ms, 150 to 600 Hz 5 ms, above 600 Hz 1.5 ms: shorter than a cycle of a bass note's harmonics and its ripple reads as repeated attacks; a peak keeps its place unless a rival is a third stronger; runner-up limit 0.65), in each the magnitude's moving average, log, positive differences only; cross-correlated through the same FFT frames with its own average (1 s slow, 0.4 s fast); normalised per band and averaged. A reading counts as clear after at least 6 of the last 8 computations are clear within 0.3 ms (value reported: their median). On the user's pairs, slow, every reading clear: kick 1.28 to 1.30 ms, snare 0.91, guitar 0.01, hats 0.02; bass -0.24 from its first stab, dropping out in the strumming; the waveform lag is clear for the bass, guitar and hats and unclear for the kick and snare, so both are always shown. Shown as a second column and a dashed trace; TRANSIENTS OUT OF DELAY RANGE follows the attack reading where it is clear, else the waveform's. The scrub preview still applies to the waveform measures only. A detection-mode switch was considered and not built: both readings are always computed, and ANALYSE can pick between them. | Hits whose waveforms don't match; any material | Refines 3 (R6, R17) |
| R19 | **ALIGNMENT view (`scope` in the code), and the view selector becomes a drop-up menu** (user, 2026-10-07: "zoom in to the start of a transient", "a sidechain overlay so you can manually adjust the signal to line up"; the bottom row had run out of room). ALIGNMENT: input, output and sidechain waveforms overlaid, each scaled to its own peak (the aim is comparing shapes and lining up where hits start); triggered on the sidechain's loudest onset of the last 3 s (`meter/ScopeBuffer`, a 4 s history of the capture's streams, which the capture has already lined up; the trigger holds while in the buffer unless a rival is 25% stronger, and stays put when frozen); mouse wheel zooms 0.5 to 200 ms; the hit sits a quarter of the way in. Frozen, dragging slides the input: the OUTPUT trace becomes the PREVIEW (the input delayed by the drag, linear interpolation), shared with the analyser's preview, so the other views follow. The selector is one label with an up triangle; its menu opens upwards (`PopupMenu` in the screen's own colours). Preview in ALIGNMENT is relative to where the drag began. | Aligning by eye | Refines 3 (R6, R17, R18) |
| R20 | **While held, the knobs drive the preview** (user, 2026-10-07: "if you were paused or held, will adjusting phase and delay update the alignment? That'd be a nice feature"). Step 1 (built): the delay (0 when the delay is off) and the polarity flip are applied to the held cross-spectrum (and to the ALIGNMENT view's input trace, slid and sign-flipped), so every view follows the knobs. Exact, because a delay and a polarity flip are. The view's label became ALIGNMENT (was SCOPE). Step 2 (built, `dsp/PhaseResponse.h`): the phase stage, by applying its closed-form frequency response (Hi/Lo: two first-order all-passes from the mapping's k at the oversampled rate; Constant: e^(-j theta)) to the held cross-spectrum, for the analysis views; checked against the real Chain's measured transfer function to within 2 degrees, 60 Hz to 12 kHz, at 44.1 and 48 kHz (MeterTests). The ALIGNMENT waveforms leave the phase out and say so. Step 3 (not built, probably not needed): the phase stage in the ALIGNMENT waveforms, by running the chain over the held audio. | Tuning on a frozen picture | Refines R17, R19 |
| R21 | **Only the active view's work runs** (user, 2026-10-07). The analyser takes `Needs` (curve, phase, bands, attack): the per-bin averages behind the overall bar always run (three FFTs a frame); the FREQUENCY curve, PHASE angles, BANDS and the ATTACK features (three bands of filters on three streams and nine more FFTs a frame) run only in their own view, set by `MeterScreen::applyNeeds()` on every view change. Switching a view on fills it at once from the averages, except the attack, which starts from nothing (about a second to settle). Measured in Release (MeterTests `[.analysercost]`, 48 kHz, one core): everything on 2.83% (the previous behaviour); FREQUENCY 0.28%, PHASE 0.25%, BANDS 0.23%, ALIGNMENT 0.22%, TIME OFFSET 2.69% (FAST: 2.00% on, 0.2% to 0.24% off the time view, 1.92% in it). | CPU | Refines 3 (R6, R17, R18) |
| R22 | **ALIGNMENT captures a hit** (user, 2026-10-07: "capture each transient and hold that view so the user can adjust, without having to scrub through to find one"). With CAPTURE on (a UI-state bool, default on, a label at the top left of the plot) the view holds the last hit `ScopeBuffer::scan` detects (a local peak of the sidechain's 0.5 ms envelope rise within 12 dB of the strongest of the last ~3 s, whose 4 ms envelope is at least 2.0 times its quietest level of the 5 to 22 ms before, so a ringing tail's ripples aren't hits, and at least 60 ms after the last) and shows, as the bright trace, the captured input rendered through the knobs (`meter/HitCapture`: 60 ms before to 240 ms after the onset of input and sidechain, the input's FFT times the delay, polarity and `phaseStageResponse`, back to time; exact for these linear stages, so ALIGNMENT now shows the phase turn too). A new hit replaces it only if no knob was turned in the last 2 s. On the user's acoustic pairs the detector finds kick 68 (61 real), snare 35 (31), guitar 16 (18), hats 31 (32) and every capture renders as the input shifted by the delay (MeterTests `[.usercapture]`). Capture off: the live view, as R19. Only the active view's work runs (R21): scanning runs in ALIGNMENT with CAPTURE on only. | Aligning without scrubbing | Refines R19, R20 |
| R23 | **Bold moves: three views, a vectorscope, blue input, slower and smoother** (user, 2026-10-07). FREQUENCY, TIME OFFSET and PHASE are gone from the screen; **BANDS** is the default, with **VECTORSCOPE** (a goniometer: input and output each against the sidechain, each scaled to its own level and turned 45 degrees, dots, the last 60 ms; on a stereo track a SOURCE toggle switches to the track's own L and R, from the capture's new opt-in side streams `inputSide` and `outputSide`, filled only while STEREO shows, delayed like the input) and **ALIGNMENT** (R19, R22) beside it, from the drop-up. The analysis side stays in `meter/CorrelationAnalyser` for the planned ANALYSE (curves, PHAT and attack lags, their tests and the user's pairs), unused by any view; it is switched off by the Needs of R21, and a cleanup of what ANALYSE doesn't use comes after that work (user). Old saved views (frequency, time, phase) open on BANDS. The input is **blue** (`design::meterInput`) in every view. SLOW is slower (tau at least 0.75 s, 12 cycles; was 0.3 s, 8), FAST a little steadier (0.08 s, 3 cycles; was 0.04 s, 2), and the bars and the overall bar ease toward the analysis at the screen's own pace (0.3 s on SLOW, 0.1 s on FAST), the end segment of a bar fading in with the fraction reached, so they glide. ALIGNMENT has - and + zoom buttons (a step of 1.5) beside the wheel. The legend labels (INPUT, OUTPUT, and SIDECHAIN in ALIGNMENT) are switches that hide or show that trace in every view, struck through when off, all on by default, kept in the UI state (`showInput`, `showOutput`, `showSidechain`). | The views that earn their place; one colour for one meaning | Refines R17 to R22 |
| R24 | **Fixes from the user's DAW use (2026-10-07).** (1) **Logic with no sidechain source chosen** treated the meter as having a sidechain: the plugin only checked that the sidechain bus had channels, and Logic (the user found) feeds the track's own signal in; `PhaseAlignProcessor::sidechainIsOwnInput` now treats a sidechain that is sample for sample the main input, for 0.25 s while capturing, as absent (silence in both doesn't count, the first differing sample ends it). (2) **Bands vanishing on FAST**: the analysis gates a band (not measured) under -70 dBFS of mean square; with FAST's short averages a band dips under it between hits (15 to 50% of readings on the user's kick, snare, bass and guitar pairs, 0% on SLOW: MeterTests `[.bandgate]`); the bars now hold their last value for `gateHoldMs` (0.8 s) before they go, and the desktop test checks it (it fails with the hold at 0). (3) **The VECTORSCOPE follows SLOW/FAST**: its window is 0.25 s on SLOW, 60 ms on FAST (it had been 60 ms always). (4) The CAPTURE and SOURCE boxes have HOLD's clearance (8 above the capitals and below the baseline). | Bugs found in use | Refines R23 |
| R11 | **Dim a switched-off section** (user, 2026-10-06; not built yet): its knob, switch, readout values and buttons go semi-transparent but stay usable; the on/off toggles stay at full opacity (4.5) | Shows at a glance what's in the signal path | New |

---

## 1. Architecture

```
AUDIO THREAD (processBlock)
  main in ─► flip (gain ramp) ─► phase stage ─► delay stage ─► main out
                                   ├ High/Low: 2× TPT all-pass, 0 latency
                                   └ Constant: cosθ·x[n−L] + sinθ·FIR-Hilbert(x), L latency
  sidechain ─┐
  in, out ───┴─► mono sums, latency-aligned ─► SPSC FIFO   (only when editor open AND meter on)
                                                  │
GUI THREAD (30 Hz timer)                          ▼
                                     FFT cross-spectrum analyser ─► MeterScreen
```

### 1.1 Source layout

```
src/
  dsp/                     # plain C++17 + juce_dsp FFT only: unit-testable and reusable offline (PLAN 4.9)
    Ramp.h                 # linear ramps and crossfade helpers
    DelayLine.h            # power-of-two circular buffer (used by the delay stage, Constant's dry path, meter alignment)
    DelayStage.h           # delay in 0.1-sample steps with dual-tap crossfade
    FractionalDelay.h      # the 0.1-sample interpolation kernels (Kaiser sinc, or the low-delay tables; length per rate)
    FractionalKernelTables.h # generated by prototype/fractional_tables.py: the low-delay minimax kernels (2.6)
    AllpassCascade.h       # 2× first-order TPT all-pass, double state (Hi/Lo)
    PhaseMapping.h         # knob angle + mode → section coefficients (provisional, then measured tables)
    HilbertFir.h           # FIR design (Kaiser-windowed ideal Hilbert) for the current sample rate
    PartitionedConvolver.h # zero-latency non-uniform partitioned convolution (direct head + FFT levels), work levels
    RealFft.h              # real FFT in vDSP's packed split format: vDSP on Apple, juce::dsp::FFT (half size) elsewhere
    ConstantRotator.h      # dry delay + convolver + cos/sin rotation
    Chain.h                # flip → phase → delay (PolarityStage, PhaseStage); ChainSettings; process; latency()
  meter/
    MeterCapture.h         # audio side: lock-free SPSC FIFO of 3 mono streams
    CorrelationAnalyser.h  # GUI side: windowed FFT, band cross-spectra, EMA, r per band + overall
  params/
    Parameters.h/.cpp      # versioned IDs, layout, ranges, string converters
  ui/
    Layout.h               # constants transcribed from ui/ui-info.csv (centre-based, 1954×1224 design space)
    Assets.h/.cpp          # SharedResourcePointer image/font store + scaled-image cache
    ImageKnob.h            # rotating cap image, gestures
    ToggleSwitch3.h        # 3-way image switch + clickable labels (selected label highlighted)
    PanelButton.h          # in/out image button + LED
    Readout.h              # green segment display with glow, double-click to edit
    MeterScreen.h          # opaque, 30 fps, monospace text; states (off / no sidechain / metering / later: analyse)
  PluginProcessor.*  PluginEditor.*
tests/                     # Catch2 v3 via FetchContent: PhaseAlignTests (full plugin; counts allocations per thread) and
                           #   tests/dsp/ → PhaseAlignDspTests (src/dsp + src/meter + juce_dsp only, counts allocations;
                           #   R8; also the hidden [bench], [dump] and [p3] cases)
prototype/                 # Python (use .venv, see CLAUDE.md)
tools/                     # asset pipeline script (magick + pngquant)
```

I'm writing our own small convolver instead of using `juce::dsp::Convolution`. We need precise control over
priming and latency, and the IR is fixed and simple. Both use `juce::dsp::FFT`, which uses Accelerate/vDSP on
macOS (checked 2026-10-06: `juce_dsp.cpp` defines `JUCE_USE_VDSP_FRAMEWORK 1` by default, and `AppleFFT` is the
highest-priority engine on macOS).

### 1.2 Parameters (permanent IDs, version 1)

| ID | Type | Range / values | Default | Notes |
|---|---|---|---|---|
| `delayMs` | float | −4 to +4 ms | 0 | Applied in tenths of a sample, `round(ms·fs/100)/10`, clamped to ±`round(4 ms·fs·10)/10` (2.1a) |
| `delayOn` | bool | | off | LED lit = on. **Not automatable** (it changes the latency, 2.1a). |
| `phase` | float | 0 to 1 (knob position) | 0 | Angle θ = phase × range. The host sees the angle at the current range. Replaced `phaseDeg` (2026-10-05, before any release). |
| `phaseRange` | bool | 90° / 180° | 90° | RANGE button: out = 90, in = 180. The knob keeps its position, so switching doubles or halves θ (user, 2026-10-05). |
| `phaseMode` | choice | High / Low / Constant | High | Switch up / centre / down. **Not automatable** (R5). |
| `polarity` | bool | | off | LED lit = inverted |
| `phaseOn` | bool | | on | |

**Not parameters** (stored in a `ui` child of the state ValueTree, so they don't clutter host automation):
`delayUnit` (ms/samples/cm), `meterOn`, `uiScale`, `meterView` (frequency/time, M4). The state root carries `stateVersion="1"`. Use
`juce::ParameterID{id, 1}` everywhere, and never rename an ID.

### 1.3 Real-time rules (enforced in review)

- No allocation, locks, logging or `std::function` on the audio thread. Allocate in `prepareToPlay` for the
  actual sample rate, and size scratch for the max block. If the host sends a bigger block, process it in chunks
  instead of reallocating.
- Read parameters through cached `std::atomic<float>*`. Handle control changes in sub-blocks of 32 samples, which
  is where coefficients get recomputed while a parameter is moving.
- `ScopedNoDenormals` in `processBlock`, double-precision IIR state, and no `-ffast-math`. For offline use without
  flush-to-zero, `Chain` also sets outputs below 1e-35 (about −700 dBFS) to 0: fades multiplying an already tiny
  value could otherwise produce subnormal floats (found by CI on Linux and Windows, whose random automation differs
  from macOS's; the test now runs 20 scripts).
- **Latency:** `Lmax + H` while the delay is on (2.1a: the reach in whole samples plus the interpolation lookahead), plus `L` in Constant (section 2.4). It never
  follows phase on/off, so toggling the phase stage never changes it. **As built (2.1a):** a listener on `delayOn`
  calls `setLatencySamples` at once when the change arrives on the message thread (an `AsyncUpdater` otherwise), and
  `prepareToPlay` sets it too; `phaseMode` joins the same listener with Constant. **Checked in JUCE's source
  (2026-10-06):** `setLatencySamples` does nothing if the value is unchanged, else calls every listener's
  `audioProcessorChanged` synchronously. VST3 (`juce_audio_plugin_client_VST3.cpp`, `audioProcessorChanged`) sets
  `kLatencyChanged` only when the latency differs from the last one reported, and its `ComponentRestarter` calls the
  host's `restartComponent` at once on the message thread (through an `AsyncUpdater` from any other thread);
  `getLatencySamples` returns ours. AU (`juce_audio_plugin_client_AU_1.mm`) does the same with
  `PropertyChanged(kAudioUnitProperty_Latency)` (plus `ClassInfo`), and `GetLatency()` is samples / rate. So the host is
  told, from the message thread, inside whatever host call delivered the parameter (e.g. VST3 `setParamNormalized` while
  stopped, or `setState`). How each host then re-aligns is the M5 host check.
- Override `processBlockBypassed` so host bypass delays the signal by the current latency. Otherwise the track
  jumps out of time when bypassed in Constant mode. **As built (M2, 2.1a):** host bypass runs the chain with every
  stage off but the latency kept (`ChainSettings::bypassed()`: the delay stays on at a knob value of 0, the mode
  stays), so it fades out like a stage bypass and is then the input delayed by the reported latency, bit-exact;
  leaving bypass fades back in. The delay line keeps being written throughout.
- Tail: about 0.05 s for Hi/Lo (IIR decay plus 8 ms). In Constant mode, add the FIR's half-length. **As built:** 0.1 s
  in every mode.
- `isBusesLayoutSupported` stays as it is: main mono/stereo with in = out; sidechain disabled, mono or stereo.

---

## 2. DSP design

### 2.1 Delay stage

(Steps of 0.1 sample since 2.1a/R12: taps are in tenths; whole-sample taps are a plain read, the others go through an
interpolation kernel. The rest of this section is unchanged.)

- Power-of-two circular buffer sized for `8 ms × fs` (2·Lmax, 2.1a) plus margin, with a mask instead of modulo. The buffer is
  **always written**, even when the stage is off, so a re-enable never reads stale or empty memory. Writing costs
  one store per sample.
- Two read taps and a linear (equal-gain) crossfade of **50 ms** (`dsp::crossfadeMs`, decided 2026-10-06, heard as click-free;
  P3 may tune it). Equal-gain is right because the two taps
  carry the same signal a few samples apart, so they are highly correlated.
- Knob drags generate many targets. If a fade is running, latch the newest target and start the next fade when
  the current one ends ("latest wins"). There is no pitch bend or Doppler effect, unlike a fractional read.
- Bypassing the stage is a fade to a 0-sample tap, then the tap work is skipped. The output is then the stage
  input, bit-exact.

### 2.1a Negative delay (R10; user, 2026-10-06; built 2026-10-06 on `dsp-negative-delay`)

- `delayMs` becomes −4 to +4 ms, default 0 (the knob at noon; the panel art already shows −4/+4 ms). `delayOn` defaults
  to **off**. Pre-release, so the ID keeps version 1.
- Implementation: with `Lmax = round(4 ms · fs)`, the delay stage applies `Lmax + d` samples (0 to 2·Lmax) and the
  plugin reports `Lmax` of latency, so the net shift is `d`. Delay off: the stage applies 0 and reports 0.
- **Total reported latency = (delay on ? Lmax : 0) + (Constant ? L : 0).** It now follows `delayOn` as well as
  `phaseMode`, so toggling the delay is a latency change like entering Constant (2.4): fade out, switch, update the
  latency on the message thread, fade in; the host re-aligns (when, varies by host [verify in M5]).
- Knock-on: the delay buffer needs 8 ms plus margin; host bypass must delay by the current latency
  (`processBlockBypassed`); the meter's input and sidechain streams need the latency-alignment delay line that was
  planned for Constant (the output is `Lmax` later than both); the TIME view shades −4 to +4 ms; readouts and typed values handle a minus sign (and the ghost digits);
  wheel/arrow steps cross zero; reset goes to 0; parameter, state and processor tests change.
- **Decided (user, 2026-10-06):**
  1. `delayOn` is **not automatable**, like `phaseMode` (R5), since it changes the latency.
  2. The knob value is kept while the delay is off, just not applied. Latencies simply add with Constant (about 4 + 48 ms).
  3. Meter TIME view: **remove** the "use Phase Align on the other track" and "move the track in the DAW first"
     hints. Instead, when the input's clear peak is outside ±4 ms, show **"TRANSIENTS OUT OF DELAY RANGE"**.
- **As built:**
  - Steps of 0.1 sample (R12, below): `d = round(ms·fs/100)` tenths, clamped to the reach `round(4 ms·fs·10)` tenths
    (exactly ±4 ms to 0.1 sample). `Lmax` = the reach rounded up to whole samples (177 at 44.1 kHz, 353 at 88.2 kHz).
    The latency while on is `Lmax + H`, H = the interpolation kernels' lookahead.
  - `Chain` owns the latency switch: on a `delayOn` change the output gain fades to 0 over `dsp::latencyFadeMs`
    (10 ms, P3 may tune), the delay stage jumps to its new tap (history kept) on the exact sample the fade lands
    (block-size independent), and the gain fades back in. Changing back during the fade-out reverses the fade without
    switching. `Chain::latency()` is the latency of what is coming out now; the meter alignment follows it.
  - The reported latency is updated when the parameter changes, i.e. at the start of the fade-out rather than after
    the switch (about 10 ms apart; hosts apply latency changes on their own schedule anyway). So the host has been told
    before the fade-in starts.
  - Readouts: ASCII minus; the ghost has a sign position (`-8.88`, `-888`, `-888.8`). Typed values accept `-` and
    U+2212. Host text is `-1.25 ms`.
  - Meter: `MeterCapture::push(..., latency)` delays the input and sidechain sums (cleared when capture starts). The
    TIME view shades −4 to +4 ms. Beyond ±5 ms (the view), a coarse search out to ±40 ms (whole samples, same "clear"
    test) finds the peak: the INPUT readout shows it to one decimal and the screen says TRANSIENTS OUT OF DELAY RANGE
    (`CorrelationAnalyser::inputPeak`, `inputOutOfReach`; MeterTests, 2026-10-06).
- **Sub-sample steps (R12; user, 2026-10-06: "lets add 0.1 samples"; 0.1 rather than 0.01 on my advice).**
  `prototype/subsample.py` → `prototype/out/subsample/report.md`: whole-sample rounding leaves up to 0.5 sample, a dip
  of up to 2.4 dB at 20 kHz (0.56 dB at 10 kHz) at 44.1 kHz for an equal, coherent pair; a 0.1-sample grid leaves
  0.02 dB, and 0.01 gains nothing audible (and is finer than the interpolation's own 0.005-sample error).
  - **As built:** `dsp::FractionalKernels`: for each fraction 1–9 tenths, a sinc centred at H + f/10 under a Kaiser
    window of the same centre, unity at DC. Length per rate, the shortest within 0.02 dB and 0.01 samples to 20 kHz:
    48 taps (β 7) below 46 kHz, 24 (β 6) below 80 kHz, 8 (β 7, 8 from 120 kHz) above; H = taps/2 − 1 = 23 / 11 / 3
    samples (0.52 / 0.23 / 0.03 ms). Whole-sample taps skip the kernel (bit-exact). The kernel loop runs over taps
    outside and outputs inside, over runs of samples with the same tap pair, so it vectorises and is identical for any
    block size. `DelayLine` stores each sample twice so any kernel window is one contiguous span.
  - Cost (Release, M-series, stereo): 3 ns per frame whole-sample, 13 ns fractional at 48 kHz, 21 ns at 44.1 kHz,
    4 ns at 96 kHz; 19–21 ns under constant automation. (Since 2.6's pass: the kernel loop keeps its sums in registers.)
  - **Low-delay kernels (built 2026-10-06; the user kept the Kaiser kernels, so they stay off).** `prototype/subsample.py` part 4 designs
    minimax kernels by linear programming with the lookahead free (not linear phase), within the same 0.02 dB /
    0.01-sample spec on float32 taps; `prototype/fractional_tables.py` writes them into `FractionalKernelTables.h`, and
    `FractionalKernels::design` picks `kaiser` (current) or `lowDelay`. Every test passes with either. See 2.6 for the
    numbers and the trade-off.
  - UI: readouts `-1.234 ms` / `-59.3 samp` / `-42.1 cm` (cm stays at 1 mm; 0.1 sample is 0.7 mm at 48 kHz), ghosts
    `-8.888`, `-888.8`, `-888.8`. Wheel/arrows: 1 sample, keeping the fraction; with Shift, 0.1 sample (the step function
    reads `ModifierKeys::currentModifiers`, so ImageKnob is unchanged). Host text `-1.250 ms`.
  - Meter: the PHAT peak is refined by Newton steps on the band-limited lag function (exact between samples) after the
    parabola, so the TIME view's "+1.042 ms" readings are good to about 0.002 samples on noise (MeterTests); readouts
    show 3 decimals.
  - The widest samples value (`-192.0 samp`) put the minus against the screen's inner edge. Fixed (2026-10-06): when
    the ghost would start within the screen's 10-unit inset, the digits and suffix are centred as a group (equal
    margins); ms, cm and the phase readout are pixel-identical to before (snapshots compared).

### 2.2 Polarity

A gain ramp from +1 to −1 over 50 ms (the same `dsp::crossfadeMs`), which dips through zero. Once
settled it becomes a multiply by ±1 (nothing at +1). Toggling mid-ramp reverses from where it is.

### 2.3 Hi / Lo modes (oversampled since 2026-10-06, R13: latency under 1 ms)

**Filter:** two first-order all-pass sections in series, with double state (designed as TPT one-poles; run in direct
form I since 2026-10-06, see "As built"). Parametrise each section by
`k = 1/g`, where `g = tan(π·fc/fs)`. The phase at frequency f is then exactly `φ(f) = 2·atan(k·tan(π·f/fs))`. So:

- `k → 0` is identity (corner at Nyquist), and `k → ∞` is a polarity inversion.
- `k` is clamped at `k_min` (phase at 20 kHz below 0.25° at 44.1 kHz). At `k = 0` the pole is on the unit circle,
  so the clamp is needed, and it keeps the section "running" at 0° without any audible effect.

**Knob mapping (RANGE redesign, 2026-10-06, R15; reference code `prototype/hilo.py`; replaces the P5 mapping):**

The panel angle θ (0–90° with RANGE out, 0–180° with RANGE in) gives φ = θ (90) or θ/2 (180), the angle of section 1,
which is its lag at its reference f₁. Section 2 carries θ₂ = φ with RANGE in and nothing (identity) with RANGE out.
`kᵢ = max(tan(θᵢ/2) / tan(π·fᵢ/fs), k_min)`: closed form, no tables. Each (mode, range) is its own **shape** with its
own references, all taken from measurements of the reference unit (nothing invented): 75.1 Hz, 150.1 Hz and
1502 Hz (20 × 75.1 Hz).

| Mode (named for what it does) | RANGE out | RANGE in |
|---|---|---|
| **LOW** | one section at 75.1 Hz | two stacked sections at 150.1 Hz |
| **HIGH** | one section at 150.1 Hz | sections at 75.1 Hz and 1502 Hz |

- **Why these names (user, 2026-10-06).** The manual describes RANGE in's two settings as one filter on the lows
  and one on the highs (the wide one, "high") and both filters on the lows (the narrow one, "low"). The captured
  behaviours had the opposite names in RANGE in, but matched the manual in RANGE out (Lo = the lower corner),
  so a pure rename could not make all four combinations consistent. Because RANGE is just a button, each combination
  takes whichever captured geometry fits its name: LOW 90 and HIGH 90 are the captured 90 behaviours unchanged;
  LOW 180 is the captured stacked pair (at 150.1 Hz), HIGH 180 the captured spread pair (75.1 and 1502 Hz).
  RANGE out sounds exactly as it did before (identical A/B levels, `prototype/out/range/report.md`); only the 180
  range changed, where the two sections now share the whole knob travel from 0° instead of the second one only
  joining past 90°.
- **How to read the names (user, 2026-10-07).** LOW centres the turn lower down in frequency and HIGH higher up, in both
  ranges: the frequency where half the full turn has happened, at full knob, is 75 Hz (LOW out), 150 Hz (HIGH out and
  LOW in) and about 336 Hz (HIGH in); with RANGE in, HIGH is also more spread out (239° of 360° at 1 kHz against LOW's
  326°). NARROW and WIDE were considered and rejected: with RANGE out both modes are one section of the same width, so
  those words would describe nothing there. The README's "Where the turn sits" says this to users.
- **Switching shape** (a RANGE press or a mode change) glides each section's k geometrically to the new shape's value
  over 30 ms (R3, the same glide as Hi↔Lo). The glide state is a weight on each of the four shapes (summing to 1);
  a change sets the target to the new shape alone and moves every weight to its target on a linear ramp of 30 ms from
  where it is, and each section's log k is the weighted sum of the shapes' log k at the current φ. So a change that
  arrives mid-glide, back to the shape just left or on to a third, continues from the current blend. (The first build
  of the redesign glided between just two shapes and jumped to the previous shape's k when a third arrived mid-glide:
  CI caught it as a sample step of up to 8.8× the test's limit on Linux and Windows, 12–26× the sine's own step in
  the reference; fixed 2026-10-06, with a regression test over random sequences and the CI seeds that exposed it
  covered in the automation test.) The knob keeps its position through a RANGE press, so φ is unchanged.
- **The panel angle (what the readout, scale label and host text show):** the shift at the first section's reference
  where that is exact, else φ. LOW and HIGH with RANGE out read 0–90 (the lag at 75.1 / 150.1 Hz); LOW with RANGE in reads
  0–180 exactly (the stacked pair's lag at 150.1 Hz); **HIGH with RANGE in shows φ, 0–90, marked with an asterisk**
  (on the scale label, after the readout's degree sign, and in the tooltips), because its first section's lag at
  75.1 Hz reaches only about 96° and the second adds its own turn higher up. Constant with RANGE in reads 0–180.
  `params::shownRangeDegrees`; the tooltips for the knob, readout, RANGE button and mode switch describe the current
  state.
- **As built (2026-10-06, branch `hilo-range-modes`):** `src/dsp/PhaseMapping.h` (the four shapes, `knobAngle`,
  `angles`, `kAngles`), `AllpassCascade` (state: φ on the linear ramp, plus shape, previous shape and glide; one
  `set(theta, mode, wide)` call so a mode and a range change together start one glide, not two), `HiLoStage`, and
  `ChainSettings::phaseWide`. Goldens regenerated (`prototype/golden.py`, with RANGE toggles and a simultaneous mode and
  range change in the script): worst difference 1.2e-7 at 44.1 and 48 kHz, 6e-8 at 96 kHz, below 1e-12 at 192 kHz;
  Constant's goldens are byte-identical. CPU is unchanged (Release: Hi at 60° 16.2 ns at 44.1 kHz, 14.6 at 48 kHz,
  Constant 38 ns at 48 kHz, before and after). New tests: the static response of each shape against the closed form
  within 0.01°, the lag at the first reference equal to θ where exact, RANGE and mode toggles on noise (energy within
  1.5 dB, no larger step than 1.5× the input's), and the UI's labels, asterisk, host text and tooltips by state.
- **No dead knob travel (user, 2026-10-06):** the reference's knob has dead zones (no change over about the bottom 20%
  and top 10% of its travel, with a floor of about 7° at 150 Hz at its minimum); ours starts at exactly 0° (identity)
  and uses its whole travel. Confirmed for all four shapes at every rate, 2026-10-06 (`prototype/out/hf/report.md`
  part 5): every 0.25° step of θ moves the lag by at least 0.138° somewhere in 20 Hz–20 kHz; the readout's frequency
  is within 0.0002° of θ.
- **Extending below the lowest corners (75 Hz) is closed** (user, 2026-10-06): the corners are the reference unit's,
  which was designed with them in mind; extending them would not be an improvement we can show.
- **Properties (P1, `prototype/out/p1/report.md`):**
  - Lag is non-decreasing in θ at every frequency in all four shapes.
  - There is no handover at 90° any more: with RANGE in both sections run over the whole travel, and a RANGE press or mode change glides (see above).
  - The readout is exact within 0.004° at every rate. In Hi it is the true lag at 150 Hz. In Lo past 90° it is
    90° + section 2's lag at 1502 Hz, because Lo's true lag at 75 Hz only reaches 95.7° at 180°.
  - The top octave moves fast just past 0° and just past 90°, where a corner sweeps down from far above 20 kHz. That
    is inherent in starting at identity, and the smoothing covers it.
- **Against analog sections (cramping; measured 2026-10-06, `prototype/hf_check.py` → `prototype/out/hf/report.md`
  part 2).** At the session rate each section is a bilinear first-order all-pass, exact at its reference frequency and
  squeezed towards 180° at Nyquist: against analog sections with the same lag at each reference frequency, worst over
  the knob's travel (just past 0° and 90°, where a corner sits highest), 11° at 5–10 kHz and 80° at 16–20 kHz at
  44.1 kHz (58° at 48 kHz, 9° at 96 kHz, 2° at 192 kHz), so the same setting's top octave differed between a 44.1 and
  a 192 kHz session by up to 78°.
- **Why it can't be fixed at zero latency (2026-10-06).** For any stable all-pass, X = tan(lag / 2) is a reactance
  function of the warped frequency Ω = tan(π f / fs), and Foster's reactance theorem (dX/dΩ ≥ |X| / Ω) says X / Ω
  never decreases. The first-order section holds it constant, so no all-pass of any order with the same lag at the
  reference frequency has less lag above it. Checked numerically: none of 19,981 random stable all-passes of orders 1–4
  went below the bound, and the planned two-anchor second-order fit put a pole outside the unit circle at every angle.
  Matching every rate to the 44.1 kHz curve was possible at zero latency (second-order sections above 44.1 kHz, within
  about 3°), but keeps the cramping. **The user chose to spend a little latency (R13).**
- **As built: oversampled (R13; `src/dsp/HiLoStage.h`, `Oversampler.h`, `HalfbandTables.h` generated by
  `prototype/oversampling.py`; reference `HiLoOversampled` in `prototype/hilo.py`, golden-tested at 44.1, 48, 96 and
  192 kHz).**
  - The same sections, mapping, smoothing and glides, at M times the rate (4× below 85 kHz, 2× below 170 kHz, 1×
    from there up), between linear-phase halfband FIRs: 59 then 11 taps at 44.1 kHz, 31 then 11 at 48 kHz, 11 at 88.2
    and 96 kHz (equiripple, ±0.03 dB per stage; the 59-tap one is the shortest that meets that at 44.1 kHz).
  - The halfbands add only a pure delay, a whole number of session samples (the outer stage of 4× takes one extra
    sample at 2× on the way down to make it so): **32 at 44.1 kHz (0.73 ms), 18 at 48 kHz (0.38 ms), 5 at 88.2 and
    96 kHz, 0 at 176.4 and 192 kHz.** It is reported whenever Constant isn't selected, phase on or off, so phase on/off
    stays a crossfade (the dry path is delayed to match) and only entering or leaving Constant changes the latency.
  - Against analog sections: within 1.6° to 16 kHz and 2.5° to 20 kHz at every rate (target 3° / 10°; LOW with RANGE in
    stacks two sections at one corner, so its error is about double: up to 4.9° at 44.1 kHz, measured 2026-10-06); the same
    setting within 0.4° between 44.1 and 192 kHz (was 78°). Typical settings (the median over the travel) are within
    0.4° at 16–20 kHz.
  - Kept: the readout (exact at the reference frequencies, now computed at M·fs), lag never decreasing as θ rises,
    0° = the delayed input (at k_min, as before), no bursts on coefficient moves.
  - Level: within ±0.07 dB (44.1 kHz) and ±0.09 dB (48 kHz) from 20 Hz to 20 kHz, ±0.03 dB at 88.2/96, exact at
    176.4/192. Above 20 kHz at 44.1/48 kHz it rolls off through the halfbands' transition band, as the delay's
    kernels already do (accepted, 2.6).
  - The coefficient grid stays 32 session samples long (32·M at the oversampled rate), so no more tan() runs than
    before and the angle and glides move exactly as they did.
  - Kept warm while unheard (in Constant, or settled off: R2), where the outer down-filter keeps only its history.
  - A/B for listening: `prototype/decramp_ab.py` → `prototype/out/decramp/` (the same settings at 44.1 and 96 kHz,
    old and new, summed with the dry track as against a second mic). Cost: 2.6 (J).
- **Knob feel:** `phase` is linear in degrees (an optional skew is a later detail).

**Smoothing:** smooth the knob angle linearly over about 30 ms, then map it to k per 32-sample sub-block, but only
while it is moving. When it is static, no `tan` runs at all. (P3 decides the lengths by ear.)

**As built (2026-10-06, `src/dsp/AllpassCascade.h`, golden-tested against `prototype/hilo.py`):** the angle ramps
linearly over 30 ms on a fixed 32-sample grid counted from reset (so the output is identical for any host block size);
at each cell start the angle and glide move on by one cell and each section's G = 1/(1 + k) is **interpolated linearly
per sample** from its old to its new value (every G in (0, 1] is all-pass). Without that, a corner sweeping down from
near Nyquist (just past 0° and 90°) stepped by up to 0.018 at 192 kHz in the click test. No tan() runs while static.
Switching back to the previous mode mid-glide reverses the glide from where it is (the prototype jumped; both changed).
The cascade runs warm while the stage is off (R2) and, since 2026-10-06, in Constant too, so leaving Constant never
restarts it. Denormals are flushed on the grid for offline use. Cost: about 17–25 ns per stereo frame for the whole
chain (Release, M-series); 4.0 ns since the 2.6 pass (both channels interleaved, direct form I); oversampled (R13),
18 / 23 ns at 48 / 44.1 kHz (2.6, J). Static cells run two samples per step (y[n + 1] from y[n − 1]) with both channels
in one register: the same filter, rounded differently in the last bits.

**Burst fixed (2026-10-06; found by the P3 renders, fix approved by the user):** at the identity clamp (k_min) a
section's pole sits just inside z = −1. Run as a TPT structure, its state was a near-lossless resonator at Nyquist (gain
1/k_min, about 3000) driven by the input's top end, hidden from the output while the section stayed there; when the
angle left 0° or 90° (or the cascade restarted on a playing signal, leaving Constant) it came out as a burst: on white
noise at ±0.5 the output peaked at over 4 (the old goldens had it), on a 100 Hz sine a mostly ultrasonic step of 0.043.
Fix: each section now runs in **direct form I**, y = −p x + x₁ + p y₁ with p = 1 − 2G, whose state is only its last
input and output, so nothing can hide there; the transfer function is the same, so static settings are unchanged
(within double rounding). And the cascade runs warm in Constant. `prototype/hilo.py` changed identically; the Hi/Lo
goldens were regenerated (rotated noise now peaks at 0.89, as rotated noise should). Test: PhaseTests "leaving 0 or 90
degrees on broadband input releases no burst" (fails on the old code: 11 of 20 cases, peaks up to 3.2; since R13 the
peak limit is 1.25, as band-limited rotated noise is nearly Gaussian and peaks near 4σ, and each 1024-sample window's
energy must stay within 1.5 dB of the input's). The P3 renders
are now within 16% of the sine's own step everywhere.

**Hi↔Lo switch (R3):** glide `k₁` and `k₂` to the new mode's mapping over about 30 ms. The signal is all-pass
throughout.

**Cramping:** removed by oversampling (R13, above).

### 2.4 Constant mode: linear-phase FIR Hilbert (R1)

**Why not the zero-latency IIR pair:** I measured the reference design (Niemitalo's 8-coefficient pair). It holds
90° within ±0.7° from 20 Hz to 20 kHz at 44.1/48 kHz. But it rotates relative to its own I branch, and that branch
has a group delay of 10.3 ms at 20 Hz, 2.6 ms at 100 Hz and 0.29 ms at 1 kHz. So even at 0° it delays the bass by
about 2.3 ms against the mids, which is most of the delay knob's range. Allowing latency removes the problem.

**Structure:** `y[n] = cos θ · x[n − L] + sin θ · (h ∗ x)[n]`

- `h` is an odd-length (type III), Kaiser-windowed ideal Hilbert transformer centred at tap `D`. Every
  even-offset tap is exactly zero, and its phase is exactly 90° at all frequencies. The only error is magnitude
  roll-off at the extremes, which turns into a small angle error there.
- The I path is a pure delay, so **0° is bit-exact with the delayed dry signal**, and 180° is an exact inversion.
- Q is computed by uniform partitioned FFT convolution with block `B`. The I path is delayed by `L = D + B` to
  match. The reported latency is `L`.

**Length (measured in Python, 48 kHz, β = 6):**

| Taps (latency D) | Error at 20 Hz | Error at 25 Hz and up |
|---|---|---|
| 2049 (21 ms) | −2.8 dB / 9° | ≤ 5°, exact by 50 Hz |
| **4097 (43 ms)** | **−0.1 dB / 0.4°** | **exact** |
| 8193 (85 ms) | exact | exact |

**Choice: 4097 taps at 48 kHz** (confirmed by the user after P2, 2026-10-06: "4097 works for now"). Scale the tap count with the sample rate to keep about 43 ms (≈ 0.0427·fs, odd),
with `B` = 256 (512 at 96 kHz and above). Total reported latency is about 48 ms at any rate. P2 confirms this by
ear; there is room to trade towards 21 ms if needed.

**Sound note:** a true constant rotation spreads a transient symmetrically in time, so a rotated kick gets a
small pre-wiggle. That is what phase rotation genuinely is, not an artefact. P2 includes a listening check.

**As built (2026-10-06, `src/dsp/HilbertFir.h`, `PartitionedConvolver.h`, `ConstantRotator.h`; golden-tested against
`ConstantRotator` in `prototype/p2_constant.py`):**
- Taps: 4097 at 48 kHz scaled with the rate and rounded to N ≡ 1 (mod 4), so D is even and taps 0 and N − 1 are zero
  too; they are dropped, so 4095 taps.
- **Since the 2.6 pass (2026-10-06):** non-uniform partitions (`PartitionedConvolver` levels: a direct head of 128
  taps, uniform 128-sample FFT partitions, and from 88.2 kHz a second level of 2048-sample partitions for the tail; with
  the generic FFT, 1024 and only from 176.4 kHz); the frequency-domain multiply-add and the head keep their sums in
  registers, and both channels share each partition's spectrum; FFTs through `RealFft` (vDSP directly in its packed
  split format; elsewhere a complex FFT of half the size). Equal to direct convolution within 2e-6 for any block
  layout and any change of work level (PhaseTests). Latency unchanged.
- **Latency L = D − 1** (2047 at 48 kHz: 42.6 ms; 42.7 ms at 44.1 kHz): the convolver applies the first block of taps
  directly per sample (only the nonzero ones) and the rest by uniform partitioned FFT one block ahead, so it adds no
  block latency. Block 128 (256 at 96 kHz and up), measured fastest with the direct head.
- Work levels: the convolver computes its output only while it is heard (not while sin θ is settled at 0, nor while the
  stage is off); in Constant mode it keeps its frequency-domain history current meanwhile, so it resumes on any sample
  for one block's cost (5 µs at 48 kHz). In Hi/Lo it keeps only its time-domain history and rebuilds on entering
  Constant (in the silence of the switch; 5–20 µs). This replaces the priming step below.
- θ on a 32-sample grid (20 ms ramp), cos/sin interpolated per sample across each cell; exact at 0, 90 and 180.
- Entering or leaving Constant goes through the chain's latency switch (fade out 10 ms, switch on the sample the fade
  lands, fade in). The delay line after the phase stage is cleared at that moment and the new path fades in ahead of it,
  so no later delay tap can read the old path's audio (found by the click test: a Constant switch followed within a
  few ms by the delay switching on clicked).
- Cost: about 62 ns per stereo frame at 48 kHz, 79 ns at 96 kHz (budget 150); 18 ns while not computing output.
  Since the 2.6 pass: 30 / 34 / 39 ns at 48 / 96 / 192 kHz (vDSP), 63 / 83 / 103 ns with the generic FFT (2.6).

**Transitions inside Constant:** θ is smoothed over about 20 ms, with cos/sin recomputed per 32-sample sub-block
only while moving. Phase on/off crossfades between `x[n − L]` and the rotated signal. Latency is unchanged.

**Entering or leaving Constant** (a rare, deliberate act, and not automatable):

1. Fade out over about 10 ms.
2. While muted, prime the convolver from an always-written input history (cheap writes). The priming is spread
   across the callbacks of the fade, so no single callback spikes.
3. Switch the path. The message thread updates the reported latency.
4. Fade in over about 10 ms.

There's a short dip, and then the host re-aligns. Hosts differ in *when* they apply a latency change. Some do it
immediately, and some (reportedly Logic) only on the next transport start **[verify per host in M5]**. The
fallback, if priming proves fiddly, is to keep the convolver running whenever the plugin runs (about +0.2% of a
core).

### 2.5 Transitions (all click-free, all tested)

| Event | Method | Length |
|---|---|---|
| Delay value change | dual-tap crossfade, "latest wins" (crossing zero is an ordinary change; taps may be fractional) | 50 ms (built, M2) |
| Delay on/off (a latency change) | fade out, switch while silent, fade in | 10 + 10 ms (built, 2.1a) |
| Polarity | gain ramp through zero | 50 ms (built, M2) |
| Stage on/off | delay: fade to tap 0; phase: dry/wet crossfade (filters warm) | 50 ms (built, M2) |
| Phase knob | angle smoothing, then coefficients (interpolated per sample) | 30 ms (Hi/Lo), 20 ms (Constant) (built) |
| Hi ↔ Lo | k-glide (all-pass throughout), reversible mid-glide | 30 ms (built) |
| Hi/Lo ↔ Constant | fade out, switch + latency change (delay line cleared), fade in | 10 + 10 ms (built) |

### 2.6 CPU (and latency): optimisation pass, 2026-10-06

- **Budgets** (checked by `PhaseAlignDspTests "[bench]"`, full chain per stereo frame): Hi/Lo **< 50 ns**; Constant
  **< 150 ns** (about 0.7% of a core at 48 kHz).
- **Method.** Release, Apple M1 Pro. Each benchmark case is the best of three 5 s runs in 512-sample blocks (the
  benchmark was made sturdier first; `PA_BENCH` picks cases, `PA_BENCH_SECONDS` lengthens them for a profiler). "Before"
  is master (6452ea9) built with the same benchmark, run back to back. Worst-callback figures swing with OS scheduling
  (±20 µs between runs); the means are steady. **Sound:** every change below is bit-exact except where noted, checked
  by rendering the automation script in every mode, at every rate, stereo and mono, with both builds
  (`PhaseAlignDspTests "[dump]"`) and comparing; the goldens still match within 1e-6 (Hi/Lo exactly; Constant 5.4e-7,
  as before), with either FFT engine.
- **Windows/Linux.** `-DPA_GENERIC_FFT_TESTS=ON` builds the DSP runner on macOS with the FFT Windows and Linux use (no
  vDSP), to measure it on the same machine; CI now prints the benchmark on every OS (numbers once CI runs).

**What each change did** (ns per stereo frame; the cases each change touches):

| # | Change | Case | Before | After |
|---|---|---|---|---|
| A | Idle Hi/Lo phase stage: no cos/sin while the rotator's angle is still (it ran twice per 32 samples even in Hi/Lo), `DelayLine` reads and writes as contiguous copies, no copies in `PhaseStage` when settled, cascade channels interleaved (bit-exact) | idle (delay off, Hi, phase off) | 10.5 | 6.6 |
| | | Hi at 60°, delay off | 11.2 | 6.5 |
| | | Constant at 60°, 48 kHz | 61.8 | 59.4 |
| B | Cascade recursion as one multiply-add per section (output bit-identical in every render) | idle | 6.6 | 4.4 |
| | | Hi at 60°, delay off | 6.5 | 4.3 |
| C | Convolver: head and frequency-domain multiply-add with sums in registers, both channels per spectrum load (within float rounding) | convolver alone, 48 kHz, 128 | 49 | 32.6 |
| D | `RealFft`: vDSP directly in its packed split format (JUCE's wrapper adds a strided layout and two extra passes) | convolver alone, 48 kHz, 128 | 32.6 | 23.9 |
| E | Non-uniform partitions from 88.2 kHz: 128 + 2048 (C–E together below) | convolver alone, 96 kHz | 53 (256) | 28.7 |
| | | convolver alone, 192 kHz | 75 (256) | 33.6 |
| A–E | (cumulative, master → after E) | Constant at 60°, 44.1 / 48 / 96 / 192 kHz | 67.8 / 61.8 / 79.4 / 97.5 | 40.6 / 33.7 / 33.9 / 39.3 |
| | | Constant at 0° (spectra only), 48 kHz | 18.1 | 12.3 |
| | | worst 64-sample callback, 192 kHz | 78–99 µs | 42–46 µs |
| | | mean 64-sample callback, 48 / 192 kHz | 4.0 / 6.3 µs | 2.2 / 2.5 µs |
| | | one-off rebuild on entering Constant, 48 / 192 kHz | 5–8 / 15 µs | 3–4 / 23–33 µs |
| F | Fractional delay kernel loop with sums in registers (bit-exact) | delay only, 44.1 kHz (48 taps) | 18.6 | 9.6 |
| | | delay only, 48 kHz (24 taps) | 11.7 | 6.6 |
| G | Generic FFT: complex FFT of n/2 plus one twiddle pass, instead of JUCE's real transform (a complex one of n) | generic: Constant at 60°, 48 / 96 / 192 kHz | 74 / 95 / 137 | 61 / 81 / 125 |
| H | Generic FFT: 128 + 1024 partitions from 176.4 kHz | generic: Constant at 60°, 192 kHz | 125 | 103 |
| I | Hi/Lo sections in direct form I, and the cascade warm in Constant (the burst fix, 2.3; not a CPU change: goldens regenerated) | idle | 4.5 | 4.0 |
| | | Constant at 60°, 48 kHz | 28.6 | 30.3 |

**All together, before → after** (vDSP; generic FFT in the last columns, same machine; after I):

| Case | master | branch | master, generic FFT | branch, generic FFT |
|---|---|---|---|---|
| idle: delay off, Hi, phase off, 48 kHz | 10.9 | 4.0 | | 4.1 |
| delay only, whole samples, 48 kHz | 11.0 | 4.3 | | 4.3 |
| delay only, fractional, 44.1 / 48 / 96 kHz | 24.8 / 17.8 / 13.2 | 9.1 / 6.1 / 5.4 | | 9.4 / 6.1 / 5.0 |
| Hi at 60°, fractional delay, 44.1 / 48 kHz | 25.4 / 18.1 | 9.1 / 6.0 | | 9.0 / 6.0 |
| Constant at 60°, delay off, 48 kHz | 54.3 | 28.1 | 89.5 | 59.9 |
| Constant at 60°, fractional delay, 44.1 / 48 kHz | 67.6 / 61.9 | 32.8 / 30.3 | 103.0 / 97.1 | 64.7 / 62.5 |
| Constant at 60°, fractional delay, 96 / 192 kHz | 78.5 / 97.2 | 33.8 / 38.8 | 129.7 / 148.2 | 83.1 / 102.8 |
| Constant at 0° (spectra only), 48 kHz | 18.2 | 9.0 | 35.0 | 24.5 |
| constant automation (all modes), 44.1 / 48 kHz | 47.8 / 40.7 | 30.9 / 26.0 | 63.6 / 52.5 | 45.2 / 36.4 |
| mean 64-sample callback, Constant, 48 / 96 / 192 kHz (µs) | 4.1 / 5.1 / 6.3 | 2.0 / 2.2 / 2.5 | 6.2 / 8.5 / 9.7 | 4.0 / 5.3 / 6.6 |
| worst 64-sample callback, Constant, 192 kHz (µs, of 333) | 79–100 | 35–46 | 89 | 63–82 |
| rebuild on entering Constant, 48 / 192 kHz (µs, once, while muted) | 6–8 / 15 | 3.5–4 / 22 | 8.6 / 21 | 5–6 / 36–74 |

**Windows and Linux (CI, run 37396526948, 2026-10-06; GitHub-hosted x86 runners, shared VMs, so pessimistic):**

| Case (ns per stereo frame) | M1, vDSP | M1, generic FFT | Linux CI | Windows CI |
|---|---|---|---|---|
| Hi/Lo idle | 4.0 | 4.1 | 6.4 | 10.3 |
| Hi at 60°, fractional delay, 44.1 / 48 kHz | 9.1 / 6.0 | 9.0 / 6.0 | 39.9 / 22.8 | 35.0 / 38.6 |
| Constant at 60°, fractional delay, 44.1 / 48 kHz | 32.8 / 30.3 | 64.7 / 62.5 | 134 / 119 | 236 / 232 |
| Constant at 60°, fractional delay, 96 / 192 kHz | 33.8 / 38.8 | 83 / 103 | 137 / 160 | 327 / 273 |
| worst 64-sample callback, Constant, 192 kHz (µs, of 333) | 35–46 | 63–82 | 188 | 1088 |

Hi/Lo is within budget everywhere. **Constant is over budget on Linux at 192 kHz and on Windows at every rate**, and
one Windows callback at 192 kHz took three times its length. The convolver-only layouts also rank differently there
(Windows 96 kHz: 525 ns uniform 128, 279 ns with 128/1024/4096). **Recommended: PFFFT behind `RealFft` (fetching its
source needs the user's OK), then re-tune the generic layouts from CI's convolver benchmark** (`HANDOVER.md` item 2).

**J. Hi/Lo oversampled (R13, 2026-10-06, branch `decramp-hilo`).** Same machine and method; master (dad5331) and the
branch built and run back to back (ns per stereo frame):

| Case | master | branch |
|---|---|---|
| idle: delay off, Hi, phase off, 48 kHz | 4.2 | 17.8 |
| Hi at 60°, delay off, 44.1 / 48 / 96 / 192 kHz | 4.1 (48) | 22.9 / 18.1 / 9.4 / 4.9 |
| Hi at 60°, fractional delay, 44.1 / 48 kHz | 9.2 / 6.2 | 28.0 / 20.3 |
| delay only, fractional, 44.1 / 48 / 96 kHz (Hi/Lo warm) | 9.3 / 6.4 / 5.6 | 26.3 / 19.7 / 9.9 |
| Constant at 60°, fractional delay, 44.1 / 48 / 96 / 192 kHz | 33.4 / 31.1 / 34.0 / 39.6 | 50.8 / 44.1 / 38.4 / 39.8 |
| Constant at 0° (spectra only), 48 kHz | 9.3 | 22.1 |
| constant automation (all modes), 44.1 / 48 kHz | 31.8 / 26.4 | 51.9 / 42.7 |

The Hi/Lo stage alone (`HiLoStage`, stereo, 32-sample blocks): 13.2 ns at 44.1 kHz (halfbands 9.2, sections at 4×
4.0), 11.6 at 48 kHz, 4.8 at 96 kHz; unheard (warm), 11.1 / 10.2 / 4.4. How it got there from a first cut of 28.5 /
18.3 / 7.3: the halfbands' taps are symmetric, so each pair of samples is added before its multiply, in blocks of 16
outputs with the sums in registers through `src/dsp/Simd.h` (NEON / SSE2 / scalar; clang vectorised the plain loops
along the taps instead, with lane reversals); a fused multiply-add on ARM; static cascade cells two samples per step
with the stereo pair in one register; the outer down-filter skipped while unheard. Moving the filters' history less
often measured no gain and was dropped. The rest of the chain adds about 6 ns more than before around the stage (the
delayed dry path, and more); not chased further. Within budget on the M1. **On the x86 CI runners Hi at 44.1 kHz may
go over 50 ns** (they ran 3–4 times the M1's figures before; the SSE2 path has no fused multiply-add). **Accepted by
the user (2026-10-06):** over 50 ns at 44.1 kHz on Windows/Linux is fine.

**K. PFFFT as the engine where vDSP isn't available (2026-10-06, branch `pffft-engine`; `HANDOVER.md` item 2).**
`libs/pffft` (submodule, BSD-style licence; `THIRD_PARTY_NOTICES.md`) replaces juce::dsp::FFT behind `RealFft`: its
ordered real transform is the packed split format interleaved, so one pass splits or joins it; 16-byte alignment is met
by its own buffers where the caller's aren't aligned. Measured on the M1 with `-DPA_GENERIC_FFT_TESTS=ON` (PFFFT on
NEON), master's generic engine and the branch back to back (ns per stereo frame):

| Case | old generic | PFFFT | vDSP (for reference) |
|---|---|---|---|
| Constant at 60°, fractional delay, 44.1 / 48 kHz | 65.2 / 62.5 | 36.0 / 32.9 | 32.8 / 30.3 |
| Constant at 60°, fractional delay, 96 / 192 kHz | 83.2 / 103.4 | 34.1 / 40.0 | 33.8 / 38.8 |
| Constant at 0° (spectra only), 48 kHz | 24.7 | 9.5 | 9.0 |
| worst 64-sample callback, Constant, 192 kHz (µs, of 333) | 63–82 | 42 | 35–46 |

The convolver's layouts were re-measured with PFFFT and the best are vDSP's (uniform 128 below 80 kHz, 128 + 2048
above: 27.7 / 33.6 ns for the convolver alone at 96 / 192 kHz), so `ConstantRotator::blockSizesFor` no longer depends on
the engine. CI's x86 numbers (SSE, and whether MSVC vectorises the register-blocked loops), which decide whether the
layouts need an x86 variant, are in L below. With J and K together (master after the merge, M1): Constant at 60°,
fractional delay, 44.1 / 48 / 96 / 192 kHz, 50.8 / 44.9 / 38.8 / 39.5 ns with vDSP and 51.1 / 45.7 / 38.1 / 39.1 with
PFFFT (the warm oversampled Hi/Lo is about 13 ns of it at 44.1 and 48 kHz).

**L. CI numbers with J and K (run 37420074764, commit 0511d9b, 2026-10-06).** CI runner numbers, not the M1's:
GitHub-hosted macos-latest (Apple silicon, vDSP), ubuntu-24.04 and windows-latest (x86-64, PFFFT on SSE; MSVC on
Windows), each one run on a shared VM, so noisy (the Windows idle case below is slower than Hi at 60° for that reason).
ns per stereo frame, from `PhaseAlignDspTests "chain cost per stereo frame"`:

| Case | macOS CI | Linux CI | Windows CI |
|---|---|---|---|
| idle: delay off, Hi, phase off, 48 kHz | 13.0 | 20.4 | 46.9 |
| delay only, whole samples, 48 kHz | 14.3 | 20.6 | 38.5 |
| delay only, fractional, 44.1 / 48 / 96 kHz | 18.5 / 14.9 / 8.3 | 43.0 / 31.7 / 15.3 | 64.6 / 50.4 / 24.5 |
| Hi at 60°, delay off, 44.1 / 48 / 96 / 192 kHz | 15.2 / 13.7 / 7.8 / 4.7 | 24.3 / 21.8 / 11.4 / 6.0 | 44.1 / 40.8 / 21.9 / 10.8 |
| Hi at 60°, fractional delay, 44.1 / 48 kHz | 21.6 / 16.5 | 45.1 / 32.7 | 69.2 / 91.5 |
| Constant at 60°, delay off, 48 kHz | 38.8 | 60.8 | 193 |
| Constant at 60°, fractional delay, 44.1 / 48 kHz | 44.3 / 40.8 | 82.0 / 72.1 | 214 / 208 |
| Constant at 60°, fractional delay, 96 / 192 kHz | 40.4 / 44.3 | 56.9 / 59.7 | 155 / 167 |
| Constant at 0° (spectra only), 48 kHz | 17.7 | 35.7 | 57.5 |
| constant automation (all modes), 44.1 / 48 kHz | 45.5 / 39.8 | 79.6 / 70.1 | 149 / 137 |
| mean 64-sample callback, Constant, 48 / 96 / 192 kHz (µs) | 2.7 / 2.6 / 2.9 | 4.7 / 3.9 / 4.0 | 13.3 / 10.0 / 10.6 |
| worst 64-sample callback, Constant, 48 / 96 / 192 kHz (µs, of 1333 / 667 / 333) | 15.9 / 23.4 / 63.7 | 22.4 / 42.9 / 64.4 | 55.9 / 122 / 156 |
| rebuild on entering Constant, 48 / 192 kHz (µs, once, while muted) | 5.0 / 26.8 | 7.3 / 34.2 | 21.4 / 95.4 |

Against the budgets (Hi/Lo < 50 ns, Constant < 150 ns):
- **macOS and Linux: all within budget**, Hi/Lo and Constant at every rate (Linux Constant peaks at 82 ns at 44.1 kHz,
  Hi/Lo at 45 ns). Linux's worst callbacks (64 µs at 192 kHz) are a fifth of the 333 µs the callback has. The Windows
  worst callbacks (156 µs at 192 kHz) are under half of it, where the previous CI run (before PFFFT) had one of 1088 µs.
- **Windows: Constant is over 150 ns** with delay off or fractional at 44.1 / 48 / 192 kHz (193 / 214 / 208 / 167 ns;
  155 at 96 kHz); the "all modes" automation case, which includes Constant, sits at 137 to 149, just under. Before K
  the Windows figures were 232 to 327 ns, so PFFFT took off a quarter to a half but did not reach the budget.
- **Windows: Hi/Lo is over 50 ns** in the delay-only and fractional-delay cases at 44.1 and 48 kHz (50 to 92 ns) and in
  the idle case (47 ns, noise: it exceeds the 41 ns of Hi at 60°). Hi at 60° with delay off is 44 / 41 ns, within
  budget. The 44.1 kHz excess on Windows and Linux is the accepted one (user, 2026-10-06); the 48 kHz fractional-delay
  case (91.5 ns, but 69 at 44.1 kHz, the dearer rate, so most likely a noisy sample) is not covered by that acceptance.
  Linux Hi/Lo is within budget everywhere (45 ns at 44.1 kHz with a fractional delay).
- Per frame, Windows is 2 to 3× Linux for the Simd.h halfbands and cascade (Hi at 60° 40.8 vs 21.8 ns, delay-only whole
  samples 38.5 vs 20.6) but 3 to 4× for the convolver (Constant 193 vs 61; convolver alone at 48 kHz, 128 partitions:
  152 vs 36.6, and macOS 24.3). Linux's ratio to macOS is about 1.5× across the board.

**Convolver layouts on x86** (`"convolver cost per stereo frame by block layout"`, ns per stereo frame; the layouts now
in use are 128 below 80 kHz and 128 / 2048 from 88.2 kHz):

| Layout | 48 kHz: mac / Linux / Windows | 96 kHz: mac / Linux / Windows | 192 kHz: mac / Linux / Windows |
|---|---|---|---|
| 128 (uniform; in use below 80 kHz) | **24.3** / 36.6 / 152 | 41.3 / 56.2 / 257 | 83.8 / 106 / 496 |
| 128 / 2048 (in use from 88.2 kHz) | 30.8 / 36.7 / 122 | **29.2** / 39.2 / 128 | **35.8** / **46.9** / 146 |
| 128 / 512 | 30.6 / 33.8 / 100 | 34.6 / 39.2 / 127 | 51.7 / 50.2 / 186 |
| 64 / 512 | 27.8 / **33.5** / **98.3** | 34.1 / **38.8** / 124 | 49.9 / 49.3 / 184 |

(Bold: the best of the 14 layouts on that machine, where it is among those four. Windows's best at 96 kHz is
64 / 512 / 2048 at 111 against 128 / 2048's 128, and at 192 kHz 130 against 146. Mac's best at 48 kHz is 128 and Linux's
at 192 kHz is 128 / 2048. The rest is in the CI log.)

**Decision on `ConstantRotator::blockSizesFor`: left as it is.** Linux's differences are within the run's noise (36.6
against 33.8 ns at 48 kHz) and its 96 / 192 kHz choice is already the best or within 1 ns of it. Only Windows
below 80 kHz clearly prefers a two-tier layout (128 / 512: 100 against 152 ns, about a third less), while the M1 prefers
the uniform 128 by a quarter (24.3 against 30.6). An x86-only layout would therefore help Windows alone and would also
apply to Intel Macs (R14), which no number here covers; and the ranking on Windows probably moves when the loops
vectorise (next paragraph), so tuning it now would tune around a compiler problem. If Windows stays over budget after
that, 128 / 512 below 80 kHz behind `_M_X64` is the first thing to try (about 52 ns off the 193).

**MSVC and the register-blocked loops (judged from the numbers; the disassembly is not available).** The loops run at
2 to 3× Linux's cost on Windows where the explicitly vectorised Simd.h code (halfbands, cascade) does, and at 3 to 4× where
the convolver's loops (head, multiply-add, fractional kernel) rely on the compiler: the runners share a CPU model, so
a gap that grows from 2× to 4× is what unvectorised, unfused float sums look like (MSVC will not reorder float
additions without `/fp:fast`, and the loops' sums in registers are such reductions). This points at those loops not
being vectorised on MSVC, but it does not prove it. **Not changed**: moving them onto `src/dsp/Simd.h` is the fix, to be
done and measured on a Windows runner or machine; with Constant at 155 to 214 ns against the 150 ns budget on a shared
VM it is worth doing, though no user-heard effect follows from it being over, and the worst callback is under half of its
length.

**Result of moving them (2026-10-06, branch `windows-convolver`, CI run 37436614308, windows-latest; the loops use
`Simd.h` under `_MSC_VER` only, so macOS and Linux compile exactly what they did).** It was MSVC: ns per stereo frame,
before (run 37420074764) then after:

| Case (Windows CI) | Before | After |
|---|---|---|
| Constant at 60°, delay off, 48 kHz | 193 | **53.3** |
| Constant at 60°, fractional delay, 44.1 / 48 kHz | 214 / 208 | **70.2 / 61.0** |
| Constant at 60°, fractional delay, 96 / 192 kHz | 155 / 167 | **61.1 / 45.9** |
| constant automation (all modes), 44.1 / 48 kHz | 149 / 137 | **62.7 / 53.3** |
| convolver alone at 48 kHz, uniform 128 | 152 | **28.3** |

Windows Constant is now well inside the 150 ns budget at every rate, about a third of what it was, and close to
Linux's. The block layouts are now within a few ns of each other on Windows (48 kHz: 128 is 28.3, 64 / 512 is 26.9), so
the `_M_X64` layout change is **not needed** and `blockSizesFor` stays as it is. Hi/Lo on Windows read 24 to 25 ns at
44.1 and 48 kHz with delay off (40 ns with a fractional delay at 44.1 kHz; the 91.5 ns at 48 kHz in the earlier run was
noise), all within budget; those figures were 44 / 41 before with no change to that code, which shows the runners'
spread (about ±30%: Linux's Constant read 79 against 61 in the two runs, with nothing changed there). macOS: Constant
42.1 against 38.8, likewise noise. pluginval passed on all three platforms.

**pluginval on the CI runners (same run).** Strictness 10, in-process, on the VST3: **passed on Windows and Linux**
(each ended `SUCCESS`, no failed tests), as on macOS. `continue-on-error` is dropped from their step in `ci.yml`.

**Candidates looked at and not built:**
- Polyphase split of the Hilbert (every other tap is zero): in the frequency domain it is the same work as doubling the
  block. Per sample, uniform 128 is 31 complex multiply-adds plus 64 head taps; polyphase at 64 decimated is 31.5 + 64,
  and at 128 decimated it is 15 + 128, the same as uniform 256. The block-layout benchmark covers that trade directly.
- `vDSP_zvma` for the multiply-add: measured slower than C (28.1 vs 23.9 ns at 48 kHz, 31.0 vs 28.7 at 96 kHz), since
  it reads and writes the sum once per partition.
- Packing stereo into one complex FFT: a complex FFT of n points costs about two real ones, and the multiply-add covers
  the same number of bins, so no gain is expected.
- PFFFT: on the M1 the generic engine was within budget after G and H (103 ns at 192 kHz), but CI's x86 numbers
  above say Windows and Linux need it. Built since (K).

**Latency.**
- Constant's L = D − 1 follows from the tap count. Nothing else in the path adds samples. The delay's Lmax + H is the
  least its ±4 ms reach allows with a lookahead of H. In Constant, the Hilbert's lookahead and the delay's add, because
  their non-causal parts add.
- **The fractional-delay kernels can lose most of their lookahead**, at the same 0.02 dB / 0.01-sample spec (phase delay,
  20 Hz–20 kHz, float32 taps): `prototype/subsample.py` part 4, checked independently and by the C++ tests. This is
  built but not switched on (`FractionalKernels::design`; **user, 2026-10-06: keep Kaiser**):

  | rate | Kaiser now: lookahead / taps | low-delay minimax: lookahead / taps | delay-on latency now → low-delay |
  |---|---|---|---|
  | 44.1 kHz | 23 / 48 | 6 / 28 | 200 → 183 samples (4.54 → 4.15 ms) |
  | 48 kHz | 11 / 24 | 4 / 16 | 203 → 196 (4.23 → 4.08 ms) |
  | 88.2 / 96 kHz | 3 / 8 | 1 / 6 | 356 → 354, 387 → 385 |
  | 176.4 / 192 kHz | 3 / 8 | 0 / 4 | 709 → 706, 771 → 768 |

  The trade-offs:
  - The Kaiser kernels sit far inside the spec in the mids, at 0.0013 dB / 0.0003 samples below 10 kHz at 44.1 kHz.
    The minimax ones use nearly all of it in every band (0.019 dB / 0.0097 samples at 44.1 kHz).
  - The minimax kernels are not linear phase. Their group delay ripples by up to 1.2 samples near 20 kHz at 44.1 kHz
    (0.5 at 48 kHz, under 0.1 from 88.2 kHz), against 0.04 for the Kaiser kernels.
  - They are cheaper: the delay-only fractional case at 44.1 kHz drops from 9.6 to 6.9 ns.
  - With more headroom: 7 / 32 at 44.1 kHz uses 66% of the spec.
- **Above 20 kHz** (`prototype/out/hf/report.md` part 4): the Kaiser kernels' spec stops at 20 kHz, and a fractional
  setting rolls off above it, worst at half a sample: −1.6 dB at 21 kHz and −26 dB at 22 kHz at 44.1 kHz; −0.3 dB at
  21 kHz and −1.9 dB at 22 kHz at 48 kHz; flat at 96 kHz and up. Whole-sample settings are exact everywhere. Holding
  44.1 kHz flat to about 21.5 kHz needs about twice the taps (about 0.5 ms more latency). **Accepted as is (user,
  2026-10-06): rolling off above 20 kHz at 44.1 kHz is fine.**

---

## 3. Correlation meter (R6, R7; as built in M4 after P4)

**Audio thread** (`meter/MeterCapture.h`): if the capture is active (editor showing AND meter on), push three mono
sums per 32-sample sub-block (input, output, sidechain; zeros without a sidechain bus) into a preallocated SPSC FIFO
(`juce::AbstractFifo`). Otherwise do nothing. The FIFO is a fixed 2^16 samples per stream (1.4 s at 48 kHz, 0.34 s at
192 kHz), allocated once in the constructor so a rate change never reallocates under the reader. If it is full,
samples are dropped from all three streams together. The output is later than the input by the chain's latency (Lmax
while the delay is on; `L` more in Constant), so the input and sidechain sums go through a delay line of that length
first (built with 2.1a; Constant only has to raise its maximum).

**GUI thread** (`meter/CorrelationAnalyser`, driven by MeterScreen's 30 Hz timer):

1. Pull samples. FFT size `N` = 8192 at 44.1/48 kHz, scaled with fs (16384 at 88.2/96, 32768 at 176.4/192); Hann;
   **75% overlap** (a new result about every 43 ms, so the display moves smoothly; P4 used 50%, same averaging).
2. Per **bin**, from 20 Hz to 20 kHz: exponential averages of `X·Y*` and `Z·Y*` (complex) and `|X|²`, `|Z|²`, `|Y|²`,
   with time constant `max(0.3 s, 8 cycles of the bin frequency)` (P4: settles in about 0.75 s, jitter ≤ 0.04).
3. Everything shown is a ratio of sums of these: `r = Σ Re(X·Y*) / sqrt(Σ|X|² Σ|Y|²)`, the band-limited Pearson
   correlation.
4. Gate: if either signal's mean-square level in a window is below −70 dBFS, that part is not drawn.

**What the screen shows** (P4, `prototype/out/p4/report.md`; the panel art's 16 band bars were only an example, user
2026-10-06). Band bars average the comb filter of two spaced mics away above about 1 kHz, so the screen has two views,
chosen by clicking the labels on its bottom row:

- **FREQUENCY** (default): r(f) over 1/6 octave at 256 log-spaced points, processed bright with a faint fill,
  unprocessed (input) dim.
- **TIME OFFSET**: the PHAT lag function from −5 to +5 ms (every bin's averaged cross-spectrum set to unit magnitude,
  then inverse FFT): a unit spike at the lag of a pure delay on any material, and a spike at 0 ms (negative if
  inverted) for a phase rotation. The plain cross-correlation would turn a phase rotation into a hump at about
  +0.9 ms and suggest a delay that isn't there. The input's and output's peaks are printed ("INPUT +1.04 ms"). The
  delay knob's reach (−4 to +4 ms) is shaded; a clear input peak outside it shows "TRANSIENTS OUT OF DELAY RANGE" (2.1a),
  found out to ±40 ms by a coarse search beyond the view (the readout then shows it to one decimal).
- **PHASE** (R17): the angle of the cross-spectrum (output minus sidechain, -180 to 180) per curve point, 1/24-octave windows, brightness by coherence, nothing drawn where coherence is within noise (a window of few bins needs more). A straight slope is a delay; a flat offset is a rotation.
- **SLOW / FAST** (R17) and **HOLD** (R17): see R17. A preview is exact for the pure delay the plugin applies; it turns each input bin by its own delay phase before the windowed sums, so it also brings back high-frequency phase a long delay had scrambled.
- All views: the overall pair on the right (processed: lit segments; input: a tick), r over all bins 20 Hz–20 kHz.

**"No sidechain":** NO SIDECHAIN SIGNAL when the sidechain bus is disabled, **or** its level has been below the gate
for more than 1 s, **or** no audio has arrived for 1 s. **[verify the Logic, Live and Reaper behaviour]**

When the meter is off, the editor is closed, or the window is hidden (checked on every timer tick), nothing runs
anywhere: no capture, no timer, no analysis. The screen repaints only itself, only on a new result while metering.

---

## 4. UI implementation

### 4.1 Layout from `ui/ui-info.csv`

- All CSV x/y values are **image centres** in the 1954×1224 design space (confirmed against the artwork). This
  remains the coordinate system even though `plugin-base.png` is now 3908×2448: same aspect ratio, exactly 2×.
- ~~Everything lives in one `content` component sized 1954×1224, and the editor applies
  `AffineTransform::scale(editorWidth / 1954.0f)`.~~ **Changed in M3:** the editor lays each control out directly at
  `designRect × (editorWidth / 1954)` with its edges rounded to whole pixels (`ui/DesignComponent.h`). Under a
  transform, slot edges land on fractional device pixels, so the 1:1 pre-scaled images would be resampled again by
  half a pixel and blur. Controls convert design-space rectangles with `toLocal()`.
- `src/ui/Layout.h` is generated from the CSV by `tools/ui_layout.py` (run by `tools/build_assets.sh`): one `Slot` per
  component (CSV name in camelCase) and the image list. Hand-measured values the CSV doesn't carry (screen interiors,
  label spacing, meter geometry, colours) are in `src/ui/Design.h`.
- CSV gaps and interpretations: the delay knob has no w/h, so I'll use 245×245 like the phase knob. The ANALYSE
  rows saying "when meter is on" are read as "lit".
- Knob rotary range: ±142.1° from the top, measured on the 17 baked ring dots (17.8° apart), so the pointer
  lands on dots.

### 4.2 Size, scaling and assets

- **Reference size 977×612** (`uiScale` 1), user resizable from 60% to 200% of it (586 to 1954 px wide; the minimum was 75%, 733 px, until user 2026-10-06), with a locked
  aspect ratio, a corner handle and the scale saved in state. **A new instance opens at 80%, 782×490** (user,
  2026-10-06; `PhaseAlignProcessor::defaultUiScale`). 250% of that is 1954×1224: the design space, and the maximum size.
- **Assets at 2.5× of default slot sizes**, which since the 80% default (2026-10-06) is exactly the design space
  (base 1954×1224; 1.5 MB embedded, was 3.0 MB). That is 1:1 on Retina at 977 px wide (125% of default) and on 1×
  displays at the 1954 px maximum; larger Retina sizes are upsampled. Everything drawn in code stays crisp at any size.
  The pipeline never enlarges a master, so the 1954 px original base works as well as the 3908 px upscale.
- **Pipeline** (`tools/build_assets.sh`): `magick` resamples each master to 2.5× of its CSV slot size, and
  `pngquant` compresses. Output goes to `assets/images/`, which is what BinaryData embeds (fonts in `assets/fonts/`,
  OFL texts in `assets/fonts/licenses/`). `full example.png` is excluded.
- **At runtime:** decoded images live in a `SharedResourcePointer`, shared across plugin instances. On resize,
  each image is resampled *once* to its exact physical pixel size and then painted 1:1. Nothing is resampled per
  paint. The physical size comes from the paint context, so moving between displays and snapshot rendering work too.
  Big reductions (3–4× at 75% on a 1× display) are box-filter halvings followed by one interpolated step.

### 4.3 Visual spec

| Element | Spec |
|---|---|
| Panel labels (switch options) | **Arial Bold.** It's a system font on macOS and Windows; Linux falls back to Liberation Sans, which is metric-compatible. **Only the selected option is highlighted** (near-white with a faint glow); the others are dimmed grey. Delay labels are left-aligned to the right of the switch; phase labels are right-aligned to its left. |
| Knob readouts | **DSEG7 Classic** (OFL), the calculator-style 7-segment font, in green with a subtle green glow. The glow is a blurred copy of the text, rendered to a cached image only when the value changes. Optional: unlit "8" ghost segments at about 6–8% opacity for an authentic LED-display look. **Every readout carries a unit indicator**: a small suffix, bottom-right of the digits, in the same green and glow but set in Arial Bold, so the font's glyph coverage doesn't matter. Delay: `1.234 ms` / `59.3 samp` / `42.1 cm` (R12), following the unit switch. Phase: `90.0°`. |
| Meter screen text | Monospace "terminal" look: **IBM Plex Mono** (OFL), crisp at small axis-label sizes. If you want it chunkier and more retro, **VT323** (OFL) is a drop-in alternative. Phosphor green to match the bars. |
| LED rings | Decorative, left as baked into the art. |
| Knob caps | The whole image rotates, baked lighting included. |
| ANALYSE | Present in v0.7, non-functional: unlit face, pressed and released visuals on click, tooltip "Auto-suggest: coming in a future version". |

All fonts are embedded via BinaryData except Arial, and their OFL licence texts ship with the installer.

### 4.4 Components

| Component | Notes |
|---|---|
| Knobs | Vertical drag; Shift = fine (×0.1); wheel and arrow keys = 1 sample (delay; 0.1 sample with Shift, R12) / 0.5° (phase); Cmd- or Alt-click = reset; double-click = edit (override JUCE's double-click reset). Repaint only on value change. |
| Toggle switches | Three images. Click the upper or lower half to step, drag up or down, or click a label to select it directly. |
| DELAY / Ø / PHASE + LEDs | `button-in/out` + `led-on/off`. Circular hit test. LED lit = stage on / polarity inverted. |
| Readouts | The delay shows the **effective** (rounded-to-samples) value in the selected unit. Double-click opens an in-place TextEditor styled to match; typed values convert back to ms. |
| METER | Four-state images (in/out × lit/unlit). Turning it off stops capture, analysis and the timer. |
| Meter screen | `setOpaque(true)`, bounds = the black screen interior, so the panel behind is never repainted. Repaints only itself at 30 fps, and only while the meter is on and the editor is showing. Processed bar = lit segments; unprocessed = outline or tick marker; overall pair on the right. States: OFF, NO SIDECHAIN SIGNAL, METERING (and in v0.8: CAPTURING, ANALYSING, RESULTS). |
| Tooltips | One `TooltipWindow`. Every control states what it does and its units. **Both knob tooltips end with the fine-control hint**, e.g. "Shift-drag for fine control; mouse wheel or arrow keys nudge by one sample" (delay) / "... by 0.5°" (phase), plus "Double-click to type a value, Cmd/Alt-click to reset". The cm tooltip gives 343 m/s; the Constant tooltip mentions the added (compensated) latency, and the DELAY button's its about 4.5 ms. |

### 4.5 Switched-off sections (R11; user, 2026-10-06; built on `ui-dimming`, alpha `design::dimmedAlpha` pending sign-off)

- Delay off: the delay knob, the unit switch and its labels, and the delay readout's value go semi-transparent.
  Phase off: the phase knob, the mode switch and its labels, RANGE, the drawn 90°/180° scale label and the phase
  readout's value. Everything stays fully usable.
- Always full opacity: the on/off toggles (DELAY, Ø, PHASE) and their LEDs, METER, ANALYSE.
- Readouts: the faint ghost "8" segments stay as they are; only the value (digits, glow, unit suffix) dims.
- Fade the change (about 100 ms) or switch instantly; only the affected components repaint, never the panel.
- **Decided (user, 2026-10-06, "agreed all"):** everything above. The knob ring dots and the "−4ms/4ms", "0°" scale
  text are baked into the panel art and stay at full brightness (no separate layer; this is my reading of "agreed
  all", confirm if it matters). **The polarity flip remains regardless:** Ø is independent of the phase section and
  always applies, so with phase off and polarity inverted the phase readout shows the rotation actually applied,
  "180.0", at full brightness; with phase off and no inversion, the readout's value is dimmed.

---

## 5. Milestones

Each item has a "done when" check. P* (Python) and U* (UI) work can run in parallel. D* depends on the P results.

### M0: Foundations
- [x] Project venv + `prototype/requirements.txt` + CLAUDE.md note.
- [x] Source layout per 1.1 (2026-10-05; `src/dsp/` and `src/meter/` arrive with M2/M4). Parameters and state per
  1.2, with state round-trip tests (`tests/StateTests.cpp`). `meterOn` defaults to on, `delayUnit` is stored as
  `ms`/`samples`/`cm`, `uiScale` is 0.6–2. Loading validates the ui properties and keeps the editor's ValueTree.
- [x] `tests/` target (Catch2 v3.16 via FetchContent) + `ctest` in CI (Linux under xvfb). pluginval (strictness 10) in
  CI on macOS: **added but not yet seen running** (not installed locally).
  - **Deviation from R8:** for now the single test runner compiles the full plugin sources, editor included, because
    the state and editor tests need the real processor and editor. That roughly doubles a clean build (CI caps
    parallelism already; release builds pass `-DPA_BUILD_TESTS=OFF`). M2's DSP tests can get a lighter
    `src/dsp` + `juce_dsp` runner if build time matters.
- **Done when:** the plugin loads with all parameters, state round-trips, and CI is green with tests running.

### M1: Python prototypes
- [x] **P1 Hi/Lo** (2026-10-05): `prototype/hilo.py`. Two-section cascade with the P5 mapping, angle smoothing and Hi↔Lo k-glide;
  plots at 44.1/48/96/192 kHz; the 90° handover is continuous, phase is monotonic in θ, the sample processor matches the closed
  form to 1e-13°. Results in `prototype/out/p1/report.md` and section 2.3.
- [ ] **P2 Constant:** FIR Hilbert at 2049/4097/8193 taps (48 kHz equivalent); accuracy plots per rate; render multi-mic pairs rotated at several angles → **you listen and confirm the length** (and that true rotation is useful on real material).
  - **Built (2026-10-06); length confirmed by the user: 4097 taps.** `prototype/p2_constant.py` → `prototype/out/p2/` (report,
    accuracy.png, kernel.png, renders of four synthetic pairs). The table in 2.4 is confirmed at every rate (tap counts
    scaled with the rate give the same low end everywhere; the top is exact at all lengths). On the synthetic pairs the
    2049- and 4097-tap results differ from 8193 by −44 to −62 dB and −51 to −81 dB, because they carry little below 40 Hz;
    the choice is about real sub-bass (30–40 Hz kick and bass fundamentals), where 2049 loses up to 0.8 dB / 2.6° at
    30 Hz. Real stems still needed for "is rotation useful".
- [ ] **P3 Transitions** (starting point: linear 50 ms crossfades for delay changes, delay bypass and polarity; about
  30–50 ms coefficient glides for the knob, Hi/Lo, range and phase bypass): render worst-case automation (delay jumps, flips, Hi↔Lo glides, bypass toggles, the Constant entry/exit sequence) to WAV; tune fade lengths by ear and by peak sample-to-sample step.
  - **Groundwork built (2026-10-06):** `PhaseAlignDspTests "[p3]"` (`tests/dsp/P3Renders.cpp`) renders nine scenarios
    (delay jumps and sweeps, delay on/off, polarity, Hi↔Lo glides with reversals, phase sweeps in each mode, phase
    on/off, Constant entry/exit, everything at once) through the C++ chain at 48 kHz in 64-sample callbacks, on a
    100 Hz sine, pink noise and a drum/bass loop; `prototype/p3_report.py` writes the WAVs, event lists, plots and
    `prototype/out/p3/report.md` (worst steps raw and to 20 kHz, with times and the change just before). Every scenario
    stays within 16% of the sine's own step (it found the burst fixed in 2.3). Fade lengths unchanged.
    **Left: the user's listening.**
- [x] **P4 Meter** (2026-10-06): `prototype/p4_meter.py` → `prototype/out/p4/` (report, mock-ups). Band correlation is
  accurate at any width from octave to 2/3 octave (1/3 octave has empty low bands at N = 8192); averaging stays at
  max(0.3 s, 8 cycles). Bars hide the comb, so the meter became a frequency curve plus a PHAT time-offset view (section 3).
- [x] **P5 Hi/Lo mapping** (2026-10-05): the corner frequencies and the knob-to-angle split per mode (section 2.3),
  closed form in `prototype/hilo.py`. Answers PLAN open questions 1 and 2. By ear (user, M2): no clicks on switching,
  no zipper noise, smooth delay sweeps.

### M2: C++ DSP core
- [x] **First part (2026-10-06):** `src/dsp/` Ramp, DelayLine, DelayStage (whole-sample, dual-tap 50 ms crossfade, latest
  wins), polarity ramp, phase stage on/off crossfade around an **identity placeholder**, Chain (32-sample sub-blocks,
  `latency()` = 0). Wired into `processBlock` (cached atomics, ScopedNoDenormals, no allocation) and
  `processBlockBypassed`. Tests in `tests/dsp/ChainTests.cpp` and `tests/ProcessorTests.cpp`: exact impulse delay at
  44.1–192 kHz, settled stages bit-exact, latest-wins, polarity ramp, no step above threshold under scripted automation
  (fails if the fades are removed), no NaN/denormals, block sizes 1–4096 and variable bit-identical, no allocation,
  host bypass. Benchmark (`PhaseAlignDspTests "[bench]"`, Release, M-series): 1.6 ns per stereo frame static, 11 ns
  under constant automation.
- [x] AllpassCascade (current P5 mapping), HilbertFir + PartitionedConvolver +
  ConstantRotator (4097 taps), phase-knob smoothing (2026-10-06, branch `dsp-port`; as built in 2.3 and 2.4).
- [x] Tests (`tests/dsp/PhaseTests.cpp`, `ChainTests.cpp`, `ProcessorTests.cpp`; the automation script now moves the angle
  and switches Hi/Lo/Constant too):
  - impulse delay is exact at every rate
  - Hi/Lo magnitude flat within ±0.001 dB; the readout (section 2.3) equals the knob angle within 0.1°
  - Constant at 0° is bit-exact with dry delayed by L; Constant at 90° gives 90° ± 0.5° from 20 Hz to 20 kHz
  - reported latency matches the measured impulse offset in every mode
  - bypassed stage is bit-exact
  - no sample step above threshold during scripted automation
  - no NaN or denormals under fast automation
  - block sizes 1 to 4096 and variable; sample rates 44.1 to 192 kHz
- [x] **Golden reference tests:** `prototype/golden.py` renders scripted noise (angle sweeps through 90°, a Hi→Lo glide
  reversed half way, Constant through 90° and 180°) at 44.1/48/96 kHz into `tests/golden/*.f32`; `tests/dsp/GoldenTests.cpp`
  matches them within 1e-6 with misaligned host blocks.
- [x] Benchmark against the 2.6 budgets (measured in 2.6).
- **Done when:** all tests pass, within budget, and it sounds right in a DAW with a temporary generic editor. **Left:
  the user's listening in a DAW** (the real panel is in place, so no generic editor is needed).

### M3: UI
- [x] `tools/build_assets.sh`, `Layout.h`, the Assets cache, fonts, all components in 4.3/4.4, resizing, tooltips, readout editing, unit switching (2026-10-05).
  - Checked: snapshots at 75/100/150/200% and 100% Retina (`PhaseAlignTests "[snapshot]"`); every control within
    1 px of `full example.png` at 200% (the example's knob angles, raised buttons and lit ANALYSE are illustrative).
    On screen, the idle editor painted nothing in 2 s with the meter on (`PhaseAlignTests "[desktop]"`), and a value
    change repainted the panel but not the meter screen. Gestures are covered by `tests/InteractionTests.cpp`.
  - Choices to confirm: ANALYSE stays unlit in v0.7 (plan 4.3) although the CSV has lit images "when meter is on";
    the meter defaults to on; METER OFF shows a dark screen with a dim "METER OFF"; idle shows the gated bars as
    unlit segments with "NO SIDECHAIN SIGNAL"; 16 provisional 2/3-octave bands (P4 decides); the readout ghosts are
    `8.88` (ms), `888` (samples), `888.8` (cm, phase); the selected switch label is white with a glow, the others
    dim grey.
  - Art update (2026-10-05): switches moved (delay y 280, phase y 352), RANGE button (no LED) above the phase switch,
    and the phase knob's upper scale label drawn in code (`ui/ScaleLabel.h`): "90°" or "180°", in Arial Hebrew as in
    the art (Arial elsewhere), placed by ink bounds to match the baked "0°" within 1 px.
  - Phase readout (user, 2026-10-06): shows the total rotation, θ + 180 while the polarity is inverted (so 0–360),
    with typed values taken the same way. Its units digit (second digit from the right) is centred in the display
    and extra digits grow to the left; the delay readout stays right-aligned against its unit suffix.
- **Done when:** it matches `full example.png` at 75/100/200%, is sharp on Retina at 100%, and the static panel causes no repaints while idle.

### M4: Meter
- [x] **Built (2026-10-06):** MeterCapture, CorrelationAnalyser, MeterScreen (OFF / NO SIDECHAIN SIGNAL / METERING, two
  views per section 3), gating on editor showing AND meter on. Tests: `tests/dsp/MeterTests.cpp` (delayed copy reads
  +1 aligned at 44.1–192 kHz, unaligned follows cos 2πfd, PHAT finds the delay, inversion, gate, silence timing,
  settling, FIFO); `tests/ProcessorTests.cpp` (the done-when through processBlock; no capture with the editor closed
  or hidden); `PhaseAlignTests "[desktop]"` (on screen: metering, +1 once the delay knob aligns, panel not repainted,
  NO SIDECHAIN after 1 s of silence with no further repaints, capture stops on meter off, hidden window and close;
  writes `meter_*.png` snapshots).
- [x] Latency alignment of the meter streams (2026-10-06, with the −4..+4 ms delay); Constant only raises its maximum.
- [x] The Instruments check (2026-10-06): `tools/meter_profile.py` records `PhaseAlignTests "[profile]"` (about 10 s of
  audio with a live sidechain, in real time) with the Time Profiler in four conditions and counts the samples whose
  stack holds a meter function, over the steady part of each run (Debug build, M1 Pro):

  | condition | samples (s) | CorrelationAnalyser | MeterScreen | MeterCapture | processBlock |
  |---|---|---|---|---|---|
  | metering (the control) | 2116 (11.6) | 331 | 448 | 57 | 757 |
  | editor showing, meter off | 1093 (11.5) | 0 | 0 | 0 | 646 |
  | window hidden | 963 (11.3) | 0 | 0 | 0 | 618 |
  | editor closed | 828 (11.2) | 0 | 0 | 1 | 666 |

  The one sample when closed is `MeterCapture::isActive()`, the per-block gate itself (not inlined in Debug). So
  nothing of the meter runs while it is off, hidden or closed; the counters in the tests say the same.
- Checked (2026-10-06): a delayed copy reads +1 once aligned in Hi, Lo and Constant (`ProcessorTests`, M4 case).
- **Done when:** a delayed copy as sidechain reads +1 once aligned, in all three modes; nothing runs when the meter is off or the editor is closed (confirm in Instruments).

### M5: Hardening → v0.7
- [ ] auval + pluginval (strictness 10). In CI on macOS (auval passed once, on the early skeleton; pluginval has never
  run, since the commits that added it and ctest were never pushed). CI also prints the DSP benchmark on every OS now.
- [x] Robustness tests (2026-10-06, `tests/RobustnessTests.cpp`): mono buses in every mode, Constant included (equal to a
  stereo instance's left channel, with or without a sidechain); a sample-rate change while running (no
  `releaseResources`); any host block size, larger than prepared, with the meter on or off, in every mode (bit-equal);
  offline rendering in one 96000-sample block; host bypass toggled during a latency switch; a mode change during a
  delay crossfade; state recalled into a running instance switching it to Constant (click-free, then equal to a fresh
  instance); no allocation in `processBlock` or `processBlockBypassed` in every mode with automation and the meter on
  (counted per thread). The click checks were shown to fail with a hard latency switch.
- [ ] Hosts: Logic, Live, Reaper, Cubase/Nuendo, Studio One, Bitwig. Check sidechain routing, automation, state recall, sample-rate changes, offline bounce, mono/stereo, **latency changes when entering or leaving Constant**, and host bypass in Constant.
- [ ] Release checklist from PLAN section 10 (separate arm64 and Intel builds (R14), notarisation, JUCE licence tier,
  OFL font notices). Done 2026-10-06, needing no credentials:
  - **Both macOS architectures:** built once with both slices to check them (`lipo -info`: x86_64 + arm64 for the AU,
    the VST3 and the DSP runner); the DSP tests pass on arm64 and as x86_64 under Rosetta (the code the Intel build
    ships: the SSE2 paths of `Simd.h`, PFFFT's x86 build), 31 test cases each. Releases stay separate builds (R14);
    `release.yml`'s arm64 job now states its architecture.
  - **Licence: GNU AGPLv3** (user, 2026-10-06; `LICENSE`, the FSF's text as JUCE ships it). JUCE is used under its
    AGPLv3 option, so no JUCE licence tier needs buying.
  - **Notices:** `installer/stage_docs.sh` writes `Readme.txt` (the README's quick start, the AGPLv3 source pointer)
    and `licenses/` (the AGPLv3, PFFFT's licence, both fonts' OFL, `THIRD_PARTY_NOTICES.md`). Every installer and
    every release zip ships them, beside the plugins rather than inside the signed bundles: macOS
    `/Library/Application Support/Phase Align` plus a readme page in the .pkg (checked locally: built, expanded, the
    docs package and the readme page present), Windows `Program Files\Phase Align` (removed by the uninstaller), Linux
    `/usr/share/doc/phasealign`. The Windows and Linux packaging is unverified until the release workflow runs.
  - **`release.yml` review** (read, not run): (1) the release builds turn the tests off, so a release can be cut from a
    commit whose CI failed: run it only on a green commit, or have it check CI's status first (the user's call);
    (2) the zips lacked the notices (fixed above); (3) Windows and Linux binaries are unsigned (needs credentials);
    (4) `codesign --deep` for signing is deprecated by Apple (works; signing nested code first is the modern way).
    The separate arm64 and Intel jobs are as intended (R14). Its Linux build is pinned to Ubuntu 24.04 as in CI.
  - **Signing (user, 2026-10-06):** macOS notarisation is covered: the Apple secrets are set in the repo, and
    `release.yml` signs and notarises both macOS builds and their .pkg (confirmed on the first release run). Windows
    and Linux ship unsigned (no credentials; none needed).
  - Left: the first release run, the host checks.
- [ ] **Quick-start guide** (README section + installer readme). The essential point: the plugin can only *delay*, so it goes on the track that arrives **earlier** (usually the closer mic), with the later track as the sidechain. Also covers routing a sidechain in the major hosts, what the meter's two markers mean, and that Constant mode adds compensated latency.
- [x] Name check: web search (2026-10-06) found no audio product called "Phase Align"; "phase alignment" is common as a
  description of other products. **No trademark search (user, 2026-10-06): open source.**
- [ ] Quick-start guide: **drafted in README (2026-10-06)** with the −4..+4 ms delay (it can move a track either way, so
  "only delay" no longer applies); host routing steps to be checked in each host; the installer readme is generated
  from it (`installer/stage_docs.sh`), draft note included until the routing is checked.

### v0.8: Record and compare
- Capture into a lazily allocated buffer (allocated on the message thread when ANALYSE is pressed). Analyse on a background thread with the same `Chain` code.
- GCC-PHAT over **±4 ms**. A negative peak means this track is already behind the sidechain: say "put Phase Align on the other track" instead of clamping.
- Constant mode is now a true rotation, so the closed form `θ ≈ atan2(corr(sc, Q), corr(sc, I))` is accurate, with I = the delayed dry signal. Use it directly, then refine by scoring the chain offline.

---

## 6. Remaining unknowns (where the real work is)

Most of this plan is standard plugin engineering. The parts that could need iteration are:

1. **Hi/Lo knob feel.** Tuning by ear on real stems (P1/P5).
2. **The meter's visual design** (PLAN open question 3): proposed after P4 (section 3: frequency curve + time-offset
   view + overall pair); awaiting the user's view on it in a DAW.
3. **Constant-mode latency changes across hosts.** The code is simple, but host behaviour varies, so M5 testing decides whether the fade-and-prime approach is enough.

## 7. Still needed from you

- **Real multi-mic stem pairs** for P2/P3 listening: kick in/out, snare top/bottom, bass DI/amp, guitar close/room.
- Work that needs no input from the user, and what is waiting on the user, is in `HANDOVER.md`.
