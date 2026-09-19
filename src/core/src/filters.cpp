#include "compositor/filters.h"
#include "compositor/blur.h"
#include "compositor/kernels.h"
#include "compositor/parallel.h"
#include <algorithm>
#include <cmath>
#include <cstring>

extern "C" {
#include "LensPixels.h"
#include "NoisePixels.h"
}

namespace compositor {

namespace {
double clampFinite(double v, double lo, double hi, double fallback) { return std::isfinite(v) ? std::min(hi, std::max(lo, v)) : fallback; }
constexpr double lensStrength = 0.35;
} // namespace

const char* filterKindName(FilterKind kind) {
    switch (kind) {
    case FilterKind::GaussianBlur: return "Gaussian Blur";
    case FilterKind::MotionBlur: return "Motion Blur";
    case FilterKind::AddNoise: return "Add Noise";
    case FilterKind::LensCorrection: return "Lens Correction";
    }
    return "";
}

FilterSettings FilterSettings::normalized() const {
    FilterSettings s = *this;
    s.radius = clampFinite(radius, 0.1, 250, 1);
    s.angle = clampFinite(angle, -90, 90, 0);
    s.distance = clampFinite(distance, 1, 2000, 10);
    s.amount = clampFinite(amount, 0.1, 400, 10);
    s.distortion = clampFinite(distortion, -100, 100, 0);
    return s;
}

double blurMargin(FilterKind kind, const FilterSettings& settings) {
    FilterSettings s = settings.normalized();
    switch (kind) {
    case FilterKind::GaussianBlur: return s.radius * 3 + 2;
    case FilterKind::MotionBlur: return s.distance / 2 + 2;
    default: return 0;
    }
}

std::shared_ptr<Image> growImage(const Image& image, const LayerTransform& transform, int margin, LayerTransform& grownTransform) {
    if (margin <= 0) { grownTransform = transform; return std::make_shared<Image>(image); }
    int w = image.width() + 2 * margin, h = image.height() + 2 * margin;
    if (w > 30000 || h > 30000 || (long long)w * h > Document::pixelBudget) return nullptr;
    auto out = std::make_shared<Image>(w, h);
    for (int y = 0; y < image.height(); y++) std::memcpy(out->pixel(margin, y + margin), image.row(y), size_t(image.width()) * 4);
    LayerTransform t = transform;
    t.size = {double(w) * transform.size.width / image.width(), double(h) * transform.size.height / image.height()};
    Point c = transform.center();
    t.origin = {c.x - t.size.width / 2, c.y - t.size.height / 2};
    grownTransform = t;
    return out;
}

std::shared_ptr<Image> trimToPixels(const Image& image, const LayerTransform& transform, LayerTransform& trimmedTransform) {
    PixelBounds b = alphaBounds(image);
    trimmedTransform = transform;
    if (b.isEmpty() || (b.x0 == 0 && b.y0 == 0 && b.x1 == image.width() && b.y1 == image.height())) return std::make_shared<Image>(image);
    auto cropped = cropImage(image, b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0);
    LayerTransform t = transform;
    t.size = {(b.x1 - b.x0) * transform.size.width / image.width(), (b.y1 - b.y0) * transform.size.height / image.height()};
    Point middle = transform.pixelToDocument(image.width(), image.height()).apply({(b.x0 + b.x1) / 2.0, (b.y0 + b.y1) / 2.0});
    t.origin = {middle.x - t.size.width / 2, middle.y - t.size.height / 2};
    trimmedTransform = t;
    return cropped;
}

// ---- Blurs: see blur.cpp ---------------------------------------------------------------------

void applyFilter(FilterKind kind, Image& image, const FilterSettings& settings, double scale, uint32_t seed) {
    FilterSettings s = settings.normalized();
    switch (kind) {
    case FilterKind::GaussianBlur: gaussianBlur(image, s.radius * scale); break;
    case FilterKind::MotionBlur: motionBlur(image, s.distance * scale, s.angle); break;
    case FilterKind::AddNoise:
        kernels::addNoise(image, float(s.amount), s.gaussian, s.monochromatic, seed);
        break;
    case FilterKind::LensCorrection: {
        Image source = image;
        kernels::lensDistort(source, image, s.distortion / 100 * lensStrength);
        break;
    }
    }
}

// ---- Selections on the layer grid ----------------------------------------------------------

std::shared_ptr<GrayImage> selectionInGrid(const GrayImage& selection, const Affine& pixelToDocument, int width, int height) {
    auto out = std::make_shared<GrayImage>(width, height, 0);
    parallelRows(0, height, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            Point d = pixelToDocument.apply({0.5, y + 0.5});
            Point dd = pixelToDocument.applyVector({1, 0});
            uint8_t* row = out->row(y);
            for (int x = 0; x < width; x++, d = d + dd) {
                int sx = int(std::floor(d.x)), sy = int(std::floor(d.y));
                if (sx < 0 || sy < 0 || sx >= selection.width() || sy >= selection.height()) continue;
                row[x] = selection.at(sx, sy);
            }
        }
    });
    return out;
}

void blendThroughCoverage(Image& adjusted, const Image& original, const GrayImage& coverage) {
    int w = std::min({adjusted.width(), original.width(), coverage.width()}), h = std::min({adjusted.height(), original.height(), coverage.height()});
    parallelRows(0, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            uint8_t* a = adjusted.row(y);
            const uint8_t* o = original.row(y);
            const uint8_t* c = coverage.row(y);
            for (int x = 0; x < w; x++) {
                unsigned k = c[x];
                if (k == 255) continue;
                for (int i = 0; i < 4; i++) a[x * 4 + i] = uint8_t((a[x * 4 + i] * k + o[x * 4 + i] * (255 - k) + 127) / 255);
            }
        }
    });
}

void blendThroughCoverage(GrayImage& adjusted, const GrayImage& original, const GrayImage& coverage) {
    int w = std::min({adjusted.width(), original.width(), coverage.width()}), h = std::min({adjusted.height(), original.height(), coverage.height()});
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        unsigned k = coverage.at(x, y);
        adjusted.at(x, y) = uint8_t((adjusted.at(x, y) * k + original.at(x, y) * (255 - k) + 127) / 255);
    }
}

} // namespace compositor
