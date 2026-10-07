#include "analyse/Session.h"

#include "params/Parameters.h"

#include <algorithm>
#include <cmath>

namespace pa::analyse
{
namespace
{
constexpr double hitPreSeconds = 0.06, hitPostSeconds = 0.24; // as meter::HitCapture

// The strongest onset in the sidechain: its loudest point (on a 1 ms smoothed envelope), then back to where the
// envelope first rose above a tenth of that, within 30 ms.
int strongestOnset(const std::vector<float>& sc, double fs)
{
    const auto n = (int)sc.size();
    const auto smooth = std::max(1, (int)(0.001 * fs));
    std::vector<float> envelope((size_t)n);
    double sum = 0.0;
    for (int i = 0; i < n; ++i)
    {
        sum += std::abs(sc[(size_t)i]);
        if (i >= smooth)
            sum -= std::abs(sc[(size_t)(i - smooth)]);
        envelope[(size_t)i] = (float)(sum / smooth);
    }
    const auto peak = (int)(std::max_element(envelope.begin(), envelope.end()) - envelope.begin());
    auto onset = peak;
    const auto limit = std::max(0, peak - (int)(0.03 * fs));
    while (onset > limit && envelope[(size_t)onset - 1] > 0.1f * envelope[(size_t)peak])
        --onset;
    return std::max(0, onset - smooth); // the envelope lags the signal by its smoothing
}

std::vector<float> window(const std::vector<float>& v, int first, int length)
{
    std::vector<float> out((size_t)length, 0.0f);
    for (int i = 0; i < length; ++i)
        if (first + i >= 0 && first + i < (int)v.size())
            out[(size_t)i] = v[(size_t)(first + i)];
    return out;
}
} // namespace

//==============================================================================
bool PanelSettings::sameAs(const PanelSettings& o) const
{
    return delayOn == o.delayOn && std::abs(delayMs - o.delayMs) < 1e-4 && polarity == o.polarity &&
           phaseOn == o.phaseOn && phaseMode == o.phaseMode && range180 == o.range180 &&
           std::abs(phase - o.phase) < 1e-5;
}

Candidate PanelSettings::candidate(double sampleRate) const
{
    Candidate c;
    c.mode = ! phaseOn                                       ? Mode::none
             : phaseMode == (int)params::PhaseMode::constant ? Mode::constant
             : phaseMode == (int)params::PhaseMode::low      ? Mode::lo
                                                             : Mode::hi;
    c.wide = range180;
    c.theta = phase * params::rangeDegrees(range180);
    c.delayMs = delayOn ? params::effectiveDelayMs(delayMs, sampleRate) : 0.0;
    c.flip = polarity;
    return c;
}

PanelSettings settingsFor(const Candidate& c, const PanelSettings& base, Scope scope)
{
    auto s = base;
    s.polarity = c.flip;
    if (scope.delayOn)
    {
        s.delayOn = true;
        s.delayMs = c.delayMs;
    }
    if (scope.phaseOn)
    {
        if (c.mode == Mode::none)
            s.phaseOn = false;
        else
        {
            s.phaseOn = true;
            s.phaseMode = (int)(c.mode == Mode::constant ? params::PhaseMode::constant
                                : c.mode == Mode::lo     ? params::PhaseMode::low
                                                         : params::PhaseMode::high);
            s.range180 = c.mode == Mode::constant ? base.range180 || c.theta > 90.0 + 1e-9 : c.wide;
            s.phase = std::clamp(c.theta / params::rangeDegrees(s.range180), 0.0, 1.0);
        }
    }
    return s;
}

//==============================================================================
Session::Session(CaptureFifo& f, Host h) : fifo(f), host(std::move(h)) {}

Session::~Session()
{
    stopTimer();
    fifo.setActive(false);
    cancelWorker();
}

void Session::cancelWorker()
{
    cancel.store(true);
    if (worker.joinable())
        worker.join();
    cancel.store(false);
    done.store(false);
    pending.reset();
}

void Session::setState(State s)
{
    state = s;
    changed();
}

void Session::start()
{
    original = host.settings();
    restart();
}

void Session::restart()
{
    cancelWorker();
    outcome.reset();
    fs = host.sampleRate();
    const auto ringLength = (size_t)std::ceil(maxSeconds * fs);
    ringInput.assign(ringLength, 0.0f);
    ringSidechain.assign(ringLength, 0.0f);
    scratchInput.resize((size_t)CaptureFifo::capacity);
    scratchSidechain.resize((size_t)CaptureFifo::capacity);
    const auto chunk = std::max(1, (int)std::lround(chunkSeconds * fs));
    chunkInput.assign((size_t)chunk, 0.0f);
    chunkSidechain.assign((size_t)chunk, 0.0f);
    chunkFill = 0;
    written = 0;
    tooShort = false;
    sawPlaying = false;
    inputLevel = sidechainLevel = -100.0f;
    inputPeakHold = sidechainPeakHold = 0.0;
    levelSamples = 0;
    fifo.allocate();
    fifo.setActive(true);
    startTimerHz(30);
    setState(State::capturing);
}

void Session::stop()
{
    stopTimer();
    fifo.setActive(false);
    cancelWorker();
    outcome.reset();
    // The capture's memory goes with it (up to 30 s of two streams).
    std::vector<float>().swap(ringInput);
    std::vector<float>().swap(ringSidechain);
    std::vector<float>().swap(scratchInput);
    std::vector<float>().swap(scratchSidechain);
    written = 0;
    setState(State::idle);
}

double Session::capturedSeconds() const
{
    return (double)std::min<long long>(written, (long long)ringInput.size()) / fs;
}

bool Session::isWaitingForPlay() const
{
    return host.transportKnown() && ! host.transportPlaying();
}

void Session::timerCallback()
{
    if (state == State::analysing)
    {
        if (done.load(std::memory_order_acquire))
            finishAnalysis();
        return;
    }
    if (state != State::capturing)
        return;

    // A new rate makes the capture meaningless: start again.
    if (! juce::exactlyEqual(host.sampleRate(), fs))
    {
        restart();
        return;
    }
    const auto n = fifo.pull(scratchInput.data(), scratchSidechain.data(), CaptureFifo::capacity);
    if (fifo.takeDropped() > 0)
        chunkFill = 0; // a gap: the chunk in progress would straddle it
    const auto playing = ! isWaitingForPlay();
    if (n > 0 && playing && hasSidechain())
        takeIn(scratchInput.data(), scratchSidechain.data(), n);
    else
        chunkFill = 0;

    // The transport stopping is the cue to analyse (if there's enough; otherwise say so and keep it).
    if (host.transportKnown())
    {
        if (playing)
            sawPlaying = true;
        else if (sawPlaying)
        {
            sawPlaying = false;
            analyseNow();
            return;
        }
    }
    changed();
}

void Session::takeIn(const float* in, const float* sc, int n)
{
    // Levels for the screen: the peak of the last 0.3 s.
    for (int i = 0; i < n; ++i)
    {
        inputPeakHold = std::max(inputPeakHold, (double)std::abs(in[i]));
        sidechainPeakHold = std::max(sidechainPeakHold, (double)std::abs(sc[i]));
    }
    levelSamples += n;
    if (levelSamples >= (int)(0.3 * fs))
    {
        const auto db = [](double v) { return (float)(v > 1e-5 ? 20.0 * std::log10(v) : -100.0); };
        inputLevel = db(inputPeakHold);
        sidechainLevel = db(sidechainPeakHold);
        inputPeakHold = sidechainPeakHold = 0.0;
        levelSamples = 0;
    }

    const auto chunk = (int)chunkInput.size();
    for (int i = 0; i < n;)
    {
        const auto m = std::min(chunk - chunkFill, n - i);
        std::copy(in + i, in + i + m, chunkInput.begin() + chunkFill);
        std::copy(sc + i, sc + i + m, chunkSidechain.begin() + chunkFill);
        chunkFill += m;
        i += m;
        if (chunkFill == chunk)
        {
            keepChunk();
            chunkFill = 0;
        }
    }
}

void Session::keepChunk()
{
    // Only stretches where both play: the rest would only dilute the correlation.
    const auto floor = std::pow(10.0, signalFloorDb / 20.0);
    const auto rms = [](const std::vector<float>& v)
    {
        double sum = 0.0;
        for (const auto x : v)
            sum += (double)x * x;
        return std::sqrt(sum / (double)v.size());
    };
    if (rms(chunkInput) < floor || rms(chunkSidechain) < floor)
        return;
    // A sidechain that is the track itself (Logic with no sidechain chosen, plan R24) is no reference. The processor
    // takes a quarter of a second to decide that; here it is exact at once.
    if (std::equal(chunkInput.begin(), chunkInput.end(), chunkSidechain.begin()))
        return;
    const auto size = (long long)ringInput.size();
    for (size_t i = 0; i < chunkInput.size(); ++i)
    {
        const auto at = (size_t)((written + (long long)i) % size);
        ringInput[at] = chunkInput[i];
        ringSidechain[at] = chunkSidechain[i];
    }
    written += (long long)chunkInput.size();
    tooShort = false;
}

void Session::analyseNow()
{
    if (state != State::capturing)
        return;
    if (capturedSeconds() < minSeconds)
    {
        tooShort = true;
        changed();
        return;
    }

    // The capture, oldest first, for the worker; capturing pauses meanwhile.
    fifo.setActive(false);
    const auto size = (long long)ringInput.size();
    const auto captured = (int)std::min(written, size);
    const auto oldest = written > size ? written % size : 0;
    std::vector<float> input((size_t)captured), reference((size_t)captured);
    for (int i = 0; i < captured; ++i)
    {
        input[(size_t)i] = ringInput[(size_t)((oldest + i) % size)];
        reference[(size_t)i] = ringSidechain[(size_t)((oldest + i) % size)];
    }

    cancelWorker();
    const auto scope = host.scope();
    const auto rate = fs;
    const auto originalCandidate = original.candidate(rate);
    worker = std::thread(
        [this, x = std::move(input), y = std::move(reference), scope, rate, originalCandidate]
        {
            auto out = std::make_unique<Outcome>();
            const auto n = (int)x.size();
            out->scope = scope;
            out->sampleRate = rate;
            out->seconds = n / rate;
            out->result = suggestWithShift(x.data(), y.data(), n, rate, scope, &cancel);
            if (out->result.cancelled)
                return;
            out->spectra = Spectra::compute(x.data(), y.data(), n, rate);
            out->originalScore = out->spectra.valid() ? scoreAt(out->spectra, originalCandidate) : 0.0;
            const auto shift = out->result.shiftSamples;
            std::vector<float> shifted;
            if (shift != 0)
            {
                shifted.assign((size_t)n, 0.0f);
                for (int i = 0; i < n; ++i)
                    if (i - shift >= 0 && i - shift < n)
                        shifted[(size_t)i] = x[(size_t)(i - shift)];
                out->shiftedSpectra = Spectra::compute(shifted.data(), y.data(), n, rate);
            }
            const auto onset = strongestOnset(y, rate);
            const auto first = onset - (int)(hitPreSeconds * rate);
            const auto length = (int)((hitPreSeconds + hitPostSeconds) * rate);
            out->hitInput = window(x, first, length);
            out->hitShiftedInput = shift != 0 ? window(shifted, first, length) : out->hitInput;
            out->hitSidechain = window(y, first, length);
            out->hitOnset = onset - first;
            pending = std::move(out);
            done.store(true, std::memory_order_release);
        });
    tooShort = false;
    setState(State::analysing);
}

void Session::finishAnalysis()
{
    if (worker.joinable())
        worker.join();
    outcome = std::move(pending);
    done.store(false);
    stopTimer();
    setState(outcome != nullptr ? State::results : State::capturing);
}

void Session::waitForAnalysisForTesting()
{
    if (worker.joinable())
        worker.join();
    if (state == State::analysing)
        finishAnalysis();
}

PanelSettings Session::optionSettings(int index) const
{
    if (index < 0 || outcome == nullptr || index >= numOptions())
        return original;
    return settingsFor(outcome->result.options[(size_t)index].candidate, original, outcome->scope);
}

void Session::choose(int index)
{
    if (outcome == nullptr)
        return;
    host.apply(optionSettings(index));
    changed();
}

int Session::chosenNow() const
{
    if (outcome == nullptr)
        return -2;
    const auto now = host.settings();
    for (int i = 0; i < numOptions(); ++i)
        if (optionSettings(i).sameAs(now))
            return i;
    return original.sameAs(now) ? -1 : -2;
}
} // namespace pa::analyse
