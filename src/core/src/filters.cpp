#include "compositor/filters.h"
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

// ---- Blurs ------------------------------------------------------------------------------

namespace {

// A horizontal box blur of radius r on float RGBA rows, then the transpose is handled by the caller
// running the same on columns. Outside the image is transparent.
void boxBlurRows(std::vector<float>& data, int w, int h, int r) {
    if (r <= 0) return;
    std::vector<float> row(size_t(w) * 4);
    parallelRows(0, h, [&](int y0, int y1) {
        std::vector<float> line(size_t(w) * 4);
        for (int y = y0; y < y1; y++) {
            float* src = &data[size_t(y) * w * 4];
            std::memcpy(line.data(), src, size_t(w) * 4 * sizeof(float));
            float sum[4] = {0, 0, 0, 0};
            float inv = 1.0f / (2 * r + 1);
            for (int x = -r; x <= r; x++) if (x >= 0 && x < w) for (int c = 0; c < 4; c++) sum[c] += line[size_t(x) * 4 + c];
            for (int x = 0; x < w; x++) {
                for (int c = 0; c < 4; c++) src[size_t(x) * 4 + c] = sum[c] * inv;
                int out = x - r, in = x + r + 1;
                if (out >= 0) for (int c = 0; c < 4; c++) sum[c] -= line[size_t(out) * 4 + c];
                if (in < w) for (int c = 0; c < 4; c++) sum[c] += line[size_t(in) * 4 + c];
            }
        }
    });
}

void transpose(const std::vector<float>& in, std::vector<float>& out, int w, int h) {
    out.resize(in.size());
    parallelRows(0, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < w; x++)
                for (int c = 0; c < 4; c++) out[(size_t(x) * h + y) * 4 + c] = in[(size_t(y) * w + x) * 4 + c];
    });
}

// Three box blurs approximating a Gaussian of standard deviation sigma (Kovesi's sizes).
void boxSizes(double sigma, int sizes[3]) {
    double wIdeal = std::sqrt(12 * sigma * sigma / 3 + 1);
    int wl = int(std::floor(wIdeal));
    if (wl % 2 == 0) wl--;
    int wu = wl + 2;
    double mIdeal = (12 * sigma * sigma - 3 * wl * wl - 12 * wl - 9) / (-4 * wl - 4);
    int m = int(std::round(mIdeal));
    for (int i = 0; i < 3; i++) sizes[i] = i < m ? wl : wu;
}

void gaussianRows(std::vector<float>& data, int w, int h, double sigma) {
    int radius = std::max(1, int(std::ceil(sigma * 3)));
    std::vector<float> kernel(size_t(radius) * 2 + 1);
    float total = 0;
    for (int i = -radius; i <= radius; i++) { kernel[size_t(i + radius)] = float(std::exp(-(i * i) / (2 * sigma * sigma))); total += kernel[size_t(i + radius)]; }
    for (auto& k : kernel) k /= total;
    parallelRows(0, h, [&](int y0, int y1) {
        std::vector<float> line(size_t(w) * 4);
        for (int y = y0; y < y1; y++) {
            float* src = &data[size_t(y) * w * 4];
            std::memcpy(line.data(), src, size_t(w) * 4 * sizeof(float));
            for (int x = 0; x < w; x++) {
                float acc[4] = {0, 0, 0, 0};
                int i0 = std::max(-radius, -x), i1 = std::min(radius, w - 1 - x);
                for (int i = i0; i <= i1; i++) { float k = kernel[size_t(i + radius)]; const float* p = &line[size_t(x + i) * 4]; for (int c = 0; c < 4; c++) acc[c] += p[c] * k; }
                for (int c = 0; c < 4; c++) src[size_t(x) * 4 + c] = acc[c];
            }
        }
    });
}

void toFloat(const Image& image, std::vector<float>& out) {
    int w = image.width(), h = image.height();
    out.resize(size_t(w) * h * 4);
    parallelRows(0, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) { const uint8_t* p = image.row(y); for (int i = 0; i < w * 4; i++) out[size_t(y) * w * 4 + i] = p[i]; }
    });
}

void fromFloat(const std::vector<float>& in, Image& image) {
    int w = image.width(), h = image.height();
    parallelRows(0, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            uint8_t* p = image.row(y);
            for (int x = 0; x < w; x++) {
                const float* f = &in[(size_t(y) * w + x) * 4];
                uint8_t a = uint8_t(std::min(255.0f, std::max(0.0f, f[3] + 0.5f)));
                for (int c = 0; c < 3; c++) p[x * 4 + c] = uint8_t(std::min(float(a), std::max(0.0f, f[c] + 0.5f)));
                p[x * 4 + 3] = a;
            }
        }
    });
}

} // namespace

void gaussianBlur(Image& image, double sigma) {
    if (image.isEmpty() || !(sigma > 0)) return;
    int w = image.width(), h = image.height();
    std::vector<float> data, transposed;
    toFloat(image, data);
    if (sigma <= 6) {
        gaussianRows(data, w, h, sigma);
        transpose(data, transposed, w, h);
        gaussianRows(transposed, h, w, sigma);
    } else {
        int sizes[3];
        boxSizes(sigma, sizes);
        for (int s : sizes) boxBlurRows(data, w, h, (s - 1) / 2);
        transpose(data, transposed, w, h);
        for (int s : sizes) boxBlurRows(transposed, h, w, (s - 1) / 2);
    }
    transpose(transposed, data, h, w);
    fromFloat(data, image);
}

void motionBlur(Image& image, double distance, double angleDegrees) {
    if (image.isEmpty() || distance < 1) return;
    int w = image.width(), h = image.height();
    std::vector<float> data;
    toFloat(image, data);
    Image source = image;
    // Photoshop smears evenly along the whole distance; counterclockwise from horizontal, y down.
    double radians = angleDegrees * M_PI / 180;
    double dx = std::cos(radians), dy = -std::sin(radians);
    int samples = std::max(1, int(std::round(distance)));
    double start = -(samples - 1) / 2.0;
    parallelRows(0, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            uint8_t* out = image.row(y);
            for (int x = 0; x < w; x++, out += 4) {
                float acc[4] = {0, 0, 0, 0};
                for (int s = 0; s < samples; s++) {
                    double t = start + s;
                    double sx = x + 0.5 + t * dx - 0.5, sy = y + 0.5 + t * dy - 0.5;
                    int ix = int(std::floor(sx)), iy = int(std::floor(sy));
                    float fx = float(sx - ix), fy = float(sy - iy);
                    // Bilinear with transparent outside.
                    for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) {
                        int px = ix + i, py = iy + j;
                        float wgt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
                        if (wgt <= 0 || px < 0 || py < 0 || px >= w || py >= h) continue;
                        const float* p = &data[(size_t(py) * w + px) * 4];
                        for (int c = 0; c < 4; c++) acc[c] += p[c] * wgt;
                    }
                }
                float inv = 1.0f / samples;
                uint8_t a = uint8_t(std::min(255.0f, std::max(0.0f, acc[3] * inv + 0.5f)));
                for (int c = 0; c < 3; c++) out[c] = uint8_t(std::min(float(a), std::max(0.0f, acc[c] * inv + 0.5f)));
                out[3] = a;
            }
        }
    });
}

void applyFilter(FilterKind kind, Image& image, const FilterSettings& settings, double scale, uint32_t seed) {
    FilterSettings s = settings.normalized();
    switch (kind) {
    case FilterKind::GaussianBlur: gaussianBlur(image, s.radius * scale); break;
    case FilterKind::MotionBlur: motionBlur(image, s.distance * scale, s.angle); break;
    case FilterKind::AddNoise:
        noise_add(image.data(), size_t(image.width()), size_t(image.height()), size_t(image.stride()), float(s.amount), s.gaussian ? 1 : 0, s.monochromatic ? 1 : 0, seed);
        break;
    case FilterKind::LensCorrection: {
        Image source = image;
        lens_distort(source.data(), image.data(), size_t(image.width()), size_t(image.height()), size_t(image.stride()), s.distortion / 100 * lensStrength);
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
