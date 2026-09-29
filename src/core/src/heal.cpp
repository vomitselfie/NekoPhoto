#include "compositor/heal.h"
#include "compositor/depth.h"
#include "compositor/inpaint.h"
#include "compositor/parallel.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

extern "C" {
#include "WandPixels.h"
}

namespace compositor {

namespace {

// ---- Membrane by mean-value coordinates ---------------------------------------------------------

/// One boundary loop of the hole: the centres of the known pixels along it, in order, with their values.
struct Loop {
    std::vector<double> x, y;
    std::vector<float> value;   // channels per vertex
};

/// The hole's boundary loops (outer boundaries clockwise, islands counterclockwise, so the hole is always
/// on the same side), as polygons through the known pixels just outside each boundary edge.
std::vector<Loop> boundaryLoops(const uint8_t* hole, const uint8_t* known, const float* values, int channels, int width, int height) {
    std::vector<Loop> loops;
    int32_t* points = nullptr; int32_t* counts = nullptr;
    size_t pointCount = 0, loopCount = 0;
    if (wand_trace(hole, size_t(width), size_t(height), &points, &pointCount, &counts, &loopCount) != 0) return loops;
    size_t at = 0;
    for (size_t l = 0; l < loopCount; l++) {
        const int n = counts[l];
        Loop loop;
        for (int k = 0; k < n; k++) {
            const int x0 = points[(at + size_t(k)) * 2], y0 = points[(at + size_t(k)) * 2 + 1];
            const int x1 = points[(at + size_t((k + 1) % n)) * 2], y1 = points[(at + size_t((k + 1) % n)) * 2 + 1];
            // The pixel on the left of the outgoing edge lies outside the hole.
            const int dx = (x1 > x0) - (x1 < x0), dy = (y1 > y0) - (y1 < y0);
            int px, py;
            if (dx > 0) { px = x0; py = y0 - 1; }        // east: the pixel above
            else if (dy > 0) { px = x0; py = y0; }       // south: the pixel to the right
            else if (dx < 0) { px = x0 - 1; py = y0; }   // west: the pixel below
            else { px = x0 - 1; py = y0 - 1; }           // north: the pixel to the left
            if (px < 0 || py < 0 || px >= width || py >= height) continue;
            const size_t p = size_t(py) * width + size_t(px);
            if (!known[p] || hole[p]) continue;
            // The polygon runs through the centres of the known pixels beside the hole, where the values are
            // exact (a corner would sit half a pixel off and bend a ramp); one edge per pixel.
            const double vx = px + 0.5, vy = py + 0.5;
            if (!loop.x.empty() && loop.x.back() == vx && loop.y.back() == vy) continue;
            loop.x.push_back(vx); loop.y.push_back(vy);
            for (int c = 0; c < channels; c++) loop.value.push_back(values[p * size_t(channels) + size_t(c)]);
        }
        if (loop.x.size() > 1 && loop.x.front() == loop.x.back() && loop.y.front() == loop.y.back()) {
            loop.x.pop_back(); loop.y.pop_back();
            loop.value.resize(loop.value.size() - size_t(channels));
        }
        if (loop.x.size() >= 3) loops.push_back(std::move(loop));
        at += size_t(n);
    }
    std::free(points);
    std::free(counts);
    return loops;
}

/// tan(a / 2) for the signed angle a from u to v.
inline double tanHalfAngle(double ux, double uy, double vx, double vy) {
    const double cross = ux * vy - uy * vx, dot = ux * vx + uy * vy;
    if (std::fabs(cross) < 1e-12) return dot > 0 ? 0.0 : 1e12;
    return (std::sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy)) - dot) / cross;
}

/// The interpolated value at (x, y): mean-value weights over the loops, the boundary sampled hierarchically
/// (vertices far away are visited with a stride proportional to their distance, which keeps the cost of a
/// large hole near linear while the nearby boundary is used in full).
void membraneAt(const std::vector<Loop>& loops, int channels, double x, double y, float* out, std::vector<size_t>& used) {
    constexpr double spacing = 2.0;   // pixels of distance per unit of stride
    double total = 0;
    for (int c = 0; c < channels; c++) out[c] = 0;
    for (const Loop& loop : loops) {
        const size_t n = loop.x.size();
        used.clear();
        for (size_t i = 0; i < n;) {
            used.push_back(i);
            const double d = std::hypot(loop.x[i] - x, loop.y[i] - y);
            size_t stride = 1;
            while (stride * 2 * spacing <= d && stride * 16 < n) stride *= 2;
            i = (i / stride + 1) * stride;
        }
        const size_t m = used.size();
        if (m < 3) continue;
        for (size_t k = 0; k < m; k++) {
            const size_t a = used[(k + m - 1) % m], b = used[k], c = used[(k + 1) % m];
            const double bx = loop.x[b] - x, by = loop.y[b] - y;
            const double w = (tanHalfAngle(loop.x[a] - x, loop.y[a] - y, bx, by) + tanHalfAngle(bx, by, loop.x[c] - x, loop.y[c] - y)) / std::hypot(bx, by);
            total += w;
            for (int ch = 0; ch < channels; ch++) out[ch] += float(w * loop.value[b * size_t(channels) + size_t(ch)]);
        }
    }
    if (std::fabs(total) > 1e-12) for (int c = 0; c < channels; c++) out[c] = float(out[c] / total);
}

// ---- Spot healing ----------------------------------------------------------------------------

enum Role : uint8_t { Outside = 0, Ring = 1, Hole = 2 };

inline uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
inline double unitRandom(uint32_t key) { return double(hash32(key) >> 8) / 16777216.0; }

/// Mean squared difference between the ring around the spot and the ring around the patch offset by
/// (dx, dy); infinite when the patch would overlap the spot or leave the image.
double ringScore(const Image& image, const std::vector<uint8_t>& role, int wx0, int wy0, int ww, int wh, int dx, int dy, const GrayImage* visible) {
    if (std::abs(dx) < ww && std::abs(dy) < wh) return INFINITY;
    if (wx0 + dx < 0 || wy0 + dy < 0 || wx0 + ww + dx > image.width() || wy0 + wh + dy > image.height()) return INFINITY;
    double sum = 0;
    long n = 0;
    for (int y = 0; y < wh; y++) {
        const uint8_t* roleRow = &role[size_t(y) * ww];
        const uint8_t* t = image.pixel(wx0, wy0 + y);
        const uint8_t* s = image.pixel(wx0 + dx, wy0 + y + dy);
        for (int x = 0; x < ww; x++, t += 4, s += 4) {
            if (roleRow[x] == Outside) continue;
            // A patch that would copy hidden pixels is no candidate.
            if (visible && visible->at(wx0 + x + dx, wy0 + y + dy) < 128) return INFINITY;
            if (roleRow[x] != Ring) continue;
            for (int c = 0; c < 4; c++) { double d = double(t[c]) - s[c]; sum += d * d; }
            n++;
        }
    }
    return n ? sum / n : INFINITY;
}

/// Content-Aware healing: the spot and a thin band around it are synthesised from the surroundings (so
/// edges and patterns continue through it), then the band's difference from the original is spread across
/// the spot, the membrane, so the tone matches. False when there is nothing to synthesise from.
bool healBySynthesis(Image& image, const GrayImage& coverage, const PixelBounds& b, float opacity, uint32_t seed, const GrayImage* visible) {
    const int W = image.width(), H = image.height(), band = 2;
    GrayImage grown(W, H, 0);
    for (int y = std::max(0, b.y0 - band); y < std::min(H, b.y1 + band); y++)
        for (int x = std::max(0, b.x0 - band); x < std::min(W, b.x1 + band); x++) {
            bool near = false;
            for (int j = -band; j <= band && !near; j++)
                for (int i = -band; i <= band && !near; i++) {
                    const int sx = x + i, sy = y + j;
                    near = sx >= 0 && sy >= 0 && sx < W && sy < H && coverage.at(sx, sy) != 0;
                }
            grown.at(x, y) = near ? 255 : 0;
        }
    Image synthesised = image;
    InpaintOptions options;
    options.seed = seed;
    if (!contentFill(synthesised, grown, options, visible)) return false;
    // The membrane over the spot, from the band where both the original and the synthesis are known.
    const int x0 = std::max(0, b.x0 - band - 1), y0 = std::max(0, b.y0 - band - 1);
    const int x1 = std::min(W, b.x1 + band + 1), y1 = std::min(H, b.y1 + band + 1);
    const int ww = x1 - x0, wh = y1 - y0;
    std::vector<float> values(size_t(ww) * wh * 4, 0.0f);
    std::vector<uint8_t> hole(size_t(ww) * wh, 0), known(size_t(ww) * wh, 0);
    for (int y = 0; y < wh; y++)
        for (int x = 0; x < ww; x++) {
            const size_t p = size_t(y) * ww + size_t(x);
            const int ix = x0 + x, iy = y0 + y;
            if (coverage.at(ix, iy)) { hole[p] = 1; continue; }
            if (!grown.at(ix, iy) || image.pixel(ix, iy)[3] != 255 || (visible && visible->at(ix, iy) < 128)) continue;
            known[p] = 1;
            for (int c = 0; c < 4; c++) values[p * 4 + size_t(c)] = float(image.pixel(ix, iy)[c]) - float(synthesised.pixel(ix, iy)[c]);
        }
    membraneFill(values.data(), 4, hole.data(), known.data(), ww, wh);
    for (int y = 0; y < wh; y++)
        for (int x = 0; x < ww; x++) {
            const size_t p = size_t(y) * ww + size_t(x);
            if (!hole[p]) continue;
            const int ix = x0 + x, iy = y0 + y;
            uint8_t* t = image.pixel(ix, iy);
            const uint8_t* s = synthesised.pixel(ix, iy);
            const double amount = coverage.at(ix, iy) / 255.0 * opacity;
            double out[4];
            for (int c = 0; c < 4; c++) out[c] = t[c] + (s[c] + values[p * 4 + size_t(c)] - t[c]) * amount;
            t[3] = uint8_t(std::lround(std::clamp(out[3], 0.0, 255.0)));
            for (int c = 0; c < 3; c++) t[c] = uint8_t(std::lround(std::clamp(out[c], 0.0, double(t[3]))));
        }
    return true;
}

} // namespace

void membraneFill(float* values, int channels, const uint8_t* hole, const uint8_t* known, int width, int height) {
    std::vector<Loop> loops = boundaryLoops(hole, known, values, channels, width, height);
    // The fallback where no boundary is known: the mean of what is.
    std::vector<double> mean(size_t(channels), 0);
    size_t count = 0;
    for (const Loop& loop : loops) for (size_t i = 0; i < loop.x.size(); i++, count++) for (int c = 0; c < channels; c++) mean[size_t(c)] += loop.value[i * size_t(channels) + size_t(c)];
    if (count) for (auto& m : mean) m /= double(count);
    parallelRows(0, height, [&](int y0, int y1) {
        std::vector<size_t> used;
        std::vector<float> out(static_cast<size_t>(channels));
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < width; x++) {
                const size_t p = size_t(y) * width + size_t(x);
                if (!hole[p]) continue;
                float* v = values + p * size_t(channels);
                if (loops.empty()) { for (int c = 0; c < channels; c++) v[c] = float(mean[size_t(c)]); continue; }
                membraneAt(loops, channels, x + 0.5, y + 0.5, out.data(), used);
                for (int c = 0; c < channels; c++) v[c] = out[size_t(c)];
            }
    }, 4);
}

namespace {

/// Where a spot is healed from, decided on 8-bit pixels: the work box (the spot and its ring, clipped to the image),
/// each pixel's role in it, and for Proximity Match (and Content-Aware without a synthesis) the offset of the patch
/// whose ring matches best.
struct SpotPlan {
    int wx0 = 0, wy0 = 0, ww = 0, wh = 0;
    std::vector<uint8_t> role;
    long ringCount = 0;
    int ox = 0, oy = 0;
    bool haveSource = false;
};

/// The plan for the spot `coverage` marks (nonzero) within `b`; false when nothing known surrounds it.
bool planSpot(const Image& image, const GrayImage& coverage, const PixelBounds& b, int mode, uint32_t seed, const GrayImage* visible, SpotPlan& plan) {
    const int W = image.width(), H = image.height();
    const int size = std::max(b.x1 - b.x0, b.y1 - b.y0);
    const int ring = std::clamp(size / 8, 2, 16);
    // Work box: the spot plus its ring, clipped to the image.
    const int wx0 = std::max(0, b.x0 - ring), wy0 = std::max(0, b.y0 - ring);
    const int wx1 = std::min(W, b.x1 + ring), wy1 = std::min(H, b.y1 + ring);
    const int ww = wx1 - wx0, wh = wy1 - wy0;
    const size_t wn = size_t(ww) * wh;

    std::vector<uint8_t>& role = plan.role;
    role.assign(wn, Outside);
    std::vector<uint8_t> near(wn, 0);
    for (int y = 0; y < wh; y++) for (int x = 0; x < ww; x++) role[size_t(y) * ww + size_t(x)] = coverage.at(wx0 + x, wy0 + y) ? Hole : Outside;
    // The ring: pixels within `ring` of the spot (a square dilation, row pass then column pass).
    std::vector<int> prefix(size_t(std::max(ww, wh)) + 1);
    for (int y = 0; y < wh; y++) {
        prefix[0] = 0;
        for (int x = 0; x < ww; x++) prefix[size_t(x) + 1] = prefix[size_t(x)] + (role[size_t(y) * ww + size_t(x)] == Hole);
        for (int x = 0; x < ww; x++) near[size_t(y) * ww + size_t(x)] = prefix[size_t(std::min(ww, x + ring + 1))] - prefix[size_t(std::max(0, x - ring))] > 0;
    }
    for (int x = 0; x < ww; x++) {
        prefix[0] = 0;
        for (int y = 0; y < wh; y++) prefix[size_t(y) + 1] = prefix[size_t(y)] + near[size_t(y) * ww + size_t(x)];
        for (int y = 0; y < wh; y++) {
            uint8_t& r = role[size_t(y) * ww + size_t(x)];
            if (r == Outside && prefix[size_t(std::min(wh, y + ring + 1))] - prefix[size_t(std::max(0, y - ring))] > 0) r = Ring;
        }
    }
    // Hidden pixels are no part of the ring: they neither set the tone nor score a candidate patch.
    if (visible)
        for (int y = 0; y < wh; y++)
            for (int x = 0; x < ww; x++) {
                uint8_t& r = role[size_t(y) * ww + size_t(x)];
                if (r == Ring && visible->at(wx0 + x, wy0 + y) < 128) r = Outside;
            }
    long ringCount = 0;
    for (uint8_t r : role) ringCount += r == Ring;
    if (!ringCount) return false;

    // Source patch for Proximity Match (and Content-Aware when there was nothing to synthesise from): 24
    // directions at a few distances, the nearer winning ties, then a fine alignment so repeating texture lines up.
    int ox = 0, oy = 0;
    bool haveSource = false;
    if (mode != 1) {
        // The reference's ring of candidates (24 directions at a few distances, nearer ones favoured), plus
        // random offsets within reach, scored in parallel; then a random search that closes in on the best.
        static const double factors[5] = {1.05, 1.35, 1.75, 2.25, 2.8};
        const int count = mode == 2 ? 2 : 5, fixed = count * 24, random = 96;
        struct Candidate { double score; int dx, dy; };
        std::vector<Candidate> candidates(size_t(fixed + random));
        const int reachX = int(factors[size_t(count) - 1] * ww) + ww, reachY = int(factors[size_t(count) - 1] * wh) + wh;
        uint32_t rng = hash32(seed ^ 0x9e3779b9u) | 1u;
        for (int i = fixed; i < fixed + random; i++) {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            const int dx = int(rng % unsigned(2 * reachX + 1)) - reachX;
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            const int dy = int(rng % unsigned(2 * reachY + 1)) - reachY;
            candidates[size_t(i)] = {INFINITY, dx, dy};
        }
        auto scored = [&](int dx, int dy) {
            double score = ringScore(image, role, wx0, wy0, ww, wh, dx, dy, visible);
            if (!std::isfinite(score)) return score;
            // Nearer patches win ties, more so for Proximity Match.
            const double distance = std::hypot(double(dx) / ww, double(dy) / wh);
            return score * (1 + (mode == 2 ? 0.35 : 0.06) * std::max(0.0, distance - 1));
        };
        parallelRows(0, fixed + random, [&](int i0, int i1) {
            for (int i = i0; i < i1; i++) {
                Candidate& c = candidates[size_t(i)];
                if (i < fixed) {
                    const int f = i / 24, a = i % 24;
                    const double angle = a * M_PI / 12.0;
                    c.dx = int(std::lround(std::cos(angle) * factors[f] * ww)); c.dy = int(std::lround(std::sin(angle) * factors[f] * wh));
                }
                c.score = scored(c.dx, c.dy);
            }
        }, 1);
        double best = INFINITY;
        for (const Candidate& c : candidates) if (c.score < best) { best = c.score; ox = c.dx; oy = c.dy; }
        for (int radius = std::max(reachX, reachY) / 2; radius >= 4 && std::isfinite(best); radius /= 2)
            for (int k = 0; k < 4; k++) {
                rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
                const int dx = ox + int(rng % unsigned(2 * radius + 1)) - radius;
                rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
                const int dy = oy + int(rng % unsigned(2 * radius + 1)) - radius;
                const double score = scored(dx, dy);
                if (score < best) { best = score; ox = dx; oy = dy; }
            }
        if (std::isfinite(best)) {
            const int cx = ox, cy = oy;
            double refined = ringScore(image, role, wx0, wy0, ww, wh, cx, cy, visible);
            for (int j = -3; j <= 3; j++)
                for (int i = -3; i <= 3; i++) {
                    double score = ringScore(image, role, wx0, wy0, ww, wh, cx + i, cy + j, visible);
                    if (score < refined) { refined = score; ox = cx + i; oy = cy + j; }
                }
            haveSource = true;
        }
    }
    plan.wx0 = wx0; plan.wy0 = wy0; plan.ww = ww; plan.wh = wh;
    plan.ringCount = ringCount;
    plan.ox = ox; plan.oy = oy;
    plan.haveSource = haveSource;
    return true;
}

} // namespace

void spotHeal(Image& image, const GrayImage& coverage, float opacity, int mode, uint32_t seed, const GrayImage* visible) {
    const int W = image.width(), H = image.height();
    if (visible && (visible->width() != W || visible->height() != H)) visible = nullptr;
    PixelBounds b = nonzeroBounds(coverage);
    if (b.isEmpty()) return;
    if (mode == 0 && healBySynthesis(image, coverage, b, opacity, seed, visible)) return;
    SpotPlan plan;
    if (!planSpot(image, coverage, b, mode, seed, visible, plan)) return;
    const int wx0 = plan.wx0, wy0 = plan.wy0, ww = plan.ww, wh = plan.wh;
    const size_t wn = size_t(ww) * wh;
    const std::vector<uint8_t>& role = plan.role;
    const long ringCount = plan.ringCount;
    const int ox = plan.ox, oy = plan.oy;
    const bool haveSource = plan.haveSource;

    // Membrane: the difference between the original and the patch along the ring (or the original itself
    // for a smooth fill), spread across the spot.
    std::vector<float> value(wn * 4, 0.0f);
    std::vector<uint8_t> hole(wn), known(wn);
    double detail[3] = {0, 0, 0};
    for (int y = 0; y < wh; y++)
        for (int x = 0; x < ww; x++) {
            const size_t p = size_t(y) * ww + size_t(x);
            hole[p] = role[p] == Hole; known[p] = role[p] == Ring;
            if (role[p] != Ring) continue;
            const int ix = wx0 + x, iy = wy0 + y;
            const uint8_t* t = image.pixel(ix, iy);
            const uint8_t* s = haveSource ? image.pixel(ix + ox, iy + oy) : nullptr;
            for (int c = 0; c < 4; c++) value[p * 4 + size_t(c)] = float(t[c]) - (s ? s[c] : 0);
            if (!haveSource) {
                // Fine detail around the spot: each pixel against the average of its neighbours.
                for (int c = 0; c < 3; c++) {
                    double around = 0; int n = 0;
                    const int offsets[4][2] = {{ix - 1, iy}, {ix + 1, iy}, {ix, iy - 1}, {ix, iy + 1}};
                    for (auto& o : offsets) {
                        if (o[0] < 0 || o[1] < 0 || o[0] >= W || o[1] >= H) continue;
                        around += image.pixel(o[0], o[1])[c];
                        n++;
                    }
                    if (n) { double d = t[c] - around / n; detail[c] += d * d; }
                }
            }
        }
    membraneFill(value.data(), 4, hole.data(), known.data(), ww, wh);
    for (int c = 0; c < 3; c++) detail[c] = std::sqrt(detail[c] / double(ringCount)) * 0.9;

    for (int y = 0; y < wh; y++)
        for (int x = 0; x < ww; x++) {
            const size_t p = size_t(y) * ww + size_t(x);
            if (role[p] != Hole) continue;
            const int ix = wx0 + x, iy = wy0 + y;
            uint8_t* t = image.pixel(ix, iy);
            const uint8_t* s = haveSource ? image.pixel(ix + ox, iy + oy) : nullptr;
            const double amount = coverage.at(ix, iy) / 255.0 * opacity;
            double grain = 0;
            if (!haveSource) {
                const uint32_t key = hash32(seed ^ hash32(uint32_t(long(iy) * W + ix)));
                const double u1 = unitRandom(key), u2 = unitRandom(key ^ 0x68e31da4U);
                grain = std::sqrt(-2.0 * std::log(1.0 - u1)) * std::cos(2.0 * M_PI * u2);
            }
            double out[4];
            for (int c = 0; c < 4; c++) {
                const double healed = (s ? s[c] : 0) + value[p * 4 + size_t(c)] + (c < 3 ? grain * detail[c] : 0);
                out[c] = t[c] + (healed - t[c]) * amount;
            }
            t[3] = uint8_t(std::lround(std::clamp(out[3], 0.0, 255.0)));
            for (int c = 0; c < 3; c++) t[c] = uint8_t(std::lround(std::clamp(out[c], 0.0, double(t[3]))));
        }
}

void healFrom(Image& image, const Image& source, const GrayImage& coverage, float opacity, const GrayImage* visible) {
    const int W = image.width(), H = image.height();
    if (source.width() != W || source.height() != H || coverage.width() != W || coverage.height() != H) return;
    int x0 = W, y0 = H, x1 = -1, y1 = -1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (coverage.at(x, y)) { x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); }
    if (x1 < 0) return;
    // The window: the hole and a ring of one pixel around it.
    const int wx0 = std::max(0, x0 - 1), wy0 = std::max(0, y0 - 1), wx1 = std::min(W - 1, x1 + 1), wy1 = std::min(H - 1, y1 + 1);
    const int ww = wx1 - wx0 + 1, wh = wy1 - wy0 + 1;
    const size_t wn = size_t(ww) * size_t(wh);
    auto shown = [&](int x, int y) { return !visible || visible->at(x, y) >= 128; };
    std::vector<uint8_t> hole(wn), known(wn);
    for (int y = 0; y < wh; y++)
        for (int x = 0; x < ww; x++) hole[size_t(y) * ww + size_t(x)] = coverage.at(wx0 + x, wy0 + y) ? 1 : 0;
    std::vector<float> value(wn * 4, 0.0f);
    bool anyKnown = false;
    for (int y = 0; y < wh; y++)
        for (int x = 0; x < ww; x++) {
            const size_t p = size_t(y) * ww + size_t(x);
            if (hole[p]) continue;
            bool touches = false;
            const int n[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
            for (auto& o : n) if (o[0] >= 0 && o[1] >= 0 && o[0] < ww && o[1] < wh && hole[size_t(o[1]) * ww + size_t(o[0])]) touches = true;
            if (!touches || !shown(wx0 + x, wy0 + y)) continue;
            known[p] = 1;
            anyKnown = true;
            const uint8_t* t = image.pixel(wx0 + x, wy0 + y);
            const uint8_t* s = source.pixel(wx0 + x, wy0 + y);
            for (int c = 0; c < 4; c++) value[p * 4 + size_t(c)] = float(t[c]) - float(s[c]);
        }
    // No edge to match (the hole covers the image): the source goes in as it is.
    if (anyKnown) membraneFill(value.data(), 4, hole.data(), known.data(), ww, wh);
    parallelRows(0, wh, [&](int r0, int r1) {
        for (int y = r0; y < r1; y++)
            for (int x = 0; x < ww; x++) {
                const size_t p = size_t(y) * ww + size_t(x);
                if (!hole[p]) continue;
                const int ix = wx0 + x, iy = wy0 + y;
                uint8_t* t = image.pixel(ix, iy);
                const uint8_t* s = source.pixel(ix, iy);
                const double amount = coverage.at(ix, iy) / 255.0 * opacity;
                double out[4];
                for (int c = 0; c < 4; c++) out[c] = t[c] + (s[c] + value[p * 4 + size_t(c)] - t[c]) * amount;
                const double a = std::clamp(out[3], 0.0, 255.0);
                t[3] = uint8_t(std::lround(a));
                for (int c = 0; c < 3; c++) t[c] = uint8_t(std::lround(std::clamp(out[c], 0.0, double(t[3]))));
            }
    });
}

// ---- 16 bits ---------------------------------------------------------------------------------------------

namespace {

constexpr double one15 = 32768.0;
/// A layer mask shows a pixel from half up: 128 of 255 at 8 bits, the same level widened at 16.
constexpr uint16_t shownFrom16 = widen8(128);
/// Coverage under half an 8-bit level (the faint outer rim of a soft tip) is no part of the spot: at 8 bits it
/// rounds to nothing, and letting it in would move the ring the membrane is taken from by a pixel.
constexpr uint16_t spotFrom16 = 65;

/// `coverage` with what is below spotFrom16 cleared (shared when nothing is).
std::shared_ptr<const Gray16> spotCoverage(const Gray16& coverage) {
    bool faint = false;
    for (int y = 0; y < coverage.height() && !faint; y++)
        for (int x = 0; x < coverage.width(); x++) {
            const uint16_t v = coverage.at(x, y);
            if (v && v < spotFrom16) { faint = true; break; }
        }
    if (!faint) return std::shared_ptr<const Gray16>(std::shared_ptr<const Gray16>(), &coverage);
    auto out = std::make_shared<Gray16>(coverage);
    for (int y = 0; y < out->height(); y++)
        for (int x = 0; x < out->width(); x++)
            if (out->at(x, y) < spotFrom16) out->at(x, y) = 0;
    return out;
}

/// The healed value `out` (premultiplied, 0..32768 scale) stored with colour kept within alpha.
inline void store16(uint16_t* t, const double out[4]) {
    t[3] = uint16_t(std::lround(std::clamp(out[3], 0.0, one15)));
    for (int c = 0; c < 3; c++) t[c] = uint16_t(std::lround(std::clamp(out[c], 0.0, double(t[3]))));
}

bool healBySynthesis16(Image16& image, const Gray16& coverage, const PixelBounds& b, float opacity, uint32_t seed, const Gray16* visible) {
    const int W = image.width(), H = image.height(), band = 2;
    Gray16 grown(W, H, 0);
    for (int y = std::max(0, b.y0 - band); y < std::min(H, b.y1 + band); y++)
        for (int x = std::max(0, b.x0 - band); x < std::min(W, b.x1 + band); x++) {
            bool near = false;
            for (int j = -band; j <= band && !near; j++)
                for (int i = -band; i <= band && !near; i++) {
                    const int sx = x + i, sy = y + j;
                    near = sx >= 0 && sy >= 0 && sx < W && sy < H && coverage.at(sx, sy) != 0;
                }
            grown.at(x, y) = near ? uint16_t(one16) : 0;
        }
    Image16 synthesised = image;
    InpaintOptions options;
    options.seed = seed;
    if (!contentFill(synthesised, grown, options, visible)) return false;
    const int x0 = std::max(0, b.x0 - band - 1), y0 = std::max(0, b.y0 - band - 1);
    const int x1 = std::min(W, b.x1 + band + 1), y1 = std::min(H, b.y1 + band + 1);
    const int ww = x1 - x0, wh = y1 - y0;
    std::vector<float> values(size_t(ww) * wh * 4, 0.0f);
    std::vector<uint8_t> hole(size_t(ww) * wh, 0), known(size_t(ww) * wh, 0);
    for (int y = 0; y < wh; y++)
        for (int x = 0; x < ww; x++) {
            const size_t p = size_t(y) * ww + size_t(x);
            const int ix = x0 + x, iy = y0 + y;
            if (coverage.at(ix, iy)) { hole[p] = 1; continue; }
            if (!grown.at(ix, iy) || image.pixel(ix, iy)[3] < one16 || (visible && visible->at(ix, iy) < shownFrom16)) continue;
            known[p] = 1;
            for (int c = 0; c < 4; c++) values[p * 4 + size_t(c)] = float(image.pixel(ix, iy)[c]) - float(synthesised.pixel(ix, iy)[c]);
        }
    membraneFill(values.data(), 4, hole.data(), known.data(), ww, wh);
    for (int y = 0; y < wh; y++)
        for (int x = 0; x < ww; x++) {
            const size_t p = size_t(y) * ww + size_t(x);
            if (!hole[p]) continue;
            const int ix = x0 + x, iy = y0 + y;
            uint16_t* t = image.pixel(ix, iy);
            const uint16_t* s = synthesised.pixel(ix, iy);
            const double amount = std::min<uint32_t>(coverage.at(ix, iy), one16) / one15 * opacity;
            double out[4];
            for (int c = 0; c < 4; c++) out[c] = t[c] + (s[c] + values[p * 4 + size_t(c)] - t[c]) * amount;
            store16(t, out);
        }
    return true;
}

} // namespace

namespace {
template <SampleType S> void spotHealInks(ImageT<S>& image, const GrayOf<S>& painted, float opacity, int mode, uint32_t seed, const GrayOf<S>* visible);
template <SampleType S> void healFromInks(ImageT<S>& image, const ImageT<S>& source, const GrayOf<S>& painted, float opacity, const GrayOf<S>* visible);
} // namespace

void spotHeal(Image16& image, const Gray16& painted, float opacity, int mode, uint32_t seed, const Gray16* visible) {
    if (image.channels() == 5) { spotHealInks<SampleType::U16>(image, painted, opacity, mode, seed, visible); return; }
    const int W = image.width(), H = image.height();
    if (painted.width() != W || painted.height() != H) return;
    const std::shared_ptr<const Gray16> held = spotCoverage(painted);
    const Gray16& coverage = *held;
    if (visible && (visible->width() != W || visible->height() != H)) visible = nullptr;
    const PixelBounds b = nonzeroBounds(coverage);
    if (b.isEmpty()) return;
    if (mode == 0 && healBySynthesis16(image, coverage, b, opacity, seed, visible)) return;
    // The plan (the ring, and the patch to copy) on the pixels rounded to 8 bits, the spot being every touched pixel.
    const std::shared_ptr<Image> eight = narrowImage(image);
    GrayImage spot(W, H, 0);
    for (int y = b.y0; y < b.y1; y++)
        for (int x = b.x0; x < b.x1; x++) spot.at(x, y) = coverage.at(x, y) ? 255 : 0;
    std::shared_ptr<GrayImage> visible8 = visible ? narrowGray(*visible) : nullptr;
    SpotPlan plan;
    if (!planSpot(*eight, spot, b, mode, seed, visible8.get(), plan)) return;
    const int wx0 = plan.wx0, wy0 = plan.wy0, ww = plan.ww, wh = plan.wh;
    const size_t wn = size_t(ww) * wh;
    const std::vector<uint8_t>& role = plan.role;
    const int ox = plan.ox, oy = plan.oy;
    const bool haveSource = plan.haveSource;

    // The membrane and the grain at 16 bits, as spotHeal does at 8.
    std::vector<float> value(wn * 4, 0.0f);
    std::vector<uint8_t> hole(wn), known(wn);
    double detail[3] = {0, 0, 0};
    for (int y = 0; y < wh; y++)
        for (int x = 0; x < ww; x++) {
            const size_t p = size_t(y) * ww + size_t(x);
            hole[p] = role[p] == Hole; known[p] = role[p] == Ring;
            if (role[p] != Ring) continue;
            const int ix = wx0 + x, iy = wy0 + y;
            const uint16_t* t = image.pixel(ix, iy);
            const uint16_t* s = haveSource ? image.pixel(ix + ox, iy + oy) : nullptr;
            for (int c = 0; c < 4; c++) value[p * 4 + size_t(c)] = float(t[c]) - (s ? float(s[c]) : 0.0f);
            if (!haveSource)
                for (int c = 0; c < 3; c++) {
                    double around = 0; int n = 0;
                    const int offsets[4][2] = {{ix - 1, iy}, {ix + 1, iy}, {ix, iy - 1}, {ix, iy + 1}};
                    for (auto& o : offsets) {
                        if (o[0] < 0 || o[1] < 0 || o[0] >= W || o[1] >= H) continue;
                        around += image.pixel(o[0], o[1])[c];
                        n++;
                    }
                    if (n) { const double d = t[c] - around / n; detail[c] += d * d; }
                }
        }
    membraneFill(value.data(), 4, hole.data(), known.data(), ww, wh);
    for (int c = 0; c < 3; c++) detail[c] = std::sqrt(detail[c] / double(plan.ringCount)) * 0.9;
    for (int y = 0; y < wh; y++)
        for (int x = 0; x < ww; x++) {
            const size_t p = size_t(y) * ww + size_t(x);
            if (role[p] != Hole) continue;
            const int ix = wx0 + x, iy = wy0 + y;
            uint16_t* t = image.pixel(ix, iy);
            const uint16_t* s = haveSource ? image.pixel(ix + ox, iy + oy) : nullptr;
            const double amount = std::min<uint32_t>(coverage.at(ix, iy), one16) / one15 * opacity;
            double grain = 0;
            if (!haveSource) {
                const uint32_t key = hash32(seed ^ hash32(uint32_t(long(iy) * W + ix)));
                const double u1 = unitRandom(key), u2 = unitRandom(key ^ 0x68e31da4U);
                grain = std::sqrt(-2.0 * std::log(1.0 - u1)) * std::cos(2.0 * M_PI * u2);
            }
            double out[4];
            for (int c = 0; c < 4; c++) {
                const double healed = (s ? s[c] : 0) + value[p * 4 + size_t(c)] + (c < 3 ? grain * detail[c] : 0);
                out[c] = t[c] + (healed - t[c]) * amount;
            }
            store16(t, out);
        }
}

void healFrom(Image16& image, const Image16& source, const Gray16& painted, float opacity, const Gray16* visible) {
    if (image.channels() == 5) { healFromInks<SampleType::U16>(image, source, painted, opacity, visible); return; }
    const int W = image.width(), H = image.height();
    if (source.width() != W || source.height() != H || painted.width() != W || painted.height() != H) return;
    const std::shared_ptr<const Gray16> held = spotCoverage(painted);
    const Gray16& coverage = *held;
    const PixelBounds bounds = nonzeroBounds(coverage);
    if (bounds.isEmpty()) return;
    const int wx0 = std::max(0, bounds.x0 - 1), wy0 = std::max(0, bounds.y0 - 1), wx1 = std::min(W - 1, bounds.x1), wy1 = std::min(H - 1, bounds.y1);
    const int ww = wx1 - wx0 + 1, wh = wy1 - wy0 + 1;
    const size_t wn = size_t(ww) * size_t(wh);
    auto shown = [&](int x, int y) { return !visible || visible->at(x, y) >= shownFrom16; };
    std::vector<uint8_t> hole(wn), known(wn);
    for (int y = 0; y < wh; y++)
        for (int x = 0; x < ww; x++) hole[size_t(y) * ww + size_t(x)] = coverage.at(wx0 + x, wy0 + y) ? 1 : 0;
    std::vector<float> value(wn * 4, 0.0f);
    bool anyKnown = false;
    for (int y = 0; y < wh; y++)
        for (int x = 0; x < ww; x++) {
            const size_t p = size_t(y) * ww + size_t(x);
            if (hole[p]) continue;
            bool touches = false;
            const int n[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
            for (auto& o : n) if (o[0] >= 0 && o[1] >= 0 && o[0] < ww && o[1] < wh && hole[size_t(o[1]) * ww + size_t(o[0])]) touches = true;
            if (!touches || !shown(wx0 + x, wy0 + y)) continue;
            known[p] = 1;
            anyKnown = true;
            const uint16_t* t = image.pixel(wx0 + x, wy0 + y);
            const uint16_t* s = source.pixel(wx0 + x, wy0 + y);
            for (int c = 0; c < 4; c++) value[p * 4 + size_t(c)] = float(t[c]) - float(s[c]);
        }
    if (anyKnown) membraneFill(value.data(), 4, hole.data(), known.data(), ww, wh);
    parallelRows(0, wh, [&](int r0, int r1) {
        for (int y = r0; y < r1; y++)
            for (int x = 0; x < ww; x++) {
                const size_t p = size_t(y) * ww + size_t(x);
                if (!hole[p]) continue;
                const int ix = wx0 + x, iy = wy0 + y;
                uint16_t* t = image.pixel(ix, iy);
                const uint16_t* s = source.pixel(ix, iy);
                const double amount = std::min<uint32_t>(coverage.at(ix, iy), one16) / one15 * opacity;
                double out[4];
                for (int c = 0; c < 4; c++) out[c] = t[c] + (s[c] + value[p * 4 + size_t(c)] - t[c]) * amount;
                store16(t, out);
            }
    });
}

// ---- CMYK: five samples (docs/color-modes.md, "Painting") -----------------------------------------------------
// The inks as stored (inverted, premultiplied) with alpha last. Where the spot heals from is decided on an 8-bit proxy
// of the plates' look (each of C, M and Y as stored times K as stored: the plates' brightness overprinted); the
// membrane, the grain and the copied samples are the five native ones. Content-Aware synthesis works on RGBA, so in
// CMYK the Content-Aware type copies the best-matching nearby patch, searching as widely as it does in RGB.

namespace {

template <SampleType S> constexpr double inkOne() { return double(SampleTraits<S>::one); }

template <SampleType S>
void storeInks(SampleOf<S>* t, const double out[5]) {
    const double one = inkOne<S>();
    t[4] = SampleOf<S>(std::lround(std::clamp(out[4], 0.0, one)));
    for (int c = 0; c < 4; c++) t[c] = SampleOf<S>(std::lround(std::clamp(out[c], 0.0, double(t[4]))));
}

template <SampleType S>
std::shared_ptr<Image> inkProxy(const ImageT<S>& image) {
    const double one = inkOne<S>();
    auto out = std::make_shared<Image>(image.width(), image.height());
    for (int y = 0; y < image.height(); y++)
        for (int x = 0; x < image.width(); x++) {
            const SampleOf<S>* p = image.pixel(x, y);
            uint8_t* o = out->pixel(x, y);
            const double a = std::min(double(p[4]), one);
            const double k = a > 0 ? std::min(1.0, p[3] / a) : 0;
            for (int c = 0; c < 3; c++) o[c] = uint8_t(std::lround(std::clamp(p[c] * k / one, 0.0, a / one) * 255));
            o[3] = uint8_t(std::lround(a / one * 255));
        }
    return out;
}

template <SampleType S>
std::shared_ptr<GrayImage> grayAt8(const GrayOf<S>& gray, bool spot) {
    auto out = std::make_shared<GrayImage>(gray.width(), gray.height(), 0);
    for (int y = 0; y < gray.height(); y++)
        for (int x = 0; x < gray.width(); x++) {
            const double v = double(gray.at(x, y));
            out->at(x, y) = spot ? (v ? 255 : 0) : uint8_t(std::lround(std::min(1.0, v / inkOne<S>()) * 255));
        }
    return out;
}

template <SampleType S>
std::shared_ptr<const GrayOf<S>> healCoverage(const GrayOf<S>& painted) {
    if constexpr (S == SampleType::U16) return spotCoverage(painted);
    else return std::shared_ptr<const GrayOf<S>>(std::shared_ptr<const GrayOf<S>>(), &painted);
}

template <SampleType S>
void spotHealInks(ImageT<S>& image, const GrayOf<S>& painted, float opacity, int mode, uint32_t seed, const GrayOf<S>* visible) {
    constexpr int N = 5;
    const double one = inkOne<S>();
    const int W = image.width(), H = image.height();
    if (painted.width() != W || painted.height() != H) return;
    const auto held = healCoverage<S>(painted);
    const GrayOf<S>& coverage = *held;
    if (visible && (visible->width() != W || visible->height() != H)) visible = nullptr;
    const PixelBounds b = nonzeroBounds(coverage);
    if (b.isEmpty()) return;
    const std::shared_ptr<Image> eight = inkProxy<S>(image);
    const std::shared_ptr<GrayImage> spot = grayAt8<S>(coverage, true);
    const std::shared_ptr<GrayImage> visible8 = visible ? grayAt8<S>(*visible, false) : nullptr;
    SpotPlan plan;
    if (!planSpot(*eight, *spot, b, mode, seed, visible8.get(), plan)) return;
    const int wx0 = plan.wx0, wy0 = plan.wy0, ww = plan.ww, wh = plan.wh;
    const size_t wn = size_t(ww) * wh;
    const std::vector<uint8_t>& role = plan.role;
    const int ox = plan.ox, oy = plan.oy;
    const bool haveSource = plan.haveSource;
    std::vector<float> value(wn * N, 0.0f);
    std::vector<uint8_t> hole(wn), known(wn);
    double detail[N - 1] = {0, 0, 0, 0};
    for (int y = 0; y < wh; y++)
        for (int x = 0; x < ww; x++) {
            const size_t p = size_t(y) * ww + size_t(x);
            hole[p] = role[p] == Hole; known[p] = role[p] == Ring;
            if (role[p] != Ring) continue;
            const int ix = wx0 + x, iy = wy0 + y;
            const SampleOf<S>* t = image.pixel(ix, iy);
            const SampleOf<S>* s = haveSource ? image.pixel(ix + ox, iy + oy) : nullptr;
            for (int c = 0; c < N; c++) value[p * N + size_t(c)] = float(t[c]) - (s ? float(s[c]) : 0.0f);
            if (!haveSource)
                for (int c = 0; c < N - 1; c++) {
                    double around = 0; int n = 0;
                    const int offsets[4][2] = {{ix - 1, iy}, {ix + 1, iy}, {ix, iy - 1}, {ix, iy + 1}};
                    for (auto& o : offsets) {
                        if (o[0] < 0 || o[1] < 0 || o[0] >= W || o[1] >= H) continue;
                        around += image.pixel(o[0], o[1])[c];
                        n++;
                    }
                    if (n) { const double d = t[c] - around / n; detail[c] += d * d; }
                }
        }
    membraneFill(value.data(), N, hole.data(), known.data(), ww, wh);
    for (int c = 0; c < N - 1; c++) detail[c] = std::sqrt(detail[c] / double(plan.ringCount)) * 0.9;
    for (int y = 0; y < wh; y++)
        for (int x = 0; x < ww; x++) {
            const size_t p = size_t(y) * ww + size_t(x);
            if (role[p] != Hole) continue;
            const int ix = wx0 + x, iy = wy0 + y;
            SampleOf<S>* t = image.pixel(ix, iy);
            const SampleOf<S>* s = haveSource ? image.pixel(ix + ox, iy + oy) : nullptr;
            const double amount = std::min(double(coverage.at(ix, iy)), one) / one * opacity;
            double grain = 0;
            if (!haveSource) {
                const uint32_t key = hash32(seed ^ hash32(uint32_t(long(iy) * W + ix)));
                const double u1 = unitRandom(key), u2 = unitRandom(key ^ 0x68e31da4U);
                grain = std::sqrt(-2.0 * std::log(1.0 - u1)) * std::cos(2.0 * M_PI * u2);
            }
            double out[N];
            for (int c = 0; c < N; c++) {
                const double healed = (s ? double(s[c]) : 0) + value[p * N + size_t(c)] + (c < N - 1 ? grain * detail[c] : 0);
                out[c] = t[c] + (healed - t[c]) * amount;
            }
            storeInks<S>(t, out);
        }
}

template <SampleType S>
void healFromInks(ImageT<S>& image, const ImageT<S>& source, const GrayOf<S>& painted, float opacity, const GrayOf<S>* visible) {
    constexpr int N = 5;
    const double one = inkOne<S>();
    const int W = image.width(), H = image.height();
    if (source.width() != W || source.height() != H || source.channels() != N || painted.width() != W || painted.height() != H) return;
    const auto held = healCoverage<S>(painted);
    const GrayOf<S>& coverage = *held;
    const PixelBounds bounds = nonzeroBounds(coverage);
    if (bounds.isEmpty()) return;
    const int wx0 = std::max(0, bounds.x0 - 1), wy0 = std::max(0, bounds.y0 - 1), wx1 = std::min(W - 1, bounds.x1), wy1 = std::min(H - 1, bounds.y1);
    const int ww = wx1 - wx0 + 1, wh = wy1 - wy0 + 1;
    const size_t wn = size_t(ww) * size_t(wh);
    const double shownFrom = S == SampleType::U16 ? double(shownFrom16) : 128.0;
    auto shown = [&](int x, int y) { return !visible || double(visible->at(x, y)) >= shownFrom; };
    std::vector<uint8_t> hole(wn), known(wn);
    for (int y = 0; y < wh; y++)
        for (int x = 0; x < ww; x++) hole[size_t(y) * ww + size_t(x)] = coverage.at(wx0 + x, wy0 + y) ? 1 : 0;
    std::vector<float> value(wn * N, 0.0f);
    bool anyKnown = false;
    for (int y = 0; y < wh; y++)
        for (int x = 0; x < ww; x++) {
            const size_t p = size_t(y) * ww + size_t(x);
            if (hole[p]) continue;
            bool touches = false;
            const int n[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
            for (auto& o : n) if (o[0] >= 0 && o[1] >= 0 && o[0] < ww && o[1] < wh && hole[size_t(o[1]) * ww + size_t(o[0])]) touches = true;
            if (!touches || !shown(wx0 + x, wy0 + y)) continue;
            known[p] = 1;
            anyKnown = true;
            const SampleOf<S>* t = image.pixel(wx0 + x, wy0 + y);
            const SampleOf<S>* s = source.pixel(wx0 + x, wy0 + y);
            for (int c = 0; c < N; c++) value[p * N + size_t(c)] = float(t[c]) - float(s[c]);
        }
    if (anyKnown) membraneFill(value.data(), N, hole.data(), known.data(), ww, wh);
    parallelRows(0, wh, [&](int r0, int r1) {
        for (int y = r0; y < r1; y++)
            for (int x = 0; x < ww; x++) {
                const size_t p = size_t(y) * ww + size_t(x);
                if (!hole[p]) continue;
                const int ix = wx0 + x, iy = wy0 + y;
                SampleOf<S>* t = image.pixel(ix, iy);
                const SampleOf<S>* s = source.pixel(ix, iy);
                const double amount = std::min(double(coverage.at(ix, iy)), one) / one * opacity;
                double out[N];
                for (int c = 0; c < N; c++) out[c] = t[c] + (s[c] + value[p * N + size_t(c)] - t[c]) * amount;
                storeInks<S>(t, out);
            }
    });
}

} // namespace

void spotHeal(ImageC8& image, const GrayImage& coverage, float opacity, int mode, uint32_t seed, const GrayImage* visible) {
    if (image.channels() == 5) spotHealInks<SampleType::U8>(image, coverage, opacity, mode, seed, visible);
}
void healFrom(ImageC8& image, const ImageC8& source, const GrayImage& coverage, float opacity, const GrayImage* visible) {
    if (image.channels() == 5) healFromInks<SampleType::U8>(image, source, coverage, opacity, visible);
}

} // namespace compositor
