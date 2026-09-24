// Canvas input: hit testing and cursors, the wheel and keyboard.
#include "CanvasWidget.h"
#include <cstring>
#include "QtGeometry.h"
#include <QKeyEvent>
#include <QNativeGestureEvent>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

using namespace compositor;

namespace app {

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
    case Tool::Marquee: case Tool::Lasso: case Tool::Wand: case Tool::Scribble: case Tool::Crop: case Tool::Gradient: case Tool::Shape: case Tool::Eyedropper: setCursor(Qt::CrossCursor); return;
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

void CanvasWidget::zoomGesture(double factor, QPointF viewPoint) {
    zoomSettle_.start();
    session_->zoomTo(session_->viewport.zoom * factor, viewPoint);
}

void CanvasWidget::wheelEvent(QWheelEvent* e) {
    if (!session_->hasDocument()) return;
    if (e->modifiers() & Qt::ControlModifier) {
        double steps = e->angleDelta().y() / 120.0;
        if (steps == 0) steps = e->pixelDelta().y() / 50.0;
        zoomGesture(std::pow(1.25, steps), e->position());
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
            zoomGesture(1 + g->value(), g->position());
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
        if (session_->tool() == Tool::Scribble && (!session_->scribbles().empty() || !session_->clickPrompts().empty())) { session_->clearScribbles(); session_->clearClickPrompts(); return; }
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
        if (session_->tool() == Tool::Scribble && session_->quickSelectClicks && !session_->clickPrompts().empty()) { session_->removeLastClickPrompt(); return; }
        if (session_->tool() == Tool::Scribble && !session_->quickSelectClicks && !session_->scribbles().empty()) { session_->removeLastScribble(); return; }
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
