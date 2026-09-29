// Colour modes: RGB, CMYK and Lab documents (docs/high-bit-depth-plan.md, "P7 plan: CMYK and Lab").
//
// A document is in one mode, as in Photoshop (Image > Mode). The pixels stay premultiplied and top-down; what the
// samples mean depends on the mode:
//
// - RGB: 4 samples, R, G, B, alpha. 8-bit `Image`, 16-bit `Image16`.
// - CMYK: 5 samples, C, M, Y, K, alpha, stored as **inverted ink** (PSD's convention: 0 is full ink, `one` is none),
//   premultiplied, so a transparent pixel is all zeros as in every other mode and the separable blend kernels apply
//   per ink. 8-bit `ImageC8`, 16-bit `Image16` with 5 channels.
// - Lab: 4 samples, L, a, b, alpha, premultiplied; a and b are offset by 128 at 8 bits and 16384 at 16 so they are
//   unsigned. 8-bit `Image`, 16-bit `Image16`.
//
// The accessors below hide the inversion and the offset, so code reads ink amounts and signed a/b values. CMYK and Lab
// exist at 8 and 16 bits only (Photoshop has no 32-bit CMYK or Lab).
#pragma once
#include "sampletype.h"
#include <cstdint>

namespace compositor {

enum class ColorMode : uint8_t { RGB, CMYK, Lab };

/// Samples per pixel, alpha included: 5 for CMYK, 4 for RGB and Lab.
constexpr int colorModeChannels(ColorMode mode) { return mode == ColorMode::CMYK ? 5 : 4; }
/// Colour samples per pixel, alpha excluded.
constexpr int colorModeColorChannels(ColorMode mode) { return colorModeChannels(mode) - 1; }
/// "rgb", "cmyk", "lab": the manifest's and automation's keys.
constexpr const char* colorModeKey(ColorMode mode) { return mode == ColorMode::CMYK ? "cmyk" : mode == ColorMode::Lab ? "lab" : "rgb"; }
/// "RGB", "CMYK", "Lab": as Photoshop's Image > Mode menu names them.
constexpr const char* colorModeName(ColorMode mode) { return mode == ColorMode::CMYK ? "CMYK" : mode == ColorMode::Lab ? "Lab" : "RGB"; }
/// Whether `type` can hold `mode`: RGB at every depth, CMYK and Lab at 8 and 16 bits.
constexpr bool colorModeSupportsDepth(ColorMode mode, SampleType type) { return mode == ColorMode::RGB || type != SampleType::F32; }

// ---- CMYK: inverted ink --------------------------------------------------------------------------------------------

/// The ink amount (0: none, `one`: full) a straight (unpremultiplied) CMYK sample stands for, and back.
template <SampleType S> constexpr SampleOf<S> inkFromStored(SampleOf<S> stored) { return SampleOf<S>(SampleTraits<S>::one - stored); }
template <SampleType S> constexpr SampleOf<S> storedFromInk(SampleOf<S> ink) { return SampleOf<S>(SampleTraits<S>::one - ink); }
/// The ink of a premultiplied sample under `alpha`, straight (0..one): what Photoshop's Info panel shows as a percentage
/// of `one`. A transparent pixel has no ink.
template <SampleType S> constexpr SampleOf<S> inkValue(SampleOf<S> stored, SampleOf<S> alpha) {
    using T = SampleTraits<S>;
    if (alpha == 0) return 0;
    const uint32_t straight = alpha >= T::one ? uint32_t(stored) : (uint32_t(stored) * T::one + alpha / 2) / alpha;
    return SampleOf<S>(T::one - (straight > T::one ? T::one : straight));
}
/// The premultiplied sample for `ink` (straight, 0..one) under `alpha`.
template <SampleType S> constexpr SampleOf<S> storedInk(SampleOf<S> ink, SampleOf<S> alpha) {
    using T = SampleTraits<S>;
    const uint32_t straight = T::one - (ink > T::one ? T::one : ink);
    return SampleOf<S>(alpha >= T::one ? straight : (straight * alpha + T::one / 2) / T::one);
}
/// Ink as a percentage, 0..100 (Photoshop's CMYK sliders).
template <SampleType S> constexpr double inkPercent(SampleOf<S> ink) { return double(ink) * 100.0 / double(SampleTraits<S>::one); }

// ---- Lab: offset a and b -------------------------------------------------------------------------------------------

/// The stored value of a = 0 and b = 0: 128 at 8 bits, 16384 at 16 (half of 32768).
template <SampleType S> constexpr int labOffset() { return S == SampleType::U8 ? 128 : 16384; }
/// Stored units per unit of a or b: 1 at 8 bits, 128 at 16 (so 16-bit a and b span -128..+128).
template <SampleType S> constexpr int labScale() { return S == SampleType::U8 ? 1 : 128; }

namespace detail {
template <SampleType S> constexpr uint32_t labStraight(SampleOf<S> stored, SampleOf<S> alpha) {
    using T = SampleTraits<S>;
    if (alpha == 0) return uint32_t(labOffset<S>());
    const uint32_t s = alpha >= T::one ? uint32_t(stored) : (uint32_t(stored) * T::one + alpha / 2) / alpha;
    return s > T::one ? T::one : s;
}
} // namespace detail

/// L as 0..100 from a premultiplied sample under `alpha`.
template <SampleType S> constexpr double labL(SampleOf<S> stored, SampleOf<S> alpha) {
    using T = SampleTraits<S>;
    if (alpha == 0) return 0;
    const double s = alpha >= T::one ? double(stored) : double(stored) * double(T::one) / double(alpha);
    return (s > T::one ? double(T::one) : s) * 100.0 / double(T::one);
}
/// a and b as signed values (-128..127 at 8 bits, -128..+128 at 16) from premultiplied samples under `alpha`: the
/// sample is made straight first, then the offset removed. A transparent pixel is neutral.
template <SampleType S> constexpr double labA(SampleOf<S> stored, SampleOf<S> alpha) {
    return (double(detail::labStraight<S>(stored, alpha)) - labOffset<S>()) / labScale<S>();
}
template <SampleType S> constexpr double labB(SampleOf<S> stored, SampleOf<S> alpha) { return labA<S>(stored, alpha); }
/// The premultiplied sample for a signed a or b value under `alpha` (clamped to the stored range).
template <SampleType S> constexpr SampleOf<S> storedLabAB(double value, SampleOf<S> alpha) {
    using T = SampleTraits<S>;
    double straight = value * labScale<S>() + labOffset<S>();
    straight = straight < 0 ? 0 : straight > T::one ? double(T::one) : straight;
    return SampleOf<S>(double(alpha) >= double(T::one) ? straight + 0.5 : straight * double(alpha) / double(T::one) + 0.5);
}
/// The premultiplied sample for L (0..100) under `alpha`.
template <SampleType S> constexpr SampleOf<S> storedLabL(double l, SampleOf<S> alpha) {
    using T = SampleTraits<S>;
    double straight = (l < 0 ? 0 : l > 100 ? 100 : l) * double(T::one) / 100.0;
    return SampleOf<S>(straight * double(alpha) / double(T::one) + 0.5);
}

} // namespace compositor
