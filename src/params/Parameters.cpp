#include "params/Parameters.h"

namespace pa::params
{
namespace
{
const juce::String degreeSign = juce::String::fromUTF8("\xc2\xb0");

juce::ParameterID pid(const char* name)
{
    return {name, version};
}

// Typed or pasted minus signs (U+2212 and the en dash) as ASCII.
juce::String asciiMinus(const juce::String& text)
{
    return text.replace(juce::String::fromUTF8("\xe2\x88\x92"), "-")
        .replace(juce::String::fromUTF8("\xe2\x80\x93"), "-");
}

// The first number in the text, or nothing. Accepts a leading sign and one decimal point.
std::optional<double> leadingNumber(const juce::String& text)
{
    const auto t = asciiMinus(text).trim();
    const auto numeric = t.initialSectionContainingOnly("+-.0123456789");
    if (! numeric.containsAnyOf("0123456789"))
        return std::nullopt;
    return numeric.getDoubleValue();
}

std::optional<DelayUnit> typedUnit(const juce::String& text)
{
    const auto rest = asciiMinus(text).trim().trimCharactersAtStart("+-.0123456789").trim().toLowerCase();
    if (rest.isEmpty())
        return std::nullopt;
    if (rest.startsWith("ms") || rest.startsWith("milli"))
        return DelayUnit::ms;
    if (rest.startsWith("s")) // samp, samples, smp
        return DelayUnit::samples;
    if (rest.startsWith("c")) // cm, centimetres
        return DelayUnit::cm;
    return std::nullopt;
}

double cmPerMs()
{
    return speedOfSoundMetresPerSecond / 10.0;
} // 343 m/s = 34.3 cm/ms

// Fixed decimals, with no "-0.0" when a small negative value rounds to zero.
juce::String fixed(double value, int decimals)
{
    const auto scale = std::pow(10.0, decimals);
    const auto rounded = std::round(value * scale) / scale;
    return juce::String(juce::exactlyEqual(rounded, 0.0) ? 0.0 : rounded, decimals);
}
} // namespace

juce::StringArray phaseModeNames()
{
    return {"High", "Low", "Constant"};
}

juce::AudioProcessorValueTreeState::ParameterLayout createLayout(std::function<double()> currentRangeDegrees)
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add(std::make_unique<AudioParameterFloat>(
        pid(id::delayMs), "Delay", NormalisableRange<float>(-maxDelayMs, maxDelayMs), 0.0f,
        AudioParameterFloatAttributes()
            .withLabel("ms")
            .withStringFromValueFunction([](float v, int) { return fixed(v, 3) + " ms"; })
            .withValueFromStringFunction([](const String& s) { return (float)leadingNumber(s).value_or(0.0); })));

    // Not automatable, off by default: switching the delay on or off changes the reported latency.
    layout.add(std::make_unique<AudioParameterBool>(pid(id::delayOn), "Delay On", false,
                                                    AudioParameterBoolAttributes().withAutomatable(false)));

    // The knob position; the host sees it as the angle at the current range.
    layout.add(std::make_unique<AudioParameterFloat>(
        pid(id::phase), "Phase", NormalisableRange<float>(0.0f, 1.0f), 0.0f,
        AudioParameterFloatAttributes()
            .withLabel(degreeSign)
            .withStringFromValueFunction([currentRangeDegrees](float v, int)
                                         { return String(v * currentRangeDegrees(), 1) + degreeSign; })
            .withValueFromStringFunction(
                [currentRangeDegrees](const String& s)
                {
                    const auto range = currentRangeDegrees();
                    return (float)(parsePhase(s, range).value_or(0.0) / range);
                })));

    layout.add(std::make_unique<AudioParameterBool>(
        pid(id::phaseRange), "Phase Range", false,
        AudioParameterBoolAttributes()
            // The button is pressed in for the wider range (two sections, 0 to 180) and out for the finer one (one
            // section, 0 to 90). Typed text also accepts the old "180" and "90".
            .withStringFromValueFunction([](bool in, int) { return in ? "In" : "Out"; })
            .withValueFromStringFunction(
                [](const String& s)
                {
                    const auto t = s.trim().toLowerCase();
                    return t.startsWith("in") || t.startsWith("180") || t == "on" || t == "1";
                })));

    // Not automatable: leaving or entering Constant changes the reported latency.
    layout.add(std::make_unique<AudioParameterChoice>(pid(id::phaseMode), "Phase Mode", phaseModeNames(), 0,
                                                      AudioParameterChoiceAttributes().withAutomatable(false)));

    layout.add(std::make_unique<AudioParameterBool>(pid(id::polarity), "Polarity Invert", false));
    layout.add(std::make_unique<AudioParameterBool>(pid(id::phaseOn), "Phase On", true));

    return layout;
}

//==============================================================================
juce::String toString(DelayUnit u)
{
    switch (u)
    {
    case DelayUnit::samples:
        return "samples";
    case DelayUnit::cm:
        return "cm";
    case DelayUnit::ms:
        break;
    }
    return "ms";
}

DelayUnit delayUnitFromString(const juce::String& s)
{
    if (s == "samples")
        return DelayUnit::samples;
    if (s == "cm")
        return DelayUnit::cm;
    return DelayUnit::ms;
}

juce::String unitSuffix(DelayUnit u)
{
    switch (u)
    {
    case DelayUnit::samples:
        return "samp";
    case DelayUnit::cm:
        return "cm";
    case DelayUnit::ms:
        break;
    }
    return "ms";
}

int maxDelayTenths(double sampleRate)
{
    return (int)std::lround(maxDelayMs * sampleRate * delaySteps / 1000.0);
}

int maxDelaySamples(double sampleRate)
{
    return (maxDelayTenths(sampleRate) + delaySteps - 1) / delaySteps;
}

int delayInTenths(double ms, double sampleRate)
{
    const auto reach = maxDelayTenths(sampleRate);
    return juce::jlimit(-reach, reach, (int)std::lround(ms * sampleRate * delaySteps / 1000.0));
}

double delayInSamples(double ms, double sampleRate)
{
    return delayInTenths(ms, sampleRate) / (double)delaySteps;
}

double effectiveDelayMs(double ms, double sampleRate)
{
    return delayInSamples(ms, sampleRate) * 1000.0 / sampleRate;
}

double toUnit(double ms, DelayUnit u, double sampleRate)
{
    switch (u)
    {
    case DelayUnit::samples:
        return delayInSamples(ms, sampleRate);
    case DelayUnit::cm:
        return effectiveDelayMs(ms, sampleRate) * cmPerMs();
    case DelayUnit::ms:
        break;
    }
    return effectiveDelayMs(ms, sampleRate);
}

juce::String formatDelay(double ms, DelayUnit u, double sampleRate)
{
    switch (u)
    {
    case DelayUnit::samples:
        return fixed(delayInSamples(ms, sampleRate), 1);
    case DelayUnit::cm:
        return fixed(toUnit(ms, u, sampleRate), 1);
    case DelayUnit::ms:
        break;
    }
    return fixed(effectiveDelayMs(ms, sampleRate), 3);
}

std::optional<double> parseDelay(const juce::String& text, DelayUnit unit, double sampleRate)
{
    const auto value = leadingNumber(text);
    if (! value)
        return std::nullopt;

    double ms = *value;
    switch (typedUnit(text).value_or(unit))
    {
    case DelayUnit::samples:
        ms = *value * 1000.0 / sampleRate;
        break;
    case DelayUnit::cm:
        ms = *value / cmPerMs();
        break;
    case DelayUnit::ms:
        break;
    }
    return juce::jlimit(-(double)maxDelayMs, (double)maxDelayMs, ms);
}

juce::String formatPhase(double degrees)
{
    return juce::String(degrees, 1);
}

std::optional<double> parsePhase(const juce::String& text, double rangeDegrees)
{
    if (const auto value = leadingNumber(text))
        return juce::jlimit(0.0, rangeDegrees, *value);
    return std::nullopt;
}

double stepDelayMs(double ms, int direction, double sampleRate, bool fine)
{
    const auto reach = maxDelayTenths(sampleRate);
    const auto tenths =
        juce::jlimit(-reach, reach, delayInTenths(ms, sampleRate) + direction * (fine ? 1 : delaySteps));
    return juce::jlimit(-(double)maxDelayMs, (double)maxDelayMs, tenths * 1000.0 / (sampleRate * delaySteps));
}

double stepPhaseDeg(double degrees, int direction, double rangeDegrees)
{
    // Snap to the 0.5 degree grid first, so a step from 10.3 lands on 10.5 or 10.0.
    const auto grid = std::round(degrees * 2.0) / 2.0;
    auto next = grid + 0.5 * direction;
    if ((direction > 0 && grid > degrees + 1.0e-9) || (direction < 0 && grid < degrees - 1.0e-9))
        next = grid;
    return juce::jlimit(0.0, rangeDegrees, next);
}
} // namespace pa::params
