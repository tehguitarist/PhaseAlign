// The DSP test runner: src/dsp and src/meter plus juce_dsp, no GUI modules, so it
// builds and runs quickly. Global operator new is counted, so tests can check that the real-time paths
// never allocate.
//
//   ctest --test-dir build --output-on-failure
//   build/PhaseAlignDspTests_artefacts/<cfg>/PhaseAlignDspTests "[bench]"   # hidden benchmarks

#include "AllocationCounter.h"

#include <catch2/catch_session.hpp>

#include <cstdlib>
#include <new>

namespace pa::test
{
std::atomic<long> allocationCount{0};
}

void* operator new(std::size_t size)
{
    pa::test::allocationCount.fetch_add(1, std::memory_order_relaxed);
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
    return Catch::Session().run(argc, argv);
}
