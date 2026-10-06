#include "dsp/Chain.h"

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <vector>

// The C++ port against the Python prototypes (IMPLEMENTATION_PLAN M2): prototype/golden.py renders fixed scripts
// through prototype/hilo.py (HiLoOversampled) and prototype/p2_constant.py (ConstantRotator) into tests/golden/, and
// the chain must match them sample by sample. Keep the scripts in step with golden.py.
using namespace pa::dsp;

namespace
{
std::vector<float> load(const std::string& name)
{
    std::ifstream f(std::string(PA_GOLDEN_DIR) + "/" + name, std::ios::binary);
    REQUIRE(f.good());
    f.seekg(0, std::ios::end);
    const auto bytes = (size_t)f.tellg();
    f.seekg(0);
    std::vector<float> v(bytes / sizeof(float));
    f.read(reinterpret_cast<char*>(v.data()), (std::streamsize)bytes);
    return v;
}

struct Event
{
    int at;
    double theta;  // < 0: unchanged
    int mode;      // < 0: unchanged; else PhaseMode
    int wide = -1; // < 0: unchanged; else 0 or 1 (RANGE 180)
};

// Runs the case's input through a chain with only the phase stage on, applying the events, in blocks of `block`
// (which needn't line up with anything), and returns the worst difference from the expected output.
double worstError(const std::string& name, double fs, PhaseMode initialMode, const std::vector<Event>& events,
                  int block)
{
    const auto in = load(name + "_in.f32"), expected = load(name + "_out.f32");
    const auto n = (int)in.size() / 2;
    std::vector<float> l(in.begin(), in.begin() + n), r(in.begin() + n, in.end());

    ChainSettings s = ChainSettings::neutral();
    s.phaseOn = true;
    s.phaseMode = initialMode;
    Chain chain;
    chain.prepare(fs, 2, 400);
    chain.reset(s);

    size_t next = 0;
    for (int pos = 0; pos < n;)
    {
        while (next < events.size() && events[next].at <= pos)
        {
            if (events[next].theta >= 0.0)
                s.phaseDegrees = events[next].theta;
            if (events[next].mode >= 0)
                s.phaseMode = (PhaseMode)events[next].mode;
            if (events[next].wide >= 0)
                s.phaseWide = events[next].wide != 0;
            chain.setSettings(s);
            ++next;
        }
        auto m = std::min(block, n - pos);
        if (next < events.size())
            m = std::min(m, events[next].at - pos);
        float* ptrs[] = {l.data() + pos, r.data() + pos};
        chain.process(ptrs, 2, m);
        pos += m;
    }

    double worst = 0.0;
    for (int i = 0; i < n; ++i)
    {
        worst = std::max(worst, (double)std::abs(l[(size_t)i] - expected[(size_t)i]));
        worst = std::max(worst, (double)std::abs(r[(size_t)i] - expected[(size_t)(n + i)]));
    }
    return worst;
}

constexpr int hi = (int)PhaseMode::high, lo = (int)PhaseMode::low;
} // namespace

TEST_CASE("golden: Hi/Lo matches prototype/hilo.py sample by sample", "[dsp][golden]")
{
    // golden.py HILO_SCRIPT
    const std::vector<Event> events = {{1024, 45.0, -1, -1}, {2048, 90.0, -1, -1},       {3072, 120.0, -1, 1},
                                       {4096, -1.0, lo, -1}, {4096 + 320, -1.0, hi, -1}, {5120, 170.0, -1, -1},
                                       {5632, -1.0, -1, 0},  {6144, 30.0, -1, -1},       {6400, -1.0, lo, -1},
                                       {7168, 180.0, -1, 1}, {7680, 100.0, hi, 0}};
    for (const auto fs : {44100.0, 48000.0, 96000.0, 192000.0}) // oversampled 4x (both outer halfbands), 2x, not
        for (const auto block : {512, 37})
        {
            INFO("fs " << fs << ", block " << block);
            CHECK(worstError("hilo_" + std::to_string((int)fs), fs, PhaseMode::high, events, block) < 1.0e-6);
        }
}

TEST_CASE("golden: Constant matches prototype/p2_constant.py sample by sample", "[dsp][golden]")
{
    // golden.py CONSTANT_SCRIPT. The prototype convolves exactly; the C++ in float FFT partitions.
    const std::vector<Event> events = {{2048, 90.0, -1}, {6144, 180.0, -1}, {9216, 37.0, -1}, {12288, 0.0, -1}};
    for (const auto fs : {48000.0, 96000.0})
        for (const auto block : {512, 37})
        {
            INFO("fs " << fs << ", block " << block);
            CHECK(worstError("constant_" + std::to_string((int)fs), fs, PhaseMode::constant, events, block) < 1.0e-6);
        }
}
