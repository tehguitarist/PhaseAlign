// Catch2 runner with JUCE's message manager and GUI subsystems initialised (the editor tests need them). Global
// operator new is counted per thread (ThreadAllocations.h), so tests can check that the audio callbacks never allocate.
//
//   ctest --test-dir build --output-on-failure        # everything except hidden tests
//   build/PhaseAlignTests_artefacts/<cfg>/PhaseAlignTests "[snapshot]"   # PNGs into $PA_SNAPSHOT_DIR
//   build/PhaseAlignTests_artefacts/<cfg>/PhaseAlignTests "[desktop]"    # opens a window: idle repaints

#include "ThreadAllocations.h"

#include <catch2/catch_session.hpp>
#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdlib>
#include <new>

namespace
{
thread_local long allocationsOnThisThread = 0;
}

long pa::test::threadAllocations()
{
    return allocationsOnThisThread;
}

void* operator new(std::size_t size)
{
    ++allocationsOnThisThread;
    if (auto* p = std::malloc(size == 0 ? 1 : size))
        return p;
    throw std::bad_alloc();
}

void operator delete(void* p) noexcept
{
    std::free(p);
}

void operator delete(void* p, std::size_t) noexcept
{
    std::free(p);
}

int main(int argc, char* argv[])
{
    const juce::ScopedJuceInitialiser_GUI juce;
    return Catch::Session().run(argc, argv);
}
