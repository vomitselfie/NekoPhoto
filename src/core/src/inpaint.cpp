// Content-aware fill by classic exemplar-based inpainting (Criminisi, Perez & Toyama 2003, whose patent, Microsoft
// US 6987520, expired 2023-03-30; Efros & Leung 1999). See inpaint.h for the legal boundary this file keeps; in short:
// the fill front advances in a fixed deterministic priority order, every target patch is matched by an exhaustive scan
// of a bounded window at the working resolution, nothing found for one patch is reused, propagated, perturbed or
// stored for another, and the only colour adaptation is the boundary membrane on the low-pass band.
//
// The fill works on the document's own samples: RGB's red, green and blue, Lab's L, a and b, CMYK's four inks (as
// stored, inverted), each with alpha last. Nothing is converted: in CMYK a hole fills with the inks of the patches it
// copies, so K-only black stays K-only.
#include "nekophoto/inpaint.h"
#include "nekophoto/depth.h"
#include "nekophoto/heal.h"
#include "nekophoto/parallel.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <vector>
#if defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace nekophoto {

namespace {

enum State : uint8_t { Unknown = 0, Known = 1, Filled = 2, Hole = 3 };
constexpr int32_t kFullConfidence = 4096;
constexpr int64_t kDataFloor = 64;   // keeps flat areas advancing by confidence alone

/// The work region: the hole with the search window's reach around it, in its own coordinates.
struct Work {
    int w = 0, h = 0, r = 4;
    int n = 4;                       // samples per pixel, alpha last: 4 (RGB, Lab) or 5 (CMYK)
    ColorMode mode = ColorMode::RGB;
    std::vector<uint8_t> pix;        // the document's own samples at 8 bits, premultiplied
    std::vector<uint8_t> state;
    std::vector<uint8_t> holeAtStart;
    std::vector<uint8_t> sourceOk;   // the patch centred here is inside the region and wholly Known
    std::vector<uint32_t> sourceSum; // sourceOk's summed-area table, (w + 1) x (h + 1), modulo 2^32
    std::vector<int32_t> confidence;
    std::vector<int32_t> from;       // for each filled pixel, the region index of the pixel it copies
    size_t at(int x, int y) const { return size_t(y) * size_t(w) + size_t(x); }
    bool inside(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h; }
    /// The lightness the data term follows: Lab's L; in CMYK the plates' look (C, M and Y as stored, the inks
    /// inverted, times K as stored); in RGB the luma.
    int luma(size_t p) const {
        const uint8_t* q = &pix[p * size_t(n)];
        if (mode == ColorMode::Lab) return q[0];
        const int rgb = (q[0] * 77 + q[1] * 151 + q[2] * 28) >> 8;
        return mode == ColorMode::CMYK ? rgb * q[3] / 255 : rgb;
    }
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
    const size_t sw = size_t(w) + 1;
    W.sourceSum.assign(sw * (size_t(h) + 1), 0);
    for (int y = 0; y < h; y++) {
        uint32_t run = 0;
        for (int x = 0; x < w; x++) {
            run += W.sourceOk[W.at(x, y)];
            W.sourceSum[(size_t(y) + 1) * sw + size_t(x) + 1] = W.sourceSum[size_t(y) * sw + size_t(x) + 1] + run;
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

/// The target patch as the scan compares it: per patch row, the target's samples and a mask of its seen pixels, both
/// padded to whole 16-byte blocks, so a source row is compared with a few byte-wide absolute-difference sums. Pixels
/// the target has not seen are masked out of the source and are zero in the target: they add nothing, as before.
struct Template {
    int blocks = 0, stride = 0;                // 16-byte blocks per patch row, bytes per patch row
    std::vector<uint8_t> values, mask;
    /// The patch rows with any seen pixel (their offsets from the patch's top-left sample, in the region, and in the
    /// template), the fullest first: the sum is the same in any order, and the big rows pass a bound soonest.
    struct Row { ptrdiff_t source; size_t at; };
    std::vector<Row> rows;
};

template <int N>
Template templateAt(const Work& W, int tx, int ty) {
    const int r = W.r, side = 2 * r + 1;
    Template T;
    T.blocks = (side * N + 15) / 16;
    T.stride = T.blocks * 16;
    T.values.assign(size_t(side) * size_t(T.stride), 0);
    T.mask.assign(size_t(side) * size_t(T.stride), 0);
    std::vector<std::pair<int, int>> counted;  // (seen pixels, row)
    for (int j = -r; j <= r; j++) {
        int seen = 0;
        for (int i = -r; i <= r; i++) {
            if (!W.seen(tx + i, ty + j)) continue;
            const size_t at = size_t(j + r) * size_t(T.stride) + size_t(i + r) * N;
            std::memcpy(&T.values[at], &W.pix[W.at(tx + i, ty + j) * N], N);
            std::memset(&T.mask[at], 0xff, N);
            seen++;
        }
        if (seen) counted.push_back({seen, j + r});
    }
    std::stable_sort(counted.begin(), counted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (const auto& [seen, row] : counted) T.rows.push_back({ptrdiff_t(row) * W.w * N, size_t(row) * size_t(T.stride)});
    return T;
}

/// The L1 distance over the target's seen pixels between the template and the source patch whose top-left sample is
/// `source`, in `B` blocks a row, added to `sum`; it stops, returning a sum past `bound`, once a patch row takes it past
/// `bound`. A sum that passes the bound can only grow, so the candidate could not have won: stopping early changes no
/// result.
template <int B>
int64_t patchDistance(const Template& T, const uint8_t* source, int64_t sum, int64_t bound) {
    for (const Template::Row& row : T.rows) {
        const uint8_t* s = source + row.source;
        const uint8_t* t = &T.values[row.at];
        const uint8_t* m = &T.mask[row.at];
#if defined(__SSE2__)
        __m128i acc = _mm_setzero_si128();
        for (int b = 0; b < B; b++) {
            const __m128i sv = _mm_and_si128(_mm_loadu_si128(reinterpret_cast<const __m128i*>(s + b * 16)), _mm_loadu_si128(reinterpret_cast<const __m128i*>(m + b * 16)));
            acc = _mm_add_epi64(acc, _mm_sad_epu8(sv, _mm_loadu_si128(reinterpret_cast<const __m128i*>(t + b * 16))));
        }
        sum += _mm_cvtsi128_si64(acc) + _mm_cvtsi128_si64(_mm_unpackhi_epi64(acc, acc));
#else
        int part = 0;
        for (int k = 0; k < B * 16; k++) part += std::abs(int(t[k]) - int(s[k] & m[k]));
        sum += part;
#endif
        if (sum > bound) break;
    }
    return sum;
}

/// Whole patches whose centres lie in [x0, x1] x [y0, y1] (inclusive), from the region's summed-area table: exact in
/// unsigned arithmetic, since a window's count fits in 32 bits.
uint32_t sourcesIn(const Work& W, int x0, int y0, int x1, int y1) {
    const size_t sw = size_t(W.w) + 1;
    const auto at = [&](int x, int y) { return W.sourceSum[size_t(y) * sw + size_t(x)]; };
    return at(x1 + 1, y1 + 1) - at(x0, y1 + 1) - at(x1 + 1, y0) + at(x0, y0);
}

/// The scan behind bestSource with `B` blocks a patch row. Every source centre in the window is a candidate; one is
/// passed over only once its distance provably exceeds one already found in this same scan (the offset penalty alone,
/// or a partial sum), and rows holding no whole patch are passed over whole, which leaves the winner what scoring every
/// candidate in full would choose. The scan scores every eighth candidate of every eighth row first, at full
/// resolution and against this target alone: the best of those bounds the winner's distance, so every band of the
/// parallel scan starts passing over candidates at once. The bound is dropped when the scan ends, like everything else
/// here, and the winner is still the exhaustive scan's: the smallest distance, then the earliest in scan order.
template <int B>
Candidate scanWith(const Work& W, const Template& T, int tx, int ty, int x0, int y0, int x1, int y1, int penalty) {
    const size_t corner = size_t(W.r) * size_t(W.w) + size_t(W.r);   // a patch's centre to its top-left pixel
    const size_t N = size_t(W.n);
    // One candidate scored against `bound`: its distance, past `bound` when it cannot win.
    auto score = [&](int x, int y, int64_t bound) {
        const int64_t sum = int64_t(penalty) * (std::abs(x - tx) + std::abs(y - ty));
        if (sum > bound) return sum;
        return patchDistance<B>(T, &W.pix[(W.at(x, y) - corner) * N], sum, bound);
    };
    int64_t bound = std::numeric_limits<int64_t>::max();
    constexpr int sparse = 8;
    for (int y = y0; y <= y1; y += sparse)
        for (int x = x0; x <= x1; x += sparse)
            if (W.sourceOk[W.at(x, y)]) bound = std::min(bound, score(x, y, bound));
    Candidate best;
    std::mutex lock;
    parallelRows(y0, y1 + 1, [&](int ya, int yb) {
        // Starting from the bound: a candidate at it or under it is still taken, so the one the sparse pass found is
        // met again in its place in scan order.
        Candidate local;
        local.distance = bound;
        for (int y = ya; y < yb; y++) {
            if (int64_t(penalty) * std::abs(y - ty) > local.distance || !sourcesIn(W, x0, y, x1, y)) continue;
            const uint8_t* ok = &W.sourceOk[W.at(0, y)];
            for (int x = x0; x <= x1; x++) {
                if (!ok[x]) continue;
                const int64_t sum = score(x, y, local.distance);
                if (sum > local.distance) continue;
                const int64_t order = int64_t(y) * W.w + x;
                if (sum < local.distance || order < local.order) local = {sum, order, x, y};
            }
        }
        if (local.x < 0) return;
        std::lock_guard<std::mutex> guard(lock);
        if (local.distance < best.distance || (local.distance == best.distance && local.order < best.order)) best = local;
    }, 4);
    return best;
}

/// The scan behind bestSource, over `N` samples per pixel. A window holding no whole patch finds nothing, as its scan
/// would.
template <int N>
Candidate scanWindow(const Work& W, int tx, int ty, int radius, int penalty) {
    const int r = W.r;
    const int y0 = std::max(r, ty - radius), y1 = std::min(W.h - r - 1, ty + radius);
    const int x0 = std::max(r, tx - radius), x1 = std::min(W.w - r - 1, tx + radius);
    if (y1 < y0 || x1 < x0 || !sourcesIn(W, x0, y0, x1, y1)) return {};
    const Template T = templateAt<N>(W, tx, ty);
    switch (T.blocks) {
    case 1: return scanWith<1>(W, T, tx, ty, x0, y0, x1, y1, penalty);
    case 2: return scanWith<2>(W, T, tx, ty, x0, y0, x1, y1, penalty);
    case 3: return scanWith<3>(W, T, tx, ty, x0, y0, x1, y1, penalty);
    case 4: return scanWith<4>(W, T, tx, ty, x0, y0, x1, y1, penalty);
    case 5: return scanWith<5>(W, T, tx, ty, x0, y0, x1, y1, penalty);
    default: return scanWith<6>(W, T, tx, ty, x0, y0, x1, y1, penalty);   // 17 x 5 samples: 85 bytes
    }
}

/// The best source patch for the target at (tx, ty): every source centre within `radius` is scored, independently
/// of any other target, on every sample (CMYK's inks and alpha, so a K-only black and a rich black differ). The
/// parallel reduction keeps the smallest distance, then the earliest in scan order, so the result does not depend on
/// how the rows are split.
Candidate bestSource(const Work& W, int tx, int ty, int radius, int penalty) {
    return W.n == 5 ? scanWindow<5>(W, tx, ty, radius, penalty) : scanWindow<4>(W, tx, ty, radius, penalty);
}

/// Tone match: the healing membrane on the low-pass band. At each pixel b of the ring just outside the hole, the
/// destination-minus-source offset of the low-pass band is taken: the local mean of the surroundings at b minus the
/// local mean at the pixel b was matched against (b's place relative to the copied patch next to it, at the patch's
/// source), both read from the untouched known pixels. Those boundary offsets are interpolated across the hole
/// (Dirichlet interpolation of boundary values, heal.h, the expired US 6587592 membrane) and added to the fill, so a
/// patch copied from a darker or lighter place takes the tone of where it lands. Matching content has equal local
/// means at both places, so a pattern copied in phase is left alone; the fill's detail is untouched and no gradient
/// is composited (US 9058699). `n` samples per pixel, alpha last: each colour sample (every ink in CMYK, L, a and b
/// in Lab) takes its own offset.
template <typename T>
void matchTone(T* pix, int n, int w, int h, const uint8_t* hole, const uint8_t* known, const int32_t* from, int radius, double one) {
    const size_t count = size_t(w) * size_t(h), N = size_t(n);
    // A summed-area table of the known cells and their count: their local means.
    const size_t sw = size_t(w) + 1;
    std::vector<double> sum((size_t(h) + 1) * sw * N, 0);
    std::vector<int> cells((size_t(h) + 1) * sw, 0);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const size_t p = size_t(y) * w + size_t(x), s = (size_t(y) + 1) * sw + size_t(x) + 1;
            const size_t up = s - sw, left = s - 1, diag = s - sw - 1;
            const bool isK = known[p];
            cells[s] = cells[up] + cells[left] - cells[diag] + isK;
            for (size_t c = 0; c < N; c++)
                sum[s * N + c] = sum[up * N + c] + sum[left * N + c] - sum[diag * N + c] + (isK ? double(pix[p * N + c]) : 0);
        }
    auto mean = [&](int x, int y, double* out) {
        const int ax = std::max(0, x - radius), ay = std::max(0, y - radius), bx = std::min(w, x + radius + 1), by = std::min(h, y + radius + 1);
        const size_t a = size_t(ay) * sw + size_t(ax), b = size_t(ay) * sw + size_t(bx), c0 = size_t(by) * sw + size_t(ax), d = size_t(by) * sw + size_t(bx);
        const int k = cells[d] - cells[b] - cells[c0] + cells[a];
        if (!k) return false;
        for (size_t c = 0; c < N; c++) out[c] = (sum[d * N + c] - sum[b * N + c] - sum[c0 * N + c] + sum[a * N + c]) / k;
        return true;
    };
    std::vector<float> values(count * N, 0.0f);
    std::vector<uint8_t> ring(count, 0);
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
            double here[5], there[5];
            if (!mean(x, y, here) || !mean(sx, sy, there)) continue;
            for (size_t c = 0; c < N; c++) values[p * N + c] = float(here[c] - there[c]);
            ring[p] = 1;
            any = true;
        }
    if (!any) return;
    membraneFill(values.data(), n, hole, ring.data(), w, h);
    const size_t alpha = N - 1;
    for (size_t p = 0; p < count; p++) {
        if (!hole[p]) continue;
        T* o = &pix[p * N];
        const double a = std::clamp(double(o[alpha]) + values[p * N + alpha], 0.0, one);
        o[alpha] = T(std::lround(a));
        for (size_t c = 0; c < alpha; c++) o[c] = T(std::lround(std::clamp(double(o[c]) + values[p * N + c], 0.0, double(o[alpha]))));
    }
}

/// The fill itself over the region of `image` (8-bit samples, `image.channels()` of them, alpha last) around the hole.
/// On success `W` holds the filled region, with `from` naming where every filled pixel came from (for the 16-bit
/// copy), and (x0, y0) its place in the image.
template <class Eight>
bool exemplarFill(const Eight& image, int channels, ColorMode mode, const GrayImage& hole, const InpaintOptions& options, const GrayImage* visible, Work& W, int& x0, int& y0) {
    const int r = std::clamp(options.patchRadius, 1, 8);
    const bool wide = visible && options.sampleWholeVisible;
    const int radius = std::max(2 * r + 1, wide ? options.wideSearchRadius : options.searchRadius);
    const int penalty = std::max(0, options.offsetPenalty);
    const PixelBounds b = nonzeroBounds(hole);
    if (b.isEmpty()) return true;
    const int reach = radius + 2 * r + 1;
    x0 = std::max(0, b.x0 - reach); y0 = std::max(0, b.y0 - reach);
    const int x1 = std::min(image.width(), b.x1 + reach), y1 = std::min(image.height(), b.y1 + reach);
    W.w = x1 - x0; W.h = y1 - y0; W.r = r; W.n = channels; W.mode = mode;
    const size_t n = size_t(W.w) * size_t(W.h), N = size_t(channels);
    // Sixteen spare bytes: the scan reads each source patch row in whole 16-byte blocks (masked), past its last pixel.
    W.pix.resize(n * N + 16); W.state.assign(n, Unknown); W.confidence.assign(n, 0); W.from.assign(n, -1);
    for (int y = 0; y < W.h; y++) {
        std::memcpy(&W.pix[W.at(0, y) * N], image.pixel(x0, y0 + y), size_t(W.w) * N);
        for (int x = 0; x < W.w; x++) {
            const size_t p = W.at(x, y);
            if (hole.at(x0 + x, y0 + y)) { W.state[p] = Hole; continue; }
            if (image.pixel(x0 + x, y0 + y)[channels - 1] == 255 && (!visible || visible->at(x0 + x, y0 + y) >= 128)) { W.state[p] = Known; W.confidence[p] = kFullConfidence; }
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
                std::memcpy(&W.pix[p * N], &W.pix[s * N], N);
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

/// The fill at 8 bits, in place: the patches copied are the image's own samples, so the region's filled pixels go back
/// as they are (after the tone match).
template <class Eight>
bool fillEight(Eight& image, int channels, ColorMode mode, const GrayImage& hole, const InpaintOptions& options, const GrayImage* visible) {
    if (visible && (visible->width() != image.width() || visible->height() != image.height())) visible = nullptr;
    if (nonzeroBounds(hole).isEmpty()) return true;
    Work W;
    int x0 = 0, y0 = 0;
    if (!exemplarFill(image, channels, mode, hole, options, visible, W, x0, y0)) return false;
    const std::vector<uint8_t> known = knownCells(W);
    if (options.toneMatch) matchTone<uint8_t>(W.pix.data(), channels, W.w, W.h, W.holeAtStart.data(), known.data(), W.from.data(), 2 * W.r + 1, 255.0);
    const size_t N = size_t(channels);
    for (int y = 0; y < W.h; y++)
        for (int x = 0; x < W.w; x++) {
            const size_t p = W.at(x, y);
            if (W.holeAtStart[p]) std::memcpy(image.pixel(x0 + x, y0 + y), &W.pix[p * N], N);
        }
    return true;
}

} // namespace

bool contentFill(Image& image, const GrayImage& hole, const InpaintOptions& options, const GrayImage* visible) {
    return fillEight(image, 4, options.colorMode == ColorMode::Lab ? ColorMode::Lab : ColorMode::RGB, hole, options, visible);
}

bool contentFill(ImageC8& image, const GrayImage& hole, const InpaintOptions& options, const GrayImage* visible) {
    if (image.channels() != 5) return false;
    return fillEight(image, 5, ColorMode::CMYK, hole, options, visible);
}

bool contentFill16(Image16& image, const GrayImage& hole, const InpaintOptions& options, const GrayImage* visible) {
    // The order and the patches are chosen on the image rounded to 8 bits; every filled pixel then takes the 16-bit
    // pixel its 8-bit copy came from, and the tone match runs on the 16-bit values. Five channels are CMYK's.
    if (visible && (visible->width() != image.width() || visible->height() != image.height())) visible = nullptr;
    if (nonzeroBounds(hole).isEmpty()) return true;
    const int channels = image.channels();
    if (channels != 4 && channels != 5) return false;
    const size_t N = size_t(channels);
    Work W;
    int x0 = 0, y0 = 0;
    if (channels == 5) {
        auto eight = narrowImageC8(image);
        if (!exemplarFill(*eight, 5, ColorMode::CMYK, hole, options, visible, W, x0, y0)) return false;
    } else {
        auto eight = narrowImage(image);
        if (!exemplarFill(*eight, 4, options.colorMode == ColorMode::Lab ? ColorMode::Lab : ColorMode::RGB, hole, options, visible, W, x0, y0)) return false;
    }
    std::vector<uint16_t> deep(size_t(W.w) * size_t(W.h) * N);
    for (int y = 0; y < W.h; y++) std::memcpy(&deep[W.at(0, y) * N], image.pixel(x0, y0 + y), size_t(W.w) * N * sizeof(uint16_t));
    for (size_t p = 0; p < W.from.size(); p++)
        if (W.holeAtStart[p] && W.from[p] >= 0) std::memcpy(&deep[p * N], &deep[size_t(W.from[p]) * N], N * sizeof(uint16_t));
    const std::vector<uint8_t> known = knownCells(W);
    if (options.toneMatch) matchTone<uint16_t>(deep.data(), channels, W.w, W.h, W.holeAtStart.data(), known.data(), W.from.data(), 2 * W.r + 1, double(one16));
    for (int y = 0; y < W.h; y++)
        for (int x = 0; x < W.w; x++) {
            const size_t p = W.at(x, y);
            if (W.holeAtStart[p]) std::memcpy(image.pixel(x0 + x, y0 + y), &deep[p * N], N * sizeof(uint16_t));
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

} // namespace nekophoto
