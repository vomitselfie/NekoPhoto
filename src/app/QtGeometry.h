// Conversions between Qt's geometry types and the core's.
#pragma once
#include "nekophoto/geometry.h"
#include <QPointF>
#include <QRectF>

namespace app {

inline nekophoto::Point toPoint(QPointF p) { return {p.x(), p.y()}; }
inline QPointF toQPoint(nekophoto::Point p) { return {p.x, p.y}; }
inline QRectF toQRect(const nekophoto::Rect& r) { return {r.x, r.y, r.width, r.height}; }

} // namespace app
