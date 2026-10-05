#include "PluginEditor.h"

// Placeholder size: half of the 1954x1224 base artwork in ui/plugin-base.png.
PhaseAlignEditor::PhaseAlignEditor(PhaseAlignProcessor& p) : AudioProcessorEditor(&p) { setSize(977, 612); }

void PhaseAlignEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colours::black);
    g.setColour(juce::Colours::white);
    g.setFont(24.0f);
    g.drawFittedText("Phase Align", getLocalBounds(), juce::Justification::centred, 1);
}
