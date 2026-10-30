// The edges' pages (WindowEdges.h, EdgePages.h): what each edge holds, which page is on screen, the flips, and
// keeping the edges' own pages (the brushes, the colours, the documents, Info) in step with the window.
#include "WindowEdges.h"
#include "Flyout.h"
#include "EditorSession.h"
#include "BrushImporter.h"
#include "BrushLibrary.h"
#include "BrushPicker.h"
#include "ColorSwatches.h"
#include "EdgePageWidgets.h"
#include "EdgePages.h"
#include "MainWindow.h"
#include <QBoxLayout>
#include <QDockWidget>
#include <QSplitter>
#include <cmath>

using namespace nekophoto;

namespace app {

namespace {

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

// ---- Building and finding pages ------------------------------------------------------------------------------

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

} // namespace app
