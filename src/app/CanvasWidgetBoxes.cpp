// The Artboard and Slice tools on the canvas: drag out a rectangle, drag inside one to move it, drag an edge or a
// corner to resize it; each gesture is one undo step on release. Artboards' outlines and names are always drawn,
// slices' while the Slice tool is chosen.
#include "CanvasWidget.h"
#include <QPainter>
#include <algorithm>
#include <cmath>

using namespace compositor;

namespace app {

namespace {

QRectF rectOf(const Artboard& a) { return QRectF(a.x, a.y, a.width, a.height); }
QRectF rectOf(const Slice& s) { return QRectF(s.x, s.y, s.width, s.height); }

QRect whole(const QRectF& r) {
    const QRectF n = r.normalized();
    const int x0 = int(std::lround(n.left())), y0 = int(std::lround(n.top())), x1 = int(std::lround(n.right())), y1 = int(std::lround(n.bottom()));
    return QRect(x0, y0, std::max(0, x1 - x0), std::max(0, y1 - y0));
}

} // namespace

void CanvasWidget::pressBox(QPointF view, QPointF doc, Qt::KeyboardModifiers modifiers) {
    Q_UNUSED(modifiers);
    const auto& document = *session_->document();
    // The rectangles the tool edits, topmost first.
    std::vector<std::pair<QRectF, BoxDrag>> targets;
    if (session_->tool() == Tool::Artboard) {
        for (auto it = document.layers.rbegin(); it != document.layers.rend(); ++it)
            if (it->isGroup && it->artboard) { BoxDrag d; d.artboard = it->id; targets.push_back({rectOf(*it->artboard), d}); }
    } else {
        for (auto it = document.slices.rbegin(); it != document.slices.rend(); ++it) { BoxDrag d; d.slice = it->id; targets.push_back({rectOf(*it), d}); }
    }
    const double reach = handleRadius + 2;
    for (auto& [rect, drag] : targets) {
        const QRectF v(viewPoint(rect.topLeft()), viewPoint(rect.bottomRight()));
        if (!v.adjusted(-reach, -reach, reach, reach).contains(view)) continue;
        int edges = 0;
        if (std::abs(view.x() - v.left()) <= reach) edges |= 1;
        if (std::abs(view.y() - v.top()) <= reach) edges |= 2;
        if (std::abs(view.x() - v.right()) <= reach) edges |= 4;
        if (std::abs(view.y() - v.bottom()) <= reach) edges |= 8;
        drag.origin = rect;
        drag.edges = edges;
        drag.kind = edges ? BoxDrag::Kind::Resize : BoxDrag::Kind::Move;
        if (drag.artboard) session_->selectLayer(*drag.artboard);
        boxDrag_ = drag;
        boxDraft_ = rect;
        drag_ = Drag::Box;
        return;
    }
    BoxDrag d;
    d.kind = BoxDrag::Kind::Draw;
    const QPointF start = snapPoint(doc);
    d.origin = QRectF(QPointF(std::round(start.x()), std::round(start.y())), QSizeF(0, 0));
    dragStartDocument_ = d.origin.topLeft();
    boxDrag_ = d;
    boxDraft_ = d.origin;
    drag_ = Drag::Box;
    update();
}

void CanvasWidget::moveBox(QPointF doc, Qt::KeyboardModifiers modifiers) {
    if (!boxDrag_) return;
    const QPointF p = (modifiers & Qt::ControlModifier) ? doc : snapPoint(doc);
    const QPointF whole(std::round(p.x()), std::round(p.y()));
    QRectF r = boxDrag_->origin;
    switch (boxDrag_->kind) {
    case BoxDrag::Kind::Draw:
        r = dragBox(dragStartDocument_, whole, modifiers & Qt::ShiftModifier, modifiers & Qt::AltModifier);
        break;
    case BoxDrag::Kind::Move: {
        const QPointF d = doc - dragStartDocument_;
        r.translate(std::round(d.x()), std::round(d.y()));
        break;
    }
    case BoxDrag::Kind::Resize:
        if (boxDrag_->edges & 1) r.setLeft(std::min(whole.x(), r.right() - 1));
        if (boxDrag_->edges & 2) r.setTop(std::min(whole.y(), r.bottom() - 1));
        if (boxDrag_->edges & 4) r.setRight(std::max(whole.x(), r.left() + 1));
        if (boxDrag_->edges & 8) r.setBottom(std::max(whole.y(), r.top() + 1));
        break;
    }
    boxDraft_ = r;
    update();
}

void CanvasWidget::releaseBox() {
    session_->setSnapGuides({}, {});
    const std::optional<BoxDrag> drag = boxDrag_;
    const std::optional<QRectF> draft = boxDraft_;
    boxDrag_.reset();
    boxDraft_.reset();
    update();
    if (!drag || !draft) return;
    const QRect r = whole(*draft);
    if (r.width() < 1 || r.height() < 1) return;
    const auto& document = *session_->document();
    if (drag->kind == BoxDrag::Kind::Draw) {
        if (r.width() < 2 && r.height() < 2) return;   // a click, not a drag
        if (session_->tool() == Tool::Artboard) {
            Artboard a;
            a.x = r.x(); a.y = r.y(); a.width = r.width(); a.height = r.height();
            a.presetName = "Custom";
            session_->addArtboard(a);
        } else {
            Slice s;
            s.id = 0;
            s.x = r.x(); s.y = r.y(); s.width = r.width(); s.height = r.height();
            session_->addSlice(s);
        }
        return;
    }
    if (drag->artboard) {
        const Layer* l = document.find(*drag->artboard);
        if (!l || !l->artboard) return;
        Artboard a = *l->artboard;
        a.x = r.x(); a.y = r.y(); a.width = r.width(); a.height = r.height();
        session_->setArtboard(l->id, a, drag->kind == BoxDrag::Kind::Move);
    } else if (drag->slice) {
        for (const Slice& s : document.slices) {
            if (s.id != *drag->slice) continue;
            Slice changed = s;
            changed.x = r.x(); changed.y = r.y(); changed.width = r.width(); changed.height = r.height();
            session_->setSlice(changed);
            break;
        }
    }
}

void CanvasWidget::drawBoxes(QPainter& painter) {
    if (!session_->hasDocument()) return;
    const auto& document = *session_->document();
    auto viewRect = [&](const QRectF& r) { return QRectF(viewPoint(r.topLeft()), viewPoint(r.bottomRight())); };
    painter.setBrush(Qt::NoBrush);
    QFont font = painter.font();
    font.setPointSizeF(std::max(7.0, font.pointSizeF() - 1));
    painter.setFont(font);
    const bool editingArtboards = session_->tool() == Tool::Artboard;
    for (const Layer& l : document.layers) {
        if (!l.isGroup || !l.artboard) continue;
        const QRectF v = viewRect(rectOf(*l.artboard));
        const bool active = session_->activeLayerId() == l.id;
        painter.setPen(QPen(active && editingArtboards ? QColor(40, 120, 255) : QColor(128, 128, 128, 200), 1));
        painter.drawRect(v);
        painter.setPen(QColor(200, 200, 200));
        painter.drawText(v.topLeft() + QPointF(0, -4), QString::fromStdString(l.name));
    }
    if (session_->tool() == Tool::Slice) {
        for (const Slice& s : document.slices) {
            const QRectF v = viewRect(rectOf(s));
            painter.setPen(QPen(QColor(40, 120, 255), 1));
            painter.drawRect(v);
            const QString label = QStringLiteral("%1 %2").arg(s.id).arg(QString::fromStdString(s.name));
            const QRectF tag(v.topLeft(), QSizeF(painter.fontMetrics().horizontalAdvance(label) + 6, painter.fontMetrics().height() + 2));
            painter.fillRect(tag, QColor(40, 120, 255));
            painter.setPen(Qt::white);
            painter.drawText(tag, Qt::AlignCenter, label);
        }
    }
    if (boxDraft_) {
        const QRectF v = viewRect(boxDraft_->normalized());
        painter.setPen(QPen(Qt::white, 3));
        painter.drawRect(v);
        painter.setPen(QPen(Qt::black, 1, Qt::DashLine));
        painter.drawRect(v);
        const QString size = QStringLiteral("%1 x %2").arg(std::lround(boxDraft_->normalized().width())).arg(std::lround(boxDraft_->normalized().height()));
        painter.setPen(Qt::white);
        painter.drawText(v.bottomRight() + QPointF(6, 14), size);
    }
}

} // namespace app
