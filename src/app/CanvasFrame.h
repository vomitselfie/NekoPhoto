// The canvas with its rulers: a corner square, the top and left rulers, and the
// canvas widget itself in a grid.
#pragma once
#include "CanvasWidget.h"
#include "Ruler.h"
#include <QGridLayout>
#include <QWidget>

namespace app {

class CanvasFrame : public QWidget {
    Q_OBJECT
public:
    CanvasFrame(EditorSession* session, CanvasWidget* canvas, QWidget* parent = nullptr) : QWidget(parent), canvas_(canvas) {
        auto* grid = new QGridLayout(this);
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setSpacing(0);
        corner_ = new QWidget;
        corner_->setFixedSize(Ruler::thickness, Ruler::thickness);
        corner_->setAutoFillBackground(true);
        top_ = new Ruler(session, Qt::Horizontal);
        left_ = new Ruler(session, Qt::Vertical);
        grid->addWidget(corner_, 0, 0);
        grid->addWidget(top_, 0, 1);
        grid->addWidget(left_, 1, 0);
        grid->addWidget(canvas_, 1, 1);
        grid->setRowStretch(1, 1);
        grid->setColumnStretch(1, 1);
        for (Ruler* ruler : {top_, left_}) {
            const Qt::Orientation orientation = ruler == top_ ? Qt::Horizontal : Qt::Vertical;
            ruler->setCursor(orientation == Qt::Horizontal ? Qt::SplitVCursor : Qt::SplitHCursor);
            ruler->guideDrag = [this, orientation](int phase, QPoint global, Qt::KeyboardModifiers modifiers) {
                if (phase == 0) canvas_->beginRulerGuide(orientation, global);
                else if (phase == 1) canvas_->moveRulerGuide(global, modifiers);
                else canvas_->endRulerGuide(global, modifiers);
            };
        }
        connect(canvas_, &CanvasWidget::cursorMoved, this, [this](QPointF p) { top_->setPointer(p); left_->setPointer(p); });
    }

    CanvasWidget* canvas() const { return canvas_; }
    bool rulersVisible() const { return top_->isVisible(); }
    void setRulersVisible(bool on) { corner_->setVisible(on); top_->setVisible(on); left_->setVisible(on); }

private:
    CanvasWidget* canvas_;
    QWidget* corner_;
    Ruler* top_;
    Ruler* left_;
};

} // namespace app
