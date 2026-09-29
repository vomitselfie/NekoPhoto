#include "compositor/morphology.h"
#include "compositor/blur.h"
#include "compositor/imaget.h"
#include "compositor/parallel.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace compositor {

namespace {

constexpr float farAway = 1e20f;

/// Full scale and the "selected" threshold (half, rounded up as 128 is at 8 bits) of a coverage raster.
template <class G> struct Scale;
template <> struct Scale<GrayImage> { using Sample = uint8_t; static constexpr float one = 255.0f; static constexpr unsigned half = 128; static constexpr float round = 0.5f; };
template <> struct Scale<Gray16> { using Sample = uint16_t; static constexpr float one = 32768.0f; static constexpr unsigned half = 16384; static constexpr float round = 0.5f; };
/// 32 bits: coverage 0..1, selected from one half, nothing rounded.
template <> struct Scale<GrayF> { using Sample = float; static constexpr float one = 1.0f; static constexpr float half = 0.5f; static constexpr float round = 0.0f; };

/// One-dimensional squared distance transform of `f` (n samples, farAway where there is no source)
/// into `d`, with scratch `v` (n ints) and `z` (n + 1 floats). Felzenszwalb & Huttenlocher 2012, §3.
void transform1d(const float* f, float* d, int n, int* v, float* z) {
    int k = 0;
    v[0] = 0;
    z[0] = -farAway;
    z[1] = farAway;
    for (int q = 1; q < n; q++) {
        // Where the parabola from q overtakes the one from the current rightmost vertex; while that is left of
        // the vertex's own start, the vertex is never the minimum and is dropped.
        float s = ((f[q] + float(q) * q) - (f[v[k]] + float(v[k]) * v[k])) / (2.0f * float(q - v[k]));
        while (k > 0 && s <= z[k]) {
            k--;
            s = ((f[q] + float(q) * q) - (f[v[k]] + float(v[k]) * v[k])) / (2.0f * float(q - v[k]));
        }
        k++;
        v[k] = q;
        z[k] = s;
        z[k + 1] = farAway;
    }
    k = 0;
    for (int q = 0; q < n; q++) {
        while (z[k + 1] < float(q)) k++;
        float dx = float(q - v[k]);
        d[q] = dx * dx + f[v[k]];
    }
}

template <class G>
std::vector<float> squaredDistanceTransformImpl(const G& coverage, bool selected) {
    const int w = coverage.width(), h = coverage.height();
    std::vector<float> d(size_t(w) * h);
    // Columns first: for each x, the 1-D transform down y of "0 where the pixel is a source, far otherwise".
    parallelRows(0, w, [&](int x0, int x1) {
        const size_t n = size_t(h);
        std::vector<float> f(n), out(n), z(n + 1);
        std::vector<int> v(n);
        for (int x = x0; x < x1; x++) {
            for (int y = 0; y < h; y++) f[size_t(y)] = (coverage.at(x, y) >= Scale<G>::half) == selected ? 0.0f : farAway;
            transform1d(f.data(), out.data(), h, v.data(), z.data());
            for (int y = 0; y < h; y++) d[size_t(y) * w + size_t(x)] = out[size_t(y)];
        }
    }, 8);
    // Then rows, on the column results.
    parallelRows(0, h, [&](int y0, int y1) {
        const size_t n = size_t(w);
        std::vector<float> f(n), out(n), z(n + 1);
        std::vector<int> v(n);
        for (int y = y0; y < y1; y++) {
            float* row = &d[size_t(y) * w];
            std::copy(row, row + w, f.begin());
            transform1d(f.data(), out.data(), w, v.data(), z.data());
            std::copy(out.begin(), out.end(), row);
        }
    }, 8);
    return d;
}

template <class G>
std::shared_ptr<G> growSelectionImpl(const G& coverage, int amount) {
    using S = typename Scale<G>::Sample;
    constexpr float one = Scale<G>::one;
    constexpr auto half = Scale<G>::half;
    const int w = coverage.width(), h = coverage.height();
    auto out = std::make_shared<G>(w, h, 0);
    if (amount == 0) { *out = coverage; return out; }
    const float r = float(std::abs(amount));
    if (amount > 0) {
        // Outside pixels within r of the selection join it; the rim ramps over one pixel.
        std::vector<float> d = squaredDistanceTransformImpl(coverage, true);
        parallelRows(0, h, [&](int y0, int y1) {
            for (int y = y0; y < y1; y++) {
                S* o = out->row(y);
                const float* dr = &d[size_t(y) * w];
                for (int x = 0; x < w; x++) {
                    float v = coverage.at(x, y) >= half ? 1.0f : std::clamp(r + 0.5f - std::sqrt(dr[x]), 0.0f, 1.0f);
                    o[x] = S(v * one + Scale<G>::round);
                }
            }
        });
    } else {
        // Inside pixels within r of the outside leave it.
        std::vector<float> d = squaredDistanceTransformImpl(coverage, false);
        parallelRows(0, h, [&](int y0, int y1) {
            for (int y = y0; y < y1; y++) {
                S* o = out->row(y);
                const float* dr = &d[size_t(y) * w];
                for (int x = 0; x < w; x++) {
                    float v = coverage.at(x, y) < half ? 0.0f : std::clamp(std::sqrt(dr[x]) - r + 0.5f, 0.0f, 1.0f);
                    o[x] = S(v * one + Scale<G>::round);
                }
            }
        });
    }
    return out;
}

template <class G>
std::shared_ptr<G> borderSelectionImpl(const G& coverage, int width) {
    using S = typename Scale<G>::Sample;
    const int w = coverage.width(), h = coverage.height();
    auto out = std::make_shared<G>(w, h, 0);
    if (width <= 0) return out;
    std::vector<float> toSelected = squaredDistanceTransformImpl(coverage, true);
    std::vector<float> toOutside = squaredDistanceTransformImpl(coverage, false);
    const float half = width / 2.0f;
    parallelRows(0, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            S* o = out->row(y);
            for (int x = 0; x < w; x++) {
                size_t i = size_t(y) * w + size_t(x);
                // Signed distance to the edge: positive inside (to the outside), negative outside (to the selection).
                float signedDistance = coverage.at(x, y) >= Scale<G>::half ? std::sqrt(toOutside[i]) - 0.5f : -(std::sqrt(toSelected[i]) - 0.5f);
                float v = std::clamp(half + 0.5f - std::fabs(signedDistance), 0.0f, 1.0f);
                o[x] = S(v * Scale<G>::one + Scale<G>::round);
            }
        }
    });
    return out;
}

template <class G>
std::shared_ptr<G> smoothSelectionImpl(const G& coverage, int radius) {
    using S = typename Scale<G>::Sample;
    const int w = coverage.width(), h = coverage.height();
    auto out = std::make_shared<G>(w, h, 0);
    if (radius <= 0) { *out = coverage; return out; }
    // Row prefix sums of the selected mask, then each pixel counts the disc as one span per row.
    std::vector<int32_t> prefix(size_t(w + 1) * h);
    parallelRows(0, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            int32_t* p = &prefix[size_t(y) * (w + 1)];
            p[0] = 0;
            for (int x = 0; x < w; x++) p[x + 1] = p[x] + (coverage.at(x, y) >= Scale<G>::half ? 1 : 0);
        }
    });
    std::vector<int> halfWidth(size_t(radius) + 1);
    long discArea = 0;
    for (int dy = 0; dy <= radius; dy++) { halfWidth[size_t(dy)] = int(std::floor(std::sqrt(double(radius) * radius - double(dy) * dy))); discArea += (dy == 0 ? 1 : 2) * (2 * halfWidth[size_t(dy)] + 1); }
    parallelRows(0, h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            S* o = out->row(y);
            for (int x = 0; x < w; x++) {
                long count = 0;
                for (int dy = -radius; dy <= radius; dy++) {
                    int yy = y + dy;
                    if (yy < 0 || yy >= h) continue;
                    int hw = halfWidth[size_t(std::abs(dy))];
                    int xa = std::max(0, x - hw), xb = std::min(w, x + hw + 1);
                    const int32_t* p = &prefix[size_t(yy) * (w + 1)];
                    count += p[xb] - p[xa];
                }
                o[x] = count * 2 > discArea ? S(Scale<G>::one) : S(0);
            }
        }
    });
    return out;
}

template <class G>
std::shared_ptr<G> featherSelectionImpl(const G& coverage, double radius) {
    auto out = std::make_shared<G>(coverage);
    if (radius > 0) gaussianBlur(*out, radius);
    return out;
}

} // namespace

std::vector<float> squaredDistanceTransform(const GrayImage& coverage, bool selected) { return squaredDistanceTransformImpl(coverage, selected); }
std::shared_ptr<GrayImage> growSelection(const GrayImage& coverage, int amount) { return growSelectionImpl(coverage, amount); }
std::shared_ptr<GrayImage> smoothSelection(const GrayImage& coverage, int radius) { return smoothSelectionImpl(coverage, radius); }
std::shared_ptr<GrayImage> borderSelection(const GrayImage& coverage, int width) { return borderSelectionImpl(coverage, width); }
std::shared_ptr<GrayImage> featherSelection(const GrayImage& coverage, double radius) { return featherSelectionImpl(coverage, radius); }
std::shared_ptr<Gray16> growSelection(const Gray16& coverage, int amount) { return growSelectionImpl(coverage, amount); }
std::shared_ptr<Gray16> smoothSelection(const Gray16& coverage, int radius) { return smoothSelectionImpl(coverage, radius); }
std::shared_ptr<Gray16> borderSelection(const Gray16& coverage, int width) { return borderSelectionImpl(coverage, width); }
std::shared_ptr<Gray16> featherSelection(const Gray16& coverage, double radius) { return featherSelectionImpl(coverage, radius); }
std::shared_ptr<GrayF> growSelection(const GrayF& coverage, int amount) { return growSelectionImpl(coverage, amount); }
std::shared_ptr<GrayF> smoothSelection(const GrayF& coverage, int radius) { return smoothSelectionImpl(coverage, radius); }
std::shared_ptr<GrayF> borderSelection(const GrayF& coverage, int width) { return borderSelectionImpl(coverage, width); }
std::shared_ptr<GrayF> featherSelection(const GrayF& coverage, double radius) { return featherSelectionImpl(coverage, radius); }

} // namespace compositor
