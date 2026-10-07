// Filters ported from PhotoCraft (https://github.com/storytold/photocraft, crates/algo/src/distort.rs, distort2.rs,
// other.rs, stylize.rs and crates/engine/src/extra_cmds.rs at commit b07a2e5).
// Copyright (c) 2026 ArtCraft Team and the PhotoCraft contributors. MIT License (LICENSES/PhotoCraft-MIT.txt).
//
// Distortions are inverse mappings with an alpha-weighted bilinear sample, centred on the reference bounds; Minimum and
// Maximum take the van Herk / Gil-Werman running extreme; Find Edges is a Sobel magnitude; Clouds is hashed value noise
// summed over octaves. Rows run in parallel; each pixel depends on the source alone, so the result does not depend on
// the thread count.
#include "filters_photocraft.h"
#include "compositor/parallel.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace compositor::photocraft {

namespace {

constexpr float pi = 3.14159265358979323846f;
constexpr float tau = 2.0f * pi;

int euclidMod(int a, int m) { const int r = a % m; return r < 0 ? r + m : r; }
float euclidModF(float a, float m) { const float r = std::fmod(a, m); return r < 0 ? r + m : r; }

/// PhotoCraft's remap: every pixel of `bounds` reads `src` at f(centre).
template <class F>
void remap(const Plane& src, Plane& out, const Rect& bounds, Edge edge, F&& f) {
    const int n = src.channels;
    parallelRows(bounds.y0, bounds.y1, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++)
            for (int x = bounds.x0; x < bounds.x1; x++) {
                float sx = 0, sy = 0;
                f(float(x) + 0.5f, float(y) + 0.5f, sx, sy);
                src.sample(sx, sy, edge, bounds, out.at(x, y));
            }
    }, 8);
    (void)n;
}

void centre(const Rect& b, float& cx, float& cy, float& rr) {
    const float w = float(b.width()), h = float(b.height());
    cx = float(b.x0) + w / 2.0f;
    cy = float(b.y0) + h / 2.0f;
    rr = std::min(w, h) / 2.0f;
}

float sphere1d(float rn, float a) {
    // rn in [0, 1): a positive amount magnifies the centre (samples closer in).
    const float target = a >= 0.0f ? std::asin(rn) * 2.0f / pi : std::sin(rn * pi / 2.0f);
    return rn + std::fabs(a) * (target - rn);
}

float signum(float v) { return v > 0 ? 1.0f : v < 0 ? -1.0f : 0.0f; }

uint32_t hashMix(uint32_t h) {
    h ^= h >> 16; h *= 0x7feb352dU;
    h ^= h >> 15; h *= 0x846ca68bU;
    h ^= h >> 16;
    return h;
}

/// PhotoCraft's noise::hash01.
float hash01(int32_t x, int32_t y, uint32_t c, uint32_t seed) {
    const uint32_t h = hashMix(uint32_t(x) * 0x8da6b343U ^ uint32_t(y) * 0xd8163841U ^ c * 0xcb1ab31fU ^ seed * 0x9e3779b9U);
    return float(h >> 8) / float(1u << 24);
}

/// extra_cmds.rs's hash for Clouds.
float cloudHash(int32_t x, int32_t y, uint32_t seed) {
    uint32_t h = uint32_t(x) * 0x8da6b343U ^ uint32_t(y) * 0xd8163841U ^ seed * 0xcb1ab31fU;
    h ^= h >> 13;
    h *= 0x5bd1e995U;
    h ^= h >> 15;
    return float(h & 0x00ffffffU) / float(0x00ffffff);
}

float waveShape(WaveType t, float phase) {
    const float p = euclidModF(phase, tau) / tau;
    switch (t) {
    case WaveType::Sine: return std::sin(phase);
    case WaveType::Triangle: return 1.0f - 4.0f * std::fabs(p - 0.5f);
    case WaveType::Square: return p < 0.5f ? 1.0f : -1.0f;
    }
    return 0.0f;
}

/// The extreme of every full window of `k` in `v` (van Herk / Gil-Werman).
void runningExtreme(const std::vector<float>& v, size_t k, bool max, std::vector<float>& out, std::vector<float>& pre, std::vector<float>& suf) {
    out.clear();
    const size_t n = v.size();
    if (k <= 1) { out = v; return; }
    if (n < k) return;
    auto pick = [max](float a, float b) { return max ? std::max(a, b) : std::min(a, b); };
    pre = v;
    suf = v;
    for (size_t i = 1; i < n; i++) if (i % k != 0) pre[i] = pick(pre[i - 1], v[i]);
    for (size_t i = n - 1; i-- > 0;) if ((i + 1) % k != 0) suf[i] = pick(suf[i + 1], v[i]);
    out.resize(n - k + 1);
    for (size_t i = 0; i + k <= n; i++) out[i] = pick(suf[i], pre[i + k - 1]);
}

/// Catmull-Rom through sorted points (clamped ends): distort2.rs's curve_at.
float curveAt(const std::vector<std::array<float, 2>>& pts, float t) {
    if (pts.empty()) return 0;
    if (pts.size() == 1) return pts[0][1];
    if (t <= pts[0][0]) return pts[0][1];
    const size_t last = pts.size() - 1;
    if (t >= pts[last][0]) return pts[last][1];
    size_t i = 0;
    for (size_t j = 0; j + 1 < pts.size(); j++) if (t >= pts[j][0] && t <= pts[j + 1][0]) { i = j; break; }
    const auto p1 = pts[i], p2 = pts[i + 1];
    const auto p0 = i > 0 ? pts[i - 1] : p1;
    const auto p3 = i + 2 <= last ? pts[i + 2] : p2;
    const float span = std::max(p2[0] - p1[0], 1e-6f);
    const float s = (t - p1[0]) / span;
    const float m1 = i > 0 ? (p2[1] - p0[1]) / std::max(p2[0] - p0[0], 1e-6f) * span : p2[1] - p1[1];
    const float m2 = i + 2 <= last ? (p3[1] - p1[1]) / std::max(p3[0] - p1[0], 1e-6f) * span : p2[1] - p1[1];
    const float s2 = s * s, s3 = s2 * s;
    return (2 * s3 - 3 * s2 + 1) * p1[1] + (s3 - 2 * s2 + s) * m1 + (-2 * s3 + 3 * s2) * p2[1] + (s3 - s2) * m2;
}

} // namespace

float Plane::getEdge(int x, int y, int c, Edge edge, const Rect& area) const {
    if (area.contains(x, y)) return get(x, y, c);
    switch (edge) {
    case Edge::Transparent: return 0.0f;
    case Edge::Repeat: return get(std::clamp(x, area.x0, area.x1 - 1), std::clamp(y, area.y0, area.y1 - 1), c);
    case Edge::Wrap: {
        const int w = std::max(1, area.width()), h = std::max(1, area.height());
        return get(area.x0 + euclidMod(x - area.x0, w), area.y0 + euclidMod(y - area.y0, h), c);
    }
    }
    return 0.0f;
}

void Plane::sample(float x, float y, Edge edge, const Rect& area, float* out) const {
    const float fx = x - 0.5f, fy = y - 0.5f;
    const float fx0 = std::floor(fx), fy0 = std::floor(fy);
    const float ax = fx - fx0, ay = fy - fy0;
    // Far outside any grid (a wild mapping): transparent, before the cast could overflow.
    if (!(std::fabs(fx0) < 1e8f && std::fabs(fy0) < 1e8f)) { for (int c = 0; c < channels; c++) out[c] = 0; return; }
    const int x0 = int(fx0), y0 = int(fy0);
    const int n = channels;
    for (int c = 0; c < n; c++) out[c] = 0.0f;
    float accA = 0.0f;
    const struct { int dx, dy; float w; } taps[4] = {{0, 0, (1 - ax) * (1 - ay)}, {1, 0, ax * (1 - ay)}, {0, 1, (1 - ax) * ay}, {1, 1, ax * ay}};
    for (const auto& t : taps) {
        if (t.w <= 0.0f) continue;
        const float a = getEdge(x0 + t.dx, y0 + t.dy, n - 1, edge, area);
        for (int c = 0; c < n - 1; c++) out[c] += getEdge(x0 + t.dx, y0 + t.dy, c, edge, area) * a * t.w;
        accA += a * t.w;
    }
    if (accA > 0.0f) for (int c = 0; c < n - 1; c++) out[c] /= accA;
    out[n - 1] = accA;
}

void twirl(const Plane& src, Plane& out, const Rect& bounds, float angle) {
    float cx, cy, rr;
    centre(bounds, cx, cy, rr);
    const float a = angle * pi / 180.0f;
    remap(src, out, bounds, Edge::Repeat, [&](float x, float y, float& sx, float& sy) {
        const float dx = x - cx, dy = y - cy;
        const float r = std::sqrt(dx * dx + dy * dy);
        if (r >= rr || rr <= 0.0f) { sx = x; sy = y; return; }
        const float t = a * (1.0f - r / rr);
        const float s = std::sin(t), c = std::cos(t);
        sx = cx + dx * c - dy * s;
        sy = cy + dx * s + dy * c;
    });
}

void pinch(const Plane& src, Plane& out, const Rect& bounds, float amount) {
    float cx, cy, rr;
    centre(bounds, cx, cy, rr);
    const float a = std::clamp(amount / 100.0f, -1.0f, 1.0f);
    remap(src, out, bounds, Edge::Repeat, [&](float x, float y, float& sx, float& sy) {
        const float dx = x - cx, dy = y - cy;
        const float r = std::sqrt(dx * dx + dy * dy);
        if (r >= rr || r <= 0.0f) { sx = x; sy = y; return; }
        const float rs = rr * std::pow(r / rr, 1.0f - a * 0.5f);
        sx = cx + dx * rs / r;
        sy = cy + dy * rs / r;
    });
}

void spherize(const Plane& src, Plane& out, const Rect& bounds, float amount, SpherizeMode mode) {
    float cx, cy, rr;
    centre(bounds, cx, cy, rr);
    const float hx = float(bounds.width()) / 2.0f, hy = float(bounds.height()) / 2.0f;
    const float a = std::clamp(amount / 100.0f, -1.0f, 1.0f);
    remap(src, out, bounds, Edge::Repeat, [&](float x, float y, float& sx, float& sy) {
        const float dx = x - cx, dy = y - cy;
        sx = x;
        sy = y;
        switch (mode) {
        case SpherizeMode::Normal: {
            const float r = std::sqrt(dx * dx + dy * dy);
            if (r >= rr || r <= 0.0f) return;
            const float rs = rr * sphere1d(r / rr, a);
            sx = cx + dx * rs / r;
            sy = cy + dy * rs / r;
            break;
        }
        case SpherizeMode::HorizontalOnly:
            if (std::fabs(dx) < hx) sx = cx + signum(dx) * hx * sphere1d(std::fabs(dx) / hx, a);
            break;
        case SpherizeMode::VerticalOnly:
            if (std::fabs(dy) < hy) sy = cy + signum(dy) * hy * sphere1d(std::fabs(dy) / hy, a);
            break;
        }
    });
}

void wave(const Plane& src, Plane& out, const Rect& bounds, const WaveSpec& w) {
    const uint32_t n = std::clamp<uint32_t>(w.generators, 1, 64);
    auto lerp = [](float lo, float hi, float t) { return lo + std::max(hi - lo, 0.0f) * t; };
    struct Gen { float length, amplitude, phase1, phase2; };
    std::vector<Gen> gens;
    for (uint32_t i = 0; i < n; i++) {
        auto r = [&](uint32_t k) { return hash01(int32_t(i), int32_t(k), 7, w.seed); };
        gens.push_back({std::max(lerp(w.wavelengthMin, w.wavelengthMax, r(1)), 1.0f), lerp(w.amplitudeMin, w.amplitudeMax, r(2)), r(3) * tau, r(4) * tau});
    }
    remap(src, out, bounds, w.undefined, [&](float x, float y, float& sx, float& sy) {
        const float lx = x - float(bounds.x0), ly = y - float(bounds.y0);
        float dx = 0, dy = 0;
        for (const Gen& g : gens) {
            dx += g.amplitude * waveShape(w.type, tau * ly / g.length + g.phase1);
            dy += g.amplitude * waveShape(w.type, tau * lx / g.length + g.phase2);
        }
        sx = x + dx / float(n);
        sy = y + dy / float(n);
    });
}

void ripple(const Plane& src, Plane& out, const Rect& bounds, float amount, RippleSize size) {
    const float len = size == RippleSize::Small ? 8.0f : size == RippleSize::Medium ? 16.0f : 32.0f;
    const float amp = amount / 100.0f * len / 8.0f;
    remap(src, out, bounds, Edge::Repeat, [&](float x, float y, float& sx, float& sy) {
        const float lx = x - float(bounds.x0), ly = y - float(bounds.y0);
        sx = x + amp * std::sin(tau * ly / len);
        sy = y + amp * std::sin(tau * lx / len);
    });
}

void polar(const Plane& src, Plane& out, const Rect& bounds, PolarMode mode) {
    const float w = float(std::max(1, bounds.width())), h = float(std::max(1, bounds.height()));
    const float cx = float(bounds.x0) + w / 2.0f, cy = float(bounds.y0) + h / 2.0f;
    const float rmax = std::min(w, h) / 2.0f;
    // Photoshop leaves no hole: past the mapped disc (Rectangular to Polar's corners) the source's edge pixels repeat,
    // so the corners carry the bottom row's colours outwards along each angle and an opaque layer stays opaque.
    remap(src, out, bounds, Edge::Repeat, [&](float x, float y, float& sx, float& sy) {
        if (mode == PolarMode::RectangularToPolar) {
            // The output is polar: the angle from 12 o'clock clockwise gives the source's x, the radius its y.
            const float dx = x - cx, dy = y - cy;
            const float theta = euclidModF(std::atan2(dx, -dy), tau);
            const float r = std::sqrt(dx * dx + dy * dy);
            sx = float(bounds.x0) + theta / tau * w;
            sy = float(bounds.y0) + r / rmax * h;
        } else {
            const float theta = (x - float(bounds.x0)) / w * tau;
            const float r = (y - float(bounds.y0)) / h * rmax;
            sx = cx + r * std::sin(theta);
            sy = cy - r * std::cos(theta);
        }
    });
}

void zigzag(const Plane& src, Plane& out, const Rect& bounds, float amount, float ridges, ZigZagStyle style) {
    float cx, cy, rr;
    centre(bounds, cx, cy, rr);
    const float a = std::clamp(amount / 100.0f, -1.0f, 1.0f);
    const float k = std::max(std::clamp(ridges, 0.0f, 20.0f), 0.5f);
    remap(src, out, bounds, Edge::Repeat, [&](float x, float y, float& sx, float& sy) {
        const float dx = x - cx, dy = y - cy;
        const float r = std::sqrt(dx * dx + dy * dy);
        sx = x;
        sy = y;
        if (r >= rr || r <= 0.0f || a == 0.0f) return;
        const float rn = r / rr;
        const float wv = std::sin(tau * k * rn) * (1.0f - rn);
        float radial = 0, angular = 0;
        switch (style) {
        case ZigZagStyle::OutFromCenter: radial = a * wv * rr / (4.0f * k); break;
        case ZigZagStyle::AroundCenter: angular = a * wv * pi / (2.0f * k); break;
        case ZigZagStyle::PondRipples: radial = a * wv * rr / (8.0f * k); angular = a * wv * pi / (4.0f * k); break;
        }
        const float th = std::atan2(dy, dx) + angular;
        const float rs = std::max(r + radial, 0.0f);
        sx = cx + rs * std::cos(th);
        sy = cy + rs * std::sin(th);
    });
}

void shear(const Plane& src, Plane& out, const Rect& bounds, float amount, Edge undefined) {
    const float a = std::clamp(amount, -100.0f, 100.0f) / 100.0f;
    const std::vector<std::array<float, 2>> pts{{0.0f, 0.0f}, {0.5f, a}, {1.0f, 0.0f}};
    const float bw = float(std::max(1, bounds.width())), bh = float(std::max(1, bounds.height()));
    std::vector<float> rows(size_t(std::max(0, bounds.height())));
    for (int y = bounds.y0; y < bounds.y1; y++) rows[size_t(y - bounds.y0)] = curveAt(pts, (float(y) + 0.5f - float(bounds.y0)) / bh) * bw / 2.0f;
    remap(src, out, bounds, undefined, [&](float x, float y, float& sx, float& sy) {
        const int i = std::clamp(int(y - 0.5f) - bounds.y0, 0, int(rows.size()) - 1);
        sx = x - rows[size_t(i)];
        sy = y;
    });
}

void offset(const Plane& src, Plane& out, const Rect& bounds, int dx, int dy, Edge undefined) {
    const int n = src.channels;
    parallelRows(bounds.y0, bounds.y1, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++)
            for (int x = bounds.x0; x < bounds.x1; x++) {
                float* o = out.at(x, y);
                for (int c = 0; c < n; c++) o[c] = src.getEdge(x - dx, y - dy, c, undefined, bounds);
            }
    });
}

void minMax(const Plane& src, Plane& out, float radius, bool max, bool round) {
    const int n = src.channels, w = src.width, h = src.height;
    const float lowest = max ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
    // Samples past the grid repeat its edge pixels, so a layer filling the canvas keeps its border.
    auto get = [&](int x, int y, int c) { return src.at(std::clamp(x, 0, w - 1), std::clamp(y, 0, h - 1))[c]; };
    auto pick = [max](float a, float b) { return max ? std::max(a, b) : std::min(a, b); };
    if (w <= 0 || h <= 0) return;
    if (!round) {
        // A square of 2r + 1, separably: rows, then columns.
        const int r = int(std::lround(std::max(0.0f, radius)));
        if (r == 0) { out = src; return; }
        Plane tmp(w, h, n);
        parallelRows(0, h, [&](int y0, int y1) {
            std::vector<float> row, ext, pre, suf;
            for (int y = y0; y < y1; y++)
                for (int c = 0; c < n; c++) {
                    row.clear();
                    for (int x = -r; x < w + r; x++) row.push_back(get(x, y, c));
                    runningExtreme(row, size_t(2 * r + 1), max, ext, pre, suf);
                    for (int x = 0; x < w; x++) tmp.at(x, y)[c] = ext[size_t(x)];
                }
        });
        parallelRows(0, w, [&](int x0, int x1) {
            std::vector<float> col, ext, pre, suf;
            for (int x = x0; x < x1; x++)
                for (int c = 0; c < n; c++) {
                    col.clear();
                    for (int y = -r; y < h + r; y++) col.push_back(tmp.at(x, std::clamp(y, 0, h - 1))[c]);
                    runningExtreme(col, size_t(2 * r + 1), max, ext, pre, suf);
                    for (int y = 0; y < h; y++) out.at(x, y)[c] = ext[size_t(y)];
                }
        });
        return;
    }
    // A disc: every offset with dx² + dy² <= r², each of its rows a running window.
    const float r = std::max(0.0f, radius);
    const int ri = int(std::floor(r));
    if (ri == 0) { out = src; return; }
    parallelRows(0, h, [&](int y0, int y1) {
        std::vector<float> row, ext, pre, suf, acc;
        for (int y = y0; y < y1; y++) {
            acc.assign(size_t(w) * size_t(n), lowest);
            for (int dy = -ri; dy <= ri; dy++) {
                // The disc's half-width on this row (the epsilon keeps exact squares: r 5, dy 3 gives 4).
                const int hw = int(std::floor(std::sqrt(std::max(0.0f, r * r - float(dy * dy))) + 1e-4f));
                for (int c = 0; c < n; c++) {
                    row.clear();
                    for (int x = -hw; x < w + hw; x++) row.push_back(get(x, y + dy, c));
                    runningExtreme(row, size_t(2 * hw + 1), max, ext, pre, suf);
                    for (int x = 0; x < w; x++) {
                        float& v = acc[size_t(x) * size_t(n) + size_t(c)];
                        v = pick(v, ext[size_t(x)]);
                    }
                }
            }
            for (int x = 0; x < w; x++) std::copy_n(&acc[size_t(x) * size_t(n)], n, out.at(x, y));
        }
    }, 4);
}

void findEdges(const Plane& src, Plane& out, int colours) {
    parallelRows(0, src.height, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < src.width; x++) {
                float* o = out.at(x, y);
                for (int c = 0; c < colours; c++) {
                    auto g = [&](int dx, int dy) { return src.get(x + dx, y + dy, c); };
                    const float gx = (g(1, -1) + 2.0f * g(1, 0) + g(1, 1)) - (g(-1, -1) + 2.0f * g(-1, 0) + g(-1, 1));
                    const float gy = (g(-1, 1) + 2.0f * g(0, 1) + g(1, 1)) - (g(-1, -1) + 2.0f * g(0, -1) + g(1, -1));
                    o[c] = std::clamp(1.0f - std::sqrt(gx * gx + gy * gy) / 4.0f, 0.0f, 1.0f);
                }
            }
    });
}

float cloudsValue(float x, float y, float base, uint32_t seed) {
    float sum = 0, amp = 1, freq = 1.0f / base, norm = 0;
    for (uint32_t o = 0; o < 8; o++) {
        const float fx = x * freq, fy = y * freq;
        const float flx = std::floor(fx), fly = std::floor(fy);
        const int ix = int(flx), iy = int(fly);
        const float tx = fx - flx, ty = fy - fly;
        const float sx = tx * tx * (3.0f - 2.0f * tx), sy = ty * ty * (3.0f - 2.0f * ty);
        const uint32_t s = seed + o * 7919u;
        const float top = cloudHash(ix, iy, s) + (cloudHash(ix + 1, iy, s) - cloudHash(ix, iy, s)) * sx;
        const float bot = cloudHash(ix, iy + 1, s) + (cloudHash(ix + 1, iy + 1, s) - cloudHash(ix, iy + 1, s)) * sx;
        sum += (top + (bot - top) * sy) * amp;
        norm += amp;
        amp *= 0.5f;
        freq *= 2.0f;
    }
    // The bell-shaped sum stretched so the clouds use the whole foreground-to-background range.
    return std::clamp((sum / norm - 0.5f) * 2.2f + 0.5f, 0.0f, 1.0f);
}

float cloudsBase(int side) { return std::clamp(float(std::max(1, side)) / 4.0f, 16.0f, 512.0f); }

} // namespace compositor::photocraft
