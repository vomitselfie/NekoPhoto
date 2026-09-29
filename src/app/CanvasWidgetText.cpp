// Typing on the canvas with the Type tool, as in Photoshop: a click on text puts the caret in it, a click on the
// canvas starts point text and a drag a paragraph box. Typing, the arrows, Home and End, Shift to select, a drag
// to select, Ctrl+A, copy and paste, Enter for a new line; the options bar styles the selected letters. Ctrl+Enter
// (or Enter on the keypad, or a click outside) commits, Esc cancels: one undo step for the whole session
// (EditorSessionTextEdit.cpp). The caret and selection live here; the pixels are the text layer's raster, redrawn
// on every change.
#include "CanvasWidget.h"
#include "QtGeometry.h"
#include "TextLayer.h"
#include <QApplication>
#include <QClipboard>
#include <QElapsedTimer>
#include <QFontMetricsF>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QTextBoundaryFinder>
#include <algorithm>
#include <cmath>

using namespace compositor;

namespace app {

namespace {

QString qText(const LayerText& t) { return QString::fromStdString(t.text); }

/// The next or previous grapheme boundary (a surrogate pair or a combining mark stays whole).
int graphemeStep(const QString& s, int at, int direction) {
    QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, s);
    finder.setPosition(std::clamp(at, 0, int(s.size())));
    const int next = direction > 0 ? finder.toNextBoundary() : finder.toPreviousBoundary();
    return next < 0 ? (direction > 0 ? int(s.size()) : 0) : next;
}

/// Ctrl+Left and Ctrl+Right: the start of the previous or next word.
int wordStep(const QString& s, int at, int direction) {
    QTextBoundaryFinder finder(QTextBoundaryFinder::Word, s);
    finder.setPosition(std::clamp(at, 0, int(s.size())));
    for (;;) {
        const int next = direction > 0 ? finder.toNextBoundary() : finder.toPreviousBoundary();
        if (next < 0) return direction > 0 ? int(s.size()) : 0;
        if (finder.boundaryReasons() & QTextBoundaryFinder::StartOfItem) return next;
        if (next == 0 || next == int(s.size())) return next;
    }
}

/// The word around `at`, for a double-click.
std::pair<int, int> wordAround(const QString& s, int at) {
    QTextBoundaryFinder finder(QTextBoundaryFinder::Word, s);
    finder.setPosition(std::clamp(at, 0, int(s.size())));
    int from = finder.isAtBoundary() && (finder.boundaryReasons() & QTextBoundaryFinder::StartOfItem) ? at : finder.toPreviousBoundary();
    finder.setPosition(std::clamp(at, 0, int(s.size())));
    int to = finder.toNextBoundary();
    return {std::max(0, from), to < 0 ? int(s.size()) : to};
}

TextRunPatch patchBetween(const LayerText& before, const LayerText& after) {
    TextRunPatch patch;
    if (after.fontFamily != before.fontFamily) patch.fontFamily = after.fontFamily;
    if (after.fontSize != before.fontSize) patch.fontSize = after.fontSize;
    if (after.bold != before.bold) patch.bold = after.bold;
    if (after.italic != before.italic) patch.italic = after.italic;
    if (after.red != before.red || after.green != before.green || after.blue != before.blue) patch.color = std::array<double, 3>{after.red, after.green, after.blue};
    return patch;
}

bool patchEmpty(const TextRunPatch& p) {
    return !p.fontFamily && !p.fontSize && !p.bold && !p.italic && !p.weight && !p.color && !p.letterSpacing && !p.baselineShift
        && !p.leading && !p.caps && !p.underline && !p.strikethrough;
}

} // namespace

TextRunPatch CanvasWidget::typePatch(const LayerText& before, const LayerText& after) { return patchBetween(before, after); }

const Layer* CanvasWidget::typeLayer() const {
    if (!typeEdit_ || !session_->hasDocument()) return nullptr;
    const Layer* layer = session_->document()->find(typeEdit_->layer);
    return layer && layer->text && layer->asset && layer->asset->image ? layer : nullptr;
}

void CanvasWidget::refreshTypeGeometry() {
    const Layer* layer = typeLayer();
    if (!layer) return;
    typeEdit_->geometry = textCaretGeometry(*layer->text);
    typeEdit_->toDocument = layer->transform.pixelToDocument(layer->asset->image.width(), layer->asset->image.height());
    typeEdit_->rasterSize = QSizeF(layer->asset->image.width(), layer->asset->image.height());
    const int length = int(qText(*layer->text).size());
    typeEdit_->caret = std::clamp(typeEdit_->caret, 0, length);
    typeEdit_->anchor = std::clamp(typeEdit_->anchor, 0, length);
}

QPointF CanvasWidget::typeRaster(QPointF doc) const {
    const Point p = typeEdit_->toDocument.inverted().apply(Point(doc.x(), doc.y()));
    return {p.x, p.y};
}

QPointF CanvasWidget::typeView(QPointF raster) const {
    const Point p = typeEdit_->toDocument.apply(Point(raster.x(), raster.y()));
    return viewPoint(QPointF(p.x, p.y));
}

bool CanvasWidget::typeBoxed() const {
    const Layer* layer = typeLayer();
    return layer && layer->text->boxWidth > 0 && layer->text->boxHeight > 0;
}

void CanvasWidget::startTypeSession() {
    typeEdit_->caretOn = true;
    caretBlink_.start();
    setAttribute(Qt::WA_InputMethodEnabled, true);
    refreshTypeGeometry();
    if (auto* im = QGuiApplication::inputMethod()) im->update(Qt::ImEnabled | Qt::ImCursorRectangle);
    update();
    emit typeEditChanged();
}

bool CanvasWidget::startTypeOn(const Uuid& id, std::optional<QPointF> doc) {
    if (!session_->beginTypeEdit(id)) return false;
    typeEdit_ = TypeState{};
    typeEdit_->layer = id;
    refreshTypeGeometry();
    const int length = int(qText(*typeLayer()->text).size());
    typeEdit_->caret = typeEdit_->anchor = doc ? typeEdit_->geometry.positionAt(typeRaster(*doc)) : length;
    startTypeSession();
    return true;
}

bool CanvasWidget::startNewType(QPointF doc, std::optional<QRectF> box) {
    LayerText style = session_->textStyle;
    style.red = session_->foregroundColor.redF(); style.green = session_->foregroundColor.greenF(); style.blue = session_->foregroundColor.blueF();
    style.boxWidth = style.boxHeight = 0;
    style.warp = {};
    const std::optional<Uuid> id = session_->beginNewTypeEdit(box ? box->topLeft() : doc, style, box ? std::optional(box->size()) : std::nullopt);
    if (!id) return false;
    typeEdit_ = TypeState{};
    typeEdit_->layer = *id;
    startTypeSession();
    return true;
}

void CanvasWidget::finishTypeSession(bool keep) {
    if (!typeEdit_) return;
    typeEdit_.reset();
    caretBlink_.stop();
    setAttribute(Qt::WA_InputMethodEnabled, false);
    session_->endTypeEdit(keep);
    update();
    emit typeEditChanged();
}

void CanvasWidget::commitType() { finishTypeSession(true); }
void CanvasWidget::cancelType() { finishTypeSession(false); }

void CanvasWidget::setTypeText(LayerText text, int caret, int anchor, QPointF rasterShift, bool record) {
    const Layer* layer = typeLayer();
    if (!layer) return;
    if (record) {
        typeEdit_->undo.push_back({*layer->text, typeEdit_->caret, typeEdit_->anchor});
        if (typeEdit_->undo.size() > 200) typeEdit_->undo.erase(typeEdit_->undo.begin());
        typeEdit_->redo.clear();
    }
    QElapsedTimer timer;
    timer.start();
    session_->setTypeEditText(text, rasterShift);
    typeEdit_->caret = caret;
    typeEdit_->anchor = anchor;
    typeEdit_->caretOn = true;
    caretBlink_.start();
    refreshTypeGeometry();
    lastTypeMs_ = timer.nsecsElapsed() / 1e6;
    if (auto* im = QGuiApplication::inputMethod()) im->update(Qt::ImCursorRectangle);
    update();
    emit typeEditChanged();
}

void CanvasWidget::typeReplace(int from, int to, const QString& insert) {
    const Layer* layer = typeLayer();
    if (!layer) return;
    if (from > to) std::swap(from, to);
    LayerText text = *layer->text;
    const QString before = qText(text);
    from = std::clamp(from, 0, int(before.size()));
    to = std::clamp(to, 0, int(before.size()));
    const QString after = before.left(from) + insert + before.mid(to);
    // The letters typed take the style of the run they start in (or the options bar's, when it was changed with
    // nothing selected).
    if (!text.runs.empty()) text.runs = adjustTextRuns(text.runs, text.text, after.toStdString());
    text.text = after.toStdString();
    if (!insert.isEmpty() && typeEdit_->pending && !patchEmpty(*typeEdit_->pending)) styleTextRange(text, from, int(insert.size()), *typeEdit_->pending);
    else if (!text.runs.empty()) settleTextRuns(text);
    const int caret = from + int(insert.size());
    setTypeText(text, caret, caret);
}

void CanvasWidget::applyTypeStyle(const TextRunPatch& patch, std::optional<int> alignment) {
    const Layer* layer = typeLayer();
    if (!layer) return;
    LayerText text = *layer->text;
    if (alignment) text.alignment = *alignment;
    const int from = std::min(typeEdit_->caret, typeEdit_->anchor), to = std::max(typeEdit_->caret, typeEdit_->anchor);
    if (!patchEmpty(patch)) {
        if (from == to && !qText(text).isEmpty()) {
            // Nothing selected: the style is for the letters typed next, as in Photoshop.
            TextRunPatch merged = typeEdit_->pending.value_or(TextRunPatch{});
            if (patch.fontFamily) merged.fontFamily = patch.fontFamily;
            if (patch.fontSize) merged.fontSize = patch.fontSize;
            if (patch.bold) merged.bold = patch.bold;
            if (patch.italic) merged.italic = patch.italic;
            if (patch.color) merged.color = patch.color;
            typeEdit_->pending = merged;
        } else if (qText(text).isEmpty()) {
            // Empty text: the style is the layer's.
            TextRun run = baseTextRun(text);
            patch.applyTo(run);
            text.fontFamily = run.fontFamily; text.fontSize = run.fontSize; text.bold = run.bold; text.italic = run.italic;
            text.red = run.red; text.green = run.green; text.blue = run.blue;
        } else styleTextRange(text, from, to - from, patch);
    }
    if (text == *layer->text) return;
    setTypeText(text, typeEdit_->caret, typeEdit_->anchor);
}

std::optional<TextRun> CanvasWidget::typeStyleAtCaret() const {
    const Layer* layer = typeLayer();
    if (!layer) return std::nullopt;
    const int from = std::min(typeEdit_->caret, typeEdit_->anchor), to = std::max(typeEdit_->caret, typeEdit_->anchor);
    const std::vector<TextRun> runs = textRuns(*layer->text);
    TextRun found = runs.empty() ? baseTextRun(*layer->text) : runs.front();
    // The first selected letter's run, or with only a caret the letter before it (after it at the start), as
    // Photoshop reads the style.
    const int letter = from != to ? from : std::max(0, from - 1);
    int start = 0;
    for (const TextRun& run : runs) {
        if (letter >= start && letter < start + run.length) { found = run; break; }
        start += run.length;
    }
    if (typeEdit_->pending && typeEdit_->caret == typeEdit_->anchor) typeEdit_->pending->applyTo(found);
    return found;
}

std::optional<int> CanvasWidget::typeAlignment() const {
    const Layer* layer = typeLayer();
    return layer ? std::optional(layer->text->alignment) : std::nullopt;
}

bool CanvasWidget::typeUndo(bool redo) {
    auto& from = redo ? typeEdit_->redo : typeEdit_->undo;
    auto& to = redo ? typeEdit_->undo : typeEdit_->redo;
    if (from.empty() || !typeLayer()) return true;
    to.push_back({*typeLayer()->text, typeEdit_->caret, typeEdit_->anchor});
    const TypeState::Snapshot s = from.back();
    from.pop_back();
    setTypeText(s.text, s.caret, s.anchor, {}, false);
    return true;
}

bool CanvasWidget::typeShortcut(QKeyEvent* e) const {
    // Keys typing takes before the menus' shortcuts: every key without Ctrl, and the editing ones with it.
    if (!(e->modifiers() & (Qt::ControlModifier | Qt::MetaModifier))) return true;
    switch (e->key()) {
    case Qt::Key_A: case Qt::Key_C: case Qt::Key_V: case Qt::Key_X: case Qt::Key_Z: case Qt::Key_Y:
    case Qt::Key_Return: case Qt::Key_Enter: case Qt::Key_Left: case Qt::Key_Right: case Qt::Key_Up: case Qt::Key_Down:
    case Qt::Key_Home: case Qt::Key_End: case Qt::Key_Backspace: case Qt::Key_Delete:
        return true;
    default: return false;
    }
}

bool CanvasWidget::typeKey(QKeyEvent* e) {
    const Layer* layer = typeLayer();
    if (!layer) return false;
    const QString s = qText(*layer->text);
    const bool shift = e->modifiers() & Qt::ShiftModifier, ctrl = e->modifiers() & Qt::ControlModifier;
    int& caret = typeEdit_->caret;
    int& anchor = typeEdit_->anchor;
    const int from = std::min(caret, anchor), to = std::max(caret, anchor);
    auto moveTo = [&](int position, bool keepX = false) {
        if (!keepX) typeEdit_->preferredX = -1;
        caret = std::clamp(position, 0, int(s.size()));
        if (!shift) anchor = caret;
        typeEdit_->pending.reset();
        typeEdit_->caretOn = true;
        caretBlink_.start();
        if (auto* im = QGuiApplication::inputMethod()) im->update(Qt::ImCursorRectangle);
        update();
        emit typeEditChanged();
        return true;
    };
    switch (e->key()) {
    case Qt::Key_Escape: cancelType(); return true;
    case Qt::Key_Enter:
        commitType();   // Enter on the keypad commits, as in Photoshop
        return true;
    case Qt::Key_Return:
        if (ctrl) { commitType(); return true; }
        typeReplace(from, to, QStringLiteral("\n"));
        return true;
    case Qt::Key_Left:
        if (!shift && from != to) return moveTo(from);
        return moveTo(ctrl ? wordStep(s, caret, -1) : graphemeStep(s, caret, -1));
    case Qt::Key_Right:
        if (!shift && from != to) return moveTo(to);
        return moveTo(ctrl ? wordStep(s, caret, 1) : graphemeStep(s, caret, 1));
    case Qt::Key_Up: case Qt::Key_Down: {
        const auto& g = typeEdit_->geometry;
        if (typeEdit_->preferredX < 0) typeEdit_->preferredX = g.caretX[size_t(std::clamp(caret, 0, int(g.caretX.size()) - 1))];
        return moveTo(g.verticalMove(caret, e->key() == Qt::Key_Up ? -1 : 1, typeEdit_->preferredX), true);
    }
    case Qt::Key_Home: {
        if (ctrl) return moveTo(0);
        const auto& g = typeEdit_->geometry;
        return moveTo(g.lines[size_t(g.lineOf[size_t(std::clamp(caret, 0, int(g.lineOf.size()) - 1))])].start);
    }
    case Qt::Key_End: {
        if (ctrl) return moveTo(int(s.size()));
        const auto& g = typeEdit_->geometry;
        return moveTo(g.lines[size_t(g.lineOf[size_t(std::clamp(caret, 0, int(g.lineOf.size()) - 1))])].end);
    }
    case Qt::Key_Backspace:
        if (from != to) typeReplace(from, to, {});
        else if (caret > 0) typeReplace(ctrl ? wordStep(s, caret, -1) : graphemeStep(s, caret, -1), caret, {});
        return true;
    case Qt::Key_Delete:
        if (from != to) typeReplace(from, to, {});
        else if (caret < int(s.size())) typeReplace(caret, ctrl ? wordStep(s, caret, 1) : graphemeStep(s, caret, 1), {});
        return true;
    default: break;
    }
    if (ctrl) {
        switch (e->key()) {
        case Qt::Key_A: anchor = 0; caret = int(s.size()); update(); emit typeEditChanged(); return true;
        case Qt::Key_C: case Qt::Key_X:
            if (from != to) {
                QApplication::clipboard()->setText(s.mid(from, to - from));
                if (e->key() == Qt::Key_X) typeReplace(from, to, {});
            }
            return true;
        case Qt::Key_V: {
            QString pasted = QApplication::clipboard()->text();
            pasted.replace(QStringLiteral("\r\n"), QStringLiteral("\n")).replace('\r', '\n');
            if (!pasted.isEmpty()) typeReplace(from, to, pasted);
            return true;
        }
        case Qt::Key_Z: return typeUndo(shift);
        case Qt::Key_Y: return typeUndo(true);
        default: return false;
        }
    }
    const QString typed = e->text();
    if (!typed.isEmpty() && (typed.at(0).isPrint() || typed == QStringLiteral("\t"))) {
        typeReplace(from, to, typed);
        return true;
    }
    return false;
}

void CanvasWidget::inputMethodEvent(QInputMethodEvent* e) {
    if (!typeEdit_ || !typeLayer()) { QWidget::inputMethodEvent(e); return; }
    // An input method's committed text (Japanese and the like) goes in as typed; the preedit shows under the
    // caret until it is committed.
    typeEdit_->preedit = e->preeditString();
    if (!e->commitString().isEmpty()) {
        const int from = std::min(typeEdit_->caret, typeEdit_->anchor), to = std::max(typeEdit_->caret, typeEdit_->anchor);
        typeReplace(from, to, e->commitString());
    }
    update();
    e->accept();
}

QVariant CanvasWidget::inputMethodQuery(Qt::InputMethodQuery query) const {
    if (!typeEdit_ || !typeLayer()) return QWidget::inputMethodQuery(query);
    switch (query) {
    case Qt::ImEnabled: return true;
    case Qt::ImCursorRectangle: {
        const auto [top, bottom] = typeEdit_->geometry.caret(typeEdit_->caret);
        return QRectF(typeView(top), typeView(bottom)).normalized().adjusted(-1, 0, 1, 0);
    }
    case Qt::ImCursorPosition: return typeEdit_->caret;
    case Qt::ImAnchorPosition: return typeEdit_->anchor;
    case Qt::ImSurroundingText: return qText(*typeLayer()->text);
    case Qt::ImCurrentSelection: {
        const int from = std::min(typeEdit_->caret, typeEdit_->anchor), to = std::max(typeEdit_->caret, typeEdit_->anchor);
        return qText(*typeLayer()->text).mid(from, to - from);
    }
    default: return QWidget::inputMethodQuery(query);
    }
}

int CanvasWidget::typeBoxHandle(QPointF view) const {
    // Paragraph text: the box's corners and edge midpoints resize it (edges: 1 left, 2 top, 4 right, 8 bottom).
    if (!typeBoxed()) return 0;
    const Layer* layer = typeLayer();
    const double w = layer->text->boxWidth, h = layer->text->boxHeight;
    const QPointF p0(textPadding, textPadding);
    static const int edges[8] = {1 | 2, 2, 4 | 2, 4, 4 | 8, 8, 1 | 8, 1};
    static const double ux[8] = {0, 0.5, 1, 1, 1, 0.5, 0, 0}, uy[8] = {0, 0, 0, 0.5, 1, 1, 1, 0.5};
    for (int i = 0; i < 8; i++) {
        const QPointF v = typeView(p0 + QPointF(ux[i] * w, uy[i] * h));
        if (std::hypot(v.x() - view.x(), v.y() - view.y()) <= handleRadius + 3) return edges[i];
    }
    return 0;
}

bool CanvasWidget::typeContains(QPointF doc) const {
    const QPointF r = typeRaster(doc);
    const double slack = 6 / std::max(0.01, session_->viewport.pointsPerPixel());
    return QRectF(QPointF(0, 0), typeEdit_->rasterSize).adjusted(-slack, -slack, slack, slack).contains(r);
}

void CanvasWidget::typePress(QPointF view, QPointF doc, Qt::KeyboardModifiers modifiers) {
    if (typeEdit_ && typeLayer()) {
        if (const int edges = typeBoxHandle(view)) {
            typeEdit_->drag = TypeState::Drag::Box;
            typeEdit_->boxEdges = edges;
            typeEdit_->boxStart = *typeLayer()->text;
            typeEdit_->boxStartRaster = typeRaster(doc);
            typeEdit_->boxStartMap = typeEdit_->toDocument;
            drag_ = Drag::Type;
            return;
        }
        if (typeContains(doc)) {
            const int at = typeEdit_->geometry.positionAt(typeRaster(doc));
            typeEdit_->caret = at;
            if (!(modifiers & Qt::ShiftModifier)) typeEdit_->anchor = at;
            typeEdit_->pending.reset();
            typeEdit_->preferredX = -1;
            typeEdit_->drag = TypeState::Drag::Select;
            typeEdit_->caretOn = true;
            caretBlink_.start();
            drag_ = Drag::Type;
            update();
            emit typeEditChanged();
            return;
        }
        // A click outside commits, as in Photoshop.
        commitType();
        return;
    }
    // A click on text edits it; anywhere else starts new type (a drag: a paragraph box).
    std::optional<Uuid> under = session_->layerAt(doc);
    const Layer* hit = under ? session_->document()->find(*under) : nullptr;
    if (!hit || !hit->isLiveText()) {
        const Layer* active = session_->activeLayer();
        if (active && active->isLiveText() && session_->displayedTransform(*active).contains(Point(doc.x(), doc.y()))) hit = active;
    }
    if (hit && hit->isLiveText()) {
        if (hit->text->warp.active()) { session_->selectLayer(hit->id, false); session_->requestTextEdit(hit->id); return; }   // warped: the dialog
        session_->selectLayer(hit->id, false);
        if (startTypeOn(hit->id, doc)) { typeEdit_->drag = TypeState::Drag::Select; drag_ = Drag::Type; }
        return;
    }
    typeCreateStart_ = doc;
    typeCreateRect_.reset();
    drag_ = Drag::Type;
}

void CanvasWidget::typeMove(QPointF doc, Qt::KeyboardModifiers modifiers) {
    if (typeCreateStart_) {
        // Dragging out a paragraph box (Shift: a square, Alt: from the centre).
        if (dragMoved_) typeCreateRect_ = dragBox(*typeCreateStart_, doc, modifiers & Qt::ShiftModifier, modifiers & Qt::AltModifier);
        update();
        return;
    }
    if (!typeEdit_ || !typeLayer()) return;
    if (typeEdit_->drag == TypeState::Drag::Select) {
        const int at = typeEdit_->geometry.positionAt(typeRaster(doc));
        if (at != typeEdit_->caret) { typeEdit_->caret = at; update(); emit typeEditChanged(); }
        return;
    }
    if (typeEdit_->drag == TypeState::Drag::Box) {
        // The box in its raster's pixels, as it was when the drag began.
        const Point r = typeEdit_->boxStartMap.inverted().apply(Point(doc.x(), doc.y()));
        const QPointF d = QPointF(r.x, r.y) - typeEdit_->boxStartRaster;
        const LayerText& start = typeEdit_->boxStart;
        double left = 0, top = 0, right = start.boxWidth, bottom = start.boxHeight;
        const int edges = typeEdit_->boxEdges;
        if (edges & 1) left = std::min(right - 4, left + d.x());
        if (edges & 4) right = std::max(left + 4, right + d.x());
        if (edges & 2) top = std::min(bottom - 4, top + d.y());
        if (edges & 8) bottom = std::max(top + 4, bottom + d.y());
        LayerText text = *typeLayer()->text;
        text.boxWidth = std::round(right - left);
        text.boxHeight = std::round(bottom - top);
        if (text.boxWidth == typeLayer()->text->boxWidth && text.boxHeight == typeLayer()->text->boxHeight) return;
        // The box's corner moves with a left or top edge: the shift from where it is now.
        const double nowLeft = typeEdit_->boxShift.x(), nowTop = typeEdit_->boxShift.y();
        const QPointF shift(std::round(left) - nowLeft, std::round(top) - nowTop);
        typeEdit_->boxShift = QPointF(std::round(left), std::round(top));
        setTypeText(text, typeEdit_->caret, typeEdit_->anchor, shift, !typeEdit_->boxRecorded);
        typeEdit_->boxRecorded = true;
    }
}

void CanvasWidget::typeRelease(QPointF doc) {
    if (typeCreateStart_) {
        const QPointF start = *typeCreateStart_;
        const std::optional<QRectF> box = typeCreateRect_;
        typeCreateStart_.reset();
        typeCreateRect_.reset();
        if (box && box->width() >= 4 && box->height() >= 4) startNewType(doc, box);
        else startNewType(start, std::nullopt);
        return;
    }
    if (typeEdit_) {
        typeEdit_->drag = TypeState::Drag::None;
        typeEdit_->boxShift = {};
        typeEdit_->boxRecorded = false;
    }
}

bool CanvasWidget::typeDoubleClick(QPointF doc) {
    if (!typeEdit_ || !typeLayer() || !typeContains(doc)) return false;
    const QString s = qText(*typeLayer()->text);
    const auto [from, to] = wordAround(s, typeEdit_->geometry.positionAt(typeRaster(doc)));
    typeEdit_->anchor = from;
    typeEdit_->caret = to;
    update();
    emit typeEditChanged();
    return true;
}

void CanvasWidget::drawTypeOverlay(QPainter& painter) {
    if (typeCreateRect_) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(255, 255, 255, 200), 1, Qt::DashLine));
        painter.drawRect(QRectF(viewPoint(typeCreateRect_->topLeft()), viewPoint(typeCreateRect_->bottomRight())));
    }
    const Layer* layer = typeLayer();
    if (!layer) return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    auto polygon = [this](const QRectF& r) {
        return QPolygonF({typeView(r.topLeft()), typeView(r.topRight()), typeView(r.bottomRight()), typeView(r.bottomLeft())});
    };
    // The box: paragraph text's frame with its handles, point text's extent.
    const bool boxed = typeBoxed();
    const QRectF frame = boxed ? QRectF(textPadding, textPadding, layer->text->boxWidth, layer->text->boxHeight)
                               : QRectF(QPointF(0, 0), typeEdit_->rasterSize).adjusted(textPadding / 2.0, textPadding / 2.0, -textPadding / 2.0, -textPadding / 2.0);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(0, 0, 0, 160), 1));
    painter.drawPolygon(polygon(frame));
    painter.setPen(QPen(QColor(255, 255, 255, 220), 1, Qt::DashLine));
    painter.drawPolygon(polygon(frame));
    if (boxed) {
        painter.setBrush(Qt::white);
        painter.setPen(QPen(Qt::black, 1));
        static const double ux[8] = {0, 0.5, 1, 1, 1, 0.5, 0, 0}, uy[8] = {0, 0, 0, 0.5, 1, 1, 1, 0.5};
        for (int i = 0; i < 8; i++) {
            const QPointF v = typeView(frame.topLeft() + QPointF(ux[i] * frame.width(), uy[i] * frame.height()));
            painter.drawRect(QRectF(v.x() - handleRadius + 1, v.y() - handleRadius + 1, 2 * handleRadius - 2, 2 * handleRadius - 2));
        }
    }
    // The selection, then the caret.
    const int from = std::min(typeEdit_->caret, typeEdit_->anchor), to = std::max(typeEdit_->caret, typeEdit_->anchor);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 120, 215, 110));
    for (const QRectF& r : typeEdit_->geometry.selection(from, to)) painter.drawPolygon(polygon(r));
    if (!typeEdit_->preedit.isEmpty()) {
        // The input method's text not yet committed, underlined at the caret.
        const auto [top, bottom] = typeEdit_->geometry.caret(typeEdit_->caret);
        const QPointF a = typeView(bottom);
        QFont font = this->font();
        font.setPixelSize(std::max(10, int(QLineF(typeView(top), a).length() * 0.8)));
        painter.setFont(font);
        const QRectF bounds = QFontMetricsF(font).boundingRect(typeEdit_->preedit);
        painter.setBrush(QColor(255, 255, 255, 230));
        painter.drawRect(QRectF(a.x(), a.y() - bounds.height(), bounds.width() + 4, bounds.height()));
        painter.setPen(Qt::black);
        painter.drawText(QPointF(a.x() + 2, a.y() - 3), typeEdit_->preedit);
        painter.drawLine(QPointF(a.x() + 2, a.y() - 1), QPointF(a.x() + 2 + bounds.width(), a.y() - 1));
    }
    if (typeEdit_->caretOn && from == to) {
        const auto [top, bottom] = typeEdit_->geometry.caret(typeEdit_->caret);
        painter.setPen(QPen(QColor(255, 255, 255, 200), 3));
        painter.drawLine(typeView(top), typeView(bottom));
        painter.setPen(QPen(Qt::black, 1.2));
        painter.drawLine(typeView(top), typeView(bottom));
    }
    painter.restore();
}

} // namespace app
