// An edge's slide-out card (Flyout.h).
#include "Flyout.h"
#include "Motion.h"
#include "Theme.h"
#include "ToolOptionsBar.h"
#include <QBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <cmath>

using namespace nekophoto;

namespace app {

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

} // namespace app
