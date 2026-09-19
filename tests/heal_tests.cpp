// The mean-value membrane against exact cases and the reference solver, and spot healing end to end.
#include "check.h"
#include "compositor/heal.h"
#include <cmath>
#include <cstring>
#include <random>
#include <vector>
extern "C" {
#include "HealPixels.h"
}

using namespace compositor;

namespace {

/// A disc hole of radius r at the centre, with the ring of width 3 around it known.
void discHole(int w, int h, double r, std::vector<uint8_t>& hole, std::vector<uint8_t>& known) {
    hole.assign(size_t(w) * h, 0); known.assign(size_t(w) * h, 0);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        double d = std::hypot(x + 0.5 - w / 2.0, y + 0.5 - h / 2.0);
        if (d < r) hole[size_t(y) * w + size_t(x)] = 1;
        else if (d < r + 3) known[size_t(y) * w + size_t(x)] = 1;
    }
}

} // namespace

TEST_CASE(membrane_reproduces_a_linear_ramp) {
    const int w = 60, h = 50;
    std::vector<uint8_t> hole, known;
    discHole(w, h, 14, hole, known);
    std::vector<float> values(size_t(w) * h * 2, 0.0f);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { values[(size_t(y) * w + size_t(x)) * 2] = float(3 * x + 2 * y); values[(size_t(y) * w + size_t(x)) * 2 + 1] = 100 - float(x); }
    std::vector<float> filled = values;
    for (size_t p = 0; p < hole.size(); p++) if (hole[p]) filled[p * 2] = filled[p * 2 + 1] = -1;
    membraneFill(filled.data(), 2, hole.data(), known.data(), w, h);
    // Mean-value coordinates reproduce linear functions; the boundary is sampled at pixel corners with
    // the neighbouring pixel's value, so the ramp comes back within a level.
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        size_t p = size_t(y) * w + size_t(x);
        if (!hole[p]) continue;
        CHECK(std::fabs(filled[p * 2] - values[p * 2]) <= 1.5f);
        CHECK(std::fabs(filled[p * 2 + 1] - values[p * 2 + 1]) <= 1.0f);
    }
    // An island inside the hole (a known ring in the middle) is a second boundary loop; still exact.
    std::vector<uint8_t> hole2 = hole, known2 = known;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        double d = std::hypot(x + 0.5 - w / 2.0, y + 0.5 - h / 2.0);
        if (d < 4) { hole2[size_t(y) * w + size_t(x)] = 0; known2[size_t(y) * w + size_t(x)] = 1; }
    }
    std::vector<float> filled2 = values;
    for (size_t p = 0; p < hole2.size(); p++) if (hole2[p]) filled2[p * 2] = filled2[p * 2 + 1] = -1;
    membraneFill(filled2.data(), 2, hole2.data(), known2.data(), w, h);
    for (size_t p = 0; p < hole2.size(); p++) if (hole2[p]) CHECK(std::fabs(filled2[p * 2] - values[p * 2]) <= 1.5f);
    // A large hole takes the hierarchical boundary path and stays close to the ramp.
    const int W = 400, H = 300;
    std::vector<uint8_t> bigHole, bigKnown;
    discHole(W, H, 120, bigHole, bigKnown);
    std::vector<float> big(size_t(W) * H, 0.0f);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) big[size_t(y) * W + size_t(x)] = float(x) * 0.5f + float(y) * 0.25f;
    std::vector<float> bigFilled = big;
    membraneFill(bigFilled.data(), 1, bigHole.data(), bigKnown.data(), W, H);
    float worst = 0;
    for (size_t p = 0; p < bigHole.size(); p++) if (bigHole[p]) worst = std::max(worst, std::fabs(bigFilled[p] - big[p]));
    CHECK(worst <= 1.5f);
}

TEST_CASE(spot_heal_matches_the_reference_on_a_flat_field) {
    // A flat grey field with a black spot: healing brings the spot back to the field's tone, as the reference does.
    const int w = 64, h = 64;
    Image ours(w, h);
    ours.fill(120, 120, 120, 255);
    for (int y = 28; y < 36; y++) for (int x = 28; x < 36; x++) { uint8_t* p = ours.pixel(x, y); p[0] = p[1] = p[2] = 0; }
    GrayImage coverage(w, h, 0);
    for (int y = 27; y < 37; y++) for (int x = 27; x < 37; x++) coverage.at(x, y) = 255;
    for (int mode : {0, 1, 2}) {
        Image mine = ours, reference = ours;
        spotHeal(mine, coverage, 1.0f, mode, 1234);
        REQUIRE(spot_heal(reference.data(), coverage.data(), size_t(w), size_t(h), size_t(reference.stride()), 1.0f, mode, 1234) == 0);
        int worst = 0;
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) for (int c = 0; c < 4; c++) worst = std::max(worst, std::abs(int(mine.pixel(x, y)[c]) - int(reference.pixel(x, y)[c])));
        CHECK(worst <= 3);
        CHECK(mine.pixel(32, 32)[0] > 100);
    }
}

TEST_CASE(spot_heal_carries_texture_and_tone) {
    // A vertical gradient with a dark spot: Content-Aware copies texture and the membrane matches the tone,
    // so the healed spot follows the gradient rather than the copied patch's level.
    const int w = 96, h = 96;
    Image img(w, h);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { uint8_t* p = img.pixel(x, y); p[0] = p[1] = p[2] = uint8_t(40 + y * 2); p[3] = 255; }
    for (int y = 44; y < 52; y++) for (int x = 44; x < 52; x++) { uint8_t* p = img.pixel(x, y); p[0] = p[1] = p[2] = 0; }
    GrayImage coverage(w, h, 0);
    for (int y = 43; y < 53; y++) for (int x = 43; x < 53; x++) coverage.at(x, y) = 255;
    spotHeal(img, coverage, 1.0f, 0, 7);
    for (int y = 44; y < 52; y++) CHECK(std::abs(int(img.pixel(48, y)[0]) - (40 + y * 2)) <= 4);
    // Half opacity leaves the spot half way.
    Image half(w, h);
    half.fill(200, 200, 200, 255);
    for (int y = 44; y < 52; y++) for (int x = 44; x < 52; x++) { uint8_t* p = half.pixel(x, y); p[0] = p[1] = p[2] = 0; }
    spotHeal(half, coverage, 0.5f, 1, 7);
    CHECK(std::abs(int(half.pixel(48, 48)[0]) - 100) <= 12);
}

TEST_MAIN()
