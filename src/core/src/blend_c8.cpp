// The blend modes of 8-bit CMYK and Lab documents (docs/high-bit-depth-plan.md, P7 step C): the RGB byte kernels of
// blend_u8.cpp on each channel (blend_modes.inc), so every separable mode keeps its Photoshop calibration, and Lab's
// own non-separable modes. blend_u8.cpp is not touched: RGB documents never reach this file.
#include "blend_modes.inc"

namespace compositor {

namespace {

struct Kernels8 {
    using Sample = uint8_t;
    using Steps = uint16_t;
    static constexpr uint32_t one = 255;
    static constexpr uint32_t full = 256;
    static constexpr int labOffset = 128;
    static void pixel(BlendMode mode, const uint8_t* src, unsigned k, uint8_t* dst) { compositePixelSteps(mode, src, k, dst); }
    static Steps stepsOf(float coverage) { return Steps(coverageSteps(coverage)); }
    /// Source-over on `n` samples with blend_u8.cpp's Normal arithmetic (the alpha lane uses the colour expression).
    static void normal(const uint8_t* src, unsigned k, uint8_t* dst, int n) {
        const unsigned sa = (src[n - 1] * k + 128) >> 8;
        if (sa == 0) return;
        const unsigned inv = 255 - sa;
        for (int c = 0; c < n; c++) dst[c] = uint8_t(((src[c] * k + 128) >> 8) + (((dst[c] * inv + 128) * 257) >> 16));
    }
    static void rgbSpan(BlendMode mode, const uint8_t* src, const uint16_t* steps, uint8_t* dst, int count) {
        for (int i = 0; i < count; i++, src += 4, dst += 4) if (steps[i]) compositePixelSteps(mode, src, steps[i], dst);
    }
};

} // namespace

bool blendModeAvailable(BlendMode mode, ColorMode colorMode) {
    return colorMode != ColorMode::Lab || !modes::unavailableInLab(mode);
}

BlendMode blendModeFor(BlendMode mode, ColorMode colorMode) {
    if (!blendModeAvailable(mode, colorMode)) return BlendMode::Normal;
    return mode;
}

void compositeSpanMode8(BlendMode mode, ColorMode colorMode, const uint8_t* src, const uint16_t* steps, uint8_t* dst, int count) {
    modes::span<Kernels8>(blendModeFor(mode, colorMode), colorMode, src, steps, dst, count);
}

void compositePixelAtMode8(BlendMode mode, ColorMode colorMode, const uint8_t* src, float coverage, uint8_t* dst, int x, int y) {
    modes::pixelAt<Kernels8>(blendModeFor(mode, colorMode), colorMode, src, coverage, dst, x, y);
}

void blendStraightMode(BlendMode mode, ColorMode colorMode, const float* cb, float* cs) {
    mode = blendModeFor(mode, colorMode);
    if (mode == BlendMode::Normal || mode == BlendMode::Dissolve) return;
    const int colours = colorModeColorChannels(colorMode);
    if (colorMode == ColorMode::CMYK && modes::nonSeparable(mode)) {
        float m[4];
        modes::cmykNonSeparableColor(mode, cb, cs, m);
        std::copy(m, m + 4, cs);
        return;
    }
    if (colorMode == ColorMode::Lab && modes::nonSeparable(mode)) {
        // Lab's non-separable modes on straight colour (blend_modes.inc's labNonSeparable, without the compositing).
        const float n = 0.5f;
        const float sa = cs[1] - n, sb = cs[2] - n, ba = cb[1] - n, bb = cb[2] - n;
        const float sc = std::hypot(sa, sb), bc = std::hypot(ba, bb);
        float m[3] = {cb[0], cb[1], cb[2]};
        switch (mode) {
        case BlendMode::Luminosity: m[0] = cs[0]; break;
        case BlendMode::Color: m[1] = cs[1]; m[2] = cs[2]; break;
        case BlendMode::Hue: if (sc > 0) { m[1] = n + bc * sa / sc; m[2] = n + bc * sb / sc; } break;
        case BlendMode::Saturation: if (bc > 0) { m[1] = n + sc * ba / bc; m[2] = n + sc * bb / bc; } break;
        case BlendMode::DarkerColor: if (cs[0] < cb[0]) return; break;
        case BlendMode::LighterColor: if (cs[0] > cb[0]) return; break;
        default: return;
        }
        for (int k = 0; k < 3; k++) cs[k] = std::clamp(m[k], 0.0f, 1.0f);
        return;
    }
    for (int k = 0; k < colours; k++) cs[k] = blendChannel16(mode, cb[k], cs[k]);
}

} // namespace compositor
