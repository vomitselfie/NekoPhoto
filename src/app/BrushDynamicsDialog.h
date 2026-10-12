// Brush Dynamics: the few dynamics a tip brush exposes for editing (docs/brush-engine.md). Pen pressure on size and on
// flow, each through a curve with a minimum; density by spacing; and a mouse's speed as simulated pressure. The
// brush's other mappings (jitter, tilt, fade...) are kept as they are. Above them, for every brush, the Brush tool's
// smoothing: the stabiliser's modes (its amount is the options bar's Smoothing), input smoothing and pressure smoothing.
#pragma once
#include "nekophoto/brushsmoothing.h"
#include "nekophoto/tipbrush.h"
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
    void setCurve(const nekophoto::DynamicsCurve& curve);
    nekophoto::DynamicsCurve curve() const { return curve_; }
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
    nekophoto::DynamicsCurve curve_;
    int dragging_ = -1;
};

class BrushDynamicsDialog : public QDialog {
    Q_OBJECT
public:
    /// Edits the tip brush `presetId` and saves it when accepted, and `smoothing` (the Brush tool's) when given. False
    /// when there is nothing to edit: not a tip brush, and no smoothing.
    static bool edit(QWidget* parent, const QString& presetId, nekophoto::BrushSmoothing* smoothing = nullptr);
    /// The Brush tool's button that opens this, and its tooltip when the brush is not a tip brush.
    static QString buttonText();
    static QString unavailableText();
    /// An input's and a target's name as shown: the two speeds say where they are measured, "Speed (on screen)" and
    /// "Speed (in the document)".
    static QString inputLabel(nekophoto::DynamicsInput input);
    static QString targetLabel(nekophoto::DynamicsTarget target);

private:
    BrushDynamicsDialog(const nekophoto::BrushTip* tip, const QString& name, const nekophoto::BrushSmoothing* smoothing, QWidget* parent);
    void applySmoothing(nekophoto::BrushSmoothing& smoothing) const;
    struct Row {
        QGroupBox* box = nullptr;
        CurveEditor* curve = nullptr;
        QDoubleSpinBox* minimum = nullptr;
        QCheckBox* smooth = nullptr;
    };
    Row addRow(nekophoto::DynamicsTarget target, const QString& title, const nekophoto::BrushTip& tip);
    void apply(nekophoto::BrushTip& tip) const;
    void applyRow(const Row& row, nekophoto::DynamicsTarget target, nekophoto::BrushTip& tip) const;
    Row size_, flow_;
    QCheckBox* density_ = nullptr;
    QCheckBox* mouseSpeed_ = nullptr;
    QCheckBox* tiltShape_ = nullptr;
    QDoubleSpinBox* flattest_ = nullptr;
    QDoubleSpinBox* stabilizer_ = nullptr;
    QCheckBox* pulledString_ = nullptr;
    QCheckBox* strokeCatchUp_ = nullptr;
    QCheckBox* catchUpOnEnd_ = nullptr;
    QCheckBox* adjustForZoom_ = nullptr;
    QDoubleSpinBox* inputSmoothing_ = nullptr;
    QDoubleSpinBox* pressureSmoothing_ = nullptr;
};

} // namespace app
