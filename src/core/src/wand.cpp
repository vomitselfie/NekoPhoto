#include "compositor/wand.h"
#include "compositor/parallel.h"
#include <algorithm>
#include <cstring>
#include <vector>

namespace compositor {

namespace {

/// The reference colour: the rounded mean over the sample square, clipped to the image.
void referenceColour(const Image& pixels, int seedX, int seedY, int radius, int out[4]) {
    const int x0 = std::max(0, seedX - radius), x1 = std::min(pixels.width() - 1, seedX + radius);
    const int y0 = std::max(0, seedY - radius), y1 = std::min(pixels.height() - 1, seedY + radius);
    unsigned long sums[4] = {0, 0, 0, 0}, samples = 0;
    for (int y = y0; y <= y1; y++) {
        const uint8_t* p = pixels.row(y) + size_t(x0) * 4;
        for (int x = x0; x <= x1; x++, p += 4, samples++) for (int c = 0; c < 4; c++) sums[c] += p[c];
    }
    for (int c = 0; c < 4; c++) out[c] = int((sums[c] + samples / 2) / samples);
}

/// One row's match bytes (`hit` where every channel lies in [lo, hi], 0 elsewhere).
void matchRow(const uint8_t* p, int width, const uint8_t lo[4], const uint8_t hi[4], uint8_t hit, uint8_t* out) {
    for (int x = 0; x < width; x++, p += 4) {
        // min/max against the bounds instead of two compares per channel: pminub/pmaxub in vector form.
        unsigned inside = 1;
        for (int c = 0; c < 4; c++) inside &= unsigned(std::max(p[c], lo[c]) == p[c]) & unsigned(std::min(p[c], hi[c]) == p[c]);
        out[x] = inside ? hit : 0;
    }
}

} // namespace

long wandMask(const Image& pixels, int seedX, int seedY, int radius, int tolerance, bool contiguous, GrayImage& mask) {
    const int width = pixels.width(), height = pixels.height();
    std::memset(mask.data(), 0, mask.byteCount());
    if (width <= 0 || height <= 0 || seedX < 0 || seedY < 0 || seedX >= width || seedY >= height) return 0;
    int reference[4];
    referenceColour(pixels, seedX, seedY, std::max(0, radius), reference);
    uint8_t lo[4], hi[4];
    for (int c = 0; c < 4; c++) { lo[c] = uint8_t(std::max(0, reference[c] - tolerance)); hi[c] = uint8_t(std::min(255, reference[c] + tolerance)); }

    // Every pixel tested once: 255 for a match when that is the answer, 1 for a candidate the fill may reach.
    const uint8_t hit = contiguous ? 1 : 255;
    parallelRows(0, height, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) matchRow(pixels.row(y), width, lo, hi, hit, mask.row(y));
    });
    long count = 0;
    if (!contiguous) {
        for (int y = 0; y < height; y++) { const uint8_t* row = mask.row(y); for (int x = 0; x < width; x++) count += row[x] != 0; }
        return count;
    }

    // Scanline flood fill over the candidate map: each popped seed fills its whole run, then pushes one seed
    // per candidate run in the rows directly above and below it.
    std::vector<std::pair<int, int>> stack;
    stack.emplace_back(seedX, seedY);
    while (!stack.empty()) {
        auto [x, y] = stack.back();
        stack.pop_back();
        uint8_t* row = mask.row(y);
        if (row[x] != 1) continue;
        int left = x, right = x;
        while (left > 0 && row[left - 1] == 1) left--;
        while (right + 1 < width && row[right + 1] == 1) right++;
        std::memset(row + left, 255, size_t(right - left + 1));
        count += right - left + 1;
        for (int ny : {y - 1, y + 1}) {
            if (ny < 0 || ny >= height) continue;
            const uint8_t* next = mask.row(ny);
            bool inRun = false;
            for (int nx = left; nx <= right; nx++) {
                bool candidate = next[nx] == 1;
                if (candidate && !inRun) stack.emplace_back(nx, ny);
                inRun = candidate;
            }
        }
    }
    // Candidates the fill never reached go back to unselected.
    parallelRows(0, height, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) { uint8_t* row = mask.row(y); for (int x = 0; x < width; x++) row[x] = row[x] == 255 ? 255 : 0; }
    });
    return count;
}

} // namespace compositor
