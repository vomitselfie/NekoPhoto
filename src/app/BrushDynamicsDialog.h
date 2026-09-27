// Brush Dynamics: the few dynamics a tip brush exposes for editing (docs/brush-engine.md). Pen pressure on size and on
// flow, each through a curve with a minimum; density by spacing; and a mouse's speed as simulated pressure. The
// brush's other mappings (jitter, tilt, fade...) are kept as they are.
#pragma once
#include "compositor/tipbrush.h"
#include <QDialog>
#include <QWidget>

class QCheckBox;
class QDoubleSpinBox;
class QGroupBox;

namespace app {

/// A response curve over the unit square: drag a point to move it, click empty space to add one, double-click a
/// point to remove it (the ends stay at the left and right edges).
class CurveEditor : public QWidget {
    Q_OBJECT
public:
    explicit CurveEditor(QWidget* parent = nullptr);
    void setCurve(const compositor::DynamicsCurve& curve);
    compositor::DynamicsCurve curve() const { return curve_; }
    QSize sizeHint() const override { return {180, 180}; }

signals:
    void changed();

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;

private:
    QRectF plot() const;
    QPointF toView(double x, double y) const;
    std::pair<double, double> toUnit(QPointF p) const;
    int pointAt(QPointF p) const;
    compositor::DynamicsCurve curve_;
    int dragging_ = -1;
};

class BrushDynamicsDialog : public QDialog {
    Q_OBJECT
public:
    /// Edits the tip brush `presetId` and saves it when accepted. False when it is not a tip brush.
    static bool edit(QWidget* parent, const QString& presetId);
    /// The Brush tool's button that opens this, and its tooltip when the brush is not a tip brush.
    static QString buttonText();
    static QString unavailableText();

private:
    BrushDynamicsDialog(const compositor::BrushTip& tip, const QString& name, QWidget* parent);
    struct Row {
        QGroupBox* box = nullptr;
        CurveEditor* curve = nullptr;
        QDoubleSpinBox* minimum = nullptr;
        QCheckBox* smooth = nullptr;
    };
    Row addRow(compositor::DynamicsTarget target, const QString& title, const compositor::BrushTip& tip);
    void apply(compositor::BrushTip& tip) const;
    void applyRow(const Row& row, compositor::DynamicsTarget target, compositor::BrushTip& tip) const;
    Row size_, flow_;
    QCheckBox* density_ = nullptr;
    QCheckBox* mouseSpeed_ = nullptr;
    QCheckBox* tiltShape_ = nullptr;
    QDoubleSpinBox* flattest_ = nullptr;
};

} // namespace app
