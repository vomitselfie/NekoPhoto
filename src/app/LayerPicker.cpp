// The Layer Picker (LayerPicker.h): the overlay's column of layers and the controller that steps through them.
#include "LayerPicker.h"
#include "CanvasWidget.h"
#include "CommandRegistry.h"
#include "ImageConvert.h"
#include "MainWindow.h"
#include "Motion.h"
#include "Names.h"
#include <QKeyEvent>
#include <QKeySequence>
#include <QMenu>
#include <QPainter>
#include <algorithm>
#include <cmath>

using namespace nekophoto;

namespace app {

namespace {

QColor withAlpha(QColor c, double alpha) { c.setAlphaF(float(std::clamp(alpha, 0.0, 1.0))); return c; }

/// A layer's thumbnail over a checkerboard, fitted in the row's box (as the Layers panel draws it).
QPixmap thumbnailFor(const Layer& layer, double dpr) {
    constexpr int w = LayerPickerOverlay::thumbWidth, h = LayerPickerOverlay::thumbHeight;
    QPixmap pixmap(int(w * dpr), int(h * dpr));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    for (int y = 0; y < h; y += 5)
        for (int x = 0; x < w; x += 5) p.fillRect(QRect(x, y, 5, 5), ((x / 5 + y / 5) % 2) ? QColor(200, 200, 200) : QColor(255, 255, 255));
    if (layer.asset && layer.asset->thumbnail) {
        const QImage q = wrapImage(*layer.asset->thumbnail);
        const double scale = std::min(w / double(q.width()), h / double(q.height()));
        const QSizeF size(q.width() * scale, q.height() * scale);
        p.drawImage(QRectF((w - size.width()) / 2, (h - size.height()) / 2, size.width(), size.height()), q);
    }
    p.setPen(QColor(0, 0, 0, 90));
    p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(0.5, 0.5, w - 1, h - 1));
    return pixmap;
}

} // namespace

// ---- The overlay --------------------------------------------------------------------------------

LayerPickerOverlay::LayerPickerOverlay(QWidget* parent) : RingOverlay(parent) {
    setObjectName(QStringLiteral("layerPicker"));
    resize(preferredSize());
    bar_ = new motion::Tween(this, [this] { update(); });
    bar_->jump(1);
    scroll_ = new motion::Tween(this, [this] { update(); });
    scroll_->jump(1);
}

int LayerPickerOverlay::activeIndex() const {
    const auto active = std::find_if(entries_.begin(), entries_.end(), [](const Entry& e) { return e.active; });
    return active == entries_.end() ? -1 : int(active - entries_.begin());
}

double LayerPickerOverlay::shownBar() const {
    if (!bar_->running() || barFrom_ < 0 || barTo_ < 0) return barTo_ >= 0 ? barTo_ : barFrom_;
    return motion::lerp(barFrom_, barTo_, motion::easeOut(bar_->value()));
}

double LayerPickerOverlay::shownFirst() const {
    if (!scroll_->running()) return firstTo_;
    return motion::lerp(firstFrom_, firstTo_, motion::easeOut(scroll_->value()));
}

double LayerPickerOverlay::highlightRow() const { return shownBar(); }
bool LayerPickerOverlay::scrolling() const { return scroll_->running(); }

void LayerPickerOverlay::setEntries(std::vector<Entry> entries, const QString& empty) {
    // The same rows with another lit (a step): the bar glides there and the column scrolls after it. Other rows (an
    // undo took one away, it opened again): in place at once.
    bool sameRows = entries.size() == entries_.size();
    for (size_t i = 0; sameRows && i < entries.size(); ++i) sameRows = entries[i].name == entries_[i].name;
    const double barNow = shownBar(), firstNow = shownFirst();
    entries_ = std::move(entries);
    empty_ = empty;
    const int active = activeIndex(), first = firstShown();
    if (sameRows && animates()) {
        if (active != barTo_) {
            barFrom_ = barNow;
            barTo_ = active;
            bar_->run(0, 1, motion::glideMs);
        }
        if (first != firstTo_) {
            firstFrom_ = firstNow;
            firstTo_ = first;
            scroll_->run(0, 1, motion::glideMs);
        }
    } else {
        barFrom_ = barTo_ = active;
        firstFrom_ = firstTo_ = first;
        bar_->jump(1);
        scroll_->jump(1);
    }
    update();
}

void LayerPickerOverlay::settleOn(int index) {
    if (activeIndex() == index) { dismiss(false); return; }
    std::vector<Entry> entries = entries_;
    for (size_t i = 0; i < entries.size(); ++i) entries[i].active = int(i) == index;
    setEntries(std::move(entries), empty_);
    // The bar back in place first, then the fade.
    dismiss(false, bar_->running() || scroll_->running() ? motion::glideMs : 0);
}

void LayerPickerOverlay::finishMotion() {
    RingOverlay::finishMotion();
    bar_->finish();
    scroll_->finish();
}

void LayerPickerOverlay::hidden() {
    bar_->jump(1);
    scroll_->jump(1);
    barFrom_ = barTo_;
    firstFrom_ = firstTo_;
}

int LayerPickerOverlay::shownCount() const { return std::max(1, std::min(int(entries_.size()), maxRows)); }

int LayerPickerOverlay::firstShown() const {
    const int count = int(entries_.size());
    if (count <= maxRows) return 0;
    const int at = std::max(0, activeIndex());
    return std::clamp(at - maxRows / 2, 0, count - maxRows);
}

QSize LayerPickerOverlay::preferredSize() const { return {width_, hintHeight * 2 + shownCount() * rowHeight + 8 + captionHeight}; }

QRect LayerPickerOverlay::entryRect(int index) const {
    const int row = index - firstShown();
    if (index < 0 || index >= int(entries_.size()) || row < 0 || row >= shownCount()) return {};
    return {4, 4 + hintHeight + row * rowHeight, width() - 8, rowHeight};
}

void LayerPickerOverlay::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette pal = palette();
    // The Tool Ring's glass, in the theme's colours.
    const QColor fill = pal.color(QPalette::Window), text = pal.color(QPalette::WindowText);
    const QColor accent = pal.color(QPalette::Highlight), onAccent = pal.color(QPalette::HighlightedText), line = pal.color(QPalette::Link);
    const QRectF panel(0.5, 0.5, width() - 1, hintHeight * 2 + shownCount() * rowHeight + 8 - 1);
    // Popping open as the rings do, about the panel's middle (no overshoot: the panel fills the widget).
    applyPop(p, panel.center(), false);
    p.setPen(QPen(withAlpha(line, 0.7), 1.2));
    p.setBrush(withAlpha(fill, 0.86));
    p.drawRoundedRect(panel, 9, 9);

    QFont small = font();
    small.setPixelSize(10);
    QFont letter = small;
    letter.setBold(true);
    QFont name = font();
    name.setPixelSize(12);

    // The keys' hints: R and F go up the stack (towards the top of the list), S and V down it.
    auto keys = [&](double y, const char* first, const char* second, const QString& arrow) {
        double x = width() / 2.0 - 24;
        p.setFont(small);
        p.setPen(withAlpha(line, 0.95));
        p.drawText(QRectF(x - 26, y - 8, 22, 16), Qt::AlignCenter, arrow);
        for (const char* k : {first, second}) {
            p.setPen(Qt::NoPen);
            p.setBrush(withAlpha(accent, 0.92));
            p.drawEllipse(QPointF(x + 8, y), 8, 8);
            p.setPen(onAccent);
            p.setFont(letter);
            p.drawText(QRectF(x, y - 8, 16, 16), Qt::AlignCenter, QString::fromLatin1(k));
            x += 22;
        }
        p.setFont(small);
        p.setPen(withAlpha(line, 0.95));
        p.drawText(QRectF(x + 2, y - 8, 22, 16), Qt::AlignCenter, arrow);
    };
    keys(4 + hintHeight / 2.0, "R", "F", QStringLiteral("▲"));
    keys(4 + hintHeight + shownCount() * rowHeight + hintHeight / 2.0, "S", "V", QStringLiteral("▼"));

    if (entries_.empty()) {
        p.setFont(name);
        p.setPen(withAlpha(text, 0.8));
        p.drawText(QRectF(4, 4 + hintHeight, width() - 8, rowHeight), Qt::AlignCenter, empty_);
    }
    // The column, scrolled to `top` (a fraction of a row while it scrolls), clipped to its rows.
    const double top = shownFirst();
    const QRectF column(4, 4 + hintHeight, width() - 8, shownCount() * rowHeight);
    auto rowRect = [&](double index) { return QRectF(column.left(), column.top() + (index - top) * rowHeight, column.width(), rowHeight).adjusted(2, 2, -2, -2); };
    p.save();
    p.setClipRect(column.adjusted(0, -1, 0, 1));
    // The lit bar, gliding from row to row (fading in or out when it comes from or goes to no row).
    const double bar = shownBar();
    if (bar >= 0) {
        double opacity = 1;
        if (bar_->running() && barFrom_ < 0) opacity = motion::easeOut(bar_->value());
        else if (bar_->running() && barTo_ < 0) opacity = 1 - motion::easeOut(bar_->value());
        p.save();
        p.setOpacity(p.opacity() * opacity);
        p.setPen(QPen(withAlpha(accent, 0.95), 1.6));
        p.setBrush(withAlpha(accent, 0.38));
        p.drawRoundedRect(rowRect(bar), 6, 6);
        p.restore();
    }
    const int from = std::max(0, int(std::floor(top))), to = std::min(int(entries_.size()), int(std::ceil(top)) + shownCount());
    for (int i = from; i < to; ++i) {
        const Entry& e = entries_[size_t(i)];
        const QRectF row = rowRect(i);
        const QPointF thumbAt(row.left() + 5, row.center().y() - thumbHeight / 2.0);
        p.drawPixmap(thumbAt, e.thumbnail);
        const QRectF words(thumbAt.x() + thumbWidth + 8, row.top() + 2, row.right() - thumbAt.x() - thumbWidth - 12, row.height() - 4);
        p.setFont(name);
        p.setPen(text);
        const QFontMetrics nameMetrics(name), hintMetrics(small);
        p.drawText(QRectF(words.left(), words.top(), words.width(), words.height() * 0.58), Qt::AlignLeft | Qt::AlignBottom,
                   nameMetrics.elidedText(e.name, Qt::ElideRight, int(words.width())));
        p.setFont(small);
        p.setPen(withAlpha(text, 0.72));
        p.drawText(QRectF(words.left(), words.top() + words.height() * 0.6, words.width(), words.height() * 0.4), Qt::AlignLeft | Qt::AlignTop,
                   hintMetrics.elidedText(e.hint, Qt::ElideRight, int(words.width())));
    }
    p.restore();
    // More rows than the column shows: a mark at the end that has more.
    const int first = firstShown();
    if (first > 0 || first + shownCount() < int(entries_.size())) {
        p.setFont(small);
        p.setPen(withAlpha(text, 0.6));
        if (first > 0) p.drawText(QRectF(width() - 30, 4, 24, hintHeight), Qt::AlignCenter, QStringLiteral("…"));
        if (first + shownCount() < int(entries_.size())) p.drawText(QRectF(width() - 30, 4 + hintHeight + shownCount() * rowHeight, 24, hintHeight), Qt::AlignCenter, QStringLiteral("…"));
    }

    // The caption under the panel.
    paintCaption(p, panel.bottom() + 3, width() - 8, captionHeight);
}

// ---- The controller ----------------------------------------------------------------------------

LayerPicker::LayerPicker(MainWindow* window) : RingController(window, new LayerPickerOverlay(window)) {
    pickerOverlay_ = static_cast<LayerPickerOverlay*>(RingController::overlay_);
}

void LayerPicker::registerCommands(CommandRegistry& registry, QMenu* viewMenu) {
    Command c;
    c.id = QStringLiteral("view.layerPicker");
    c.label = tr("La&yer Picker");
    c.shortcuts = {QKeySequence(QStringLiteral("Alt+A"))};
    c.run = [this] { toggle(); };
    c.unavailable = [this] { return currentSession() && currentSession()->hasDocument() ? QString() : tr("No document is open"); };
    registry.add(viewMenu, std::move(c))->setObjectName(QStringLiteral("command.view.layerPicker"));
}

bool LayerPicker::mayOpen() const { return currentSession() && currentSession()->hasDocument(); }

char LayerPicker::keyOf(const QKeyEvent* e) const {
    // Enter keeps the choice, as a click on the canvas does (the keypad's Enter too).
    if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && (e->modifiers() & ~Qt::KeypadModifier) == Qt::NoModifier) return '\n';
    return compassKey(e);
}

QString LayerPicker::busy() const {
    const EditorSession* s = session_;
    if (!s) return {};
    if (s->brushActive() || s->warpActive() || s->pixelMoveActive()) return tr("Not while a stroke is being painted");
    // Choosing another layer would apply these (Photoshop's too): the picker leaves the document alone.
    if (s->transformEdit()) return tr("Apply or cancel the transform first");
    if (s->gradientPending()) return tr("Apply the gradient first");
    if (s->warpCage()) return tr("Apply or cancel the warp first");
    return {};
}

void LayerPicker::opening() {
    EditorSession* s = session_;
    CanvasWidget* view = canvas();
    if (!s || !view) return;
    // The pixel under the pointer; with the pointer elsewhere, the one in the middle of the canvas.
    const QPointF at = openedAt() ? *openedAt() : QPointF(view->width() / 2.0, view->height() / 2.0);
    point_ = view->documentPoint(at);
    layers_ = s->layersAt(point_);
    activeBefore_ = s->activeLayerId();
    selectedBefore_ = s->selectedLayerIds();
    maskBefore_ = s->isMaskSelected();
}

void LayerPicker::closed(bool cancelled) {
    EditorSession* s = session_;
    if (!cancelled || !s || !s->hasDocument()) return;
    // Esc: the layer (or layers, or mask) that was active when it opened.
    if (s->activeLayerId() == activeBefore_ && s->selectedLayerIds() == selectedBefore_ && s->isMaskSelected() == maskBefore_) return;
    if (selectedBefore_.size() > 1) s->selectLayers(selectedBefore_, activeBefore_);
    else s->selectLayer(activeBefore_, maskBefore_);
    // The bar glides back to that layer's row before the picker fades.
    const auto row = activeBefore_ ? std::find(layers_.begin(), layers_.end(), *activeBefore_) : layers_.end();
    pickerOverlay_->settleOn(row == layers_.end() ? -1 : int(row - layers_.begin()));
}

void LayerPicker::refresh() {
    EditorSession* s = session_;
    if (!s || !s->hasDocument()) return;
    const Document& document = *s->document();
    const double dpr = pickerOverlay_->devicePixelRatioF();
    std::vector<LayerPickerOverlay::Entry> entries;
    std::vector<Uuid> still;
    for (const Uuid& id : layers_) {
        const Layer* layer = document.find(id);
        if (!layer) continue;   // deleted (an undo) while open
        still.push_back(id);
        LayerPickerOverlay::Entry e;
        e.thumbnail = thumbnailFor(*layer, dpr);
        e.name = QString::fromStdString(layer->name);
        const QString mode = names::blendMode(layer->blendMode);
        const long opacity = std::lround(layer->opacity * 100);
        const Layer* folder = layer->parentId ? document.find(*layer->parentId) : nullptr;
        e.hint = folder ? tr("%1 · %2% · in “%3”").arg(mode).arg(opacity).arg(QString::fromStdString(folder->name)) : tr("%1 · %2%").arg(mode).arg(opacity);
        e.active = s->activeLayerId() == id;
        entries.push_back(std::move(e));
    }
    layers_ = std::move(still);
    pickerOverlay_->setEntries(std::move(entries), tr("No layer here"));
    const QSize size = pickerOverlay_->preferredSize();
    if (pickerOverlay_->size() != size) {
        pickerOverlay_->resize(size);
        replace();
    }
}

void LayerPicker::press(char key, bool autoRepeat) {
    Q_UNUSED(autoRepeat);   // a held key steps on
    restartIdle();
    switch (key) {
    case '\n': case 'D': close(); return;
    case 'F': case 'R': stepBy(-1); return;
    case 'S': case 'V': stepBy(1); return;
    default: return;   // W E X C: the picker's while it is open, nothing to do
    }
}

void LayerPicker::stepBy(int by) {
    EditorSession* s = session_;
    if (!s || !s->hasDocument()) return;
    if (layers_.empty()) { pickerOverlay_->showCaption(tr("No layer here")); return; }
    if (const QString why = busy(); !why.isEmpty()) { pickerOverlay_->showCaption(why); return; }
    const auto active = s->activeLayerId();
    const auto at = active ? std::find(layers_.begin(), layers_.end(), *active) : layers_.end();
    int next = -1;
    if (at != layers_.end()) {
        next = int(at - layers_.begin()) + by;
        if (next < 0 || next >= int(layers_.size())) {
            pickerOverlay_->showCaption(by < 0 ? tr("The top of the list") : tr("The bottom of the list"));
            return;
        }
    } else {
        // The active layer is not under the pointer: the listed layer nearest it in the stack that way (else the
        // nearest the other way), so any key enters the list.
        const std::vector<HierarchyEntry> order = hierarchyEntries(s->document()->layers, true);
        auto rank = [&](const std::optional<Uuid>& id) {
            for (size_t i = 0; i < order.size(); ++i) if (id && order[i].layer->id == *id) return int(i);
            return -1;
        };
        const int here = rank(active);
        int above = -1, below = -1;
        for (int i = 0; i < int(layers_.size()); ++i) {
            const int r = rank(layers_[size_t(i)]);
            if (here >= 0 && r < here) above = i;                 // listed top first: the last one above is nearest
            if ((here < 0 || r > here) && below < 0) below = i;   // the first one below
        }
        next = by < 0 ? (above >= 0 ? above : below) : (below >= 0 ? below : above);
        if (next < 0) next = by < 0 ? int(layers_.size()) - 1 : 0;
    }
    s->selectLayer(layers_[size_t(next)]);
    refresh();
}

} // namespace app
