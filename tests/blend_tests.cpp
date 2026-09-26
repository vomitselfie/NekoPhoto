// The integer blend formulas against the float implementation they replaced, and the Normal
// span path against the per-pixel one.
#include "check.h"
#include <set>
#include <array>
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

TEST_CASE(photoshops_other_modes_match_its_captures) {
    // Patchy's samples of full 256 x 256 Photoshop 2026 captures (tests/core/compositor_blend_if_tests.cpp there).
    struct Triple { int s, d, expected; };
    auto blendGray = [](BlendMode mode, int s, int d) {
        uint8_t src[4] = {uint8_t(s), uint8_t(s), uint8_t(s), 255}, dst[4] = {uint8_t(d), uint8_t(d), uint8_t(d), 255};
        compositePixel(mode, src, 1.0f, dst);
        return int(dst[0]);
    };
    const Triple vivid[] = {{0, 0, 0}, {0, 255, 0}, {1, 200, 0}, {1, 254, 127}, {1, 255, 255}, {2, 254, 191}, {37, 200, 65}, {37, 254, 252},
                            {64, 128, 2}, {64, 200, 145}, {65, 128, 4}, {100, 63, 9}, {100, 126, 90}, {100, 200, 185}, {127, 63, 61},
                            {127, 128, 127}, {128, 1, 1}, {128, 128, 128}, {129, 126, 127}, {129, 200, 202}, {129, 254, 255}, {170, 63, 94},
                            {170, 126, 188}, {170, 200, 255}, {192, 1, 2}, {192, 63, 128}, {254, 1, 128}, {255, 0, 255}};
    for (const Triple& t : vivid) CHECK_EQ(blendGray(BlendMode::VividLight, t.s, t.d), t.expected);
    const Triple linear[] = {{0, 255, 0}, {1, 254, 0}, {1, 255, 1}, {2, 255, 3}, {37, 200, 18}, {64, 254, 126}, {65, 128, 2}, {100, 200, 144},
                             {127, 255, 253}, {128, 128, 128}, {129, 0, 2}, {170, 128, 212}, {192, 126, 254}, {255, 0, 254}};
    for (const Triple& t : linear) CHECK_EQ(blendGray(BlendMode::LinearLight, t.s, t.d), t.expected);
    const Triple hardMix[] = {{0, 254, 0}, {0, 255, 255}, {1, 254, 255}, {37, 200, 0}, {37, 254, 255}, {64, 128, 0}, {64, 200, 255},
                              {127, 126, 0}, {127, 128, 255}, {128, 126, 0}, {128, 128, 255}, {170, 63, 0}, {170, 126, 255}, {254, 1, 0}, {255, 1, 255}};
    for (const Triple& t : hardMix) CHECK_EQ(blendGray(BlendMode::HardMix, t.s, t.d), t.expected);
    // Darker / Lighter Color take whole colours by rounded luma.
    auto whole = [](BlendMode mode, std::array<uint8_t, 3> s, std::array<uint8_t, 3> d) {
        uint8_t src[4] = {s[0], s[1], s[2], 255}, dst[4] = {d[0], d[1], d[2], 255};
        compositePixel(mode, src, 1.0f, dst);
        return std::array<uint8_t, 3>{dst[0], dst[1], dst[2]};
    };
    const std::array<uint8_t, 3> reddish{200, 60, 40}, greenish{40, 160, 40};
    CHECK(whole(BlendMode::DarkerColor, reddish, greenish) == reddish);
    CHECK(whole(BlendMode::LighterColor, reddish, greenish) == greenish);
    // The simple ones.
    CHECK_EQ(blendGray(BlendMode::LinearBurn, 100, 200), 45);
    CHECK_EQ(blendGray(BlendMode::LinearDodge, 100, 200), 255);
    CHECK_EQ(blendGray(BlendMode::Subtract, 100, 200), 100);
    CHECK_EQ(blendGray(BlendMode::Exclusion, 100, 200), 100 + 200 - 2 * ((100 * 200 + 127) / 255));
    CHECK_EQ(blendGray(BlendMode::Divide, 0, 10), 255);
    // Dissolve: each pixel whole or nothing, about as many as the alpha says, the same pattern every time.
    int drawn = 0;
    for (int y = 0; y < 100; y++)
        for (int x = 0; x < 100; x++) {
            uint8_t src[4] = {64, 0, 0, 128}, dst[4] = {0, 0, 0, 0}, again[4] = {0, 0, 0, 0};
            compositePixelAt(BlendMode::Dissolve, src, 1.0f, dst, x, y);
            compositePixelAt(BlendMode::Dissolve, src, 1.0f, again, x, y);
            CHECK(dst[3] == 0 || dst[3] == 255);
            CHECK(dst[3] == again[3]);
            drawn += dst[3] == 255;
        }
    CHECK(drawn > 4600 && drawn < 5400);   // alpha 128: about half
    // Every mode has a name that reads back, and the menu lists each once.
    std::set<int> listed;
    for (int m : blendModeMenuOrder()) if (m >= 0) CHECK(listed.insert(m).second);
    CHECK_EQ(int(listed.size()), blendModeCount);
    for (int i = 0; i < blendModeCount; i++) { BlendMode back; CHECK(parseBlendMode(blendModeName(BlendMode(i)), back) && back == BlendMode(i)); }
}

TEST_MAIN()
