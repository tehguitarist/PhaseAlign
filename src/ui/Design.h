#pragma once

#include "ui/Layout.h"

// Hand-measured design values that ui-info.csv doesn't carry: measured on ui/plugin-base.png and
// "ui/full example.png", in the same 1954x1224 design space as Layout.h.
namespace pa::design
{
// The reference editor size (uiScale 1) is half the design space; resizable from 75% to 200% of it, opening at 80%
// (PhaseAlignProcessor::defaultUiScale; IMPLEMENTATION_PLAN 4.2).
inline constexpr int defaultWidth = 977;
inline constexpr int defaultHeight = 612;

// Knob sweep: the 17 baked ring dots run from -142.1 to +142.1 degrees from 12 o'clock.
inline constexpr float knobSweepDegrees = 142.1f;

// Flat black interiors of the baked screens (inside the bezel highlights and the inner shadow).
inline const juce::Rectangle<float> meterInterior{122.0f, 614.0f, 1405.0f, 490.0f};
inline const juce::Rectangle<float> delayReadoutInterior{127.0f, 472.0f, 248.0f, 68.0f};
inline const juce::Rectangle<float> phaseReadoutInterior{1584.0f, 472.0f, 248.0f, 68.0f};
inline const juce::Colour screenBlack{0xff030302};

// Switch labels: Arial Bold, cap height 18, rows 69.5 above and below the switch centre, 15 from the
// switch edge. Delay labels sit to the right, phase labels to the left.
inline constexpr float labelCapHeight = 18.0f;
inline constexpr float labelRowSpacing = 69.5f;
inline constexpr float labelGap = 15.0f;
inline constexpr float labelAreaWidth = 230.0f;
inline const juce::Colour labelSelected{0xffffffff};
inline const juce::Colour labelDimmed{0xff62625e};
inline const juce::Colour labelGlow{0x99ffffff};

// The phase knob's upper scale label (90 or 180 degrees, following RANGE), drawn to match the baked "0°":
// digits 22 high starting 10 below the slot top, left-aligned; a 12x12 degree sign at the slot top, 5 after
// the digits. The font is Arial Hebrew where installed (as in the art), else the label font.
inline constexpr float scaleDigitHeight = 22.0f;
inline constexpr float scaleDegreeSize = 12.0f;
inline constexpr float scaleDegreeGap = 5.0f;
inline const juce::Colour scaleLabelColour{0xfff6f4f2};

// A switched-off section's controls (IMPLEMENTATION_PLAN 4.5) are drawn at this opacity but stay fully usable.
inline constexpr float dimmedAlpha = 0.4f;

// Readouts: green 7-segment digits with a glow; the unit suffix sits bottom-right in Arial Bold.
inline constexpr float readoutDigitHeight = 47.0f; // lit segment height in the example
inline constexpr float readoutSuffixCapHeight = 15.0f;
inline constexpr float readoutDegreeCapHeight = 34.0f; // the degree sign is drawn from the digit top
inline const juce::Colour readoutGreen{0xff3cf04e};
inline const juce::Colour readoutGlow{0xa02ee040};
inline constexpr float readoutGhostAlpha = 0.07f;

// Meter screen (positions measured on the example; the band count is provisional until P4).
inline const juce::Colour phosphor{0xff3cf04e};
inline const juce::Colour meterAxisText{0xffd0d0cc};
inline const juce::Colour meterGrid{0xff5a5a58};
inline constexpr float meterPlotLeft = 186.0f, meterPlotRight = 1298.0f;
inline constexpr float meterPlusOneY = 656.0f, meterZeroY = 837.0f, meterMinusOneY = 1016.0f;
inline constexpr float meterFreqLabelY = 1051.0f, meterFreqTitleY = 1089.0f;
inline constexpr float meterSeparatorX = 1330.0f;
inline constexpr float meterOverallAxisX = 1375.0f, meterOverallLeft = 1445.0f, meterOverallRight = 1496.0f;
} // namespace pa::design
