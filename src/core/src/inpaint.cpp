#include "compositor/inpaint.h"
#include "compositor/heal.h"
#include "compositor/parallel.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace compositor {

namespace {

inline uint32_t xorshift(uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

/// One pyramid level of the work region.
struct Level {
    int w = 0, h = 0;
    std::vector<uint8_t> pix;       // premultiplied RGBA; hole pixels hold the synthesis in progress
    std::vector<uint8_t> hole;      // 1: to synthesise
    std::vector<uint8_t> known;     // 1: opaque and not hole, usable as source
    std::vector<uint8_t> target;    // 1: a patch centred here overlaps the hole (a nearest neighbour is kept)
    std::vector<uint8_t> source;    // 1: the patch centred here is fully known and inside the level
    std::vector<int32_t> nn;        // nearest source centre per target, x then y
    std::vector<int32_t> dist;      // its patch distance
    std::vector<int32_t> sources;   // every source centre, x then y, for random draws
    int tx0 = 0, ty0 = 0, tx1 = 0, ty1 = 0;   // the rows and columns holding targets (half-open)
    size_t at(int x, int y) const { return size_t(y) * w + size_t(x); }
};

/// 1 where any pixel of the (2r+1)^2 box around (x, y), clipped to the level, is set: a row pass of window
/// counts, then a column pass over those.
std::vector<uint8_t> boxAny(const std::vector<uint8_t>& m, int w, int h, int r) {
    std::vector<int> horizontal(size_t(w) * h);
    parallelRows(0, h, [&](int y0, int y1) {
        std::vector<int> prefix(size_t(w) + 1);
        for (int y = y0; y < y1; y++) {
            prefix[0] = 0;
            for (int x = 0; x < w; x++) prefix[size_t(x) + 1] = prefix[size_t(x)] + m[size_t(y) * w + size_t(x)];
            for (int x = 0; x < w; x++) horizontal[size_t(y) * w + size_t(x)] = prefix[size_t(std::min(w, x + r + 1))] - prefix[size_t(std::max(0, x - r))];
        }
    }, 64);
    std::vector<uint8_t> out(size_t(w) * h);
    parallelRows(0, w, [&](int x0, int x1) {
        std::vector<int> prefix(size_t(h) + 1);
        for (int x = x0; x < x1; x++) {
            prefix[0] = 0;
            for (int y = 0; y < h; y++) prefix[size_t(y) + 1] = prefix[size_t(y)] + horizontal[size_t(y) * w + size_t(x)];
            for (int y = 0; y < h; y++) out[size_t(y) * w + size_t(x)] = (prefix[size_t(std::min(h, y + r + 1))] - prefix[size_t(std::max(0, y - r))]) > 0;
        }
    }, 64);
    return out;
}

/// Patch masks for a level: targets overlap the hole; sources are fully known patches inside the level.
void classify(Level& L, int r) {
    L.target = boxAny(L.hole, L.w, L.h, r);
    std::vector<uint8_t> unknown(L.known.size());
    for (size_t i = 0; i < unknown.size(); i++) unknown[i] = !L.known[i];
    std::vector<uint8_t> anyUnknown = boxAny(unknown, L.w, L.h, r);
    L.source.assign(L.known.size(), 0);
    L.sources.clear();
    for (int y = r; y < L.h - r; y++)
        for (int x = r; x < L.w - r; x++)
            if (!anyUnknown[L.at(x, y)]) { L.source[L.at(x, y)] = 1; L.sources.push_back(x); L.sources.push_back(y); }
    L.tx0 = L.w; L.ty0 = L.h; L.tx1 = 0; L.ty1 = 0;
    for (int y = 0; y < L.h; y++)
        for (int x = 0; x < L.w; x++)
            if (L.target[L.at(x, y)]) { L.tx0 = std::min(L.tx0, x); L.ty0 = std::min(L.ty0, y); L.tx1 = std::max(L.tx1, x + 1); L.ty1 = std::max(L.ty1, y + 1); }
    if (L.tx1 <= L.tx0) L.tx0 = L.ty0 = L.tx1 = L.ty1 = 0;
}

/// Sum of squared differences over the patches at (px, py) and (qx, qy); the source patch is inside the
/// level, the target's pixels beyond it are skipped. Stops early once past `best`.
inline int patchDistance(const Level& L, int r, int px, int py, int qx, int qy, int best) {
    int sum = 0;
    for (int j = -r; j <= r; j++) {
        const int ty = py + j;
        if (ty < 0 || ty >= L.h) continue;
        const int x0 = std::max(-r, -px), x1 = std::min(r, L.w - 1 - px);
        const uint8_t* t = &L.pix[(L.at(px + x0, ty)) * 4];
        const uint8_t* s = &L.pix[(L.at(qx + x0, qy + j)) * 4];
        for (int i = x0; i <= x1; i++, t += 4, s += 4)
            for (int c = 0; c < 4; c++) { int d = int(t[c]) - int(s[c]); sum += d * d; }
        if (sum >= best) return sum;
    }
    return sum;
}

void randomSource(const Level& L, uint32_t& rng, int& qx, int& qy) {
    const size_t n = L.sources.size() / 2;
    const size_t k = size_t(xorshift(rng)) % n;
    qx = L.sources[k * 2]; qy = L.sources[k * 2 + 1];
}

/// Distances of the current field, recomputed after the hole's pixels changed.
void refreshDistances(Level& L, int r) {
    parallelRows(L.ty0, L.ty1, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++)
            for (int x = L.tx0; x < L.tx1; x++) {
                const size_t p = L.at(x, y);
                if (L.target[p]) L.dist[p] = patchDistance(L, r, x, y, L.nn[p * 2], L.nn[p * 2 + 1], INT32_MAX);
            }
    }, 8);
}

/// One PatchMatch pass: propagate from the neighbour already visited in this scan, then random search around
/// the current best with a halving radius. Bands of rows run in parallel, propagating within the band.
void propagateAndSearch(Level& L, int r, uint32_t& rng, bool forward) {
    const uint32_t base = xorshift(rng);
    parallelRows(L.ty0, L.ty1, [&](int y0, int y1) {
        uint32_t seed = (base ^ (uint32_t(y0) * 2654435761u)) | 1u;
        for (int yy = y0; yy < y1; yy++) {
            const int y = forward ? yy : y1 - 1 - (yy - y0);
            for (int xx = L.tx0; xx < L.tx1; xx++) {
                const int x = forward ? xx : L.tx1 - 1 - (xx - L.tx0);
                const size_t p = L.at(x, y);
                if (!L.target[p]) continue;
                int bx = L.nn[p * 2], by = L.nn[p * 2 + 1], bd = L.dist[p];
                auto consider = [&](int qx, int qy) {
                    if (qx < r || qy < r || qx >= L.w - r || qy >= L.h - r) return;
                    if (!L.source[L.at(qx, qy)] || (qx == bx && qy == by)) return;
                    int d = patchDistance(L, r, x, y, qx, qy, bd);
                    if (d < bd) { bd = d; bx = qx; by = qy; }
                };
                const int step = forward ? -1 : 1;
                if (x + step >= 0 && x + step < L.w) { size_t n = L.at(x + step, y); if (L.target[n]) consider(L.nn[n * 2] - step, L.nn[n * 2 + 1]); }
                if (y + step >= y0 && y + step < y1) { size_t n = L.at(x, y + step); if (L.target[n]) consider(L.nn[n * 2], L.nn[n * 2 + 1] - step); }
                for (int radius = std::max(L.w, L.h); radius >= 1; radius /= 2) {
                    const int qx = bx + int(xorshift(seed) % unsigned(2 * radius + 1)) - radius;
                    const int qy = by + int(xorshift(seed) % unsigned(2 * radius + 1)) - radius;
                    consider(qx, qy);
                }
                L.nn[p * 2] = bx; L.nn[p * 2 + 1] = by; L.dist[p] = bd;
            }
        }
    }, 8);
}

/// The spread of the field's distances, for the vote weights: half the median over the targets.
float distanceScale(const Level& L) {
    std::vector<int32_t> sample;
    for (int y = L.ty0; y < L.ty1; y++) for (int x = L.tx0; x < L.tx1; x++) { size_t p = L.at(x, y); if (L.target[p]) sample.push_back(L.dist[p]); }
    if (sample.empty()) return 1;
    std::nth_element(sample.begin(), sample.begin() + long(sample.size() / 2), sample.end());
    return std::max(1.0f, float(sample[sample.size() / 2]) * 0.5f);
}

/// Every hole pixel becomes the weighted mean of what the patches covering it propose, the weights falling
/// off from the best-matching patch so the vote stays sharp without ever collapsing to nothing.
void vote(Level& L, int r, float scale) {
    parallelRows(L.ty0, L.ty1, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++)
            for (int x = L.tx0; x < L.tx1; x++) {
                const size_t p = L.at(x, y);
                if (!L.hole[p]) continue;
                int best = INT32_MAX;
                for (int j = -r; j <= r; j++)
                    for (int i = -r; i <= r; i++) {
                        const int cx = x - i, cy = y - j;
                        if (cx < 0 || cy < 0 || cx >= L.w || cy >= L.h) continue;
                        const size_t c = L.at(cx, cy);
                        if (L.target[c]) best = std::min(best, L.dist[c]);
                    }
                if (best == INT32_MAX) continue;
                double acc[4] = {0, 0, 0, 0}, total = 0;
                for (int j = -r; j <= r; j++)
                    for (int i = -r; i <= r; i++) {
                        const int cx = x - i, cy = y - j;
                        if (cx < 0 || cy < 0 || cx >= L.w || cy >= L.h) continue;
                        const size_t c = L.at(cx, cy);
                        if (!L.target[c]) continue;
                        const double w = std::exp(-double(L.dist[c] - best) / scale);
                        const uint8_t* s = &L.pix[L.at(L.nn[c * 2] + i, L.nn[c * 2 + 1] + j) * 4];
                        for (int k = 0; k < 4; k++) acc[k] += w * s[k];
                        total += w;
                    }
                uint8_t* o = &L.pix[p * 4];
                o[3] = uint8_t(std::lround(acc[3] / total));
                for (int k = 0; k < 3; k++) o[k] = uint8_t(std::min<long>(o[3], std::lround(acc[k] / total)));
            }
    }, 8);
}

/// The next coarser level: 2x2 means of the known children; a coarse pixel is hole when any child is, known
/// when all its children are.
Level coarsen(const Level& fine) {
    Level L;
    L.w = (fine.w + 1) / 2; L.h = (fine.h + 1) / 2;
    L.pix.assign(size_t(L.w) * L.h * 4, 0); L.hole.assign(size_t(L.w) * L.h, 0); L.known.assign(size_t(L.w) * L.h, 1);
    parallelRows(0, L.h, [&](int ya, int yb) {
    for (int y = ya; y < yb; y++)
        for (int x = 0; x < L.w; x++) {
            int sum[4] = {0, 0, 0, 0}, all[4] = {0, 0, 0, 0}, n = 0, m = 0;
            for (int j = 0; j < 2; j++)
                for (int i = 0; i < 2; i++) {
                    const int fx = 2 * x + i, fy = 2 * y + j;
                    if (fx >= fine.w || fy >= fine.h) continue;
                    const size_t f = fine.at(fx, fy);
                    const uint8_t* p = &fine.pix[f * 4];
                    for (int c = 0; c < 4; c++) all[c] += p[c];
                    m++;
                    if (fine.hole[f]) L.hole[L.at(x, y)] = 1;
                    if (!fine.known[f]) { L.known[L.at(x, y)] = 0; continue; }
                    for (int c = 0; c < 4; c++) sum[c] += p[c];
                    n++;
                }
            uint8_t* o = &L.pix[L.at(x, y) * 4];
            if (n) for (int c = 0; c < 4; c++) o[c] = uint8_t((sum[c] + n / 2) / n);
            else if (m) for (int c = 0; c < 4; c++) o[c] = uint8_t((all[c] + m / 2) / m);
        }
    }, 64);
    return L;
}

/// A first guess for the coarsest level's hole: the smooth spread of the surrounding colours.
void smoothStart(Level& L) {
    std::vector<float> values(size_t(L.w) * L.h * 4);
    for (size_t i = 0; i < values.size(); i++) values[i] = L.pix[i];
    membraneFill(values.data(), 4, L.hole.data(), L.known.data(), L.w, L.h);
    for (size_t p = 0; p < L.hole.size(); p++) {
        if (!L.hole[p]) continue;
        uint8_t* o = &L.pix[p * 4];
        o[3] = uint8_t(std::lround(std::clamp(values[p * 4 + 3], 0.0f, 255.0f)));
        for (int c = 0; c < 3; c++) o[c] = uint8_t(std::lround(std::clamp(values[p * 4 + size_t(c)], 0.0f, float(o[3]))));
    }
}

} // namespace

bool contentFill(Image& image, const GrayImage& hole, const InpaintOptions& options) {
    const int r = std::clamp(options.patchRadius, 1, 8);
    PixelBounds b = nonzeroBounds(hole);
    if (b.isEmpty()) return true;
    // The work region: the hole with room around it to copy from, twice its size each way (as far as a
    // match is likely to sit), bounded so a large fill does not pull the whole layer through the pyramid.
    const int reach = std::clamp(2 * std::max(b.x1 - b.x0, b.y1 - b.y0), 64, 384);
    const int x0 = std::max(0, b.x0 - reach), y0 = std::max(0, b.y0 - reach);
    const int x1 = std::min(image.width(), b.x1 + reach), y1 = std::min(image.height(), b.y1 + reach);
    Level fine;
    fine.w = x1 - x0; fine.h = y1 - y0;
    fine.pix.resize(size_t(fine.w) * fine.h * 4); fine.hole.resize(size_t(fine.w) * fine.h); fine.known.resize(size_t(fine.w) * fine.h);
    for (int y = 0; y < fine.h; y++) {
        std::memcpy(&fine.pix[fine.at(0, y) * 4], image.pixel(x0, y0 + y), size_t(fine.w) * 4);
        for (int x = 0; x < fine.w; x++) {
            const size_t p = fine.at(x, y);
            fine.hole[p] = hole.at(x0 + x, y0 + y) != 0;
            fine.known[p] = !fine.hole[p] && image.pixel(x0 + x, y0 + y)[3] == 255;
        }
    }
    // The pyramid, down to where the hole is a few patches wide.
    std::vector<Level> levels;
    levels.push_back(std::move(fine));
    while (levels.size() < 8) {
        const Level& last = levels.back();
        PixelBounds hb;
        { int hx0 = last.w, hy0 = last.h, hx1 = 0, hy1 = 0;
          for (int y = 0; y < last.h; y++) for (int x = 0; x < last.w; x++) if (last.hole[last.at(x, y)]) { hx0 = std::min(hx0, x); hy0 = std::min(hy0, y); hx1 = std::max(hx1, x + 1); hy1 = std::max(hy1, y + 1); }
          hb = {hx0, hy0, hx1, hy1}; }
        if (std::max(hb.x1 - hb.x0, hb.y1 - hb.y0) <= 4 * r || std::min(last.w, last.h) <= 8 * r) break;
        levels.push_back(coarsen(last));
    }
    for (Level& L : levels) classify(L, r);
    // Coarsest first: nothing to copy from means no fill at all.
    int coarsest = int(levels.size()) - 1;
    while (coarsest > 0 && levels[size_t(coarsest)].sources.empty()) coarsest--;
    if (levels[size_t(coarsest)].sources.empty()) return false;

    uint32_t rng = options.seed * 2654435761u + 1;
    for (int li = coarsest; li >= 0; li--) {
        Level& L = levels[size_t(li)];
        L.nn.assign(L.target.size() * 2, -1); L.dist.assign(L.target.size(), INT32_MAX);
        if (L.sources.empty()) continue;
        if (li == coarsest) {
            smoothStart(L);
            for (int y = L.ty0; y < L.ty1; y++) for (int x = L.tx0; x < L.tx1; x++) { size_t p = L.at(x, y); if (L.target[p]) randomSource(L, rng, L.nn[p * 2], L.nn[p * 2 + 1]); }
        } else {
            // The coarser field, doubled; the hole then takes its first vote at this resolution.
            const Level& C = levels[size_t(li + 1)];
            for (int y = L.ty0; y < L.ty1; y++)
                for (int x = L.tx0; x < L.tx1; x++) {
                    const size_t p = L.at(x, y);
                    if (!L.target[p]) continue;
                    const size_t c = C.at(std::min(x / 2, C.w - 1), std::min(y / 2, C.h - 1));
                    int qx = -1, qy = -1;
                    if (C.target[c] && C.nn[c * 2] >= 0) { qx = C.nn[c * 2] * 2 + (x & 1); qy = C.nn[c * 2 + 1] * 2 + (y & 1); }
                    if (qx < r || qy < r || qx >= L.w - r || qy >= L.h - r || !L.source[L.at(qx, qy)]) randomSource(L, rng, qx, qy);
                    L.nn[p * 2] = qx; L.nn[p * 2 + 1] = qy;
                }
            refreshDistances(L, r);
            vote(L, r, distanceScale(L));
        }
        const int passes = li == coarsest ? options.iterations + 2 : options.iterations;
        for (int it = 0; it < passes; it++) {
            refreshDistances(L, r);
            propagateAndSearch(L, r, rng, (it & 1) == 0);
            vote(L, r, distanceScale(L));
        }
    }
    const Level& L = levels[0];
    for (int y = 0; y < L.h; y++)
        for (int x = 0; x < L.w; x++) {
            const size_t p = L.at(x, y);
            if (L.hole[p]) std::memcpy(image.pixel(x0 + x, y0 + y), &L.pix[p * 4], 4);
        }
    return true;
}

} // namespace compositor
