// The canvas overlays: transform box, selection ants, scribbles, crop, guides and the tool previews.
#include "CanvasWidget.h"
#include <cstring>
#include "QtGeometry.h"
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

using namespace compositor;

namespace app {

bool CanvasWidget::boxShown() const {
    const Layer* active = session_->activeLayer();
    if (!active) return false;
    if (session_->transformEdit()) return true;
    return session_->tool() == Tool::Move && session_->showsTransformControls && session_->canTransform();
}

void CanvasWidget::drawOverlays(QPainter& painter) {
    painter.setRenderHint(QPainter::Antialiasing, true);
    double ppp = session_->viewport.pointsPerPixel();
    if (session_->showsPixelGrid && ppp >= 8) {
        QRectF docView = documentViewRect();
        QRectF visible = docView.intersected(rect());
        painter.setPen(QPen(QColor(0, 0, 0, 40), 1));
        int x0 = int(std::floor((visible.left() - docView.left()) / ppp)), x1 = int(std::ceil((visible.right() - docView.left()) / ppp));
        int y0 = int(std::floor((visible.top() - docView.top()) / ppp)), y1 = int(std::ceil((visible.bottom() - docView.top()) / ppp));
        for (int x = x0; x <= x1; x++) { double vx = docView.left() + x * ppp; painter.drawLine(QPointF(vx, visible.top()), QPointF(vx, visible.bottom())); }
        for (int y = y0; y <= y1; y++) { double vy = docView.top() + y * ppp; painter.drawLine(QPointF(visible.left(), vy), QPointF(visible.right(), vy)); }
    }
    drawSelectionAnts(painter);
    drawScribbles(painter);
    const Layer* active = session_->activeLayer();
    boxPainted_ = active && boxShown();
    if (boxPainted_) {
        bool distorting = session_->transformEdit() && session_->transformEdit()->corners;
        drawTransformBox(painter, session_->editedCorners(*active), true, distorting);
    }
    painter.setPen(QPen(QColor(255, 0, 200), 1));
    for (double x : session_->snapGuidesX) { double vx = viewPoint({x, 0}).x(); painter.drawLine(QPointF(vx, 0), QPointF(vx, height())); }
    for (double y : session_->snapGuidesY) { double vy = viewPoint({0, y}).y(); painter.drawLine(QPointF(0, vy), QPointF(width(), vy)); }
    if (marquee_) {
        painter.setPen(QPen(Qt::black, 1, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        QRectF r(viewPoint(marquee_->topLeft()), viewPoint(marquee_->bottomRight()));
        if (session_->marqueeKind == MarqueeKind::Ellipse) painter.drawEllipse(r); else painter.drawRect(r);
    }
    if (!lassoPoints_.empty()) {
        QPolygonF poly;
        for (auto& p : lassoPoints_) poly << viewPoint(p);
        if (lassoCursor_) poly << viewPoint(*lassoCursor_);
        painter.setPen(QPen(Qt::white, 3)); painter.drawPolyline(poly);
        painter.setPen(QPen(Qt::black, 1)); painter.drawPolyline(poly);
    }
    if (auto line = session_->gradientLine()) {
        QPointF a = viewPoint(line->first), b = viewPoint(line->second);
        painter.setPen(QPen(Qt::white, 3)); painter.drawLine(a, b);
        painter.setPen(QPen(Qt::black, 1)); painter.drawLine(a, b);
        painter.setBrush(Qt::white);
        painter.drawEllipse(a, 4, 4); painter.drawEllipse(b, 4, 4);
    }
    if (auto& draft = session_->shapeDraft()) {
        QRectF r(viewPoint(draft->rect.topLeft()), viewPoint(draft->rect.bottomRight()));
        painter.setBrush(QColor(session_->foregroundColor.red(), session_->foregroundColor.green(), session_->foregroundColor.blue(), 90));
        painter.setPen(QPen(Qt::black, 1, Qt::DashLine));
        if (draft->kind == ShapeKind::Ellipse) painter.drawEllipse(r);
        else { double rad = std::min({draft->cornerRadius * ppp, r.width() / 2, r.height() / 2}); painter.drawRoundedRect(r, rad, rad); }
        painter.setBrush(Qt::NoBrush);
    }
    if (zoomRect_) {
        painter.setPen(QPen(Qt::white, 1, Qt::DashLine));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(viewPoint(zoomRect_->topLeft()), viewPoint(zoomRect_->bottomRight())));
    }
    drawCropOverlay(painter);
    if (session_->tool() == Tool::CloneStamp && hover_) {
        if (auto sample = session_->cloneSamplePoint(documentPoint(*hover_))) {
            QPointF v = viewPoint(*sample);
            painter.setPen(QPen(Qt::white, 3)); painter.drawLine(v + QPointF(-8, 0), v + QPointF(8, 0)); painter.drawLine(v + QPointF(0, -8), v + QPointF(0, 8));
            painter.setPen(QPen(Qt::black, 1)); painter.drawLine(v + QPointF(-8, 0), v + QPointF(8, 0)); painter.drawLine(v + QPointF(0, -8), v + QPointF(0, 8));
        }
    }
    if (isBrushLike() && hover_ && !spaceHeld_) {
        double r = session_->brushSettings.diameter / 2 * ppp;
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(255, 255, 255, 200), 1));
        painter.drawEllipse(*hover_, r + 0.5, r + 0.5);
        painter.setPen(QPen(QColor(0, 0, 0, 200), 1));
        painter.drawEllipse(*hover_, r, r);
        if (r < 3) { painter.drawLine(*hover_ + QPointF(-6, 0), *hover_ + QPointF(6, 0)); painter.drawLine(*hover_ + QPointF(0, -6), *hover_ + QPointF(0, 6)); }
    }
}

void CanvasWidget::drawTransformBox(QPainter& painter, const Corners& corners, bool active, bool distorting) {
    QPolygonF box;
    for (auto& c : corners) box << viewPoint(toQPoint(c));
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(255, 255, 255, 180), 3));
    painter.drawPolygon(box);
    painter.setPen(QPen(active ? (distorting ? QColor(255, 140, 0) : QColor(0, 122, 255)) : QColor(120, 120, 120), 1));
    painter.drawPolygon(box);
    if (!active) return;
    painter.setBrush(Qt::white);
    for (int i = 0; i < 8; i++) {
        QPointF a = box[i / 2], b = box[(i / 2 + 1) % 4];
        QPointF p = i % 2 == 0 ? a : (a + b) / 2;
        painter.drawRect(QRectF(p.x() - handleRadius + 0.5, p.y() - handleRadius + 0.5, handleRadius * 2 - 1, handleRadius * 2 - 1));
    }
}

void CanvasWidget::drawRasterAnts(QPainter& painter, const GrayImage& coverage) {
    // Sample the selection at every view pixel, mark the inside pixels with an outside neighbour, dash them.
    const int vw = width(), vh = height();
    if (vw <= 0 || vh <= 0) return;
    const QPointF origin = documentPoint(QPointF(0.5, 0.5));
    const QPointF stepX = documentPoint(QPointF(1.5, 0.5)) - origin, stepY = documentPoint(QPointF(0.5, 1.5)) - origin;
    std::vector<uint8_t> mask(size_t(vw) * size_t(vh), 0);
    const int cw = coverage.width(), ch = coverage.height();
    for (int y = 0; y < vh; y++) {
        uint8_t* row = &mask[size_t(y) * size_t(vw)];
        QPointF p = origin + stepY * y;
        for (int x = 0; x < vw; x++, p += stepX) {
            int dx = int(std::floor(p.x())), dy = int(std::floor(p.y()));
            row[x] = dx >= 0 && dy >= 0 && dx < cw && dy < ch && coverage.at(dx, dy) >= 128;
        }
    }
    QImage ants(vw, vh, QImage::Format_ARGB32_Premultiplied);
    ants.fill(Qt::transparent);
    const QRgb black = qRgb(0, 0, 0), white = qRgb(255, 255, 255);
    for (int y = 0; y < vh; y++) {
        const uint8_t* row = &mask[size_t(y) * size_t(vw)];
        QRgb* out = reinterpret_cast<QRgb*>(ants.scanLine(y));
        for (int x = 0; x < vw; x++) {
            if (!row[x]) continue;
            bool edge = x == 0 || y == 0 || x == vw - 1 || y == vh - 1 || !row[x - 1] || !row[x + 1] || !mask[size_t(y - 1) * size_t(vw) + size_t(x)] || !mask[size_t(y + 1) * size_t(vw) + size_t(x)];
            if (edge) out[x] = ((x + y + antsPhase_) & 7) < 4 ? black : white;
        }
    }
    painter.drawImage(0, 0, ants);
}

void CanvasWidget::drawScribbles(QPainter& painter) {
    if (session_->tool() != Tool::Scribble || !session_->hasDocument()) return;
    const double zoom = session_->viewport.zoom;
    auto stroke = [&](const std::vector<QPointF>& points, double size, bool background) {
        if (points.empty()) return;
        QPen pen(background ? QColor(230, 60, 60, 120) : QColor(60, 200, 90, 120));
        pen.setWidthF(std::max(2.0, size * zoom));
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        QPolygonF poly;
        for (const QPointF& p : points) poly << viewPoint(p);
        if (poly.size() == 1) poly << poly.front();
        painter.drawPolyline(poly);
    };
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    if (session_->quickSelectClicks) {
        // Prompts: a dot per click (green the subject, red not), the box as a dashed rectangle.
        std::optional<QPointF> corner;
        for (const EditorSession::ClickPrompt& p : session_->clickPrompts()) {
            if (p.label >= 2) { if (!corner) corner = p.at; else { QPen pen(QColor(60, 200, 90, 200)); pen.setStyle(Qt::DashLine); pen.setWidthF(1.5); painter.setPen(pen); painter.setBrush(Qt::NoBrush); painter.drawRect(QRectF(viewPoint(*corner), viewPoint(p.at)).normalized()); corner.reset(); } continue; }
            painter.setPen(QPen(Qt::white, 1.5));
            painter.setBrush(p.label ? QColor(60, 200, 90, 220) : QColor(230, 60, 60, 220));
            painter.drawEllipse(viewPoint(p.at), 6.0, 6.0);
        }
        if (drag_ == Drag::ClickBox && dragMoved_) { QPen pen(QColor(60, 200, 90, 200)); pen.setStyle(Qt::DashLine); pen.setWidthF(1.5); painter.setPen(pen); painter.setBrush(Qt::NoBrush); painter.drawRect(QRectF(viewPoint(clickStart_), viewPoint(clickCurrent_)).normalized()); }
    } else {
        for (const EditorSession::Scribble& s : session_->scribbles()) stroke(s.points, s.size, s.background);
        if (drag_ == Drag::Scribble) stroke(scribblePoints_, session_->scribbleSize, scribbleBackground_);
    }
    painter.restore();
}

void CanvasWidget::drawSelectionAnts(QPainter& painter) {
    if (selectionRasterAnts_) {
        auto selection = session_->displayedSelection();
        if (selection && selection->coverage) drawRasterAnts(painter, *selection->coverage);
        return;
    }
    if (selectionOutline_.empty()) return;
    painter.setBrush(Qt::NoBrush);
    QPen white(Qt::white, 1);
    QPen black(Qt::black, 1, Qt::CustomDashLine);
    black.setDashPattern({4, 4});
    black.setDashOffset(antsPhase_);
    for (auto& loop : selectionOutline_) {
        QPolygonF poly;
        for (auto& p : loop) poly << viewPoint(p);
        painter.setPen(white); painter.drawPolygon(poly);
        painter.setPen(black); painter.drawPolygon(poly);
    }
}

void CanvasWidget::drawCropOverlay(QPainter& painter) {
    if (!crop_) return;
    QRectF docView = documentViewRect();
    QRectF cropView(viewPoint(crop_->topLeft()), viewPoint(crop_->bottomRight()));
    QPainterPath shade;
    shade.addRect(docView);
    shade.addRect(cropView);
    painter.fillPath(shade, QColor(0, 0, 0, 120));
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(Qt::white, 1));
    painter.drawRect(cropView);
    painter.setPen(QPen(QColor(255, 255, 255, 90), 1));
    for (int i = 1; i < 3; i++) {
        painter.drawLine(QPointF(cropView.left() + cropView.width() * i / 3, cropView.top()), QPointF(cropView.left() + cropView.width() * i / 3, cropView.bottom()));
        painter.drawLine(QPointF(cropView.left(), cropView.top() + cropView.height() * i / 3), QPointF(cropView.right(), cropView.top() + cropView.height() * i / 3));
    }
    painter.setBrush(Qt::white);
    painter.setPen(QPen(Qt::black, 1));
    const QPointF corners[] = {cropView.topLeft(), cropView.topRight(), cropView.bottomRight(), cropView.bottomLeft()};
    for (auto& c : corners) painter.drawRect(QRectF(c.x() - handleRadius, c.y() - handleRadius, handleRadius * 2, handleRadius * 2));
}

void CanvasWidget::refreshSelectionOutline() {
    selectionOutline_.clear();
    selectionRasterAnts_ = false;
    auto selection = session_->displayedSelection();
    if (selection && selection->coverage) {
        bool tooDetailed = false;
        auto loops = selectionOutline(*selection->coverage, &tooDetailed);
        size_t points = 0;
        for (auto& loop : loops) points += loop.size();
        // Past a couple of hundred thousand corners the vector ants cost more than a raster pass per tick.
        // COMPOSITOR_RASTER_ANTS=1 forces the raster path, for checking it.
        if (tooDetailed || points > 200000 || qEnvironmentVariableIsSet("COMPOSITOR_RASTER_ANTS")) selectionRasterAnts_ = true;
        else for (auto& loop : loops) {
            QPolygonF poly;
            for (auto& p : loop) poly << toQPoint(p);
            selectionOutline_.push_back(poly);
        }
    }
    if (selectionOutline_.empty() && !selectionRasterAnts_) antsTimer_.stop(); else if (!antsTimer_.isActive()) antsTimer_.start();
}


} // namespace app
