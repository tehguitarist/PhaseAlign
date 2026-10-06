#pragma once

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <memory>
#include <vector>

// Define PA_REAL_FFT_NO_VDSP to use the generic engine on Apple too (to measure it there).
#if (JUCE_MAC || JUCE_IOS) && ! defined(PA_REAL_FFT_NO_VDSP)
#include <Accelerate/Accelerate.h>
#define PA_REAL_FFT_VDSP 1
#else
#define PA_REAL_FFT_VDSP 0
#endif

// A real FFT in the packed split format the convolver works in (IMPLEMENTATION_PLAN 2.6): n real samples become n/2
// bins in two arrays, re[0] = DC and im[0] = Nyquist (both real), (re[k], im[k]) = bin k for 0 < k < n/2. Each engine
// has its own scale, so the transforms are only exact up to convolutionScale(): for kernel h and input x,
// inverse(forward(h) * convolutionScale() * forward(x)) is their circular convolution (bin 0's DC and Nyquist parts
// multiplied separately).
//
// Engines: vDSP on Apple (directly, in the packed split format it uses natively; JUCE's wrapper adds a strided layout
// and two extra passes). Elsewhere, juce::dsp::FFT's complex transform of n/2 points with the usual split into even and
// odd samples and one twiddle pass (what vDSP does inside): JUCE's own real transform is a complex one of n points,
// about twice the work. A faster engine for Windows/Linux would go here behind the same interface (plan 2.6).
//
// prepare() allocates; forward() and inverse() don't.
namespace pa::dsp
{
class RealFft
{
  public:
    // True for vDSP; false for the generic engine (juce::dsp::FFT, much slower for large transforms).
    static constexpr bool isNative = PA_REAL_FFT_VDSP != 0;

    void prepare(int orderIn)
    {
        order = orderIn;
        n = 1 << order;
#if PA_REAL_FFT_VDSP
        setup.reset(vDSP_create_fftsetup((vDSP_Length)order, kFFTRadix2));
#else
        const auto m = n / 2;
        fft = std::make_unique<juce::dsp::FFT>(order - 1);
        z.assign((size_t)m, {});
        spectrum.assign((size_t)m, {});
        twiddle.resize((size_t)m);
        for (int k = 0; k < m; ++k)
            twiddle[(size_t)k] = std::polar(1.0f, (float)(-2.0 * juce::MathConstants<double>::pi * k / n));
#endif
        // The engine's scales, measured: forward's gain on a unit impulse, and inverse(forward(x)) / x.
        std::vector<float> x((size_t)n, 0.0f), re((size_t)(n / 2)), im((size_t)(n / 2)), back((size_t)n);
        x[1] = 1.0f;
        forward(x.data(), re.data(), im.data());
        const auto gain = std::hypot((double)re[1], (double)im[1]);
        inverse(re.data(), im.data(), back.data());
        convolutionScaleValue = (float)(1.0 / (gain * (double)back[1]));
    }

    int size() const { return n; }

    float convolutionScale() const { return convolutionScaleValue; }

    // n real samples into n/2 packed bins.
    void forward(const float* in, float* re, float* im)
    {
#if PA_REAL_FFT_VDSP
        DSPSplitComplex split{re, im};
        vDSP_ctoz(reinterpret_cast<const DSPComplex*>(in), 2, &split, 1, (vDSP_Length)(n / 2));
        vDSP_fft_zrip(setup.get(), &split, 1, (vDSP_Length)order, kFFTDirection_Forward);
#else
        // z = even + j odd; Z = DFT(z); X[k] = E[k] + w^k O[k] with E[k] = (Z[k] + Z*[m-k]) / 2 and
        // O[k] = (Z[k] - Z*[m-k]) / 2j.
        const auto m = n / 2;
        for (int k = 0; k < m; ++k)
            z[(size_t)k] = {in[2 * k], in[2 * k + 1]};
        fft->perform(z.data(), spectrum.data(), false);
        re[0] = spectrum[0].real() + spectrum[0].imag(); // DC
        im[0] = spectrum[0].real() - spectrum[0].imag(); // Nyquist
        for (int k = 1; k < m; ++k)
        {
            const auto a = spectrum[(size_t)k], b = std::conj(spectrum[(size_t)(m - k)]);
            const auto even = 0.5f * (a + b), odd = Complex(0.0f, -0.5f) * (a - b);
            const auto x = even + twiddle[(size_t)k] * odd;
            re[k] = x.real();
            im[k] = x.imag();
        }
#endif
    }

    // n/2 packed bins (overwritten) into n real samples.
    void inverse(float* re, float* im, float* out)
    {
#if PA_REAL_FFT_VDSP
        DSPSplitComplex split{re, im};
        vDSP_fft_zrip(setup.get(), &split, 1, (vDSP_Length)order, kFFTDirection_Inverse);
        vDSP_ztoc(&split, 1, reinterpret_cast<DSPComplex*>(out), 2, (vDSP_Length)(n / 2));
#else
        // The forward steps backwards: E[k] = (X[k] + X*[m-k]) / 2, O[k] = (X[k] - X*[m-k]) w^-k / 2, Z = E + j O,
        // z = IDFT(Z) = even + j odd.
        const auto m = n / 2;
        const auto bin = [&](int k)
        {
            return k == 0 ? Complex(re[0], 0.0f) : k == m ? Complex(im[0], 0.0f) : Complex(re[k], im[k]);
        };
        for (int k = 0; k < m; ++k)
        {
            const auto a = bin(k), b = std::conj(bin(m - k));
            const auto even = 0.5f * (a + b), odd = 0.5f * (a - b) * std::conj(twiddle[(size_t)k]);
            spectrum[(size_t)k] = even + Complex(0.0f, 1.0f) * odd;
        }
        fft->perform(spectrum.data(), z.data(), true);
        for (int k = 0; k < m; ++k)
        {
            out[2 * k] = z[(size_t)k].real();
            out[2 * k + 1] = z[(size_t)k].imag();
        }
#endif
    }

  private:
    int order = 1, n = 2;
    float convolutionScaleValue = 1.0f;
#if PA_REAL_FFT_VDSP
    struct SetupDeleter
    {
        void operator()(OpaqueFFTSetup* s) const { vDSP_destroy_fftsetup(s); }
    };
    std::unique_ptr<OpaqueFFTSetup, SetupDeleter> setup;
#else
    using Complex = juce::dsp::Complex<float>;
    std::unique_ptr<juce::dsp::FFT> fft;
    std::vector<Complex> z, spectrum, twiddle;
#endif
};
} // namespace pa::dsp
