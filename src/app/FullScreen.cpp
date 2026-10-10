// Full screen mode with edge flyouts: see FullScreen.h.
#include "FullScreen.h"
#include "CanvasWidget.h"
#include "MainWindow.h"
#include "ToolOptionsBar.h"
#include <QApplication>
#include <QBoxLayout>
#include <QCursor>
#include <QDockWidget>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEasingCurve>
#include <QEnterEvent>
#include <QHoverEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSplitter>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTabletEvent>
#include <QTimer>
#include <QToolBar>
#include <QVariantAnimation>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>

namespace app {

namespace {

/// A field that takes typed text: Tab and the letters stay with it.
bool isTextField(const QWidget* w) {
    return w && (w->inherits("QLineEdit") || w->inherits("QAbstractSpinBox") || w->inherits("QTextEdit") || w->inherits("QPlainTextEdit"));
}

Qt::Edge qtEdge(FullScreenMode::Edge edge) {
    switch (edge) {
    case FullScreenMode::Left: return Qt::LeftEdge;
    case FullScreenMode::Top: return Qt::TopEdge;
    case FullScreenMode::Right: return Qt::RightEdge;
    case FullScreenMode::Bottom: return Qt::BottomEdge;
    }
    return Qt::RightEdge;
}

/// How far past the window's edge a polled pointer still counts as being at that edge: a fast pen or a pointer
/// crossing onto another monitor may never be seen in the last pixels.
constexpr int outside = 64;

} // namespace

// ---- Flyout ----------------------------------------------------------------------------------------------------

Flyout::Flyout(Qt::Edge edge, QWidget* parent) : QWidget(parent), edge_(edge) {
    static const char* names[] = {"flyoutTop", "flyoutLeft", "flyoutRight", "flyoutBottom"};
    const int index = edge == Qt::TopEdge ? 0 : edge == Qt::LeftEdge ? 1 : edge == Qt::RightEdge ? 2 : 3;
    setObjectName(QString::fromLatin1(names[index]));
    body_ = new QBoxLayout(QBoxLayout::TopToBottom, this);
    body_->setSpacing(0);
    updateMargins();
    hide();
}

void Flyout::setShadow(int s) {
    shadow_ = std::clamp(s, 0, 48);
    updateMargins();
    update();
}

void Flyout::updateMargins() {
    // The shadow on every side but the screen's, then room for the rounded corners.
    const int m = shadow_, pad = radius_ / 3;
    QMargins margins(m + pad, m + pad, m + pad, m + pad);
    switch (edge_) {
    case Qt::LeftEdge: margins.setLeft(0); break;
    case Qt::RightEdge: margins.setRight(0); break;
    case Qt::TopEdge: margins.setTop(0); break;
    case Qt::BottomEdge: margins.setBottom(0); break;
    }
    body_->setContentsMargins(margins);
}

QRect Flyout::cardRect() const {
    QRect r = rect();
    const int m = shadow_;
    if (edge_ != Qt::LeftEdge) r.setLeft(r.left() + m);
    if (edge_ != Qt::RightEdge) r.setRight(r.right() - m);
    if (edge_ != Qt::TopEdge) r.setTop(r.top() + m);
    if (edge_ != Qt::BottomEdge) r.setBottom(r.bottom() - m);
    return r;
}

void Flyout::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    // The card runs past the screen's edge, so only its inner corners are rounded.
    QRectF card = QRectF(cardRect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const double r = radius_;
    switch (edge_) {
    case Qt::LeftEdge: card.setLeft(card.left() - r - 2); break;
    case Qt::RightEdge: card.setRight(card.right() + r + 2); break;
    case Qt::TopEdge: card.setTop(card.top() - r - 2); break;
    case Qt::BottomEdge: card.setBottom(card.bottom() + r + 2); break;
    }
    // A soft shadow: rings fading outwards, darkest at the card.
    for (int i = shadow_; i >= 1; i--) {
        QPainterPath ring;
        ring.addRoundedRect(card.adjusted(-i, -i + 1, i, i + 1), r + i, r + i);
        p.fillPath(ring, QColor(0, 0, 0, 7));
    }
    QPainterPath shape;
    shape.addRoundedRect(card, r, r);
    p.fillPath(shape, fill_.isValid() ? fill_ : palette().color(QPalette::Window));
    p.setPen(QPen(border_.isValid() ? border_ : palette().color(QPalette::Mid), 1));
    p.drawPath(shape);
}

// ---- FullScreenMode --------------------------------------------------------------------------------------------

FullScreenMode::FullScreenMode(MainWindow* window) : QObject(window), window_(window) {
    for (int i = 0; i < edgeCount; i++) {
        EdgeState& s = edges_[size_t(i)];
        const Edge edge = Edge(i);
        s.flyout = new Flyout(qtEdge(edge), window);
        s.slide = new QVariantAnimation(this);
        s.slide->setEasingCurve(QEasingCurve::OutCubic);
        connect(s.slide, &QVariantAnimation::valueChanged, this, [this, i](const QVariant& v) {
            edges_[size_t(i)].progress = v.toDouble();
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
            if (active_ && e.target && !e.pinned && !keepsOut(edge)) slide(edge, false);
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
    // F7 and the Window menu show a panel: in full screen, its edge slides out instead of the dock showing.
    for (QDockWidget* dock : window->findChildren<QDockWidget*>(Qt::FindDirectChildrenOnly))
        connect(dock->toggleViewAction(), &QAction::triggered, this, [this, dock = QPointer<QDockWidget>(dock)] {
            if (!active_ || !dock) return;
            dock->hide();
            revealPanel(dock);
        });
    window->installEventFilter(this);
}

FullScreenMode::~FullScreenMode() {
    if (active_) qApp->removeEventFilter(this);
}

bool FullScreenMode::hasContent(Edge edge) const {
    const EdgeState& s = edges_[size_t(edge)];
    if (!s.groups.empty()) return true;
    if (edge == Left) return rail_;
    if (edge == Top) return shownOptions_;
    return false;
}

QTabWidget* FullScreenMode::tabsHolding(QDockWidget* dock) const {
    for (const EdgeState& s : edges_)
        for (const Group& g : s.groups)
            if (std::find(g.docks.begin(), g.docks.end(), dock) != g.docks.end()) return g.tabs;
    return nullptr;
}

int FullScreenMode::edgeHolding(QDockWidget* dock) const {
    for (int i = 0; i < edgeCount; i++)
        for (const Group& g : edges_[size_t(i)].groups)
            if (std::find(g.docks.begin(), g.docks.end(), dock) != g.docks.end()) return i;
    return -1;
}

void FullScreenMode::enter() {
    if (active_) return;
    MainWindow* w = window_;
    savedState_ = w->saveState();
    savedGeometry_ = w->saveGeometry();
    savedWindowState_ = w->windowState();
    normalSize_ = w->size();
    reapplyState_ = false;
    active_ = true;
    pointerKnown_ = false;
    pressEdge_ = -1;
    buttons_ = Qt::NoButton;
    tabletDown_ = altArmed_ = false;
    build();
    menuBarShown_ = !w->menuBar()->isHidden();
    statusBarShown_ = !w->statusBar()->isHidden();
    tabBarShown_ = !w->tabBar_->isHidden();
    // The menus' keys work without the menu bar on screen only when the window holds the actions too.
    addWindowShortcuts();
    w->menuBar()->hide();
    w->statusBar()->hide();
    w->tabBar_->hide();
    for (EdgeState& s : edges_) s.flyout->ensurePolished();
    qApp->installEventFilter(this);
    w->showFullScreen();
    tick_->start();
    relayout();
    emit activeChanged(true);
}

void FullScreenMode::build() {
    MainWindow* w = window_;
    const QList<QDockWidget*> docks = w->findChildren<QDockWidget*>(Qt::FindDirectChildrenOnly);
    // Each tab group's order and its tab in front, from the window's own dock tab bars (each tab's data is its dock).
    std::map<QDockWidget*, std::pair<QTabBar*, int>> tabAt;
    for (QTabBar* bar : w->findChildren<QTabBar*>(Qt::FindDirectChildrenOnly)) {
        if (bar->isHidden()) continue;
        for (int i = 0; i < bar->count(); i++) {
            auto* dock = reinterpret_cast<QDockWidget*>(bar->tabData(i).value<quintptr>());
            if (docks.contains(dock)) tabAt[dock] = {bar, i};
        }
    }
    struct Pending {
        Edge edge;
        std::vector<QDockWidget*> docks;
        QDockWidget* current;
        QRect geometry;
    };
    std::vector<Pending> pending;
    std::set<QDockWidget*> placed;
    for (QDockWidget* d : docks) {
        if (d->isHidden() || placed.count(d) || !d->widget()) continue;
        std::vector<QDockWidget*> group{d};
        for (QDockWidget* t : w->tabifiedDockWidgets(d))
            if (!t->isHidden() && t->widget() && !placed.count(t)) group.push_back(t);
        for (QDockWidget* g : group) placed.insert(g);
        QDockWidget* current = group.front();
        if (auto it = tabAt.find(d); it != tabAt.end() && group.size() > 1) {
            std::stable_sort(group.begin(), group.end(), [&](QDockWidget* a, QDockWidget* b) {
                const auto ia = tabAt.find(a), ib = tabAt.find(b);
                return (ia == tabAt.end() ? 1000 : ia->second.second) < (ib == tabAt.end() ? 1000 : ib->second.second);
            });
            const int front = it->second.first->currentIndex();
            for (QDockWidget* g : group)
                if (auto gt = tabAt.find(g); gt != tabAt.end() && gt->second.second == front) current = g;
        } else {
            // Without the tab bar: the tab in front is the one on screen (the others are moved out of sight).
            for (QDockWidget* g : group)
                if (g->geometry().right() >= 0 && g->geometry().bottom() >= 0) { current = g; break; }
        }
        Edge edge = Right;
        if (!d->isFloating()) {
            switch (w->dockWidgetArea(d)) {
            case Qt::LeftDockWidgetArea: edge = Left; break;
            case Qt::TopDockWidgetArea: edge = Top; break;
            case Qt::BottomDockWidgetArea: edge = Bottom; break;
            default: edge = Right; break;
            }
        }
        pending.push_back({edge, group, current, current->geometry()});
    }
    // Top to bottom down the columns, left to right along the rows, as the window has them.
    std::stable_sort(pending.begin(), pending.end(), [](const Pending& a, const Pending& b) {
        if (a.edge != b.edge) return a.edge < b.edge;
        const bool row = a.edge == Top || a.edge == Bottom;
        return row ? std::make_pair(a.geometry.x(), a.geometry.y()) < std::make_pair(b.geometry.x(), b.geometry.y())
                   : std::make_pair(a.geometry.y(), a.geometry.x()) < std::make_pair(b.geometry.y(), b.geometry.x());
    });
    std::array<QList<int>, edgeCount> sizes;
    for (EdgeState& s : edges_) s.extent = 0;
    for (const Pending& p : pending) {
        EdgeState& s = edges_[size_t(p.edge)];
        const bool row = p.edge == Top || p.edge == Bottom;
        s.extent = std::max(s.extent, row ? p.geometry.height() : p.geometry.width());
        addGroup(p.edge, p.docks, p.current);
        sizes[size_t(p.edge)] << std::max(1, row ? p.geometry.width() : p.geometry.height());
    }
    for (int i = 0; i < edgeCount; i++)
        if (edges_[size_t(i)].splitter) edges_[size_t(i)].splitter->setSizes(sizes[size_t(i)]);
    for (QDockWidget* d : docks) if (!d->isHidden()) d->hide();

    // The tool rail on the left, the tab's options bar at the top.
    rail_ = w->toolRail_ && !w->toolRail_->isHidden() ? w->toolRail_ : nullptr;   // a rail the person closed stays closed
    if (rail_) {
        w->removeToolBar(rail_);
        edges_[Left].flyout->body()->insertWidget(0, rail_, 0, Qt::AlignTop | Qt::AlignLeft);
        rail_->show();
    }
    optionBars_.clear();
    shownOptions_ = nullptr;
    for (const auto& tab : w->tabs_) {
        optionBars_.emplace_back(tab.options);
        w->removeToolBar(tab.options);
    }
    placeOptionsBar();
}

FullScreenMode::Group& FullScreenMode::addGroup(Edge edge, const std::vector<QDockWidget*>& docks, QDockWidget* current) {
    EdgeState& s = edges_[size_t(edge)];
    if (!s.splitter) {
        s.splitter = new QSplitter(edge == Top || edge == Bottom ? Qt::Horizontal : Qt::Vertical);
        s.splitter->setChildrenCollapsible(false);
        s.flyout->body()->addWidget(s.splitter, 1);
    }
    Group g;
    g.tabs = new QTabWidget;
    g.tabs->setObjectName(QStringLiteral("flyoutPanels"));
    g.tabs->setDocumentMode(true);
    for (QDockWidget* d : docks) {
        QWidget* content = d->widget();
        if (!content) continue;
        moved_.push_back({d, content});
        g.tabs->addTab(content, d->windowTitle());   // the tab widget shows only the page in front
        g.docks.push_back(d);
        if (d == current) g.tabs->setCurrentWidget(content);
    }
    s.splitter->addWidget(g.tabs);
    s.groups.push_back(g);
    return s.groups.back();
}

void FullScreenMode::placeOptionsBar() {
    MainWindow* w = window_;
    if (w->current_ < 0 || w->current_ >= int(w->tabs_.size())) return;
    QToolBar* bar = w->tabs_[size_t(w->current_)].options;
    if (bar == shownOptions_) { if (bar && bar->isHidden()) bar->show(); return; }
    if (shownOptions_) {
        // Parked on the window, out of sight, until the window takes the bars back.
        edges_[Top].flyout->body()->removeWidget(shownOptions_);
        shownOptions_->hide();
        shownOptions_->setParent(w);
        shownOptions_->hide();
    }
    // A tab opened in full screen put its bar in the window's layout.
    if (std::find(optionBars_.begin(), optionBars_.end(), QPointer<QToolBar>(bar)) == optionBars_.end()) {
        w->removeToolBar(bar);
        optionBars_.emplace_back(bar);
    }
    edges_[Top].flyout->body()->insertWidget(0, bar);
    bar->show();
    shownOptions_ = bar;
    relayout();
}

void FullScreenMode::currentTabChanged() {
    if (active_) placeOptionsBar();
}

void FullScreenMode::addWindowShortcuts() {
    MainWindow* w = window_;
    const QList<QAction*> own = w->actions();
    std::function<void(QMenu*)> walk = [&](QMenu* menu) {
        for (QAction* a : menu->actions()) {
            if (QMenu* sub = a->menu()) { walk(sub); continue; }
            if (a->shortcuts().isEmpty() || own.contains(a) || addedShortcuts_.contains(a)) continue;
            w->addAction(a);
            addedShortcuts_ << a;
        }
    };
    for (QAction* top : w->menuBar()->actions())
        if (QMenu* menu = top->menu()) walk(menu);
}

void FullScreenMode::leave() {
    if (!active_) return;
    MainWindow* w = window_;
    active_ = false;
    tick_->stop();
    qApp->removeEventFilter(this);
    for (EdgeState& s : edges_) {
        s.slide->stop();
        s.closeTimer->stop();
        s.target = s.pinned = s.held = s.inZone = false;
        s.progress = 0;
        s.flyout->hide();
    }
    if (menuStrip_) menuStrip_->hide();
    // The panels back in their docks, the tab groups and splitters gone.
    for (const Moved& m : moved_)
        if (m.dock && m.content) {
            m.dock->setWidget(m.content);
            m.content->show();   // a tab behind another was hidden by its tab widget
        }
    moved_.clear();
    for (EdgeState& s : edges_) {
        delete s.splitter;
        s.splitter = nullptr;
        s.groups.clear();
        s.extent = 0;
    }
    if (rail_) {
        edges_[Left].flyout->body()->removeWidget(rail_);
        w->addToolBar(Qt::LeftToolBarArea, rail_);
    }
    if (shownOptions_) edges_[Top].flyout->body()->removeWidget(shownOptions_);
    for (const auto& bar : optionBars_)
        if (bar) w->addToolBar(Qt::TopToolBarArea, bar);
    optionBars_.clear();
    shownOptions_ = nullptr;
    rail_ = nullptr;
    for (QAction* a : addedShortcuts_) w->removeAction(a);
    addedShortcuts_.clear();
    w->menuBar()->setVisible(menuBarShown_);
    w->statusBar()->setVisible(statusBarShown_);
    w->tabBar_->setVisible(tabBarShown_);
    if (savedWindowState_ & Qt::WindowMaximized) {
        w->showMaximized();
    } else {
        w->showNormal();
        w->restoreGeometry(savedGeometry_);
        // restoreGeometry fits the window to the screen; one that was larger than it comes back at its own size.
        if (w->size() != normalSize_) w->resize(normalSize_);
    }
    w->restoreState(savedState_);
    for (size_t i = 0; i < w->tabs_.size(); i++) w->tabs_[i].options->setVisible(int(i) == w->current_);
    // The window comes back to its size a moment later on most desktops: the layout is restored again then.
    reapplyState_ = true;
    QTimer::singleShot(1500, this, [this] { reapplyState_ = false; });
    if (w->canvas_) w->canvas_->setFocus();
    emit activeChanged(false);
}

QRect FullScreenMode::shownGeometry(Edge edge) const {
    const EdgeState& s = edges_[size_t(edge)];
    const int W = window_->width(), H = window_->height();
    const QMargins m = s.flyout->body()->contentsMargins();
    const auto& top = edges_[Top];
    const auto& bottom = edges_[Bottom];
    const int topHeight = top.flyout->sizeHint().height();
    const int bottomHeight = bottom.extent + bottom.flyout->body()->contentsMargins().top() + bottom.flyout->body()->contentsMargins().bottom();
    const int topIn = int(std::lround(top.progress * topHeight)), bottomIn = int(std::lround(bottom.progress * bottomHeight));
    switch (edge) {
    case Top: return QRect(0, 0, W, topHeight);
    case Bottom: return QRect(0, H - bottomHeight, W, bottomHeight);
    case Left: {
        const QSize hint = s.flyout->sizeHint();
        return QRect(0, topIn, hint.width(), std::max(1, std::min(hint.height(), H - topIn - bottomIn)));
    }
    case Right: {
        const int width = std::min(s.extent + m.left() + m.right(), std::max(240, W / 2));   // a narrow screen keeps half the canvas
        return QRect(W - width, topIn, width, std::max(1, H - topIn - bottomIn));
    }
    }
    return {};
}

void FullScreenMode::relayout() {
    if (!active_) return;
    for (int i = 0; i < edgeCount; i++) {
        const Edge edge = Edge(i);
        const EdgeState& s = edges_[size_t(i)];
        QRect r = shownGeometry(edge);
        // Slid partly out: the hidden part lies past the screen's edge.
        switch (edge) {
        case Left: r.moveLeft(int(std::lround((s.progress - 1) * r.width()))); break;
        case Right: r.moveLeft(window_->width() - int(std::lround(s.progress * r.width()))); break;
        case Top: r.moveTop(int(std::lround((s.progress - 1) * r.height()))); break;
        case Bottom: r.moveTop(window_->height() - int(std::lround(s.progress * r.height()))); break;
        }
        if (s.flyout->geometry() != r) s.flyout->setGeometry(r);
    }
    if (menuStrip_ && menuStrip_->isVisible()) menuStrip_->setGeometry(0, 0, window_->width(), menuStrip_->sizeHint().height());
}

void FullScreenMode::slide(Edge edge, bool out) {
    EdgeState& s = edges_[size_t(edge)];
    if (out && !hasContent(edge)) return;
    if (s.target == out && (s.slide->state() == QAbstractAnimation::Running || s.progress == (out ? 1.0 : 0.0))) return;
    s.target = out;
    s.closeTimer->stop();
    if (out) {
        s.flyout->show();
        s.flyout->raise();
        if (menuStrip_ && menuStrip_->isVisible()) menuStrip_->raise();
    } else {
        s.held = false;
        moveFocusOut(edge);
    }
    s.slide->stop();
    const double to = out ? 1.0 : 0.0;
    if (slideMs_ == 0 || std::abs(to - s.progress) < 1e-6) {
        s.progress = to;
        if (!out) s.flyout->hide();
        relayout();
        return;
    }
    s.slide->setStartValue(s.progress);
    s.slide->setEndValue(to);
    s.slide->setDuration(std::max(1, int(std::lround(slideMs_ * std::abs(to - s.progress)))));
    s.slide->start();
}

void FullScreenMode::moveFocusOut(Edge edge) {
    QWidget* focus = QApplication::focusWidget();
    if (focus && edges_[size_t(edge)].flyout->isAncestorOf(focus) && window_->canvas_) window_->canvas_->setFocus();
}

bool FullScreenMode::pinnedAll(bool withTools) const {
    bool any = false;
    for (int i = 0; i < edgeCount; i++) {
        const bool want = (withTools || i != Left) && hasContent(Edge(i));
        if (edges_[size_t(i)].pinned != want) return false;
        any = any || want;
    }
    return any;
}

void FullScreenMode::togglePinned(bool withTools) {
    if (active_) pin(!pinnedAll(withTools), withTools);
}

void FullScreenMode::pin(bool on, bool withTools) {
    if (!active_) return;
    for (int i = 0; i < edgeCount; i++) {
        EdgeState& s = edges_[size_t(i)];
        const bool want = on && (withTools || i != Left) && hasContent(Edge(i));
        s.pinned = want;
        s.held = false;
        slide(Edge(i), want);
    }
}

void FullScreenMode::revealPanel(QDockWidget* dock) {
    if (!active_ || !dock) return;
    int at = edgeHolding(dock);
    if (at < 0) {
        // A panel that was closed when full screen began: it joins the edge its dock belongs to.
        Edge edge = Right;
        if (!dock->isFloating()) {
            const Qt::DockWidgetArea area = window_->dockWidgetArea(dock);
            edge = area == Qt::LeftDockWidgetArea ? Left : area == Qt::TopDockWidgetArea ? Top : area == Qt::BottomDockWidgetArea ? Bottom : Right;
        }
        if (!dock->widget()) return;
        EdgeState& s = edges_[size_t(edge)];
        if (s.extent <= 0) s.extent = edge == Top || edge == Bottom ? std::max(160, dock->widget()->sizeHint().height()) : std::max(240, dock->widget()->sizeHint().width());
        addGroup(edge, {dock}, dock);
        at = edge;
    }
    QTabWidget* tabs = tabsHolding(dock);
    QWidget* content = nullptr;
    for (const Moved& m : moved_) if (m.dock == dock) content = m.content;
    if (!tabs || !content) return;
    EdgeState& s = edges_[size_t(at)];
    if (s.target && !s.pinned && tabs->currentWidget() == content) { slide(Edge(at), false); return; }
    tabs->setCurrentWidget(content);
    s.held = true;
    slide(Edge(at), true);
}

void FullScreenMode::showEdge(Edge edge) {
    if (!active_) return;
    edges_[size_t(edge)].held = true;
    slide(edge, true);
}

bool FullScreenMode::closeFlyouts() {
    bool any = false;
    for (int i = 0; i < edgeCount; i++) {
        EdgeState& s = edges_[size_t(i)];
        if (s.target) any = true;
        s.pinned = s.held = false;
        slide(Edge(i), false);
    }
    return any;
}

void FullScreenMode::showMenus() {
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
            if (QMenu* menu = a->menu()) connect(menu, &QMenu::aboutToHide, this, &FullScreenMode::hideMenuStripLater);
    }
    menuStrip_->setGeometry(0, 0, w->width(), menuStrip_->sizeHint().height());
    menuStrip_->show();
    menuStrip_->raise();
    menuStrip_->setActiveAction(first);
}

void FullScreenMode::hideMenuStripLater() {
    // Moving from one menu to the next closes one and opens the other: look once that has settled.
    QTimer::singleShot(0, this, [this] {
        if (menuStrip_ && !QApplication::activePopupWidget()) menuStrip_->hide();
    });
}

int FullScreenMode::edgeOf(const QWidget* widget) const {
    for (int i = 0; i < edgeCount; i++) {
        const Flyout* f = edges_[size_t(i)].flyout;
        if (widget == f || f->isAncestorOf(widget)) return i;
    }
    return -1;
}

bool FullScreenMode::inZone(Edge edge, QPoint p) const {
    const int W = window_->width(), H = window_->height();
    const bool across = p.y() >= -outside && p.y() < H + outside;
    const bool along = p.x() >= zone_ && p.x() < W - zone_;   // the corners belong to the side columns
    switch (edge) {
    case Left: return across && p.x() < zone_ && p.x() >= -outside;
    case Right: return across && p.x() >= W - zone_ && p.x() < W + outside;
    case Top: return along && p.y() < zone_ && p.y() >= -outside;
    case Bottom: return along && p.y() >= H - zone_ && p.y() < H + outside;
    }
    return false;
}

void FullScreenMode::pointerAt(QPoint p) {
    if (!active_) return;
    lastPointer_ = p;
    pointerKnown_ = true;
    // A stroke on the canvas (a button or the pen down there) never pulls an edge out, even when it runs into one.
    const bool blocked = tabletDown_ || (buttons_ != Qt::NoButton && pressEdge_ < 0) || (menuStrip_ && menuStrip_->isVisible());
    bool overFlyout = false;
    for (const EdgeState& s : edges_)
        if (s.progress > 0 && s.flyout->isVisible() && s.flyout->geometry().contains(p)) overFlyout = true;
    for (int i = 0; i < edgeCount; i++) {
        EdgeState& s = edges_[size_t(i)];
        const bool zone = inZone(Edge(i), p) && !(overFlyout && !s.target);
        if (zone && !s.inZone && !blocked && !s.target) slide(Edge(i), true);
        s.inZone = zone;
        if (s.target && s.flyout->geometry().contains(p)) s.held = false;   // it has been visited
    }
    evaluate();
}

bool FullScreenMode::keepsOut(Edge edge) const {
    const EdgeState& s = edges_[size_t(edge)];
    if (s.held) return true;
    if (pointerKnown_ && (s.inZone || s.flyout->geometry().adjusted(-2, -2, 2, 2).contains(lastPointer_))) return true;
    // A menu, a context menu, a combo's list or a dialog is open (one from the flyout keeps it until it closes).
    if (QApplication::activePopupWidget() || QApplication::activeModalWidget()) return true;
    if (QWidget* active = QApplication::activeWindow(); active && active != window_) return true;
    // A drag that began in it (a layer dragged onto the canvas), or a field in it with the keys.
    if (pressEdge_ == int(edge) && buttons_ != Qt::NoButton) return true;
    if (QWidget* focus = QApplication::focusWidget(); focus && s.flyout->isAncestorOf(focus) && isTextField(focus)) return true;
    return false;
}

void FullScreenMode::evaluate() {
    if (!active_) return;
    for (int i = 0; i < edgeCount; i++) {
        EdgeState& s = edges_[size_t(i)];
        if (!s.target || s.pinned || keepsOut(Edge(i))) { s.closeTimer->stop(); continue; }
        if (!s.closeTimer->isActive() || s.closeTimer->interval() != closeDelayMs_) s.closeTimer->start(closeDelayMs_);
    }
}

bool FullScreenMode::eventFilter(QObject* watched, QEvent* event) {
    if (watched == window_) {
        if (event->type() == QEvent::Resize) {
            if (active_) relayout();
            else if (reapplyState_) {
                // Back at its size after full screen: the docks as they were.
                window_->restoreState(savedState_);
                for (size_t i = 0; i < window_->tabs_.size(); i++) window_->tabs_[i].options->setVisible(int(i) == window_->current_);
                if (window_->size() == normalSize_) reapplyState_ = false;
            }
        }
        if (!active_) return false;
    }
    if (!active_ || !watched->isWidgetType()) return false;
    auto* target = static_cast<QWidget*>(watched);
    const bool ours = target->window() == window_;
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseMove: {
        if (!ours) break;
        auto* me = static_cast<QMouseEvent*>(event);
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick) {
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
        if (event->type() == QEvent::TabletPress) {
            if (!tabletDown_ && buttons_ == Qt::NoButton) pressEdge_ = edgeOf(target);
            tabletDown_ = pressEdge_ < 0;
        } else if (event->type() == QEvent::TabletRelease) {
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
        QTimer::singleShot(0, this, &FullScreenMode::evaluate);
        break;
    case QEvent::ShortcutOverride: {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() != Qt::Key_Alt) altArmed_ = false;
        // Tab and Shift+Tab move between fields while one has the keys, rather than pinning the edges.
        if ((ke->key() == Qt::Key_Tab || ke->key() == Qt::Key_Backtab) && isTextField(QApplication::focusWidget())) {
            ke->accept();
            return true;
        }
        break;
    }
    case QEvent::KeyPress: {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Alt) { if (!ke->isAutoRepeat()) altArmed_ = (ke->modifiers() & ~Qt::AltModifier) == Qt::NoModifier; }
        else altArmed_ = false;
        break;
    }
    case QEvent::KeyRelease: {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (ke->key() == Qt::Key_Alt && !ke->isAutoRepeat() && altArmed_ && ours) {
            // Alt pressed and let go alone: the menus, as F10.
            altArmed_ = false;
            QTimer::singleShot(0, this, &FullScreenMode::showMenus);
        }
        break;
    }
    case QEvent::WindowDeactivate:
        altArmed_ = false;
        break;
    case QEvent::FocusIn:
        if (ours) QTimer::singleShot(0, this, &FullScreenMode::evaluate);
        break;
    default:
        break;
    }
    return false;
}

} // namespace app
