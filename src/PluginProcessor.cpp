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

    // Parameters that change the latency.
    delayOnParameter = parameters.getParameter(id::delayOn);
    phaseModeParameter = parameters.getParameter(id::phaseMode);
    delayOnParameter->addListener(this);
    phaseModeParameter->addListener(this);

    pa::analyse::Session::Host host;
    host.sampleRate = [this] { return sampleRate.load(); };
    host.scope = [this]
    {
        const auto s = panelSettings();
        return pa::analyse::Scope{s.delayOn, s.phaseOn};
    };
    host.settings = [this] { return panelSettings(); };
    host.apply = [this](const pa::analyse::PanelSettings& s) { applyPanelSettings(s); };
    host.sidechainPresent = [this] { return meterCapture.hasSidechain(); };
    host.transportKnown = [this] { return meterCapture.isTransportKnown(); };
    host.transportPlaying = [this] { return meterCapture.isTransportPlaying(); };
    analyseSession = std::make_unique<pa::analyse::Session>(analyseCapture, std::move(host));
}

PhaseAlignProcessor::~PhaseAlignProcessor()
{
    analyseSession.reset(); // stops its search thread and the capture first
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

pa::analyse::PanelSettings PhaseAlignProcessor::panelSettings() const
{
    namespace id = pa::params::id;
    const auto value = [this](const char* paramId)
    {
        const auto* p = parameters.getParameter(paramId);
        return (double)p->convertFrom0to1(p->getValue());
    };
    pa::analyse::PanelSettings s;
    s.delayOn = value(id::delayOn) >= 0.5;
    s.delayMs = value(id::delayMs);
    s.polarity = value(id::polarity) >= 0.5;
    s.phaseOn = value(id::phaseOn) >= 0.5;
    s.phaseMode = juce::jlimit(0, 2, juce::roundToInt(value(id::phaseMode)));
    s.range180 = value(id::phaseRange) >= 0.5;
    s.phase = value(id::phase);
    return s;
}

void PhaseAlignProcessor::applyPanelSettings(const pa::analyse::PanelSettings& s)
{
    namespace id = pa::params::id;
    const std::pair<const char*, double> values[] = {{id::delayOn, s.delayOn ? 1.0 : 0.0},
                                                     {id::delayMs, s.delayMs},
                                                     {id::polarity, s.polarity ? 1.0 : 0.0},
                                                     {id::phaseOn, s.phaseOn ? 1.0 : 0.0},
                                                     {id::phaseMode, (double)s.phaseMode},
                                                     {id::phaseRange, s.range180 ? 1.0 : 0.0},
                                                     {id::phase, s.phase}};
    std::vector<std::pair<juce::RangedAudioParameter*, float>> changes;
    for (const auto& [paramId, value] : values)
    {
        auto* p = parameters.getParameter(paramId);
        const auto normalised = p->convertTo0to1((float)value);
        if (std::abs(normalised - p->getValue()) > 1e-7f)
            changes.push_back({p, normalised});
    }
    for (auto& [p, v] : changes)
        p->beginChangeGesture();
    for (auto& [p, v] : changes)
        p->setValueNotifyingHost(v);
    for (auto& [p, v] : changes)
        p->endChangeGesture();
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

// With no sidechain source chosen, some hosts (Logic Pro) don't give the plugin silence: they feed
// its own input in as the sidechain. The meter would then compare the track with a perfect copy of itself and read +1.
// So a sidechain that is sample for sample the main input, for a quarter of a second, counts as no sidechain at all.
// Silence in both doesn't count either way (they are trivially equal), and the first sample that differs ends it at
// once. Audio thread, before the chain overwrites the input (it runs in place); only while the meter or ANALYSE is
// capturing.
bool PhaseAlignProcessor::sidechainIsOwnInput(const juce::AudioBuffer<float>& main, const juce::AudioBuffer<float>& sidechain)
{
    if (! meterCapture.isActive() && ! analyseCapture.isActive())
    {
        identicalSamples = 0;
        return false;
    }
    const auto n = main.getNumSamples();
    const auto channels = std::min(main.getNumChannels(), sidechain.getNumChannels());
    bool same = channels > 0, energy = false;
    for (int ch = 0; ch < channels && same; ++ch)
    {
        const auto* a = main.getReadPointer(ch);
        const auto* b = sidechain.getReadPointer(ch);
        for (int i = 0; i < n; ++i)
        {
            if (a[i] != b[i])
            {
                same = false;
                break;
            }
            energy = energy || a[i] != 0.0f;
        }
    }
    if (! same)
        identicalSamples = 0;
    else if (energy) // identical and not just silence
        identicalSamples = std::min(identicalSamples + n, 1 << 30);
    return identicalSamples >= std::max(1, (int)(0.25 * sampleRate.load())); // a quarter of a second
}

void PhaseAlignProcessor::runChain(juce::AudioBuffer<float>& buffer, const pa::dsp::ChainSettings& settings)
{
    juce::ScopedNoDenormals noDenormals;
    auto main = getBusBuffer(buffer, false, 0); // in place: the main input shares these channels
    const auto sidechain = getBusBuffer(buffer, true, 1);
    const auto numChannels = main.getNumChannels();
    meterCapture.setSidechainPresent(sidechain.getNumChannels() > 0 && ! sidechainIsOwnInput(main, sidechain));
    meterCapture.setStereoTrack(numChannels > 1);
    {
        const auto position = getPlayHead() != nullptr ? getPlayHead()->getPosition() : juce::Optional<juce::AudioPlayHead::PositionInfo>();
        meterCapture.setTransport(position.hasValue(), position.hasValue() && position->getIsPlaying());
    }
    chain.setSettings(settings);

    const auto metering = meterCapture.isActive(), analysing = analyseCapture.isActive();
    if (! metering && ! analysing)
    {
        chain.process(main.getArrayOfWritePointers(), numChannels, main.getNumSamples());
        return;
    }

    // Metering: mono sums of the input, output and sidechain, a sub-block at a time (no scratch to size). The capture
    // delays the input and sidechain by the chain's current latency, so all three line up. ANALYSE takes the input and
    // sidechain as they arrive (already lined up with each other).
    constexpr int sub = pa::dsp::Chain::subBlockSize;
    float in[sub], out[sub], sc[sub], inSide[sub], outSide[sub];
    const auto withSide = metering && numChannels > 1 && meterCapture.wantsSide();
    float* channels[pa::dsp::Chain::maxChannels];
    const auto mono = [](const juce::AudioBuffer<float>& b, int start, int n, float* dest)
    {
        std::fill(dest, dest + n, 0.0f);
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            juce::FloatVectorOperations::add(dest, b.getReadPointer(ch, start), n);
        if (b.getNumChannels() > 1)
            juce::FloatVectorOperations::multiply(dest, 1.0f / (float)b.getNumChannels(), n);
    };

    // (L - R) / 2 of the first two channels.
    const auto side = [](const juce::AudioBuffer<float>& b, int start, int n, float* dest)
    {
        const auto* l = b.getReadPointer(0, start);
        const auto* r = b.getReadPointer(1, start);
        for (int i = 0; i < n; ++i)
            dest[i] = 0.5f * (l[i] - r[i]);
    };

    for (int start = 0; start < main.getNumSamples(); start += sub)
    {
        const auto n = std::min(sub, main.getNumSamples() - start);
        mono(main, start, n, in);
        if (withSide)
            side(main, start, n, inSide);
        mono(sidechain, start, n, sc);
        if (analysing)
            analyseCapture.push(in, sc, n);
        for (int ch = 0; ch < std::min(numChannels, pa::dsp::Chain::maxChannels); ++ch)
            channels[ch] = main.getWritePointer(ch, start);
        chain.process(channels, std::min(numChannels, pa::dsp::Chain::maxChannels), n);
        if (! metering)
            continue;
        mono(main, start, n, out);
        if (withSide)
            side(main, start, n, outSide);
        meterCapture.push(in, out, sc, n, chain.latency(), withSide ? inSide : nullptr, withSide ? outSide : nullptr);
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
                                           {UiProps::meterView, "bands"},
                                           {UiProps::meterSpeed, "slow"},
                                           {UiProps::alignCapture, true},
                                           {UiProps::vectorStereo, false},
                                           {UiProps::showInput, true},
                                           {UiProps::showOutput, true},
                                           {UiProps::showSidechain, true},
                                           {UiProps::analysePreview, "bands"}});
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
    // The old FREQUENCY, TIME OFFSET and PHASE views are gone: an old state opens on BANDS.
    const auto view = get(UiProps::meterView).toString();
    uiState.setProperty(UiProps::meterView, view == "vector" || view == "scope" ? view : juce::String("bands"), nullptr);
    uiState.setProperty(UiProps::meterSpeed, get(UiProps::meterSpeed).toString() == "fast" ? "fast" : "slow", nullptr);
    uiState.setProperty(UiProps::alignCapture, (bool)get(UiProps::alignCapture), nullptr);
    uiState.setProperty(UiProps::vectorStereo, (bool)get(UiProps::vectorStereo), nullptr);
    for (const auto& id : {UiProps::showInput, UiProps::showOutput, UiProps::showSidechain})
        uiState.setProperty(id, (bool)get(id), nullptr);
    uiState.setProperty(UiProps::analysePreview,
                        get(UiProps::analysePreview).toString() == "alignment" ? "alignment" : "bands", nullptr);
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
