// Distance-transform morphology and the coverage rasterizer against brute force and the previous code.
#include "check.h"
#include "compositor/morphology.h"
#include "compositor/selection.h"
#include <cmath>
#include <random>

using namespace compositor;

namespace {

GrayImage randomBlobs(int w, int h, uint32_t seed) {
    GrayImage g(w, h, 0);
    std::mt19937 rng(seed);
    for (int i = 0; i < 6; i++) {
        int cx = int(rng() % w), cy = int(rng() % h), r = 4 + int(rng() % 14);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) g.at(x, y) = 255;
    }
    // A soft edge somewhere, to check the >= 128 threshold.
    for (int y = 20; y < 30 && y < h; y++) for (int x = 0; x < 10 && x < w; x++) g.at(x, y) = uint8_t(x * 25);
    return g;
}

/// O(N^2) squared distance to the nearest pixel with `inside` truth.
std::vector<float> bruteDistance(const GrayImage& g, bool selected) {
    int w = g.width(), h = g.height();
    std::vector<float> d(size_t(w) * h, 1e20f);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        float best = 1e20f;
        for (int sy = 0; sy < h; sy++) for (int sx = 0; sx < w; sx++)
            if ((g.at(sx, sy) >= 128) == selected) best = std::min(best, float((sx - x) * (sx - x) + (sy - y) * (sy - y)));
        d[size_t(y) * w + size_t(x)] = best;
    }
    return d;
}

/// The previous brute-force disc dilation / erosion, thresholded input.
GrayImage discResize(const GrayImage& src, int amount) {
    int w = src.width(), h = src.height(), r = std::abs(amount);
    GrayImage out(w, h, 0);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        int v = amount > 0 ? 0 : 255;
        for (int dy = -r; dy <= r; dy++) for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy > r * r) continue;
            int sx = x + dx, sy = y + dy;
            int s = (sx < 0 || sy < 0 || sx >= w || sy >= h) ? 0 : (src.at(sx, sy) >= 128 ? 255 : 0);
            v = amount > 0 ? std::max(v, s) : std::min(v, s);
        }
        out.at(x, y) = uint8_t(v);
    }
    return out;
}

double coverageSum(const GrayImage& g) {
    double s = 0;
    for (int y = 0; y < g.height(); y++) for (int x = 0; x < g.width(); x++) s += g.at(x, y) / 255.0;
    return s;
}

} // namespace

TEST_CASE(distance_transform_is_exact) {
    for (uint32_t seed : {1u, 2u, 3u}) {
        GrayImage g = randomBlobs(64, 48, seed);
        for (bool selected : {true, false}) {
            std::vector<float> fast = squaredDistanceTransform(g, selected), slow = bruteDistance(g, selected);
            float worst = 0;
            for (size_t i = 0; i < fast.size(); i++) if (slow[i] < 1e19f) worst = std::max(worst, std::fabs(fast[i] - slow[i]));
            CHECK(worst < 1e-3f);
        }
    }
}

TEST_CASE(grow_matches_disc_morphology_away_from_the_rim) {
    GrayImage g = randomBlobs(80, 60, 5);
    for (int amount : {3, 9, -3, -7}) {
        auto fast = growSelection(g, amount);
        GrayImage slow = discResize(g, amount);
        // Hard pixels agree exactly; the antialiased rim (values strictly between) may differ by up to a pixel.
        // The old disc erosion also ate into shapes touching the canvas edge (outside counted as unselected);
        // the distance transform, like Photoshop, leaves those edges alone, so the comparison stays away from them.
        int r = std::abs(amount), disagreements = 0;
        for (int y = r + 1; y < g.height() - r - 1; y++) for (int x = r + 1; x < g.width() - r - 1; x++) {
            uint8_t f = fast->at(x, y), s = slow.at(x, y);
            if (f == 0 || f == 255) { if (f != s) disagreements++; }
        }
        CHECK(disagreements <= 4);   // a few pixels lying exactly on the circle's edge
    }
    // Growing then shrinking a round blob returns roughly the blob.
    auto back = growSelection(*growSelection(g, 6), -6);
    CHECK(std::fabs(coverageSum(*back) - coverageSum(g)) < coverageSum(g) * 0.15);
}

TEST_CASE(smooth_border_feather_behave) {
    GrayImage g(60, 60, 0);
    for (int y = 10; y < 50; y++) for (int x = 10; x < 50; x++) g.at(x, y) = 255;
    g.at(30, 30) = 0;          // a hole a smooth fills
    g.at(5, 5) = 255;          // a speck a smooth removes
    auto smooth = smoothSelection(g, 3);
    CHECK_EQ(int(smooth->at(30, 30)), 255);
    CHECK_EQ(int(smooth->at(5, 5)), 0);
    CHECK_EQ(int(smooth->at(20, 20)), 255);
    CHECK_EQ(int(smooth->at(2, 40)), 0);
    auto border = borderSelection(g, 4);
    CHECK(border->at(10, 30) > 200);     // on the edge
    CHECK_EQ(int(border->at(30, 20)), 0);   // deep inside
    CHECK_EQ(int(border->at(30, 2)), 0);    // far outside
    auto feather = featherSelection(g, 3);
    CHECK(feather->at(10, 30) > 60 && feather->at(10, 30) < 200);
    CHECK_EQ(int(feather->at(30, 30)), 255 - 255 + int(feather->at(30, 30)));   // finite
    CHECK(feather->at(30, 30) > 240);
}

TEST_CASE(rasterizer_coverage_is_exact_for_squares_and_close_for_circles) {
    // Half-pixel-aligned square: interior 255, edge pixels exactly 50 %.
    auto square = rasterizeRect(Rect(10.5, 10.5, 20, 20), 50, 50, true);
    CHECK_EQ(int(square->at(20, 20)), 255);
    CHECK_EQ(int(square->at(10, 20)), 128);
    CHECK_EQ(int(square->at(30, 20)), 128);
    CHECK_EQ(int(square->at(10, 10)), 64);
    CHECK_EQ(int(square->at(5, 5)), 0);
    CHECK_NEAR(coverageSum(*square), 400.0, 0.5);
    // A circle's coverage sums to its area; a big one is smooth at the rim.
    auto circle = rasterizeEllipse(Rect(100, 100, 600, 600), 800, 800, true);
    CHECK_NEAR(coverageSum(*circle), M_PI * 300 * 300, M_PI * 300 * 300 * 0.002);
    int steps = 0;
    for (int x = 95; x < 110; x++) if (circle->at(x, 400) != circle->at(x - 1, 400)) steps++;
    CHECK(steps >= 1 && steps <= 3);
    // Aliased rasterization stays a pixel-centre test.
    auto hard = rasterizeRect(Rect(10.4, 10.4, 20, 20), 50, 50, false);
    CHECK_EQ(int(hard->at(10, 10)), 255);
    CHECK_EQ(int(hard->at(9, 10)), 0);
    // Winding: a polygon drawn clockwise or counterclockwise fills the same; a self-overlap stays opaque.
    std::vector<Point> tri = {{5, 5}, {45, 8}, {25, 44}};
    auto a = rasterizePolygon(tri, 50, 50, true);
    std::reverse(tri.begin(), tri.end());
    auto b = rasterizePolygon(tri, 50, 50, true);
    int diff = 0;
    for (int y = 0; y < 50; y++) for (int x = 0; x < 50; x++) diff = std::max(diff, std::abs(int(a->at(x, y)) - int(b->at(x, y))));
    CHECK(diff <= 1);
    std::vector<Point> bowtie = {{5, 5}, {45, 45}, {45, 5}, {5, 45}};
    auto bt = rasterizePolygon(bowtie, 50, 50, true);
    CHECK_EQ(int(bt->at(44, 10)), 255);   // inside the right lobe only
    CHECK_EQ(int(bt->at(10, 30)), 255);   // inside the left lobe only
    CHECK_EQ(int(bt->at(25, 10)), 0);     // inside both: the windings cancel (nonzero rule, as before)
    CHECK(bt->at(25, 3) < 10);            // above both
}

TEST_MAIN()
