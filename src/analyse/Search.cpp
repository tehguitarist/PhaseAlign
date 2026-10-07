#include "analyse/Search.h"

#include "dsp/PhaseResponse.h"

#include <pffft/pffft_double.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>

// Line for line with prototype/analyse.py: where the Python rounds with round() (half to even) this uses nearbyint, and
// where it truncates with int(), a cast; the expressions keep the Python's order of operations, so thresholds and lags
// land on the same side.
namespace pa::analyse
{
namespace
{
constexpr double pi = 3.14159265358979323846;
constexpr int pad = 4; // the lag search's zero-padding: a quarter of a sample
constexpr double bandLoHz = 40.0, bandHiHz = 16000.0, activeDb = -40.0;
constexpr double familyDelayMs = 0.5, familyDegrees = 20.0;
constexpr int refineLeaders = 200;

struct Cancelled
{
};

void checkCancel(const std::atomic<bool>* cancel)
{
    if (cancel != nullptr && cancel->load(std::memory_order_relaxed))
        throw Cancelled{};
}

int roundHalfEven(double v)
{
    return (int)std::nearbyint(v);
}

// A double-precision real FFT (PFFFT's double engine), unscaled both ways. Ordered packing: [X0, X(n/2), re 1, im 1,
// re 2, im 2, ...].
class Fft
{
  public:
    explicit Fft(int size) : n(size)
    {
        setup = pffftd_new_setup(n, PFFFT_REAL);
        if (setup == nullptr)
            throw std::runtime_error("unsupported FFT size");
        work = alloc(n);
        buffer = alloc(n);
    }
    ~Fft()
    {
        pffftd_destroy_setup(setup);
        pffftd_aligned_free(work);
        pffftd_aligned_free(buffer);
    }
    Fft(const Fft&) = delete;
    Fft& operator=(const Fft&) = delete;

    int size() const { return n; }
    double* data() { return buffer; } // n samples in, packed bins out (in place), or the other way
    void forward() { pffftd_transform_ordered(setup, buffer, buffer, work, PFFFT_FORWARD); }
    void inverse() { pffftd_transform_ordered(setup, buffer, buffer, work, PFFFT_BACKWARD); }

    static double* alloc(int count)
    {
        return static_cast<double*>(pffftd_aligned_malloc((size_t)count * sizeof(double)));
    }

  private:
    int n;
    PFFFTD_Setup* setup = nullptr;
    double* work = nullptr;
    double* buffer = nullptr;
};

int frameSize(double fs)
{
    return 8192 * (fs < 60000.0 ? 1 : fs < 120000.0 ? 2 : 4);
}

dsp::PhaseMode chainMode(Mode m)
{
    return m == Mode::hi ? dsp::PhaseMode::high : m == Mode::lo ? dsp::PhaseMode::low : dsp::PhaseMode::constant;
}

// The phase stage's response at the bins of the active bands (others are never read).
std::vector<std::complex<double>> stageResponse(const Spectra& sp, Mode mode, bool wide, double theta)
{
    std::vector<std::complex<double>> h(sp.sxy.size(), 1.0);
    if (mode == Mode::none)
        return h;
    Candidate c;
    c.mode = mode;
    c.wide = wide;
    c.theta = theta;
    const dsp::PhaseStageResponse response(c.toSettings(sp.fs), sp.fs);
    for (const auto& [a, b] : sp.edges)
        for (int k = a; k < b; ++k)
            h[(size_t)k] = response(sp.binHz(k));
    return h;
}

// Re(sum G Sxy) / norm per band, G = sign * h * e^(-j w d), d in samples.
double scoreWith(const Spectra& sp, const std::vector<std::complex<double>>& h, double sign, double delaySamples,
                 double belowHz = 0.0)
{
    double total = 0.0;
    int count = 0;
    for (size_t b = 0; b < sp.edges.size(); ++b)
    {
        if (belowHz > 0.0 && sp.centres[b] >= belowHz)
            continue;
        double sum = 0.0;
        for (int k = sp.edges[b].first; k < sp.edges[b].second; ++k)
        {
            const auto w = -2.0 * pi * sp.binHz(k) * delaySamples / sp.fs;
            const auto g = sign * h[(size_t)k] * std::complex<double>(std::cos(w), std::sin(w));
            sum += (g * sp.sxy[(size_t)k]).real();
        }
        total += sum / sp.norms[b];
        ++count;
    }
    return count > 0 ? total / count : std::numeric_limits<double>::quiet_NaN();
}

struct Setting
{
    Mode mode;
    bool wide;
    double theta;
};

std::vector<Setting> settings(bool phaseOn)
{
    std::vector<Setting> out{{Mode::none, false, 0.0}};
    if (! phaseOn)
        return out;
    const std::pair<Mode, bool> shapes[] = {{Mode::lo, false}, {Mode::lo, true}, {Mode::hi, false}, {Mode::hi, true}};
    for (const auto& [mode, wide] : shapes)
    {
        const auto top = wide ? 180.0 : 90.0;
        for (int i = 0; angleStep + i * angleStep < top + 1e-9; ++i)
            out.push_back({mode, wide, angleStep + i * angleStep});
    }
    for (int i = 0; angleStep + i * angleStep < 180.0 + 1e-9; ++i)
        out.push_back({Mode::constant, true, angleStep + i * angleStep});
    return out;
}

// The polarity and the phase stage (no delay) at the centre of each active band.
std::vector<std::complex<double>> familyResponse(const Spectra& sp, const Candidate& c)
{
    std::vector<std::complex<double>> g(sp.centres.size(), c.flip ? -1.0 : 1.0);
    if (c.mode == Mode::none)
        return g;
    const dsp::PhaseStageResponse response(c.toSettings(sp.fs), sp.fs);
    for (size_t b = 0; b < g.size(); ++b)
        g[b] *= response(sp.centres[b]);
    return g;
}

// One family when they would sound alike, as the score tells it: the band-mean correlation of one's output with the
// other's (the mean over the active bands of cos(their phase difference at the band's centre)) at least
// cos(familyDegrees), either for the polarity and phase stage alone with delays within familyDelayMs, or for the whole
// response, delay included (a Hi/Lo turn at a small angle is close to a short delay). So CONSTANT 180 is the polarity
// flip.
bool sameFamily(const Spectra& sp, const Candidate& f, const Candidate& c)
{
    const auto gf = familyResponse(sp, f), gc = familyResponse(sp, c);
    const auto limit = std::cos(familyDegrees * pi / 180.0);
    const auto meanCos = [&](double delayMs)
    {
        double sum = 0.0;
        for (size_t b = 0; b < gf.size(); ++b)
        {
            const auto w = -2.0 * pi * sp.centres[b] * delayMs * 1e-3;
            sum += std::cos(std::arg(gf[b] * std::complex<double>(std::cos(w), std::sin(w)) * std::conj(gc[b])));
        }
        return sum / (double)gf.size();
    };
    if (std::abs(f.delayMs - c.delayMs) < familyDelayMs && meanCos(0.0) >= limit)
        return true;
    return meanCos(f.delayMs - c.delayMs) >= limit;
}

// --- the attack lag
// ---------------------------------------------------------------------------------------------------
struct Biquad
{
    double b0, b1, b2, a1, a2;
};

// scipy.signal.butter(4, ..., output="sos") for a band-pass [lo, hi] or (hi <= 0) a high-pass at lo: the same zeros,
// poles and gain (bilinear, prewarped), as sections. Any pairing of them is the same filter.
std::vector<Biquad> butterworth4(double fs, double lo, double hi)
{
    constexpr int order = 4;
    constexpr double fsi = 2.0; // scipy designs at a normalised rate of 2
    using C = std::complex<double>;
    const auto warp = [&](double f) { return 2.0 * fsi * std::tan(pi * (2.0 * f / fs) / fsi); };
    std::vector<C> prototype;
    for (int m = -order + 1; m < order; m += 2)
        prototype.push_back(-std::exp(C(0.0, pi * m / (2.0 * order))));

    std::vector<C> poles;
    double gain = 1.0;
    const auto bandPass = hi > 0.0;
    if (bandPass)
    {
        const auto w1 = warp(lo), w2 = warp(hi);
        const auto bw = w2 - w1, wo = std::sqrt(w1 * w2);
        std::vector<C> lower;
        for (const auto& p : prototype)
        {
            const auto plp = p * bw / 2.0;
            const auto root = std::sqrt(plp * plp - wo * wo);
            poles.push_back(plp + root);
            lower.push_back(plp - root);
        }
        poles.insert(poles.end(), lower.begin(), lower.end());
        gain = std::pow(bw, order);
    }
    else
    {
        const auto wo = warp(lo);
        C product = 1.0;
        for (const auto& p : prototype)
        {
            poles.push_back(wo / p);
            product *= -p;
        }
        gain = (1.0 / product).real();
    }
    // Zeros at s = 0 (as many as the order) go to z = +1; the band-pass's other zeros (at infinity) to z = -1.
    constexpr double fs2 = 2.0 * fsi;
    C denominator = 1.0;
    std::vector<C> digital;
    for (const auto& p : poles)
    {
        denominator *= fs2 - p;
        digital.push_back((fs2 + p) / (fs2 - p));
    }
    gain *= (std::pow(fs2, order) / denominator).real();

    std::vector<Biquad> sections;
    for (const auto& p : digital)
        if (p.imag() > 0.0)
            sections.push_back(bandPass ? Biquad{1.0, 0.0, -1.0, -2.0 * p.real(), std::norm(p)}
                                        : Biquad{1.0, -2.0, 1.0, -2.0 * p.real(), std::norm(p)});
    if (sections.size() * 2 != digital.size())
        throw std::runtime_error("unpaired pole");
    for (auto* b : {&sections[0].b0, &sections[0].b1, &sections[0].b2})
        *b *= gain;
    return sections;
}

struct AttackBand
{
    double lo, hi, smoothMs;
};
constexpr AttackBand attackBands[] = {{35.0, 150.0, 12.0}, {150.0, 600.0, 5.0}, {600.0, 0.0, 1.5}};
constexpr double attackRunnerUp = 0.65, attackSearchMs = 40.0;

// One band's feature: the log of the smoothed magnitude, its positive differences.
std::vector<float> attackFeature(const float* x, int length, double fs, const AttackBand& band)
{
    const auto sections = butterworth4(fs, band.lo, band.hi > 0.0 ? std::min(band.hi, fs * 0.45) : 0.0);
    std::vector<double> state(sections.size() * 2, 0.0);
    std::vector<double> magnitude((size_t)length);
    for (int i = 0; i < length; ++i)
    {
        double v = x[i];
        for (size_t s = 0; s < sections.size(); ++s)
        {
            const auto& q = sections[s];
            auto& z1 = state[2 * s];
            auto& z2 = state[2 * s + 1];
            const auto out = q.b0 * v + z1;
            z1 = q.b1 * v - q.a1 * out + z2;
            z2 = q.b2 * v - q.a2 * out;
            v = out;
        }
        magnitude[(size_t)i] = std::abs(v);
    }

    // np.convolve(m, ones(n) / n, "same"): the window [i - n/2, i + (n-1)/2], zeros outside.
    const auto n = std::max(1, roundHalfEven(band.smoothMs * 1e-3 * fs));
    std::vector<double> prefix((size_t)length + 1, 0.0);
    for (int i = 0; i < length; ++i)
        prefix[(size_t)i + 1] = prefix[(size_t)i] + magnitude[(size_t)i];
    std::vector<float> feature((size_t)length);
    double previous = 0.0;
    for (int i = 0; i < length; ++i)
    {
        const auto first = std::max(0, i - n / 2), end = std::min(length, i + (n - 1) / 2 + 1);
        const auto smoothed = (prefix[(size_t)end] - prefix[(size_t)first]) / n;
        const auto lg = std::log(smoothed + 1e-6);
        feature[(size_t)i] = i == 0 ? 0.0f : (float)std::max(lg - previous, 0.0);
        previous = lg;
    }
    return feature;
}

// c[m] = sum_t a[t + m] b[t] for m in [-reach, reach], linear (not circular), in blocks through one FFT size.
std::vector<double> crossCorrelation(const std::vector<double>& a, const std::vector<double>& b, int reach)
{
    const auto length = (int)a.size();
    int size = 1 << 16;
    while (size < 4 * reach)
        size <<= 1;
    const auto block = size - 2 * reach;
    Fft fa(size), fb(size);
    std::vector<double> c((size_t)(2 * reach + 1), 0.0);
    for (int t0 = 0; t0 < length; t0 += block)
    {
        auto* pa = fa.data();
        auto* pb = fb.data();
        for (int u = 0; u < size; ++u)
        {
            const auto ia = t0 - reach + u;
            pa[u] = u < block + 2 * reach && ia >= 0 && ia < length ? a[(size_t)ia] : 0.0;
            const auto ib = t0 + u;
            pb[u] = u < block && ib < length ? b[(size_t)ib] : 0.0;
        }
        fa.forward();
        fb.forward();
        pa[0] *= pb[0]; // DC
        pa[1] *= pb[1]; // Nyquist
        for (int k = 1; k < size / 2; ++k)
        {
            const std::complex<double> x(pa[2 * k], pa[2 * k + 1]), y(pb[2 * k], pb[2 * k + 1]);
            const auto p = x * std::conj(y);
            pa[2 * k] = p.real();
            pa[2 * k + 1] = p.imag();
        }
        fa.inverse();
        for (int m = -reach; m <= reach; ++m)
            c[(size_t)(m + reach)] += pa[reach + m] / size;
    }
    return c;
}

// --- suggest()
// --------------------------------------------------------------------------------------------------------
enum class How
{
    delayOff,
    attack,
    score
};

double lowEndChange(const Spectra& sp, const Candidate& c)
{
    const auto change = scoreAt(sp, c, lowEndHz) - scoreAt(sp, nothingChanged(), lowEndHz);
    return std::isnan(change) ? 0.0 : change;
}

Result suggest(const Spectra& sp, const AttackReading& lag, Scope scope, const std::atomic<bool>* cancel)
{
    Result r;
    r.attack = lag;
    if (! sp.valid())
    {
        r.verdict = Verdict::noSignal;
        return r;
    }
    SearchOptions o;
    o.phaseOn = scope.phaseOn;
    How how = How::score;
    if (! scope.delayOn)
    {
        o.reachMs = 0.0;
        how = How::delayOff;
    }
    else if (lag.clear && std::abs(lag.lagMs) <= maxDelayMs)
    {
        o.windowed = true;
        o.windowLoMs = lag.lagMs - 0.3;
        o.windowHiMs = lag.lagMs + 0.3;
        how = How::attack;
    }
    auto families = search(sp, o, cancel);
    r.baseline = scoreAt(sp, nothingChanged());

    const auto strong = how == How::attack && lag.peak >= attackStrong;
    if (how == How::attack && ! strong)
    {
        o.windowed = false;
        families = search(sp, o, cancel);
    }
    for (const auto& c : families)
    {
        if (c.score - r.baseline >= minGain)
            r.options.push_back({c, c.score - r.baseline, lowEndChange(sp, c), strong, false});
        if (r.options.size() == 2)
            break;
    }
    if (r.options.size() == 2 && r.options[0].candidate.score - r.options[1].candidate.score >= minMargin)
        r.options.resize(1);
    if (! r.options.empty())
    {
        r.verdict = r.options.size() == 1 ? Verdict::suggest : Verdict::close;
        for (const auto& option : r.options)
            r.lessLowEnd = r.lessLowEnd || option.lowEndChange < 0.0;
        return r;
    }
    if (scope.delayOn && lag.clear && std::abs(lag.lagMs) >= minDelayMs && std::abs(lag.lagMs) <= maxDelayMs)
    {
        Candidate c;
        c.delayMs = lag.lagMs;
        c.score = r.baseline;
        const auto lf = lowEndChange(sp, c);
        r.options.push_back({c, 0.0, lf, true, true});
        r.verdict = Verdict::delayOnly;
        return r;
    }
    r.verdict = Verdict::nothing;
    return r;
}

std::string signedInt(int v)
{
    return (v >= 0 ? "+" : "") + std::to_string(v);
}
} // namespace

// --- public
// -----------------------------------------------------------------------------------------------------------
dsp::ChainSettings Candidate::toSettings(double sampleRate) const
{
    dsp::ChainSettings s;
    s.delayTenths = (int)std::lround(delayMs * sampleRate / 100.0);
    s.delayOn = s.delayTenths != 0;
    s.polarityInverted = flip;
    s.phaseOn = mode != Mode::none;
    s.phaseMode = chainMode(mode);
    s.phaseWide = wide || mode == Mode::constant;
    s.phaseDegrees = theta;
    return s;
}

Spectra Spectra::compute(const float* x, const float* y, int length, double fs)
{
    Spectra sp;
    sp.fs = fs;
    sp.n = frameSize(fs);
    const auto n = sp.n, bins = n / 2 + 1, hop = n / 4;
    sp.sxy.assign((size_t)bins, 0.0);
    sp.sxx.assign((size_t)bins, 0.0);
    sp.syy.assign((size_t)bins, 0.0);

    std::vector<double> window((size_t)n);
    for (int i = 0; i < n; ++i)
        window[(size_t)i] = 0.5 - 0.5 * std::cos(2.0 * pi * i / (n - 1)); // np.hanning
    Fft fx(n), fy(n);
    const auto count = length >= n ? (length - n) / hop + 1 : 0;
    const auto bin = [](const double* p, int k, int size) -> std::complex<double>
    {
        return k == 0 ? p[0] : k == size / 2 ? p[1] : std::complex<double>(p[2 * k], p[2 * k + 1]);
    };
    for (int f = 0; f < count; ++f)
    {
        for (int i = 0; i < n; ++i)
        {
            fx.data()[i] = window[(size_t)i] * x[f * hop + i];
            fy.data()[i] = window[(size_t)i] * y[f * hop + i];
        }
        fx.forward();
        fy.forward();
        for (int k = 0; k < bins; ++k)
        {
            const auto a = bin(fx.data(), k, n), b = bin(fy.data(), k, n);
            sp.sxy[(size_t)k] += a * std::conj(b);
            sp.sxx[(size_t)k] += std::norm(a);
            sp.syy[(size_t)k] += std::norm(b);
        }
    }

    // 1/3-octave bands from 31.5 Hz up, kept between 40 Hz and 16 kHz (10% either side) and below Nyquist.
    const auto third = std::pow(2.0, 1.0 / 3.0), sixth = std::pow(2.0, 1.0 / 6.0);
    std::vector<std::pair<int, int>> edges;
    std::vector<double> centres;
    for (double f = 31.5; f < bandHiHz * 1.1; f *= third)
    {
        const auto lo = f / sixth, hi = f * sixth;
        if (lo >= bandLoHz * 0.9 && hi <= std::min(bandHiHz * 1.1, fs / 2.0))
        {
            int first = -1, last = -1;
            for (int k = 0; k < bins; ++k)
                if (sp.binHz(k) >= lo && sp.binHz(k) < hi)
                {
                    first = first < 0 ? k : first;
                    last = k;
                }
            if (first >= 0 && last - first + 1 >= 2)
            {
                centres.push_back(f);
                edges.push_back({first, last + 1});
            }
        }
    }
    std::vector<double> px, py;
    for (const auto& [a, b] : edges)
    {
        double sx = 0.0, sy = 0.0;
        for (int k = a; k < b; ++k)
        {
            sx += sp.sxx[(size_t)k];
            sy += sp.syy[(size_t)k];
        }
        px.push_back(sx);
        py.push_back(sy);
    }
    const auto maxX = px.empty() ? 0.0 : *std::max_element(px.begin(), px.end());
    const auto maxY = py.empty() ? 0.0 : *std::max_element(py.begin(), py.end());
    if (maxX <= 0.0 || maxY <= 0.0)
        return sp; // silence in one of them: no bands, nothing to score
    const auto floor = std::pow(10.0, activeDb / 10.0);
    for (size_t b = 0; b < edges.size(); ++b)
        if (px[b] >= maxX * floor && py[b] >= maxY * floor)
        {
            sp.edges.push_back(edges[b]);
            sp.centres.push_back(centres[b]);
            sp.norms.push_back(std::sqrt(px[b] * py[b]));
        }
    return sp;
}

double scoreAt(const Spectra& sp, const Candidate& c, double belowHz)
{
    const auto h = stageResponse(sp, c.mode, c.wide, c.theta);
    return scoreWith(sp, h, c.flip ? -1.0 : 1.0, c.delayMs * 1e-3 * sp.fs, belowHz);
}

double bandCorrelation(const Spectra& sp, const Candidate& c, double loHz, double hiHz)
{
    const dsp::PhaseStageResponse response(c.toSettings(sp.fs), sp.fs);
    const auto d = c.delayMs * 1e-3 * sp.fs;
    double sum = 0.0, sx = 0.0, sy = 0.0;
    for (int k = 1; k < (int)sp.sxy.size(); ++k)
    {
        const auto hz = sp.binHz(k);
        if (hz < loHz || hz >= hiHz)
            continue;
        const auto w = -2.0 * pi * hz * d / sp.fs;
        const auto g = (c.flip ? -1.0 : 1.0) * response(hz) * std::complex<double>(std::cos(w), std::sin(w));
        sum += (g * sp.sxy[(size_t)k]).real();
        sx += sp.sxx[(size_t)k];
        sy += sp.syy[(size_t)k];
    }
    return sx > 0.0 && sy > 0.0 ? sum / std::sqrt(sx * sy) : std::numeric_limits<double>::quiet_NaN();
}

std::vector<Candidate> search(const Spectra& sp, const SearchOptions& o, const std::atomic<bool>* cancel)
{
    if (! sp.valid())
        return {};
    const auto reach = (int)std::ceil(o.reachMs * 1e-3 * sp.fs) * pad;
    const auto lo = (o.windowed ? std::max(o.windowLoMs, -o.reachMs) : -o.reachMs) * 1e-3 * sp.fs;
    const auto hi = (o.windowed ? std::min(o.windowHiMs, o.reachMs) : o.reachMs) * 1e-3 * sp.fs;

    // The band mean of r at every lag is one inverse FFT of the spectrum weighted per band.
    auto size = 1;
    while (size < sp.n * pad)
        size <<= 1;
    Fft fft(size);
    const auto bandWeight = 1.0 / (double)sp.edges.size();

    struct Entry
    {
        double score;
        size_t setting;
        double lag; // samples
        bool flip;
    };
    std::vector<Entry> best;
    const auto all = settings(o.phaseOn);
    std::vector<std::pair<double, int>> ranked;
    for (size_t s = 0; s < all.size(); ++s)
    {
        checkCancel(cancel);
        const auto h = stageResponse(sp, all[s].mode, all[s].wide, all[s].theta);
        auto* p = fft.data();
        std::fill(p, p + size, 0.0);
        for (size_t b = 0; b < sp.edges.size(); ++b)
            for (int k = sp.edges[b].first; k < sp.edges[b].second; ++k)
            {
                const auto v = h[(size_t)k] * sp.sxy[(size_t)k] * (bandWeight / sp.norms[b]);
                p[2 * k] = v.real();
                p[2 * k + 1] = v.imag();
            }
        fft.inverse(); // p[m] = 2 sum_k Re(V_k e^(j 2 pi k m / size))
        for (const auto sign : {1.0, -1.0})
        {
            ranked.clear();
            for (int m = -reach; m <= reach; ++m)
            {
                const auto lag = m / (double)pad;
                if (lag >= lo && lag <= hi)
                    ranked.push_back({sign * 0.5 * p[(size - m) % size], m});
            }
            const auto keep = std::min<size_t>(3, ranked.size());
            std::partial_sort(ranked.begin(), ranked.begin() + (long)keep, ranked.end(),
                              [](const auto& a, const auto& b) { return a.first > b.first; });
            for (size_t j = 0; j < keep; ++j)
                best.push_back({ranked[j].first, s, ranked[j].second / (double)pad, sign < 0.0});
        }
    }
    std::stable_sort(best.begin(), best.end(), [](const Entry& a, const Entry& b) { return a.score > b.score; });

    // Refine the leaders on the knob's 0.1-sample grid.
    std::vector<Candidate> candidates;
    for (size_t i = 0; i < std::min<size_t>(refineLeaders, best.size()); ++i)
    {
        checkCancel(cancel);
        const auto& e = best[i];
        const auto& setting = all[e.setting];
        const auto h = stageResponse(sp, setting.mode, setting.wide, setting.theta);
        const auto sign = e.flip ? -1.0 : 1.0;
        auto bestScore = -9.0, bestDelay = e.lag;
        const auto tenths = std::floor(e.lag * 10.0 + 0.5);
        for (int j = -3; j <= 3; ++j)
        {
            const auto d = (tenths + j) / 10.0;
            if (d < lo - 1e-9 || d > hi + 1e-9)
                continue;
            const auto score = scoreWith(sp, h, sign, d);
            if (score > bestScore || (! (score < bestScore) && d > bestDelay)) // the Python's max of (score, delay)
            {
                bestScore = score;
                bestDelay = d;
            }
        }
        candidates.push_back({setting.mode, setting.wide, setting.theta, bestDelay / sp.fs * 1e3, e.flip, bestScore});
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) { return a.score > b.score; });

    std::vector<Candidate> families;
    for (const auto& c : candidates)
    {
        const auto same = [&](const Candidate& f) { return sameFamily(sp, f, c); };
        if (std::none_of(families.begin(), families.end(), same))
            families.push_back(c);
        if ((int)families.size() == o.top)
            break;
    }
    return families;
}

AttackReading attackLag(const float* x, const float* y, int length, double fs)
{
    const auto reach = (int)(attackSearchMs * 1e-3 * fs);
    std::vector<double> acc((size_t)(2 * reach + 1), 0.0);
    constexpr auto numBands = (int)std::size(attackBands);
    for (const auto& band : attackBands)
    {
        const auto fx = attackFeature(x, length, fs, band);
        const auto fy = attackFeature(y, length, fs, band);
        double meanA = 0.0, meanB = 0.0;
        for (int i = 0; i < length; ++i)
        {
            meanA += fx[(size_t)i];
            meanB += fy[(size_t)i];
        }
        meanA /= length;
        meanB /= length;
        std::vector<double> a((size_t)length), b((size_t)length);
        double energyA = 0.0, energyB = 0.0;
        for (int i = 0; i < length; ++i)
        {
            a[(size_t)i] = fx[(size_t)i] - meanA;
            b[(size_t)i] = fy[(size_t)i] - meanB;
            energyA += a[(size_t)i] * a[(size_t)i];
            energyB += b[(size_t)i] * b[(size_t)i];
        }
        const auto norm = std::sqrt(energyA * energyB);
        if (! (norm > 0.0))
            continue;
        const auto c = crossCorrelation(a, b, reach);
        for (size_t i = 0; i < c.size(); ++i)
            acc[i] += c[i] / norm / numBands;
    }
    // c's lag m > 0 means the input is later, so the delay that lines it up is d = -m: reversed, index i is d = i -
    // reach.
    std::reverse(acc.begin(), acc.end());
    const auto k = (int)(std::max_element(acc.begin(), acc.end()) - acc.begin());
    AttackReading r;
    r.peak = acc[(size_t)k];
    const auto lobe = (int)(0.004 * fs);
    double rival = -std::numeric_limits<double>::infinity();
    bool any = false;
    for (int i = 0; i < (int)acc.size(); ++i)
        if (i < std::max(0, k - lobe) || i >= k + lobe)
        {
            rival = std::max(rival, acc[(size_t)i]);
            any = true;
        }
    r.runnerUp = r.peak > 0.0 && any ? rival / r.peak : 1.0;
    double frac = 0.0;
    if (k > 0 && k < (int)acc.size() - 1)
    {
        const auto y0 = acc[(size_t)k - 1], y1 = acc[(size_t)k], y2 = acc[(size_t)k + 1];
        const auto den = y0 - 2.0 * y1 + y2;
        frac = den != 0.0 ? 0.5 * (y0 - y2) / den : 0.0;
    }
    r.lagMs = ((k - reach) + frac) / fs * 1e3;
    r.clear = r.peak > 0.0 && r.runnerUp < attackRunnerUp;
    return r;
}

Result suggestWithShift(const float* x, const float* y, int length, double fs, Scope scope,
                        const std::atomic<bool>* cancel)
{
    try
    {
        const auto sp = Spectra::compute(x, y, length, fs);
        checkCancel(cancel);
        const auto lag = attackLag(x, y, length, fs);
        checkCancel(cancel);
        auto r = suggest(sp, lag, scope, cancel);
        if (r.verdict == Verdict::noSignal)
            return r;
        if (! scope.delayOn)
        {
            if (lag.clear && std::abs(lag.lagMs) >= minDelayMs)
                r.message = "DELAY is off: the transients are " + signedInt(roundHalfEven(lag.lagMs * 1e-3 * fs)) +
                            " samples apart";
            return r;
        }
        int shift = 0;
        if (lag.clear && std::abs(lag.lagMs) > maxDelayMs)
            shift = roundHalfEven(lag.lagMs * 1e-3 * fs);
        else if (! r.options.empty() && std::abs(r.options[0].candidate.delayMs) >= maxDelayMs - 0.15)
            shift = roundHalfEven(r.options[0].candidate.delayMs * 1e-3 * fs);
        else
        {
            SearchOptions o;
            o.top = 1;
            o.phaseOn = scope.phaseOn;
            o.reachMs = wideReachMs;
            const auto wide = search(sp, o, cancel);
            const auto inside = r.options.empty() ? r.baseline : r.options[0].candidate.score;
            if (! wide.empty() && std::abs(wide[0].delayMs) > maxDelayMs && wide[0].score - inside >= minGain)
                shift = roundHalfEven(wide[0].delayMs * 1e-3 * fs);
        }
        if (shift == 0)
            return r;

        // The track as it will be once shifted by hand (positive delays it), and the options for that.
        std::vector<float> shifted((size_t)length, 0.0f);
        for (int i = 0; i < length; ++i)
            if (i - shift >= 0 && i - shift < length)
                shifted[(size_t)i] = x[i - shift];
        const auto spShifted = Spectra::compute(shifted.data(), y, length, fs);
        checkCancel(cancel);
        const auto lagShifted = attackLag(shifted.data(), y, length, fs);
        auto after = suggest(spShifted, lagShifted, scope, cancel);
        after.baseline = r.baseline;
        after.attack = lag;
        after.shiftSamples = shift;
        after.message =
            "Transient may be out of range, consider shifting " + signedInt(shift) + " samples manually if needed";
        return after;
    }
    catch (const Cancelled&)
    {
        Result r;
        r.cancelled = true;
        return r;
    }
}

std::string describe(const Candidate& c)
{
    char text[96];
    char phase[48];
    if (c.mode == Mode::none)
        std::snprintf(phase, sizeof(phase), "phase off");
    else if (c.mode == Mode::constant)
        std::snprintf(phase, sizeof(phase), "CONSTANT %.1f\xc2\xb0", c.theta);
    else
        std::snprintf(phase, sizeof(phase), "%s %s %.1f\xc2\xb0", c.mode == Mode::lo ? "LO" : "HI",
                      c.wide ? "in" : "out", c.theta);
    std::snprintf(text, sizeof(text), "%s, %sdelay %+.2f ms", phase, c.flip ? "\xc3\x98 " : "", c.delayMs);
    return text;
}
} // namespace pa::analyse
