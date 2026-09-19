// Each optimised kernel against the portable C reference it replaces.
#include "check.h"
#include "compositor/kernels.h"
#include "compositor/adjustments.h"
extern "C" {
#include "AdjustPixels.h"
#include "LensPixels.h"
#include "LevelsPixels.h"
#include "NoisePixels.h"
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
