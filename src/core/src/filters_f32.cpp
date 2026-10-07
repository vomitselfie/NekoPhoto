// The Filter menu's built-in filters and the selection helpers at 32 bits (filters.h): premultiplied linear float,
// nothing rounded, colour above 1 kept.
//
// Gaussian and Motion Blur average light as it is (blur_f32.cpp). Add Noise draws the 8- and 16-bit pattern for a seed
// and adds it to the colour in the document's encoding (its curve, continued above 1), so a given amount looks as it
// does at 8 bits; Lens Correction resamples the linear values with exact bilinear or Catmull-Rom weights.
#include "compositor/filters.h"
#include "compositor/adjustments.h"
#include "compositor/blur.h"
#include "compositor/colormgmt.h"
#include "compositor/depth.h"
#include "compositor/parallel.h"
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

/// kernels::addNoise at 32 bits: the same pattern for the same seed, the spread in 8-bit levels of the encoded colour.
void addNoiseF(ImageF& image, float amount, bool gaussian, bool monochromatic, uint32_t seed, const TransferCurve& curve) {
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
            float* p = image.row(y);
            for (uint32_t x = 0; x < width; x++, p += 4) {
                const float alpha = p[3];
                if (!(alpha > 0)) continue;
                const uint32_t base = hash32(seed ^ hash32(uint32_t(y) * width + x));
                const float mono = monochromatic ? sample(base) : 0;
                for (int c = 0; c < 3; c++) {
                    const float n = monochromatic ? mono : sample(base + uint32_t(c) * 0x9e3779b9U);
                    const float encoded = std::max(0.0f, encodeExtended(curve, p[c] / alpha) + n);
                    p[c] = cleanColour(decodeExtended(curve, encoded) * alpha);
                }
            }
        }
    });
}

/// Catmull-Rom's four weights at a fraction, exact.
inline void catmullRom(double t, double w[4]) {
    const double t2 = t * t, t3 = t2 * t;
    w[0] = (-t3 + 2 * t2 - t) / 2;
    w[1] = (3 * t3 - 5 * t2 + 2) / 2;
    w[2] = (-3 * t3 + 4 * t2 + t) / 2;
    w[3] = (t3 - t2) / 2;
}

/// kernels::lensDistort at 32 bits: the same taps, exact weights, zero outside.
void lensDistortF(const ImageF& source, ImageF& destination, double k, bool bicubic) {
    const int width = source.width(), height = source.height();
    const double cx = width * 0.5, cy = height * 0.5;
    const double halfDiagonal2 = cx * cx + cy * cy;
    parallelRows(0, height, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            const double dy = y + 0.5 - cy;
            float* out = destination.row(y);
            for (int x = 0; x < width; x++, out += 4) {
                const double dx = x + 0.5 - cx;
                const double scale = 1.0 - k * (dx * dx + dy * dy) / halfDiagonal2;
                const double sx = cx + dx * scale - 0.5, sy = cy + dy * scale - 0.5;
                const double fx0 = std::floor(sx), fy0 = std::floor(sy);
                const double fx = sx - fx0, fy = sy - fy0;
                const long x0 = long(fx0), py0 = long(fy0);
                double sums[4] = {0, 0, 0, 0};
                const int taps = bicubic ? 4 : 2;
                const long firstX = bicubic ? x0 - 1 : x0, firstY = bicubic ? py0 - 1 : py0;
                double wx[4], wy[4];
                if (bicubic) { catmullRom(fx, wx); catmullRom(fy, wy); }
                else { wx[0] = 1 - fx; wx[1] = fx; wy[0] = 1 - fy; wy[1] = fy; }
                for (int j = 0; j < taps; j++) {
                    const long row = firstY + j;
                    if (row < 0 || row >= height || wy[j] == 0) continue;
                    const float* line = source.row(int(row));
                    for (int i = 0; i < taps; i++) {
                        const long column = firstX + i;
                        if (column < 0 || column >= width) continue;
                        const double weight = wy[j] * wx[i];
                        const float* p = line + size_t(column) * 4;
                        for (int c = 0; c < 4; c++) sums[c] += weight * p[c];
                    }
                }
                for (int c = 0; c < 3; c++) out[c] = cleanColour(float(sums[c]));
                out[3] = cleanCoverage(float(sums[3]));
            }
        }
    });
}

} // namespace

std::shared_ptr<ImageF> growImage(const ImageF& image, const LayerTransform& transform, int margin, LayerTransform& grownTransform) {
    if (margin <= 0) { grownTransform = transform; return std::make_shared<ImageF>(image); }
    const int w = image.width() + 2 * margin, h = image.height() + 2 * margin;
    if (w > 30000 || h > 30000 || (long long)w * h > Document::imagePixelBudget(SampleType::F32)) return nullptr;
    auto out = std::make_shared<ImageF>(w, h);
    for (int y = 0; y < image.height(); y++) std::memcpy(out->pixel(margin, y + margin), image.row(y), size_t(image.width()) * 4 * sizeof(float));
    LayerTransform t = transform;
    t.size = {double(w) * transform.size.width / image.width(), double(h) * transform.size.height / image.height()};
    const Point c = transform.center();
    t.origin = {c.x - t.size.width / 2, c.y - t.size.height / 2};
    grownTransform = t;
    return out;
}

std::shared_ptr<ImageF> trimToPixels(const ImageF& image, const LayerTransform& transform, LayerTransform& trimmedTransform) {
    const PixelBounds b = alphaBounds(image);
    trimmedTransform = transform;
    if (b.isEmpty() || (b.x0 == 0 && b.y0 == 0 && b.x1 == image.width() && b.y1 == image.height())) return std::make_shared<ImageF>(image);
    auto cropped = cropImage(image, b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0);
    LayerTransform t = transform;
    t.size = {(b.x1 - b.x0) * transform.size.width / image.width(), (b.y1 - b.y0) * transform.size.height / image.height()};
    const Point middle = transform.pixelToDocument(image.width(), image.height()).apply({(b.x0 + b.x1) / 2.0, (b.y0 + b.y1) / 2.0});
    t.origin = {middle.x - t.size.width / 2, middle.y - t.size.height / 2};
    trimmedTransform = t;
    return cropped;
}

void applyFilter(FilterKind kind, ImageF& image, const FilterSettings& settings, const TransferCurve& curve, double scale, uint32_t seed) {
    const FilterSettings s = settings.normalized();
    switch (kind) {
    case FilterKind::GaussianBlur: gaussianBlur(image, s.radius * scale); break;
    case FilterKind::MotionBlur: motionBlur(image, s.distance * scale, s.angle); break;
    case FilterKind::AddNoise: addNoiseF(image, float(s.amount), s.gaussian, s.monochromatic, seed, curve); break;
    case FilterKind::LensCorrection: {
        const ImageF source = image;
        lensDistortF(source, image, s.distortion / 100 * lensStrength, s.bicubic);
        break;
    }
    default: break;   // the grid filters: applyGridFilter
    }
}

std::shared_ptr<GrayF> selectionInGrid(const GrayF& selection, const Affine& pixelToDocument, int width, int height) {
    auto out = std::make_shared<GrayF>(width, height, 0.0f);
    parallelRows(0, height, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            Point d = pixelToDocument.apply({0.5, y + 0.5});
            const Point dd = pixelToDocument.applyVector({1, 0});
            float* row = out->row(y);
            for (int x = 0; x < width; x++, d = d + dd) {
                const int sx = int(std::floor(d.x)), sy = int(std::floor(d.y));
                if (sx < 0 || sy < 0 || sx >= selection.width() || sy >= selection.height()) continue;
                row[x] = selection.at(sx, sy);
            }
        }
    });
    return out;
}

void blendThroughCoverage(ImageF& adjusted, const ImageF& original, const GrayF& coverage) {
    const int w = std::min({adjusted.width(), original.width(), coverage.width()}), h = std::min({adjusted.height(), original.height(), coverage.height()});
    parallelRows(0, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            float* a = adjusted.row(y);
            const float* o = original.row(y);
            const float* c = coverage.row(y);
            for (int x = 0; x < w; x++) {
                const float k = cleanCoverage(c[x]);
                if (k == 1.0f) continue;
                for (int i = 0; i < 4; i++) a[x * 4 + i] = o[x * 4 + i] + (a[x * 4 + i] - o[x * 4 + i]) * k;
            }
        }
    });
}

void blendThroughCoverage(GrayF& adjusted, const GrayF& original, const GrayF& coverage) {
    const int w = std::min({adjusted.width(), original.width(), coverage.width()}), h = std::min({adjusted.height(), original.height(), coverage.height()});
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const float k = cleanCoverage(coverage.at(x, y));
            adjusted.at(x, y) = original.at(x, y) + (adjusted.at(x, y) - original.at(x, y)) * k;
        }
}

} // namespace compositor
