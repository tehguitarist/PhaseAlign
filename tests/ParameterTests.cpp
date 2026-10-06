#include "PluginProcessor.h"
#include "params/Parameters.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace pa::params;
using Catch::Approx;

TEST_CASE("parameters have their permanent IDs, ranges and defaults", "[params]")
{
    PhaseAlignProcessor proc;
    auto& apvts = proc.getValueTreeState();

    struct Expected
    {
        const char* id;
        float min, max, def;
        bool automatable;
    };
    const Expected expected[] = {
        {id::delayMs, -4.0f, 4.0f, 0.0f, true},   {id::delayOn, 0.0f, 1.0f, 0.0f, false},
        {id::phase, 0.0f, 1.0f, 0.0f, true},      {id::phaseRange, 0.0f, 1.0f, 0.0f, true},
        {id::phaseMode, 0.0f, 2.0f, 0.0f, false}, {id::polarity, 0.0f, 1.0f, 0.0f, true},
        {id::phaseOn, 0.0f, 1.0f, 1.0f, true},
    };

    REQUIRE(proc.getParameters().size() == (int)std::size(expected));

    for (const auto& e : expected)
    {
        INFO(e.id);
        auto* p = apvts.getParameter(e.id);
        REQUIRE(p != nullptr);
        CHECK(p->getVersionHint() == 1);
        CHECK(p->getNormalisableRange().start == e.min);
        CHECK(p->getNormalisableRange().end == e.max);
        CHECK(p->convertFrom0to1(p->getDefaultValue()) == Approx(e.def));
        CHECK(p->isAutomatable() == e.automatable);
    }

    auto* mode = dynamic_cast<juce::AudioParameterChoice*>(apvts.getParameter(id::phaseMode));
    REQUIRE(mode != nullptr);
    CHECK(mode->choices == juce::StringArray{"High", "Low", "Constant"});
}

TEST_CASE("parameter text round-trips through the host's text entry", "[params]")
{
    PhaseAlignProcessor proc;
    auto* delay = proc.getValueTreeState().getParameter(id::delayMs);
    auto* phase = proc.getValueTreeState().getParameter(id::phase);
    auto* range = proc.getValueTreeState().getParameter(id::phaseRange);
    const auto deg = [](const char* text) { return juce::String::fromUTF8(text); };

    CHECK(delay->getText(delay->convertTo0to1(1.25f), 32) == "1.250 ms");
    CHECK(delay->convertFrom0to1(delay->getValueForText("2.5 ms")) == Approx(2.5f));
    CHECK(delay->getText(delay->convertTo0to1(-1.25f), 32) == "-1.250 ms");
    CHECK(delay->convertFrom0to1(delay->getValueForText("-2.5 ms")) == Approx(-2.5f));
    CHECK(delay->convertFrom0to1(delay->getValueForText(juce::String::fromUTF8("\xe2\x88\x92"
                                                                               "2 ms"))) == Approx(-2.0f));

    // The phase parameter is the knob position; the host sees the angle at the current range (RANGE out, 0 to 90, by
    // default). The RANGE button reads Out or In, like the button.
    CHECK(range->getText(0.0f, 32) == "Out");
    CHECK(phase->getText(0.5f, 32) == deg("45.0\xc2\xb0"));
    CHECK(phase->getValueForText("45") == Approx(0.5f));
    CHECK(phase->getValueForText("120") == Approx(1.0f)); // clamped to the range

    // With RANGE in the number depends on the mode: High shows the first section's angle (0-90, marked with an
    // asterisk on the panel), Low and Constant read the full 0-180.
    range->setValueNotifyingHost(1.0f);
    CHECK(range->getText(1.0f, 32) == "In");
    CHECK(range->getValueForText("In") == 1.0f);
    CHECK(range->getValueForText("out") == 0.0f);
    CHECK(range->getValueForText("180") == 1.0f); // the old text still reads
    CHECK(range->getValueForText("90") == 0.0f);
    CHECK(phase->getText(0.5f, 32) == deg("45.0\xc2\xb0")); // High
    CHECK(phase->getValueForText("45") == Approx(0.5f));

    auto* mode = proc.getValueTreeState().getParameter(id::phaseMode);
    for (const auto index : {1, 2}) // Low, Constant
    {
        mode->setValueNotifyingHost(mode->convertTo0to1((float)index));
        CHECK(phase->getText(0.5f, 32) == deg("90.0\xc2\xb0"));
        CHECK(phase->getValueForText("45") == Approx(0.25f));
    }
}

TEST_CASE("what the phase knob's number means, by mode and range", "[params]")
{
    CHECK(shownRangeDegrees(false, PhaseMode::high) == 90.0);
    CHECK(shownRangeDegrees(false, PhaseMode::low) == 90.0);
    CHECK(shownRangeDegrees(false, PhaseMode::constant) == 90.0);
    CHECK(shownRangeDegrees(true, PhaseMode::high) == 90.0); // the first section's angle
    CHECK(shownRangeDegrees(true, PhaseMode::low) == 180.0);
    CHECK(shownRangeDegrees(true, PhaseMode::constant) == 180.0);

    // Only High with RANGE in is approximate.
    for (const auto wide : {false, true})
        for (const auto mode : {PhaseMode::high, PhaseMode::low, PhaseMode::constant})
            CHECK(shownRangeIsApproximate(wide, mode) == (wide && mode == PhaseMode::high));
}

TEST_CASE("the delay is rounded to 0.1 sample and shown as the effective value", "[params]")
{
    CHECK(delayInTenths(1.0, 48000.0) == 480);
    CHECK(delayInSamples(1.0, 48000.0) == 48.0);
    CHECK(delayInSamples(1.01, 48000.0) == 48.5);         // 48.48 rounds to 48.5
    CHECK(delayInSamples(1.0009, 48000.0) == 48.0);       // 48.04 rounds down
    CHECK(delayInSamples(4.0, 44100.0) == Approx(176.4)); // the reach is exactly 4 ms, to 0.1 sample
    CHECK(delayInSamples(4.0, 88200.0) == Approx(352.8));
    CHECK(delayInSamples(4.0, 192000.0) == 768.0);
    CHECK(delayInSamples(-1.0, 48000.0) == -48.0);
    CHECK(delayInSamples(-4.0, 44100.0) == Approx(-176.4));
    CHECK(delayInSamples(-9.0, 48000.0) == -192.0); // clamped
    CHECK(maxDelayTenths(44100.0) == 1764);
    CHECK(maxDelaySamples(44100.0) == 177); // rounded up: the latency must cover the whole reach
    CHECK(maxDelaySamples(88200.0) == 353);
    CHECK(maxDelaySamples(192000.0) == 768);
    CHECK(effectiveDelayMs(4.0, 44100.0) == Approx(4.0));

    CHECK(formatDelay(1.0, DelayUnit::ms, 44100.0) == "1.000"); // 44.1 samples exactly
    CHECK(formatDelay(1.0, DelayUnit::ms, 48000.0) == "1.000");
    CHECK(formatDelay(1.001, DelayUnit::ms, 48000.0) == "1.000"); // 48.048 samples rounds to 48.0
    CHECK(formatDelay(1.002, DelayUnit::ms, 48000.0) == "1.002"); // 48.1 samples = 1.00208 ms
    CHECK(formatDelay(0.5, DelayUnit::samples, 48000.0) == "24.0");
    CHECK(formatDelay(0.5021, DelayUnit::samples, 48000.0) == "24.1");
    CHECK(formatDelay(1.0, DelayUnit::cm, 48000.0) == "34.3");     // 343 m/s
    CHECK(formatDelay(0.001, DelayUnit::ms, 48000.0) == "0.000");  // under half a step rounds to 0
    CHECK(formatDelay(-0.001, DelayUnit::ms, 48000.0) == "0.000"); // no "-0.000"
    CHECK(formatDelay(-0.0005, DelayUnit::cm, 192000.0) == "0.0"); // -0.1 sample is -0.02 cm: no "-0.0"
    CHECK(formatDelay(-1.0, DelayUnit::ms, 44100.0) == "-1.000");
    CHECK(formatDelay(-0.5, DelayUnit::samples, 48000.0) == "-24.0");
    CHECK(formatDelay(-1.0, DelayUnit::cm, 48000.0) == "-34.3");
    CHECK(formatDelay(-4.0, DelayUnit::samples, 44100.0) == "-176.4");

    CHECK(unitSuffix(DelayUnit::samples) == "samp");
    CHECK(delayUnitFromString(toString(DelayUnit::cm)) == DelayUnit::cm);
    CHECK(delayUnitFromString("nonsense") == DelayUnit::ms);
}

TEST_CASE("typed delays convert to ms in the current unit, or the unit typed", "[params]")
{
    const double fs = 48000.0;
    CHECK(*parseDelay("1.5", DelayUnit::ms, fs) == Approx(1.5));
    CHECK(*parseDelay("96", DelayUnit::samples, fs) == Approx(2.0));
    CHECK(*parseDelay("34.3", DelayUnit::cm, fs) == Approx(1.0));
    CHECK(*parseDelay("48 samp", DelayUnit::ms, fs) == Approx(1.0));
    CHECK(*parseDelay("2ms", DelayUnit::cm, fs) == Approx(2.0));
    CHECK(*parseDelay("68.6 cm", DelayUnit::samples, fs) == Approx(2.0));
    CHECK(*parseDelay("9", DelayUnit::ms, fs) == Approx(4.0)); // clamped
    CHECK(*parseDelay("-3", DelayUnit::ms, fs) == Approx(-3.0));
    CHECK(*parseDelay("-9", DelayUnit::ms, fs) == Approx(-4.0)); // clamped
    CHECK(*parseDelay("-48 samp", DelayUnit::ms, fs) == Approx(-1.0));
    CHECK(delayInSamples(*parseDelay("59.3", DelayUnit::samples, fs), fs) == Approx(59.3));
    CHECK(*parseDelay(juce::String::fromUTF8("\xe2\x88\x92"
                                             "1.5"),
                      DelayUnit::ms, fs) == Approx(-1.5)); // U+2212
    CHECK(*parseDelay(juce::String::fromUTF8("\xe2\x88\x92"
                                             "34.3cm"),
                      DelayUnit::ms, fs) == Approx(-1.0));
    CHECK_FALSE(parseDelay("abc", DelayUnit::ms, fs).has_value());
    CHECK_FALSE(parseDelay("", DelayUnit::ms, fs).has_value());

    CHECK(*parsePhase(juce::String::fromUTF8("90\xc2\xb0"), 180.0) == Approx(90.0));
    CHECK(*parsePhase("200", 180.0) == Approx(180.0));
    CHECK(*parsePhase("120", 90.0) == Approx(90.0));
    CHECK_FALSE(parsePhase("x", 180.0).has_value());
}

TEST_CASE("wheel and arrow steps move by one sample (0.1 fine) or half a degree", "[params]")
{
    const double fs = 48000.0;
    CHECK(stepDelayMs(0.0, 1, fs) == Approx(1000.0 / fs));
    CHECK(stepDelayMs(1.0, -1, fs) == Approx(47 * 1000.0 / fs));
    CHECK(stepDelayMs(0.0, -1, fs) == Approx(-1000.0 / fs)); // through zero
    CHECK(stepDelayMs(-1000.0 / fs, 1, fs) == Approx(0.0));
    CHECK(stepDelayMs(4.0, 1, fs) == Approx(4.0));
    CHECK(stepDelayMs(-4.0, -1, fs) == Approx(-4.0));
    CHECK(delayInSamples(stepDelayMs(-4.0, 1, fs), fs) == -191.0);

    // Fine: 0.1 sample. A whole step keeps the fraction.
    CHECK(delayInSamples(stepDelayMs(0.0, 1, fs, true), fs) == Approx(0.1));
    CHECK(delayInSamples(stepDelayMs(0.0, -1, fs, true), fs) == Approx(-0.1));
    CHECK(delayInSamples(stepDelayMs(59.3 / 48.0, 1, fs), fs) == Approx(60.3));
    CHECK(delayInSamples(stepDelayMs(59.3 / 48.0, -1, fs, true), fs) == Approx(59.2));
    CHECK(stepDelayMs(4.0, 1, 44100.0, true) == Approx(4.0)); // the reach is a whole number of tenths

    CHECK(stepPhaseDeg(10.0, 1, 180.0) == Approx(10.5));
    CHECK(stepPhaseDeg(10.3, 1, 180.0) == Approx(10.5)); // snaps to the grid first
    CHECK(stepPhaseDeg(10.3, -1, 180.0) == Approx(10.0));
    CHECK(stepPhaseDeg(10.5, -1, 180.0) == Approx(10.0));
    CHECK(stepPhaseDeg(180.0, 1, 180.0) == Approx(180.0));
    CHECK(stepPhaseDeg(90.0, 1, 90.0) == Approx(90.0));
    CHECK(stepPhaseDeg(0.0, -1, 90.0) == Approx(0.0));
}
