#pragma once

// Four floats (F4) or two doubles (D2) at a time, for loops a compiler may vectorise along the wrong axis (or not at
// all, MSVC): NEON on ARM, SSE2 on x86, plain arithmetic elsewhere. Only what the filters need. madd is fused on ARM
// (one instruction) and a multiply then an add elsewhere, so the last bit can differ between platforms.
#if defined(__ARM_NEON) || defined(_M_ARM64)
#include <arm_neon.h>
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define PA_SIMD_SSE 1
#endif

namespace pa::dsp::simd
{
#if defined(__ARM_NEON) || defined(_M_ARM64)
using F4 = float32x4_t;
inline F4 load(const float* p)
{
    return vld1q_f32(p);
}
inline void store(float* p, F4 a)
{
    vst1q_f32(p, a);
}
inline F4 splat(float x)
{
    return vdupq_n_f32(x);
}
inline F4 zero()
{
    return vdupq_n_f32(0.0f);
}
inline F4 add(F4 a, F4 b)
{
    return vaddq_f32(a, b);
}
inline F4 sub(F4 a, F4 b)
{
    return vsubq_f32(a, b);
}
inline F4 mul(F4 a, F4 b)
{
    return vmulq_f32(a, b);
}
inline F4 madd(F4 acc, F4 a, F4 b)
{
    return vfmaq_f32(acc, a, b);
}

using D2 = float64x2_t;
inline D2 pair(double a, double b)
{
    return vcombine_f64(vdup_n_f64(a), vdup_n_f64(b));
}
inline D2 splat2(double x)
{
    return vdupq_n_f64(x);
}
inline double lane0(D2 v)
{
    return vgetq_lane_f64(v, 0);
}
inline double lane1(D2 v)
{
    return vgetq_lane_f64(v, 1);
}
inline D2 add(D2 a, D2 b)
{
    return vaddq_f64(a, b);
}
inline D2 sub(D2 a, D2 b)
{
    return vsubq_f64(a, b);
}
inline D2 mul(D2 a, D2 b)
{
    return vmulq_f64(a, b);
}
inline D2 madd(D2 acc, D2 a, D2 b)
{
    return vfmaq_f64(acc, a, b);
}
inline D2 msub(D2 acc, D2 a, D2 b)
{
    return vfmsq_f64(acc, a, b);
} // acc - a b
#elif defined(PA_SIMD_SSE)
using F4 = __m128;
inline F4 load(const float* p)
{
    return _mm_loadu_ps(p);
}
inline void store(float* p, F4 a)
{
    _mm_storeu_ps(p, a);
}
inline F4 splat(float x)
{
    return _mm_set1_ps(x);
}
inline F4 zero()
{
    return _mm_setzero_ps();
}
inline F4 add(F4 a, F4 b)
{
    return _mm_add_ps(a, b);
}
inline F4 sub(F4 a, F4 b)
{
    return _mm_sub_ps(a, b);
}
inline F4 mul(F4 a, F4 b)
{
    return _mm_mul_ps(a, b);
}
inline F4 madd(F4 acc, F4 a, F4 b)
{
    return _mm_add_ps(acc, _mm_mul_ps(a, b));
}

using D2 = __m128d;
inline D2 pair(double a, double b)
{
    return _mm_set_pd(b, a);
}
inline D2 splat2(double x)
{
    return _mm_set1_pd(x);
}
inline double lane0(D2 v)
{
    return _mm_cvtsd_f64(v);
}
inline double lane1(D2 v)
{
    return _mm_cvtsd_f64(_mm_unpackhi_pd(v, v));
}
inline D2 add(D2 a, D2 b)
{
    return _mm_add_pd(a, b);
}
inline D2 sub(D2 a, D2 b)
{
    return _mm_sub_pd(a, b);
}
inline D2 mul(D2 a, D2 b)
{
    return _mm_mul_pd(a, b);
}
inline D2 madd(D2 acc, D2 a, D2 b)
{
    return _mm_add_pd(acc, _mm_mul_pd(a, b));
}
inline D2 msub(D2 acc, D2 a, D2 b)
{
    return _mm_sub_pd(acc, _mm_mul_pd(a, b));
}
#else
struct F4
{
    float v[4];
};
inline F4 load(const float* p)
{
    return {{p[0], p[1], p[2], p[3]}};
}
inline void store(float* p, F4 a)
{
    for (int i = 0; i < 4; ++i)
        p[i] = a.v[i];
}
inline F4 splat(float x)
{
    return {{x, x, x, x}};
}
inline F4 zero()
{
    return splat(0.0f);
}
inline F4 add(F4 a, F4 b)
{
    return {{a.v[0] + b.v[0], a.v[1] + b.v[1], a.v[2] + b.v[2], a.v[3] + b.v[3]}};
}
inline F4 sub(F4 a, F4 b)
{
    return {{a.v[0] - b.v[0], a.v[1] - b.v[1], a.v[2] - b.v[2], a.v[3] - b.v[3]}};
}
inline F4 mul(F4 a, F4 b)
{
    return {{a.v[0] * b.v[0], a.v[1] * b.v[1], a.v[2] * b.v[2], a.v[3] * b.v[3]}};
}
inline F4 madd(F4 acc, F4 a, F4 b)
{
    for (int i = 0; i < 4; ++i)
        acc.v[i] += a.v[i] * b.v[i];
    return acc;
}

struct D2
{
    double v[2];
};
inline D2 pair(double a, double b)
{
    return {{a, b}};
}
inline D2 splat2(double x)
{
    return {{x, x}};
}
inline double lane0(D2 v)
{
    return v.v[0];
}
inline double lane1(D2 v)
{
    return v.v[1];
}
inline D2 add(D2 a, D2 b)
{
    return {{a.v[0] + b.v[0], a.v[1] + b.v[1]}};
}
inline D2 sub(D2 a, D2 b)
{
    return {{a.v[0] - b.v[0], a.v[1] - b.v[1]}};
}
inline D2 mul(D2 a, D2 b)
{
    return {{a.v[0] * b.v[0], a.v[1] * b.v[1]}};
}
inline D2 madd(D2 acc, D2 a, D2 b)
{
    return {{acc.v[0] + a.v[0] * b.v[0], acc.v[1] + a.v[1] * b.v[1]}};
}
inline D2 msub(D2 acc, D2 a, D2 b)
{
    return {{acc.v[0] - a.v[0] * b.v[0], acc.v[1] - a.v[1] * b.v[1]}};
}
#endif
} // namespace pa::dsp::simd
