// Catch2 runner with JUCE's message manager and GUI subsystems initialised (the editor tests need them).
//
//   ctest --test-dir build --output-on-failure        # everything except hidden tests
//   build/PhaseAlignTests_artefacts/<cfg>/PhaseAlignTests "[snapshot]"   # PNGs into $PA_SNAPSHOT_DIR
//   build/PhaseAlignTests_artefacts/<cfg>/PhaseAlignTests "[desktop]"    # opens a window: idle repaints

#include <catch2/catch_session.hpp>
#include <juce_gui_basics/juce_gui_basics.h>

int main(int argc, char* argv[])
{
    const juce::ScopedJuceInitialiser_GUI juce;
    return Catch::Session().run(argc, argv);
}
