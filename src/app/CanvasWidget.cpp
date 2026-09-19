#include "CanvasWidget.h"
#include "ImageConvert.h"
#include "compositor/render.h"
#include "compositor/selection.h"
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTabletEvent>
#include <QWheelEvent>
#include <cmath>

using namespace compositor;

namespace app {

namespace {

constexpr double handleRadius = 5;   // points
constexpr double rotateReach = 22;   // points beyond a corner that still rotates
constexpr double dragThreshold = 3;  // points before a press becomes a drag
constexpr double snapDistance = 10;  // points

Point toPoint(QPointF p) { return {p.x(), p.y()}; }
QPointF toQPoint(Point p) { return {p.x, p.y}; }

QPixmap checkerPixmap(double dpr) {
    int cell = int(std::round(8 * dpr));
    QPixmap pixmap(cell * 2, cell * 2);
    pixmap.setDevicePixelRatio(dpr);
    QPainter p(&pixmap);
    p.fillRect(0, 0, cell * 2, cell * 2, QColor(255, 255, 255));
    p.fillRect(0, 0, cell, cell, QColor(204, 204, 204));
    p.fillRect(cell, cell, cell, cell, QColor(204, 204, 204));
    return pixmap;
}

QCursor magnifierCursor(bool out, double dpr) {
    int s = 24;
    QPixmap pm(int(s * dpr), int(s * dpr));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(Qt::white, 3.5)); p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(10, 10), 7, 7); p.drawLine(QPointF(15, 15), QPointF(22, 22));
    p.setPen(QPen(Qt::black, 1.5));
    p.drawEllipse(QPointF(10, 10), 7, 7); p.drawLine(QPointF(15, 15), QPointF(22, 22));
    p.drawLine(QPointF(6.5, 10), QPointF(13.5, 10));
    if (!out) p.drawLine(QPointF(10, 6.5), QPointF(10, 13.5));
    return QCursor(pm, 10, 10);
}

} // namespace

CanvasWidget::CanvasWidget(EditorSession* session, QWidget* parent) : QWidget(parent), session_(session) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setAttribute(Qt::WA_TabletTracking);
    connect(session_, &EditorSession::documentChanged, this, [this](QRectF region) {
        if (region.isEmpty()) { cacheValid_ = false; update(); }
        else invalidate(region);
    });
    connect(session_, &EditorSession::layersChanged, this, [this] { update(); });
    connect(session_, &EditorSession::selectionChanged, this, [this] { refreshSelectionOutline(); update(); });
    connect(session_, &EditorSession::viewportChanged, this, [this] { cacheValid_ = false; update(); });
    connect(session_, &EditorSession::toolChanged, this, [this] {
        if (session_->tool() != Tool::Crop) crop_.reset();
        if (session_->tool() != Tool::Lasso) { lassoPoints_.clear(); lassoCursor_.reset(); }
        if (session_->tool() == Tool::Crop && !crop_ && session_->hasDocument()) { crop_ = QRectF(QPointF(0, 0), documentSize()); emit cropChanged(); }
        if (hover_) updateCursor(*hover_, QApplication::keyboardModifiers());
        update();
    });
    connect(session_, &EditorSession::transformChanged, this, [this] { if (session_->transformEdit() && session_->transformEdit()->floating) refreshSelectionOutline(); update(); });
    antsTimer_.setInterval(120);
    connect(&antsTimer_, &QTimer::timeout, this, [this] { antsPhase_ = (antsPhase_ + 1) % 8; if (!selectionOutline_.empty() || selectionRasterAnts_) update(); });
    zoomInCursor_ = magnifierCursor(false, devicePixelRatioF());
    zoomOutCursor_ = magnifierCursor(true, devicePixelRatioF());
}

QSizeF CanvasWidget::documentSize() const {
    if (!session_->hasDocument()) return {1, 1};
    return {double(session_->document()->width), double(session_->document()->height)};
}

QRectF CanvasWidget::documentViewRect() const { return session_->viewport.documentRect(documentSize()); }
QPointF CanvasWidget::documentPoint(QPointF viewPoint) const { return session_->viewport.documentPoint(viewPoint, documentSize()); }
QPointF CanvasWidget::viewPoint(QPointF documentPoint) const { return session_->viewport.viewPoint(documentPoint, documentSize()); }
void CanvasWidget::setCropRatio(double ratio) { cropRatio_ = ratio; }

bool CanvasWidget::isBrushLike() const {
    Tool t = session_->tool();
    return t == Tool::Brush || t == Tool::SpotHealing || t == Tool::CloneStamp || t == Tool::Smudge;
}

void CanvasWidget::syncViewport() {
    QSizeF docSize = documentSize();
    session_->viewport.resize(QSizeF(size()), devicePixelRatioF(), session_->hasDocument() ? &docSize : nullptr);
}

void CanvasWidget::resizeEvent(QResizeEvent*) {
    syncViewport();
    cacheValid_ = false;
    emit session_->viewportChanged();
}

// ---- Rendering ---------------------------------------------------------------------

void CanvasWidget::renderInto(QImage& target, QRect deviceRect, QPointF documentOrigin, double zoom) {
    if (deviceRect.isEmpty() || !session_->hasDocument()) return;
    RenderOptions options;
    options.region = Rect((deviceRect.x() - documentOrigin.x()) / zoom, (deviceRect.y() - documentOrigin.y()) / zoom, deviceRect.width() / zoom, deviceRect.height() / zoom);
    options.scale = zoom;
    options.version = session_->documentRevision();
    Image out;
    Overrides overrides = session_->renderOverrides();
    compositor::render(*session_->document(), options, out, overrides.empty() ? nullptr : &overrides, &renderCache_);
    target = toQImage(out);
}

void CanvasWidget::ensureCache() {
    double dpr = devicePixelRatioF();
    double zoom = session_->viewport.zoom;
    QRectF docView = documentViewRect();
    QPointF origin(docView.x() * dpr, docView.y() * dpr);
    QRectF docDevice(origin, QSizeF(documentSize().width() * zoom, documentSize().height() * zoom));
    QRectF widgetDevice(0, 0, width() * dpr, height() * dpr);
    QRect visible = docDevice.intersected(widgetDevice).toAlignedRect();
    if (cacheValid_ && cacheDeviceRect_ == visible && cacheZoom_ == zoom && cacheDocumentOrigin_ == origin) return;
    cacheDeviceRect_ = visible;
    cacheZoom_ = zoom;
    cacheDocumentOrigin_ = origin;
    renderInto(cache_, visible, origin, zoom);
    cacheValid_ = true;
}

void CanvasWidget::invalidate(QRectF documentRegion) {
    if (!cacheValid_) { update(); return; }
    double zoom = cacheZoom_;
    QRectF device(cacheDocumentOrigin_.x() + documentRegion.x() * zoom, cacheDocumentOrigin_.y() + documentRegion.y() * zoom, documentRegion.width() * zoom, documentRegion.height() * zoom);
    QRect part = device.adjusted(-2, -2, 2, 2).toAlignedRect().intersected(cacheDeviceRect_);
    if (part.isEmpty()) return;
    QImage piece;
    renderInto(piece, part, cacheDocumentOrigin_, zoom);
    QPainter p(&cache_);
    p.setCompositionMode(QPainter::CompositionMode_Source);
    p.drawImage(part.topLeft() - cacheDeviceRect_.topLeft(), piece);
    p.end();
    double dpr = devicePixelRatioF();
    update(QRectF(part.x() / dpr, part.y() / dpr, part.width() / dpr, part.height() / dpr).toAlignedRect().adjusted(-1, -1, 1, 1));
}

void CanvasWidget::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(46, 46, 46));
    if (!session_->hasDocument()) {
        painter.setPen(QColor(150, 150, 150));
        painter.drawText(rect(), Qt::AlignCenter, tr("Open an image or project, or drop one here.\nFile > New creates a blank canvas."));
        return;
    }
    QRectF docView = documentViewRect();
    double dpr = devicePixelRatioF();
    static QPixmap checker;
    static double checkerDpr = 0;
    if (checkerDpr != dpr) { checker = checkerPixmap(dpr); checkerDpr = dpr; }
    QBrush brush(checker);
    painter.save();
    painter.setClipRect(docView);
    painter.setBrushOrigin(docView.topLeft());
    painter.fillRect(docView, brush);
    painter.restore();
    ensureCache();
    if (cacheValid_ && !cache_.isNull()) {
        QImage image = cache_;
        image.setDevicePixelRatio(dpr);
        painter.drawImage(QPointF(cacheDeviceRect_.x() / dpr, cacheDeviceRect_.y() / dpr), image);
    }
    painter.setPen(QPen(QColor(0, 0, 0, 90), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(docView.adjusted(-0.5, -0.5, 0.5, 0.5));
    drawOverlays(painter);
}

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
    const Layer* active = session_->activeLayer();
    if (active && boxShown()) {
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

// ---- Hit testing and cursors --------------------------------------------------------

CanvasWidget::HandleHit CanvasWidget::hitHandle(QPointF view, const Corners& corners, bool insideBox) const {
    HandleHit hit;
    QPointF vc[4];
    for (int i = 0; i < 4; i++) vc[i] = viewPoint(toQPoint(corners[size_t(i)]));
    for (int i = 0; i < 8; i++) {
        QPointF a = vc[i / 2], b = vc[(i / 2 + 1) % 4];
        QPointF p = i % 2 == 0 ? a : (a + b) / 2;
        if (std::hypot(p.x() - view.x(), p.y() - view.y()) <= handleRadius + 3) { hit.hit = true; hit.index = i; return hit; }
    }
    if (!insideBox) {
        for (int i = 0; i < 4; i++)
            if (std::hypot(vc[i].x() - view.x(), vc[i].y() - view.y()) <= rotateReach) { hit.hit = true; hit.rotate = true; hit.index = i * 2; return hit; }
    }
    return hit;
}

void CanvasWidget::updateCursor(QPointF view, Qt::KeyboardModifiers modifiers) {
    if (!session_->hasDocument()) { setCursor(Qt::ArrowCursor); return; }
    if (spaceHeld_ || session_->tool() == Tool::Hand || drag_ == Drag::Pan) { setCursor(drag_ == Drag::Pan ? Qt::ClosedHandCursor : Qt::OpenHandCursor); return; }
    if (session_->canvasPressHook) { setCursor(Qt::CrossCursor); return; }
    switch (session_->tool()) {
    case Tool::Move: {
        const Layer* active = session_->activeLayer();
        if (active && boxShown()) {
            Corners corners = session_->editedCorners(*active);
            HandleHit hit = hitHandle(view, corners, session_->editedTransform(*active).contains(toPoint(documentPoint(view))));
            if (hit.hit) {
                if (hit.rotate) { setCursor(Qt::CrossCursor); return; }
                static const Qt::CursorShape shapes[8] = {Qt::SizeFDiagCursor, Qt::SizeVerCursor, Qt::SizeBDiagCursor, Qt::SizeHorCursor, Qt::SizeFDiagCursor, Qt::SizeVerCursor, Qt::SizeBDiagCursor, Qt::SizeHorCursor};
                setCursor((modifiers & Qt::ControlModifier) ? Qt::CrossCursor : shapes[hit.index]);
                return;
            }
        }
        if (session_->canMovePixels(documentPoint(view))) { setCursor(Qt::DragMoveCursor); return; }
        setCursor((modifiers & Qt::ControlModifier) || session_->transformAutoSelect ? Qt::PointingHandCursor : Qt::SizeAllCursor);
        return;
    }
    case Tool::Brush: case Tool::SpotHealing: case Tool::CloneStamp: case Tool::Smudge: setCursor(Qt::BlankCursor); return;
    case Tool::Marquee: case Tool::Lasso: case Tool::Wand: case Tool::Crop: case Tool::Gradient: case Tool::Shape: case Tool::Eyedropper: setCursor(Qt::CrossCursor); return;
    case Tool::Text: setCursor(Qt::IBeamCursor); return;
    case Tool::Zoom: setCursor((modifiers & Qt::AltModifier) ? zoomOutCursor_ : zoomInCursor_); return;
    case Tool::Hand: setCursor(Qt::OpenHandCursor); return;
    }
}

SelectionMode CanvasWidget::selectionMode(Qt::KeyboardModifiers modifiers) const {
    bool shift = modifiers & Qt::ShiftModifier, alt = modifiers & Qt::AltModifier;
    if (shift && alt) return SelectionMode::Intersect;
    if (alt) return SelectionMode::Subtract;
    if (shift) return SelectionMode::Add;
    return SelectionMode::Replace;
}

QRectF CanvasWidget::dragBox(QPointF anchor, QPointF point, bool square, bool fromCenter, double ratio) const {
    double dx = std::round(point.x()) - anchor.x(), dy = std::round(point.y()) - anchor.y();
    if (square) { double side = std::max(std::fabs(dx), std::fabs(dy)); dx = dx < 0 ? -side : side; dy = dy < 0 ? -side : side; }
    else if (ratio > 0) {
        // Keep the aspect ratio, following whichever axis was dragged further.
        double w = std::fabs(dx), h = std::fabs(dy);
        if (w / ratio >= h) h = std::round(w / ratio); else w = std::round(h * ratio);
        dx = dx < 0 ? -w : w; dy = dy < 0 ? -h : h;
    }
    if (fromCenter) return {anchor.x() - std::fabs(dx), anchor.y() - std::fabs(dy), std::fabs(dx) * 2, std::fabs(dy) * 2};
    return {std::min(anchor.x(), anchor.x() + dx), std::min(anchor.y(), anchor.y() + dy), std::fabs(dx), std::fabs(dy)};
}

// ---- Input ----------------------------------------------------------------------------

void CanvasWidget::mousePressEvent(QMouseEvent* e) { setFocus(); press(e->position(), e->button(), e->modifiers()); }
void CanvasWidget::mouseMoveEvent(QMouseEvent* e) { move(e->position(), e->buttons(), e->modifiers()); }
void CanvasWidget::mouseReleaseEvent(QMouseEvent* e) { release(e->position(), e->button(), e->modifiers()); }

void CanvasWidget::mouseDoubleClickEvent(QMouseEvent* e) {
    // Double-clicking text with any tool opens its editor (the Text tool needs only a click).
    if (e->button() == Qt::LeftButton && session_->hasDocument() && !session_->brushActive() && !session_->warpActive() && !session_->pixelMoveActive()) {
        QPointF doc = documentPoint(e->position());
        std::optional<Uuid> under = session_->layerAt(doc);
        const Layer* hit = under ? session_->document()->find(*under) : nullptr;
        if (!hit || !hit->isLiveText()) {
            // Between the glyphs the pixel is transparent: the active text layer still counts inside its box.
            const Layer* active = session_->activeLayer();
            if (active && active->isLiveText() && session_->displayedTransform(*active).contains(compositor::Point(doc.x(), doc.y()))) hit = active;
        }
        if (hit && hit->isLiveText()) {
            if (session_->transformEdit()) session_->commitTransform();
            session_->selectLayer(hit->id, false);
            session_->requestTextEdit(hit->id);
            return;
        }
    }
    if (session_->tool() == Tool::Lasso && session_->lassoKind == LassoKind::Polygonal && !lassoPoints_.empty()) { finishPolygonalLasso(); return; }
    if (session_->tool() == Tool::Crop && crop_) { applyCrop(); return; }
    if (session_->tool() == Tool::Move && session_->transformEdit()) { session_->commitTransform(); return; }
    if (session_->tool() == Tool::Gradient && session_->gradientPending()) { session_->commitGradient(); return; }
    press(e->position(), e->button(), e->modifiers());
}

void CanvasWidget::tabletEvent(QTabletEvent* e) {
    e->accept();
    switch (e->type()) {
    case QEvent::TabletPress: setFocus(); press(e->position(), e->button(), e->modifiers()); break;
    case QEvent::TabletMove: move(e->position(), e->buttons(), e->modifiers()); break;
    case QEvent::TabletRelease: release(e->position(), e->button(), e->modifiers()); break;
    default: break;
    }
}

void CanvasWidget::press(QPointF view, Qt::MouseButton button, Qt::KeyboardModifiers modifiers) {
    if (!session_->hasDocument()) return;
    dragStartView_ = lastView_ = view;
    dragStartDocument_ = documentPoint(view);
    dragMoved_ = false;
    if (button == Qt::MiddleButton || spaceHeld_ || session_->tool() == Tool::Hand) { drag_ = Drag::Pan; updateCursor(view, modifiers); return; }
    if (button != Qt::LeftButton) return;
    QPointF doc = dragStartDocument_;
    if (session_->canvasPressHook && session_->canvasPressHook(doc)) { drag_ = Drag::Hook; return; }
    switch (session_->tool()) {
    case Tool::Move: {
        const Layer* active = session_->activeLayer();
        if (active && boxShown()) {
            Corners corners = session_->editedCorners(*active);
            HandleHit hit = hitHandle(view, corners, session_->editedTransform(*active).contains(toPoint(doc)));
            if (hit.hit) {
                if (!session_->transformEdit()) session_->beginTransform(false);
                if (!session_->transformEdit()) return;
                bool distort = session_->transformEdit()->corners.has_value() || ((modifiers & Qt::ControlModifier) && !hit.rotate && !session_->transformEdit()->mask);
                if (distort) {
                    if (!session_->transformEdit()->corners) session_->beginDistort();
                    if (!session_->transformEdit()->corners) return;
                    distortStart_ = *session_->transformEdit()->corners;
                    distortIndex_ = hit.index;
                    drag_ = Drag::Distort;
                    return;
                }
                transformDrag_ = TransformDrag{session_->transformEdit()->draft, toPoint(doc), hit.rotate ? TransformDrag::Mode::Rotate : TransformDrag::Mode::Resize, hit.index};
                drag_ = hit.rotate ? Drag::Rotate : Drag::Resize;
                return;
            }
        }
        // Selected pixels under the pointer: drag them (Alt duplicates).
        if (!session_->transformEdit() && session_->canMovePixels(doc)) {
            if (session_->beginPixelMove(modifiers & Qt::AltModifier)) { drag_ = Drag::PixelMove; return; }
        }
        // Pick the layer under the pointer with Ctrl (or auto-select); otherwise drag the active layer.
        layerPickedOnPress_ = false;
        bool pick = (modifiers & Qt::ControlModifier) || session_->transformAutoSelect;
        if (pick && !session_->transformEdit()) {
            auto id = session_->layerAt(doc);
            if (id && id != session_->activeLayerId()) { session_->selectLayer(id); layerPickedOnPress_ = true; }
            else if (!id) { drag_ = Drag::None; return; }
        }
        if (!session_->canTransform()) return;
        if (session_->transformEdit() && session_->transformEdit()->corners) {
            // Distorting: dragging the body moves the whole shape.
            distortStart_ = *session_->transformEdit()->corners;
            distortIndex_ = -1;
            drag_ = Drag::Distort;
            return;
        }
        if (!session_->transformEdit()) {
            if (modifiers & Qt::AltModifier) session_->beginDuplicateTransform();
            else session_->beginTransform(false);
        }
        if (!session_->transformEdit()) return;
        transformDrag_ = TransformDrag{session_->transformEdit()->draft, toPoint(doc), TransformDrag::Mode::Move, 0};
        drag_ = Drag::Move;
        return;
    }
    case Tool::Brush: case Tool::SpotHealing: case Tool::CloneStamp:
        if (session_->tool() == Tool::CloneStamp && (modifiers & Qt::AltModifier)) { session_->setCloneSource(doc); update(); return; }
        if (session_->beginBrush(doc, modifiers & Qt::ShiftModifier)) drag_ = Drag::Brush;
        return;
    case Tool::Smudge:
        if (session_->beginWarp(doc)) drag_ = Drag::Warp;
        return;
    case Tool::Gradient:
        session_->beginGradient(doc);
        if (session_->gradientPending()) drag_ = Drag::Gradient;
        return;
    case Tool::Shape:
        session_->beginShape(doc);
        if (session_->shapeDraft()) drag_ = Drag::Shape;
        return;
    case Tool::Text: {
        // A click on a text layer edits it; anywhere else starts a new one in the current style.
        std::optional<Uuid> under = session_->layerAt(doc);
        const Layer* hit = under ? session_->document()->find(*under) : nullptr;
        if (hit && hit->isLiveText()) { session_->selectLayer(hit->id, false); session_->requestTextEdit(hit->id); return; }
        LayerText text = session_->textStyle;
        text.text = tr("Text").toStdString();
        text.red = session_->foregroundColor.redF(); text.green = session_->foregroundColor.greenF(); text.blue = session_->foregroundColor.blueF();
        session_->addTextLayer(doc, text, true);
        return;
    }
    case Tool::Marquee: {
        const auto& d = session_->document();
        if (selectionMode(modifiers) == SelectionMode::Replace && d->selection && d->selection->coverage) {
            int x = int(std::floor(doc.x())), y = int(std::floor(doc.y()));
            if (x >= 0 && y >= 0 && x < d->width && y < d->height && d->selection->coverage->at(x, y) > 127) {
                selectionMoveOrigin_ = d->selection;
                session_->beginEdit("Move Selection");
                drag_ = Drag::SelectionMove;
                return;
            }
        }
        QPointF anchor(std::round(doc.x()), std::round(doc.y()));
        dragStartDocument_ = anchor;
        marquee_ = QRectF(anchor, QSizeF(0, 0));
        drag_ = Drag::Marquee;
        return;
    }
    case Tool::Lasso:
        if (session_->lassoKind == LassoKind::Polygonal) {
            if (!lassoPoints_.empty()) {
                QPointF first = viewPoint(lassoPoints_.front());
                if (lassoPoints_.size() >= 3 && std::hypot(first.x() - view.x(), first.y() - view.y()) <= 8) { finishPolygonalLasso(); return; }
            }
            lassoPoints_.push_back(doc);
            lassoCursor_ = doc;
            update();
            return;
        }
        lassoPoints_ = {doc};
        drag_ = Drag::Lasso;
        return;
    case Tool::Wand:
        session_->magicWand(doc, session_->wandTolerance, session_->wandContiguous, session_->wandSampleAll, selectionMode(modifiers), session_->wandSampleRadius);
        return;
    case Tool::Crop: {
        if (crop_) {
            QRectF cropView(viewPoint(crop_->topLeft()), viewPoint(crop_->bottomRight()));
            const QPointF corners[] = {cropView.topLeft(), cropView.topRight(), cropView.bottomRight(), cropView.bottomLeft()};
            for (int i = 0; i < 4; i++) if (std::hypot(corners[i].x() - view.x(), corners[i].y() - view.y()) <= handleRadius + 3) { cropHandle_ = i; cropOrigin_ = *crop_; drag_ = Drag::CropResize; return; }
            if (cropView.contains(view)) { cropOrigin_ = *crop_; drag_ = Drag::CropMove; return; }
        }
        crop_ = QRectF(QPointF(std::round(doc.x()), std::round(doc.y())), QSizeF(0, 0));
        dragStartDocument_ = crop_->topLeft();
        drag_ = Drag::Crop;
        return;
    }
    case Tool::Eyedropper:
        sampleColor(doc, modifiers & Qt::AltModifier);
        return;
    case Tool::Zoom:
        zoomRect_.reset();
        drag_ = Drag::ZoomRect;
        return;
    case Tool::Hand:
        return;
    }
}

void CanvasWidget::guideTargets(std::vector<double>& xs, std::vector<double>& ys) const {
    const auto& doc = session_->document();
    xs = {0, doc->width / 2.0, double(doc->width)};
    ys = {0, doc->height / 2.0, double(doc->height)};
    std::set<Uuid> moving;
    if (auto& edit = session_->transformEdit()) {
        moving.insert(edit->layerId);
        if (edit->group) for (auto& [id, t] : edit->group->originals) moving.insert(id);
    }
    for (const Layer* layer : renderLayers(doc->layers)) {
        if (moving.count(layer->id) || !layer->asset) continue;
        Rect b = layer->transform.bounds();
        xs.insert(xs.end(), {b.minX(), b.midX(), b.maxX()});
        ys.insert(ys.end(), {b.minY(), b.midY(), b.maxY()});
    }
}

void CanvasWidget::snapMove(LayerTransform& draft) {
    std::vector<double> xs, ys;
    guideTargets(xs, ys);
    Rect box = draft.bounds();
    SnapResult snap = snapOffset(box, xs, ys, snapDistance / session_->viewport.pointsPerPixel());
    draft.origin.x += snap.dx;
    draft.origin.y += snap.dy;
    std::vector<double> gx, gy;
    if (snap.snappedX) gx.push_back(snap.x);
    if (snap.snappedY) gy.push_back(snap.y);
    session_->setSnapGuides(gx, gy);
}

QPointF CanvasWidget::snapPoint(QPointF p) {
    std::vector<double> xs, ys;
    guideTargets(xs, ys);
    SnapResult snap = snapOffset(Rect(p.x(), p.y(), 0, 0), xs, ys, snapDistance / session_->viewport.pointsPerPixel());
    std::vector<double> gx, gy;
    if (snap.snappedX) gx.push_back(snap.x);
    if (snap.snappedY) gy.push_back(snap.y);
    session_->setSnapGuides(gx, gy);
    return {p.x() + snap.dx, p.y() + snap.dy};
}

void CanvasWidget::move(QPointF view, Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers) {
    hover_ = view;
    QPointF doc = documentPoint(view);
    emit cursorMoved(doc);
    if (drag_ == Drag::None) {
        if (session_->tool() == Tool::Lasso && session_->lassoKind == LassoKind::Polygonal && !lassoPoints_.empty()) { lassoCursor_ = doc; update(); }
        updateCursor(view, modifiers);
        if (isBrushLike()) update();
        return;
    }
    QPointF delta = view - lastView_;
    if (std::hypot(view.x() - dragStartView_.x(), view.y() - dragStartView_.y()) > dragThreshold) dragMoved_ = true;
    switch (drag_) {
    case Drag::Pan:
        session_->viewport.translate(delta);
        emit session_->viewportChanged();
        break;
    case Drag::Hook:
        if (session_->canvasDragHook) session_->canvasDragHook(dragStartDocument_, doc);
        break;
    case Drag::Move: case Drag::Resize: case Drag::Rotate: {
        if (!transformDrag_ || !session_->transformEdit()) break;
        bool shift = modifiers & Qt::ShiftModifier, alt = modifiers & Qt::AltModifier;
        QPointF target = doc;
        if (drag_ == Drag::Resize && !(modifiers & Qt::ControlModifier)) {
            // Snap the dragged handle to guides: move the pointer by whatever the handle would snap.
            LayerTransform trial = transformDrag_->updated(toPoint(doc), session_->locksTransformRatio, shift, alt);
            QPointF h = toQPoint(trial.point(LayerTransform::handles[size_t(transformDrag_->handle)]));
            QPointF snapped = snapPoint(h);
            target = doc + (snapped - h);
        }
        LayerTransform draft = transformDrag_->updated(toPoint(target), session_->locksTransformRatio, shift, alt);
        draft = draft.rounded();
        if (drag_ == Drag::Move && !(modifiers & Qt::ControlModifier)) snapMove(draft);
        session_->previewTransform(draft);
        break;
    }
    case Drag::Distort: {
        Corners c = distortStart_;
        double dx = doc.x() - dragStartDocument_.x(), dy = doc.y() - dragStartDocument_.y();
        if (modifiers & Qt::ShiftModifier) { if (std::fabs(dx) >= std::fabs(dy)) dy = 0; else dx = 0; }
        std::vector<int> moved;
        if (distortIndex_ < 0) moved = {0, 1, 2, 3};
        else if (distortIndex_ % 2 == 0) moved = {distortIndex_ / 2};
        else moved = {distortIndex_ / 2, (distortIndex_ / 2 + 1) % 4};
        for (int i : moved) { c[size_t(i)].x = std::round(c[size_t(i)].x + dx); c[size_t(i)].y = std::round(c[size_t(i)].y + dy); }
        session_->previewCorners(c);
        break;
    }
    case Drag::PixelMove:
        session_->movePixels(doc - dragStartDocument_);
        break;
    case Drag::Brush:
        session_->continueBrush(doc);
        break;
    case Drag::Warp:
        session_->continueWarp(doc);
        break;
    case Drag::Gradient: {
        QPointF end = doc;
        if (modifiers & Qt::ShiftModifier) {
            // Snap the line to 45 degree steps.
            QPointF d = end - dragStartDocument_;
            double angle = std::round(std::atan2(d.y(), d.x()) / (M_PI / 4)) * (M_PI / 4), len = std::hypot(d.x(), d.y());
            end = dragStartDocument_ + QPointF(std::cos(angle) * len, std::sin(angle) * len);
        }
        session_->moveGradient(end);
        break;
    }
    case Drag::Shape:
        session_->dragShape(doc, modifiers & Qt::ShiftModifier, modifiers & Qt::AltModifier);
        break;
    case Drag::Marquee:
        marquee_ = dragBox(dragStartDocument_, doc, modifiers & Qt::ShiftModifier, false);
        update();
        break;
    case Drag::Lasso:
        if (lassoPoints_.empty() || std::hypot(doc.x() - lassoPoints_.back().x(), doc.y() - lassoPoints_.back().y()) >= 0.25) lassoPoints_.push_back(doc);
        update();
        break;
    case Drag::SelectionMove: {
        if (!selectionMoveOrigin_ || !selectionMoveOrigin_->coverage) break;
        int dx = int(std::round(doc.x() - dragStartDocument_.x())), dy = int(std::round(doc.y() - dragStartDocument_.y()));
        const GrayImage& src = *selectionMoveOrigin_->coverage;
        auto moved = std::make_shared<GrayImage>(src.width(), src.height(), 0);
        for (int y = 0; y < src.height(); y++) {
            int sy = y - dy;
            if (sy < 0 || sy >= src.height()) continue;
            for (int x = 0; x < src.width(); x++) { int sx = x - dx; if (sx >= 0 && sx < src.width()) moved->at(x, y) = src.at(sx, sy); }
        }
        Selection s = *selectionMoveOrigin_;
        s.coverage = moved;
        const_cast<Document&>(*session_->document()).selection = s; // inside a begin/end edit
        refreshSelectionOutline();
        update();
        break;
    }
    case Drag::Crop: {
        QPointF p = (modifiers & Qt::ControlModifier) ? doc : snapPoint(doc);
        crop_ = dragBox(dragStartDocument_, p, modifiers & Qt::ShiftModifier, modifiers & Qt::AltModifier, cropRatio_).intersected(QRectF(QPointF(0, 0), documentSize()));
        emit cropChanged();
        update();
        break;
    }
    case Drag::CropMove: {
        QPointF d = doc - dragStartDocument_;
        QRectF r = cropOrigin_.translated(std::round(d.x()), std::round(d.y()));
        if (!(modifiers & Qt::ControlModifier)) {
            std::vector<double> xs, ys;
            guideTargets(xs, ys);
            SnapResult snap = snapOffset(Rect(r.x(), r.y(), r.width(), r.height()), xs, ys, snapDistance / session_->viewport.pointsPerPixel());
            r.translate(snap.dx, snap.dy);
            std::vector<double> gx, gy;
            if (snap.snappedX) gx.push_back(snap.x);
            if (snap.snappedY) gy.push_back(snap.y);
            session_->setSnapGuides(gx, gy);
        }
        QSizeF ds = documentSize();
        r.moveLeft(std::clamp(r.left(), 0.0, ds.width() - r.width()));
        r.moveTop(std::clamp(r.top(), 0.0, ds.height() - r.height()));
        crop_ = r;
        emit cropChanged();
        update();
        break;
    }
    case Drag::CropResize: {
        QRectF r = cropOrigin_;
        QPointF snapped = (modifiers & Qt::ControlModifier) ? doc : snapPoint(doc);
        QPointF p(std::round(snapped.x()), std::round(snapped.y()));
        QPointF opposite = cropHandle_ == 0 ? r.bottomRight() : cropHandle_ == 1 ? r.bottomLeft() : cropHandle_ == 2 ? r.topLeft() : r.topRight();
        QRectF box = cropRatio_ > 0 || (modifiers & Qt::ShiftModifier) ? dragBox(opposite, p, modifiers & Qt::ShiftModifier, false, cropRatio_) : QRectF(opposite, p).normalized();
        crop_ = box.intersected(QRectF(QPointF(0, 0), documentSize()));
        emit cropChanged();
        update();
        break;
    }
    case Drag::ZoomRect:
        zoomRect_ = QRectF(dragStartDocument_, doc).normalized();
        update();
        break;
    default: break;
    }
    lastView_ = view;
}

void CanvasWidget::release(QPointF view, Qt::MouseButton button, Qt::KeyboardModifiers modifiers) {
    Q_UNUSED(button);
    QPointF doc = documentPoint(view);
    Drag drag = drag_;
    drag_ = Drag::None;
    switch (drag) {
    case Drag::Hook:
        if (session_->canvasReleaseHook) session_->canvasReleaseHook();
        break;
    case Drag::Move: case Drag::Resize: case Drag::Rotate:
        transformDrag_.reset();
        session_->setSnapGuides({}, {});
        if (session_->transformEdit() && !session_->transformEdit()->persistent) session_->commitTransform();
        update();
        break;
    case Drag::Distort:
        update();
        break;
    case Drag::PixelMove:
        session_->finishPixelMove();
        break;
    case Drag::Brush:
        session_->continueBrush(doc);
        session_->endBrush();
        break;
    case Drag::Warp:
        session_->continueWarp(doc);
        session_->endWarp();
        break;
    case Drag::Gradient:
        session_->endGradientDrag();
        break;
    case Drag::Shape:
        session_->finishShape();
        break;
    case Drag::Marquee:
        finishMarquee(modifiers);
        break;
    case Drag::Lasso:
        finishFreehandLasso();
        break;
    case Drag::SelectionMove:
        selectionMoveOrigin_.reset();
        session_->endEdit();
        emit session_->historyChanged();
        emit session_->selectionChanged();
        break;
    case Drag::Crop: case Drag::CropMove: case Drag::CropResize:
        session_->setSnapGuides({}, {});
        if (crop_ && (crop_->width() < 1 || crop_->height() < 1)) crop_ = QRectF(QPointF(0, 0), documentSize());
        emit cropChanged();
        update();
        break;
    case Drag::ZoomRect: {
        if (zoomRect_ && dragMoved_ && zoomRect_->width() > 2 && zoomRect_->height() > 2) {
            double zoom = std::min((width() - 40) / zoomRect_->width(), (height() - 40) / zoomRect_->height()) * devicePixelRatioF();
            session_->viewport.setZoom(zoom, session_->viewport.center(), documentSize());
            QPointF centerView = viewPoint(zoomRect_->center());
            session_->viewport.translate(session_->viewport.center() - centerView);
            emit session_->viewportChanged();
        } else {
            double factor = (modifiers & Qt::AltModifier) ? 0.5 : 2;
            session_->zoomTo(session_->viewport.zoom * factor, view);
        }
        zoomRect_.reset();
        update();
        break;
    }
    default: break;
    }
    updateCursor(view, modifiers);
}

void CanvasWidget::finishMarquee(Qt::KeyboardModifiers modifiers) {
    std::optional<QRectF> box = marquee_;
    marquee_.reset();
    update();
    if (!box) return;
    SelectionMode mode = selectionMode(modifiers);
    if (box->width() < 1 || box->height() < 1) { if (mode == SelectionMode::Replace) session_->deselect(); return; }
    QSizeF ds = documentSize();
    Rect rect(box->x(), box->y(), box->width(), box->height());
    auto shape = session_->marqueeKind == MarqueeKind::Ellipse
        ? rasterizeEllipse(rect, int(ds.width()), int(ds.height()), session_->selectionAntialiased)
        : rasterizeRect(rect, int(ds.width()), int(ds.height()), session_->selectionAntialiased);
    session_->applySelectionShape(*shape, mode, session_->marqueeKind == MarqueeKind::Ellipse ? "Elliptical Marquee" : "Rectangular Marquee");
}

void CanvasWidget::finishFreehandLasso() {
    std::vector<QPointF> points = std::move(lassoPoints_);
    lassoPoints_.clear();
    update();
    SelectionMode mode = selectionMode(QApplication::keyboardModifiers());
    if (points.size() < 3) { if (mode == SelectionMode::Replace) session_->deselect(); return; }
    std::vector<Point> poly;
    for (auto& p : points) poly.push_back(toPoint(p));
    QSizeF ds = documentSize();
    auto shape = rasterizePolygon(poly, int(ds.width()), int(ds.height()), session_->selectionAntialiased);
    session_->applySelectionShape(*shape, mode, "Lasso");
}

void CanvasWidget::finishPolygonalLasso() {
    lassoCursor_.reset();
    std::vector<QPointF> points = std::move(lassoPoints_);
    lassoPoints_.clear();
    update();
    if (points.size() < 3) return;
    std::vector<Point> poly;
    for (auto& p : points) poly.push_back(toPoint(p));
    QSizeF ds = documentSize();
    auto shape = rasterizePolygon(poly, int(ds.width()), int(ds.height()), session_->selectionAntialiased);
    session_->applySelectionShape(*shape, selectionMode(QApplication::keyboardModifiers()), "Polygonal Lasso");
}

void CanvasWidget::cancelLasso() { lassoPoints_.clear(); lassoCursor_.reset(); update(); }

void CanvasWidget::applyCrop() {
    if (!crop_) return;
    QRectF rect = *crop_;
    crop_.reset();
    session_->cropTo(rect);
    session_->selectTool(Tool::Move);
}

void CanvasWidget::cancelCrop() {
    crop_.reset();
    emit cropChanged();
    session_->selectTool(Tool::Move);
}

void CanvasWidget::sampleColor(QPointF doc, bool background) {
    std::optional<QColor> sampled = session_->compositeColorAt(doc);
    if (!sampled) return;
    // Copied out by value: GCC 13 misreads a dereference here as a dangling pointer.
    QColor color = sampled.value_or(QColor());
    if (background) session_->backgroundColor = color; else session_->foregroundColor = color;
    session_->refreshGradient();
    emit session_->toolChanged();
}

void CanvasWidget::wheelEvent(QWheelEvent* e) {
    if (!session_->hasDocument()) return;
    if (e->modifiers() & Qt::ControlModifier) {
        double steps = e->angleDelta().y() / 120.0;
        if (steps == 0) steps = e->pixelDelta().y() / 50.0;
        session_->zoomTo(session_->viewport.zoom * std::pow(1.25, steps), e->position());
    } else {
        QPointF delta = e->pixelDelta().isNull() ? QPointF(e->angleDelta().x(), e->angleDelta().y()) / 2 : QPointF(e->pixelDelta());
        if (e->modifiers() & Qt::ShiftModifier && delta.x() == 0) delta = {delta.y(), 0};
        session_->viewport.translate(delta);
        emit session_->viewportChanged();
    }
    e->accept();
}

bool CanvasWidget::event(QEvent* e) {
    if (e->type() == QEvent::NativeGesture) {
        auto* g = static_cast<QNativeGestureEvent*>(e);
        if (g->gestureType() == Qt::ZoomNativeGesture && session_->hasDocument()) {
            session_->zoomTo(session_->viewport.zoom * (1 + g->value()), g->position());
            return true;
        }
    }
    return QWidget::event(e);
}

void CanvasWidget::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) { spaceHeld_ = true; if (hover_) updateCursor(*hover_, e->modifiers()); return; }
    if (!session_->hasDocument()) { QWidget::keyPressEvent(e); return; }
    if (e->key() == Qt::Key_Alt || e->key() == Qt::Key_Control) { if (hover_) updateCursor(*hover_, e->modifiers()); }
    double step = (e->modifiers() & Qt::ShiftModifier) ? 10 : 1;
    switch (e->key()) {
    case Qt::Key_Escape:
        if (session_->brushActive()) { session_->cancelBrush(); return; }
        if (session_->warpActive()) { session_->cancelWarp(); return; }
        if (session_->pixelMoveActive()) { session_->cancelPixelMove(); return; }
        if (session_->gradientPending()) { session_->cancelGradient(); return; }
        if (session_->shapeDraft()) { session_->cancelShape(); return; }
        if (session_->transformEdit()) { session_->cancelTransform(); return; }
        if (!lassoPoints_.empty()) { cancelLasso(); return; }
        if (crop_) { cancelCrop(); return; }
        return;
    case Qt::Key_Return: case Qt::Key_Enter:
        if (session_->transformEdit()) { session_->commitTransform(); return; }
        if (session_->gradientPending()) { session_->commitGradient(); return; }
        if (!lassoPoints_.empty() && session_->lassoKind == LassoKind::Polygonal) { finishPolygonalLasso(); return; }
        if (crop_) { applyCrop(); return; }
        return;
    case Qt::Key_Backspace: case Qt::Key_Delete:
        if (session_->lassoKind == LassoKind::Polygonal && !lassoPoints_.empty()) { lassoPoints_.pop_back(); update(); return; }
        break;
    case Qt::Key_Left: case Qt::Key_Right: case Qt::Key_Up: case Qt::Key_Down: {
        double dx = e->key() == Qt::Key_Left ? -step : e->key() == Qt::Key_Right ? step : 0;
        double dy = e->key() == Qt::Key_Up ? -step : e->key() == Qt::Key_Down ? step : 0;
        if ((e->modifiers() & Qt::ControlModifier) && session_->document()->selection) { session_->nudgePixels(dx, dy); return; }
        if ((session_->tool() == Tool::Marquee || session_->tool() == Tool::Lasso || session_->tool() == Tool::Wand) && session_->document()->selection) { session_->nudgeSelection(dx, dy); return; }
        if (session_->tool() == Tool::Move && session_->canTransform()) { session_->nudgeLayer(dx, dy); return; }
        break;
    }
    case Qt::Key_BracketLeft: case Qt::Key_BracketRight: {
        if (!isBrushLike()) break;
        bool increase = e->key() == Qt::Key_BracketRight;
        if (e->modifiers() & Qt::ShiftModifier) session_->changeBrushHardness(increase); else session_->changeBrushSize(increase);
        update();
        return;
    }
    case Qt::Key_BraceLeft: case Qt::Key_BraceRight:
        if (!isBrushLike()) break;
        session_->changeBrushHardness(e->key() == Qt::Key_BraceRight);
        return;
    default: break;
    }
    if (e->key() >= Qt::Key_0 && e->key() <= Qt::Key_9 && !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
        session_->typeOpacityDigit(e->key() - Qt::Key_0);
        return;
    }
    QWidget::keyPressEvent(e);
}

void CanvasWidget::keyReleaseEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) { spaceHeld_ = false; if (drag_ == Drag::Pan && session_->tool() != Tool::Hand) drag_ = Drag::None; if (hover_) updateCursor(*hover_, e->modifiers()); return; }
    if (e->key() == Qt::Key_Alt || e->key() == Qt::Key_Control) { if (hover_) updateCursor(*hover_, e->modifiers()); }
    QWidget::keyReleaseEvent(e);
}

void CanvasWidget::leaveEvent(QEvent*) { hover_.reset(); update(); }
void CanvasWidget::focusOutEvent(QFocusEvent* e) { spaceHeld_ = false; QWidget::focusOutEvent(e); }

} // namespace app
