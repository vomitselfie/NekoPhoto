// The screen modes over the edges (WindowEdges.h): an edge of the standard window autohiding (its part of the
// window claimed into its flyout, and given back), and View > Screen Mode > Full Screen, which claims every edge and
// restores the window exactly on leaving; the options bar and the window's keys while their bars are hidden.
#include "WindowEdges.h"
#include "Flyout.h"
#include "EditorSession.h"
#include "CanvasWidget.h"
#include "EdgePages.h"
#include "HidePanels.h"
#include "MainWindow.h"
#include "ToolOptionsBar.h"
#include <QBoxLayout>
#include <QDockWidget>
#include <QMenuBar>
#include <QSplitter>
#include <QStatusBar>
#include <QVariantAnimation>
#include <QMenu>
#include <QTimer>
#include <QToolBar>
#include <functional>

using namespace nekophoto;

namespace app {

// ---- Claiming an edge's parts ----------------------------------------------------------------------------------

bool WindowEdges::anyClaimed() const {
    for (const EdgeState& s : edges_) if (s.claimed) return true;
    return false;
}

bool WindowEdges::claimsWidget(const QWidget* widget) const {
    if (!widget) return false;
    if (widget == rail_ || std::find(optionBars_.begin(), optionBars_.end(), QPointer<QToolBar>(const_cast<QToolBar*>(qobject_cast<const QToolBar*>(widget)))) != optionBars_.end()) return true;
    for (const Moved& m : moved_) if (m.dock == widget) return true;
    return false;
}

void WindowEdges::claim(Edge edge) {
    EdgeState& s = edges_[size_t(edge)];
    if (s.claimed) return;
    MainWindow* w = window_;
    fillOwnPages();
    s.claimed = true;
    s.flyout->ensurePolished();
    s.deck->setBase(s.flyout->fill().isValid() ? s.flyout->fill() : s.flyout->palette().color(QPalette::Window));
    if (edge == Left) {
        // A rail the person closed stays closed (one Tab hid is the rail still).
        QToolBar* rail = w->toolRail_;
        if (rail && (!rail->isHidden() || (w->hidePanels_ && w->hidePanels_->hidden() == HidePanels::All))) {
            railWasVisible_ = !rail->isHidden();
            w->removeToolBar(rail);
            edges_[Left].pages[size_t(pageIndex(Left, QStringLiteral("tools")))].layout->insertWidget(0, rail, 0, Qt::AlignTop | Qt::AlignHCenter);
            rail->show();
            rail_ = rail;
        }
    } else if (edge == Top) {
        optionBars_.clear();
        shownOptions_ = nullptr;
        for (const auto& tab : w->tabs_) {
            optionBars_.emplace_back(tab.options);
            w->removeToolBar(tab.options);
        }
        placeOptionsBar();
    } else if (edge == Right) {
        // As wide as the panels' column was in the window.
        int width = 0;
        for (QDockWidget* d : w->findChildren<QDockWidget*>(Qt::FindDirectChildrenOnly))
            if (!d->isHidden() && !d->isFloating() && w->dockWidgetArea(d) == Qt::RightDockWidgetArea) width = std::max(width, d->width());
        if (width > 0) s.extent = width;
        else if (s.extent <= 0) s.extent = 300;
    } else if (edge == Bottom) {
        timelineWanted_ = w->timelineDock_ && !w->timelineDock_->isHidden();
        if (timelineWanted_ && w->timelineDock_->height() > 40) s.extent = w->timelineDock_->height();
        else if (s.extent <= 0) s.extent = 220;
        // The status bar goes with the bottom edge in the standard window (full screen hides it anyway).
        if (!active_ && !w->statusBar()->isHidden()) {
            w->statusBar()->hide();
            s.hidStatusBar = true;
        }
    }
    // A guest comes only while its own edge stays in the standard window (the Histogram's, the bottom).
    const bool guests = !active_ && edge == Right && !autohide_[Bottom];
    for (Page& p : s.pages)
        for (const QString& name : guests ? p.docks + p.guests : p.docks) {
            auto* dock = w->findChild<QDockWidget*>(name, Qt::FindDirectChildrenOnly);
            QWidget* content = dock ? dock->widget() : nullptr;
            if (!content) continue;
            moved_.push_back({int(edge), dock, content, !dock->isHidden()});
            if (p.split) p.split->addWidget(content);
            else p.layout->addWidget(content, 1);
            content->show();
            dock->hide();
        }
    if (edge == Bottom) {
        Page& info = s.pages[size_t(pageIndex(Bottom, QStringLiteral("info")))];
        info.split->setStretchFactor(0, 0);
        info.split->setStretchFactor(1, 1);
    }
    if (guests) {
        // Properties: the adjustment's editor has the room, as in Photoshop's Properties panel. The Histogram under it
        // starts small (about its own height, a quarter of the page at most), keeps its height as the edge grows, and
        // can be dragged smaller, to nothing.
        Page& properties = s.pages[size_t(pageIndex(Right, QStringLiteral("properties")))];
        if (properties.split && properties.split->count() == 2) {
            QWidget* histogram = properties.split->widget(1);
            properties.split->setStretchFactor(0, 1);
            properties.split->setStretchFactor(1, 0);
            properties.split->setCollapsible(1, true);
            const int hint = std::max(1, histogram->sizeHint().height());
            properties.split->setSizes({std::max(3 * hint, 600), hint});   // proportions: the page's height is not known yet
        }
    }
    // It opens on the page it was last on, when it has that page now; the left on the brushes while the Brush is the
    // tool, else on the tools (followWindow).
    int index = pageIndex(edge, edges::lastPage(edge));
    if (edge == Left && w->session_) index = pageIndex(Left, w->session_->tool() == Tool::Brush ? QStringLiteral("brushes") : QStringLiteral("tools"));
    if (!pageAvailable(edge, index)) {
        index = 0;
        while (index < int(s.pages.size()) && !pageAvailable(edge, index)) index++;
        if (index >= int(s.pages.size())) index = 0;
    }
    s.current = index;
    s.deck->setCurrent(index);
    refreshStrip(edge);
    lendShortcuts();
    tick_->start();
    relayout();
}

void WindowEdges::unclaim(Edge edge) {
    EdgeState& s = edges_[size_t(edge)];
    if (!s.claimed) return;
    MainWindow* w = window_;
    s.slide->stop();
    s.closeTimer->stop();
    s.dwell->stop();
    s.deck->finishFlip();
    moveFocusOut(edge);
    s.target = s.pinned = s.held = s.inZone = s.peeked = false;
    s.progress = 0;
    s.flyout->hide();
    s.flyout->clearMask();
    s.claimed = false;
    // The panels back in their docks, as they were.
    for (auto it = moved_.begin(); it != moved_.end();) {
        if (it->edge != int(edge)) { ++it; continue; }
        if (it->dock && it->content) {
            it->dock->setWidget(it->content);
            it->content->show();   // a page behind another was hidden with it
            it->dock->setVisible(it->wasVisible);
        }
        it = moved_.erase(it);
    }
    if (edge == Left && rail_) {
        edges_[Left].pages[size_t(pageIndex(Left, QStringLiteral("tools")))].layout->removeWidget(rail_);
        w->addToolBar(Qt::LeftToolBarArea, rail_);
        rail_->setVisible(railWasVisible_);
        rail_ = nullptr;
    }
    if (edge == Top) {
        if (shownOptions_) edges_[Top].pages[size_t(pageIndex(Top, QStringLiteral("options")))].layout->removeWidget(shownOptions_);
        for (const auto& bar : optionBars_)
            if (bar) w->addToolBar(Qt::TopToolBarArea, bar);
        optionBars_.clear();
        shownOptions_ = nullptr;
        const bool hidden = w->hidePanels_ && w->hidePanels_->hidesOptions();
        for (size_t i = 0; i < w->tabs_.size(); i++) w->tabs_[i].options->setVisible(int(i) == w->current_ && !hidden);
    }
    if (edge == Bottom && s.hidStatusBar) {
        w->statusBar()->show();
        s.hidStatusBar = false;
    }
    lendShortcuts();
    if (!anyClaimed()) tick_->stop();
}

void WindowEdges::engageNormal() {
    if (active_ || suspended_ > 0) return;
    for (int i = 0; i < edgeCount; i++) {
        if (autohide_[size_t(i)]) claim(Edge(i));
        else unclaim(Edge(i));
    }
    relayout();
}

void WindowEdges::suspend() {
    if (suspended_++ == 0 && !active_)
        for (int i = 0; i < edgeCount; i++) unclaim(Edge(i));
}

void WindowEdges::resume() {
    if (suspended_ > 0 && --suspended_ == 0) engageNormal();
}

void WindowEdges::applyPreferences() {
    reduceMotion_ = edges::reduceMotion();
    std::array<bool, edgeCount> wanted{};
    for (int i = 0; i < edgeCount; i++) wanted[size_t(i)] = edges::autohide(i);
    if (wanted == autohide_ && !active_) { engageNormal(); return; }
    // Every edge's parts back to the window, then taken again: a guest panel goes to its own edge.
    suspend();
    autohide_ = wanted;
    resume();
}

void WindowEdges::setAutohide(Edge edge, bool on) {
    suspend();
    autohide_[size_t(edge)] = on;
    resume();
}

// ---- Full screen -----------------------------------------------------------------------------------------------

void WindowEdges::enter() {
    if (active_) return;
    MainWindow* w = window_;
    // The autohiding edges give their parts back first: what is kept is the window's own layout.
    suspend();
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
    menuBarShown_ = !w->menuBar()->isHidden();
    statusBarShown_ = !w->statusBar()->isHidden();
    tabBarShown_ = !w->tabBar_->isHidden();
    for (int i = 0; i < edgeCount; i++) claim(Edge(i));
    // The menus' keys work without the menu bar on screen only when the window holds the actions too.
    lendShortcuts();
    w->menuBar()->hide();
    w->statusBar()->hide();
    w->tabBar_->hide();
    w->showFullScreen();
    relayout();
    resume();
    emit activeChanged(true);
}

void WindowEdges::leave() {
    if (!active_) return;
    MainWindow* w = window_;
    active_ = false;
    heldKey_ = 0;
    holdTimer_->stop();
    peeking_ = false;
    if (menuStrip_) menuStrip_->hide();
    for (int i = 0; i < edgeCount; i++) unclaim(Edge(i));
    takeBackShortcuts();
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
    emit normalLayoutRestored();
    // The window comes back to its size a moment later on most desktops: the layout is restored again then.
    reapplyState_ = true;
    QTimer::singleShot(1500, this, [this] { reapplyState_ = false; });
    engageNormal();
    if (w->canvas_) w->canvas_->setFocus();
    emit activeChanged(false);
}

// ---- The options bar, the window's keys ------------------------------------------------------------------------

void WindowEdges::placeOptionsBar() {
    MainWindow* w = window_;
    if (!edges_[Top].claimed || w->current_ < 0 || w->current_ >= int(w->tabs_.size())) return;
    QToolBar* bar = w->tabs_[size_t(w->current_)].options;
    QBoxLayout* page = edges_[Top].pages[size_t(pageIndex(Top, QStringLiteral("options")))].layout;
    if (bar == shownOptions_) { if (bar && bar->isHidden()) bar->show(); return; }
    if (shownOptions_) {
        // Parked on the window, out of sight, until the window takes the bars back.
        page->removeWidget(shownOptions_);
        shownOptions_->hide();
        shownOptions_->setParent(w);
        shownOptions_->hide();
    }
    // A tab opened meanwhile put its bar in the window's layout.
    if (std::find(optionBars_.begin(), optionBars_.end(), QPointer<QToolBar>(bar)) == optionBars_.end()) {
        w->removeToolBar(bar);
        optionBars_.emplace_back(bar);
    }
    page->insertWidget(0, bar);
    bar->show();
    shownOptions_ = bar;
    refreshStrip(Top);
    relayout();
}

void WindowEdges::currentTabChanged() {
    if (edges_[Top].claimed) placeOptionsBar();
    refreshPagesLater();
}

void WindowEdges::takeBackShortcuts() {
    for (QAction* a : addedShortcuts_) window_->removeAction(a);
    addedShortcuts_.clear();
}

void WindowEdges::lendShortcuts() {
    takeBackShortcuts();
    if (!anyClaimed()) return;
    MainWindow* w = window_;
    const QList<QAction*> own = w->actions();
    auto lend = [&](QAction* a) {
        if (a->shortcuts().isEmpty() || own.contains(a) || addedShortcuts_.contains(a)) return;
        w->addAction(a);
        addedShortcuts_ << a;
    };
    std::function<void(QMenu*)> walk = [&](QMenu* menu) {
        for (QAction* a : menu->actions()) {
            if (QMenu* sub = a->menu()) { walk(sub); continue; }
            lend(a);
        }
    };
    if (active_)
        for (QAction* top : w->menuBar()->actions())
            if (QMenu* menu = top->menu()) walk(menu);
    // The tool rail's letters (B, V, M, …) and any toolbar key live on toolbars that may be hidden in their flyouts; a
    // hidden widget's shortcuts don't fire, so they are lent to the window as well.
    for (QToolBar* bar : w->findChildren<QToolBar*>())
        for (QAction* a : bar->actions()) lend(a);
}

} // namespace app
