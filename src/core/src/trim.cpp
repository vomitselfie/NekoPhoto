// A port of upstream Compositor's ImageTrim.swift (MIT, LICENSES/MIT-Compositor.txt).
#include "compositor/trim.h"
#include <cstdlib>

namespace compositor {

std::optional<Rect> trimRect(const Image& flat, const TrimOptions& o) {
    if (!o.any() || flat.isEmpty()) return std::nullopt;
    const int w = flat.width(), h = flat.height();
    const uint8_t* sample = o.basedOn == TrimOptions::BasedOn::TopLeftColor ? flat.pixel(0, 0)
                          : o.basedOn == TrimOptions::BasedOn::BottomRightColor ? flat.pixel(w - 1, h - 1) : nullptr;
    // A pixel that is trimmed away: transparent, or within the tolerance of the corner's colour.
    auto trimmed = [&](int x, int y) {
        const uint8_t* p = flat.pixel(x, y);
        if (!sample) return p[3] == 0;
        for (int c = 0; c < 4; c++) if (std::abs(int(p[c]) - int(sample[c])) > o.tolerance) return false;
        return true;
    };
    int left = w, right = 0, top = h, bottom = 0;
    for (int y = 0; y < h; y++) {
        int first = 0;
        while (first < w && trimmed(first, y)) first++;
        if (first == w) continue;
        int last = w;
        while (last > first && trimmed(last - 1, y)) last--;
        left = std::min(left, first);
        right = std::max(right, last);
        top = std::min(top, y);
        bottom = y + 1;
    }
    if (right == 0) return std::nullopt;   // nothing but what is trimmed
    const int x0 = o.left ? left : 0, y0 = o.top ? top : 0, x1 = o.right ? right : w, y1 = o.bottom ? bottom : h;
    if (x1 <= x0 || y1 <= y0) return std::nullopt;
    return Rect(x0, y0, x1 - x0, y1 - y0);
}

} // namespace compositor
