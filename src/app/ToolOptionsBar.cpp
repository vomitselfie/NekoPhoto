#include "ToolOptionsBar.h"
#include "CanvasWidget.h"
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>
#include <cmath>

using namespace compositor;

namespace app {

namespace {

QDoubleSpinBox* numberField(double min, double max, int decimals, const QString& suffix, const QString& tip) {
    auto* f = new QDoubleSpinBox;
    f->setRange(min, max);
    f->setDecimals(decimals);
    f->setSuffix(suffix);
    f->setToolTip(tip);
    f->setKeyboardTracking(false);
    f->setFixedWidth(suffix.isEmpty() ? 80 : 104);
    return f;
}

QWidget* row() {
    auto* w = new QWidget;
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(6, 2, 6, 2);
    h->setSpacing(8);
    return w;
}

QHBoxLayout* layoutOf(QWidget* w) { return static_cast<QHBoxLayout*>(w->layout()); }

} // namespace

ToolOptionsBar::ToolOptionsBar(EditorSession* session, CanvasWidget* canvas, QWidget* parent)
    : QToolBar(tr("Tool Options"), parent), session_(session), canvas_(canvas) {
    setMovable(false);
    setFloatable(false);
    stack_ = new QStackedWidget;
    stack_->addWidget(buildMoveOptions());       // 0
    stack_->addWidget(buildBrushOptions());      // 1
    stack_->addWidget(buildMarqueeOptions());    // 2
    stack_->addWidget(buildLassoOptions());      // 3
    stack_->addWidget(buildWandOptions());       // 4
    stack_->addWidget(buildCropOptions());       // 5
    stack_->addWidget(buildZoomOptions());       // 6
    stack_->addWidget(buildEyedropperOptions()); // 7
    stack_->addWidget(buildHealingOptions());    // 8
    stack_->addWidget(buildCloneOptions());      // 9
    addWidget(stack_);
    connect(session_, &EditorSession::toolChanged, this, &ToolOptionsBar::syncTool);
    connect(session_, &EditorSession::transformChanged, this, &ToolOptionsBar::syncTransformFields);
    connect(session_, &EditorSession::layersChanged, this, &ToolOptionsBar::syncTransformFields);
    syncTool();
}

void ToolOptionsBar::syncTool() {
    int index = 0;
    switch (session_->tool()) {
    case Tool::Move: index = 0; break;
    case Tool::Brush: index = 1; break;
    case Tool::Marquee: index = 2; break;
    case Tool::Lasso: index = 3; break;
    case Tool::Wand: index = 4; break;
    case Tool::Crop: index = 5; break;
    case Tool::Zoom: case Tool::Hand: index = 6; break;
    case Tool::Eyedropper: index = 7; break;
    case Tool::SpotHealing: index = 8; break;
    case Tool::CloneStamp: index = 9; break;
    }
    stack_->setCurrentIndex(index);
    // Widgets that mirror session state.
    for (int page : {1, 8, 9})
    for (auto* spin : stack_->widget(page)->findChildren<QDoubleSpinBox*>()) {
        QSignalBlocker b(spin);
        QString role = spin->property("role").toString();
        if (role == "size") spin->setValue(session_->brushSettings.diameter);
        else if (role == "hardness") spin->setValue(session_->brushSettings.hardness * 100);
        else if (role == "opacity") spin->setValue(session_->brushSettings.opacity * 100);
    }
    for (auto* b : stack_->widget(1)->findChildren<QToolButton*>()) {
        QString role = b->property("role").toString();
        if (role == "paint") b->setChecked(!session_->brushErase);
        else if (role == "erase") b->setChecked(session_->brushErase);
    }
    syncTransformFields();
}

QWidget* ToolOptionsBar::buildMoveOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    auto* autoSelect = new QCheckBox(tr("Auto-Select"));
    autoSelect->setToolTip(tr("Click picks the layer under the pointer (or hold Ctrl)"));
    autoSelect->setChecked(session_->transformAutoSelect);
    connect(autoSelect, &QCheckBox::toggled, this, [this](bool on) { session_->transformAutoSelect = on; });
    h->addWidget(autoSelect);
    auto* controls = new QCheckBox(tr("Transform Controls"));
    controls->setChecked(session_->showsTransformControls);
    connect(controls, &QCheckBox::toggled, this, [this](bool on) { session_->showsTransformControls = on; emit session_->transformChanged(); });
    h->addWidget(controls);
    auto* lock = new QCheckBox(tr("Keep Ratio"));
    lock->setToolTip(tr("Corner handles keep the proportions (Shift reverses)"));
    lock->setChecked(session_->locksTransformRatio);
    connect(lock, &QCheckBox::toggled, this, [this](bool on) { session_->locksTransformRatio = on; });
    h->addWidget(lock);

    transformFields_ = new QWidget;
    auto* fields = new QHBoxLayout(transformFields_);
    fields->setContentsMargins(12, 0, 0, 0);
    fields->setSpacing(4);
    auto add = [&](const QString& label, QDoubleSpinBox* f) { fields->addWidget(new QLabel(label)); fields->addWidget(f); };
    xField_ = numberField(-1000000, 1000000, 1, " px", tr("Left"));
    yField_ = numberField(-1000000, 1000000, 1, " px", tr("Top"));
    wField_ = numberField(1, 300000, 1, " px", tr("Width"));
    hField_ = numberField(1, 300000, 1, " px", tr("Height"));
    wPercent_ = numberField(0.01, 100000, 2, "%", tr("Width as a percentage of the pixels"));
    hPercent_ = numberField(0.01, 100000, 2, "%", tr("Height as a percentage of the pixels"));
    angleField_ = numberField(-100000, 100000, 1, "°", tr("Rotation, clockwise"));
    add("X", xField_); add("Y", yField_); add("W", wField_); add("H", hField_); add("", wPercent_); add("", hPercent_); add(tr("Angle"), angleField_);
    for (auto* f : {xField_, yField_, wField_, hField_, wPercent_, hPercent_, angleField_})
        connect(f, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, &ToolOptionsBar::applyTransformField);
    auto* apply = new QPushButton(tr("Apply"));
    apply->setToolTip(tr("Apply the pending transform (Enter)"));
    connect(apply, &QPushButton::clicked, this, [this] { session_->commitTransform(); });
    auto* cancel = new QPushButton(tr("Cancel"));
    connect(cancel, &QPushButton::clicked, this, [this] { session_->cancelTransform(); });
    fields->addWidget(apply);
    fields->addWidget(cancel);
    apply->setProperty("pending", true);
    cancel->setProperty("pending", true);
    h->addWidget(transformFields_);
    h->addStretch();
    return w;
}

void ToolOptionsBar::syncTransformFields() {
    const Layer* active = session_->activeLayer();
    bool enabled = active && !active->isGroup && session_->canTransform();
    transformFields_->setEnabled(enabled);
    for (auto* b : transformFields_->findChildren<QPushButton*>()) b->setVisible(session_->transformEdit().has_value());
    if (!enabled) return;
    syncingFields_ = true;
    LayerTransform t = session_->editedTransform(*active);
    auto pixels = session_->transformPixelSize();
    xField_->setValue(t.origin.x);
    yField_->setValue(t.origin.y);
    wField_->setValue(t.size.width);
    hField_->setValue(t.size.height);
    wPercent_->setEnabled(pixels.has_value());
    hPercent_->setEnabled(pixels.has_value());
    if (pixels) { wPercent_->setValue(t.size.width / std::max(1.0, pixels->width) * 100); hPercent_->setValue(t.size.height / std::max(1.0, pixels->height) * 100); }
    angleField_->setValue(t.rotation);
    syncingFields_ = false;
}

void ToolOptionsBar::applyTransformField() {
    if (syncingFields_) return;
    const Layer* active = session_->activeLayer();
    if (!active || !session_->canTransform()) return;
    if (!session_->transformEdit()) session_->beginTransform(true);
    if (!session_->transformEdit()) return;
    LayerTransform t = session_->transformEdit()->draft;
    auto* sender = qobject_cast<QDoubleSpinBox*>(QObject::sender());
    auto pixels = session_->transformPixelSize();
    Point center = t.center();
    if (sender == wPercent_ && pixels) {
        double w = pixels->width * wPercent_->value() / 100;
        if (session_->locksTransformRatio) { double h = pixels->height * wPercent_->value() / 100; t.size = {w, h}; } else t.size.width = w;
        t.origin = {center.x - t.size.width / 2, center.y - t.size.height / 2};
    } else if (sender == hPercent_ && pixels) {
        double h = pixels->height * hPercent_->value() / 100;
        if (session_->locksTransformRatio) { double w = pixels->width * hPercent_->value() / 100; t.size = {w, h}; } else t.size.height = h;
        t.origin = {center.x - t.size.width / 2, center.y - t.size.height / 2};
    } else if (sender == wField_) {
        double w = wField_->value();
        if (session_->locksTransformRatio) t.size.height = std::max(1.0, t.size.height * w / t.size.width);
        t.size.width = w;
    } else if (sender == hField_) {
        double h = hField_->value();
        if (session_->locksTransformRatio) t.size.width = std::max(1.0, t.size.width * h / t.size.height);
        t.size.height = h;
    } else {
        t.origin = {xField_->value(), yField_->value()};
        t.rotation = angleField_->value();
    }
    session_->previewTransform(t);
}

QWidget* ToolOptionsBar::buildBrushOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    auto* paint = new QToolButton;
    paint->setText(tr("Paint"));
    paint->setCheckable(true);
    paint->setProperty("role", "paint");
    auto* erase = new QToolButton;
    erase->setText(tr("Erase"));
    erase->setCheckable(true);
    erase->setProperty("role", "erase");
    auto* group = new QButtonGroup(w);
    group->addButton(paint);
    group->addButton(erase);
    group->setExclusive(true);
    paint->setChecked(true);
    connect(paint, &QToolButton::toggled, this, [this](bool on) { session_->brushErase = !on; emit session_->toolChanged(); });
    h->addWidget(paint);
    h->addWidget(erase);
    auto spin = [&](const QString& label, const QString& role, double min, double max, double value, const QString& suffix, auto apply) {
        h->addWidget(new QLabel(label));
        auto* f = numberField(min, max, 0, suffix, label);
        f->setProperty("role", role);
        f->setValue(value);
        connect(f, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, apply](double v) { apply(v); emit session_->toolChanged(); });
        h->addWidget(f);
    };
    spin(tr("Size"), "size", 1, 2000, session_->brushSettings.diameter, " px", [this](double v) { session_->brushSettings.diameter = v; });
    spin(tr("Hardness"), "hardness", 0, 100, session_->brushSettings.hardness * 100, "%", [this](double v) { session_->brushSettings.hardness = v / 100; });
    spin(tr("Opacity"), "opacity", 1, 100, session_->brushSettings.opacity * 100, "%", [this](double v) { session_->brushSettings.opacity = v / 100; });
    auto* hint = new QLabel(tr("[ and ] change the size; Shift-click paints a straight line"));
    hint->setStyleSheet("color: palette(mid);");
    h->addWidget(hint);
    h->addStretch();
    return w;
}

QWidget* ToolOptionsBar::buildHealingOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    auto* mode = new QComboBox;
    mode->addItems({tr("Content-Aware"), tr("Create Texture"), tr("Proximity Match")});
    connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { session_->spotHealingMode = i; });
    h->addWidget(new QLabel(tr("Type")));
    h->addWidget(mode);
    addBrushTipFields(h);
    h->addStretch();
    return w;
}

QWidget* ToolOptionsBar::buildCloneOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    auto* aligned = new QCheckBox(tr("Aligned"));
    aligned->setToolTip(tr("The source moves with the brush and keeps its offset between strokes"));
    aligned->setChecked(session_->cloneAligned);
    connect(aligned, &QCheckBox::toggled, this, [this](bool on) { session_->cloneAligned = on; });
    h->addWidget(aligned);
    auto* all = new QCheckBox(tr("Sample All Layers"));
    all->setChecked(session_->cloneSampleAll);
    connect(all, &QCheckBox::toggled, this, [this](bool on) { session_->cloneSampleAll = on; });
    h->addWidget(all);
    addBrushTipFields(h);
    auto* hint = new QLabel(tr("Alt-click sets the source"));
    hint->setStyleSheet("color: palette(mid);");
    h->addWidget(hint);
    h->addStretch();
    return w;
}

void ToolOptionsBar::addBrushTipFields(QHBoxLayout* h) {
    auto spin = [&](const QString& label, const QString& role, double min, double max, double value, const QString& suffix, auto apply) {
        h->addWidget(new QLabel(label));
        auto* f = numberField(min, max, 0, suffix, label);
        f->setProperty("role", role);
        f->setValue(value);
        connect(f, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, apply](double v) { apply(v); emit session_->toolChanged(); });
        h->addWidget(f);
    };
    spin(tr("Size"), "size", 1, 2000, session_->brushSettings.diameter, " px", [this](double v) { session_->brushSettings.diameter = v; });
    spin(tr("Hardness"), "hardness", 0, 100, session_->brushSettings.hardness * 100, "%", [this](double v) { session_->brushSettings.hardness = v / 100; });
    spin(tr("Opacity"), "opacity", 1, 100, session_->brushSettings.opacity * 100, "%", [this](double v) { session_->brushSettings.opacity = v / 100; });
}

QWidget* ToolOptionsBar::buildMarqueeOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    auto* kind = new QComboBox;
    kind->addItems({tr("Rectangle"), tr("Ellipse")});
    connect(kind, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { session_->marqueeKind = i == 1 ? MarqueeKind::Ellipse : MarqueeKind::Rectangle; });
    h->addWidget(kind);
    auto* aa = new QCheckBox(tr("Anti-alias"));
    aa->setChecked(session_->selectionAntialiased);
    connect(aa, &QCheckBox::toggled, this, [this](bool on) { session_->selectionAntialiased = on; });
    h->addWidget(aa);
    auto* hint = new QLabel(tr("Shift adds, Alt subtracts; drag inside a selection to move its outline"));
    hint->setStyleSheet("color: palette(mid);");
    h->addWidget(hint);
    h->addStretch();
    return w;
}

QWidget* ToolOptionsBar::buildLassoOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    auto* kind = new QComboBox;
    kind->addItems({tr("Freehand"), tr("Polygonal")});
    connect(kind, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { canvas_->setProperty("polygonal", i == 1); emit session_->toolChanged(); });
    kind->setProperty("role", "lassoKind");
    h->addWidget(kind);
    auto* hint = new QLabel(tr("Polygonal: click to add points, double-click or Enter to close, Backspace removes the last point"));
    hint->setStyleSheet("color: palette(mid);");
    h->addWidget(hint);
    h->addStretch();
    return w;
}

QWidget* ToolOptionsBar::buildWandOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    h->addWidget(new QLabel(tr("Tolerance")));
    auto* tol = new QSpinBox;
    tol->setRange(0, 255);
    tol->setValue(session_->wandTolerance);
    connect(tol, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) { session_->wandTolerance = v; });
    h->addWidget(tol);
    auto* contiguous = new QCheckBox(tr("Contiguous"));
    contiguous->setChecked(session_->wandContiguous);
    connect(contiguous, &QCheckBox::toggled, this, [this](bool on) { session_->wandContiguous = on; });
    h->addWidget(contiguous);
    auto* all = new QCheckBox(tr("Sample All Layers"));
    all->setChecked(session_->wandSampleAll);
    connect(all, &QCheckBox::toggled, this, [this](bool on) { session_->wandSampleAll = on; });
    h->addWidget(all);
    h->addStretch();
    return w;
}

QWidget* ToolOptionsBar::buildCropOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    auto* hint = new QLabel(tr("Drag the crop, then press Enter or double-click; Shift squares, Alt grows from the centre"));
    hint->setStyleSheet("color: palette(mid);");
    h->addWidget(hint);
    auto* apply = new QPushButton(tr("Crop"));
    connect(apply, &QPushButton::clicked, this, [this] { canvas_->applyCrop(); });
    auto* cancel = new QPushButton(tr("Cancel"));
    connect(cancel, &QPushButton::clicked, this, [this] { canvas_->cancelCrop(); });
    h->addWidget(apply);
    h->addWidget(cancel);
    h->addStretch();
    return w;
}

QWidget* ToolOptionsBar::buildZoomOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    auto* fit = new QPushButton(tr("Fit"));
    connect(fit, &QPushButton::clicked, this, [this] { session_->fitView(); });
    auto* actual = new QPushButton(tr("100%"));
    connect(actual, &QPushButton::clicked, this, [this] { session_->zoomTo(1); });
    h->addWidget(fit);
    h->addWidget(actual);
    auto* hint = new QLabel(tr("Click zooms in, Alt-click out, drag a box to zoom to it; Ctrl-wheel zooms anywhere"));
    hint->setStyleSheet("color: palette(mid);");
    h->addWidget(hint);
    h->addStretch();
    return w;
}

QWidget* ToolOptionsBar::buildEyedropperOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    auto* hint = new QLabel(tr("Click sets the foreground colour, Alt-click the background"));
    hint->setStyleSheet("color: palette(mid);");
    h->addWidget(hint);
    h->addStretch();
    return w;
}

} // namespace app
