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
    return tr("Smoothing for every brush; pen pressure curves and spacing for imported tip brushes (the round tip and the MyPaint presets keep their own).");
}

QString BrushDynamicsDialog::inputLabel(DynamicsInput input) {
    switch (input) {
    case DynamicsInput::Pressure: return tr("Pressure");
    case DynamicsInput::Speed: return tr("Speed (in the document)");
    case DynamicsInput::ScreenSpeed: return tr("Speed (on screen)");
    case DynamicsInput::Tilt: return tr("Tilt");
    case DynamicsInput::TiltDirection: return tr("Tilt direction");
    case DynamicsInput::Twist: return tr("Barrel rotation");
    case DynamicsInput::Random: return tr("Random");
    case DynamicsInput::StrokeProgress: return tr("Fade");
    case DynamicsInput::Roll: return tr("Roll");
    case DynamicsInput::StrokeRandom: return tr("Random per stroke");
    case DynamicsInput::InitialDirection: return tr("Initial direction");
    case DynamicsInput::Wheel: return tr("Stylus wheel");
    }
    return {};
}

QString BrushDynamicsDialog::targetLabel(DynamicsTarget target) {
    switch (target) {
    case DynamicsTarget::Size: return tr("Size");
    case DynamicsTarget::Flow: return tr("Flow");
    case DynamicsTarget::Opacity: return tr("Opacity");
    case DynamicsTarget::Angle: return tr("Angle");
    case DynamicsTarget::Roundness: return tr("Roundness");
    case DynamicsTarget::Spacing: return tr("Spacing");
    case DynamicsTarget::Scatter: return tr("Scatter");
    case DynamicsTarget::GrainDepth: return tr("Grain depth");
    case DynamicsTarget::GrainRotation: return tr("Grain rotation");
    }
    return {};
}

BrushDynamicsDialog::BrushDynamicsDialog(const BrushTip* tipOrNull, const QString& name, const BrushSmoothing* smoothing, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(name.isEmpty() ? tr("Brush Dynamics") : tr("Brush Dynamics: %1").arg(name));
    auto* layout = new QVBoxLayout(this);
    if (smoothing) {
        // Photoshop's Smoothing options, and the two filters before it.
        auto* box = new QGroupBox(tr("Smoothing"));
        auto* grid = new QVBoxLayout(box);
        auto percent = [](double value, const QString& tip) {
            auto* f = new QDoubleSpinBox;
            f->setRange(0, 100);
            f->setDecimals(0);
            f->setSuffix("%");
            f->setValue(value);
            f->setToolTip(tip);
            return f;
        };
        auto* amountRow = new QHBoxLayout;
        amountRow->addWidget(new QLabel(tr("Smoothing")));
        stabilizer_ = percent(smoothing->stabilizer, tr("The line trails the pen and evens out its wobble: the higher, the steadier and the further behind"));
        amountRow->addWidget(stabilizer_);
        amountRow->addStretch();
        grid->addLayout(amountRow);
        pulledString_ = new QCheckBox(tr("Pulled String Mode"));
        pulledString_->setToolTip(tr("Paints only when the string is taut: moving the pen within the smoothing radius leaves no mark"));
        pulledString_->setChecked(smoothing->pulledString);
        strokeCatchUp_ = new QCheckBox(tr("Stroke Catch-Up"));
        strokeCatchUp_->setToolTip(tr("The paint keeps catching up with the pen while you pause; off, it stops as soon as the pen stops"));
        strokeCatchUp_->setChecked(smoothing->strokeCatchUp);
        catchUpOnEnd_ = new QCheckBox(tr("Catch-Up On Stroke End"));
        catchUpOnEnd_->setToolTip(tr("Completes the stroke from the last paint position to where you released the pen"));
        catchUpOnEnd_->setChecked(smoothing->catchUpOnEnd);
        adjustForZoom_ = new QCheckBox(tr("Adjust For Zoom"));
        adjustForZoom_->setToolTip(tr("Less smoothing when zoomed in, more when zoomed out, so it feels the same on screen"));
        adjustForZoom_->setChecked(smoothing->adjustForZoom);
        for (QCheckBox* c : {pulledString_, strokeCatchUp_, catchUpOnEnd_, adjustForZoom_}) grid->addWidget(c);
        auto sync = [this] { strokeCatchUp_->setEnabled(!pulledString_->isChecked()); };
        connect(pulledString_, &QCheckBox::toggled, this, sync);
        sync();
        auto* filters = new QHBoxLayout;
        filters->addWidget(new QLabel(tr("Input smoothing")));
        inputSmoothing_ = percent(smoothing->input, tr("Steadies a jittery tablet with little lag: a slow pen is held still, a fast one followed closely"));
        filters->addWidget(inputSmoothing_);
        filters->addWidget(new QLabel(tr("Pressure smoothing")));
        pressureSmoothing_ = percent(smoothing->pressure, tr("Evens out uneven pressure without slowing the line"));
        filters->addWidget(pressureSmoothing_);
        filters->addStretch();
        grid->addLayout(filters);
        layout->addWidget(box);
    }
    if (!tipOrNull) {
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        layout->addWidget(buttons);
        return;
    }
    const BrushTip& tip = *tipOrNull;
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
    int others = 0;
    QStringList listed;
    BrushDynamics rest = tip.dynamics;   // without the pencil's pair, which the checkbox above shows
    removeTiltShape(rest);
    for (const DynamicsMapping& m : rest)
        if (m.input != DynamicsInput::Pressure || (m.target != DynamicsTarget::Size && m.target != DynamicsTarget::Flow)) {
            others++;
            // Mappings the engine never applies (static grain, no speed on deposition: docs/legal-boundaries.md) say so.
            const QString line = mappingAllowed(m.target, m.input) ? tr("%1 → %2").arg(inputLabel(m.input), targetLabel(m.target))
                                                                   : tr("%1 → %2 (not applied)").arg(inputLabel(m.input), targetLabel(m.target));
            if (!listed.contains(line)) listed.push_back(line);
        }
    if (others) {
        auto* note = new QLabel(tr("Other dynamics of this brush (%n: jitter, tilt, fade and the like) stay as they are.", nullptr, others));
        note->setWordWrap(true);
        layout->addWidget(note);
        // Each by name, so a brush that follows the speed says which: on the screen or in the document.
        auto* list = new QLabel(listed.join(QStringLiteral(", ")));
        list->setWordWrap(true);
        list->setEnabled(false);
        layout->addWidget(list);
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

void BrushDynamicsDialog::applySmoothing(BrushSmoothing& smoothing) const {
    if (!stabilizer_) return;
    smoothing.stabilizer = stabilizer_->value();
    smoothing.pulledString = pulledString_->isChecked();
    smoothing.strokeCatchUp = strokeCatchUp_->isChecked();
    smoothing.catchUpOnEnd = catchUpOnEnd_->isChecked();
    smoothing.adjustForZoom = adjustForZoom_->isChecked();
    smoothing.input = inputSmoothing_->value();
    smoothing.pressure = pressureSmoothing_->value();
}

bool BrushDynamicsDialog::edit(QWidget* parent, const QString& presetId, BrushSmoothing* smoothing) {
    const BrushPreset* preset = presetId.isEmpty() ? nullptr : BrushLibrary::find(presetId);
    std::shared_ptr<const TipPreset> tip;
    if (preset && preset->engine == BrushPreset::Engine::Tip) tip = preset->tip();
    if (!tip && !smoothing) return false;
    BrushDynamicsDialog dialog(tip ? &tip->tip : nullptr, tip ? preset->name : QString(), smoothing, parent);
    if (dialog.exec() != QDialog::Accepted) return true;
    if (smoothing) dialog.applySmoothing(*smoothing);
    if (!tip) return true;
    TipPreset changed = *tip;
    dialog.apply(changed.tip);
    QString error;
    if (!BrushLibrary::saveTip(presetId, changed, &error))
        QMessageBox::warning(parent, tr("Brush Dynamics"), tr("The brush could not be saved: %1").arg(error));
    return true;
}

} // namespace app
