#include "PluginProcessor.h"
#include "params/Parameters.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace pa::params;
using Catch::Approx;
using UiProps = PhaseAlignProcessor::UiProps;

namespace
{
void setParam(PhaseAlignProcessor& p, const char* paramId, float value)
{
    auto* param = p.getValueTreeState().getParameter(paramId);
    param->setValueNotifyingHost(param->convertTo0to1(value));
}

float getParam(PhaseAlignProcessor& p, const char* paramId)
{
    auto* param = p.getValueTreeState().getParameter(paramId);
    return param->convertFrom0to1(param->getValue());
}

juce::MemoryBlock save(PhaseAlignProcessor& p)
{
    juce::MemoryBlock block;
    p.getStateInformation(block);
    return block;
}

void load(PhaseAlignProcessor& p, const juce::MemoryBlock& block)
{
    p.setStateInformation(block.getData(), (int)block.getSize());
}
} // namespace

TEST_CASE("state round-trips every parameter and the ui settings", "[state]")
{
    PhaseAlignProcessor a;
    setParam(a, id::delayMs, -2.25f);
    setParam(a, id::delayOn, 1.0f);
    setParam(a, id::phase, 0.75f);
    setParam(a, id::phaseRange, 1.0f);
    setParam(a, id::phaseMode, 2.0f);
    setParam(a, id::polarity, 1.0f);
    setParam(a, id::phaseOn, 0.0f);
    a.getUiState().setProperty(UiProps::delayUnit, "cm", nullptr);
    a.getUiState().setProperty(UiProps::meterOn, false, nullptr);
    a.getUiState().setProperty(UiProps::uiScale, 1.5, nullptr);
    a.getUiState().setProperty(UiProps::meterView, "vector", nullptr);
    a.getUiState().setProperty(UiProps::showInput, false, nullptr);
    a.getUiState().setProperty(UiProps::vectorStereo, true, nullptr);

    PhaseAlignProcessor b;
    const auto uiTree = b.getUiState(); // the editor holds this handle; loading must not replace it
    load(b, save(a));

    CHECK(getParam(b, id::delayMs) == Approx(-2.25f));
    CHECK(getParam(b, id::delayOn) == 1.0f);
    // Loading the delay on (and Constant) reports their latency.
    CHECK(b.getLatencySamples() == pa::dsp::Chain::delayLatencyFor(48000.0, maxDelayTenths(48000.0)) +
                                       pa::dsp::ConstantRotator::latencyFor(48000.0)); // and Constant's
    CHECK(getParam(b, id::phase) == Approx(0.75f));
    CHECK(getParam(b, id::phaseRange) == 1.0f);
    CHECK(getParam(b, id::phaseMode) == 2.0f);
    CHECK(getParam(b, id::polarity) == 1.0f);
    CHECK(getParam(b, id::phaseOn) == 0.0f);

    CHECK(b.getUiState() == uiTree);
    CHECK(b.getUiState()[UiProps::delayUnit].toString() == "cm");
    CHECK((bool)b.getUiState()[UiProps::meterOn] == false);
    CHECK((double)b.getUiState()[UiProps::uiScale] == Approx(1.5));
    CHECK(b.getUiState()[UiProps::meterView].toString() == "vector");
    CHECK_FALSE((bool)b.getUiState()[UiProps::showInput]);
    CHECK((bool)b.getUiState()[UiProps::showOutput]); // not touched: still on
    CHECK((bool)b.getUiState()[UiProps::vectorStereo]);

    // And a second trip gives identical bytes.
    CHECK(save(b) == save(a));
}

TEST_CASE("saved state carries stateVersion and a ui child", "[state]")
{
    PhaseAlignProcessor p;
    const auto block = save(p);
    const auto xml = juce::AudioProcessor::getXmlFromBinary(block.getData(), (int)block.getSize());
    REQUIRE(xml != nullptr);

    const auto tree = juce::ValueTree::fromXml(*xml);
    CHECK((int)tree.getProperty("stateVersion") == PhaseAlignProcessor::stateVersion);
    CHECK(tree.getChildWithName(UiProps::type).isValid());
    CHECK(tree.getNumChildren() == 7 + 1); // one per parameter, plus ui
}

TEST_CASE("loading tolerates missing, bad and foreign state", "[state]")
{
    PhaseAlignProcessor p;
    setParam(p, id::phase, 0.25f);
    p.getUiState().setProperty(UiProps::delayUnit, "samples", nullptr);

    SECTION("garbage is ignored")
    {
        const char junk[] = "not a plugin state";
        p.setStateInformation(junk, (int)sizeof(junk));
        CHECK(getParam(p, id::phase) == Approx(0.25f));
        CHECK(p.getUiState()[UiProps::delayUnit].toString() == "samples");
    }

    SECTION("a state without a ui child resets the ui settings to defaults")
    {
        juce::ValueTree tree("PhaseAlign");
        juce::MemoryBlock block;
        juce::AudioProcessor::copyXmlToBinary(*tree.createXml(), block);
        load(p, block);
        CHECK(p.getUiState()[UiProps::delayUnit].toString() == "ms");
        CHECK((bool)p.getUiState()[UiProps::meterOn] == true);
        CHECK((double)p.getUiState()[UiProps::uiScale] == Approx(PhaseAlignProcessor::defaultUiScale));
        CHECK(p.getUiState()[UiProps::meterView].toString() == "bands");
    }

    SECTION("out-of-range ui values are clamped or replaced")
    {
        juce::ValueTree tree("PhaseAlign");
        tree.appendChild(
            juce::ValueTree(UiProps::type,
                            {{UiProps::delayUnit, "furlongs"}, {UiProps::uiScale, 9.0}, {UiProps::meterView, "x"}}),
            nullptr);
        juce::MemoryBlock block;
        juce::AudioProcessor::copyXmlToBinary(*tree.createXml(), block);
        load(p, block);
        CHECK(p.getUiState()[UiProps::delayUnit].toString() == "ms");
        CHECK((double)p.getUiState()[UiProps::uiScale] == Approx(PhaseAlignProcessor::maxUiScale));
        CHECK(p.getUiState()[UiProps::meterView].toString() == "bands");
    }

    SECTION("a state from before BANDS was the default (FREQUENCY, TIME or PHASE) opens on BANDS")
    {
        for (const auto* old : {"frequency", "time", "phase"})
        {
            juce::ValueTree tree("PhaseAlign");
            tree.appendChild(juce::ValueTree(UiProps::type, {{UiProps::meterView, old}}), nullptr);
            juce::MemoryBlock block;
            juce::AudioProcessor::copyXmlToBinary(*tree.createXml(), block);
            load(p, block);
            CHECK(p.getUiState()[UiProps::meterView].toString() == "bands");
        }
    }

    SECTION("another plugin's state is ignored")
    {
        juce::ValueTree tree("SomethingElse");
        juce::MemoryBlock block;
        juce::AudioProcessor::copyXmlToBinary(*tree.createXml(), block);
        load(p, block);
        CHECK(getParam(p, id::phase) == Approx(0.25f));
    }
}
