// The window's edges, in full screen and autohiding in the standard window (WindowEdges.h): the engine. The flyouts'
// geometry and slides, the hot zones and the pointer, Tab and Shift+Tab, and the events; the pages are in
// WindowEdgePages.cpp, the screen modes in ScreenModes.cpp, the card in Flyout.cpp.
#include "WindowEdges.h"
#include "Flyout.h"
#include "CanvasWidget.h"
#include "EdgePages.h"
#include "HidePanels.h"
#include "MainWindow.h"
#include "Motion.h"
#include "ToolOptionsBar.h"
#include <QApplication>
#include <QBoxLayout>
#include <QDockWidget>
#include <QMenuBar>
#include <QTabletEvent>
#include <QVariantAnimation>
#include <QCursor>
#include <QDragMoveEvent>
#include <QEnterEvent>
#include <QHoverEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QScreen>
#include <QTimer>
#include <cmath>

using namespace nekophoto;

namespace app {

namespace {

Qt::Edge qtEdge(WindowEdges::Edge edge) {
    switch (edge) {
    case WindowEdges::Left: return Qt::LeftEdge;
    case WindowEdges::Top: return Qt::TopEdge;
    case WindowEdges::Right: return Qt::RightEdge;
    case WindowEdges::Bottom: return Qt::BottomEdge;
    }
    return Qt::RightEdge;
}

/// How far past the window's edge a polled pointer still counts as being at that edge, where that edge is the
/// screen's: a fast pen or a pointer crossing onto another monitor may never be seen in the last pixels.
constexpr int outside = 64;

/// A key as the shortcuts write it: Shift+Tab, not Shift+Backtab.
QKeySequence normalized(int key, Qt::KeyboardModifiers mods) {
    if (key == Qt::Key_Backtab) { key = Qt::Key_Tab; mods |= Qt::ShiftModifier; }
    return QKeySequence(QKeyCombination(mods & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier), Qt::Key(key)));
}

} // namespace

// ---- WindowEdges --------------------------------------------------------------------------------------------

WindowEdges::WindowEdges(MainWindow* window) : QObject(window), window_(window) {
    for (int i = 0; i < edgeCount; i++) {
        EdgeState& s = edges_[size_t(i)];
        const Edge edge = Edge(i);
        s.flyout = new Flyout(qtEdge(edge), window);
        s.strip = new PageStrip;
        s.deck = new PageDeck(qtEdge(edge));
        // The strip on the flyout's inner side: under the top's bar, over the others' pages.
        if (edge == Top) {
            s.flyout->body()->addWidget(s.deck, 1);
            s.flyout->body()->addWidget(s.strip);
        } else {
            s.flyout->body()->addWidget(s.strip);
            s.flyout->body()->addWidget(s.deck, 1);
        }
        buildPages(edge);
        connect(s.strip, &PageStrip::stepRequested, this, [this, edge](int step) { flip(edge, step); });
        connect(s.strip, &PageStrip::pageClicked, this, [this, edge](int shown) {
            const QStringList ids = pages(edge);
            if (shown >= 0 && shown < ids.size()) showPage(edge, ids[shown]);
        });
        s.slide = new QVariantAnimation(this);
        s.slide->setStartValue(0.0);
        s.slide->setEndValue(1.0);
        connect(s.slide, &QVariantAnimation::valueChanged, this, [this, i](const QVariant& v) {
            // Linear over the delay and the slide: the delay holds it where it was, the slide eases out.
            EdgeState& e = edges_[size_t(i)];
            const double at = v.toDouble() * (e.slideDelay + e.slideLength);
            e.progress = motion::lerp(e.slideFrom, e.slideTo, motion::easeOut(motion::window(at, e.slideDelay, e.slideLength)));
            relayout();
        });
        connect(s.slide, &QVariantAnimation::finished, this, [this, i] {
            EdgeState& e = edges_[size_t(i)];
            e.progress = e.target ? 1 : 0;
            if (!e.target) e.flyout->hide();
            relayout();
        });
        s.closeTimer = new QTimer(this);
        s.closeTimer->setSingleShot(true);
        connect(s.closeTimer, &QTimer::timeout, this, [this, edge] {
            const EdgeState& e = edges_[size_t(edge)];
            if (e.claimed && e.target && !e.pinned && !keepsOut(edge)) slide(edge, false);
        });
        // In the standard window the pointer rests in a hot zone a moment first: one passing through on its way out
        // of the window, or to the menus and the document tabs, brings nothing out.
        s.dwell = new QTimer(this);
        s.dwell->setSingleShot(true);
        connect(s.dwell, &QTimer::timeout, this, [this, edge] {
            const EdgeState& e = edges_[size_t(edge)];
            const bool blocked = tabletDown_ || (buttons_ != Qt::NoButton && pressEdge_ < 0);
            if (e.claimed && e.inZone && !e.target && !blocked) slide(edge, true);
        });
    }
    // The pointer's position is also polled: no widget reports it when it sits on a pixel nothing tracks, or has
    // left the window across the screen's edge.
    tick_ = new QTimer(this);
    tick_->setInterval(50);
    connect(tick_, &QTimer::timeout, this, [this] {
        if (polling_ && window_->isActiveWindow()) {
            const QPoint at = QCursor::pos(window_->screen());
            if (at != lastPolled_) {
                lastPolled_ = at;
                pointerAt(window_->mapFromGlobal(at));
            }
        }
        evaluate();
    });
    refreshTimer_ = new QTimer(this);
    refreshTimer_->setSingleShot(true);
    refreshTimer_->setInterval(120);
    connect(refreshTimer_, &QTimer::timeout, this, [this] {
        for (int i = 0; i < edgeCount; i++) {
            refreshStrip(Edge(i));
            if (edges_[size_t(i)].claimed && edges_[size_t(i)].progress > 0) refreshPage(Edge(i));
        }
    });
    holdTimer_ = new QTimer(this);
    holdTimer_->setSingleShot(true);
    connect(holdTimer_, &QTimer::timeout, this, [this] { if (heldKey_) peek(true, heldKey_ == 1); });
    // F7 and the Window menu show a panel: while its edge holds it, that edge slides out on its page instead.
    for (QDockWidget* dock : window->findChildren<QDockWidget*>(Qt::FindDirectChildrenOnly))
        connect(dock->toggleViewAction(), &QAction::triggered, this, [this, dock = QPointer<QDockWidget>(dock)] {
            if (!dock || edgeHolding(dock) < 0) return;
            dock->hide();
            revealPanel(dock);
        });
    connect(window, &MainWindow::automationEvent, this, &WindowEdges::followWindow);
    connect(edges::notifier(), &edges::Notifier::changed, this, &WindowEdges::applyPreferences);
    window->installEventFilter(this);
    window->canvasStack_->installEventFilter(this);
    qApp->installEventFilter(this);
}

WindowEdges::~WindowEdges() { qApp->removeEventFilter(this); }

QRect WindowEdges::area() const {
    if (active_) return window_->rect();
    const QWidget* canvas = window_->canvasStack_;
    return QRect(canvas->mapTo(window_, QPoint(0, 0)), canvas->size());
}

// ---- Geometry and the slides -----------------------------------------------------------------------------------

int WindowEdges::pageLength(Edge edge) const {
    // How tall the top and bottom rows are, their pages' own height (the bottom's Timeline as it was in the window).
    const EdgeState& s = edges_[size_t(edge)];
    const QRect a = area();
    const QMargins m = s.flyout->body()->contentsMargins();
    const int width = std::max(1, (edge == Top && !active_ ? window_->width() : a.width()) - m.left() - m.right());
    const int strip = s.strip->isHidden() ? 0 : s.strip->heightForWidth(width);
    int page = s.deck->sizeHint().height();
    if (edge == Bottom) page = this->page(Bottom) == QLatin1String("timeline") ? s.extent : std::max(page, 170);
    const int height = m.top() + m.bottom() + strip + page;
    return edge == Bottom ? std::min(height, std::max(120, a.height() / 2)) : height;
}

QRect WindowEdges::shownGeometry(Edge edge) const {
    const EdgeState& s = edges_[size_t(edge)];
    const QRect a = area();
    const QMargins m = s.flyout->body()->contentsMargins();
    const auto& top = edges_[Top];
    const auto& bottom = edges_[Bottom];
    const int topHeight = top.claimed ? pageLength(Top) : 0;
    const int bottomHeight = bottom.claimed ? pageLength(Bottom) : 0;
    const int topIn = int(std::lround(top.progress * topHeight)), bottomIn = int(std::lround(bottom.progress * bottomHeight));
    const int length = std::max(1, a.height() - topIn - bottomIn);
    switch (edge) {
    // The options bar as wide as the window, as it is in it (over the panels' column while it is out), so it keeps its
    // fields; in full screen the window is the area.
    case Top: return active_ ? QRect(a.left(), a.top(), a.width(), topHeight) : QRect(0, a.top(), window_->width(), topHeight);
    case Bottom: return QRect(a.left(), a.bottom() + 1 - bottomHeight, a.width(), bottomHeight);
    case Left: {
        const QSize hint = s.deck->sizeHint();
        const int width = std::min(std::max(hint.width(), s.strip->minimumSizeHint().width()) + m.left() + m.right(), std::max(60, a.width() / 2));
        const int inner = std::max(1, width - m.left() - m.right());
        const int strip = s.strip->isHidden() ? 0 : s.strip->heightForWidth(inner);
        const bool fill = s.current >= 0 && s.current < int(s.pages.size()) && s.pages[size_t(s.current)].fill;
        const int height = fill ? length : std::min(m.top() + m.bottom() + strip + hint.height(), length);
        return QRect(a.left(), a.top() + topIn, width, height);
    }
    case Right: {
        const int width = std::min(s.extent + m.left() + m.right(), std::max(240, a.width() / 2));   // a narrow window keeps half the canvas
        return QRect(a.right() + 1 - width, a.top() + topIn, width, length);
    }
    }
    return {};
}

void WindowEdges::relayout() {
    if (!anyClaimed()) return;
    const QRect a = area();
    for (int i = 0; i < edgeCount; i++) {
        const Edge edge = Edge(i);
        EdgeState& s = edges_[size_t(i)];
        if (!s.claimed) continue;
        QRect r = shownGeometry(edge);
        // Slid partly out: the hidden part lies past the edge.
        switch (edge) {
        case Left: r.moveLeft(a.left() + int(std::lround((s.progress - 1) * r.width()))); break;
        case Right: r.moveLeft(a.right() + 1 - int(std::lround(s.progress * r.width()))); break;
        case Top: r.moveTop(a.top() + int(std::lround((s.progress - 1) * r.height()))); break;
        case Bottom: r.moveTop(a.bottom() + 1 - int(std::lround(s.progress * r.height()))); break;
        }
        if (s.flyout->geometry() != r) s.flyout->setGeometry(r);
        s.flyout->setReveal(s.progress);
        // In the standard window the part past the canvas's area (over the document tabs, a dock) is not drawn; the top
        // row may lie over the panels' column, not over the tabs.
        const QRect allowed = !active_ && edge == Top ? QRect(0, a.top(), window_->width(), a.height()) : a;
        const QRect inside = allowed.intersected(r).translated(-r.topLeft());
        if (!active_ && !inside.isEmpty() && inside.size() != r.size()) s.flyout->setMask(QRegion(inside));
        else if (!s.flyout->mask().isEmpty()) s.flyout->clearMask();
    }
    if (menuStrip_ && menuStrip_->isVisible()) menuStrip_->setGeometry(0, 0, window_->width(), menuStrip_->sizeHint().height());
}

void WindowEdges::slide(Edge edge, bool out, int delayMs) {
    EdgeState& s = edges_[size_t(edge)];
    if (out && !hasContent(edge)) return;
    if (!out) s.peeked = false;
    if (s.target == out && (motion::running(s.slide) || s.progress == (out ? 1.0 : 0.0))) return;
    s.target = out;
    s.closeTimer->stop();
    s.dwell->stop();
    if (out) {
        s.opened = ++openCount_;
        s.deck->setBase(s.flyout->fill().isValid() ? s.flyout->fill() : s.flyout->palette().color(QPalette::Window));   // the theme may have changed
        refreshStrip(edge);
        refreshPage(edge);
        s.flyout->show();
        s.flyout->raise();
        if (menuStrip_ && menuStrip_->isVisible()) menuStrip_->raise();
    } else {
        s.held = false;
        s.deck->finishFlip();
        moveFocusOut(edge);
    }
    s.slide->stop();
    const double to = out ? 1.0 : 0.0;
    // Reduce motion (and the instant hook): there at once.
    const int ms = slideMs_ >= 0 ? slideMs_ : reduceMotion_ || motion::instant() ? 0 : motion::slideMs;
    if (ms == 0 || std::abs(to - s.progress) < 1e-6) {
        s.progress = to;
        if (!out) s.flyout->hide();
        relayout();
        return;
    }
    // From where it is (turning round mid-slide), the time scaled by how far it has to go.
    s.slideFrom = s.progress;
    s.slideTo = to;
    s.slideDelay = reduceMotion_ ? 0 : std::max(0, delayMs);
    s.slideLength = std::max(1, int(std::lround(ms * std::abs(to - s.progress))));
    s.slide->setDuration(s.slideDelay + s.slideLength);
    motion::start(s.slide);
}

void WindowEdges::moveFocusOut(Edge edge) {
    QWidget* focus = QApplication::focusWidget();
    if (focus && edges_[size_t(edge)].flyout->isAncestorOf(focus) && window_->canvas_) window_->canvas_->setFocus();
}

bool WindowEdges::pinnedAll(bool withTools) const {
    bool any = false;
    for (int i = 0; i < edgeCount; i++) {
        const bool want = (withTools || i != Left) && hasContent(Edge(i));
        if (edges_[size_t(i)].pinned != want) return false;
        any = any || want;
    }
    return any;
}

void WindowEdges::togglePinned(bool withTools) {
    if (active_) pin(!pinnedAll(withTools), withTools);
}

void WindowEdges::pin(bool on, bool withTools) {
    if (!active_) return;
    for (int i = 0; i < edgeCount; i++) {
        EdgeState& s = edges_[size_t(i)];
        const bool want = on && (withTools || i != Left) && hasContent(Edge(i));
        s.pinned = want;
        s.held = false;
        slide(Edge(i), want);
    }
}

void WindowEdges::peek(bool on, bool withTools) {
    peeking_ = on;
    // Out one after another, left, top, right, bottom (a little apart); back all together.
    int stagger = 0;
    for (int i = 0; i < edgeCount; i++) {
        EdgeState& s = edges_[size_t(i)];
        if (!s.claimed) continue;
        if (on) {
            if ((!withTools && i == Left) || s.target || !hasContent(Edge(i))) continue;
            slide(Edge(i), true, stagger);
            stagger += motion::peekStaggerMs;
            s.peeked = true;
        } else if (s.peeked) {
            // Let go: they slide back at once, but the one the pointer is in.
            s.peeked = false;
            if (!keepsOut(Edge(i))) slide(Edge(i), false);
        }
    }
    evaluate();
}

void WindowEdges::revealPanel(QDockWidget* dock) {
    const int at = edgeHolding(dock);
    if (at < 0) return;
    if (dock == window_->timelineDock_) timelineWanted_ = true;
    EdgeState& s = edges_[size_t(at)];
    const int index = pageIndex(Edge(at), pageHolding(dock));
    if (s.target && !s.pinned && s.current == index) { slide(Edge(at), false); return; }
    refreshStrip(Edge(at));
    showPageIndex(Edge(at), index, true, true);
    s.held = true;
    slide(Edge(at), true);
}

void WindowEdges::showEdge(Edge edge) {
    if (!edges_[size_t(edge)].claimed) return;
    edges_[size_t(edge)].held = true;
    slide(edge, true);
}

bool WindowEdges::closeFlyouts() {
    bool any = false;
    for (int i = 0; i < edgeCount; i++) {
        EdgeState& s = edges_[size_t(i)];
        if (!s.claimed) continue;
        if (s.target) any = true;
        s.pinned = s.held = s.peeked = false;
        slide(Edge(i), false);
    }
    return any;
}

void WindowEdges::showMenus() {
    MainWindow* w = window_;
    QAction* first = nullptr;
    for (QAction* a : w->menuBar()->actions()) if (a->menu() && a->isVisible() && a->isEnabled()) { first = a; break; }
    if (!first) return;
    if (!active_) {
        // The standard window: F10 opens the menu bar's first menu, as on other desktops.
        w->menuBar()->setActiveAction(first);
        return;
    }
    if (!menuStrip_) {
        // A second bar over the canvas sharing the window's menus (the window's own bar stays out of the layout).
        menuStrip_ = new QMenuBar(w);
        menuStrip_->setObjectName(QStringLiteral("menuStrip"));
        menuStrip_->addActions(w->menuBar()->actions());
        for (QAction* a : w->menuBar()->actions())
            if (QMenu* menu = a->menu()) connect(menu, &QMenu::aboutToHide, this, &WindowEdges::hideMenuStripLater);
    }
    menuStrip_->setGeometry(0, 0, w->width(), menuStrip_->sizeHint().height());
    menuStrip_->show();
    menuStrip_->raise();
    menuStrip_->setActiveAction(first);
}

void WindowEdges::hideMenuStripLater() {
    // Moving from one menu to the next closes one and opens the other: look once that has settled.
    QTimer::singleShot(0, this, [this] {
        if (menuStrip_ && !QApplication::activePopupWidget()) menuStrip_->hide();
    });
}

// ---- The pointer -----------------------------------------------------------------------------------------------

int WindowEdges::edgeOf(const QWidget* widget) const {
    for (int i = 0; i < edgeCount; i++) {
        const Flyout* f = edges_[size_t(i)].flyout;
        if (widget == f || f->isAncestorOf(widget)) return i;
    }
    return -1;
}

bool WindowEdges::atScreenEdge(Edge edge) const {
    if (active_ || window_->isFullScreen()) return true;
    const QScreen* screen = window_->screen();
    if (!screen) return false;
    const QRect sg = screen->geometry();
    const QRect a = area().translated(window_->mapToGlobal(QPoint(0, 0)));
    switch (edge) {
    case Left: return a.left() <= sg.left();
    case Right: return a.right() >= sg.right();
    case Top: return a.top() <= sg.top();
    case Bottom: return a.bottom() >= sg.bottom();
    }
    return false;
}

bool WindowEdges::inZone(Edge edge, QPoint p) const {
    const QRect a = area();
    // Past the area's side only where that side is the screen's (the pointer stops there, or runs on to the next
    // monitor), and the corners belong to the side columns.
    const int beforeTop = atScreenEdge(Top) ? outside : 0, pastBottom = atScreenEdge(Bottom) ? outside : 0;
    const int beforeLeft = atScreenEdge(Left) ? outside : 0, pastRight = atScreenEdge(Right) ? outside : 0;
    const bool across = p.y() >= a.top() - beforeTop && p.y() <= a.bottom() + pastBottom;
    const bool along = p.x() >= a.left() + zone_ && p.x() <= a.right() - zone_;
    switch (edge) {
    case Left: return across && p.x() < a.left() + zone_ && p.x() >= a.left() - beforeLeft;
    case Right: return across && p.x() > a.right() - zone_ && p.x() <= a.right() + pastRight;
    case Top: return along && p.y() < a.top() + zone_ && p.y() >= a.top() - beforeTop;
    case Bottom: return along && p.y() > a.bottom() - zone_ && p.y() <= a.bottom() + pastBottom;
    }
    return false;
}

void WindowEdges::pointerAt(QPoint p) {
    if (!anyClaimed()) return;
    lastPointer_ = p;
    pointerKnown_ = true;
    // A stroke on the canvas (a button or the pen down there) never pulls an edge out, even when it runs into one.
    const bool blocked = tabletDown_ || (buttons_ != Qt::NoButton && pressEdge_ < 0) || (menuStrip_ && menuStrip_->isVisible());
    bool overFlyout = false;
    for (const EdgeState& s : edges_)
        if (s.claimed && s.progress > 0 && s.flyout->isVisible() && s.flyout->geometry().contains(p)) overFlyout = true;
    for (int i = 0; i < edgeCount; i++) {
        EdgeState& s = edges_[size_t(i)];
        if (!s.claimed) continue;
        const bool zone = inZone(Edge(i), p) && !(overFlyout && !s.target);
        if (zone && !s.inZone && !blocked && !s.target) {
            if (dwellMs_ == 0 || atScreenEdge(Edge(i))) slide(Edge(i), true);
            else s.dwell->start(dwellMs_);
        }
        if (!zone || blocked) s.dwell->stop();
        s.inZone = zone;
        if (s.target && s.flyout->geometry().contains(p)) s.held = false;   // it has been visited
    }
    evaluate();
}

bool WindowEdges::keepsOut(Edge edge) const {
    const EdgeState& s = edges_[size_t(edge)];
    if (s.held || s.peeked) return true;
    if (pointerKnown_ && (s.inZone || s.flyout->geometry().adjusted(-2, -2, 2, 2).contains(lastPointer_))) return true;
    // A menu, a context menu, a combo's list or a dialog is open (one from the flyout keeps it until it closes).
    if (QApplication::activePopupWidget() || QApplication::activeModalWidget()) return true;
    if (QWidget* active = QApplication::activeWindow(); active && active != window_) return true;
    // A drag that began in it (a layer dragged onto the canvas), or a field in it with the keys.
    if (pressEdge_ == int(edge) && buttons_ != Qt::NoButton) return true;
    if (QWidget* focus = QApplication::focusWidget(); focus && s.flyout->isAncestorOf(focus) && takesTypedText(focus)) return true;
    return false;
}

void WindowEdges::evaluate() {
    for (int i = 0; i < edgeCount; i++) {
        EdgeState& s = edges_[size_t(i)];
        if (!s.claimed || !s.target || s.pinned || keepsOut(Edge(i))) { s.closeTimer->stop(); continue; }
        if (!s.closeTimer->isActive() || s.closeTimer->interval() != closeDelayMs_) s.closeTimer->start(closeDelayMs_);
    }
}

// ---- Tab and Shift+Tab -----------------------------------------------------------------------------------------

QList<QKeySequence> WindowEdges::commandKeys(const char* id) const {
    QList<QKeySequence> out;
    const Command* c = window_->commands_ ? window_->commandRegistry().find(QString::fromLatin1(id)) : nullptr;
    if (!c || !c->action) return out;
    for (const QKeySequence& k : c->action->shortcuts())
        if (k.count() == 1) out << normalized(int(k[0].key()), k[0].keyboardModifiers());
    return out;
}

bool WindowEdges::keyFree() const {
    // Not while a field has the keys, typing goes on on the canvas, or a menu or a dialog is open.
    QWidget* focus = QApplication::focusWidget();
    if (takesTypedText(focus)) return false;
    if (QApplication::activePopupWidget() || QApplication::activeModalWidget()) return false;
    if (QWidget* top = focus ? focus->window() : QApplication::activeWindow(); top && top != window_) return false;
    if (window_->canvas_ && window_->canvas_->typeEditing()) return false;
    return true;
}

bool WindowEdges::edgeKey(QObject*, QKeyEvent* ke) {
    const QEvent::Type type = ke->type();
    const QKeySequence key = normalized(ke->key(), ke->modifiers());
    const int code = int(key[0].key());
    if (type == QEvent::KeyRelease) {
        // The key held let go (whatever has the keys now): a hold brought the edges out, which go back; a tap is the
        // key's own command.
        if (!heldKey_ || code != heldCode_ || ke->isAutoRepeat()) return false;
        const int which = heldKey_;
        heldKey_ = 0;
        holdTimer_->stop();
        if (peeking_) peek(false, which == 1);
        else if (const Command* c = window_->commandRegistry().find(which == 1 ? QStringLiteral("view.screenMode.panels") : QStringLiteral("view.screenMode.panelsExceptTools")); c && c->action && c->action->isEnabled())
            c->action->trigger();
        return true;
    }
    if (type != QEvent::ShortcutOverride && type != QEvent::KeyPress) return false;
    if (heldKey_ && code == heldCode_) { ke->accept(); return true; }   // its repeats while held
    if (!anyClaimed() || !keyFree()) return false;
    auto matches = [&](const char* id) { return commandKeys(id).contains(key); };
    // An edge out: the keys of View's Next and Previous Edge Page flip it.
    if (const int target = flipTarget(); target >= 0 && pages(Edge(target)).size() > 1) {
        const int step = matches("view.edgePage.next") ? 1 : matches("view.edgePage.previous") ? -1 : 0;
        if (step) {
            ke->accept();
            if (type == QEvent::KeyPress && !ke->isAutoRepeat()) flip(Edge(target), step);
            return true;
        }
    }
    // None out: Show or Hide Panels' key (Tab) waits to see whether it is a tap or a hold.
    const bool panels = matches("view.screenMode.panels"), except = !panels && matches("view.screenMode.panelsExceptTools");
    if (!panels && !except) return false;
    bool hidden = false;
    for (int i = 0; i < edgeCount; i++)
        if (edges_[size_t(i)].claimed && !edges_[size_t(i)].target && hasContent(Edge(i))) hidden = true;
    if (!hidden) return false;
    ke->accept();
    if (type == QEvent::KeyPress && !ke->isAutoRepeat()) {
        heldKey_ = panels ? 1 : 2;
        heldCode_ = code;
        holdTimer_->start(holdMs_);
    }
    return true;
}

// ---- Events ----------------------------------------------------------------------------------------------------

bool WindowEdges::eventFilter(QObject* watched, QEvent* event) {
    const QEvent::Type type = event->type();
    if (watched == window_) {
        if (type == QEvent::Resize) {
            if (active_) relayout();
            else if (reapplyState_) {
                // Back at its size after full screen: the docks as they were.
                suspend();
                window_->restoreState(savedState_);
                for (size_t i = 0; i < window_->tabs_.size(); i++) window_->tabs_[i].options->setVisible(int(i) == window_->current_);
                if (window_->size() == normalSize_) reapplyState_ = false;
                emit normalLayoutRestored();
                resume();
            } else relayout();
        }
        if (type == QEvent::WindowDeactivate && heldKey_) {
            heldKey_ = 0;
            holdTimer_->stop();
            if (peeking_) peek(false, true);
        }
    }
    if (watched == window_->canvasStack_ && (type == QEvent::Resize || type == QEvent::Move)) {
        if (!active_) relayout();
        return false;
    }
    if (type == QEvent::ShortcutOverride || type == QEvent::KeyPress || type == QEvent::KeyRelease) {
        auto* ke = static_cast<QKeyEvent*>(event);
        if ((ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Backtab || heldKey_ || anyClaimed()) && edgeKey(watched, ke)) return true;
    }
    if (!anyClaimed() || !watched->isWidgetType()) return false;
    auto* target = static_cast<QWidget*>(watched);
    const bool ours = target->window() == window_;
    switch (type) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseMove: {
        if (!ours) break;
        auto* me = static_cast<QMouseEvent*>(event);
        if (type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick) {
            altArmed_ = false;
            // The first button of a gesture says where it began (the same press reaches the parents too).
            if (buttons_ == Qt::NoButton) pressEdge_ = edgeOf(target);
            buttons_ = me->buttons();
        } else {
            // A release, or a move with nothing held (a release this window never saw).
            buttons_ = me->buttons();
            if (buttons_ == Qt::NoButton) pressEdge_ = -1;
        }
        pointerAt(window_->mapFromGlobal(me->globalPosition().toPoint()));
        break;
    }
    case QEvent::TabletPress:
    case QEvent::TabletRelease:
    case QEvent::TabletMove: {
        if (!ours) break;
        auto* te = static_cast<QTabletEvent*>(event);
        if (type == QEvent::TabletPress) {
            if (!tabletDown_ && buttons_ == Qt::NoButton) pressEdge_ = edgeOf(target);
            tabletDown_ = pressEdge_ < 0;
        } else if (type == QEvent::TabletRelease) {
            tabletDown_ = false;
        }
        pointerAt(window_->mapFromGlobal(te->globalPosition().toPoint()));
        break;
    }
    case QEvent::HoverMove:
        if (ours) pointerAt(window_->mapFromGlobal(static_cast<QHoverEvent*>(event)->globalPosition().toPoint()));
        break;
    case QEvent::Enter:
        if (ours) pointerAt(window_->mapFromGlobal(static_cast<QEnterEvent*>(event)->globalPosition().toPoint()));
        break;
    case QEvent::DragEnter:
    case QEvent::DragMove:
        if (ours) pointerAt(target->mapTo(window_, static_cast<QDragMoveEvent*>(event)->position().toPoint()));
        break;
    case QEvent::Drop:
        // The drag is over: the press that began it will not be released here.
        pressEdge_ = -1;
        buttons_ = Qt::NoButton;
        QTimer::singleShot(0, this, &WindowEdges::evaluate);
        break;
    case QEvent::ShortcutOverride: {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() != Qt::Key_Alt) altArmed_ = false;
        // (Tab and Shift+Tab in a field stay with it: HidePanels sees to that in either screen mode.)
        break;
    }
    case QEvent::KeyPress: {
        if (!active_) break;
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Alt) { if (!ke->isAutoRepeat()) altArmed_ = (ke->modifiers() & ~Qt::AltModifier) == Qt::NoModifier; }
        else altArmed_ = false;
        break;
    }
    case QEvent::KeyRelease: {
        if (!active_) break;
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Alt && !ke->isAutoRepeat() && altArmed_ && ours) {
            // Alt pressed and let go alone: the menus, as F10.
            altArmed_ = false;
            QTimer::singleShot(0, this, &WindowEdges::showMenus);
        }
        break;
    }
    case QEvent::WindowDeactivate:
        altArmed_ = false;
        break;
    case QEvent::FocusIn:
        if (ours) QTimer::singleShot(0, this, &WindowEdges::evaluate);
        break;
    default:
        break;
    }
    return false;
}

} // namespace app
