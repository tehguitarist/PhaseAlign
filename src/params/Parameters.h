#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <optional>

// Parameter IDs, ranges and text conversions (IMPLEMENTATION_PLAN 1.2). The IDs are permanent: never
// rename one, and add new parameters with a new version number instead of changing an old one.
namespace pa::params
{
inline constexpr int version = 1; // juce::ParameterID version hint for every parameter below

namespace id
{
inline constexpr const char* delayMs = "delayMs";
inline constexpr const char* delayOn = "delayOn";
inline constexpr const char* phase = "phase";           // knob position, 0 to 1
inline constexpr const char* phaseRange = "phaseRange"; // false = 90, true = 180
inline constexpr const char* phaseMode = "phaseMode";
inline constexpr const char* polarity = "polarity";
inline constexpr const char* phaseOn = "phaseOn";
} // namespace id

// The delay knob runs from -maxDelayMs to +maxDelayMs in steps of 0.1 sample (plan 2.1a). While the delay is on, the
// plugin reports latency to cover the negative half (pa::dsp::Chain::latencyFor) and delays by that plus the knob's
// value, so a negative setting moves the track earlier.
inline constexpr float maxDelayMs = 4.0f;
inline constexpr float maxPhaseDeg = 180.0f;
inline constexpr double maxShownPhaseDeg = 360.0; // the readout adds 180 while the polarity is inverted

// RANGE sets what the knob's full travel means, like the hardware: the knob keeps its position, so switching
// range doubles or halves the angle. Angle = phase x range.
inline constexpr double rangeDegrees(bool range180)
{
    return range180 ? 180.0 : 90.0;
}

// Choice index = switch position: up, centre, down.
enum class PhaseMode
{
    high,
    low,
    constant
};

juce::StringArray phaseModeNames();

// What the phase knob's number means, on the panel and in the host's text. The knob keeps its position through a RANGE
// change; the number is the shift at the first section's reference frequency, which is 0-90 at RANGE 90. At RANGE 180
// Low's two stacked sections and Constant's true rotation read 0-180 exactly, but High's second section sits an octave
// or more above the first and adds its own turn there, so High shows the first section's angle, 0-90, marked with an
// asterisk (IMPLEMENTATION_PLAN 2.3).
inline constexpr double shownRangeDegrees(bool range180, PhaseMode mode)
{
    return range180 && mode != PhaseMode::high ? 180.0 : 90.0;
}
inline constexpr bool shownRangeIsApproximate(bool range180, PhaseMode mode)
{
    return range180 && mode == PhaseMode::high;
}

// currentRangeDegrees gives the RANGE setting (90 or 180) for the phase parameter's host text.
juce::AudioProcessorValueTreeState::ParameterLayout createLayout(std::function<double()> currentRangeDegrees);

//==============================================================================
// Delay units. The delay is stored in ms and applied in tenths of a sample (plan 2.1a; replaced whole samples,
// decision 12, user 2026-10-06), so everything shown to the user is the effective, rounded value.
enum class DelayUnit
{
    ms,
    samples,
    cm
};

inline constexpr double speedOfSoundMetresPerSecond = 343.0;

juce::String toString(DelayUnit);                   // "ms" / "samples" / "cm" (stored in state)
DelayUnit delayUnitFromString(const juce::String&); // unknown text gives ms
juce::String unitSuffix(DelayUnit);                 // "ms" / "samp" / "cm" (readout)

inline constexpr int delaySteps = 10;                              // per sample: steps of 0.1 sample
int maxDelayTenths(double sampleRate);                             // round(4 ms * fs * 10): the knob's reach either way
int maxDelaySamples(double sampleRate);                            // the reach rounded up to whole samples
int delayInTenths(double ms, double sampleRate);                   // round(ms * fs / 100), clamped to the reach
double delayInSamples(double ms, double sampleRate);               // the same, in samples (one decimal)
double effectiveDelayMs(double ms, double sampleRate);             // the delay actually applied, in ms
double toUnit(double ms, DelayUnit, double sampleRate);            // effective delay in the unit
juce::String formatDelay(double ms, DelayUnit, double sampleRate); // digits only: "1.234", "-59.3", "42.1"

// Parses typed text in the given unit; a unit typed after the number ("2ms", "60 samp", "40cm")
// overrides it. A minus sign may be ASCII or U+2212. Returns ms clamped to the range, or nothing if the text has no
// number.
std::optional<double> parseDelay(const juce::String&, DelayUnit, double sampleRate);

juce::String formatPhase(double degrees);                                   // "90.0"
std::optional<double> parsePhase(const juce::String&, double rangeDegrees); // degrees, clamped to the range

// One wheel or arrow-key step (IMPLEMENTATION_PLAN 4.4): one sample (0.1 sample if fine), through zero, keeping the
// fraction; or the next 0.5 degree.
double stepDelayMs(double ms, int direction, double sampleRate, bool fine = false);
double stepPhaseDeg(double degrees, int direction, double rangeDegrees);
} // namespace pa::params
