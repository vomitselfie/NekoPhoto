// The fixed-point samplers against their float formulas, the separable resample against exact cases,
// the vectorised halving against the scalar one, and the mip cache's budget.
#include "check.h"
#include "compositor/resample.h"
#include "compositor/render.h"
#include "compositor/warp.h"
#include "compositor/kernels.h"
#include <cmath>
#include <cstring>
#include <random>

using namespace compositor;

namespace {

Image busy(int w, int h, uint32_t seed = 3) {
    Image img(w, h);
    std::mt19937 rng(seed);
    for (int y = 0; y < h; y++) {
        uint8_t* p = img.row(y);
        for (int x = 0; x < w; x++, p += 4) {
            unsigned a = (x / 9) % 3 == 2 ? uint8_t(rng() % 256) : 255;
            unsigned r = uint8_t((x * 7 + y * 3) % 256), g = uint8_t((x * 2 + y * 11) % 256), b = uint8_t(rng() % 256);
            p[0] = uint8_t((r * a + 127) / 255); p[1] = uint8_t((g * a + 127) / 255); p[2] = uint8_t((b * a + 127) / 255); p[3] = uint8_t(a);
        }
    }
    return img;
}

/// A smooth premultiplied gradient (opaque), where every filter should agree closely.
Image smooth(int w, int h) {
    Image img(w, h);
    for (int y = 0; y < h; y++) {
        uint8_t* p = img.row(y);
        for (int x = 0; x < w; x++, p += 4) { p[0] = uint8_t(255 * x / (w - 1)); p[1] = uint8_t(255 * y / (h - 1)); p[2] = uint8_t((p[0] + p[1]) / 2); p[3] = 255; }
    }
    return img;
}

void floatBilinear(const Image& image, double x, double y, float out[4]) {
    int w = image.width(), h = image.height();
    x -= 0.5; y -= 0.5;
    x = std::clamp(x, 0.0, double(w - 1)); y = std::clamp(y, 0.0, double(h - 1));
    int x0 = int(x), y0 = int(y), x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    float fx = float(x - x0), fy = float(y - y0);
    const uint8_t *p00 = image.pixel(x0, y0), *p10 = image.pixel(x1, y0), *p01 = image.pixel(x0, y1), *p11 = image.pixel(x1, y1);
    for (int c = 0; c < 4; c++) out[c] = p00[c] * (1 - fx) * (1 - fy) + p10[c] * fx * (1 - fy) + p01[c] * (1 - fx) * fy + p11[c] * fx * fy;
}

std::shared_ptr<Image> halveScalar(const Image& image) {
    int w = (image.width() + 1) / 2, h = (image.height() + 1) / 2;
    auto out = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; y++) {
        int y0 = std::min(2 * y, image.height() - 1), y1 = std::min(2 * y + 1, image.height() - 1);
        for (int x = 0; x < w; x++) {
            int x0 = std::min(2 * x, image.width() - 1), x1 = std::min(2 * x + 1, image.width() - 1);
            for (int c = 0; c < 4; c++) out->pixel(x, y)[c] = uint8_t((int(image.pixel(x0, y0)[c]) + image.pixel(x1, y0)[c] + image.pixel(x0, y1)[c] + image.pixel(x1, y1)[c] + 2) / 4);
        }
    }
    return out;
}

int worstDifference(const Image& a, const Image& b, int inset = 0) {
    int worst = 0;
    for (int y = inset; y < a.height() - inset; y++)
        for (int x = inset; x < a.width() - inset; x++)
            for (int c = 0; c < 4; c++) worst = std::max(worst, std::abs(int(a.pixel(x, y)[c]) - int(b.pixel(x, y)[c])));
    return worst;
}

} // namespace

TEST_CASE(fixed_point_bilinear_matches_float) {
    Image img = busy(41, 29);
    std::mt19937 rng(5);
    for (int i = 0; i < 4000; i++) {
        double x = (rng() % 4600) / 100.0 - 2, y = (rng() % 3300) / 100.0 - 2;
        uint8_t ours[4]; float ref[4];
        sampleBilinear(img, x, y, ours);
        floatBilinear(img, x, y, ref);
        for (int c = 0; c < 4; c++) CHECK(std::fabs(ours[c] - ref[c]) <= 1.01f);
    }
    GrayImage mask(41, 29);
    for (int y = 0; y < 29; y++) for (int x = 0; x < 41; x++) mask.at(x, y) = uint8_t((x * 13 + y * 7) % 256);
    for (int i = 0; i < 2000; i++) {
        double x = (rng() % 4600) / 100.0 - 2, y = (rng() % 3300) / 100.0 - 2;
        double fx = std::clamp(x - 0.5, 0.0, 40.0), fy = std::clamp(y - 0.5, 0.0, 28.0);
        int x0 = int(fx), y0 = int(fy), x1 = std::min(x0 + 1, 40), y1 = std::min(y0 + 1, 28);
        double tx = fx - x0, ty = fy - y0;
        double ref = mask.at(x0, y0) * (1 - tx) * (1 - ty) + mask.at(x1, y0) * tx * (1 - ty) + mask.at(x0, y1) * (1 - tx) * ty + mask.at(x1, y1) * tx * ty;
        CHECK(std::fabs(sampleGrayBilinear(mask, x, y) - ref) <= 1.01);
    }
}

TEST_CASE(bicubic_is_exact_on_ramps_and_constants) {
    // Catmull-Rom reproduces linear functions, so a ramp samples to the ramp and a constant to itself.
    Image ramp(64, 8);
    for (int y = 0; y < 8; y++) for (int x = 0; x < 64; x++) { uint8_t* p = ramp.pixel(x, y); p[0] = uint8_t(x * 4); p[1] = 200; p[2] = uint8_t(100 + y * 10); p[3] = 255; }
    for (double x = 2; x < 62; x += 0.37) {
        uint8_t s[4];
        sampleBicubic(ramp, x, 4.5, s);
        CHECK(std::fabs(s[0] - (x - 0.5) * 4) <= 1.01);
        CHECK_EQ(int(s[1]), 200);
        CHECK_EQ(int(s[3]), 255);
    }
    // Half-transparent constant: colour stays within alpha.
    Image half(16, 16);
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) { uint8_t* p = half.pixel(x, y); p[0] = 100; p[1] = 50; p[2] = 0; p[3] = 100; }
    uint8_t s[4];
    sampleBicubic(half, 7.3, 8.9, s);
    CHECK_EQ(int(s[0]), 100); CHECK_EQ(int(s[1]), 50); CHECK_EQ(int(s[3]), 100);
    const int16_t* w = catmullRomWeights(128);
    CHECK_EQ(int(w[0] + w[1] + w[2] + w[3]), 256);
    CHECK(w[0] < 0);
}

TEST_CASE(separable_resample_at_unit_scale_is_the_identity) {
    Image img = busy(37, 23);
    for (ResampleFilter f : {ResampleFilter::Triangle, ResampleFilter::CatmullRom, ResampleFilter::Lanczos3}) {
        auto same = resampleAxisAligned(img, 37, 23, 0.5, 1, 0.5, 1, f);
        CHECK_EQ(worstDifference(img, *same), 0);
    }
    // A whole-pixel translation shifts exactly, with transparent pixels beyond the source.
    auto shifted = resampleAxisAligned(img, 37, 23, 3.5, 1, 0.5, 1, ResampleFilter::Lanczos3);
    CHECK_EQ(std::memcmp(shifted->pixel(0, 5), img.pixel(3, 5), 4), 0);
    CHECK_EQ(int(shifted->pixel(36, 5)[3]), 0);
}

TEST_CASE(separable_resample_reduces_and_magnifies_smoothly) {
    Image img = smooth(200, 120);
    // Halving with the triangle is the box average, up to rounding.
    auto half = resampleAxisAligned(img, 100, 60, 1, 2, 1, 2, ResampleFilter::Triangle);
    auto box = halveImage(img);
    CHECK(worstDifference(*half, *box, 1) <= 2);
    // Lanczos and the triangle agree on a smooth gradient, and both keep the corners' colours.
    auto lanczos = resampleAxisAligned(img, 100, 60, 1, 2, 1, 2, ResampleFilter::Lanczos3);
    CHECK(worstDifference(*half, *lanczos, 3) <= 3);
    // A large reduction of a constant is that constant, edge pixels included (their centres sit half an output pixel in).
    Image flat(300, 200);
    flat.fill(60, 120, 180, 255);
    auto tiny = resampleAxisAligned(flat, 30, 20, 5, 10, 5, 10, ResampleFilter::Lanczos3);
    CHECK_EQ(int(tiny->pixel(15, 10)[0]), 60); CHECK_EQ(int(tiny->pixel(15, 10)[3]), 255);
    CHECK_EQ(int(tiny->pixel(0, 0)[3]), 255);
    // Half a pixel further out the ramp starts: an output grid straddling the edge gets partial alpha there.
    auto straddle = resampleAxisAligned(flat, 31, 20, 0, 10, 5, 10, ResampleFilter::Lanczos3);
    CHECK_EQ(int(straddle->pixel(0, 10)[3]), 128);
    CHECK_EQ(int(straddle->pixel(1, 10)[3]), 255);
    // Magnification 3x of a checkerboard: Lanczos ringing is clamped and the plateau values survive.
    Image checker(20, 20);
    for (int y = 0; y < 20; y++) for (int x = 0; x < 20; x++) { uint8_t v = ((x / 5) + (y / 5)) % 2 ? 220 : 30; uint8_t* p = checker.pixel(x, y); p[0] = p[1] = p[2] = v; p[3] = 255; }
    auto big = resampleAxisAligned(checker, 60, 60, 0.5 / 3, 1.0 / 3, 0.5 / 3, 1.0 / 3, ResampleFilter::Lanczos3);
    CHECK_EQ(int(big->pixel(7, 7)[0]), 30);
    CHECK_EQ(int(big->pixel(22, 7)[0]), 220);
    CHECK_EQ(int(big->pixel(22, 7)[3]), 255);
    // Masks carry their outside value beyond the edge.
    GrayImage mask(40, 40, 200);
    auto grown = resampleAxisAligned(mask, 60, 60, -9.5, 1, -9.5, 1, ResampleFilter::Triangle, uint8_t(30));
    CHECK_EQ(int(grown->at(2, 2)), 30);
    CHECK_EQ(int(grown->at(30, 30)), 200);
}

TEST_CASE(axis_aligned_warp_uses_the_separable_path_consistently) {
    // Scaling a layer through warpImage (Image Size) and resampleLayer must agree with a direct separable resample.
    auto img = std::make_shared<Image>(smooth(120, 80));
    LayerTransform t(Point(10, 20), Size(120, 80));
    Corners scaled = {Point{10, 20}, Point{70, 20}, Point{70, 60}, Point{10, 60}};
    auto warped = warpImage(img, t, scaled, 0);
    REQUIRE(warped.has_value());
    CHECK_EQ(warped->image->width(), 60);
    CHECK_EQ(warped->image->height(), 40);
    auto direct = resampleAxisAligned(*img, 60, 40, 1, 2, 1, 2, ResampleFilter::Lanczos3);
    CHECK_EQ(worstDifference(*warped->image, *direct), 0);
    // The same placement rasterised at half the pixel count reduces 2x through the same path.
    auto resampled = resampleLayer(img, t, t, 60, 40);
    CHECK_EQ(worstDifference(*resampled, *direct), 0);
    // Smooth sampling takes the triangle; a rotated layer still goes through the projective path.
    LayerTransform smoothT = t;
    smoothT.sampling = Sampling::Smooth;
    auto tri = warpImage(img, smoothT, scaled, 0);
    CHECK_EQ(worstDifference(*tri->image, *resampleAxisAligned(*img, 60, 40, 1, 2, 1, 2, ResampleFilter::Triangle)), 0);
    Corners tilted = {Point{12, 20}, Point{70, 22}, Point{68, 60}, Point{10, 58}};
    auto rotated = warpImage(img, t, tilted, 0);
    REQUIRE(rotated.has_value());
    CHECK(worstDifference(*rotated->image, *direct, 4) > 0);
}

TEST_CASE(vectorised_halving_matches_scalar) {
    for (int w : {1, 2, 3, 7, 8, 9, 31, 64, 65}) for (int h : {1, 2, 5, 8, 17}) {
        Image img = busy(w, h, uint32_t(w * 100 + h));
        auto fast = halveImage(img);
        auto slow = halveScalar(img);
        CHECK_EQ(fast->width(), slow->width());
        CHECK_EQ(worstDifference(*fast, *slow), 0);
        GrayImage g(w, h);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) g.at(x, y) = uint8_t((x * 37 + y * 91 + w) % 256);
        auto fg = halveGray(g);
        for (int y = 0; y < fg->height(); y++) for (int x = 0; x < fg->width(); x++) {
            int x0 = std::min(2 * x, w - 1), x1 = std::min(2 * x + 1, w - 1), y0 = std::min(2 * y, h - 1), y1 = std::min(2 * y + 1, h - 1);
            CHECK_EQ(int(fg->at(x, y)), (int(g.at(x0, y0)) + g.at(x1, y0) + g.at(x0, y1) + g.at(x1, y1) + 2) / 4);
        }
    }
}

TEST_CASE(mip_cache_keeps_a_budget_and_no_sources) {
    MipCache& cache = MipCache::shared();
    cache.clear();
    size_t previous = cache.budget();
    auto a = std::make_shared<Image>(busy(512, 512));
    auto b = std::make_shared<Image>(busy(512, 512, 9));
    std::weak_ptr<const Image> watch = a;
    ImagePtr a1 = cache.level(a, 1);
    CHECK_EQ(a1->width(), 256);
    // The cache holds only the reductions; dropping the source frees it.
    CHECK_EQ(long(a.use_count()), 1L);
    size_t oneLevel = cache.bytesUsed();
    CHECK(oneLevel > 0);
    cache.setBudget(oneLevel + oneLevel / 2);
    (void)cache.level(b, 1);
    CHECK(cache.bytesUsed() <= oneLevel + oneLevel / 2);
    // The rounded level choice sits nearest 1x.
    CHECK_EQ(MipCache::levelFor(0.3), 1);
    CHECK_EQ(MipCache::levelFor(0.3, true), 2);
    CHECK_EQ(MipCache::levelFor(0.6, true), 1);
    CHECK_EQ(MipCache::levelFor(0.8, true), 0);
    a.reset(); a1.reset();
    (void)cache.level(b, 2);
    CHECK(watch.expired());
    cache.setBudget(previous);
    cache.clear();
}

TEST_CASE(lens_bicubic_matches_bilinear_on_smooth_content) {
    Image img = smooth(120, 90);
    Image bilinear(120, 90), bicubic(120, 90);
    kernels::lensDistort(img, bilinear, 0.2, false);
    kernels::lensDistort(img, bicubic, 0.2, true);
    CHECK(worstDifference(bilinear, bicubic, 4) <= 2);
    CHECK_EQ(int(bicubic.pixel(60, 45)[3]), 255);
}

TEST_MAIN()
