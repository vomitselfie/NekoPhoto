// The Filter menu's grid filters (filters.h, applyGridFilter): the filters Photoshop names that NekoPhoto drew only as
// Smart Filters, run destructively through the same kernels (a one-entry stack, smartfilter.h), and the filters ported
// from PhotoCraft (filters_photocraft.cpp), run on a float plane of straight colour at any depth and layout.
#include "compositor/filters.h"
#include "compositor/colormodes.h"
#include "compositor/depth.h"
#include "compositor/smartfilter.h"
#include "compositor/supports.h"
#include "filters_photocraft.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace compositor {

namespace {

double finiteOr(double v, double fallback) { return std::isfinite(v) ? v : fallback; }
double clampD(double v, double lo, double hi, double fallback) { return std::clamp(finiteOr(v, fallback), lo, hi); }

} // namespace

std::optional<SmartFilterParameters> smartFilterParametersFor(FilterKind kind, const FilterSettings& settings) {
    using namespace smartfilter;
    const FilterSettings s = settings.normalizedFor(kind);
    SmartFilterParameters p;
    switch (kind) {
    case FilterKind::BoxBlur: p = BoxBlur{s.radius}; break;
    case FilterKind::RadialBlur: p = RadialBlur{int32_t(std::lround(s.amount)), s.quality <= 0 ? 8 : s.quality == 1 ? 16 : 32}; break;
    case FilterKind::SurfaceBlur: p = SurfaceBlur{s.radius, s.threshold}; break;
    case FilterKind::DustAndScratches: p = DustAndScratches{int32_t(std::lround(s.radius)), s.threshold}; break;
    case FilterKind::Median: p = Median{s.radius}; break;
    case FilterKind::UnsharpMask: p = UnsharpMask{s.amount, s.radius, s.threshold}; break;
    case FilterKind::HighPass: p = HighPass{s.radius}; break;
    case FilterKind::Emboss: p = Emboss{int32_t(std::lround(s.angle)), s.height, int32_t(std::lround(s.amount))}; break;
    case FilterKind::Mosaic: p = Mosaic{s.cellSize}; break;
    default: return std::nullopt;
    }
    clampSmartFilterParameters(p);
    return p;
}

namespace {

// ---- the kernels' route --------------------------------------------------------------------------------------

/// `part` (placed at `x`, `y`) pasted into a transparent buffer of `like`'s size and layout.
AnyImage pasteInto(const AnyImage& like, const AnyImage& part, int x, int y) {
    const int w = like.width(), h = like.height();
    auto copyRows = [&](auto* dst, const auto* srcImage, int n, size_t bytes) {
        for (int row = 0; row < srcImage->height(); row++) {
            const int ty = y + row;
            if (ty < 0 || ty >= h) continue;
            const int x0 = std::max(0, x), x1 = std::min(w, x + srcImage->width());
            if (x1 <= x0) continue;
            std::memcpy(dst->pixel(x0, ty), srcImage->pixel(x0 - x, row), size_t(x1 - x0) * size_t(n) * bytes);
        }
    };
    if (const ImagePtr& u8 = part.u8()) {
        auto out = std::make_shared<Image>(w, h);
        copyRows(out.get(), u8.get(), 4, 1);
        return ImagePtr(out);
    }
    if (const ImageC8Ptr& c8 = part.c8()) {
        auto out = std::make_shared<ImageC8>(w, h, 5);
        copyRows(out.get(), c8.get(), 5, 1);
        return ImageC8Ptr(out);
    }
    if (const Image16Ptr& u16 = part.u16()) {
        auto out = std::make_shared<Image16>(w, h, u16->channels());
        copyRows(out.get(), u16.get(), u16->channels(), 2);
        return Image16Ptr(out);
    }
    return {};
}

AnyImage throughKernels(const SmartFilterParameters& parameters, const AnyImage& image, ColorMode mode) {
    SmartFilterStack stack;
    stack.supported = true;
    SmartFilterEntry entry;
    entry.parameters = parameters;
    entry.name = smartFilterName(parameters);
    stack.entries.push_back(entry);
    const PixelRect canvas{0, 0, image.width(), image.height()};
    auto r = renderSmartFilterStackAny(image, 0, 0, canvas, stack, mode);
    if (!r || !r->image) return {};
    return pasteInto(image, r->image, r->x, r->y);
}

// ---- the PhotoCraft route ------------------------------------------------------------------------------------

/// A buffer's samples as straight colour over `one`, alpha last.
template <class Img>
photocraft::Plane toPlane(const Img& image, int n, float one) {
    photocraft::Plane plane(image.width(), image.height(), n);
    for (int y = 0; y < image.height(); y++)
        for (int x = 0; x < image.width(); x++) {
            const auto* p = image.pixel(x, y);
            float* q = plane.at(x, y);
            const float a = float(p[n - 1]);
            q[n - 1] = a / one;
            for (int c = 0; c < n - 1; c++) q[c] = a > 0 ? float(p[c]) / a : 0.0f;
        }
    return plane;
}

/// Back into `image`'s layout, premultiplied and rounded; only the pixels of `changed` are written.
template <class Img>
void fromPlane(const photocraft::Plane& plane, Img& image, int n, float one, bool integral, const photocraft::Rect& changed) {
    for (int y = changed.y0; y < changed.y1; y++)
        for (int x = changed.x0; x < changed.x1; x++) {
            auto* p = image.pixel(x, y);
            using T = std::remove_reference_t<decltype(*p)>;
            const float* q = plane.at(x, y);
            if (integral) {
                const float a = std::round(std::clamp(q[n - 1], 0.0f, 1.0f) * one);
                p[n - 1] = T(a);
                for (int c = 0; c < n - 1; c++) p[c] = T(std::min(a, std::round(std::clamp(q[c], 0.0f, 1.0f) * a)));
            } else {
                const float a = std::max(q[n - 1], 0.0f);
                p[n - 1] = T(a);
                for (int c = 0; c < n - 1; c++) p[c] = T(std::max(q[c], 0.0f) * a);
            }
        }
}

photocraft::Edge edgeOf(int undefinedAreas) {
    return undefinedAreas == 1 ? photocraft::Edge::Repeat : undefinedAreas == 2 ? photocraft::Edge::Transparent : photocraft::Edge::Wrap;
}

/// The ported filter on `src` into `out` (a copy of `src`); `changed` is where it may differ.
bool runPorted(FilterKind kind, const photocraft::Plane& src, photocraft::Plane& out, const FilterSettings& s, const GridFilterContext& context,
               const photocraft::Rect& bounds, photocraft::Rect& changed, float labNeutral) {
    using namespace photocraft;
    changed = bounds;
    const int colours = src.channels - 1;
    switch (kind) {
    case FilterKind::Twirl: twirl(src, out, bounds, float(s.angle)); return true;
    case FilterKind::Pinch: pinch(src, out, bounds, float(s.amount)); return true;
    case FilterKind::Spherize: spherize(src, out, bounds, float(s.amount), SpherizeMode(std::clamp(s.style, 0, 2))); return true;
    case FilterKind::Wave: {
        WaveSpec w;
        w.generators = uint32_t(s.generators);
        w.wavelengthMin = float(s.wavelengthMin);
        w.wavelengthMax = float(s.wavelengthMax);
        w.amplitudeMin = float(s.amplitudeMin);
        w.amplitudeMax = float(s.amplitudeMax);
        w.type = WaveType(std::clamp(s.style, 0, 2));
        w.undefined = edgeOf(std::min(s.undefinedAreas, 1));
        w.seed = context.seed;
        wave(src, out, bounds, w);
        return true;
    }
    case FilterKind::Ripple: ripple(src, out, bounds, float(s.amount), RippleSize(std::clamp(s.style, 0, 2))); return true;
    case FilterKind::PolarCoordinates: polar(src, out, bounds, PolarMode(std::clamp(s.style, 0, 1))); return true;
    case FilterKind::ZigZag: zigzag(src, out, bounds, float(s.amount), float(s.ridges), ZigZagStyle(std::clamp(s.style, 0, 2))); return true;
    case FilterKind::Shear: shear(src, out, bounds, float(s.amount), edgeOf(std::min(s.undefinedAreas, 1))); return true;
    case FilterKind::Offset: offset(src, out, bounds, s.horizontal, s.vertical, edgeOf(s.undefinedAreas)); return true;
    case FilterKind::Maximum:
    case FilterKind::Minimum:
        changed = {0, 0, src.width, src.height};
        minMax(src, out, float(s.radius), kind == FilterKind::Maximum, s.style == 1);
        return true;
    case FilterKind::FindEdges:
        changed = {0, 0, src.width, src.height};
        if (context.mode == ColorMode::Lab) {
            // Lab: the edges of lightness, in grey.
            findEdges(src, out, 1);
            for (int y = 0; y < out.height; y++)
                for (int x = 0; x < out.width; x++) out.at(x, y)[1] = out.at(x, y)[2] = labNeutral;
        } else {
            findEdges(src, out, colours);
        }
        return true;
    case FilterKind::Clouds:
    case FilterKind::DifferenceClouds: {
        changed = {0, 0, src.width, src.height};
        const float base = cloudsBase(context.documentSide > 0 ? context.documentSide : std::max(src.width, src.height));
        for (int y = 0; y < src.height; y++)
            for (int x = 0; x < src.width; x++) {
                const float t = cloudsValue(float(context.originX + x), float(context.originY + y), base, context.seed);
                float* p = out.at(x, y);
                for (int k = 0; k < 3; k++) {
                    const float c = float(context.foreground[k] + (context.background[k] - context.foreground[k]) * t);
                    p[k] = kind == FilterKind::DifferenceClouds ? std::fabs(p[k] - c) : c;
                }
                if (kind == FilterKind::Clouds) p[3] = 1.0f;
            }
        return true;
    }
    default: return false;
    }
}

template <class Img>
std::shared_ptr<Img> ported(FilterKind kind, const Img& image, int n, float one, bool integral, const FilterSettings& s,
                            const GridFilterContext& context, const photocraft::Rect& bounds, float labNeutral) {
    const photocraft::Plane src = toPlane(image, n, one);
    photocraft::Plane out = src;
    photocraft::Rect changed;
    if (!runPorted(kind, src, out, s, context, bounds, changed, labNeutral)) return nullptr;
    auto result = std::make_shared<Img>(image);
    fromPlane(out, *result, n, one, integral, changed);
    return result;
}

} // namespace

FilterSettings FilterSettings::defaults(FilterKind kind) {
    FilterSettings s;
    switch (kind) {
    case FilterKind::BoxBlur: s.radius = 10; break;
    case FilterKind::RadialBlur: s.amount = 10; s.quality = 1; break;
    case FilterKind::SurfaceBlur: s.radius = 5; s.threshold = 15; break;
    case FilterKind::DustAndScratches: s.radius = 1; s.threshold = 0; break;
    case FilterKind::Median: s.radius = 1; break;
    case FilterKind::UnsharpMask: s.amount = 50; s.radius = 1; s.threshold = 0; break;
    case FilterKind::HighPass: s.radius = 10; break;
    case FilterKind::Emboss: s.angle = 135; s.height = 3; s.amount = 100; break;
    case FilterKind::Mosaic: s.cellSize = 10; break;
    case FilterKind::Twirl: s.angle = 50; break;
    case FilterKind::Pinch: s.amount = 50; break;
    case FilterKind::Spherize: s.amount = 100; break;
    case FilterKind::Ripple: s.amount = 100; s.style = 1; break;
    case FilterKind::ZigZag: s.amount = 10; s.ridges = 5; s.style = 2; break;
    case FilterKind::Shear: s.amount = 0; break;
    case FilterKind::Maximum: case FilterKind::Minimum: s.radius = 1; break;
    default: break;
    }
    return s;
}

FilterSettings FilterSettings::normalizedFor(FilterKind kind) const {
    if (!isGridFilter(kind)) return normalized();
    const FilterSettings d = defaults(kind);
    FilterSettings s = *this;
    auto clampI = [](int v, int lo, int hi) { return std::clamp(v, lo, hi); };
    switch (kind) {
    case FilterKind::BoxBlur: s.radius = clampD(radius, 1, 2000, d.radius); break;
    case FilterKind::RadialBlur: s.amount = clampD(amount, 1, 100, d.amount); s.quality = clampI(quality, 0, 2); break;
    case FilterKind::SurfaceBlur: s.radius = clampD(radius, 1, 100, d.radius); s.threshold = clampI(threshold, 2, 255); break;
    case FilterKind::DustAndScratches: s.radius = clampD(radius, 1, 500, d.radius); s.threshold = clampI(threshold, 0, 255); break;
    case FilterKind::Median: s.radius = clampD(radius, 1, 500, d.radius); break;
    case FilterKind::UnsharpMask:
        s.amount = clampD(amount, 1, 500, d.amount); s.radius = clampD(radius, 0.1, 1000, d.radius); s.threshold = clampI(threshold, 0, 255); break;
    case FilterKind::HighPass: s.radius = clampD(radius, 0.1, 1000, d.radius); break;
    case FilterKind::Emboss:
        s.angle = clampD(angle, -360, 360, d.angle); s.height = clampI(height, 1, 100); s.amount = clampD(amount, 1, 500, d.amount); break;
    case FilterKind::Mosaic: s.cellSize = clampI(cellSize, 2, 200); break;
    case FilterKind::Twirl: s.angle = clampD(angle, -999, 999, d.angle); break;
    case FilterKind::Pinch: s.amount = clampD(amount, -100, 100, d.amount); break;
    case FilterKind::Spherize: s.amount = clampD(amount, -100, 100, d.amount); s.style = clampI(style, 0, 2); break;
    case FilterKind::Wave:
        s.generators = clampI(generators, 1, 999);
        s.wavelengthMin = clampD(wavelengthMin, 1, 998, d.wavelengthMin);
        s.wavelengthMax = clampD(wavelengthMax, s.wavelengthMin + 1, 999, d.wavelengthMax);
        s.amplitudeMin = clampD(amplitudeMin, 1, 998, d.amplitudeMin);
        s.amplitudeMax = clampD(amplitudeMax, s.amplitudeMin, 999, d.amplitudeMax);
        s.style = clampI(style, 0, 2);
        s.undefinedAreas = clampI(undefinedAreas, 0, 1);
        break;
    case FilterKind::Ripple: s.amount = clampD(amount, -999, 999, d.amount); s.style = clampI(style, 0, 2); break;
    case FilterKind::PolarCoordinates: s.style = clampI(style, 0, 1); break;
    case FilterKind::ZigZag: s.amount = clampD(amount, -100, 100, d.amount); s.ridges = clampD(ridges, 0, 20, d.ridges); s.style = clampI(style, 0, 2); break;
    case FilterKind::Shear: s.amount = clampD(amount, -100, 100, d.amount); s.undefinedAreas = clampI(undefinedAreas, 0, 1); break;
    case FilterKind::Maximum: case FilterKind::Minimum: s.radius = clampD(radius, 0.2, 500, d.radius); s.style = clampI(style, 0, 1); break;
    case FilterKind::Offset:
        s.horizontal = clampI(horizontal, -30000, 30000); s.vertical = clampI(vertical, -30000, 30000); s.undefinedAreas = clampI(undefinedAreas, 0, 2); break;
    default: break;
    }
    return s;
}

int gridFilterMargin(FilterKind kind, const FilterSettings& settings) {
    const FilterSettings s = settings.normalizedFor(kind);
    switch (kind) {
    case FilterKind::BoxBlur: case FilterKind::SurfaceBlur: return int(std::ceil(s.radius)) + 1;
    case FilterKind::Maximum: return int(std::ceil(s.radius)) + 1;
    default: return 0;
    }
}

bool gridFilterTrims(FilterKind kind) {
    return kind == FilterKind::BoxBlur || kind == FilterKind::SurfaceBlur || kind == FilterKind::Maximum;
}

AnyImage applyGridFilter(FilterKind kind, const AnyImage& image, const FilterSettings& settings, const GridFilterContext& context) {
    if (!isGridFilter(kind) || !image || image.width() <= 0 || image.height() <= 0) return {};
    if (!supports(std::string("filter.") + filterKindName(kind), image.sampleType(), context.mode)) return {};
    if (image.channels() != colorModeChannels(context.mode)) return {};
    const FilterSettings s = settings.normalizedFor(kind);
    if (auto parameters = smartFilterParametersFor(kind, s)) return throughKernels(*parameters, image, context.mode);

    photocraft::Rect bounds{0, 0, image.width(), image.height()};
    if (context.boundsWidth > 0 && context.boundsHeight > 0) {
        bounds = {std::max(0, context.boundsX), std::max(0, context.boundsY), std::min(image.width(), context.boundsX + context.boundsWidth),
                  std::min(image.height(), context.boundsY + context.boundsHeight)};
        if (bounds.empty()) return image;
    }
    const int n = image.channels();
    if (const ImagePtr& u8 = image.u8()) {
        const float neutral = float(labOffset<SampleType::U8>()) / 255.0f;
        return ImagePtr(ported(kind, *u8, 4, 255.0f, true, s, context, bounds, neutral));
    }
    if (const ImageC8Ptr& c8 = image.c8()) return ImageC8Ptr(ported(kind, *c8, 5, 255.0f, true, s, context, bounds, 0.5f));
    if (const Image16Ptr& u16 = image.u16()) {
        const float neutral = float(labOffset<SampleType::U16>()) / float(one16);
        return Image16Ptr(ported(kind, *u16, n, float(one16), true, s, context, bounds, neutral));
    }
    if (const ImageFPtr& f = image.f32()) return ImageFPtr(ported(kind, *f, 4, 1.0f, false, s, context, bounds, 0.5f));
    return {};
}

bool coverageBounds(const AnyGray& coverage, GridFilterContext& context) {
    PixelBounds b;
    if (const GrayPtr& u8 = coverage.u8()) b = nonzeroBounds(*u8);
    else if (const Gray16Ptr& u16 = coverage.u16()) b = nonzeroBounds(*u16);
    else if (const GrayFPtr& f = coverage.f32()) b = nonzeroBounds(*f);
    else return false;
    if (b.isEmpty()) return false;
    context.boundsX = b.x0;
    context.boundsY = b.y0;
    context.boundsWidth = b.x1 - b.x0;
    context.boundsHeight = b.y1 - b.y0;
    return true;
}

} // namespace compositor
