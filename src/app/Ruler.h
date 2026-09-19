// Rulers in document pixels along the top and left of the canvas, with a marker
// that follows the pointer.
#pragma once
#include "EditorSession.h"
#include "Style.h"
#include <QFontMetrics>
#include <QPainter>
#include <QWidget>
#include <cmath>

namespace app {

class Ruler : public QWidget {
    Q_OBJECT
public:
    static constexpr int thickness = 20;

    Ruler(EditorSession* session, Qt::Orientation orientation, QWidget* parent = nullptr)
        : QWidget(parent), session_(session), orientation_(orientation) {
        if (orientation_ == Qt::Horizontal) setFixedHeight(thickness); else setFixedWidth(thickness);
        QFont f = font();
        f.setPointSizeF(std::max(6.0, f.pointSizeF() - 2));
        setFont(f);
        connect(session_, &EditorSession::viewportChanged, this, qOverload<>(&QWidget::update));
        connect(session_, &EditorSession::layersChanged, this, qOverload<>(&QWidget::update));
    }

    void setPointer(QPointF documentPoint) { pointer_ = documentPoint; hasPointer_ = true; update(); }
    void clearPointer() { hasPointer_ = false; update(); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), palette().color(QPalette::Window));
        QColor line = hintColor(4), text = hintColor(8);
        bool horizontal = orientation_ == Qt::Horizontal;
        int length = horizontal ? width() : height();
        // Bottom/right edge.
        p.setPen(line);
        if (horizontal) p.drawLine(0, height() - 1, width(), height() - 1); else p.drawLine(width() - 1, 0, width() - 1, height());
        if (!session_->hasDocument()) return;
        const Viewport& vp = session_->viewport;
        QSizeF docSize(session_->document()->width, session_->document()->height);
        double ppp = vp.pointsPerPixel();
        QPointF origin = vp.documentRect(docSize).topLeft();
        double start = horizontal ? origin.x() : origin.y();

        // Major ticks at least ~70 view points apart, from the 1-2-5 series; five minors between them.
        double step = 1;
        static const double series[] = {2, 2.5, 2};
        for (int k = 0; step * ppp < 70; k++) step *= series[k % 3];
        const int minors = 5;
        double minor = step / minors;

        long long first = (long long)std::floor(-start / ppp / minor) - 1, last = (long long)std::ceil((length - start) / ppp / minor) + 1;
        QFontMetrics fm(font());
        for (long long i = first; i <= last; i++) {
            double d = i * minor, v = start + d * ppp;
            if (v < -1 || v > length + 1) continue;
            bool major = i % minors == 0;
            int tick = major ? thickness : thickness / 3;
            p.setPen(line);
            if (horizontal) p.drawLine(QPointF(v, height()), QPointF(v, height() - tick));
            else p.drawLine(QPointF(width(), v), QPointF(width() - tick, v));
            if (!major) continue;
            QString label = QString::number(std::llround(d));
            p.setPen(text);
            if (horizontal) p.drawText(QPointF(v + 3, fm.ascent() + 1), label);
            else {
                p.save();
                p.translate(fm.ascent() + 1, v - 3);
                p.rotate(-90);
                p.drawText(QPointF(0, 0), label);
                p.restore();
            }
        }
        if (hasPointer_) {
            double v = start + (horizontal ? pointer_.x() : pointer_.y()) * ppp;
            p.setPen(palette().color(QPalette::Highlight));
            if (horizontal) p.drawLine(QPointF(v, 0), QPointF(v, height())); else p.drawLine(QPointF(0, v), QPointF(width(), v));
        }
    }

private:
    EditorSession* session_;
    Qt::Orientation orientation_;
    QPointF pointer_;
    bool hasPointer_ = false;
};

} // namespace app
