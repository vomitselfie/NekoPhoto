#include "compositor/bucket.h"
#include <algorithm>
#include <vector>

namespace compositor {

namespace {

template <class Img>
long bucketOf(const Img& image, int n, double one, int seedX, int seedY, int tolerance, bool contiguous, GrayImage& mask) {
    const int w = image.width(), h = image.height();
    mask = GrayImage(w, h, 0);
    if (seedX < 0 || seedY < 0 || seedX >= w || seedY >= h) return 0;
    const double tol = std::clamp(tolerance, 0, 255) * one / 255.0;
    double reference[5];
    for (int c = 0; c < n; c++) reference[c] = image.pixel(seedX, seedY)[c];
    auto matches = [&](int x, int y) {
        const auto* p = image.pixel(x, y);
        for (int c = 0; c < n; c++) if (std::abs(double(p[c]) - reference[c]) > tol) return false;
        return true;
    };
    long count = 0;
    if (!contiguous) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                if (matches(x, y)) { mask.at(x, y) = 255; count++; }
        return count;
    }
    std::vector<uint8_t> seen(size_t(w) * size_t(h), 0);
    std::vector<std::pair<int, int>> stack{{seedX, seedY}};
    seen[size_t(seedY) * w + size_t(seedX)] = 1;
    while (!stack.empty()) {
        const auto [x, y] = stack.back();
        stack.pop_back();
        if (!matches(x, y)) continue;
        mask.at(x, y) = 255;
        count++;
        const int next[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
        for (auto& o : next) {
            if (o[0] < 0 || o[1] < 0 || o[0] >= w || o[1] >= h) continue;
            uint8_t& s = seen[size_t(o[1]) * w + size_t(o[0])];
            if (s) continue;
            s = 1;
            stack.push_back({o[0], o[1]});
        }
    }
    return count;
}

} // namespace

long bucketMask(const AnyImage& image, int seedX, int seedY, int tolerance, bool contiguous, GrayImage& mask) {
    if (image.u16()) return bucketOf(*image.u16(), image.u16()->channels(), 32768.0, seedX, seedY, tolerance, contiguous, mask);
    if (image.c8()) return bucketOf(*image.c8(), image.c8()->channels(), 255.0, seedX, seedY, tolerance, contiguous, mask);
    if (image.u8()) return bucketOf(*image.u8(), 4, 255.0, seedX, seedY, tolerance, contiguous, mask);
    return 0;
}

} // namespace compositor
