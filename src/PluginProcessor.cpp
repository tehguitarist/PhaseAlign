#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "params/Parameters.h"

namespace
{
const juce::Identifier stateVersionId{"stateVersion"};
}

PhaseAlignProcessor::PhaseAlignProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)
                         .withInput("Sidechain", juce::AudioChannelSet::stereo(), true)),
      parameters(*this, nullptr, "PhaseAlign", pa::params::createLayout([this] { return getShownPhaseRangeDegrees(); }))
{
    namespace id = pa::params::id;
    phaseRangeValue = parameters.getRawParameterValue(id::phaseRange);
    delayMsValue = parameters.getRawParameterValue(id::delayMs);
    delayOnValue = parameters.getRawParameterValue(id::delayOn);
    phaseValue = parameters.getRawParameterValue(id::phase);
    phaseModeValue = parameters.getRawParameterValue(id::phaseMode);
    polarityValue = parameters.getRawParameterValue(id::polarity);
    phaseOnValue = parameters.getRawParameterValue(id::phaseOn);

    // Parameters that change the latency (plan 2.1a, 2.4).
    delayOnParameter = parameters.getParameter(id::delayOn);
    phaseModeParameter = parameters.getParameter(id::phaseMode);
    delayOnParameter->addListener(this);
    phaseModeParameter->addListener(this);
}

PhaseAlignProcessor::~PhaseAlignProcessor()
{
    delayOnParameter->removeListener(this);
    phaseModeParameter->removeListener(this);
    cancelPendingUpdate();
}

int PhaseAlignProcessor::requiredLatency() const
{
    pa::dsp::ChainSettings s;
    s.delayOn = delayOnParameter->getValue() >= 0.5f;
    s.phaseMode = (pa::dsp::PhaseMode)juce::jlimit(
        0, 2, juce::roundToInt(phaseModeParameter->convertFrom0to1(phaseModeParameter->getValue())));
    const auto fs = sampleRate.load();
    return pa::dsp::Chain::latencyFor(s, fs, pa::params::maxDelayTenths(fs));
}

void PhaseAlignProcessor::parameterValueChanged(int, float)
{
    // Any thread. The audio fades out and back in around the switch by itself (Chain); the host hears about the new
    // latency from the message thread.
    if (juce::MessageManager::existsAndIsCurrentThread())
    {
        cancelPendingUpdate();
        setLatencySamples(requiredLatency());
    }
    else
    {
        triggerAsyncUpdate();
    }
}

void PhaseAlignProcessor::prepareToPlay(double newSampleRate, int)
{
    if (newSampleRate <= 0.0)
        return;
    if (! juce::exactlyEqual(sampleRate.exchange(newSampleRate), newSampleRate))
        sendChangeMessage();

    const auto maxDelayTenths = pa::params::maxDelayTenths(newSampleRate);
    meterCapture.setSampleRate(newSampleRate);
    meterCapture.prepareAlignment(pa::dsp::Chain::maxLatencyFor(newSampleRate, maxDelayTenths),
                                  pa::dsp::Chain::subBlockSize);
    chain.prepare(newSampleRate, getMainBusNumInputChannels(), maxDelayTenths);
    chain.reset(currentSettings());
    setLatencySamples(requiredLatency());
}

pa::dsp::ChainSettings PhaseAlignProcessor::currentSettings() const
{
    pa::dsp::ChainSettings s;
    s.delayTenths = pa::params::delayInTenths(delayMsValue->load(), sampleRate.load());
    s.delayOn = delayOnValue->load() >= 0.5f;
    s.polarityInverted = polarityValue->load() >= 0.5f;
    s.phaseOn = phaseOnValue->load() >= 0.5f;
    s.phaseDegrees = phaseValue->load() * getPhaseRangeDegrees();
    s.phaseWide = phaseRangeValue->load() >= 0.5f;
    s.phaseMode = (pa::dsp::PhaseMode)juce::jlimit(0, 2, juce::roundToInt(phaseModeValue->load()));
    return s;
}

bool PhaseAlignProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto mainOut = layouts.getMainOutputChannelSet();
    if (mainOut != juce::AudioChannelSet::mono() && mainOut != juce::AudioChannelSet::stereo())
        return false;
    if (layouts.getMainInputChannelSet() != mainOut)
        return false;

    const auto sc = layouts.getChannelSet(true, 1);
    return sc.isDisabled() || sc == juce::AudioChannelSet::mono() || sc == juce::AudioChannelSet::stereo();
}

void PhaseAlignProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    runChain(buffer, currentSettings());
}

void PhaseAlignProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    runChain(buffer, currentSettings().bypassed());
}

void PhaseAlignProcessor::runChain(juce::AudioBuffer<float>& buffer, const pa::dsp::ChainSettings& settings)
{
    juce::ScopedNoDenormals noDenormals;
    auto main = getBusBuffer(buffer, false, 0); // in place: the main input shares these channels
    const auto sidechain = getBusBuffer(buffer, true, 1);
    const auto numChannels = main.getNumChannels();
    meterCapture.setSidechainPresent(sidechain.getNumChannels() > 0);
    {
        const auto position = getPlayHead() != nullptr ? getPlayHead()->getPosition() : juce::Optional<juce::AudioPlayHead::PositionInfo>();
        meterCapture.setTransport(position.hasValue(), position.hasValue() && position->getIsPlaying());
    }
    chain.setSettings(settings);

    if (! meterCapture.isActive())
    {
        chain.process(main.getArrayOfWritePointers(), numChannels, main.getNumSamples());
        return;
    }

    // Metering: mono sums of the input, output and sidechain, a sub-block at a time (no scratch to size). The capture
    // delays the input and sidechain by the chain's current latency, so all three line up.
    constexpr int sub = pa::dsp::Chain::subBlockSize;
    float in[sub], out[sub], sc[sub];
    float* channels[pa::dsp::Chain::maxChannels];
    const auto mono = [](const juce::AudioBuffer<float>& b, int start, int n, float* dest)
    {
        std::fill(dest, dest + n, 0.0f);
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            juce::FloatVectorOperations::add(dest, b.getReadPointer(ch, start), n);
        if (b.getNumChannels() > 1)
            juce::FloatVectorOperations::multiply(dest, 1.0f / (float)b.getNumChannels(), n);
    };

    for (int start = 0; start < main.getNumSamples(); start += sub)
    {
        const auto n = std::min(sub, main.getNumSamples() - start);
        mono(main, start, n, in);
        mono(sidechain, start, n, sc);
        for (int ch = 0; ch < std::min(numChannels, pa::dsp::Chain::maxChannels); ++ch)
            channels[ch] = main.getWritePointer(ch, start);
        chain.process(channels, std::min(numChannels, pa::dsp::Chain::maxChannels), n);
        mono(main, start, n, out);
        meterCapture.push(in, out, sc, n, chain.latency());
    }
}

double PhaseAlignProcessor::getPhaseRangeDegrees() const
{
    return pa::params::rangeDegrees(phaseRangeValue != nullptr && phaseRangeValue->load() >= 0.5f);
}

double PhaseAlignProcessor::getShownPhaseRangeDegrees() const
{
    const auto wide = phaseRangeValue != nullptr && phaseRangeValue->load() >= 0.5f;
    const auto mode = (pa::params::PhaseMode)juce::jlimit(
        0, 2, phaseModeValue != nullptr ? juce::roundToInt(phaseModeValue->load()) : 0);
    return pa::params::shownRangeDegrees(wide, mode);
}

bool PhaseAlignProcessor::isSidechainConnected() const
{
    const auto* sc = getBus(true, 1);
    return sc != nullptr && sc->isEnabled();
}

juce::AudioProcessorEditor* PhaseAlignProcessor::createEditor()
{
    return new PhaseAlignEditor(*this);
}

//==============================================================================
juce::ValueTree PhaseAlignProcessor::defaultUiState()
{
    return juce::ValueTree(UiProps::type, {{UiProps::delayUnit, toString(pa::params::DelayUnit::ms)},
                                           {UiProps::meterOn, true},
                                           {UiProps::uiScale, defaultUiScale},
                                           {UiProps::meterView, "frequency"},
                                           {UiProps::meterSpeed, "slow"}});
}

void PhaseAlignProcessor::restoreUiState(const juce::ValueTree& loaded)
{
    // Validate each property and fall back to the default, so an old or hand-edited state can't put
    // the editor into a state it can't show. Properties are set (not the tree replaced) so listeners
    // on uiState stay attached.
    const auto defaults = defaultUiState();
    const auto get = [&](const juce::Identifier& prop) { return loaded.getProperty(prop, defaults[prop]); };

    uiState.setProperty(UiProps::delayUnit,
                        toString(pa::params::delayUnitFromString(get(UiProps::delayUnit).toString())), nullptr);
    uiState.setProperty(UiProps::meterOn, (bool)get(UiProps::meterOn), nullptr);
    uiState.setProperty(UiProps::uiScale, juce::jlimit(minUiScale, maxUiScale, (double)get(UiProps::uiScale)), nullptr);
    const auto view = get(UiProps::meterView).toString();
    uiState.setProperty(UiProps::meterView, view == "time" || view == "phase" ? view : juce::String("frequency"),
                        nullptr);
    uiState.setProperty(UiProps::meterSpeed, get(UiProps::meterSpeed).toString() == "fast" ? "fast" : "slow", nullptr);
}

void PhaseAlignProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    state.setProperty(stateVersionId, stateVersion, nullptr);
    state.removeChild(state.getChildWithName(UiProps::type), nullptr);
    state.appendChild(uiState.createCopy(), nullptr);

    if (const auto xml = state.createXml())
        copyXmlToBinary(*xml, destData);
}

void PhaseAlignProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml == nullptr || ! xml->hasTagName(parameters.state.getType()))
        return;

    auto state = juce::ValueTree::fromXml(*xml);
    // stateVersion 1 is the first format; later versions migrate here, before anything is applied.
    // A state with no version is read as version 1, and one from a newer build loads what we recognise.

    const auto ui = state.getChildWithName(UiProps::type);
    restoreUiState(ui.isValid() ? ui : defaultUiState());
    state.removeChild(ui, nullptr);

    parameters.replaceState(state);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PhaseAlignProcessor();
}
