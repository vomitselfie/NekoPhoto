// The transpose-free blurs against the straightforward float implementations they replaced.
#include "check.h"
#include "compositor/blur.h"
#include "compositor/parallel.h"
#include <cmath>
#include <cstring>
#include <random>
#include <vector>

using namespace compositor;

namespace {

// ---- reference: the previous implementation, kept verbatim in spirit (float, transposes) ----

void refBoxRows(std::vector<float>& data, int w, int h, int r) {
    if (r <= 0) return;
    std::vector<float> line(size_t(w) * 4);
    for (int y = 0; y < h; y++) {
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
}

void refTranspose(const std::vector<float>& in, std::vector<float>& out, int w, int h) {
    out.resize(in.size());
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) for (int c = 0; c < 4; c++) out[(size_t(x) * h + y) * 4 + c] = in[(size_t(y) * w + x) * 4 + c];
}

void refBoxSizes(double sigma, int sizes[3]) {
    double wIdeal = std::sqrt(12 * sigma * sigma / 3 + 1);
    int wl = int(std::floor(wIdeal));
    if (wl % 2 == 0) wl--;
    int wu = wl + 2;
    double mIdeal = (12 * sigma * sigma - 3 * wl * wl - 12 * wl - 9) / (-4 * wl - 4);
    int m = int(std::round(mIdeal));
    for (int i = 0; i < 3; i++) sizes[i] = i < m ? wl : wu;
}

void refGaussianRows(std::vector<float>& data, int w, int h, double sigma) {
    int radius = std::max(1, int(std::ceil(sigma * 3)));
    std::vector<float> kernel(size_t(radius) * 2 + 1);
    float total = 0;
    for (int i = -radius; i <= radius; i++) { kernel[size_t(i + radius)] = float(std::exp(-(i * i) / (2 * sigma * sigma))); total += kernel[size_t(i + radius)]; }
    for (auto& k : kernel) k /= total;
    std::vector<float> line(size_t(w) * 4);
    for (int y = 0; y < h; y++) {
        float* src = &data[size_t(y) * w * 4];
        std::memcpy(line.data(), src, size_t(w) * 4 * sizeof(float));
        for (int x = 0; x < w; x++) {
            float acc[4] = {0, 0, 0, 0};
            int i0 = std::max(-radius, -x), i1 = std::min(radius, w - 1 - x);
            for (int i = i0; i <= i1; i++) { float k = kernel[size_t(i + radius)]; const float* p = &line[size_t(x + i) * 4]; for (int c = 0; c < 4; c++) acc[c] += p[c] * k; }
            for (int c = 0; c < 4; c++) src[size_t(x) * 4 + c] = acc[c];
        }
    }
}

/// The true separable Gaussian (an FIR out to three sigma) at any sigma, or Kovesi's three-box
/// approximation when `boxes` is set (what the blur used before the recursive Gaussian).
void refGaussian(Image& image, double sigma, bool boxes = false) {
    int w = image.width(), h = image.height();
    std::vector<float> data(size_t(w) * h * 4), transposed;
    for (int y = 0; y < h; y++) { const uint8_t* p = image.row(y); for (int i = 0; i < w * 4; i++) data[size_t(y) * w * 4 + i] = p[i]; }
    if (!boxes) {
        refGaussianRows(data, w, h, sigma);
        refTranspose(data, transposed, w, h);
        refGaussianRows(transposed, h, w, sigma);
    } else {
        int sizes[3];
        refBoxSizes(sigma, sizes);
        for (int s : sizes) refBoxRows(data, w, h, (s - 1) / 2);
        refTranspose(data, transposed, w, h);
        for (int s : sizes) refBoxRows(transposed, h, w, (s - 1) / 2);
    }
    refTranspose(transposed, data, h, w);
    for (int y = 0; y < h; y++) {
        uint8_t* p = image.row(y);
        for (int x = 0; x < w; x++) {
            const float* f = &data[(size_t(y) * w + x) * 4];
            uint8_t a = uint8_t(std::min(255.0f, std::max(0.0f, f[3] + 0.5f)));
            for (int c = 0; c < 3; c++) p[x * 4 + c] = uint8_t(std::min(float(a), std::max(0.0f, f[c] + 0.5f)));
            p[x * 4 + 3] = a;
        }
    }
}

void refMotion(Image& image, double distance, double angleDegrees) {
    int w = image.width(), h = image.height();
    std::vector<float> data(size_t(w) * h * 4);
    for (int y = 0; y < h; y++) { const uint8_t* p = image.row(y); for (int i = 0; i < w * 4; i++) data[size_t(y) * w * 4 + i] = p[i]; }
    double radians = angleDegrees * M_PI / 180;
    double dx = std::cos(radians), dy = -std::sin(radians);
    int samples = std::max(1, int(std::round(distance)));
    double start = -(samples - 1) / 2.0;
    for (int y = 0; y < h; y++) {
        uint8_t* out = image.row(y);
        for (int x = 0; x < w; x++, out += 4) {
            float acc[4] = {0, 0, 0, 0};
            for (int s = 0; s < samples; s++) {
                double t = start + s;
                double sx = x + 0.5 + t * dx - 0.5, sy = y + 0.5 + t * dy - 0.5;
                int ix = int(std::floor(sx)), iy = int(std::floor(sy));
                float fx = float(sx - ix), fy = float(sy - iy);
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
}

// ---- helpers ------------------------------------------------------------------------------------

Image scene(int w = 160, int h = 120) {
    // Smooth gradients, a soft blob, a hard-edged opaque square and a transparent border.
    Image img(w, h);
    for (int y = 0; y < h; y++) {
        uint8_t* p = img.row(y);
        for (int x = 0; x < w; x++, p += 4) {
            bool border = x < 6 || y < 6 || x >= w - 6 || y >= h - 6;
            double blob = std::exp(-((x - 60) * (x - 60) + (y - 50) * (y - 50)) / 400.0);
            unsigned a = border ? 0 : (x > 100 && y > 70) ? 255 : uint8_t(std::min(255.0, 80 + 175 * blob));
            unsigned r = uint8_t(x * 255 / w), g = uint8_t(y * 255 / h), b = (x > 100 && y > 70) ? 30 : 200;
            p[0] = uint8_t((r * a + 127) / 255); p[1] = uint8_t((g * a + 127) / 255); p[2] = uint8_t((b * a + 127) / 255); p[3] = uint8_t(a);
        }
    }
    return img;
}

struct Diff { int max = 0; double mean = 0; };
Diff compare(const Image& a, const Image& b) {
    Diff d; long total = 0, n = 0;
    for (int y = 0; y < a.height(); y++) {
        const uint8_t *pa = a.row(y), *pb = b.row(y);
        for (int x = 0; x < a.width() * 4; x++) { int e = std::abs(int(pa[x]) - int(pb[x])); d.max = std::max(d.max, e); total += e; n++; }
    }
    d.mean = double(total) / double(n);
    return d;
}

bool premultipliedValid(const Image& img) {
    for (int y = 0; y < img.height(); y++) { const uint8_t* p = img.row(y); for (int x = 0; x < img.width(); x++, p += 4) for (int c = 0; c < 3; c++) if (p[c] > p[3]) return false; }
    return true;
}

} // namespace

TEST_CASE(gaussian_fir_matches_reference) {
    for (double sigma : {0.8, 2.0, 5.5}) {
        Image fast = scene(), reference = scene();
        gaussianBlur(fast, sigma);
        refGaussian(reference, sigma);
        Diff d = compare(fast, reference);
        CHECK(d.max <= 1);
        CHECK(premultipliedValid(fast));
    }
}

TEST_CASE(gaussian_recursive_matches_the_true_gaussian) {
    // Above sigma 6 the blur is Deriche's recursive fit: within a level or two of the true Gaussian at every
    // sigma, where the three-box approximation it replaces drifts by several levels.
    for (double sigma : {6.5, 12.0, 30.0, 90.0}) {
        Image fast = scene(), reference = scene();
        gaussianBlur(fast, sigma);
        refGaussian(reference, sigma);
        Diff d = compare(fast, reference);
        CHECK(d.max <= 2);
        CHECK(d.mean < 0.3);
        CHECK(premultipliedValid(fast));
    }
    Image boxes = scene(), truth = scene();
    refGaussian(boxes, 12.0, true);
    refGaussian(truth, 12.0);
    CHECK(compare(boxes, truth).max > 2);
}

TEST_CASE(gaussian_gray_matches_rgba_channel) {
    Image rgba = scene();
    GrayImage gray(rgba.width(), rgba.height());
    for (int y = 0; y < rgba.height(); y++) for (int x = 0; x < rgba.width(); x++) gray.at(x, y) = rgba.pixel(x, y)[3];
    for (double sigma : {3.0, 15.0}) {
        Image r = rgba; GrayImage g = gray;
        gaussianBlur(r, sigma);
        gaussianBlur(g, sigma);
        int worst = 0;
        for (int y = 0; y < r.height(); y++) for (int x = 0; x < r.width(); x++) worst = std::max(worst, std::abs(int(r.pixel(x, y)[3]) - int(g.at(x, y))));
        CHECK(worst <= 1);
    }
}

TEST_CASE(motion_blur_matches_reference_closely) {
    // The shear path samples once per column instead of once per pixel along the line, so it is not
    // bit-identical; it must stay close, keep the premultiplied invariant, and leave flat areas flat.
    for (double angle : {0.0, 30.0, 45.0, 70.0, 90.0, -20.0}) {
        Image fast = scene(), reference = scene();
        motionBlur(fast, 15, angle);
        refMotion(reference, 15, angle);
        Diff d = compare(fast, reference);
        CHECK(d.max <= 24);
        CHECK(d.mean < 1.5);
        CHECK(premultipliedValid(fast));
    }
    Image flat(50, 40);
    for (int y = 0; y < 40; y++) for (int x = 0; x < 50; x++) { uint8_t* p = flat.pixel(x, y); p[0] = 90; p[1] = 120; p[2] = 40; p[3] = 255; }
    motionBlur(flat, 9, 30);
    CHECK_EQ(int(flat.pixel(25, 20)[0]), 90);
    CHECK_EQ(int(flat.pixel(25, 20)[3]), 255);
}

TEST_MAIN()
