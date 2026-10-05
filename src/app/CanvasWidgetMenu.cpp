// The canvas-side parts of the context menu (MainWindowCanvasMenu.cpp builds the rest): the Pen and Direct
// Selection tools' anchor and path commands, and the text being typed.
#include "CanvasWidget.h"
#include "QtGeometry.h"
#include "compositor/vectorlayer.h"
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <cmath>

using namespace compositor;

namespace app {

void CanvasWidget::contextMenuEvent(QContextMenuEvent* e) {
    // Not in the middle of a drag (a right-click while painting does nothing, as before).
    if (drag_ != Drag::None || !session_->hasDocument()) return;
    e->accept();
    emit contextMenuRequested(e->reason() == QContextMenuEvent::Mouse ? QPointF(e->pos()) : (hover_ ? *hover_ : QPointF(rect().center())));
}

void CanvasWidget::convertPoint(VectorPath::Subpath& sub, int knot) {
    // A smooth knot becomes a corner (its handles pulled in), a corner smooth (handles along its neighbours).
    auto& k = sub.knots[size_t(knot)];
    if (knotIsSmooth(k) || k.inX != k.x || k.outX != k.x || k.inY != k.y || k.outY != k.y) { k.inX = k.outX = k.x; k.inY = k.outY = k.y; return; }
    const size_t n = sub.knots.size(), i = size_t(knot);
    const auto& prev = sub.knots[(i + n - 1) % n];
    const auto& next = sub.knots[(i + 1) % n];
    const double dx = (next.x - prev.x) / 6, dy = (next.y - prev.y) / 6;
    k.inX = k.x - dx; k.inY = k.y - dy; k.outX = k.x + dx; k.outY = k.y + dy;
}

bool CanvasWidget::addPathMenu(QMenu* menu, QPointF view) {
    bool added = false;
    // A path being drawn with the Pen: close it or leave it open.
    if (const auto& draft = session_->penDraft(); draft && session_->tool() == Tool::Pen) {
        if (draft->knots.size() >= 2) menu->addAction(tr("Close Path"), this, [this] { session_->penFinish(true); });
        if (!draft->knots.empty()) menu->addAction(tr("End Path"), this, [this] { session_->penFinish(false); });
        return !menu->isEmpty();
    }
    const QPointF doc = documentPoint(view);
    if (auto path = session_->targetPath()) {
        const double radius = 6 / std::max(1e-6, session_->viewport.pointsPerPixel());
        if (auto knot = compositor::nearestKnot(*path, toPoint(doc), radius)) {
            const auto [sub, index] = *knot;
            menu->addAction(tr("Delete Anchor Point"), this, [this, sub, index] {
                auto p = session_->targetPath();
                if (!p || sub >= int(p->subpaths.size()) || index >= int(p->subpaths[size_t(sub)].knots.size())) return;
                compositor::removeAnchor(*p, sub, index);
                selectedKnot_.reset();
                session_->setTargetPath(*p, QT_TRANSLATE_NOOP("History", "Delete Anchor Point"));
            });
            menu->addAction(tr("Convert Point"), this, [this, sub, index] {
                auto p = session_->targetPath();
                if (!p || sub >= int(p->subpaths.size()) || index >= int(p->subpaths[size_t(sub)].knots.size())) return;
                convertPoint(p->subpaths[size_t(sub)], index);
                session_->setTargetPath(*p, QT_TRANSLATE_NOOP("History", "Convert Point"));
            });
            added = true;
        } else if (auto hit = compositor::nearestPathSegment(*path, toPoint(doc)); hit && hit->distance <= radius) {
            const int sub = hit->subpath, segment = hit->segment;
            const double t = hit->t;
            menu->addAction(tr("Add Anchor Point"), this, [this, sub, segment, t] {
                auto p = session_->targetPath();
                if (!p || sub >= int(p->subpaths.size())) return;
                compositor::insertAnchor(*p, sub, segment, t);
                session_->setTargetPath(*p, QT_TRANSLATE_NOOP("History", "Add Anchor Point"));
            });
            added = true;
        }
        // An open component under the pointer can be closed.
        const PathHit under = pathHit(view);
        if (under.sub >= 0 && under.sub < int(path->subpaths.size()) && !path->subpaths[size_t(under.sub)].closed && path->subpaths[size_t(under.sub)].knots.size() >= 2) {
            const int sub = under.sub;
            menu->addAction(tr("Close Path"), this, [this, sub] {
                auto p = session_->targetPath();
                if (!p || sub >= int(p->subpaths.size())) return;
                p->subpaths[size_t(sub)].closed = true;
                session_->setTargetPath(*p, QT_TRANSLATE_NOOP("History", "Close Path"));
            });
            added = true;
        }
    }
    // The Paths panel's commands for the document path being edited.
    if (auto id = session_->activePathId()) {
        if (added) menu->addSeparator();
        const uint16_t path = *id;
        menu->addAction(tr("Make Selection"), this, [this, path] { session_->pathToSelection(path, SelectionMode::Replace); });
        menu->addAction(tr("Fill Path"), this, [this, path] { session_->fillPath(path); });
        menu->addAction(tr("Stroke Path"), this, [this, path] { session_->strokePath(path); });
        menu->addSeparator();
        menu->addAction(tr("Delete Path"), this, [this, path] { selectedKnot_.reset(); session_->deletePath(path); });
        added = true;
    }
    return added;
}

bool CanvasWidget::addTypeMenu(QMenu* menu) {
    if (!typeEdit_ || !typeLayer()) return false;
    // The keys typing already answers, as menu commands.
    auto key = [this](int k, Qt::KeyboardModifiers modifiers = Qt::ControlModifier) {
        QKeyEvent event(QEvent::KeyPress, k, modifiers);
        if (typeKey(&event)) update();
    };
    const bool selected = typeEdit_->caret != typeEdit_->anchor;
    // Shortcuts shown beside the commands, not bound again (the keys already reach typing).
    auto shown = [](const QString& label, const QKeySequence& keys) { return label + '\t' + keys.toString(QKeySequence::NativeText); };
    if (!typeEdit_->undo.empty()) {
        menu->addAction(shown(tr("Undo Typing"), QKeySequence::Undo), this, [key] { key(Qt::Key_Z); });
        menu->addSeparator();
    }
    if (selected) {
        menu->addAction(shown(tr("Cut"), QKeySequence::Cut), this, [key] { key(Qt::Key_X); });
        menu->addAction(shown(tr("Copy"), QKeySequence::Copy), this, [key] { key(Qt::Key_C); });
    }
    menu->addAction(shown(tr("Paste"), QKeySequence::Paste), this, [key] { key(Qt::Key_V); });
    menu->addAction(shown(tr("Select All"), QKeySequence::SelectAll), this, [key] { key(Qt::Key_A); });
    menu->addSeparator();
    // Photoshop's faux styles and lines, for the selected letters (or those typed next).
    const std::optional<TextRun> run = typeStyleAtCaret();
    auto toggle = [&](const QString& label, bool on, auto apply) {
        QAction* a = menu->addAction(label, this, [this, on, apply] { TextRunPatch patch; apply(patch, !on); applyTypeStyle(patch); });
        a->setCheckable(true);
        a->setChecked(on);
    };
    toggle(tr("Faux Bold"), run && run->bold, [](TextRunPatch& p, bool v) { p.bold = v; });
    toggle(tr("Faux Italic"), run && run->italic, [](TextRunPatch& p, bool v) { p.italic = v; });
    toggle(tr("Underline"), run && run->underline, [](TextRunPatch& p, bool v) { p.underline = v; });
    toggle(tr("Strikethrough"), run && run->strikethrough, [](TextRunPatch& p, bool v) { p.strikethrough = v; });
    menu->addSeparator();
    menu->addAction(shown(tr("Commit Typing"), QKeySequence(Qt::CTRL | Qt::Key_Return)), this, [this] { commitType(); });
    menu->addAction(shown(tr("Cancel Typing"), QKeySequence(Qt::Key_Escape)), this, [this] { cancelType(); });
    return true;
}

} // namespace app
