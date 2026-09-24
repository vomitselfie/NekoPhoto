// Canvas pointer gestures: press, drag and release for every tool, with snapping.
#include "CanvasWidget.h"
#include <cstring>
#include "QtGeometry.h"
#include <QApplication>
#include <QMouseEvent>
#include <QTabletEvent>
#include <algorithm>
#include <cmath>

using namespace compositor;

namespace app {

// ---- Input ----------------------------------------------------------------------------

// A mouse is a pen at half pressure without tilt; the MyPaint presets read pressure, tilt and timing.
void CanvasWidget::mousePressEvent(QMouseEvent* e) { session_->pen = {0.5, 0, 0, qint64(e->timestamp()), false}; setFocus(); press(e->position(), e->button(), e->modifiers()); }
void CanvasWidget::mouseMoveEvent(QMouseEvent* e) { session_->pen = {0.5, 0, 0, qint64(e->timestamp()), false}; move(e->position(), e->buttons(), e->modifiers()); }
void CanvasWidget::mouseReleaseEvent(QMouseEvent* e) { session_->pen = {0.5, 0, 0, qint64(e->timestamp()), false}; release(e->position(), e->button(), e->modifiers()); }

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
    // Tilt arrives in degrees (about ±60 at most); MyPaint takes -1..1.
    session_->pen = {std::clamp(double(e->pressure()), 0.0, 1.0), std::clamp(e->xTilt() / 60.0, -1.0, 1.0), std::clamp(e->yTilt() / 60.0, -1.0, 1.0), qint64(e->timestamp()), true};
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
    case Tool::Scribble:
        if (session_->quickSelectClicks) {
            // A click marks the subject, Alt-click what is not it; a drag becomes a box on release.
            clickStart_ = clickCurrent_ = doc;
            clickBackground_ = modifiers & Qt::AltModifier;
            drag_ = Drag::ClickBox;
            update();
            return;
        }
        // Alt flips the stroke's kind for this stroke.
        scribbleBackground_ = session_->scribbleBackground != bool(modifiers & Qt::AltModifier);
        scribblePoints_ = {doc};
        drag_ = Drag::Scribble;
        update();
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

QRect CanvasWidget::brushCursorRect(QPointF at) const {
    const double r = session_->brushSettings.diameter / 2 * session_->viewport.pointsPerPixel() + 8;
    QRect rect = QRectF(at.x() - r, at.y() - r, 2 * r, 2 * r).toAlignedRect();
    if (session_->tool() == Tool::CloneStamp)
        if (auto sample = session_->cloneSamplePoint(documentPoint(at))) {
            const QPointF v = viewPoint(*sample);
            rect = rect.united(QRectF(v.x() - 12, v.y() - 12, 24, 24).toAlignedRect());
        }
    return rect;
}

void CanvasWidget::move(QPointF view, Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers) {
    const std::optional<QPointF> previous = hover_;
    hover_ = view;
    QPointF doc = documentPoint(view);
    emit cursorMoved(doc);
    if (drag_ == Drag::None) {
        if (session_->tool() == Tool::Lasso && session_->lassoKind == LassoKind::Polygonal && !lassoPoints_.empty()) { lassoCursor_ = doc; update(); }
        updateCursor(view, modifiers);
        // Only where the brush outline was and is now (and Clone Stamp's sample marker), not the whole view.
        if (isBrushLike()) { if (previous) update(brushCursorRect(*previous)); update(brushCursorRect(view)); }
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
    case Drag::Scribble:
        if (scribblePoints_.empty() || std::hypot(doc.x() - scribblePoints_.back().x(), doc.y() - scribblePoints_.back().y()) >= 0.5) scribblePoints_.push_back(doc);
        update();
        break;
    case Drag::ClickBox:
        clickCurrent_ = doc;
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
    case Drag::Scribble: {
        std::vector<QPointF> points = std::move(scribblePoints_);
        scribblePoints_.clear();
        session_->addScribble(points, scribbleBackground_);
        update();
        break;
    }
    case Drag::ClickBox:
        if (dragMoved_) session_->setClickBox(clickStart_, doc);
        else session_->addClickPrompt(clickStart_, clickBackground_);
        update();
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


} // namespace app
