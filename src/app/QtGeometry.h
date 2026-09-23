// Conversions between Qt's geometry types and the core's.
#pragma once
#include "compositor/geometry.h"
#include <QPointF>
#include <QRectF>

namespace app {

inline compositor::Point toPoint(QPointF p) { return {p.x(), p.y()}; }
inline QPointF toQPoint(compositor::Point p) { return {p.x, p.y}; }
inline QRectF toQRect(const compositor::Rect& r) { return {r.x, r.y, r.width, r.height}; }

} // namespace app
