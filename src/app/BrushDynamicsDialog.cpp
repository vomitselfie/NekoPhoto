#include "BrushDynamicsDialog.h"
#include "BrushLibrary.h"
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

using namespace compositor;

namespace app {

// ---- Curve editor ------------------------------------------------------------------------------------------------

CurveEditor::CurveEditor(QWidget* parent) : QWidget(parent) {
    setMinimumSize(140, 140);
    setMouseTracking(false);
    setCurve({});
}

void CurveEditor::setCurve(const DynamicsCurve& curve) {
    curve_ = curve;
    if (curve_.isIdentity()) curve_.points = {{0, 0}, {1, 1}};
    curve_.normalize();
    // The ends sit on the left and right edges.
    if (curve_.points.front().first > 0) curve_.points.insert(curve_.points.begin(), {0, curve_.points.front().second});
    if (curve_.points.back().first < 1) curve_.points.push_back({1, curve_.points.back().second});
    update();
}

QRectF CurveEditor::plot() const { return QRectF(rect()).adjusted(8, 8, -8, -8); }

QPointF CurveEditor::toView(double x, double y) const {
    const QRectF r = plot();
    return {r.left() + x * r.width(), r.bottom() - y * r.height()};
}

std::pair<double, double> CurveEditor::toUnit(QPointF p) const {
    const QRectF r = plot();
    return {std::clamp((p.x() - r.left()) / r.width(), 0.0, 1.0), std::clamp((r.bottom() - p.y()) / r.height(), 0.0, 1.0)};
}

int CurveEditor::pointAt(QPointF p) const {
    for (size_t i = 0; i < curve_.points.size(); i++) {
        const QPointF v = toView(curve_.points[i].first, curve_.points[i].second);
        if (std::hypot(v.x() - p.x(), v.y() - p.y()) <= 7) return int(i);
    }
    return -1;
}

void CurveEditor::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF r = plot();
    const QPalette& pal = palette();
    painter.fillRect(r, pal.color(QPalette::Base));
    painter.setPen(QPen(pal.color(QPalette::Mid), 1));
    for (int i = 1; i < 4; i++) {
        painter.drawLine(QPointF(r.left() + r.width() * i / 4, r.top()), QPointF(r.left() + r.width() * i / 4, r.bottom()));
        painter.drawLine(QPointF(r.left(), r.top() + r.height() * i / 4), QPointF(r.right(), r.top() + r.height() * i / 4));
    }
    painter.drawRect(r);
    painter.setPen(QPen(pal.color(QPalette::Mid), 1, Qt::DashLine));
    painter.drawLine(toView(0, 0), toView(1, 1));
    QPainterPath path;
    for (int i = 0; i <= 96; i++) {
        const double x = i / 96.0;
        const QPointF v = toView(x, curve_.evaluate(x));
        if (i == 0) path.moveTo(v); else path.lineTo(v);
    }
    painter.setPen(QPen(pal.color(QPalette::Highlight), 2));
    painter.drawPath(path);
    painter.setBrush(pal.color(QPalette::Highlight));
    painter.setPen(QPen(pal.color(QPalette::Base), 1));
    for (const auto& [x, y] : curve_.points) painter.drawEllipse(toView(x, y), 4, 4);
}

void CurveEditor::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    dragging_ = pointAt(e->position());
    if (dragging_ < 0) {
        const auto [x, y] = toUnit(e->position());
        if (x <= 0.01 || x >= 0.99) return;
        auto it = std::lower_bound(curve_.points.begin(), curve_.points.end(), std::make_pair(x, 0.0),
                                   [](const auto& a, const auto& b) { return a.first < b.first; });
        dragging_ = int(it - curve_.points.begin());
        curve_.points.insert(it, {x, y});
        update();
        emit changed();
    }
}

void CurveEditor::mouseMoveEvent(QMouseEvent* e) {
    if (dragging_ < 0 || dragging_ >= int(curve_.points.size())) return;
    auto [x, y] = toUnit(e->position());
    const size_t i = size_t(dragging_);
    // The ends keep their x; an inner point stays between its neighbours.
    if (i == 0) x = 0;
    else if (i + 1 == curve_.points.size()) x = 1;
    else x = std::clamp(x, curve_.points[i - 1].first + 0.01, curve_.points[i + 1].first - 0.01);
    curve_.points[i] = {x, y};
    update();
    emit changed();
}

void CurveEditor::mouseReleaseEvent(QMouseEvent*) { dragging_ = -1; }

void CurveEditor::mouseDoubleClickEvent(QMouseEvent* e) {
    const int i = pointAt(e->position());
    if (i <= 0 || i + 1 >= int(curve_.points.size())) return;
    curve_.points.erase(curve_.points.begin() + i);
    dragging_ = -1;
    update();
    emit changed();
}

// ---- The dialog --------------------------------------------------------------------------------------------------

QString BrushDynamicsDialog::buttonText() { return tr("Dynamics…"); }

QString BrushDynamicsDialog::unavailableText() {
    return tr("Brush dynamics: pen pressure curves and spacing for imported tip brushes. The round tip and the MyPaint presets keep their own.");
}

BrushDynamicsDialog::BrushDynamicsDialog(const BrushTip& tip, const QString& name, QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Brush Dynamics: %1").arg(name));
    auto* layout = new QVBoxLayout(this);
    auto* rows = new QHBoxLayout;
    size_ = addRow(DynamicsTarget::Size, tr("Size follows pen pressure"), tip);
    flow_ = addRow(DynamicsTarget::Flow, tr("Flow follows pen pressure"), tip);
    rows->addWidget(size_.box);
    rows->addWidget(flow_.box);
    layout->addLayout(rows);
    density_ = new QCheckBox(tr("Density by spacing"));
    density_->setToolTip(tr("Spacing leaves the stroke's density alone: closer dabs each lay down less, so the stroke looks the same at any spacing. Off, closer dabs build up more paint."));
    density_->setChecked(tip.densityBySpacing);
    layout->addWidget(density_);
    mouseSpeed_ = new QCheckBox(tr("Mouse speed as pressure (simulated)"));
    mouseSpeed_->setToolTip(tr("For a mouse only: moving slowly presses harder and a quick flick lifts, with a short ramp in at the start. A pen always uses its own pressure."));
    mouseSpeed_->setChecked(tip.mousePressureFromSpeed);
    layout->addWidget(mouseSpeed_);
    const std::optional<double> tiltShape = tiltShapeOf(tip.dynamics);
    auto* tiltRow = new QHBoxLayout;
    tiltShape_ = new QCheckBox(tr("Pen tilt shapes the tip"));
    tiltShape_->setToolTip(tr("Like the side of a pencil: the tip flattens as the pen leans and turns the way it leans."));
    tiltShape_->setChecked(tiltShape.has_value());
    tiltRow->addWidget(tiltShape_);
    tiltRow->addWidget(new QLabel(tr("Flattest")));
    flattest_ = new QDoubleSpinBox;
    flattest_->setRange(1, 100);
    flattest_->setDecimals(0);
    flattest_->setSuffix("%");
    flattest_->setToolTip(tr("The tip's roundness with the pen fully tilted"));
    flattest_->setValue(std::round(tiltShape.value_or(0.3) * 100));
    flattest_->setEnabled(tiltShape_->isChecked());
    connect(tiltShape_, &QCheckBox::toggled, flattest_, &QWidget::setEnabled);
    tiltRow->addWidget(flattest_);
    tiltRow->addStretch();
    layout->addLayout(tiltRow);
    int others = tiltShape ? -2 : 0;
    for (const DynamicsMapping& m : tip.dynamics)
        if (m.input != DynamicsInput::Pressure || (m.target != DynamicsTarget::Size && m.target != DynamicsTarget::Flow)) others++;
    if (others) {
        auto* note = new QLabel(tr("Other dynamics of this brush (%n: jitter, tilt, fade and the like) stay as they are.", nullptr, others));
        note->setWordWrap(true);
        layout->addWidget(note);
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

BrushDynamicsDialog::Row BrushDynamicsDialog::addRow(DynamicsTarget target, const QString& title, const BrushTip& tip) {
    Row row;
    row.box = new QGroupBox(title);
    row.box->setCheckable(true);
    const DynamicsMapping* mapping = findMapping(tip.dynamics, DynamicsInput::Pressure, target);
    row.box->setChecked(mapping != nullptr);
    auto* v = new QVBoxLayout(row.box);
    row.curve = new CurveEditor;
    row.curve->setToolTip(tr("Pressure across, response up. Drag a point to move it, click to add one, double-click to remove it."));
    if (mapping) row.curve->setCurve(mapping->curve);
    v->addWidget(row.curve, 1);
    auto* h = new QHBoxLayout;
    h->addWidget(new QLabel(tr("Minimum")));
    row.minimum = new QDoubleSpinBox;
    row.minimum->setRange(0, 100);
    row.minimum->setDecimals(0);
    row.minimum->setSuffix("%");
    row.minimum->setToolTip(tr("What the lightest touch still gives, as a share of the full value"));
    row.minimum->setValue(mapping ? std::clamp(mapping->minimum(), 0.0, 1.0) * 100 : 0);
    h->addWidget(row.minimum);
    h->addStretch();
    row.smooth = new QCheckBox(tr("Smooth"));
    row.smooth->setToolTip(tr("A smooth curve through the points; off, straight lines between them"));
    row.smooth->setChecked(!mapping || mapping->curve.kind == DynamicsCurve::Kind::Smooth || mapping->curve.isIdentity());
    h->addWidget(row.smooth);
    v->addLayout(h);
    connect(row.smooth, &QCheckBox::toggled, row.curve, [curve = row.curve](bool on) {
        DynamicsCurve c = curve->curve();
        c.kind = on ? DynamicsCurve::Kind::Smooth : DynamicsCurve::Kind::Linear;
        curve->setCurve(c);
    });
    DynamicsCurve c = row.curve->curve();
    c.kind = row.smooth->isChecked() ? DynamicsCurve::Kind::Smooth : DynamicsCurve::Kind::Linear;
    row.curve->setCurve(c);
    return row;
}

void BrushDynamicsDialog::applyRow(const Row& row, DynamicsTarget target, BrushTip& tip) const {
    auto it = std::find_if(tip.dynamics.begin(), tip.dynamics.end(),
                           [&](const DynamicsMapping& m) { return m.input == DynamicsInput::Pressure && m.target == target; });
    if (!row.box->isChecked()) {
        if (it != tip.dynamics.end()) tip.dynamics.erase(it);
        return;
    }
    DynamicsMapping m = it != tip.dynamics.end() ? *it : dynamicsMapping(DynamicsInput::Pressure, target, 0, 1);
    const double minimum = row.minimum->value() / 100;
    m.offset = minimum;
    m.depth = 1 - minimum;
    m.curve = row.curve->curve();
    // A straight diagonal is the identity: stored as no points.
    const bool diagonal = m.curve.points.size() == 2 && m.curve.points[0] == std::make_pair(0.0, 0.0) && m.curve.points[1] == std::make_pair(1.0, 1.0);
    if (diagonal) m.curve.points.clear();
    // Pressure comes first on its target, as it multiplies before any jitter.
    if (it != tip.dynamics.end()) *it = m;
    else tip.dynamics.insert(tip.dynamics.begin(), m);
}

void BrushDynamicsDialog::apply(BrushTip& tip) const {
    applyRow(flow_, DynamicsTarget::Flow, tip);
    applyRow(size_, DynamicsTarget::Size, tip);
    tip.densityBySpacing = density_->isChecked();
    tip.mousePressureFromSpeed = mouseSpeed_->isChecked();
    removeTiltShape(tip.dynamics);
    if (tiltShape_->isChecked())
        for (const DynamicsMapping& m : tiltShapesTip(flattest_->value() / 100)) tip.dynamics.push_back(m);
}

bool BrushDynamicsDialog::edit(QWidget* parent, const QString& presetId) {
    const BrushPreset* preset = BrushLibrary::find(presetId);
    if (!preset || preset->engine != BrushPreset::Engine::Tip) return false;
    auto tip = preset->tip();
    if (!tip) return false;
    BrushDynamicsDialog dialog(tip->tip, preset->name, parent);
    if (dialog.exec() != QDialog::Accepted) return true;
    TipPreset changed = *tip;
    dialog.apply(changed.tip);
    QString error;
    if (!BrushLibrary::saveTip(presetId, changed, &error))
        QMessageBox::warning(parent, tr("Brush Dynamics"), tr("The brush could not be saved: %1").arg(error));
    return true;
}

} // namespace app
