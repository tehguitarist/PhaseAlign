#include "PluginProcessor.h"
#include "PluginEditor.h"

PhaseAlignProcessor::PhaseAlignProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)
                         .withInput("Sidechain", juce::AudioChannelSet::stereo(), true))
{
}

void PhaseAlignProcessor::prepareToPlay(double, int) {}

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

void PhaseAlignProcessor::processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    // Pass-through: the main buffer already holds the input, so there is nothing to do yet.
}

juce::AudioProcessorEditor* PhaseAlignProcessor::createEditor() { return new PhaseAlignEditor(*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new PhaseAlignProcessor(); }
