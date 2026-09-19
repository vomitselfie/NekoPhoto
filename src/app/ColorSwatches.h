// Photoshop-style colour swatches for the tool rail: the foreground square overlaps the
// background one, with the tiny default-colours and swap controls in the free corners.
#pragma once
#include "Icons.h"
#include <QMouseEvent>
#include <QPainter>
#include <QWidget>

namespace app {

class ColorSwatches : public QWidget {
    Q_OBJECT
public:
    explicit ColorSwatches(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedSize(46, 50);
        setToolTip(tr("Foreground and background colours. Click one to change it; X swaps them, D resets them."));
        setCursor(Qt::PointingHandCursor);
    }
    void setColors(const QColor& foreground, const QColor& background) { fg_ = foreground; bg_ = background; update(); }

signals:
    void foregroundClicked();
    void backgroundClicked();
    void swapRequested();
    void defaultsRequested();

protected:
    static QRect foregroundRect() { return {2, 8, 26, 26}; }
    static QRect backgroundRect() { return {16, 22, 26, 26}; }
    static QRect defaultsRect() { return {2, 36, 12, 12}; }
    static QRect swapRect() { return {32, 2, 12, 12}; }

    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        QColor edge = palette().color(QPalette::ButtonText);
        edge.setAlpha(150);
        auto square = [&](QRect r, const QColor& c) {
            p.fillRect(r, c);
            p.setPen(edge);
            p.drawRect(r.adjusted(0, 0, -1, -1));
        };
        square(backgroundRect(), bg_);
        square(foregroundRect(), fg_);
        QRect d = defaultsRect();
        square(QRect(d.left() + 4, d.top() + 4, 8, 8), Qt::white);
        square(QRect(d.left(), d.top(), 8, 8), Qt::black);
        QRect s = swapRect();
        p.drawPixmap(s, renderIcon("arrow-left-right", palette().color(QPalette::ButtonText), s.width(), devicePixelRatioF()));
    }

    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) return;
        QPoint pos = e->pos();
        if (swapRect().contains(pos)) emit swapRequested();
        else if (defaultsRect().contains(pos)) emit defaultsRequested();
        else if (foregroundRect().contains(pos)) emit foregroundClicked();
        else if (backgroundRect().contains(pos)) emit backgroundClicked();
    }

private:
    QColor fg_ = Qt::black, bg_ = Qt::white;
};

} // namespace app
