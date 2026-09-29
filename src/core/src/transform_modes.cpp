// Resampling pixels of any layout (modetransform.h): CMYK through two 4-sample passes, everything else as it is.
#include "compositor/modetransform.h"
#include "compositor/depth.h"
#include "compositor/render.h"
#include <algorithm>
#include <cstring>

namespace compositor {

namespace {

template <class Five, class Four>
std::pair<std::shared_ptr<Four>, std::shared_ptr<Four>> split(const Five& image) {
    const int w = image.width(), h = image.height();
    auto cmy = std::make_shared<Four>(w, h), black = std::make_shared<Four>(w, h);
    for (int y = 0; y < h; y++) {
        const auto* s = image.row(y);
        auto* a = cmy->row(y);
        auto* b = black->row(y);
        for (int x = 0; x < w; x++, s += 5, a += 4, b += 4) {
            a[0] = s[0]; a[1] = s[1]; a[2] = s[2]; a[3] = s[4];
            b[0] = b[1] = b[2] = s[3]; b[3] = s[4];
        }
    }
    return {cmy, black};
}

template <class Five, class Four>
std::shared_ptr<Five> join(const Four& cmy, const Four& black) {
    const int w = cmy.width(), h = cmy.height();
    auto out = std::make_shared<Five>(w, h, 5);
    if (black.width() != w || black.height() != h) return out;
    for (int y = 0; y < h; y++) {
        const auto* a = cmy.row(y);
        const auto* b = black.row(y);
        auto* d = out->row(y);
        for (int x = 0; x < w; x++, a += 4, b += 4, d += 5) {
            d[0] = a[0]; d[1] = a[1]; d[2] = a[2];
            // Both passes weigh the same alpha; black is held within it all the same.
            d[3] = std::min(b[0], a[3]);
            d[4] = a[3];
        }
    }
    return out;
}

/// `f` over a buffer of any layout: a 4-sample buffer (Image, Image16, ImageF) as it is, a 5-sample one as its two
/// halves. `f(const Four&, shared_ptr<const Four>)` returns an optional of a struct with `image` (a shared pointer to a
/// 4-sample buffer) and `transform`; the result carries the joined image.
template <class F>
std::optional<WarpedAny> perPlanes(const AnyImage& image, F&& f) {
    auto wrap = [](auto&& r) -> std::optional<WarpedAny> {
        if (!r || !r->image) return std::nullopt;
        using P = std::remove_cvref_t<decltype(r->image)>;
        using T = typename P::element_type;
        return WarpedAny{AnyImage(std::shared_ptr<const T>(r->image)), r->transform};
    };
    if (image.c8()) {
        auto [cmy, black] = split<ImageC8, Image>(*image.c8());
        auto a = f(std::shared_ptr<const Image>(cmy));
        auto b = f(std::shared_ptr<const Image>(black));
        if (!a || !b || !a->image || !b->image) return std::nullopt;
        return WarpedAny{ImageC8Ptr(join<ImageC8, Image>(*a->image, *b->image)), a->transform};
    }
    if (image.u16() && image.u16()->channels() == 5) {
        auto [cmy, black] = split<Image16, Image16>(*image.u16());
        auto a = f(std::shared_ptr<const Image16>(cmy));
        auto b = f(std::shared_ptr<const Image16>(black));
        if (!a || !b || !a->image || !b->image) return std::nullopt;
        return WarpedAny{Image16Ptr(join<Image16, Image16>(*a->image, *b->image)), a->transform};
    }
    if (image.u8()) return wrap(f(image.u8()));
    if (image.u16()) return wrap(f(image.u16()));
    if (image.f32()) return wrap(f(image.f32()));
    return std::nullopt;
}

template <class T> struct Held { std::shared_ptr<T> image; LayerTransform transform; };

} // namespace

bool isFiveSample(const AnyImage& image) { return image.channels() == 5; }

std::pair<std::shared_ptr<Image>, std::shared_ptr<Image>> splitFiveSample(const ImageC8& image) { return split<ImageC8, Image>(image); }
std::pair<std::shared_ptr<Image16>, std::shared_ptr<Image16>> splitFiveSample(const Image16& image) { return split<Image16, Image16>(image); }
std::shared_ptr<ImageC8> joinFiveSample(const Image& cmy, const Image& black) { return join<ImageC8, Image>(cmy, black); }
std::shared_ptr<Image16> joinFiveSample(const Image16& cmy, const Image16& black) { return join<Image16, Image16>(cmy, black); }

std::optional<WarpedAny> warpImageAny(const AnyImage& image, const LayerTransform& transform, const Corners& corners, int limit) {
    return perPlanes(image, [&](const auto& p) { return warpImage(p, transform, corners, limit); });
}

std::optional<WarpedAny> warpImageTrimmedAny(const AnyImage& image, const LayerTransform& transform, const Corners& corners, Rect* crop) {
    // Both halves trim to the same alpha, so the first pass's crop is the join's.
    bool first = true;
    return perPlanes(image, [&](const auto& p) {
        auto r = warpImageTrimmed(p, transform, corners, first ? crop : nullptr);
        first = false;
        return r;
    });
}

AnyImage resampleLayerAny(const AnyImage& image, const LayerTransform& transform, const LayerTransform& target, int width, int height) {
    auto r = perPlanes(image, [&](const auto& p) {
        auto out = resampleLayer(p, transform, target, width, height);
        using T = typename decltype(out)::element_type;
        return std::optional<Held<T>>(Held<T>{out, target});
    });
    return r ? r->image : AnyImage();
}

std::optional<WarpedAny> renderWarpedImageAny(const AnyImage& image, const WarpMesh& mesh, const std::array<double, 8>& quad, const Rect* clip) {
    return perPlanes(image, [&](const auto& p) { return renderWarpedImage(*p, mesh, quad, clip); });
}

std::optional<WarpedAny> renderWarpedOverBoxAny(const AnyImage& image, const WarpMesh& mesh, const Rect& box) {
    return perPlanes(image, [&](const auto& p) { return renderWarpedOverBox(*p, mesh, box); });
}

AnyImage resampleAxisAlignedAny(const AnyImage& image, int width, int height, double originX, double stepX, double originY, double stepY, ResampleFilter filter) {
    auto r = perPlanes(image, [&](const auto& p) {
        auto out = resampleAxisAligned(*p, width, height, originX, stepX, originY, stepY, filter);
        using T = typename decltype(out)::element_type;
        return std::optional<Held<T>>(Held<T>{out, LayerTransform()});
    });
    return r ? r->image : AnyImage();
}

namespace {

template <class Img>
std::shared_ptr<Img> over(const Img& src, const Img& dst) {
    using Sample = std::remove_cv_t<std::remove_pointer_t<decltype(src.row(0))>>;
    const int n = dst.channels(), w = dst.width(), h = dst.height();
    auto out = std::make_shared<Img>(dst);
    if (src.width() != w || src.height() != h || src.channels() != n) return out;
    for (int y = 0; y < h; y++) {
        const Sample* f = src.row(y);
        Sample* d = out->row(y);
        for (int x = 0; x < w; x++, f += n, d += n) {
            if constexpr (std::is_floating_point_v<Sample>) {
                const float keep = 1.0f - std::clamp(float(f[n - 1]), 0.0f, 1.0f);
                for (int c = 0; c < n; c++) d[c] = Sample(f[c] + d[c] * keep);
            } else {
                constexpr uint32_t one = std::is_same_v<Sample, uint8_t> ? 255u : one16;
                const uint32_t keep = one - std::min<uint32_t>(f[n - 1], one);
                for (int c = 0; c < n; c++) d[c] = Sample(std::min<uint32_t>(one, f[c] + (uint32_t(d[c]) * keep + one / 2) / one));
            }
        }
    }
    return out;
}

template <class Img>
std::shared_ptr<Img> placed(const Img& image, int width, int height, int x0, int y0) {
    const int n = image.channels();
    auto out = std::make_shared<Img>(width, height, n);
    using Sample = std::remove_cv_t<std::remove_pointer_t<decltype(image.row(0))>>;
    for (int y = 0; y < image.height(); y++) {
        const int ty = y + y0;
        if (ty < 0 || ty >= height) continue;
        const int xa = std::max(0, x0), xb = std::min(width, x0 + image.width());
        if (xb <= xa) continue;
        std::memcpy(out->pixel(xa, ty), image.pixel(xa - x0, y), size_t(xb - xa) * size_t(n) * sizeof(Sample));
    }
    return out;
}

} // namespace

AnyImage compositeOverAny(const AnyImage& src, const AnyImage& dst) {
    if (src.c8() && dst.c8()) return ImageC8Ptr(over(*src.c8(), *dst.c8()));
    if (src.u16() && dst.u16()) return Image16Ptr(over(*src.u16(), *dst.u16()));
    if (src.f32() && dst.f32()) return ImageFPtr(over(*src.f32(), *dst.f32()));
    if (src.u8() && dst.u8()) {
        // Image is not ImageT: the same arithmetic over its four samples.
        const Image& f = *src.u8();
        auto out = std::make_shared<Image>(*dst.u8());
        if (f.width() != out->width() || f.height() != out->height()) return ImagePtr(out);
        for (int y = 0; y < out->height(); y++) {
            const uint8_t* s = f.row(y);
            uint8_t* d = out->row(y);
            for (int x = 0; x < out->width(); x++, s += 4, d += 4) {
                const uint32_t keep = 255u - s[3];
                for (int c = 0; c < 4; c++) d[c] = uint8_t(std::min<uint32_t>(255u, s[c] + (uint32_t(d[c]) * keep + 127) / 255));
            }
        }
        return ImagePtr(out);
    }
    return dst;
}

AnyImage placeInAny(const AnyImage& image, int width, int height, int x, int y) {
    if (image.c8()) return ImageC8Ptr(placed(*image.c8(), width, height, x, y));
    if (image.u16()) return Image16Ptr(placed(*image.u16(), width, height, x, y));
    if (image.f32()) return ImageFPtr(placed(*image.f32(), width, height, x, y));
    if (image.u8()) {
        const Image& src = *image.u8();
        auto out = std::make_shared<Image>(width, height);
        for (int yy = 0; yy < src.height(); yy++) {
            const int ty = yy + y;
            if (ty < 0 || ty >= height) continue;
            const int xa = std::max(0, x), xb = std::min(width, x + src.width());
            if (xb > xa) std::memcpy(out->pixel(xa, ty), src.pixel(xa - x, yy), size_t(xb - xa) * 4);
        }
        return ImagePtr(out);
    }
    return {};
}

} // namespace compositor
