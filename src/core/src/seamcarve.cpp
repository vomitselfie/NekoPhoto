#include "compositor/seamcarve.h"
#include "compositor/parallel.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <vector>

namespace compositor {
namespace {

// The working image: packed RGBA pixels and protection, rows of `w` live pixels in a buffer of `stride`.
struct Grid {
    int w = 0, h = 0, stride = 0;
    std::vector<uint32_t> px;
    std::vector<uint8_t> keep;
};

inline int diff(uint32_t a, uint32_t b) {
    int s = 0;
    for (int c = 0; c < 32; c += 8) s += std::abs(int((a >> c) & 255) - int((b >> c) & 255));
    return s;
}

constexpr float protectCost = 1e6f;

void energy(const Grid& g, std::vector<float>& e) {
    e.resize(size_t(g.stride) * size_t(g.h));
    parallelRows(0, g.h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            const uint32_t* r = g.px.data() + size_t(y) * size_t(g.stride);
            const uint32_t* up = g.px.data() + size_t(std::max(y - 1, 0)) * size_t(g.stride);
            const uint32_t* dn = g.px.data() + size_t(std::min(y + 1, g.h - 1)) * size_t(g.stride);
            const uint8_t* k = g.keep.empty() ? nullptr : g.keep.data() + size_t(y) * size_t(g.stride);
            float* out = e.data() + size_t(y) * size_t(g.stride);
            for (int x = 0; x < g.w; x++) {
                int l = std::max(x - 1, 0), rr = std::min(x + 1, g.w - 1);
                float v = float(diff(r[l], r[rr]) + diff(up[x], dn[x]));
                if (k && k[x]) v += protectCost;
                out[x] = v;
            }
        }
    }, 32);
}

/// Up to `count` disjoint vertical seams; seams[s][y] is the seam's column in row y.
std::vector<std::vector<int>> findSeams(const Grid& g, int count) {
    std::vector<float> e;
    energy(g, e);
    const int w = g.w, h = g.h;
    const size_t st = size_t(g.stride);
    std::vector<float> m(st * size_t(h));
    std::memcpy(m.data(), e.data(), sizeof(float) * size_t(w));
    for (int y = 1; y < h; y++) {
        const float* prev = m.data() + size_t(y - 1) * st;
        const float* er = e.data() + size_t(y) * st;
        float* cur = m.data() + size_t(y) * st;
        auto body = [&](int x0, int x1) {
            for (int x = x0; x < x1; x++) {
                float best = prev[x];
                if (x > 0) best = std::min(best, prev[x - 1]);
                if (x + 1 < w) best = std::min(best, prev[x + 1]);
                cur[x] = er[x] + best;
            }
        };
        if (w >= 8192) parallelFor(0, w, 2048, body); else body(0, w);
    }
    std::vector<int> ends(static_cast<size_t>(w));
    std::iota(ends.begin(), ends.end(), 0);
    const float* last = m.data() + size_t(h - 1) * st;
    std::sort(ends.begin(), ends.end(), [&](int a, int b) { return last[a] < last[b] || (last[a] == last[b] && a < b); });
    std::vector<uint8_t> used(st * size_t(h), 0);
    std::vector<std::vector<int>> seams;
    std::vector<int> path(static_cast<size_t>(h));
    for (int start : ends) {
        if (int(seams.size()) >= count) break;
        if (used[size_t(h - 1) * st + size_t(start)]) continue;
        int x = start;
        path[size_t(h - 1)] = x;
        bool ok = true;
        for (int y = h - 2; y >= 0; y--) {
            const float* row = m.data() + size_t(y) * st;
            const uint8_t* u = used.data() + size_t(y) * st;
            int best = -1;
            for (int dx = -1; dx <= 1; dx++) {
                int nx = x + dx;
                if (nx < 0 || nx >= w || u[nx]) continue;
                if (best < 0 || row[nx] < row[best]) best = nx;
            }
            if (best < 0) { ok = false; break; }
            x = best;
            path[size_t(y)] = x;
        }
        if (!ok) continue;
        for (int y = 0; y < h; y++) used[size_t(y) * st + size_t(path[size_t(y)])] = 1;
        seams.push_back(path);
    }
    return seams; // never empty: the lowest end is traced first, with nothing in its way
}

void removeSeams(Grid& g, const std::vector<std::vector<int>>& seams) {
    const int k = int(seams.size());
    parallelRows(0, g.h, [&](int y0, int y1) {
        std::vector<int> cols(static_cast<size_t>(k));
        for (int y = y0; y < y1; y++) {
            for (int s = 0; s < k; s++) cols[size_t(s)] = seams[size_t(s)][size_t(y)];
            std::sort(cols.begin(), cols.end());
            uint32_t* r = g.px.data() + size_t(y) * size_t(g.stride);
            uint8_t* kp = g.keep.empty() ? nullptr : g.keep.data() + size_t(y) * size_t(g.stride);
            int out = 0, c = 0;
            for (int x = 0; x < g.w; x++) {
                if (c < k && cols[size_t(c)] == x) { c++; continue; }
                r[out] = r[x];
                if (kp) kp[out] = kp[x];
                out++;
            }
        }
    }, 32);
    g.w -= k;
}

/// Each seam's pixel doubled: the copy is the average of it and its right-hand neighbour.
void insertSeams(Grid& g, const std::vector<std::vector<int>>& seams) {
    const int k = int(seams.size());
    const int nw = g.w + k;
    Grid out;
    out.w = nw; out.h = g.h; out.stride = nw;
    out.px.resize(size_t(nw) * size_t(g.h));
    if (!g.keep.empty()) out.keep.resize(out.px.size());
    parallelRows(0, g.h, [&](int y0, int y1) {
        std::vector<int> cols(static_cast<size_t>(k));
        for (int y = y0; y < y1; y++) {
            for (int s = 0; s < k; s++) cols[size_t(s)] = seams[size_t(s)][size_t(y)];
            std::sort(cols.begin(), cols.end());
            const uint32_t* r = g.px.data() + size_t(y) * size_t(g.stride);
            const uint8_t* kp = g.keep.empty() ? nullptr : g.keep.data() + size_t(y) * size_t(g.stride);
            uint32_t* o = out.px.data() + size_t(y) * size_t(nw);
            uint8_t* ok = out.keep.empty() ? nullptr : out.keep.data() + size_t(y) * size_t(nw);
            int j = 0, c = 0;
            for (int x = 0; x < g.w; x++) {
                o[j] = r[x];
                if (ok) ok[j] = kp[x];
                j++;
                if (c < k && cols[size_t(c)] == x) {
                    c++;
                    uint32_t a = r[x], b = r[std::min(x + 1, g.w - 1)], m = 0;
                    for (int s = 0; s < 32; s += 8) m |= ((((a >> s) & 255u) + ((b >> s) & 255u) + 1u) / 2u) << s;
                    o[j] = m;
                    if (ok) ok[j] = kp[x];
                    j++;
                }
            }
        }
    }, 32);
    g = std::move(out);
}

void carveWidth(Grid& g, int target, double fraction) {
    while (g.w > target) {
        int step = std::min(g.w - target, std::max(1, int(g.w * fraction)));
        removeSeams(g, findSeams(g, step));
    }
    while (g.w < target) {
        // Rounds of up to half the width, each seam duplicated once, so a stretch spreads over many seams.
        int step = std::min(target - g.w, std::max(1, g.w / 2));
        insertSeams(g, findSeams(g, step));
    }
}

Grid transpose(const Grid& g) {
    Grid t;
    t.w = g.h; t.h = g.w; t.stride = g.h;
    t.px.resize(size_t(t.stride) * size_t(t.h));
    if (!g.keep.empty()) t.keep.resize(t.px.size());
    parallelRows(0, t.h, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) for (int x = 0; x < t.w; x++) {
            size_t src = size_t(x) * size_t(g.stride) + size_t(y);
            t.px[size_t(y) * size_t(t.stride) + size_t(x)] = g.px[src];
            if (!g.keep.empty()) t.keep[size_t(y) * size_t(t.stride) + size_t(x)] = g.keep[src];
        }
    }, 32);
    return t;
}

} // namespace

Image seamCarve(const Image& image, int width, int height, const SeamCarveOptions& options) {
    if (image.isEmpty() || width < 1 || height < 1 || width > maxImageSide || height > maxImageSide) return {};
    Grid g;
    g.w = image.width(); g.h = image.height(); g.stride = g.w;
    g.px.resize(size_t(g.w) * size_t(g.h));
    for (int y = 0; y < g.h; y++) std::memcpy(g.px.data() + size_t(y) * size_t(g.w), image.row(y), size_t(g.w) * 4);
    const GrayImage* p = options.protect;
    if (p && p->width() == g.w && p->height() == g.h) {
        g.keep.resize(g.px.size());
        for (size_t i = 0; i < g.keep.size(); i++) g.keep[i] = p->data()[i] ? 1 : 0;
    }
    const double fraction = std::clamp(options.passFraction, 1e-6, 0.5);
    if (width != g.w) carveWidth(g, width, fraction);
    if (height != g.h) {
        Grid t = transpose(g);
        carveWidth(t, height, fraction);
        g = transpose(t);
    }
    Image out(g.w, g.h);
    for (int y = 0; y < g.h; y++) std::memcpy(out.row(y), g.px.data() + size_t(y) * size_t(g.stride), size_t(g.w) * 4);
    return out;
}

} // namespace compositor
