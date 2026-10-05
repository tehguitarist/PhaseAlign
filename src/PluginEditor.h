#pragma once

#include "PluginProcessor.h"

class PhaseAlignEditor : public juce::AudioProcessorEditor
{
public:
    explicit PhaseAlignEditor(PhaseAlignProcessor&);

    void paint(juce::Graphics&) override;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PhaseAlignEditor)
};
