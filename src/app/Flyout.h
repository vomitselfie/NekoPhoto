// An edge's slide-out card (WindowEdges.h): flush with the window's edge, it comes out over the canvas.
#pragma once
#include <QColor>
#include <QWidget>
#include <algorithm>

class QBoxLayout;

namespace app {

/// One edge's slide-out panel: a card flush with the edge, its inner corners rounded and a soft shadow on its inner
/// sides (Goth Kitty's stylesheet sets a deep one, rounder; the other themes have a faint one), the shadow growing as
/// the flyout comes out.
class Flyout : public QWidget {
    Q_OBJECT
    Q_PROPERTY(int cornerRadius READ cornerRadius WRITE setCornerRadius)
    Q_PROPERTY(int shadow READ shadow WRITE setShadow)
    Q_PROPERTY(QColor fill READ fill WRITE setFill)
    Q_PROPERTY(QColor border READ border WRITE setBorder)
public:
    Flyout(Qt::Edge edge, QWidget* parent);
    Qt::Edge edge() const { return edge_; }
    QBoxLayout* body() const { return body_; }
    int cornerRadius() const { return radius_; }
    void setCornerRadius(int r) { radius_ = std::max(0, r); updateMargins(); update(); }
    int shadow() const { return shadow_; }
    void setShadow(int s);
    QColor fill() const { return fill_; }
    void setFill(const QColor& c) { fill_ = c; update(); }
    QColor border() const { return border_; }
    void setBorder(const QColor& c) { border_ = c; update(); }
    /// The card without its shadow, in the flyout's own coordinates.
    QRect cardRect() const;
    /// How far it is out (0..1): the shadow deepens with it.
    void setReveal(double reveal);
    double reveal() const { return reveal_; }
protected:
    void paintEvent(QPaintEvent*) override;
private:
    void updateMargins();
    Qt::Edge edge_;
    QBoxLayout* body_;
    int radius_ = 0, shadow_ = 0;
    double reveal_ = 1;
    QColor fill_, border_;
};

} // namespace app
