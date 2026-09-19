// Each optimised kernel against the portable C reference it replaces.
#include "check.h"
#include "compositor/kernels.h"
#include "compositor/adjustments.h"
#include "compositor/wand.h"
extern "C" {
#include "AdjustPixels.h"
#include "LensPixels.h"
#include "LevelsPixels.h"
#include "NoisePixels.h"
#include "WandPixels.h"
}
#include <cmath>
#include <cstring>
#include <random>

using namespace compositor;

namespace {

/// A busy premultiplied test image: gradients, an alpha ramp, some fully transparent and opaque runs.
Image testImage(int w = 97, int h = 61, uint32_t seed = 7) {
    Image img(w, h);
    std::mt19937 rng(seed);
    for (int y = 0; y < h; y++) {
        uint8_t* p = img.row(y);
        for (int x = 0; x < w; x++, p += 4) {
            unsigned a = (x / 12) % 3 == 0 ? 255 : (x / 12) % 3 == 1 ? uint8_t(rng() % 256) : 0;
            unsigned r = uint8_t((x * 5 + y * 3) % 256), g = uint8_t((x * 2 + y * 7) % 256), b = uint8_t(rng() % 256);
            p[0] = uint8_t((r * a + 127) / 255); p[1] = uint8_t((g * a + 127) / 255); p[2] = uint8_t((b * a + 127) / 255); p[3] = uint8_t(a);
        }
    }
    return img;
}

/// The previous Hue/Saturation sampler: a float 33-point cube read trilinearly.
void hueSaturationTrilinear(Image& image, const HueSaturationSettings& settings) {
    constexpr int dim = 33;
    std::vector<float> cube(size_t(dim) * dim * dim * 3);
    for (int bi = 0; bi < dim; bi++)
        for (int gi = 0; gi < dim; gi++)
            for (int ri = 0; ri < dim; ri++) {
                double r = ri / double(dim - 1), g = gi / double(dim - 1), b = bi / double(dim - 1);
                settings.adjust(r, g, b);
                size_t index = (size_t(bi) * dim * dim + size_t(gi) * dim + size_t(ri)) * 3;
                cube[index] = float(r); cube[index + 1] = float(g); cube[index + 2] = float(b);
            }
    for (int y = 0; y < image.height(); y++) {
        uint8_t* p = image.row(y);
        for (int x = 0; x < image.width(); x++, p += 4) {
            unsigned a = p[3];
            if (!a) continue;
            float in[3] = {std::min(1.0f, p[0] / float(a)), std::min(1.0f, p[1] / float(a)), std::min(1.0f, p[2] / float(a))};
            float fr = in[0] * (dim - 1), fg = in[1] * (dim - 1), fb = in[2] * (dim - 1);
            int r0 = std::min(dim - 2, int(fr)), g0 = std::min(dim - 2, int(fg)), b0 = std::min(dim - 2, int(fb));
            float tr = fr - r0, tg = fg - g0, tb = fb - b0, out[3] = {0, 0, 0};
            for (int db = 0; db < 2; db++) for (int dg = 0; dg < 2; dg++) for (int dr = 0; dr < 2; dr++) {
                float w = (dr ? tr : 1 - tr) * (dg ? tg : 1 - tg) * (db ? tb : 1 - tb);
                const float* c = &cube[(size_t(b0 + db) * dim * dim + size_t(g0 + dg) * dim + size_t(r0 + dr)) * 3];
                for (int k = 0; k < 3; k++) out[k] += c[k] * w;
            }
            for (int c = 0; c < 3; c++) p[c] = uint8_t(std::min(float(a), std::max(0.0f, out[c] * a + 0.5f)));
        }
    }
}

int maxDifference(const Image& a, const Image& b, bool opaqueOnly) {
    int worst = 0;
    for (int y = 0; y < a.height(); y++) {
        const uint8_t *pa = a.row(y), *pb = b.row(y);
        for (int x = 0; x < a.width(); x++, pa += 4, pb += 4) {
            if (opaqueOnly && pa[3] != 255) continue;
            for (int c = 0; c < 4; c++) worst = std::max(worst, std::abs(int(pa[c]) - int(pb[c])));
        }
    }
    return worst;
}

} // namespace

TEST_CASE(channel_tables_match_levels_apply) {
    std::vector<float> tables(3 * 256);
    kernels::ChannelTables lut;
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 256; i++) {
            float v = std::pow(i / 255.0f, c == 0 ? 0.6f : c == 1 ? 1.0f : 1.8f);   // a curve per channel
            tables[size_t(c) * 256 + size_t(i)] = v;
            lut.lut[c][i] = uint8_t(std::lround(v * 255));
        }
    Image reference = testImage(), fast = testImage();
    levels_apply(reference.data(), size_t(reference.width()) * size_t(reference.height()), tables.data());
    // levels_apply assumes a packed stride; the test image is packed.
    kernels::applyChannelTables(fast, lut);
    CHECK_EQ(maxDifference(reference, fast, true), 0);
    CHECK(maxDifference(reference, fast, false) <= 2);   // byte-quantised table entries
}

TEST_CASE(add_noise_matches_reference) {
    for (int mode = 0; mode < 4; mode++) {
        bool gaussian = mode & 1, mono = mode & 2;
        Image reference = testImage(), fast = testImage();
        noise_add(reference.data(), size_t(reference.width()), size_t(reference.height()), size_t(reference.stride()), 40, gaussian, mono, 0x1234u);
        kernels::addNoise(fast, 40, gaussian, mono, 0x1234u);
        CHECK_EQ(maxDifference(reference, fast, false), 0);
    }
}

TEST_CASE(lens_distort_matches_reference) {
    Image source = testImage(120, 90);
    Image reference(120, 90), fast(120, 90);
    for (double k : {-0.35, 0.2}) {
        lens_distort(source.data(), reference.data(), 120, 90, size_t(source.stride()), k);
        kernels::lensDistort(source, fast, k);
        CHECK_EQ(maxDifference(reference, fast, false), 0);
    }
}

TEST_CASE(gradient_map_matches_reference) {
    std::vector<uint8_t> table(256 * 3);
    for (int i = 0; i < 256; i++) { table[size_t(i) * 3] = uint8_t(i); table[size_t(i) * 3 + 1] = uint8_t(255 - i); table[size_t(i) * 3 + 2] = uint8_t(std::min(255, i * 3 / 2)); }
    Image reference = testImage(), fast = testImage();
    for (int y = 0; y < reference.height(); y++) adjust_gradient_map(reference.row(y), size_t(reference.width()), 1, size_t(reference.stride()), table.data());
    kernels::gradientMap(fast, table.data());
    CHECK_EQ(maxDifference(reference, fast, true), 0);
    // At partial alpha the luma index may land one step away; this table steps by at most 2 per index.
    CHECK(maxDifference(reference, fast, false) <= 3);
}

TEST_CASE(invert_matches_scalar) {
    Image reference = testImage(101, 7), fast = testImage(101, 7);   // 101: a tail after the 4-pixel vectors
    for (int y = 0; y < reference.height(); y++) {
        uint8_t* p = reference.row(y);
        for (int x = 0; x < reference.width(); x++, p += 4) for (int c = 0; c < 3; c++) p[c] = uint8_t(p[3] - p[c]);
    }
    kernels::invertColors(fast);
    CHECK_EQ(maxDifference(reference, fast, false), 0);
}

TEST_MAIN()

TEST_CASE(hue_saturation_cube_matches_the_trilinear_sampler) {
    HueSaturationSettings settings;
    settings.adjustments[0] = {40, 25, -10};
    settings.adjustments[1] = {-30, 60, 0};
    Image reference = testImage(), ours = testImage();
    hueSaturationTrilinear(reference, settings);
    applyHueSaturation(ours, settings);
    // Tetrahedral and trilinear reads of the same cube agree except near the creases, where they differ by a level or two.
    CHECK(maxDifference(reference, ours, true) <= 3);
    CHECK(maxDifference(reference, ours, false) <= 4);
}

TEST_CASE(hue_saturation_keeps_greys_grey) {
    HueSaturationSettings settings;
    settings.adjustments[0] = {120, 40, 0};
    Image img(256, 2);
    for (int x = 0; x < 256; x++) { uint8_t* p = img.pixel(x, 0); p[0] = p[1] = p[2] = uint8_t(x); p[3] = 255; uint8_t* q = img.pixel(x, 1); q[0] = q[1] = q[2] = uint8_t(x / 2); q[3] = 128; }
    applyHueSaturation(img, settings);
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 256; x++) { const uint8_t* p = img.pixel(x, y); CHECK_EQ(int(p[0]), int(p[1])); CHECK_EQ(int(p[1]), int(p[2])); }
    // And a plain hue shift leaves the grey levels where they were.
    HueSaturationSettings shift;
    shift.adjustments[0] = {90, 0, 0};
    Image again(256, 1);
    for (int x = 0; x < 256; x++) { uint8_t* p = again.pixel(x, 0); p[0] = p[1] = p[2] = uint8_t(x); p[3] = 255; }
    applyHueSaturation(again, shift);
    for (int x = 0; x < 256; x++) CHECK_EQ(int(again.pixel(x, 0)[0]), x);
}

TEST_CASE(colorize_table_matches_the_exact_adjustment) {
    HueSaturationSettings settings = HueSaturationSettings::colorizeStart();
    settings.current() = {200, 50, 10};
    Image img = testImage();
    Image ours = img;
    applyHueSaturation(ours, settings);
    for (int y = 0; y < img.height(); y++)
        for (int x = 0; x < img.width(); x++) {
            const uint8_t* p = img.pixel(x, y);
            if (p[3] != 255) continue;
            double r = p[0] / 255.0, g = p[1] / 255.0, b = p[2] / 255.0;
            settings.adjust(r, g, b);
            const uint8_t* o = ours.pixel(x, y);
            CHECK(std::abs(int(o[0]) - int(std::lround(r * 255))) <= 1);
            CHECK(std::abs(int(o[1]) - int(std::lround(g * 255))) <= 1);
            CHECK(std::abs(int(o[2]) - int(std::lround(b * 255))) <= 1);
        }
}

TEST_CASE(photoshop_saturation_curve_saturates_fully_at_plus_100) {
    HueSaturationSettings settings;
    settings.photoshopSaturation = true;
    settings.adjustments[0] = {0, 100, 0};
    double r = 0.5, g = 0.3, b = 0.3;
    settings.adjust(r, g, b);
    // Lightness (max + min) / 2 = 0.4 is kept and the colour reaches full HSL saturation: the range 1 - |2L - 1| = 0.8.
    CHECK_NEAR(r, 0.8, 1e-9);
    CHECK_NEAR(g, 0.0, 1e-9);
    CHECK_NEAR((r + b) / 2, 0.4, 1e-9);
    settings.adjustments[0] = {0, -100, 0};
    r = 0.5; g = 0.3; b = 0.3;
    settings.adjust(r, g, b);
    CHECK_NEAR(r, 0.4, 1e-9);
    CHECK_NEAR(g, 0.4, 1e-9);
    // Half way: the saturation is scaled by 1 / (1 - 0.5) = 2, capped at full.
    settings.adjustments[0] = {0, 50, 0};
    r = 0.45; g = 0.35; b = 0.35;
    settings.adjust(r, g, b);
    CHECK_NEAR(r, 0.5, 1e-9);
    CHECK_NEAR(g, 0.3, 1e-9);
    AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::HueSaturation);
    s.hsv = settings;
    std::string json = s.toJson();
    CHECK(json.find("\"saturationCurve\":\"photoshop\"") != std::string::npos);
    AdjustmentSettings back;
    REQUIRE(AdjustmentSettings::parse(json, back));
    CHECK(back.hsv.photoshopSaturation);
    CHECK(!AdjustmentSettings::defaults(AdjustmentKind::HueSaturation).hsv.photoshopSaturation);
}

TEST_CASE(wand_matches_reference) {
    // Blobs of near-uniform colour over a gradient, with an alpha ramp, so contiguity and tolerance both matter.
    const int w = 120, h = 90;
    Image img(w, h);
    std::mt19937 rng(11);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t* p = img.pixel(x, y);
            bool blob = (x - 30) * (x - 30) + (y - 40) * (y - 40) < 400 || (x > 70 && x < 100 && y > 20 && y < 60);
            unsigned a = x < 20 ? uint8_t(x * 12) : 255;
            unsigned r = blob ? 200 + rng() % 9 : uint8_t(x * 2), g = blob ? 60 + rng() % 9 : uint8_t(y * 2), b = blob ? 90 : uint8_t((x + y) % 256);
            p[0] = uint8_t((r * a + 127) / 255); p[1] = uint8_t((g * a + 127) / 255); p[2] = uint8_t((b * a + 127) / 255); p[3] = uint8_t(a);
        }
    GrayImage ours(w, h), reference(w, h);
    const int seeds[][2] = {{30, 40}, {85, 40}, {5, 5}, {60, 80}, {119, 89}, {0, 0}};
    for (auto& seed : seeds)
        for (int radius = 0; radius <= 2; radius++)
            for (int tolerance : {0, 8, 32, 120})
                for (bool contiguous : {true, false}) {
                    long a = wandMask(img, seed[0], seed[1], radius, tolerance, contiguous, ours);
                    long b = wand_mask(img.data(), size_t(w), size_t(h), size_t(img.stride()), size_t(seed[0]), size_t(seed[1]), size_t(radius), tolerance, contiguous ? 1 : 0, reference.data());
                    CHECK_EQ(a, b);
                    CHECK_EQ(std::memcmp(ours.data(), reference.data(), size_t(w) * h), 0);
                }
    CHECK(wandMask(img, 30, 40, 0, 8, true, ours) > 1000);
}
