// The Filter menu on a CMYK or Lab document's own samples, and the pixel-edit helpers over any layout (modeedit.h).
//
// The blurs and Lens Correction weigh every premultiplied sample alike, so a Lab buffer (4 samples) runs the RGB
// kernels as it is, and a CMYK buffer runs them as two 4-sample buffers (cyan, magenta, yellow with alpha; black with
// alpha) put back together: the same kernels, the alpha computed identically in both halves. Add Noise has its own
// loop: every ink, or L, a and b, each its own noise; monochromatic the same noise on every ink, or on L alone (a
// Lab pixel's colour stays where it was).
#include "compositor/colormodes.h"
#include "compositor/depth.h"
#include "compositor/modeedit.h"
#include "compositor/parallel.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace compositor {

namespace {

inline uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

/// Add Noise over `n` samples a pixel (filters_u16.cpp's pattern and spread): `lightOnly` puts it on the first sample
/// alone (Lab, monochromatic), `same` the same amount on every colour sample (CMYK, monochromatic).
template <class Img>
void addNoiseN(Img& image, int n, double one, float amount, bool gaussian, bool same, bool lightOnly, uint32_t seed) {
    using T = std::remove_reference_t<decltype(*image.row(0))>;
    const float spread = amount / 100.0f * 127.5f / 255.0f;
    const uint32_t width = uint32_t(image.width());
    const int c = n - 1;
    auto unit = [](uint32_t key) { return float(hash32(key) >> 8) * (1.0f / 16777216.0f); };
    auto sample = [&](uint32_t key) {
        if (!gaussian) return (unit(key) * 2.0f - 1.0f) * spread;
        const float u1 = unit(key), u2 = unit(key ^ 0x68e31da4U);
        return std::sqrt(-2.0f * std::log(1.0f - u1)) * std::cos(6.2831853f * u2) * spread * (2.0f / 3.0f);
    };
    (void)one;
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            T* p = image.row(y);
            for (uint32_t x = 0; x < width; x++, p += n) {
                const uint32_t alpha = p[c];
                if (!alpha) continue;
                const uint32_t base = hash32(seed ^ hash32(uint32_t(y) * width + x));
                const float shared = sample(base);
                for (int k = 0; k < (lightOnly ? 1 : c); k++) {
                    const float noise = same || lightOnly ? shared : sample(base + uint32_t(k) * 0x9e3779b9U);
                    const float value = std::clamp(float(p[k]) / float(alpha) + noise, 0.0f, 1.0f);
                    p[k] = T(std::min<long>(long(alpha), std::lround(value * float(alpha))));
                }
            }
        }
    });
}

/// A CMYK buffer as two 4-sample ones: cyan, magenta, yellow, alpha; black, 0, 0, alpha.
template <SampleType S>
void splitCmyk(const ImageT<S>& cmyk, ImageOf<S>& cmy, ImageOf<S>& k) {
    for (int y = 0; y < cmyk.height(); y++) {
        const SampleOf<S>* p = cmyk.row(y);
        SampleOf<S>* a = cmy.row(y);
        SampleOf<S>* b = k.row(y);
        for (int x = 0; x < cmyk.width(); x++, p += 5, a += 4, b += 4) {
            a[0] = p[0]; a[1] = p[1]; a[2] = p[2]; a[3] = p[4];
            b[0] = p[3]; b[1] = 0; b[2] = 0; b[3] = p[4];
        }
    }
}

template <SampleType S>
void mergeCmyk(const ImageOf<S>& cmy, const ImageOf<S>& k, ImageT<S>& cmyk) {
    for (int y = 0; y < cmyk.height(); y++) {
        SampleOf<S>* p = cmyk.row(y);
        const SampleOf<S>* a = cmy.row(y);
        const SampleOf<S>* b = k.row(y);
        for (int x = 0; x < cmyk.width(); x++, p += 5, a += 4, b += 4) {
            p[0] = a[0]; p[1] = a[1]; p[2] = a[2]; p[3] = std::min(b[0], a[3]); p[4] = a[3];
        }
    }
}

template <SampleType S>
std::shared_ptr<ImageT<S>> filterCmyk(FilterKind kind, const ImageT<S>& source, const FilterSettings& settings, double scale, uint32_t seed) {
    auto out = std::make_shared<ImageT<S>>(source);
    if (kind == FilterKind::AddNoise) {
        const FilterSettings s = settings.normalized();
        addNoiseN(*out, 5, double(SampleTraits<S>::one), float(s.amount), s.gaussian, s.monochromatic, false, seed);
        return out;
    }
    ImageOf<S> cmy(source.width(), source.height()), k(source.width(), source.height());
    splitCmyk<S>(source, cmy, k);
    applyFilter(kind, cmy, settings, scale, seed);
    applyFilter(kind, k, settings, scale, seed);
    mergeCmyk<S>(cmy, k, *out);
    return out;
}

template <class Img>
std::shared_ptr<Img> filterLab(FilterKind kind, const Img& source, const FilterSettings& settings, double scale, uint32_t seed, double one) {
    auto out = std::make_shared<Img>(source);
    if (kind == FilterKind::AddNoise) {
        const FilterSettings s = settings.normalized();
        addNoiseN(*out, 4, one, float(s.amount), s.gaussian, false, s.monochromatic, seed);
    } else {
        applyFilter(kind, *out, settings, scale, seed);
    }
    return out;
}

template <SampleType S>
std::shared_ptr<ImageT<S>> growN(const ImageT<S>& image, const LayerTransform& transform, int margin, LayerTransform& grownTransform, ColorMode mode) {
    if (margin <= 0) { grownTransform = transform; return std::make_shared<ImageT<S>>(image); }
    const int w = image.width() + 2 * margin, h = image.height() + 2 * margin;
    if (w > 30000 || h > 30000 || (long long)w * h > Document::imagePixelBudget(S, mode)) return nullptr;
    const int n = image.channels();
    auto out = std::make_shared<ImageT<S>>(w, h, n);
    for (int y = 0; y < image.height(); y++) std::memcpy(out->pixel(margin, y + margin), image.row(y), size_t(image.width()) * size_t(n) * sizeof(SampleOf<S>));
    LayerTransform t = transform;
    t.size = {double(w) * transform.size.width / image.width(), double(h) * transform.size.height / image.height()};
    const Point c = transform.center();
    t.origin = {c.x - t.size.width / 2, c.y - t.size.height / 2};
    grownTransform = t;
    return out;
}

template <SampleType S>
void blendN(ImageT<S>& adjusted, const ImageT<S>& original, const GrayOf<S>& coverage) {
    constexpr uint32_t one = SampleTraits<S>::one;
    const int n = adjusted.channels();
    const int w = std::min({adjusted.width(), original.width(), coverage.width()}), h = std::min({adjusted.height(), original.height(), coverage.height()});
    parallelRows(0, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            SampleOf<S>* a = adjusted.row(y);
            const SampleOf<S>* o = original.row(y);
            const SampleOf<S>* c = coverage.row(y);
            for (int x = 0; x < w; x++) {
                const uint32_t k = std::min<uint32_t>(c[x], one);
                if (k == one) continue;
                for (int i = 0; i < n; i++) {
                    const size_t at = size_t(x) * size_t(n) + size_t(i);
                    a[at] = SampleOf<S>((a[at] * k + o[at] * (one - k) + one / 2) / one);
                }
            }
        }
    });
}

} // namespace

bool filterAppliesInMode(FilterKind, ColorMode) { return true; }

AnyImage filteredInMode(FilterKind kind, const AnyImage& image, ColorMode mode, const FilterSettings& settings, double scale, uint32_t seed) {
    if (!image || mode == ColorMode::RGB || image.channels() != colorModeChannels(mode)) return {};
    if (mode == ColorMode::CMYK) {
        if (const auto& c8 = image.c8()) return ImageC8Ptr(filterCmyk<SampleType::U8>(kind, *c8, settings, scale, seed));
        if (const auto& u16 = image.u16()) return Image16Ptr(filterCmyk<SampleType::U16>(kind, *u16, settings, scale, seed));
        return {};
    }
    if (const auto& u8 = image.u8()) return ImagePtr(filterLab(kind, *u8, settings, scale, seed, 255.0));
    if (const auto& u16 = image.u16()) return Image16Ptr(filterLab(kind, *u16, settings, scale, seed, double(one16)));
    return {};
}

AnyImage growImageAny(const AnyImage& image, const LayerTransform& transform, int margin, LayerTransform& grownTransform) {
    if (const auto& u8 = image.u8()) return growImage(*u8, transform, margin, grownTransform);
    if (const auto& f = image.f32()) return growImage(*f, transform, margin, grownTransform);
    const ColorMode mode = image.channels() == 5 ? ColorMode::CMYK : ColorMode::RGB;
    if (const auto& c8 = image.c8()) { auto out = growN<SampleType::U8>(*c8, transform, margin, grownTransform, mode); return out ? AnyImage(ImageC8Ptr(out)) : AnyImage(); }
    if (const auto& u16 = image.u16()) {
        if (u16->channels() == 4) return growImage(*u16, transform, margin, grownTransform);
        auto out = growN<SampleType::U16>(*u16, transform, margin, grownTransform, mode);
        return out ? AnyImage(Image16Ptr(out)) : AnyImage();
    }
    return {};
}

AnyGray selectionInGridAny(const AnyGray& selection, const Affine& pixelToDocument, int width, int height) {
    if (const auto& u8 = selection.u8()) return selectionInGrid(*u8, pixelToDocument, width, height);
    if (const auto& u16 = selection.u16()) return selectionInGrid(*u16, pixelToDocument, width, height);
    if (const auto& f = selection.f32()) return selectionInGrid(*f, pixelToDocument, width, height);
    return {};
}

AnyImage blendThroughCoverageAny(const AnyImage& adjusted, const AnyImage& original, const AnyGray& coverage) {
    if (!adjusted || !original || !coverage || adjusted.channels() != original.channels()) return adjusted;
    if (adjusted.u8() && original.u8() && coverage.u8()) {
        auto out = std::make_shared<Image>(*adjusted.u8());
        blendThroughCoverage(*out, *original.u8(), *coverage.u8());
        return ImagePtr(out);
    }
    if (adjusted.c8() && original.c8() && coverage.u8()) {
        auto out = std::make_shared<ImageC8>(*adjusted.c8());
        blendN<SampleType::U8>(*out, *original.c8(), *coverage.u8());
        return ImageC8Ptr(out);
    }
    if (adjusted.u16() && original.u16() && coverage.u16()) {
        auto out = std::make_shared<Image16>(*adjusted.u16());
        blendN<SampleType::U16>(*out, *original.u16(), *coverage.u16());
        return Image16Ptr(out);
    }
    if (adjusted.f32() && original.f32() && coverage.f32()) {
        auto out = std::make_shared<ImageF>(*adjusted.f32());
        blendThroughCoverage(*out, *original.f32(), *coverage.f32());
        return ImageFPtr(out);
    }
    return adjusted;
}

} // namespace compositor
