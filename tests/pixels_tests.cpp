// Exercises the existing C pixel routines on Linux with synthetic images:
// solid colours, gradients, checkerboards, alpha ramps and masked regions.
#include "check.h"

extern "C" {
#include "AdjustPixels.h"
#include "BrushPixels.h"
#include "ContentFill.h"
#include "HealPixels.h"
#include "LensPixels.h"
#include "LevelsPixels.h"
#include "NoisePixels.h"
#include "WandPixels.h"
}

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

struct Rgba {
    size_t width, height, stride;
    std::vector<uint8_t> bytes;
    Rgba(size_t w, size_t h, size_t pad = 0) : width(w), height(h), stride(w * 4 + pad), bytes(stride * h, 0) {}
    uint8_t* px(size_t x, size_t y) { return &bytes[y * stride + x * 4]; }
    const uint8_t* px(size_t x, size_t y) const { return &bytes[y * stride + x * 4]; }
    void fill(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        for (size_t y = 0; y < height; y++)
            for (size_t x = 0; x < width; x++) { auto* p = px(x, y); p[0] = r; p[1] = g; p[2] = b; p[3] = a; }
    }
};

} // namespace

TEST_CASE(alpha_bounds_empty_and_region) {
    Rgba image(16, 12, 8);
    size_t bounds[4] = {9, 9, 9, 9};
    brush_alpha_bounds(image.bytes.data(), image.width, image.height, image.stride, bounds);
    CHECK_EQ(bounds[0], size_t(0)); CHECK_EQ(bounds[1], size_t(0)); CHECK_EQ(bounds[2], size_t(0)); CHECK_EQ(bounds[3], size_t(0));
    // Paint a 3x2 block at (5,4).
    for (size_t y = 4; y < 6; y++) for (size_t x = 5; x < 8; x++) image.px(x, y)[3] = 1;
    brush_alpha_bounds(image.bytes.data(), image.width, image.height, image.stride, bounds);
    CHECK_EQ(bounds[0], size_t(5)); CHECK_EQ(bounds[1], size_t(4)); CHECK_EQ(bounds[2], size_t(8)); CHECK_EQ(bounds[3], size_t(6));
}

TEST_CASE(extract_unpremultiply_restore_round_trip) {
    Rgba image(8, 8);
    // Alpha ramp across x, premultiplied gray ramp across y.
    for (size_t y = 0; y < 8; y++)
        for (size_t x = 0; x < 8; x++) {
            uint8_t a = uint8_t(x * 255 / 7);
            uint8_t c = uint8_t((y * 255 / 7) * a / 255);
            auto* p = image.px(x, y); p[0] = c; p[1] = c; p[2] = c; p[3] = a;
        }
    Rgba original = image;
    std::vector<uint8_t> alpha(8 * 8);
    layer_extract_alpha(image.bytes.data(), image.stride, alpha.data(), 8, 8, 8);
    for (size_t y = 0; y < 8; y++) for (size_t x = 0; x < 8; x++) CHECK_EQ(alpha[y * 8 + x], original.px(x, y)[3]);
    layer_unpremultiply_opaque(image.bytes.data(), image.stride, 8, 8);
    for (size_t y = 0; y < 8; y++) for (size_t x = 1; x < 8; x++) CHECK_EQ(image.px(x, y)[3], uint8_t(255));
    layer_restore_alpha(image.bytes.data(), image.stride, alpha.data(), 8, 8, 8);
    for (size_t y = 0; y < 8; y++)
        for (size_t x = 0; x < 8; x++) {
            CHECK_EQ(image.px(x, y)[3], original.px(x, y)[3]);
            // Colour survives within rounding of one premultiply round trip.
            CHECK(std::abs(int(image.px(x, y)[0]) - int(original.px(x, y)[0])) <= 2);
        }
}

TEST_CASE(clamp_premultiplied_caps_colour_at_alpha) {
    uint8_t px[8] = {200, 100, 50, 60, 10, 20, 30, 255};
    rgba_clamp_premultiplied(px, 2);
    CHECK_EQ(px[0], uint8_t(60)); CHECK_EQ(px[1], uint8_t(60)); CHECK_EQ(px[2], uint8_t(50)); CHECK_EQ(px[3], uint8_t(60));
    CHECK_EQ(px[4], uint8_t(10)); CHECK_EQ(px[7], uint8_t(255));
}

TEST_CASE(levels_tables_and_histogram) {
    // Tables map an input level to an output in 0-1 (scaled by alpha on the way out).
    // Identity tables leave pixels alone; an inverting table flips them.
    std::vector<float> tables(3 * 256);
    for (int c = 0; c < 3; c++) for (int i = 0; i < 256; i++) tables[c * 256 + i] = float(i) / 255.0f;
    uint8_t px[8] = {10, 20, 30, 255, 100, 150, 200, 255};
    levels_apply(px, 2, tables.data());
    CHECK_EQ(px[0], uint8_t(10)); CHECK_EQ(px[6], uint8_t(200));
    for (int c = 0; c < 3; c++) for (int i = 0; i < 256; i++) tables[c * 256 + i] = float(255 - i) / 255.0f;
    levels_apply(px, 2, tables.data());
    CHECK_EQ(px[0], uint8_t(245)); CHECK_EQ(px[5], uint8_t(105));

    Rgba image(4, 1);
    image.fill(0, 0, 0, 255);
    image.px(1, 0)[0] = 255; image.px(1, 0)[1] = 255; image.px(1, 0)[2] = 255;
    std::vector<double> bins(4 * 256, 0);
    uint8_t coverage[4] = {255, 255, 0, 255};
    levels_histogram(image.bytes.data(), coverage, 4, bins.data());
    // Whatever the exact layout, the histogram must see mass at 0 and 255 only.
    double total = 0, ends = 0;
    for (int i = 0; i < 4 * 256; i++) { total += bins[i]; if (i % 256 == 0 || i % 256 == 255) ends += bins[i]; }
    CHECK(total > 0);
    CHECK_NEAR(ends, total, 1e-9);
}

TEST_CASE(gradient_map_maps_luminance) {
    std::vector<uint8_t> table(256 * 3);
    for (int i = 0; i < 256; i++) { table[i * 3] = uint8_t(i); table[i * 3 + 1] = 0; table[i * 3 + 2] = uint8_t(255 - i); }
    Rgba image(3, 1);
    image.fill(0, 0, 0, 255);
    auto* white = image.px(1, 0); white[0] = white[1] = white[2] = 255;
    image.px(2, 0)[3] = 0; // transparent stays untouched
    adjust_gradient_map(image.bytes.data(), 3, 1, image.stride, table.data());
    CHECK_EQ(image.px(0, 0)[0], uint8_t(0)); CHECK_EQ(image.px(0, 0)[2], uint8_t(255));
    CHECK_EQ(image.px(1, 0)[0], uint8_t(255)); CHECK_EQ(image.px(1, 0)[2], uint8_t(0));
    CHECK_EQ(image.px(2, 0)[3], uint8_t(0)); CHECK_EQ(image.px(2, 0)[0], uint8_t(0));
}

TEST_CASE(grain_and_noise_are_deterministic_by_seed) {
    Rgba a(32, 32), b(32, 32), c(32, 32);
    a.fill(128, 128, 128, 255); b.fill(128, 128, 128, 255); c.fill(128, 128, 128, 255);
    adjust_grain(a.bytes.data(), 32, 32, a.stride, 50, 1.5, 50, 7, 0, 0, 1);
    adjust_grain(b.bytes.data(), 32, 32, b.stride, 50, 1.5, 50, 7, 0, 0, 1);
    adjust_grain(c.bytes.data(), 32, 32, c.stride, 50, 1.5, 50, 8, 0, 0, 1);
    CHECK(a.bytes == b.bytes);
    CHECK(a.bytes != c.bytes);
    bool changed = false;
    for (size_t i = 0; i < a.bytes.size(); i += 4) if (a.bytes[i] != 128) changed = true;
    CHECK(changed);

    Rgba n1(16, 16), n2(16, 16);
    n1.fill(100, 100, 100, 255); n2.fill(100, 100, 100, 255);
    n1.px(0, 0)[3] = 0; n1.px(0, 0)[0] = 0; n2.px(0, 0)[3] = 0; n2.px(0, 0)[0] = 0;
    noise_add(n1.bytes.data(), 16, 16, n1.stride, 20, 0, 1, 3);
    noise_add(n2.bytes.data(), 16, 16, n2.stride, 20, 0, 1, 3);
    CHECK(n1.bytes == n2.bytes);
    CHECK_EQ(n1.px(0, 0)[0], uint8_t(0)); // transparent pixels untouched
    // Monochromatic: all three channels move together.
    for (size_t y = 0; y < 16; y++) for (size_t x = 1; x < 16; x++) {
        CHECK_EQ(n1.px(x, y)[0], n1.px(x, y)[1]);
        CHECK_EQ(n1.px(x, y)[3], uint8_t(255));
    }
}

TEST_CASE(lens_distort_identity_copies) {
    Rgba src(24, 16), dst(24, 16);
    for (size_t y = 0; y < 16; y++) for (size_t x = 0; x < 24; x++) {
        auto* p = src.px(x, y); p[0] = uint8_t(x * 10); p[1] = uint8_t(y * 15); p[2] = 7; p[3] = 255;
    }
    lens_distort(src.bytes.data(), dst.bytes.data(), 24, 16, src.stride, 0);
    CHECK(src.bytes == dst.bytes);
    lens_distort(src.bytes.data(), dst.bytes.data(), 24, 16, src.stride, -0.5);
    CHECK(src.bytes != dst.bytes);
    // Pincushion straightening pushes corners out of the source: corners become transparent.
    CHECK_EQ(dst.px(0, 0)[3], uint8_t(0));
}

TEST_CASE(wand_mask_contiguous_and_global) {
    // Checkerboard of 2x2 red/blue cells on 8x8, plus a distinct green pixel.
    Rgba image(8, 8);
    for (size_t y = 0; y < 8; y++) for (size_t x = 0; x < 8; x++) {
        bool red = ((x / 2) + (y / 2)) % 2 == 0;
        auto* p = image.px(x, y); p[0] = red ? 255 : 0; p[1] = 0; p[2] = red ? 0 : 255; p[3] = 255;
    }
    std::vector<uint8_t> mask(64);
    long count = wand_mask(image.bytes.data(), 8, 8, image.stride, 0, 0, 0, 10, 1, mask.data());
    CHECK_EQ(count, 4L); // just the seed's 2x2 red cell when contiguous
    count = wand_mask(image.bytes.data(), 8, 8, image.stride, 0, 0, 0, 10, 0, mask.data());
    CHECK_EQ(count, 32L); // every red pixel when not contiguous
    CHECK_EQ(mask[0], uint8_t(255)); CHECK_EQ(mask[2], uint8_t(0));

    int32_t* points = nullptr; int32_t* loops = nullptr; size_t pointCount = 0, loopCount = 0;
    // Trace a single 2x2 square.
    std::vector<uint8_t> square(64, 0);
    square[0] = square[1] = square[8] = square[9] = 255;
    int result = wand_trace(square.data(), 8, 8, &points, &pointCount, &loops, &loopCount);
    CHECK_EQ(result, 0);
    CHECK_EQ(loopCount, size_t(1));
    CHECK_EQ(pointCount, size_t(4));
    std::free(points); std::free(loops);
}

TEST_CASE(spot_heal_fills_a_hole_from_surroundings) {
    // A flat gray field with a black spot; healing must bring the spot back to about the field's tone.
    Rgba image(64, 64);
    image.fill(120, 120, 120, 255);
    std::vector<uint8_t> coverage(64 * 64, 0);
    for (size_t y = 28; y < 36; y++) for (size_t x = 28; x < 36; x++) {
        auto* p = image.px(x, y); p[0] = p[1] = p[2] = 0;
        coverage[y * 64 + x] = 255;
    }
    long bounds[4];
    heal_coverage_bounds(coverage.data(), 64, 64, 64, bounds);
    CHECK_EQ(bounds[0], 28L); CHECK_EQ(bounds[2], 36L);
    int result = spot_heal(image.bytes.data(), coverage.data(), 64, 64, image.stride, 1.0f, 0, 1234);
    CHECK_EQ(result, 0);
    for (size_t y = 28; y < 36; y++) for (size_t x = 28; x < 36; x++) CHECK(image.px(x, y)[0] > 100);
}

TEST_CASE(content_fill_covers_masked_region) {
    Rgba image(48, 48);
    for (size_t y = 0; y < 48; y++) for (size_t x = 0; x < 48; x++) {
        bool light = ((x / 4) + (y / 4)) % 2 == 0;
        auto* p = image.px(x, y); p[0] = p[1] = p[2] = light ? 200 : 60; p[3] = 255;
    }
    std::vector<uint8_t> mask(48 * 48, 0);
    for (size_t y = 20; y < 28; y++) for (size_t x = 20; x < 28; x++) { mask[y * 48 + x] = 255; auto* p = image.px(x, y); p[0] = p[1] = p[2] = 0; p[3] = 0; }
    int result = content_fill(image.bytes.data(), image.stride, mask.data(), 48, 48, 48);
    CHECK_EQ(result, 1);
    for (size_t y = 20; y < 28; y++) for (size_t x = 20; x < 28; x++) CHECK_EQ(image.px(x, y)[3], uint8_t(255));
}

TEST_MAIN()
