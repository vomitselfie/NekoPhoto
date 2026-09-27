// Sample types: the depth of a document's pixels (docs/high-bit-depth-plan.md, section 3).
// One depth per document, as in Photoshop; 8-bit is the fast path and keeps its byte kernels.
#pragma once
#include <cstddef>
#include <cstdint>

namespace compositor {

enum class SampleType : uint8_t { U8, U16, F32 };
constexpr int sampleTypeCount = 3;

/// What one sample of each type is. `one` is full intensity (and full alpha): U16 uses Photoshop's internal
/// 0..32768 range, so premultiplying and multiplying are an exact `x * a >> 15`; PSD's 0..65535 is mapped at
/// import and export. F32 is linear light, colour may exceed `one`, alpha is clamped to it.
template <SampleType S> struct SampleTraits;
template <> struct SampleTraits<SampleType::U8> {
    using Sample = uint8_t;
    static constexpr Sample one = 255;
    static constexpr int bits = 8;
};
template <> struct SampleTraits<SampleType::U16> {
    using Sample = uint16_t;
    static constexpr Sample one = 32768;
    static constexpr int bits = 15;
};
template <> struct SampleTraits<SampleType::F32> {
    using Sample = float;
    static constexpr Sample one = 1.0f;
    static constexpr int bits = 32;
};
template <SampleType S> using SampleOf = typename SampleTraits<S>::Sample;

constexpr size_t sampleBytes(SampleType type) {
    return type == SampleType::U8 ? 1 : type == SampleType::U16 ? 2 : 4;
}
/// "8", "16", "32": the bits per channel as Photoshop's Image > Mode menu names them.
constexpr const char* sampleTypeName(SampleType type) {
    return type == SampleType::U8 ? "8" : type == SampleType::U16 ? "16" : "32";
}

} // namespace compositor
