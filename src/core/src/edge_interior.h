// The run of a row's output pixels whose sample point lies well inside a layer, where drawLayer's edge antialias,
// clamp(min(ex, ey) + 0.5, 0, 1) with ex = min(p.x, w - p.x) / sx (and ey alike), is exactly 1.
#pragma once
#include "compositor/geometry.h"
#include <algorithm>
#include <cmath>

namespace compositor {

/// Pixels [from, to) of a row of `count` starting at `x0`, whose sample point starts at `p0` and moves by `dp` per pixel
/// (accumulated, p = p + dp), keep at least half a source pixel plus a margin from every edge of a `w` x `h` layer, so
/// the edge formula gives exactly 1 there. The margin (a thousandth of a pixel) covers the rounding the accumulated
/// point picks up over a row, so the run never takes a pixel whose computed edge would be below 1.
inline void edgeInterior(Point p0, Point dp, double sx, double sy, int w, int h, int x0, int count, int& from, int& to) {
    from = to = x0;
    if (!(sx >= 1e-9) || !(sy >= 1e-9) || count <= 0) return;
    constexpr double margin = 1e-3;
    double lo = 0, hi = double(count - 1);
    bool empty = false;
    auto constrain = [&](double value, double step, double min, double max) {
        // value + t * step within [min, max]
        if (std::fabs(step) < 1e-12) { if (value < min || value > max) empty = true; return; }
        double t0 = (min - value) / step, t1 = (max - value) / step;
        if (t0 > t1) std::swap(t0, t1);
        lo = std::max(lo, t0);
        hi = std::min(hi, t1);
    };
    constrain(p0.x, dp.x, 0.5 * sx + margin, w - 0.5 * sx - margin);
    constrain(p0.y, dp.y, 0.5 * sy + margin, h - 0.5 * sy - margin);
    if (empty || !(lo <= hi)) return;
    const int a = int(std::ceil(lo)), b = int(std::floor(hi)) + 1;
    if (b <= a) return;
    from = x0 + std::clamp(a, 0, count);
    to = x0 + std::clamp(b, 0, count);
}

} // namespace compositor
