// The blend modes of 16-bit CMYK and Lab documents (docs/high-bit-depth-plan.md, P7 step C): blend_u16.cpp's kernels
// on each channel (blend_modes.inc), and Lab's own non-separable modes. RGB documents never reach this file.
#include "blend_modes.inc"
#include "compositor/depth.h"
#include "compositor/simd.h"

namespace compositor {

namespace {

struct Kernels16 {
    using Sample = uint16_t;
    using Steps = uint32_t;
    static constexpr uint32_t one = one16;
    static constexpr uint32_t full = one16;
    static constexpr int labOffset = 16384;
    static void pixel(BlendMode mode, const uint16_t* src, uint32_t k, uint16_t* dst) { compositePixelSteps16(mode, src, k, dst); }
    static Steps stepsOf(float coverage) { return coverageSteps16(coverage); }
    /// Source-over on `n` samples with blend_u16.cpp's Normal arithmetic.
    static void normal(const uint16_t* src, uint32_t k, uint16_t* dst, int n) {
        const uint64_t sa = (uint64_t(src[n - 1]) * k + one / 2) >> 15;
        if (sa == 0) return;
        const uint64_t inv = one - sa;
        int c = 0;
        if (n >= 4 && k <= one && sa <= one) { simd::sourceOver16(src, k, uint32_t(inv), dst, one); c = 4; }
        for (; c < n; c++)
            dst[c] = uint16_t(std::min<uint64_t>(one, ((uint64_t(src[c]) * k + one / 2) >> 15) + ((uint64_t(dst[c]) * inv + one / 2) >> 15)));
    }
    static void rgbSpan(BlendMode mode, const uint16_t* src, const uint32_t* steps, uint16_t* dst, int count) { compositeSpan16(mode, src, steps, dst, count); }
};

} // namespace

void compositeSpanMode16(BlendMode mode, ColorMode colorMode, const uint16_t* src, const uint32_t* steps, uint16_t* dst, int count) {
    modes::span<Kernels16>(blendModeFor(mode, colorMode), colorMode, src, steps, dst, count);
}

void compositePixelAtMode16(BlendMode mode, ColorMode colorMode, const uint16_t* src, float coverage, uint16_t* dst, int x, int y) {
    modes::pixelAt<Kernels16>(blendModeFor(mode, colorMode), colorMode, src, coverage, dst, x, y);
}

} // namespace compositor
