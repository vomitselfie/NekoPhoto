// Pixel operations over a buffer of any depth and layout (docs/high-bit-depth-plan.md, P5c and P7 E): trimming a merge's
// result and applying a layer mask, for 32-bit, CMYK and Lab documents as for 8- and 16-bit RGB.
#include "compositor/filters.h"
#include "stroke_raster.h"

namespace compositor {

namespace {

template <class Img>
std::shared_ptr<Img> trimOf(const Img& image, const LayerTransform& transform, LayerTransform& trimmedTransform, bool& empty) {
    const PixelBounds b = stroke::alphaBoundsOf(image, PixelBounds{0, 0, image.width(), image.height()});
    trimmedTransform = transform;
    empty = b.isEmpty();
    if (b.isEmpty() || (b.x0 == 0 && b.y0 == 0 && b.x1 == image.width() && b.y1 == image.height())) return std::make_shared<Img>(image);
    auto cropped = stroke::cropInto(image, b.x0, b.y0, std::make_shared<Img>(b.x1 - b.x0, b.y1 - b.y0, image.channels()));
    LayerTransform t = transform;
    t.size = {(b.x1 - b.x0) * transform.size.width / image.width(), (b.y1 - b.y0) * transform.size.height / image.height()};
    const Point middle = transform.pixelToDocument(image.width(), image.height()).apply({(b.x0 + b.x1) / 2.0, (b.y0 + b.y1) / 2.0});
    t.origin = {middle.x - t.size.width / 2, middle.y - t.size.height / 2};
    trimmedTransform = t;
    return cropped;
}

/// Every sample of `image` times the mask's coverage (0..one) at the pixel, nearest sample of a mask of another size.
template <class Img, class Gray>
std::shared_ptr<Img> masked(const Img& image, const Gray& mask, double one) {
    using Sample = std::remove_cv_t<std::remove_pointer_t<decltype(image.row(0))>>;
    const int w = image.width(), h = image.height(), n = image.channels();
    auto out = std::make_shared<Img>(w, h, n);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const auto raw = mask.width() == 1 && mask.height() == 1 ? mask.at(0, 0)
                : mask.at(std::min(x * mask.width() / w, mask.width() - 1), std::min(y * mask.height() / h, mask.height() - 1));
            const Sample* s = image.pixel(x, y);
            Sample* d = out->pixel(x, y);
            if constexpr (std::is_floating_point_v<Sample>) {
                const float m = std::clamp(float(raw), 0.0f, 1.0f);
                for (int c = 0; c < n; c++) d[c] = s[c] * m;
            } else {
                // The integer depths' own rounding: (v * m + one / 2) / one.
                const uint32_t o = uint32_t(one), m = std::min<uint32_t>(uint32_t(raw), o);
                for (int c = 0; c < n; c++) d[c] = Sample((uint32_t(s[c]) * m + o / 2) / o);
            }
        }
    return out;
}

} // namespace

AnyImage trimToPixelsAny(const AnyImage& image, const LayerTransform& transform, LayerTransform& trimmedTransform, bool* empty) {
    bool none = true;
    AnyImage result;
    if (image.u8()) {
        auto trimmed = trimToPixels(*image.u8(), transform, trimmedTransform);
        none = alphaBounds(*trimmed).isEmpty();
        result = ImagePtr(trimmed);
    } else if (image.u16()) result = Image16Ptr(trimOf(*image.u16(), transform, trimmedTransform, none));
    else if (image.f32()) result = ImageFPtr(trimOf(*image.f32(), transform, trimmedTransform, none));
    else if (image.c8()) result = ImageC8Ptr(trimOf(*image.c8(), transform, trimmedTransform, none));
    if (empty) *empty = none;
    return result;
}

AnyImage applyMaskAny(const AnyImage& image, const AnyGray& mask) {
    if (image.u16() && mask.u16()) return Image16Ptr(masked(*image.u16(), *mask.u16(), double(one16)));
    if (image.f32() && mask.f32()) return ImageFPtr(masked(*image.f32(), *mask.f32(), 1.0));
    if (image.c8() && mask.u8()) return ImageC8Ptr(masked(*image.c8(), *mask.u8(), 255.0));
    if (image.u8() && mask.u8()) {
        // Image (8-bit RGB or Lab): its own rounding, as Apply Layer Mask has always had.
        const Image& src = *image.u8();
        const GrayImage& m8 = *mask.u8();
        const int w = src.width(), h = src.height();
        auto out = std::make_shared<Image>(w, h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const int m = m8.width() == 1 && m8.height() == 1 ? m8.at(0, 0) : m8.at(std::min(x * m8.width() / w, m8.width() - 1), std::min(y * m8.height() / h, m8.height() - 1));
                const uint8_t* s = src.pixel(x, y);
                uint8_t* d = out->pixel(x, y);
                for (int c = 0; c < 4; c++) d[c] = uint8_t((s[c] * m + 127) / 255);
            }
        return ImagePtr(out);
    }
    return {};
}

} // namespace compositor
