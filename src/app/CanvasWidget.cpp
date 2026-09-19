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
    connect(session_, &EditorSession::transformChanged, this, [this] { update(); });
    antsTimer_.setInterval(120);
    connect(&antsTimer_, &QTimer::timeout, this, [this] { antsPhase_ = (antsPhase_ + 1) % 8; if (!selectionOutline_.empty()) update(); });
}

QSizeF CanvasWidget::documentSize() const {
    if (!session_->hasDocument()) return {1, 1};
    return {double(session_->document()->width), double(session_->document()->height)};
}

QRectF CanvasWidget::documentViewRect() const { return session_->viewport.documentRect(documentSize()); }
QPointF CanvasWidget::documentPoint(QPointF viewPoint) const { return session_->viewport.documentPoint(viewPoint, documentSize()); }
QPointF CanvasWidget::viewPoint(QPointF documentPoint) const { return session_->viewport.viewPoint(documentPoint, documentSize()); }

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
    Image out;
    Overrides overrides = session_->renderOverrides();
    compositor::render(*session_->document(), options, out, overrides.empty() ? nullptr : &overrides);
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

void CanvasWidget::drawOverlays(QPainter& painter) {
    painter.setRenderHint(QPainter::Antialiasing, true);
    // Pixel grid when zoomed far in.
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
    // Transform box for the active layer.
    const Layer* active = session_->activeLayer();
    bool showBox = active && !active->isGroup && (session_->transformEdit() || (session_->tool() == Tool::Move && session_->showsTransformControls && session_->canTransform()));
    if (showBox) drawTransformBox(painter, session_->editedTransform(*active), true);
    // Snap guides.
    painter.setPen(QPen(QColor(255, 0, 200), 1));
    for (double x : session_->snapGuidesX) { double vx = viewPoint({x, 0}).x(); painter.drawLine(QPointF(vx, 0), QPointF(vx, height())); }
    for (double y : session_->snapGuidesY) { double vy = viewPoint({0, y}).y(); painter.drawLine(QPointF(0, vy), QPointF(width(), vy)); }
    // Marquee / lasso drafts.
    if (marquee_) {
        painter.setPen(QPen(Qt::black, 1, Qt::DashLine));
        QRectF r(viewPoint(marquee_->topLeft()), viewPoint(marquee_->bottomRight()));
        if (session_->marqueeKind == MarqueeKind::Ellipse) painter.drawEllipse(r); else painter.drawRect(r);
    }
    if (!lassoPoints_.empty()) {
        QPolygonF poly;
        for (auto& p : lassoPoints_) poly << viewPoint(p);
        if (lassoCursor_) poly << viewPoint(*lassoCursor_);
        painter.setPen(QPen(Qt::white, 3));
        painter.drawPolyline(poly);
        painter.setPen(QPen(Qt::black, 1));
        painter.drawPolyline(poly);
    }
    if (zoomRect_) {
        painter.setPen(QPen(Qt::white, 1, Qt::DashLine));
        painter.drawRect(QRectF(viewPoint(zoomRect_->topLeft()), viewPoint(zoomRect_->bottomRight())));
    }
    drawCropOverlay(painter);
    // Brush cursor.
    if (session_->tool() == Tool::Brush && hover_ && !spaceHeld_) {
        double r = session_->brushSettings.diameter / 2 * ppp;
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(255, 255, 255, 200), 1));
        painter.drawEllipse(*hover_, r + 0.5, r + 0.5);
        painter.setPen(QPen(QColor(0, 0, 0, 200), 1));
        painter.drawEllipse(*hover_, r, r);
        if (r < 3) { painter.drawLine(*hover_ + QPointF(-6, 0), *hover_ + QPointF(6, 0)); painter.drawLine(*hover_ + QPointF(0, -6), *hover_ + QPointF(0, 6)); }
    }
}

void CanvasWidget::drawTransformBox(QPainter& painter, const LayerTransform& transform, bool active) {
    QPolygonF box;
    for (auto& c : transform.corners()) box << viewPoint(toQPoint(c));
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(255, 255, 255, 180), 3));
    painter.drawPolygon(box);
    painter.setPen(QPen(active ? QColor(0, 122, 255) : QColor(120, 120, 120), 1));
    painter.drawPolygon(box);
    if (!active) return;
    painter.setBrush(Qt::white);
    for (auto& h : LayerTransform::handles) {
        QPointF p = viewPoint(toQPoint(transform.point(h)));
        painter.drawRect(QRectF(p.x() - handleRadius + 0.5, p.y() - handleRadius + 0.5, handleRadius * 2 - 1, handleRadius * 2 - 1));
    }
}

void CanvasWidget::drawSelectionAnts(QPainter& painter) {
    if (selectionOutline_.empty()) return;
    painter.setBrush(Qt::NoBrush);
    QPen white(Qt::white, 1);
    QPen black(Qt::black, 1, Qt::CustomDashLine);
    black.setDashPattern({4, 4});
    black.setDashOffset(antsPhase_);
    for (auto& loop : selectionOutline_) {
        QPolygonF poly;
        for (auto& p : loop) poly << viewPoint(p);
        painter.setPen(white);
        painter.drawPolygon(poly);
        painter.setPen(black);
        painter.drawPolygon(poly);
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
    const auto& doc = session_->document();
    if (doc && doc->selection && doc->selection->coverage) {
        for (auto& loop : selectionOutline(*doc->selection->coverage)) {
            QPolygonF poly;
            for (auto& p : loop) poly << toQPoint(p);
            selectionOutline_.push_back(poly);
        }
    }
    if (selectionOutline_.empty()) antsTimer_.stop(); else if (!antsTimer_.isActive()) antsTimer_.start();
}

// ---- Hit testing and cursors --------------------------------------------------------

CanvasWidget::HandleHit CanvasWidget::hitHandle(QPointF view, const LayerTransform& transform) const {
    HandleHit hit;
    for (int i = 0; i < 8; i++) {
        QPointF p = viewPoint(toQPoint(transform.point(LayerTransform::handles[size_t(i)])));
        if (std::hypot(p.x() - view.x(), p.y() - view.y()) <= handleRadius + 3) { hit.hit = true; hit.index = i; return hit; }
    }
    // Outside the box but near a corner: rotate.
    if (!transform.contains(toPoint(documentPoint(view)))) {
        for (int i = 0; i < 8; i += 2) {
            QPointF p = viewPoint(toQPoint(transform.point(LayerTransform::handles[size_t(i)])));
            if (std::hypot(p.x() - view.x(), p.y() - view.y()) <= rotateReach) { hit.hit = true; hit.rotate = true; hit.index = i; return hit; }
        }
    }
    return hit;
}

void CanvasWidget::updateCursor(QPointF view, Qt::KeyboardModifiers modifiers) {
    if (!session_->hasDocument()) { setCursor(Qt::ArrowCursor); return; }
    if (spaceHeld_ || session_->tool() == Tool::Hand || drag_ == Drag::Pan) { setCursor(drag_ == Drag::Pan ? Qt::ClosedHandCursor : Qt::OpenHandCursor); return; }
    switch (session_->tool()) {
    case Tool::Move: {
        const Layer* active = session_->activeLayer();
        if (active && !active->isGroup && (session_->transformEdit() || (session_->showsTransformControls && session_->canTransform()))) {
            HandleHit hit = hitHandle(view, session_->editedTransform(*active));
            if (hit.hit) {
                if (hit.rotate) { setCursor(Qt::CrossCursor); return; }
                static const Qt::CursorShape shapes[8] = {Qt::SizeFDiagCursor, Qt::SizeVerCursor, Qt::SizeBDiagCursor, Qt::SizeHorCursor, Qt::SizeFDiagCursor, Qt::SizeVerCursor, Qt::SizeBDiagCursor, Qt::SizeHorCursor};
                setCursor(shapes[hit.index]);
                return;
            }
        }
        setCursor((modifiers & Qt::ControlModifier) || session_->transformAutoSelect ? Qt::PointingHandCursor : Qt::SizeAllCursor);
        return;
    }
    case Tool::Brush: setCursor(Qt::BlankCursor); return;
    case Tool::Marquee: case Tool::Lasso: case Tool::Wand: case Tool::Crop: setCursor(Qt::CrossCursor); return;
    case Tool::Eyedropper: setCursor(Qt::CrossCursor); return;
    case Tool::Zoom: setCursor(Qt::CrossCursor); return;
    case Tool::Hand: setCursor(Qt::OpenHandCursor); return;
    }
}

SelectionMode CanvasWidget::selectionMode(Qt::KeyboardModifiers modifiers) const {
    if (modifiers & Qt::AltModifier) return SelectionMode::Subtract;
    if (modifiers & Qt::ShiftModifier) return SelectionMode::Add;
    return SelectionMode::Replace;
}

QRectF CanvasWidget::dragBox(QPointF anchor, QPointF point, bool square, bool fromCenter) const {
    double dx = std::round(point.x()) - anchor.x(), dy = std::round(point.y()) - anchor.y();
    if (square) { double side = std::max(std::fabs(dx), std::fabs(dy)); dx = dx < 0 ? -side : side; dy = dy < 0 ? -side : side; }
    if (fromCenter) return {anchor.x() - std::fabs(dx), anchor.y() - std::fabs(dy), std::fabs(dx) * 2, std::fabs(dy) * 2};
    return {std::min(anchor.x(), anchor.x() + dx), std::min(anchor.y(), anchor.y() + dy), std::fabs(dx), std::fabs(dy)};
}

// ---- Input ----------------------------------------------------------------------------

void CanvasWidget::mousePressEvent(QMouseEvent* e) { setFocus(); press(e->position(), e->button(), e->modifiers()); }
void CanvasWidget::mouseMoveEvent(QMouseEvent* e) { move(e->position(), e->buttons(), e->modifiers()); }
void CanvasWidget::mouseReleaseEvent(QMouseEvent* e) { release(e->position(), e->button(), e->modifiers()); }

void CanvasWidget::mouseDoubleClickEvent(QMouseEvent* e) {
    if (session_->tool() == Tool::Lasso && polygonalLasso_ && !lassoPoints_.empty()) { finishPolygonalLasso(); return; }
    if (session_->tool() == Tool::Crop && crop_) { applyCrop(); return; }
    if (session_->tool() == Tool::Move && session_->transformEdit()) { session_->commitTransform(); return; }
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
    switch (session_->tool()) {
    case Tool::Move: {
        const Layer* active = session_->activeLayer();
        bool boxShown = active && !active->isGroup && (session_->transformEdit() || (session_->showsTransformControls && session_->canTransform()));
        if (boxShown) {
            LayerTransform t = session_->editedTransform(*active);
            HandleHit hit = hitHandle(view, t);
            if (hit.hit) {
                if (!session_->transformEdit()) session_->beginTransform(false);
                if (!session_->transformEdit()) return;
                transformDrag_ = TransformDrag{session_->transformEdit()->draft, toPoint(doc), hit.rotate ? TransformDrag::Mode::Rotate : TransformDrag::Mode::Resize, hit.index};
                drag_ = hit.rotate ? Drag::Rotate : Drag::Resize;
                return;
            }
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
        if (!session_->transformEdit()) session_->beginTransform(false);
        if (!session_->transformEdit()) return;
        transformDrag_ = TransformDrag{session_->transformEdit()->draft, toPoint(doc), TransformDrag::Mode::Move, 0};
        drag_ = Drag::Move;
        return;
    }
    case Tool::Brush:
        if (session_->beginBrush(doc, modifiers & Qt::ShiftModifier)) drag_ = Drag::Brush;
        return;
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
        if (polygonalLasso_) {
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
        session_->magicWand(doc, session_->wandTolerance, session_->wandContiguous, session_->wandSampleAll, selectionMode(modifiers));
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

void CanvasWidget::snapMove(LayerTransform& draft, const LayerTransform& original) {
    Q_UNUSED(original);
    const auto& doc = session_->document();
    std::vector<double> xs{0, doc->width / 2.0, double(doc->width)}, ys{0, doc->height / 2.0, double(doc->height)};
    for (const Layer* layer : renderLayers(doc->layers)) {
        if (layer->id == session_->transformEdit()->layerId || !layer->asset) continue;
        Rect b = layer->transform.bounds();
        xs.insert(xs.end(), {b.minX(), b.midX(), b.maxX()});
        ys.insert(ys.end(), {b.minY(), b.midY(), b.maxY()});
    }
    Rect box = draft.bounds();
    SnapResult snap = snapOffset(box, xs, ys, snapDistance / session_->viewport.pointsPerPixel());
    draft.origin.x += snap.dx;
    draft.origin.y += snap.dy;
    std::vector<double> gx, gy;
    if (snap.snappedX) gx.push_back(snap.x);
    if (snap.snappedY) gy.push_back(snap.y);
    session_->setSnapGuides(gx, gy);
}

void CanvasWidget::move(QPointF view, Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers) {
    hover_ = view;
    QPointF doc = documentPoint(view);
    emit cursorMoved(doc);
    if (drag_ == Drag::None) {
        if (session_->tool() == Tool::Lasso && polygonalLasso_ && !lassoPoints_.empty()) { lassoCursor_ = doc; update(); }
        updateCursor(view, modifiers);
        if (session_->tool() == Tool::Brush) update();
        return;
    }
    QPointF delta = view - lastView_;
    if (std::hypot(view.x() - dragStartView_.x(), view.y() - dragStartView_.y()) > dragThreshold) dragMoved_ = true;
    switch (drag_) {
    case Drag::Pan:
        session_->viewport.translate(delta);
        emit session_->viewportChanged();
        break;
    case Drag::Move: case Drag::Resize: case Drag::Rotate: {
        if (!transformDrag_ || !session_->transformEdit()) break;
        bool shift = modifiers & Qt::ShiftModifier, alt = modifiers & Qt::AltModifier;
        LayerTransform draft = transformDrag_->updated(toPoint(doc), session_->locksTransformRatio, shift, alt);
        if (drag_ == Drag::Move) { draft = draft.rounded(); if (!(modifiers & Qt::ControlModifier)) snapMove(draft, transformDrag_->original); }
        else draft = draft.rounded();
        session_->previewTransform(draft);
        break;
    }
    case Drag::Brush:
        session_->continueBrush(doc);
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
    case Drag::Crop:
        crop_ = dragBox(dragStartDocument_, doc, modifiers & Qt::ShiftModifier, modifiers & Qt::AltModifier).intersected(QRectF(QPointF(0, 0), documentSize()));
        emit cropChanged();
        update();
        break;
    case Drag::CropMove: {
        QPointF d = doc - dragStartDocument_;
        QRectF r = cropOrigin_.translated(std::round(d.x()), std::round(d.y()));
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
        QPointF p(std::round(doc.x()), std::round(doc.y()));
        QPointF opposite = cropHandle_ == 0 ? r.bottomRight() : cropHandle_ == 1 ? r.bottomLeft() : cropHandle_ == 2 ? r.topLeft() : r.topRight();
        crop_ = QRectF(opposite, p).normalized().intersected(QRectF(QPointF(0, 0), documentSize()));
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
    case Drag::Move: case Drag::Resize: case Drag::Rotate:
        transformDrag_.reset();
        session_->setSnapGuides({}, {});
        if (session_->transformEdit() && !session_->transformEdit()->persistent) session_->commitTransform();
        else session_->setSnapGuides({}, {});
        update();
        break;
    case Drag::Brush:
        session_->continueBrush(doc);
        session_->endBrush();
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
    auto flattened = session_->flattened();
    if (!flattened) return;
    int x = int(std::floor(doc.x())), y = int(std::floor(doc.y()));
    if (x < 0 || y < 0 || x >= flattened->width() || y >= flattened->height()) return;
    const uint8_t* p = flattened->pixel(x, y);
    if (p[3] == 0) return;
    QColor color(p[0] * 255 / p[3], p[1] * 255 / p[3], p[2] * 255 / p[3]);
    if (background) session_->backgroundColor = color; else session_->foregroundColor = color;
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
    double step = (e->modifiers() & Qt::ShiftModifier) ? 10 : 1;
    switch (e->key()) {
    case Qt::Key_Escape:
        if (session_->brushActive()) { session_->cancelBrush(); return; }
        if (session_->transformEdit()) { session_->cancelTransform(); return; }
        if (!lassoPoints_.empty()) { cancelLasso(); return; }
        if (crop_) { cancelCrop(); return; }
        return;
    case Qt::Key_Return: case Qt::Key_Enter:
        if (session_->transformEdit()) { session_->commitTransform(); return; }
        if (!lassoPoints_.empty() && polygonalLasso_) { finishPolygonalLasso(); return; }
        if (crop_) { applyCrop(); return; }
        return;
    case Qt::Key_Backspace: case Qt::Key_Delete:
        if (polygonalLasso_ && !lassoPoints_.empty()) { lassoPoints_.pop_back(); update(); return; }
        break;
    case Qt::Key_Left: case Qt::Key_Right: case Qt::Key_Up: case Qt::Key_Down: {
        double dx = e->key() == Qt::Key_Left ? -step : e->key() == Qt::Key_Right ? step : 0;
        double dy = e->key() == Qt::Key_Up ? -step : e->key() == Qt::Key_Down ? step : 0;
        if (session_->tool() == Tool::Move && session_->canTransform()) { session_->nudgeLayer(dx, dy); return; }
        break;
    }
    case Qt::Key_BracketLeft: case Qt::Key_BracketRight: {
        if (session_->tool() != Tool::Brush) break;
        double d = session_->brushSettings.diameter;
        double stepSize = d < 10 ? 1 : d < 50 ? 5 : d < 200 ? 10 : 50;
        d += e->key() == Qt::Key_BracketRight ? stepSize : -stepSize;
        session_->brushSettings.diameter = std::clamp(d, 1.0, 2000.0);
        emit session_->toolChanged();
        update();
        return;
    }
    default: break;
    }
    QWidget::keyPressEvent(e);
}

void CanvasWidget::keyReleaseEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Space && !e->isAutoRepeat()) { spaceHeld_ = false; if (drag_ == Drag::Pan && session_->tool() != Tool::Hand) drag_ = Drag::None; if (hover_) updateCursor(*hover_, e->modifiers()); return; }
    QWidget::keyReleaseEvent(e);
}

void CanvasWidget::leaveEvent(QEvent*) { hover_.reset(); update(); }
void CanvasWidget::focusOutEvent(QFocusEvent* e) { spaceHeld_ = false; QWidget::focusOutEvent(e); }

} // namespace app
