// A vector path (compositor/vectormask.h) as a QPainterPath, each point mapped by `map` (document to view).
#pragma once
#include "compositor/vectormask.h"
#include <QPainterPath>
#include <functional>

namespace app {

inline QPainterPath painterPath(const compositor::VectorPath& path, const std::function<QPointF(double, double)>& map) {
    QPainterPath out;
    out.setFillRule(Qt::WindingFill);
    for (const auto& s : path.subpaths) {
        if (s.knots.empty()) continue;
        out.moveTo(map(s.knots[0].x, s.knots[0].y));
        const size_t n = s.knots.size();
        for (size_t i = 1; i <= n; i++) {
            if (i == n && !s.closed) break;
            const auto& a = s.knots[i - 1];
            const auto& b = s.knots[i % n];
            out.cubicTo(map(a.outX, a.outY), map(b.inX, b.inY), map(b.x, b.y));
        }
        if (s.closed) out.closeSubpath();
    }
    return out;
}

} // namespace app
