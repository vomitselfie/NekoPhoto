// Typed pixel buffers for deeper documents (docs/high-bit-depth-plan.md, section 3).
//
// `Image` and `GrayImage` (image.h) stay exactly as they are: they are the 8-bit case. `ImageT<S>` and
// `GrayImageT<S>` hold 16-bit and float samples with the same API (rows of `S` samples, an explicit byte stride),
// and `ImageOf<S>` / `GrayOf<S>` name the buffer for any sample type, `Image` for U8.
//
// Layers, masks and the selection hold `AnyImage` / `AnyGray`: a shared buffer of whichever depth the document
// has. Code written for 8-bit reads it with `.u8()`, which is null for a deeper buffer, so every existing path
// keeps working on 8-bit documents only; `visit` hands the typed pointer to depth-generic code.
#pragma once
#include "image.h"
#include "sampletype.h"
#include <cstring>
#include <type_traits>
#include <variant>

namespace compositor {

/// A premultiplied colour buffer of `channels` interleaved samples per pixel (4 for RGB and Lab, 5 for CMYK),
/// rows top-down, explicit byte stride. Zeroed (transparent) when created. `Image` stays the 8-bit RGBA and Lab
/// buffer; `ImageT<U8>` is `ImageC8`, the 8-bit CMYK one (colormodes.h).
template <SampleType S>
class ImageT {
public:
    using Sample = SampleOf<S>;
    static constexpr SampleType sampleType = S;
    ImageT() = default;
    ImageT(int width, int height, int channels = 4) {
        if (width < 0 || height < 0 || width > maxImageSide || height > maxImageSide || channels < 1 || channels > 5) return;
        width_ = width; height_ = height; channels_ = channels;
        stride_ = int(size_t(width) * size_t(channels) * sizeof(Sample));
        pixels_.resize(size_t(width) * size_t(height) * size_t(channels));
    }
    int width() const { return width_; }
    int height() const { return height_; }
    int channels() const { return channels_; }
    /// Bytes per row.
    int stride() const { return stride_; }
    bool isEmpty() const { return width_ <= 0 || height_ <= 0; }
    Sample* data() { return pixels_.data(); }
    const Sample* data() const { return pixels_.data(); }
    Sample* row(int y) { return pixels_.data() + size_t(y) * size_t(width_) * size_t(channels_); }
    const Sample* row(int y) const { return pixels_.data() + size_t(y) * size_t(width_) * size_t(channels_); }
    Sample* pixel(int x, int y) { return row(y) + size_t(x) * size_t(channels_); }
    const Sample* pixel(int x, int y) const { return row(y) + size_t(x) * size_t(channels_); }
    size_t byteCount() const { return pixels_.size() * sizeof(Sample); }
    /// Every pixel set to `value` (`channels()` samples).
    void fill(const Sample* value) {
        for (size_t i = 0; i < pixels_.size(); i += size_t(channels_)) std::memcpy(&pixels_[i], value, size_t(channels_) * sizeof(Sample));
    }
    void clear() { std::fill(pixels_.begin(), pixels_.end(), Sample(0)); }
    bool operator==(const ImageT& other) const {
        return width_ == other.width_ && height_ == other.height_ && channels_ == other.channels_ && pixels_ == other.pixels_;
    }

private:
    int width_ = 0, height_ = 0, channels_ = 4, stride_ = 0;
    std::vector<Sample, ZeroedAllocator<Sample>> pixels_;
};

/// One sample per pixel (masks, selections, alpha channels), stride = width samples.
template <SampleType S>
class GrayImageT {
    static_assert(S != SampleType::U8, "GrayImage is the 8-bit buffer; use GrayOf<S>");
public:
    using Sample = SampleOf<S>;
    static constexpr SampleType sampleType = S;
    GrayImageT() = default;
    GrayImageT(int width, int height, Sample value = 0) {
        if (width < 0 || height < 0 || width > maxImageSide || height > maxImageSide) return;
        width_ = width; height_ = height;
        pixels_.resize(size_t(width) * size_t(height));
        if (value != Sample(0)) std::fill(pixels_.begin(), pixels_.end(), value);
    }
    int width() const { return width_; }
    int height() const { return height_; }
    /// Samples per row.
    int stride() const { return width_; }
    bool isEmpty() const { return width_ <= 0 || height_ <= 0; }
    Sample* data() { return pixels_.data(); }
    const Sample* data() const { return pixels_.data(); }
    Sample* row(int y) { return pixels_.data() + size_t(y) * size_t(width_); }
    const Sample* row(int y) const { return pixels_.data() + size_t(y) * size_t(width_); }
    Sample at(int x, int y) const { return row(y)[x]; }
    Sample& at(int x, int y) { return row(y)[x]; }
    size_t byteCount() const { return pixels_.size() * sizeof(Sample); }
    void fill(Sample value) { std::fill(pixels_.begin(), pixels_.end(), value); }
    bool operator==(const GrayImageT& other) const { return width_ == other.width_ && height_ == other.height_ && pixels_ == other.pixels_; }

private:
    int width_ = 0, height_ = 0;
    std::vector<Sample, ZeroedAllocator<Sample>> pixels_;
};

template <SampleType S> struct ImageTypes { using Color = ImageT<S>; using Gray = GrayImageT<S>; };
template <> struct ImageTypes<SampleType::U8> { using Color = Image; using Gray = GrayImage; };
/// The colour and gray buffers for a sample type: `Image` and `GrayImage` for U8.
template <SampleType S> using ImageOf = typename ImageTypes<S>::Color;
template <SampleType S> using GrayOf = typename ImageTypes<S>::Gray;

using Image16 = ImageT<SampleType::U16>;
/// 8-bit CMYK: 5 samples a pixel (C, M, Y, K as inverted ink, alpha; colormodes.h). A distinct type from `Image`, so
/// code written for 8-bit RGBA never meets it: `AnyImage::u8()` is null for it and `.c8()` returns it.
using ImageC8 = ImageT<SampleType::U8>;
using ImageC8Ptr = std::shared_ptr<const ImageC8>;
using ImageF = ImageT<SampleType::F32>;
using Gray16 = GrayImageT<SampleType::U16>;
using GrayF = GrayImageT<SampleType::F32>;
using Image16Ptr = std::shared_ptr<const Image16>;
using ImageFPtr = std::shared_ptr<const ImageF>;
using Gray16Ptr = std::shared_ptr<const Gray16>;
using GrayFPtr = std::shared_ptr<const GrayF>;

/// A shared, immutable buffer of any depth, or none: `P8`, `P16`, `PF` are the shared pointers for U8, U16, F32, and
/// `PC` (when not void) the 8-bit CMYK buffer, a fourth alternative that reads as U8 but is not `u8()`. A pointer of
/// any of them converts to it implicitly, so code that assigns an `ImagePtr` keeps compiling.
template <class P8, class P16, class PF, class PC = void>
class AnyOf {
    static constexpr bool hasC8 = !std::is_void_v<PC>;
    using Variant = std::conditional_t<hasC8, std::variant<P8, P16, PF, std::conditional_t<hasC8, PC, P8>>, std::variant<P8, P16, PF>>;
    template <class P, class Q> static constexpr bool to = std::is_convertible_v<P&&, Q> && !std::is_same_v<std::remove_cvref_t<P>, std::nullptr_t>;

public:
    AnyOf() = default;
    AnyOf(std::nullptr_t) {}
    template <class P> requires to<P, P8>
    AnyOf(P&& p) : v_(std::in_place_index<0>, std::forward<P>(p)) {}
    template <class P> requires to<P, P16> && (!std::is_convertible_v<P&&, P8>)
    AnyOf(P&& p) : v_(std::in_place_index<1>, std::forward<P>(p)) {}
    template <class P> requires to<P, PF> && (!std::is_convertible_v<P&&, P8>) && (!std::is_convertible_v<P&&, P16>)
    AnyOf(P&& p) : v_(std::in_place_index<2>, std::forward<P>(p)) {}
    template <class P> requires hasC8 && to<P, std::conditional_t<hasC8, PC, void>> && (!std::is_convertible_v<P&&, P8>) && (!std::is_convertible_v<P&&, P16>) && (!std::is_convertible_v<P&&, PF>)
    AnyOf(P&& p) : v_(std::in_place_index<3>, std::forward<P>(p)) {}

    /// The depth of the buffer held (U8 when none is, and for 8-bit CMYK).
    SampleType sampleType() const { return v_.index() >= 3 ? SampleType::U8 : SampleType(v_.index()); }
    /// Samples per pixel: 5 for a CMYK buffer (8-bit, or 16-bit with 5 channels), else 4 (1 for grays).
    int channels() const {
        return std::visit([](const auto& p) -> int {
            if constexpr (requires { p->channels(); }) return p ? p->channels() : 4;
            else if constexpr (requires { p->stride(); p->at(0, 0); }) return 1;
            else return 4;
        }, v_);
    }
    explicit operator bool() const { return std::visit([](const auto& p) { return bool(p); }, v_); }
    void reset() { v_ = P8(); }
    /// The 8-bit buffer: null when there is none or it is deeper, or 8-bit CMYK.
    const P8& u8() const { if (auto* p = std::get_if<0>(&v_)) return *p; return none8(); }
    const P16& u16() const { if (auto* p = std::get_if<1>(&v_)) return *p; return none16(); }
    const PF& f32() const { if (auto* p = std::get_if<2>(&v_)) return *p; return noneF(); }
    /// The 8-bit CMYK buffer: null unless that is what is held.
    template <class Q = PC> requires (!std::is_void_v<Q>)
    const Q& c8() const {
        if (v_.index() == 3) return std::get<3>(v_);
        static const Q none;
        return none;
    }
    /// The buffer's size whatever its depth (0 when there is none): geometry, not pixels.
    int width() const { return std::visit([](const auto& p) { return p ? p->width() : 0; }, v_); }
    int height() const { return std::visit([](const auto& p) { return p ? p->height() : 0; }, v_); }
    /// The bytes its pixels take at their depth (0 when there is none): for memory accounting.
    size_t byteCount() const { return std::visit([](const auto& p) -> size_t { return p ? p->byteCount() : 0; }, v_); }
    /// The buffer's address, whatever its depth: an identity for caches and comparisons.
    const void* identity() const { return std::visit([](const auto& p) -> const void* { return p.get(); }, v_); }
    /// Calls `f` with the typed shared pointer held.
    template <class F> decltype(auto) visit(F&& f) const { return std::visit(std::forward<F>(f), v_); }
    /// Same buffer (not same pixels), as shared pointers compare.
    bool operator==(const AnyOf& other) const { return v_ == other.v_; }

private:
    static const P8& none8() { static const P8 p; return p; }
    static const P16& none16() { static const P16 p; return p; }
    static const PF& noneF() { static const PF p; return p; }
    Variant v_;
};

using AnyImage = AnyOf<ImagePtr, Image16Ptr, ImageFPtr, ImageC8Ptr>;
using AnyGray = AnyOf<GrayPtr, Gray16Ptr, GrayFPtr>;

/// Calls `f` with the typed shared pointer an `AnyImage` or `AnyGray` holds.
template <class F, class P8, class P16, class PF, class PC>
decltype(auto) visit(F&& f, const AnyOf<P8, P16, PF, PC>& any) { return any.visit(std::forward<F>(f)); }

} // namespace compositor
