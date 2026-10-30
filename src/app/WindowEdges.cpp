// The window's edges, in full screen and autohiding in the standard window: see WindowEdges.h.
#include "WindowEdges.h"
#include "EditorSession.h"
#include "BrushImporter.h"
#include "BrushLibrary.h"
#include "BrushPicker.h"
#include "CanvasWidget.h"
#include "ColorManagement.h"
#include "ColorSwatches.h"
#include "EdgePageWidgets.h"
#include "EdgePages.h"
#include "HidePanels.h"
#include "MainWindow.h"
#include "Motion.h"
#include "Theme.h"
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
#include <QScreen>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QTabletEvent>
#include <QTimer>
#include <QToolBar>
#include <QVariantAnimation>
#include <cmath>
#include <functional>

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

QString modeText(ColorMode mode) {
    switch (mode) {
    case ColorMode::RGB: return WindowEdges::tr("RGB Color");
    case ColorMode::CMYK: return WindowEdges::tr("CMYK Color");
    case ColorMode::Lab: return WindowEdges::tr("Lab Color");
    }
    return {};
}

int bitsOf(SampleType type) { return type == SampleType::U8 ? 8 : type == SampleType::U16 ? 16 : 32; }

} // namespace

// ---- Flyout ----------------------------------------------------------------------------------------------------

Flyout::Flyout(Qt::Edge edge, QWidget* parent) : QWidget(parent), edge_(edge) {
    static const char* names[] = {"flyoutTop", "flyoutLeft", "flyoutRight", "flyoutBottom"};
    const int index = edge == Qt::TopEdge ? 0 : edge == Qt::LeftEdge ? 1 : edge == Qt::RightEdge ? 2 : 3;
    setObjectName(QString::fromLatin1(names[index]));
    body_ = new QBoxLayout(QBoxLayout::TopToBottom, this);
    body_->setSpacing(0);
    // Away from Goth Kitty (whose stylesheet sets its own), a faint shadow and barely rounded corners.
    if (themeSetting() != QLatin1String("gothkitty")) { shadow_ = 6; radius_ = 4; }
    updateMargins();
    hide();
}

void Flyout::setReveal(double reveal) {
    reveal = std::clamp(reveal, 0.0, 1.0);
    if (reveal == reveal_ || (std::abs(reveal - reveal_) < 1e-3 && reveal > 0 && reveal < 1)) return;
    reveal_ = reveal;
    if (shadow_ > 0) update();
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
    // A soft shadow: rings fading outwards, darkest at the card, deepening as the card comes out (a faint one at
    // first, as if it lifted off the canvas).
    const int step = int(std::lround(7 * (0.35 + 0.65 * motion::easeOut(reveal_))));
    for (int i = shadow_; i >= 1 && step > 0; i--) {
        QPainterPath ring;
        ring.addRoundedRect(card.adjusted(-i, -i + 1, i, i + 1), r + i, r + i);
        p.fillPath(ring, QColor(0, 0, 0, step));
    }
    QPainterPath shape;
    shape.addRoundedRect(card, r, r);
    p.fillPath(shape, fill_.isValid() ? fill_ : palette().color(QPalette::Window));
    p.setPen(QPen(border_.isValid() ? border_ : palette().color(QPalette::Mid), 1));
    p.drawPath(shape);
}

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

void WindowEdges::buildPages(Edge edge) {
    EdgeState& s = edges_[size_t(edge)];
    auto add = [&](const char* id, const QString& label, QStringList docks, bool fill, Qt::Orientation split = Qt::Vertical, QStringList guests = {}) {
        Page p;
        p.id = QString::fromLatin1(id);
        p.label = label;
        p.docks = std::move(docks);
        p.guests = std::move(guests);
        p.fill = fill;
        p.box = new QWidget;
        p.box->setObjectName(QStringLiteral("page_") + p.id);
        p.layout = new QVBoxLayout(p.box);
        p.layout->setContentsMargins(0, 0, 0, 0);
        p.layout->setSpacing(0);
        if (p.docks.size() + p.guests.size() > 1 || p.id == QLatin1String("info")) {
            p.split = new QSplitter(split);
            p.split->setChildrenCollapsible(false);
            p.layout->addWidget(p.split, 1);
        }
        s.deck->addPage(p.box);
        s.pages.push_back(p);
    };
    // Photoshop's panels by edge: the tools and what they paint with on the left, the options and the documents at
    // the top, the panels in four pages on the right, the Timeline and the readouts at the bottom.
    switch (edge) {
    case Left:
        add("tools", tr("Tools"), {}, false);
        add("brushes", tr("Brushes"), {}, true);
        add("colour", tr("Colour"), {}, false);
        break;
    case Top:
        add("options", tr("Options"), {}, false);
        add("documents", tr("Documents"), {}, false);
        break;
    case Right:
        add("layers", tr("Layers"), {QStringLiteral("layersDock")}, true);
        // The Histogram is the bottom edge's; while that edge stays in the standard window, it goes with the panels
        // it is a tab of there (under Adjustments).
        add("properties", tr("Properties"), {QStringLiteral("adjustmentsDock")}, true, Qt::Vertical, {QStringLiteral("histogramDock")});
        add("channels", tr("Channels"), {QStringLiteral("channelsDock"), QStringLiteral("pathsDock")}, true);
        add("history", tr("History"), {QStringLiteral("historyDock"), QStringLiteral("actionsDock")}, true);
        break;
    case Bottom:
        add("timeline", tr("Timeline"), {QStringLiteral("timelineDock")}, true);
        add("info", tr("Info"), {QStringLiteral("histogramDock")}, true, Qt::Horizontal);
        break;
    }
}

void WindowEdges::fillOwnPages() {
    if (brushes_) return;
    MainWindow* w = window_;
    // Left: the brush presets (choosing one picks the Brush), and the colours.
    brushes_ = new BrushList;
    brushes_->setMinimumWidth(250);
    Page& brushPage = edges_[Left].pages[size_t(pageIndex(Left, QStringLiteral("brushes")))];
    brushPage.layout->setContentsMargins(6, 2, 6, 6);
    brushPage.layout->addWidget(brushes_, 1);
    connect(brushes_, &BrushList::presetChosen, this, [this](const QString& id) {
        EditorSession* s = window_->session_;
        if (!s) return;
        s->brushPreset = id;
        // A preset starts at its own size; Size then scales it like any brush.
        if (const BrushPreset* preset = BrushLibrary::find(id)) s->brushSettings.diameter = preset->diameter;
        if (s->tool() != Tool::Brush) {
            if (QAction* brush = window_->toolActions_.value(Tool::Brush)) brush->trigger();
            else s->selectTool(Tool::Brush);
        }
        emit s->toolChanged();
    });
    connect(brushes_, &BrushList::importRequested, this, [this] { importBrushesInteractively(window_, window_->session_); brushes_->refresh(); });
    connect(brushes_, &BrushList::dismissed, this, [this] { slide(Left, false); });
    colours_ = new ColourPage;
    colours_->setMinimumWidth(230);
    edges_[Left].pages[size_t(pageIndex(Left, QStringLiteral("colour")))].layout->addWidget(colours_, 1);
    connect(colours_, &ColourPage::foregroundPicked, this, [w](const QColor& c) { w->setColors(c, w->session_->backgroundColor, true, false); });
    connect(colours_, &ColourPage::backgroundPicked, this, [w](const QColor& c) { w->setColors(w->session_->foregroundColor, c, false, true); });
    connect(colours_, &ColourPage::dialogRequested, this, [w](bool background) { w->chooseColor(background); });
    auto run = [w](const char* id) {
        if (const Command* c = w->commandRegistry().find(QString::fromLatin1(id)); c && c->action) c->action->trigger();
    };
    connect(colours_, &ColourPage::swapRequested, this, [run] { run("colors.swap"); });
    connect(colours_, &ColourPage::defaultsRequested, this, [run] { run("colors.default"); });
    // Top: the open documents.
    documents_ = new DocumentsPage;
    edges_[Top].pages[size_t(pageIndex(Top, QStringLiteral("documents")))].layout->addWidget(documents_, 1);
    connect(documents_, &DocumentsPage::documentChosen, this, [w](int index) { if (index >= 0 && index < w->tabCount()) w->selectTab(index); });
    connect(documents_, &DocumentsPage::zoomChosen, this, [w](double zoom) {
        if (!w->session_->hasDocument()) return;
        if (zoom <= 0) w->session_->fitView(); else w->session_->zoomTo(zoom);
    });
    // Bottom: the readouts beside the Histogram.
    info_ = new InfoPanel;
    Page& info = edges_[Bottom].pages[size_t(pageIndex(Bottom, QStringLiteral("info")))];
    info.split->insertWidget(0, info_);
    for (EdgeState& s : edges_) s.deck->setCurrent(s.current);
}

int WindowEdges::pageIndex(Edge edge, const QString& id) const {
    const auto& pages = edges_[size_t(edge)].pages;
    for (size_t i = 0; i < pages.size(); i++) if (pages[i].id == id) return int(i);
    return -1;
}

bool WindowEdges::pageAvailable(Edge edge, int index) const {
    const EdgeState& s = edges_[size_t(edge)];
    if (!s.claimed || index < 0 || index >= int(s.pages.size())) return false;
    const Page& p = s.pages[size_t(index)];
    if (p.id == QLatin1String("tools")) return rail_ != nullptr;
    if (p.id == QLatin1String("options")) return shownOptions_ != nullptr;
    if (p.id == QLatin1String("timeline")) {
        // Only while the document has frames, or the Timeline was asked for (Window ▸ Timeline).
        const EditorSession* session = window_->session_;
        const bool frames = session && session->hasDocument() && !session->document()->animation.empty();
        if (!frames && !timelineWanted_) return false;
    }
    if (p.docks.isEmpty()) return true;
    for (const Moved& m : moved_)
        if (m.edge == int(edge) && m.dock && (p.docks.contains(m.dock->objectName()) || p.guests.contains(m.dock->objectName()))) return true;
    return false;
}

QStringList WindowEdges::pages(Edge edge) const {
    QStringList out;
    const EdgeState& s = edges_[size_t(edge)];
    for (int i = 0; i < int(s.pages.size()); i++) if (pageAvailable(edge, i)) out << s.pages[size_t(i)].id;
    return out;
}

QStringList WindowEdges::allPages(Edge edge) const {
    QStringList out;
    for (const Page& p : edges_[size_t(edge)].pages) out << p.id;
    return out;
}

QString WindowEdges::pageLabel(Edge edge, const QString& id) const {
    const int i = pageIndex(edge, id);
    return i < 0 ? QString() : edges_[size_t(edge)].pages[size_t(i)].label;
}

QString WindowEdges::page(Edge edge) const {
    const EdgeState& s = edges_[size_t(edge)];
    return s.current >= 0 && s.current < int(s.pages.size()) ? s.pages[size_t(s.current)].id : QString();
}

bool WindowEdges::hasContent(Edge edge) const {
    const EdgeState& s = edges_[size_t(edge)];
    if (!s.claimed) return false;
    for (int i = 0; i < int(s.pages.size()); i++) if (pageAvailable(edge, i)) return true;
    return false;
}

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

int WindowEdges::edgeHolding(QDockWidget* dock) const {
    for (const Moved& m : moved_) if (m.dock == dock) return m.edge;
    return -1;
}

QString WindowEdges::pageHolding(QDockWidget* dock) const {
    const int edge = edgeHolding(dock);
    if (edge < 0) return {};
    for (const Page& p : edges_[size_t(edge)].pages)
        if (p.docks.contains(dock->objectName()) || p.guests.contains(dock->objectName())) return p.id;
    return {};
}

QRect WindowEdges::area() const {
    if (active_) return window_->rect();
    const QWidget* canvas = window_->canvasStack_;
    return QRect(canvas->mapTo(window_, QPoint(0, 0)), canvas->size());
}

// ---- Claiming an edge's parts ----------------------------------------------------------------------------------

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

// ---- Pages -----------------------------------------------------------------------------------------------------

void WindowEdges::refreshStrip(Edge edge) {
    EdgeState& s = edges_[size_t(edge)];
    if (!s.claimed) return;
    if (!pageAvailable(edge, s.current)) {
        // Its page went (the Timeline's frames): the first one it has.
        for (int i = 0; i < int(s.pages.size()); i++)
            if (pageAvailable(edge, i)) { s.current = i; s.deck->setCurrent(i); relayout(); break; }
    }
    QStringList labels;
    int at = 0;
    for (int i = 0; i < int(s.pages.size()); i++) {
        if (!pageAvailable(edge, i)) continue;
        if (i == s.current) at = int(labels.size());
        labels << s.pages[size_t(i)].label;
    }
    s.strip->setPages(labels, at);
    s.strip->setVisible(!labels.isEmpty());
}

void WindowEdges::showPageIndex(Edge edge, int index, bool forwards, bool remember) {
    EdgeState& s = edges_[size_t(edge)];
    if (!pageAvailable(edge, index)) return;
    if (remember) edges::setLastPage(edge, s.pages[size_t(index)].id);
    if (index == s.current) { refreshStrip(edge); return; }
    s.current = index;
    refreshPage(edge);   // before its picture is taken
    const bool animate = s.flyout->isVisible() && s.progress > 0;
    if (animate) s.deck->flip(index, forwards, [this] { relayout(); });
    else {
        s.deck->setCurrent(index);
        relayout();
    }
    refreshStrip(edge);
    if (animate) s.strip->flash();
    emit pageChanged(edge, s.pages[size_t(index)].id);
}

bool WindowEdges::flip(Edge edge, int step) {
    const EdgeState& s = edges_[size_t(edge)];
    std::vector<int> shown;
    for (int i = 0; i < int(s.pages.size()); i++) if (pageAvailable(edge, i)) shown.push_back(i);
    if (shown.size() < 2 || step == 0) return false;
    const auto at = std::find(shown.begin(), shown.end(), s.current);
    const int n = int(shown.size()), from = at == shown.end() ? 0 : int(at - shown.begin());
    const int to = ((from + step) % n + n) % n;
    showPageIndex(edge, shown[size_t(to)], step > 0, true);
    return true;
}

bool WindowEdges::showPage(Edge edge, const QString& id, bool remember) {
    const int index = pageIndex(edge, id);
    if (!pageAvailable(edge, index)) return false;
    const int from = edges_[size_t(edge)].current;
    showPageIndex(edge, index, index > from, remember);
    return true;
}

int WindowEdges::flipTarget() const {
    int best = -1;
    quint64 latest = 0;
    for (int i = 0; i < edgeCount; i++) {
        const EdgeState& s = edges_[size_t(i)];
        if (!s.claimed || !s.target || s.pinned) continue;
        if (pointerKnown_ && s.flyout->geometry().contains(lastPointer_)) return i;
        if (best < 0 || s.opened >= latest) { latest = s.opened; best = i; }
    }
    return best;
}

void WindowEdges::refreshPagesLater() {
    if (!refreshTimer_->isActive()) refreshTimer_->start();
}

void WindowEdges::refreshPage(Edge edge) {
    const EdgeState& s = edges_[size_t(edge)];
    if (!s.claimed || !brushes_) return;
    MainWindow* w = window_;
    EditorSession* session = w->session_;
    if (!session) return;
    const QString id = page(edge);
    if (id == QLatin1String("brushes")) brushes_->setPreset(session->brushPreset);
    else if (id == QLatin1String("colour")) {
        colours_->setColors(w->swatches_->foreground(), w->swatches_->background());
        colours_->setRecent(edges::recentColours());
    } else if (id == QLatin1String("documents")) {
        std::vector<DocumentsPage::Document> list;
        for (int i = 0; i < w->tabCount(); i++) {
            EditorSession* tab = w->sessionAt(i);
            DocumentsPage::Document d;
            d.title = w->tabTitle(i);
            d.current = i == w->currentTabIndex();
            if (tab->hasDocument()) {
                // A small picture of each, made again only after its document changed.
                auto cached = std::find_if(thumbnails_.begin(), thumbnails_.end(), [tab](const auto& t) { return t.first == tab; });
                if (cached == thumbnails_.end() || staleThumbnails_.contains(tab)) {
                    QImage picture;
                    if (auto flat = tab->flattened()) picture = QImage(flat->data(), flat->width(), flat->height(), flat->stride(), QImage::Format_RGBA8888_Premultiplied).scaled(128, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                    if (cached == thumbnails_.end()) thumbnails_.emplace_back(tab, picture);
                    else cached->second = picture;
                    staleThumbnails_.remove(tab);
                    cached = std::find_if(thumbnails_.begin(), thumbnails_.end(), [tab](const auto& t) { return t.first == tab; });
                }
                d.thumbnail = cached->second;
            }
            list.push_back(d);
        }
        thumbnails_.erase(std::remove_if(thumbnails_.begin(), thumbnails_.end(), [](const auto& t) { return !t.first; }), thumbnails_.end());
        documents_->setDocuments(list);
        QString description;
        if (session->hasDocument()) {
            const Document& doc = *session->document();
            description = tr("%1 × %2 px · %3 · %4 bits").arg(doc.width).arg(doc.height).arg(modeText(doc.colorMode)).arg(bitsOf(doc.sampleType));
        }
        documents_->setCurrent(session->viewport.zoom, description);
    } else if (id == QLatin1String("info")) {
        if (session->hasDocument()) {
            const Document& doc = *session->document();
            info_->setDocument(tr("%1 × %2 px").arg(doc.width).arg(doc.height), tr("%1, %2 bits/channel").arg(modeText(doc.colorMode)).arg(bitsOf(doc.sampleType)),
                               QStringLiteral("%1%").arg(std::lround(session->viewport.zoom * 100)));
        } else {
            info_->setDocument(QStringLiteral("–"), QStringLiteral("–"), QStringLiteral("–"));
        }
    }
}

void WindowEdges::followWindow(const QString& kind) {
    EditorSession* session = window_->session_;
    if (!session) return;
    if (kind == QLatin1String("document") || kind == QLatin1String("layers")) staleThumbnails_.insert(session);
    if (kind == QLatin1String("tool")) {
        // The left edge opens on the brushes while the Brush (or the Eraser) is the tool, else on the tools; only when
        // the tool changes between them, and not while the edge is out.
        const bool painting = session->tool() == Tool::Brush;
        if (!painting_ || *painting_ != painting) {
            painting_ = painting;
            if (edges_[Left].claimed && !edges_[Left].target) showPage(Left, painting ? QStringLiteral("brushes") : QStringLiteral("tools"), false);
        }
    }
    if (kind == QLatin1String("layers") || kind == QLatin1String("tabs")) {
        // A new adjustment layer: the right edge's Properties, where it is set (Photoshop opens its Properties panel).
        QSet<QString> ids;
        const Layer* added = nullptr;
        if (session->hasDocument())
            for (const Layer& l : session->document()->layers) ids.insert(QString::fromStdString(l.id));
        if (kind == QLatin1String("layers") && layersOf_ == session) {
            const Layer* active = session->activeLayer();
            if (active && active->adjustment && !knownLayers_.contains(QString::fromStdString(active->id))) added = active;
        }
        layersOf_ = session;
        knownLayers_ = ids;
        if (added && edges_[Right].claimed) showPage(Right, QStringLiteral("properties"), false);
    }
    refreshPagesLater();
}

void WindowEdges::coloursChanged() {
    const QColor fg = window_->session_ ? window_->session_->foregroundColor : QColor();
    if (fg.isValid() && fg != lastForeground_) {
        if (lastForeground_.isValid()) edges::noteRecentColour(fg);
        lastForeground_ = fg;
    }
    if (edges_[Left].claimed && page(Left) == QLatin1String("colour") && edges_[Left].progress > 0) refreshPage(Left);
}

void WindowEdges::pointerMoved(QPointF documentPoint) {
    if (info_ && edges_[Bottom].claimed && edges_[Bottom].progress > 0 && page(Bottom) == QLatin1String("info")) info_->setPointer(documentPoint);
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
