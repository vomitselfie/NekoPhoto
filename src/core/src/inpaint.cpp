// Content-aware fill by classic exemplar-based inpainting (Criminisi, Perez & Toyama 2003, whose patent, Microsoft
// US 6987520, expired 2023-03-30; Efros & Leung 1999). See inpaint.h for the legal boundary this file keeps; in short:
// the fill front advances in a fixed deterministic priority order, every target patch is matched by an exhaustive scan
// of a bounded window at the working resolution, nothing found for one patch is reused, propagated, perturbed or
// stored for another, and the only colour adaptation is the boundary membrane on the low-pass band.
#include "compositor/inpaint.h"
#include "compositor/depth.h"
#include "compositor/heal.h"
#include "compositor/parallel.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <vector>

namespace compositor {

namespace {

enum State : uint8_t { Unknown = 0, Known = 1, Filled = 2, Hole = 3 };
constexpr int32_t kFullConfidence = 4096;
constexpr int64_t kDataFloor = 64;   // keeps flat areas advancing by confidence alone

/// The work region: the hole with the search window's reach around it, in its own coordinates.
struct Work {
    int w = 0, h = 0, r = 4;
    std::vector<uint8_t> pix;        // premultiplied RGBA at 8 bits
    std::vector<uint8_t> state;
    std::vector<uint8_t> holeAtStart;
    std::vector<uint8_t> sourceOk;   // the patch centred here is inside the region and wholly Known
    std::vector<int32_t> confidence;
    std::vector<int32_t> from;       // for each filled pixel, the region index of the pixel it copies
    size_t at(int x, int y) const { return size_t(y) * size_t(w) + size_t(x); }
    bool inside(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
    int luma(size_t p) const { const uint8_t* q = &pix[p * 4]; return (q[0] * 77 + q[1] * 151 + q[2] * 28) >> 8; }
    bool seen(int x, int y) const { return inside(x, y) && (state[at(x, y)] == Known || state[at(x, y)] == Filled); }
};

/// Patches wholly made of Known pixels: a row pass of window counts of anything else, then a column pass.
void markSources(Work& W) {
    const int w = W.w, h = W.h, r = W.r;
    std::vector<int> horizontal(size_t(w) * size_t(h), 0);
    std::vector<int> prefix(size_t(w) + 1, 0);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) prefix[size_t(x) + 1] = prefix[size_t(x)] + (W.state[W.at(x, y)] != Known);
        for (int x = r; x < w - r; x++) horizontal[W.at(x, y)] = prefix[size_t(x + r + 1)] - prefix[size_t(x - r)];
    }
    W.sourceOk.assign(size_t(w) * size_t(h), 0);
    for (int x = r; x < w - r; x++) {
        int bad = 0;
        for (int y = 0; y < std::min(h, 2 * r + 1); y++) bad += horizontal[W.at(x, y)];
        for (int y = r; y < h - r; y++) {
            if (y > r) bad += horizontal[W.at(x, y + r)] - horizontal[W.at(x, y - r - 1)];
            W.sourceOk[W.at(x, y)] = bad == 0;
        }
    }
}

bool isFront(const Work& W, int x, int y) {
    if (W.state[W.at(x, y)] != Hole) return false;
    const int n[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
    for (auto& o : n)
        if (!W.inside(o[0], o[1]) || W.state[W.at(o[0], o[1])] != Hole) return true;
    return false;
}

/// Criminisi's priority in integers: confidence (the share of the patch already trusted) times the data term (the
/// strongest isophote in the patch's known pixels across the front's normal), plus a fixed floor.
int64_t priorityAt(const Work& W, int x, int y, int32_t* confidenceOut) {
    const int r = W.r;
    int64_t trusted = 0;
    int bestG = -1, gx = 0, gy = 0;
    for (int j = -r; j <= r; j++)
        for (int i = -r; i <= r; i++) {
            const int px = x + i, py = y + j;
            if (!W.seen(px, py)) continue;
            trusted += W.confidence[W.at(px, py)];
            if (W.seen(px - 1, py) && W.seen(px + 1, py) && W.seen(px, py - 1) && W.seen(px, py + 1)) {
                const int dx = W.luma(W.at(px + 1, py)) - W.luma(W.at(px - 1, py));
                const int dy = W.luma(W.at(px, py + 1)) - W.luma(W.at(px, py - 1));
                const int g = dx * dx + dy * dy;
                if (g > bestG) { bestG = g; gx = dx; gy = dy; }
            }
        }
    const int area = (2 * r + 1) * (2 * r + 1);
    const int32_t confidence = int32_t(trusted / area);
    if (confidenceOut) *confidenceOut = confidence;
    // The front's normal: a Sobel of the hole indicator.
    auto hole = [&](int px, int py) { return W.inside(px, py) && W.state[W.at(px, py)] == Hole ? 1 : 0; };
    int nx = 0, ny = 0;
    for (int k = -1; k <= 1; k++) {
        const int weight = k == 0 ? 2 : 1;
        nx += weight * (hole(x + 1, y + k) - hole(x - 1, y + k));
        ny += weight * (hole(x + k, y + 1) - hole(x + k, y - 1));
    }
    int64_t data = 0;
    if (bestG > 0) {
        const int64_t cross = std::llabs(int64_t(gx) * ny - int64_t(gy) * nx);
        data = cross * 4 / std::max(1, std::abs(nx) + std::abs(ny));
    }
    return int64_t(confidence) * (data + kDataFloor);
}

struct Candidate { int64_t distance = std::numeric_limits<int64_t>::max(); int64_t order = std::numeric_limits<int64_t>::max(); int x = -1, y = -1; };

/// The best source patch for the target at (tx, ty): every source centre within `radius` is scored, independently
/// of any other target. The parallel reduction keeps the smallest distance, then the earliest in scan order, so the
/// result does not depend on how the rows are split.
Candidate bestSource(const Work& W, int tx, int ty, int radius, int penalty) {
    const int r = W.r;
    std::vector<int> deltas;             // region-index offsets of the target's seen pixels
    std::vector<int> values;             // their RGBA
    for (int j = -r; j <= r; j++)
        for (int i = -r; i <= r; i++) {
            if (!W.seen(tx + i, ty + j)) continue;
            deltas.push_back(j * W.w + i);
            const uint8_t* p = &W.pix[W.at(tx + i, ty + j) * 4];
            for (int c = 0; c < 4; c++) values.push_back(p[c]);
        }
    const int y0 = std::max(r, ty - radius), y1 = std::min(W.h - r - 1, ty + radius);
    const int x0 = std::max(r, tx - radius), x1 = std::min(W.w - r - 1, tx + radius);
    Candidate best;
    if (y1 < y0 || x1 < x0) return best;
    std::mutex lock;
    const size_t n = deltas.size();
    parallelRows(y0, y1 + 1, [&](int ya, int yb) {
        Candidate local;
        for (int y = ya; y < yb; y++)
            for (int x = x0; x <= x1; x++) {
                const size_t q = W.at(x, y);
                if (!W.sourceOk[q]) continue;
                int64_t sum = int64_t(penalty) * (std::abs(x - tx) + std::abs(y - ty));
                if (sum > local.distance) continue;
                const uint8_t* base = &W.pix[q * 4];
                for (size_t k = 0; k < n && sum <= local.distance; k++) {
                    const uint8_t* s = base + ptrdiff_t(deltas[k]) * 4;
                    const int* t = &values[k * 4];
                    sum += std::abs(t[0] - s[0]) + std::abs(t[1] - s[1]) + std::abs(t[2] - s[2]) + std::abs(t[3] - s[3]);
                }
                const int64_t order = int64_t(y) * W.w + x;
                if (sum < local.distance || (sum == local.distance && order < local.order)) local = {sum, order, x, y};
            }
        std::lock_guard<std::mutex> guard(lock);
        if (local.distance < best.distance || (local.distance == best.distance && local.order < best.order)) best = local;
    }, 4);
    return best;
}

/// Tone match: the healing membrane on the low-pass band. At each pixel b of the ring just outside the hole, the
/// destination-minus-source offset of the low-pass band is taken: the local mean of the surroundings at b minus the
/// local mean at the pixel b was matched against (b's place relative to the copied patch next to it, at the patch's
/// source), both read from the untouched known pixels. Those boundary offsets are interpolated across the hole
/// (Dirichlet interpolation of boundary values, heal.h, the expired US 6587592 membrane) and added to the fill, so a
/// patch copied from a darker or lighter place takes the tone of where it lands. Matching content has equal local
/// means at both places, so a pattern copied in phase is left alone; the fill's detail is untouched and no gradient
/// is composited (US 9058699).
template <typename T>
void matchTone(T* pix, int w, int h, const uint8_t* hole, const uint8_t* known, const int32_t* from, int radius, double one) {
    const size_t n = size_t(w) * size_t(h);
    // A summed-area table of the known cells and their count: their local means.
    const size_t sw = size_t(w) + 1;
    std::vector<double> sum((size_t(h) + 1) * sw * 4, 0);
    std::vector<int> count((size_t(h) + 1) * sw, 0);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const size_t p = size_t(y) * w + size_t(x), s = (size_t(y) + 1) * sw + size_t(x) + 1;
            const size_t up = s - sw, left = s - 1, diag = s - sw - 1;
            const bool isK = known[p];
            count[s] = count[up] + count[left] - count[diag] + isK;
            for (int c = 0; c < 4; c++)
                sum[s * 4 + size_t(c)] = sum[up * 4 + size_t(c)] + sum[left * 4 + size_t(c)] - sum[diag * 4 + size_t(c)] + (isK ? double(pix[p * 4 + size_t(c)]) : 0);
        }
    auto mean = [&](int x, int y, double out[4]) {
        const int ax = std::max(0, x - radius), ay = std::max(0, y - radius), bx = std::min(w, x + radius + 1), by = std::min(h, y + radius + 1);
        const size_t a = size_t(ay) * sw + size_t(ax), b = size_t(ay) * sw + size_t(bx), c0 = size_t(by) * sw + size_t(ax), d = size_t(by) * sw + size_t(bx);
        const int k = count[d] - count[b] - count[c0] + count[a];
        if (!k) return false;
        for (int c = 0; c < 4; c++) {
            const size_t cc = size_t(c);
            out[c] = (sum[d * 4 + cc] - sum[b * 4 + cc] - sum[c0 * 4 + cc] + sum[a * 4 + cc]) / k;
        }
        return true;
    };
    std::vector<float> values(n * 4, 0.0f);
    std::vector<uint8_t> ring(n, 0);
    bool any = false;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const size_t p = size_t(y) * w + size_t(x);
            if (!known[p]) continue;
            // The first hole neighbour in scan order that was filled: where b's counterpart sits at its source.
            int sx = -1, sy = -1;
            for (int j = -1; j <= 1 && sx < 0; j++)
                for (int i = -1; i <= 1 && sx < 0; i++) {
                    const int qx = x + i, qy = y + j;
                    if (qx < 0 || qy < 0 || qx >= w || qy >= h) continue;
                    const size_t q = size_t(qy) * w + size_t(qx);
                    if (!hole[q] || from[q] < 0) continue;
                    const int fx = int(from[q] % w), fy = int(from[q] / w);
                    sx = fx - i; sy = fy - j;
                }
            if (sx < 0 || sy < 0 || sx >= w || sy >= h || !known[size_t(sy) * w + size_t(sx)]) continue;
            double here[4], there[4];
            if (!mean(x, y, here) || !mean(sx, sy, there)) continue;
            for (int c = 0; c < 4; c++) values[p * 4 + size_t(c)] = float(here[c] - there[c]);
            ring[p] = 1;
            any = true;
        }
    if (!any) return;
    membraneFill(values.data(), 4, hole, ring.data(), w, h);
    for (size_t p = 0; p < n; p++) {
        if (!hole[p]) continue;
        T* o = &pix[p * 4];
        const double a = std::clamp(double(o[3]) + values[p * 4 + 3], 0.0, one);
        o[3] = T(std::lround(a));
        for (int c = 0; c < 3; c++) o[c] = T(std::lround(std::clamp(double(o[c]) + values[p * 4 + size_t(c)], 0.0, double(o[3]))));
    }
}

/// The fill itself over the region of `image` around the hole. On success `W` holds the filled region, with `from`
/// naming where every filled pixel came from (for the 16-bit copy), and (x0, y0) its place in the image.
bool exemplarFill(const Image& image, const GrayImage& hole, const InpaintOptions& options, const GrayImage* visible, Work& W, int& x0, int& y0) {
    const int r = std::clamp(options.patchRadius, 1, 8);
    const bool wide = visible && options.sampleWholeVisible;
    const int radius = std::max(2 * r + 1, wide ? options.wideSearchRadius : options.searchRadius);
    const int penalty = std::max(0, options.offsetPenalty);
    const PixelBounds b = nonzeroBounds(hole);
    if (b.isEmpty()) return true;
    const int reach = radius + 2 * r + 1;
    x0 = std::max(0, b.x0 - reach); y0 = std::max(0, b.y0 - reach);
    const int x1 = std::min(image.width(), b.x1 + reach), y1 = std::min(image.height(), b.y1 + reach);
    W.w = x1 - x0; W.h = y1 - y0; W.r = r;
    const size_t n = size_t(W.w) * size_t(W.h);
    W.pix.resize(n * 4); W.state.assign(n, Unknown); W.confidence.assign(n, 0); W.from.assign(n, -1);
    for (int y = 0; y < W.h; y++) {
        std::memcpy(&W.pix[W.at(0, y) * 4], image.pixel(x0, y0 + y), size_t(W.w) * 4);
        for (int x = 0; x < W.w; x++) {
            const size_t p = W.at(x, y);
            if (hole.at(x0 + x, y0 + y)) { W.state[p] = Hole; continue; }
            if (image.pixel(x0 + x, y0 + y)[3] == 255 && (!visible || visible->at(x0 + x, y0 + y) >= 128)) { W.state[p] = Known; W.confidence[p] = kFullConfidence; }
        }
    }
    W.holeAtStart.resize(n);
    for (size_t p = 0; p < n; p++) W.holeAtStart[p] = W.state[p] == Hole;
    markSources(W);
    bool anySource = false;
    for (uint8_t s : W.sourceOk) if (s) { anySource = true; break; }
    if (!anySource) return false;

    // The front, with each pixel's priority; a pixel is listed once and dropped when it is no longer front.
    std::vector<int64_t> priority(n, -1);
    std::vector<uint8_t> listed(n, 0);
    std::vector<int32_t> front;
    for (int y = 0; y < W.h; y++)
        for (int x = 0; x < W.w; x++)
            if (isFront(W, x, y)) { const size_t p = W.at(x, y); priority[p] = priorityAt(W, x, y, nullptr); listed[p] = 1; front.push_back(int32_t(p)); }
    if (front.empty()) return false;

    while (true) {
        // The highest priority; ties go to the top-most, then left-most pixel.
        size_t keep = 0;
        int64_t bestPriority = -1;
        int32_t target = -1;
        for (size_t k = 0; k < front.size(); k++) {
            const int32_t p = front[k];
            if (W.state[size_t(p)] != Hole) { listed[size_t(p)] = 0; continue; }
            front[keep++] = p;
            if (priority[size_t(p)] > bestPriority || (priority[size_t(p)] == bestPriority && p < target)) { bestPriority = priority[size_t(p)]; target = p; }
        }
        front.resize(keep);
        if (target < 0) break;
        const int tx = int(target % W.w), ty = int(target / W.w);
        int32_t confidence = 0;
        priorityAt(W, tx, ty, &confidence);
        Candidate best = bestSource(W, tx, ty, radius, penalty);
        // Nothing usable in the window (a hole far from any whole patch): the same scan over a larger window.
        for (int wider = radius * 2; best.x < 0 && wider < 2 * std::max(W.w, W.h); wider *= 2) best = bestSource(W, tx, ty, wider, penalty);
        if (best.x < 0) return false;
        for (int j = -r; j <= r; j++)
            for (int i = -r; i <= r; i++) {
                const int px = tx + i, py = ty + j;
                if (!W.inside(px, py) || W.state[W.at(px, py)] != Hole) continue;
                const size_t p = W.at(px, py), s = W.at(best.x + i, best.y + j);
                std::memcpy(&W.pix[p * 4], &W.pix[s * 4], 4);
                W.state[p] = Filled;
                W.confidence[p] = confidence;
                W.from[p] = int32_t(s);
            }
        // The front near the copied patch, and the priorities its change reaches.
        const int reachP = 2 * r + 1;
        for (int y = std::max(0, ty - reachP); y < std::min(W.h, ty + reachP + 1); y++)
            for (int x = std::max(0, tx - reachP); x < std::min(W.w, tx + reachP + 1); x++) {
                const size_t p = W.at(x, y);
                if (!isFront(W, x, y)) continue;
                priority[p] = priorityAt(W, x, y, nullptr);
                if (!listed[p]) { listed[p] = 1; front.push_back(int32_t(p)); }
            }
    }
    for (size_t p = 0; p < n; p++) if (W.state[p] == Hole) return false;
    return true;
}

/// The known (Known) cells of the region, for the tone match.
std::vector<uint8_t> knownCells(const Work& W) {
    std::vector<uint8_t> out(W.state.size());
    for (size_t p = 0; p < out.size(); p++) out[p] = W.state[p] == Known;
    return out;
}

} // namespace

bool contentFill(Image& image, const GrayImage& hole, const InpaintOptions& options, const GrayImage* visible) {
    if (visible && (visible->width() != image.width() || visible->height() != image.height())) visible = nullptr;
    if (nonzeroBounds(hole).isEmpty()) return true;
    Work W;
    int x0 = 0, y0 = 0;
    if (!exemplarFill(image, hole, options, visible, W, x0, y0)) return false;
    const std::vector<uint8_t> known = knownCells(W);
    if (options.toneMatch) matchTone<uint8_t>(W.pix.data(), W.w, W.h, W.holeAtStart.data(), known.data(), W.from.data(), 2 * W.r + 1, 255.0);
    for (int y = 0; y < W.h; y++)
        for (int x = 0; x < W.w; x++) {
            const size_t p = W.at(x, y);
            if (W.holeAtStart[p]) std::memcpy(image.pixel(x0 + x, y0 + y), &W.pix[p * 4], 4);
        }
    return true;
}

bool contentFill16(Image16& image, const GrayImage& hole, const InpaintOptions& options, const GrayImage* visible) {
    // The order and the patches are chosen on the image rounded to 8 bits; every filled pixel then takes the 16-bit
    // pixel its 8-bit copy came from, and the tone match runs on the 16-bit values.
    if (visible && (visible->width() != image.width() || visible->height() != image.height())) visible = nullptr;
    if (nonzeroBounds(hole).isEmpty()) return true;
    auto eight = narrowImage(image);
    Work W;
    int x0 = 0, y0 = 0;
    if (!exemplarFill(*eight, hole, options, visible, W, x0, y0)) return false;
    std::vector<uint16_t> deep(size_t(W.w) * size_t(W.h) * 4);
    for (int y = 0; y < W.h; y++) std::memcpy(&deep[W.at(0, y) * 4], image.pixel(x0, y0 + y), size_t(W.w) * 4 * sizeof(uint16_t));
    for (size_t p = 0; p < W.from.size(); p++)
        if (W.holeAtStart[p] && W.from[p] >= 0) std::memcpy(&deep[p * 4], &deep[size_t(W.from[p]) * 4], 4 * sizeof(uint16_t));
    const std::vector<uint8_t> known = knownCells(W);
    if (options.toneMatch) matchTone<uint16_t>(deep.data(), W.w, W.h, W.holeAtStart.data(), known.data(), W.from.data(), 2 * W.r + 1, double(one16));
    for (int y = 0; y < W.h; y++)
        for (int x = 0; x < W.w; x++) {
            const size_t p = W.at(x, y);
            if (W.holeAtStart[p]) std::memcpy(image.pixel(x0 + x, y0 + y), &deep[p * 4], 4 * sizeof(uint16_t));
        }
    return true;
}

namespace {
/// A 16-bit mask as the 8-bit decisions read it: anything nonzero stays nonzero.
GrayImage decisions(const Gray16& mask) {
    GrayImage out(mask.width(), mask.height());
    for (int y = 0; y < mask.height(); y++)
        for (int x = 0; x < mask.width(); x++) { const uint16_t v = mask.at(x, y); out.at(x, y) = v ? std::max<uint8_t>(1, narrow16(v)) : 0; }
    return out;
}
} // namespace

bool contentFill(Image16& image, const Gray16& hole, const InpaintOptions& options, const Gray16* visible) {
    const GrayImage hole8 = decisions(hole);
    std::shared_ptr<GrayImage> visible8 = visible ? narrowGray(*visible) : nullptr;
    return contentFill16(image, hole8, options, visible8.get());
}

} // namespace compositor
