// Document: pixels with top-left origin. View: widget points. Zoom 1 means one
// document pixel per device pixel, whatever the display scale. A port of
// Rendering/CanvasViewport.swift.
#pragma once
#include <QPointF>
#include <QRectF>
#include <QSizeF>
#include <algorithm>
#include <cmath>

namespace app {

struct Viewport {
    QSizeF viewSize;
    double backingScale = 1;
    double zoom = 1;
    QPointF pan;
    bool followsFit = true;
    static constexpr double minZoom = 0.001, maxZoom = 32;

    double pointsPerPixel() const { return zoom / backingScale; }
    QPointF center() const { return {viewSize.width() / 2, viewSize.height() / 2}; }

    QRectF documentRect(QSizeF size) const {
        QSizeF scaled(size.width() * pointsPerPixel(), size.height() * pointsPerPixel());
        return {center().x() - scaled.width() / 2 + pan.x(), center().y() - scaled.height() / 2 + pan.y(), scaled.width(), scaled.height()};
    }
    QPointF documentPoint(QPointF viewPoint, QSizeF documentSize) const {
        QPointF origin = documentRect(documentSize).topLeft();
        return {(viewPoint.x() - origin.x()) / pointsPerPixel(), (viewPoint.y() - origin.y()) / pointsPerPixel()};
    }
    QPointF viewPoint(QPointF documentPoint, QSizeF documentSize) const {
        QPointF origin = documentRect(documentSize).topLeft();
        return {origin.x() + documentPoint.x() * pointsPerPixel(), origin.y() + documentPoint.y() * pointsPerPixel()};
    }
    void fit(QSizeF documentSize) {
        if (viewSize.width() <= 0 || viewSize.height() <= 0) { followsFit = true; return; }
        zoom = clampZoom(std::min(std::max(1.0, viewSize.width() - 96) / documentSize.width(), std::max(1.0, viewSize.height() - 96) / documentSize.height()) * backingScale);
        pan = {};
        followsFit = true;
    }
    void resize(QSizeF size, double newScale, const QSizeF* documentSize) {
        double oldScale = pointsPerPixel();
        viewSize = size;
        backingScale = std::max(1.0, newScale);
        if (followsFit && documentSize) fit(*documentSize);
        else { double ratio = pointsPerPixel() / oldScale; pan = {pan.x() * ratio, pan.y() * ratio}; }
    }
    void setZoom(double value, QPointF anchor, QSizeF documentSize) {
        if (!std::isfinite(value)) return;
        QPointF pixel = documentPoint(anchor, documentSize);
        zoom = clampZoom(value);
        QPointF moved = viewPoint(pixel, documentSize);
        pan += anchor - moved;
        followsFit = false;
    }
    /// Moves the view by `delta` points, rounded to whole device pixels so the canvas can scroll what it has
    /// rendered instead of rendering the view again.
    void translate(QPointF delta) {
        const double s = std::max(1.0, backingScale);
        pan += QPointF(std::round(delta.x() * s) / s, std::round(delta.y() * s) / s);
        followsFit = false;
    }
    static double clampZoom(double value) { return std::min(maxZoom, std::max(minZoom, value)); }
};

} // namespace app
