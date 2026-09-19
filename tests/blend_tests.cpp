// The integer blend formulas against the float implementation they replaced, and the Normal
// span path against the per-pixel one.
#include "check.h"
#include "compositor/blend.h"
#include <cmath>
#include <random>

using namespace compositor;

namespace {

float sep(BlendMode mode, float cb, float cs) {
    switch (mode) {
    case BlendMode::Multiply: return cb * cs;
    case BlendMode::Screen: return cb + cs - cb * cs;
    case BlendMode::Overlay: return cb <= 0.5f ? cs * 2 * cb : (cs + (2 * cb - 1) - cs * (2 * cb - 1));
    case BlendMode::Darken: return std::min(cb, cs);
    case BlendMode::Lighten: return std::max(cb, cs);
    case BlendMode::Difference: return std::fabs(cb - cs);
    case BlendMode::ColorDodge: if (cb <= 0) return 0; if (cs >= 1) return 1; return std::min(1.0f, cb / (1 - cs));
    case BlendMode::ColorBurn: if (cb >= 1) return 1; if (cs <= 0) return 0; return 1 - std::min(1.0f, (1 - cb) / cs);
    default: return cs;
    }
}

/// The previous float path (coverage as an exact float, straight colours, PDF general formula).
void referenceComposite(BlendMode mode, const uint8_t* src, float coverage, uint8_t* dst) {
    float as = src[3] / 255.0f * coverage;
    if (as <= 0) return;
    float k = coverage / 255.0f;
    float sr = src[0] * k, sg = src[1] * k, sb = src[2] * k;
    float ab = dst[3] / 255.0f;
    float br = dst[0] / 255.0f, bg = dst[1] / 255.0f, bb = dst[2] / 255.0f;
    float ao = as + ab * (1 - as);
    float r, g, b;
    if (mode == BlendMode::Normal || ab <= 0) {
        r = sr + br * (1 - as); g = sg + bg * (1 - as); b = sb + bb * (1 - as);
    } else {
        float cs[3] = {sr / as, sg / as, sb / as}, cb[3] = {br / ab, bg / ab, bb / ab};
        float m[3] = {sep(mode, cb[0], cs[0]), sep(mode, cb[1], cs[1]), sep(mode, cb[2], cs[2])};
        r = sr * (1 - ab) + br * (1 - as) + as * ab * m[0];
        g = sg * (1 - ab) + bg * (1 - as) + as * ab * m[1];
        b = sb * (1 - ab) + bb * (1 - as) + as * ab * m[2];
    }
    auto q = [](float v) { return uint8_t(std::min(255.0f, std::max(0.0f, v * 255.0f + 0.5f))); };
    uint8_t a8 = q(ao);
    dst[0] = std::min(q(r), a8); dst[1] = std::min(q(g), a8); dst[2] = std::min(q(b), a8); dst[3] = a8;
}

void randomPremultiplied(std::mt19937& rng, uint8_t* p) {
    unsigned a = rng() % 4 == 0 ? 255 : rng() % 256;
    for (int c = 0; c < 3; c++) p[c] = uint8_t((rng() % 256) * a / 255);
    p[3] = uint8_t(a);
}

} // namespace

TEST_CASE(integer_separable_blends_match_float_within_a_level) {
    std::mt19937 rng(11);
    const BlendMode modes[] = {BlendMode::Multiply, BlendMode::Screen, BlendMode::Overlay, BlendMode::Darken, BlendMode::Lighten,
                               BlendMode::Difference, BlendMode::ColorDodge, BlendMode::ColorBurn};
    for (BlendMode mode : modes) {
        int worst = 0;
        for (int i = 0; i < 20000; i++) {
            uint8_t s[4], d[4], e[4];
            randomPremultiplied(rng, s);
            randomPremultiplied(rng, d);
            // Coverages on the 1/256 grid the fast paths use, so both sides see the same source scaling.
            float coverage = float(rng() % 257) / 256.0f;
            for (int c = 0; c < 4; c++) e[c] = d[c];
            referenceComposite(mode, s, coverage, e);
            compositePixel(mode, s, coverage, d);
            for (int c = 0; c < 4; c++) worst = std::max(worst, std::abs(int(d[c]) - int(e[c])));
            for (int c = 0; c < 3; c++) CHECK(d[c] <= d[3]);
        }
        CHECK(worst <= 2);
    }
}

TEST_CASE(normal_span_equals_per_pixel) {
    std::mt19937 rng(5);
    const int n = 1000;
    const size_t count = size_t(n);
    std::vector<uint8_t> src(count * 4), a(count * 4), b(count * 4);
    std::vector<uint16_t> steps(count);
    for (int i = 0; i < n; i++) {
        randomPremultiplied(rng, &src[size_t(i) * 4]);
        randomPremultiplied(rng, &a[size_t(i) * 4]);
        for (int c = 0; c < 4; c++) b[size_t(i) * 4 + size_t(c)] = a[size_t(i) * 4 + size_t(c)];
        steps[size_t(i)] = uint16_t(rng() % 257);
    }
    for (int i = 0; i < n; i++) compositePixelSteps(BlendMode::Normal, &src[size_t(i) * 4], steps[size_t(i)], &a[size_t(i) * 4]);
    compositeSpanNormal(src.data(), steps.data(), b.data(), n);
    CHECK(a == b);
}

TEST_CASE(opaque_source_over_is_a_copy) {
    uint8_t s[4] = {10, 200, 30, 255}, d[4] = {90, 90, 90, 255};
    compositePixel(BlendMode::Normal, s, 1.0f, d);
    CHECK_EQ(int(d[0]), 10); CHECK_EQ(int(d[1]), 200); CHECK_EQ(int(d[2]), 30); CHECK_EQ(int(d[3]), 255);
    uint8_t m[4] = {90, 90, 90, 255};
    uint8_t half[4] = {128, 128, 128, 255};
    compositePixel(BlendMode::Multiply, half, 1.0f, m);
    CHECK(std::abs(int(m[0]) - 45) <= 1);
}

TEST_MAIN()
