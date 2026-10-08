#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

// Define PA_REAL_FFT_NO_VDSP to use the other engine (PFFFT) on Apple too (to measure it there).
#if (defined(__APPLE__)) && ! defined(PA_REAL_FFT_NO_VDSP)
#include <Accelerate/Accelerate.h>
#define PA_REAL_FFT_VDSP 1
#else
#include <pffft/pffft.h>
#define PA_REAL_FFT_VDSP 0
#endif

// A real FFT in the packed split format the convolver works in: n real samples become n/2
// bins in two arrays, re[0] = DC and im[0] = Nyquist (both real), (re[k], im[k]) = bin k for 0 < k < n/2. Each engine
// has its own scale, so the transforms are only exact up to convolutionScale(): for kernel h and input x,
// inverse(forward(h) * convolutionScale() * forward(x)) is their circular convolution (bin 0's DC and Nyquist parts
// multiplied separately).
//
// Engines: vDSP on Apple (directly, in the packed split format it uses natively; JUCE's wrapper adds a strided layout
// and two extra passes). Elsewhere PFFFT (libs/pffft, BSD-style licence; SSE on x86, NEON on ARM): its ordered real
// transform is the same packing interleaved, so one pass splits or joins it. PFFFT wants 16-byte aligned buffers, so
// unaligned input or output goes through its own (n >= 32, a power of two here).
//
// prepare() allocates; forward() and inverse() don't.
namespace pa::dsp
{
class RealFft
{
  public:
    void prepare(int orderIn)
    {
        order = orderIn;
        n = 1 << order;
#if PA_REAL_FFT_VDSP
        setup.reset(vDSP_create_fftsetup((vDSP_Length)order, kFFTRadix2));
#else
        setup.reset(pffft_new_setup(n, PFFFT_REAL));
        buffer.reset(static_cast<float*>(pffft_aligned_malloc((size_t)n * sizeof(float))));
        work.reset(static_cast<float*>(pffft_aligned_malloc((size_t)n * sizeof(float))));
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
        float* b = buffer.get();
        pffft_transform_ordered(setup.get(), aligned(in) ? in : copyIn(in), b, work.get(), PFFFT_FORWARD);
        for (int k = 0; k < n / 2; ++k) // b[0] = DC, b[1] = Nyquist: the packing re[0], im[0] already
        {
            re[k] = b[2 * k];
            im[k] = b[2 * k + 1];
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
        float* b = buffer.get();
        for (int k = 0; k < n / 2; ++k)
        {
            b[2 * k] = re[k];
            b[2 * k + 1] = im[k];
        }
        if (aligned(out))
            pffft_transform_ordered(setup.get(), b, out, work.get(), PFFFT_BACKWARD);
        else
        {
            pffft_transform_ordered(setup.get(), b, b, work.get(), PFFFT_BACKWARD);
            std::copy(b, b + n, out);
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
    static bool aligned(const float* p) { return (reinterpret_cast<std::uintptr_t>(p) & 15u) == 0; }

    // Unaligned input: into the buffer, which the transform then reads and overwrites in place.
    const float* copyIn(const float* in)
    {
        std::copy(in, in + n, buffer.get());
        return buffer.get();
    }

    struct SetupDeleter
    {
        void operator()(PFFFT_Setup* s) const { pffft_destroy_setup(s); }
    };
    struct AlignedDeleter
    {
        void operator()(float* p) const { pffft_aligned_free(p); }
    };
    std::unique_ptr<PFFFT_Setup, SetupDeleter> setup;
    std::unique_ptr<float, AlignedDeleter> buffer, work; // n floats each, 16-byte aligned
#endif
};
} // namespace pa::dsp
