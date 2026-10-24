// The rings' shared controller (RingController.h): opening and closing, the keys while open, the pointer, placement.
#include "RingController.h"
#include "CanvasWidget.h"
#include "MainWindow.h"
#include "ToolRing.h"
#include <QAbstractSpinBox>
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QTabletEvent>
#include <QWheelEvent>
#include <QWindow>
#include <algorithm>
#include <cmath>

namespace app {

char compassKey(const QKeyEvent* e) {
    if (e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) return 0;
    switch (e->key()) {
    case Qt::Key_W: return 'W';
    case Qt::Key_E: return 'E';
    case Qt::Key_R: return 'R';
    case Qt::Key_S: return 'S';
    case Qt::Key_D: return 'D';
    case Qt::Key_F: return 'F';
    case Qt::Key_X: return 'X';
    case Qt::Key_C: return 'C';
    case Qt::Key_V: return 'V';
    default: return 0;
    }
}

QPointF compassDirection(char key) {
    constexpr double d = 0.70710678118654752;
    switch (key) {
    case 'W': return {-d, -d};
    case 'E': return {0, -1};
    case 'R': return {d, -d};
    case 'S': return {-1, 0};
    case 'F': return {1, 0};
    case 'X': return {-d, d};
    case 'C': return {0, 1};
    case 'V': return {d, d};
    default: return {0, 0};
    }
}

namespace {

/// Every ring of every window, so that opening one closes the others of its window.
std::vector<RingController*>& allRings() {
    static std::vector<RingController*> rings;
    return rings;
}

} // namespace

RingController::RingController(MainWindow* window, QWidget* overlay) : QObject(window), window_(window), overlay_(overlay) {
    allRings().push_back(this);
    idle_.setSingleShot(true);
    connect(&idle_, &QTimer::timeout, this, [this] { close(); });
}

RingController::~RingController() {
    auto& rings = allRings();
    rings.erase(std::remove(rings.begin(), rings.end(), this), rings.end());
    if (open_) qApp->removeEventFilter(this);
}

CanvasWidget* RingController::canvas() const { return window_->canvas_; }
EditorSession* RingController::currentSession() const { return window_->session_; }

std::pair<Tool, bool> RingController::toolKey() const {
    const EditorSession* s = window_->session_;
    if (!s) return {Tool::Move, false};
    return {s->tool(), s->tool() == Tool::Brush && s->brushErase};
}

bool RingController::typing() const {
    if (window_->canvas_ && window_->canvas_->typeEditing()) return true;
    // A text field has the keyboard (a layer's name, a filter box); a number field does not count, letters mean nothing there.
    QWidget* focus = QApplication::focusWidget();
    if (!focus) return false;
    if (focus->inherits("QTextEdit") || focus->inherits("QPlainTextEdit")) return true;
    return focus->inherits("QLineEdit") && !qobject_cast<QAbstractSpinBox*>(focus->parentWidget());
}

void RingController::sessionChanged() {
    close();
    for (const auto& c : sessionConnections_) disconnect(c);
    sessionConnections_.clear();
    session_ = window_->session_;
    if (!session_) return;
    connectSession(*session_);
    follow(connect(session_, &EditorSession::toolChanged, this, [this] {
        if (!open_) return;
        if (toolKey() != openedFor_) {
            toolSwitched();
            if (!open_) return;
        }
        refresh();
    }));
    for (auto signal : {&EditorSession::layersChanged, &EditorSession::selectionChanged, &EditorSession::historyChanged, &EditorSession::viewportChanged})
        follow(connect(session_, signal, this, [this] { if (open_) refresh(); }));
    if (CanvasWidget* canvas = window_->canvas_)
        follow(connect(canvas, &CanvasWidget::typeEditChanged, this, [this] { if (open_ && typing()) close(); }));
}

void RingController::toggle() {
    if (open_) close();
    else open();
}

void RingController::open() {
    if (open_ || !window_->canvas_ || !window_->session_ || typing() || !mayOpen()) return;
    CanvasWidget* canvas = window_->canvas_;
    if (!canvas->isVisible()) return;
    for (RingController* other : allRings())
        if (other != this && other->window_ == window_) other->close();
    if (session_ != window_->session_) sessionChanged();
    openedFor_ = toolKey();
    wheelRemainder_ = 0;
    heldKeys_.clear();
    openedAt_ = canvas->hoverPoint();
    open_ = true;
    opening();
    if (!open_) return;
    refresh();
    // By the pointer when it is over the canvas, else in the middle of it.
    const QPoint pointer = openedAt_ ? canvas->mapTo(window_, openedAt_->toPoint()) : canvasArea().center();
    lastPointer_ = pointer;
    heading_ = {};
    place(pointer, {});
    overlay_->show();
    overlay_->raise();
    qApp->installEventFilter(this);
    restartIdle();
}

void RingController::close() { close(false); }

void RingController::close(bool cancelled) {
    if (!open_) return;
    open_ = false;
    qApp->removeEventFilter(this);
    idle_.stop();
    heldKeys_.clear();
    overlay_->hide();
    closed(cancelled);
}

void RingController::restartIdle() {
    if (toolring::idleMs() > 0) idle_.start(toolring::idleMs());
}

QRect RingController::keepClear() const { return overlay_->rect(); }

QRect RingController::canvasArea() const {
    CanvasWidget* canvas = window_->canvas_;
    if (!canvas) return window_->rect();
    const QRect area(canvas->mapTo(window_, QPoint(0, 0)), canvas->size());
    return area.intersected(window_->rect()).adjusted(8, 8, -8, -8);
}

int RingController::clearance() const {
    const EditorSession* s = window_->session_;
    if (!s) return 0;
    double radius = 0;
    const Tool t = s->tool();
    if (t == Tool::Brush || t == Tool::SpotHealing || t == Tool::CloneStamp || t == Tool::Smudge || t == Tool::Dodge) radius = s->brushSettings.diameter / 2;
    else if (t == Tool::Scribble) radius = s->scribbleSize / 2.0;
    return int(std::clamp(radius * s->viewport.pointsPerPixel(), 0.0, 160.0));
}

void RingController::replace() {
    if (open_) place(lastPointer_, {});
}

void RingController::place(QPoint pointer, QPointF heading) {
    using toolring::Corner;
    const QRect area = canvasArea();
    const QSize size = overlay_->size();
    const QRect clear = keepClear();
    // The overlay's nearest edge this far from the pointer, beyond the brush's outline.
    const int gap = clearance() + 18;
    auto rectFor = [&](Corner c) {
        const bool right = c == Corner::UpRight || c == Corner::DownRight, up = c == Corner::UpRight || c == Corner::UpLeft;
        const int left = right ? pointer.x() + gap - clear.left() : pointer.x() - gap - clear.right() - 1;
        const int top = up ? pointer.y() - gap - size.height() : pointer.y() + gap - clear.top();
        return QRect(QPoint(left, top), size);
    };
    // Away from where the pointer is heading: moving right puts it on the left, moving down puts it above.
    const Corner preferred = toolring::corner();
    bool right = preferred == Corner::UpRight || preferred == Corner::DownRight, up = preferred == Corner::UpRight || preferred == Corner::UpLeft;
    if (!heading.isNull() && overlay_->isVisible()) {
        // Along an axis the pointer is not moving on, it keeps its side.
        const QPoint at = overlay_->geometry().center();
        right = std::abs(heading.x()) > 1.5 ? heading.x() < 0 : at.x() > pointer.x();
        up = std::abs(heading.y()) > 1.5 ? heading.y() > 0 : at.y() < pointer.y();
    }
    auto cornerOf = [](bool r, bool u) { return u ? (r ? Corner::UpRight : Corner::UpLeft) : (r ? Corner::DownRight : Corner::DownLeft); };
    const Corner order[] = {cornerOf(right, up), cornerOf(right, !up), cornerOf(!right, up), cornerOf(!right, !up)};
    QRect chosen;
    for (Corner c : order) {
        if (area.contains(rectFor(c))) { chosen = rectFor(c); break; }
    }
    if (chosen.isNull()) {
        // Nowhere it fits whole (a small canvas): the corner that keeps most of it inside, pushed in.
        int best = -1;
        for (Corner c : order) {
            const QRect r = rectFor(c).intersected(area);
            const int shown = r.isValid() ? r.width() * r.height() : 0;
            if (shown > best) { best = shown; chosen = rectFor(c); }
        }
        chosen.moveLeft(std::clamp(chosen.left(), area.left(), std::max(area.left(), area.right() - size.width() + 1)));
        chosen.moveTop(std::clamp(chosen.top(), area.top(), std::max(area.top(), area.bottom() - size.height() + 1)));
    }
    overlay_->move(chosen.topLeft());
    overlay_->raise();
}

void RingController::pointerMoved(QPoint pointer) {
    heading_ = heading_ * 0.6 + QPointF(pointer - lastPointer_) * 0.4;
    lastPointer_ = pointer;
    // Hysteresis: it stays put until the pointer (with the brush's outline) would reach it.
    const int margin = clearance() + 6;
    QRect clear = keepClear().translated(overlay_->pos());
    clear.setBottom(overlay_->geometry().bottom());   // the caption under it too
    const bool collides = clear.adjusted(-margin, -margin, margin, margin).contains(pointer);
    const bool outside = !canvasArea().contains(overlay_->geometry());
    if (collides || outside) place(pointer, heading_);
}

bool RingController::eventFilter(QObject* watched, QEvent* event) {
    if (!open_) return false;
    // Only what reaches this window; another window's keys are its own.
    auto ours = [this, watched] {
        if (auto* w = qobject_cast<QWidget*>(watched)) return w->window() == window_;
        return watched == static_cast<QObject*>(window_->windowHandle());
    };
    auto onCanvas = [this, watched] {
        auto* w = qobject_cast<QWidget*>(watched);
        CanvasWidget* canvas = window_->canvas_;
        return w && canvas && (w == canvas || canvas->isAncestorOf(w));
    };
    switch (event->type()) {
    case QEvent::ShortcutOverride: {
        // Its keys and Esc are not shortcuts while it is open (no tool switch, no colours, no screen mode).
        auto* ke = static_cast<QKeyEvent*>(event);
        if (!ours() || typing()) break;
        if (keyOf(ke) || (ke->key() == Qt::Key_Escape && ke->modifiers() == Qt::NoModifier)) { ke->accept(); return true; }
        break;
    }
    case QEvent::KeyPress: {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (!ours() || typing()) break;
        if (ke->key() == Qt::Key_Escape && ke->modifiers() == Qt::NoModifier) { escape(); return true; }
        if (const char key = keyOf(ke)) {
            heldKeys_.insert(ke->key());
            press(key, ke->isAutoRepeat());
            return true;
        }
        break;
    }
    case QEvent::KeyRelease: {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (heldKeys_.contains(ke->key())) {
            if (!ke->isAutoRepeat()) heldKeys_.remove(ke->key());
            return true;
        }
        break;
    }
    case QEvent::Wheel: {
        // Over the canvas, with no key held: a notch a step (the pointer stays where it is).
        auto* we = static_cast<QWheelEvent*>(event);
        if (!wantsWheel() || !onCanvas() || we->modifiers() != Qt::NoModifier || we->buttons() != Qt::NoButton) break;
        const QPoint delta = we->angleDelta();
        wheelRemainder_ += delta.y() ? delta.y() / 120.0 : delta.x() / 120.0;
        if (we->angleDelta().isNull()) wheelRemainder_ += we->pixelDelta().y() / 40.0;
        while (std::abs(wheelRemainder_) >= 1) {
            const bool more = wheelRemainder_ > 0;
            wheelRemainder_ -= more ? 1 : -1;
            wheelStep(more);
            if (!open_) break;
        }
        return true;
    }
    case QEvent::MouseButtonPress:
    case QEvent::TabletPress:
        if (onCanvas()) canvasPressed();
        break;
    case QEvent::MouseMove: {
        if (!ours()) break;
        pointerMoved(window_->mapFromGlobal(static_cast<QMouseEvent*>(event)->globalPosition().toPoint()));
        break;
    }
    case QEvent::TabletMove: {
        if (!ours()) break;
        pointerMoved(window_->mapFromGlobal(static_cast<QTabletEvent*>(event)->globalPosition().toPoint()));
        break;
    }
    case QEvent::WindowDeactivate:
        // A dialog, another window: the keyboard went elsewhere, and so does the ring.
        if (watched == window_) close();
        break;
    case QEvent::Resize:
        if (watched == window_->canvas_) place(lastPointer_, {});
        break;
    default:
        break;
    }
    return false;
}

} // namespace app
