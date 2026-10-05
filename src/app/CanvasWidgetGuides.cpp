// Ruler guides on the canvas: drawn over the document, picked up and moved with the Move tool, pulled out of the
// rulers, and dropped outside the canvas (onto a ruler) to delete them, as in Photoshop. Every add, move and
// removal is one undo step (EditorSession::addGuide and friends).
#include "ActionLibrary.h"
#include "CanvasWidget.h"
#include "ViewOptions.h"
#include <QJsonObject>
#include <QPainter>
#include <cmath>

using namespace compositor;

namespace app {

namespace {
/// How near (in screen points) the pointer must be to pick a guide up.
constexpr double guideReachPoints = 4;
/// Photoshop's default guide colour (Cyan).
const QColor guideColor(0, 255, 255);
const QColor smartGuideColor(255, 0, 200);
} // namespace

std::optional<int> CanvasWidget::guideUnder(QPointF view) const {
    const ViewOptions& options = ViewOptions::get();
    if (!session_->hasDocument() || !options.showGuides || options.lockGuides || session_->transformEdit()) return std::nullopt;
    return session_->guideAt(documentPoint(view), guideReachPoints / session_->viewport.pointsPerPixel());
}

void CanvasWidget::dragGuide(QPointF view, Qt::KeyboardModifiers modifiers) {
    if (!guideDrag_ || !session_->hasDocument()) return;
    GuideDrag& g = *guideDrag_;
    g.outside = !QRectF(rect()).contains(view);
    const bool vertical = g.orientation == Guide::Orientation::Vertical;
    const QPointF doc = documentPoint(view);
    // Whole pixels at every zoom: a guide lands on the pixel grid, as Photoshop places one dragged by hand.
    double position = std::round(vertical ? doc.x() : doc.y());
    std::vector<double> gx, gy;
    if (!(modifiers & Qt::ControlModifier) && !g.outside) {
        // Onto the canvas's edges and centre and the layers' edges, within the same screen distance as everything else.
        std::vector<double> xs, ys;
        guideTargets(xs, ys, false);
        double best = snapDistance / session_->viewport.pointsPerPixel();
        const double raw = vertical ? doc.x() : doc.y();
        std::optional<double> snapped;
        for (double t : vertical ? xs : ys) if (std::abs(t - raw) <= best) { best = std::abs(t - raw); snapped = t; }
        if (snapped) { position = *snapped; (vertical ? gx : gy).push_back(*snapped); }
    }
    g.position = guidePosition(position);
    session_->setSnapGuides(gx, gy);
    update();
}

void CanvasWidget::finishGuideDrag() {
    if (!guideDrag_) return;
    const GuideDrag g = *guideDrag_;
    guideDrag_.reset();
    session_->setSnapGuides({}, {});
    if (g.outside) {
        if (g.index && session_->removeGuide(*g.index)) recordAction("guides.delete", {{"index", *g.index}});
    } else if (g.index) {
        if (session_->moveGuide(*g.index, g.position)) recordAction("guides.move", {{"index", *g.index}, {"position", g.position}});
    } else if (session_->addGuide(Guide{g.orientation, g.position})) {
        recordAction("guides.add", {{"orientation", g.orientation == Guide::Orientation::Vertical ? "vertical" : "horizontal"}, {"position", g.position}});
    }
    update();
}

void CanvasWidget::beginRulerGuide(Qt::Orientation ruler, QPoint globalPosition) {
    if (!session_->hasDocument() || drag_ != Drag::None) return;
    // A guide drawn while they are hidden shows them all again, as Photoshop does.
    ViewOptions& options = ViewOptions::get();
    if (!options.showGuides) { options.showGuides = true; options.save(); }
    guideDrag_ = GuideDrag{std::nullopt, ruler == Qt::Horizontal ? Guide::Orientation::Horizontal : Guide::Orientation::Vertical, 0, true};
    drag_ = Drag::Guide;
    dragGuide(mapFromGlobal(QPointF(globalPosition)), Qt::NoModifier);
}

void CanvasWidget::moveRulerGuide(QPoint globalPosition, Qt::KeyboardModifiers modifiers) {
    if (drag_ != Drag::Guide) return;
    dragGuide(mapFromGlobal(QPointF(globalPosition)), modifiers);
}

void CanvasWidget::endRulerGuide(QPoint globalPosition, Qt::KeyboardModifiers modifiers) {
    if (drag_ != Drag::Guide) return;
    dragGuide(mapFromGlobal(QPointF(globalPosition)), modifiers);
    drag_ = Drag::None;
    finishGuideDrag();
}

void CanvasWidget::drawGuides(QPainter& painter) {
    const ViewOptions& options = ViewOptions::get();
    if (!session_->hasDocument()) return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    // On the device pixel grid, so a guide is one crisp line at every zoom.
    auto line = [&](bool vertical, double position) {
        const QPointF v = viewPoint(QPointF(position, position));
        if (vertical) { const double x = std::floor(v.x()) + 0.5; painter.drawLine(QPointF(x, 0), QPointF(x, height())); }
        else { const double y = std::floor(v.y()) + 0.5; painter.drawLine(QPointF(0, y), QPointF(width(), y)); }
    };
    painter.setPen(QPen(guideColor, 1));
    const auto& guides = session_->guides();
    if (options.showGuides)
        for (size_t i = 0; i < guides.size(); i++) {
            if (guideDrag_ && guideDrag_->index == int(i)) continue;
            line(guides[i].vertical(), guides[i].position);
        }
    if (guideDrag_ && !guideDrag_->outside) line(guideDrag_->orientation == Guide::Orientation::Vertical, guideDrag_->position);
    // Smart guides: the one alignment in effect, in magenta, while a drag snaps to a layer or the canvas (a snap to
    // a ruler guide shows on the guide itself).
    if (options.smartGuides) {
        painter.setPen(QPen(smartGuideColor, 1));
        auto isGuide = [&](bool vertical, double at) {
            if (!options.showGuides) return false;
            for (const Guide& g : guides) if (g.vertical() == vertical && g.position == at) return true;
            return false;
        };
        for (double x : session_->snapGuidesX) if (!isGuide(true, x)) line(true, x);
        for (double y : session_->snapGuidesY) if (!isGuide(false, y)) line(false, y);
    }
    painter.restore();
}

} // namespace app
