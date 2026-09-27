// The Filter menu's built-in filters and the selection helpers at 16 bits (filters.h): the same maths as the 8-bit
// kernels on 0..32768 samples, without their rounding to bytes between steps.
#include "compositor/filters.h"
#include "compositor/blur.h"
#include "compositor/depth.h"
#include "compositor/parallel.h"
#include "compositor/resample.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace compositor {

namespace {

constexpr double lensStrength = 0.35;   // filters.cpp

inline uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

/// kernels::addNoise at 16 bits: the same pattern for the same seed, the spread in 8-bit levels scaled to the range.
void addNoise16(Image16& image, float amount, bool gaussian, bool monochromatic, uint32_t seed) {
    const float spread = amount / 100.0f * 127.5f / 255.0f;
    const uint32_t width = uint32_t(image.width());
    auto unit = [](uint32_t key) { return float(hash32(key) >> 8) * (1.0f / 16777216.0f); };
    auto sample = [&](uint32_t key) {
        if (!gaussian) return (unit(key) * 2.0f - 1.0f) * spread;
        const float u1 = unit(key), u2 = unit(key ^ 0x68e31da4U);
        return std::sqrt(-2.0f * std::log(1.0f - u1)) * std::cos(6.2831853f * u2) * spread * (2.0f / 3.0f);
    };
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            uint16_t* p = image.row(y);
            for (uint32_t x = 0; x < width; x++, p += 4) {
                const uint32_t alpha = p[3];
                if (!alpha) continue;
                const uint32_t base = hash32(seed ^ hash32(uint32_t(y) * width + x));
                const float mono = monochromatic ? sample(base) : 0;
                for (int c = 0; c < 3; c++) {
                    const float n = monochromatic ? mono : sample(base + uint32_t(c) * 0x9e3779b9U);
                    const float value = std::clamp(float(p[c]) / float(alpha) + n, 0.0f, 1.0f);
                    p[c] = uint16_t(std::min<long>(long(alpha), std::lround(value * float(alpha))));
                }
            }
        }
    });
}

/// kernels::lensDistort at 16 bits.
void lensDistort16(const Image16& source, Image16& destination, double k, bool bicubic) {
    const int width = source.width(), height = source.height();
    const double cx = width * 0.5, cy = height * 0.5;
    const double halfDiagonal2 = cx * cx + cy * cy;
    parallelRows(0, height, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            const double dy = y + 0.5 - cy;
            uint16_t* out = destination.row(y);
            for (int x = 0; x < width; x++, out += 4) {
                const double dx = x + 0.5 - cx;
                const double scale = 1.0 - k * (dx * dx + dy * dy) / halfDiagonal2;
                const double sx = cx + dx * scale - 0.5, sy = cy + dy * scale - 0.5;
                const double fx0 = std::floor(sx), fy0 = std::floor(sy);
                const double fx = sx - fx0, fy = sy - fy0;
                const long x0 = long(fx0), py0 = long(fy0);
                if (bicubic) {
                    const int16_t *wx = catmullRomWeights(int(fx * 256)), *wy = catmullRomWeights(int(fy * 256));
                    int64_t acc[4] = {0, 0, 0, 0};
                    for (int j = 0; j < 4; j++) {
                        const long row = py0 - 1 + j;
                        if (row < 0 || row >= height || wy[j] == 0) continue;
                        const uint16_t* line = source.row(int(row));
                        int64_t h[4] = {0, 0, 0, 0};
                        for (int i = 0; i < 4; i++) {
                            const long column = x0 - 1 + i;
                            if (column < 0 || column >= width) continue;
                            const uint16_t* p = line + size_t(column) * 4;
                            for (int c = 0; c < 4; c++) h[c] += int64_t(p[c]) * wx[i];
                        }
                        for (int c = 0; c < 4; c++) acc[c] += h[c] * wy[j];
                    }
                    const int64_t a = std::clamp<int64_t>((acc[3] + 32768) >> 16, 0, one16);
                    for (int c = 0; c < 3; c++) out[c] = uint16_t(std::clamp<int64_t>((acc[c] + 32768) >> 16, 0, a));
                    out[3] = uint16_t(a);
                    continue;
                }
                double sums[4] = {0, 0, 0, 0};
                for (int j = 0; j < 2; j++) {
                    const long row = py0 + j;
                    if (row < 0 || row >= height) continue;
                    const double wy = j ? fy : 1 - fy;
                    if (wy == 0) continue;
                    const uint16_t* line = source.row(int(row));
                    for (int i = 0; i < 2; i++) {
                        const long column = x0 + i;
                        if (column < 0 || column >= width) continue;
                        const double weight = wy * (i ? fx : 1 - fx);
                        if (weight == 0) continue;
                        const uint16_t* p = line + size_t(column) * 4;
                        for (int c = 0; c < 4; c++) sums[c] += weight * p[c];
                    }
                }
                for (int c = 0; c < 4; c++) out[c] = uint16_t(std::lround(sums[c]));
            }
        }
    });
}

} // namespace

PixelBounds alphaBounds(const Image16& image) {
    PixelBounds b{image.width(), image.height(), 0, 0};
    for (int y = 0; y < image.height(); y++) {
        const uint16_t* p = image.row(y);
        for (int x = 0; x < image.width(); x++)
            if (p[x * 4 + 3]) { b.x0 = std::min(b.x0, x); b.x1 = std::max(b.x1, x + 1); b.y0 = std::min(b.y0, y); b.y1 = std::max(b.y1, y + 1); }
    }
    if (b.x1 <= b.x0) return {};
    return b;
}

std::shared_ptr<Image16> growImage(const Image16& image, const LayerTransform& transform, int margin, LayerTransform& grownTransform) {
    if (margin <= 0) { grownTransform = transform; return std::make_shared<Image16>(image); }
    const int w = image.width() + 2 * margin, h = image.height() + 2 * margin;
    if (w > 30000 || h > 30000 || (long long)w * h > Document::imagePixelBudget(SampleType::U16)) return nullptr;
    auto out = std::make_shared<Image16>(w, h);
    for (int y = 0; y < image.height(); y++) std::memcpy(out->pixel(margin, y + margin), image.row(y), size_t(image.width()) * 4 * sizeof(uint16_t));
    LayerTransform t = transform;
    t.size = {double(w) * transform.size.width / image.width(), double(h) * transform.size.height / image.height()};
    const Point c = transform.center();
    t.origin = {c.x - t.size.width / 2, c.y - t.size.height / 2};
    grownTransform = t;
    return out;
}

std::shared_ptr<Image16> trimToPixels(const Image16& image, const LayerTransform& transform, LayerTransform& trimmedTransform) {
    const PixelBounds b = alphaBounds(image);
    trimmedTransform = transform;
    if (b.isEmpty() || (b.x0 == 0 && b.y0 == 0 && b.x1 == image.width() && b.y1 == image.height())) return std::make_shared<Image16>(image);
    auto cropped = cropImage(image, b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0);
    LayerTransform t = transform;
    t.size = {(b.x1 - b.x0) * transform.size.width / image.width(), (b.y1 - b.y0) * transform.size.height / image.height()};
    const Point middle = transform.pixelToDocument(image.width(), image.height()).apply({(b.x0 + b.x1) / 2.0, (b.y0 + b.y1) / 2.0});
    t.origin = {middle.x - t.size.width / 2, middle.y - t.size.height / 2};
    trimmedTransform = t;
    return cropped;
}

void applyFilter(FilterKind kind, Image16& image, const FilterSettings& settings, double scale, uint32_t seed) {
    const FilterSettings s = settings.normalized();
    switch (kind) {
    case FilterKind::GaussianBlur: gaussianBlur(image, s.radius * scale); break;
    case FilterKind::MotionBlur: motionBlur(image, s.distance * scale, s.angle); break;
    case FilterKind::AddNoise: addNoise16(image, float(s.amount), s.gaussian, s.monochromatic, seed); break;
    case FilterKind::LensCorrection: {
        const Image16 source = image;
        lensDistort16(source, image, s.distortion / 100 * lensStrength, s.bicubic);
        break;
    }
    }
}

std::shared_ptr<Gray16> selectionInGrid(const Gray16& selection, const Affine& pixelToDocument, int width, int height) {
    auto out = std::make_shared<Gray16>(width, height, 0);
    parallelRows(0, height, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            Point d = pixelToDocument.apply({0.5, y + 0.5});
            const Point dd = pixelToDocument.applyVector({1, 0});
            uint16_t* row = out->row(y);
            for (int x = 0; x < width; x++, d = d + dd) {
                const int sx = int(std::floor(d.x)), sy = int(std::floor(d.y));
                if (sx < 0 || sy < 0 || sx >= selection.width() || sy >= selection.height()) continue;
                row[x] = selection.at(sx, sy);
            }
        }
    });
    return out;
}

void blendThroughCoverage(Image16& adjusted, const Image16& original, const Gray16& coverage) {
    const int w = std::min({adjusted.width(), original.width(), coverage.width()}), h = std::min({adjusted.height(), original.height(), coverage.height()});
    parallelRows(0, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            uint16_t* a = adjusted.row(y);
            const uint16_t* o = original.row(y);
            const uint16_t* c = coverage.row(y);
            for (int x = 0; x < w; x++) {
                const uint32_t k = std::min<uint32_t>(c[x], one16);
                if (k == one16) continue;
                for (int i = 0; i < 4; i++) a[x * 4 + i] = uint16_t((a[x * 4 + i] * k + o[x * 4 + i] * (one16 - k) + one16 / 2) >> 15);
            }
        }
    });
}

void blendThroughCoverage(Gray16& adjusted, const Gray16& original, const Gray16& coverage) {
    const int w = std::min({adjusted.width(), original.width(), coverage.width()}), h = std::min({adjusted.height(), original.height(), coverage.height()});
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint32_t k = std::min<uint32_t>(coverage.at(x, y), one16);
            adjusted.at(x, y) = uint16_t((adjusted.at(x, y) * k + original.at(x, y) * (one16 - k) + one16 / 2) >> 15);
        }
}

} // namespace compositor
