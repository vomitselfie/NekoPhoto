#include "Style.h"
#include "ToolOptionsBar.h"
#include <QColorDialog>
#include "TextLayer.h"
#include "BrushImporter.h"
#include "BrushDynamicsDialog.h"
#include "BrushLibrary.h"
#include "PresetLibrary.h"
#include "BrushPicker.h"
#include "FontPicker.h"
#include "CanvasWidget.h"
#include "Icons.h"
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QMessageBox>
#include "ModelStore.h"
#include <QResizeEvent>
#include <QSpinBox>
#include <QToolButton>
#include <algorithm>
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
    f->setButtonSymbols(QAbstractSpinBox::NoButtons);
    f->setAlignment(Qt::AlignRight);
    f->setFixedWidth(suffix.isEmpty() ? 64 : 82);
    return f;
}

/// A thin vertical rule between groups of controls.
QWidget* separator() {
    auto* line = new QWidget;
    line->setFixedSize(1, 18);
    line->setStyleSheet(QStringLiteral("background: %1;").arg(hintColor(3).name()));
    return line;
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
    // One bar per tab; unique names keep QMainWindow::saveState quiet (their state is trivial, they can't move).
    static int count = 0;
    setObjectName(QStringLiteral("toolOptions%1").arg(++count));
    setMovable(false);
    setFloatable(false);
    stack_ = new QStackedWidget;
    stack_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
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
    stack_->addWidget(buildSmudgeOptions());     // 10
    stack_->addWidget(buildGradientOptions());   // 11
    stack_->addWidget(buildShapeOptions());      // 12
    stack_->addWidget(buildTextOptions());       // 13
    stack_->addWidget(buildScribbleOptions());   // 14
    stack_->addWidget(buildToningOptions());     // 15
    stack_->addWidget(buildBucketOptions());     // 16
    stack_->addWidget(buildPenOptions());        // 17
    boxOptionsPage_ = stack_->count();
    stack_->addWidget(buildBoxOptions());        // the Artboard and Slice tools
    addWidget(stack_);
    connect(session_, &EditorSession::toolChanged, this, &ToolOptionsBar::syncTool);
    connect(session_, &EditorSession::transformChanged, this, &ToolOptionsBar::syncTransformFields);
    connect(session_, &EditorSession::layersChanged, this, &ToolOptionsBar::syncTransformFields);
    connect(session_, &EditorSession::layersChanged, this, [this] { for (auto& s : syncers_) s(); });
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
    case Tool::Scribble: index = 14; break;
    case Tool::Crop: index = 5; break;
    case Tool::Zoom: case Tool::Hand: index = 6; break;
    case Tool::Eyedropper: index = 7; break;
    case Tool::SpotHealing: index = 8; break;
    case Tool::CloneStamp: index = 9; break;
    case Tool::Smudge: index = 10; break;
    case Tool::Gradient: index = 11; break;
    case Tool::Shape: index = 12; break;
    case Tool::Text: index = 13; break;
    case Tool::Dodge: index = 15; break;
    case Tool::PaintBucket: index = 16; break;
    case Tool::Pen: case Tool::DirectSelect: index = 17; break;
    case Tool::Artboard: case Tool::Slice: index = boxOptionsPage_; break;
    }
    for (auto& s : syncers_) s();
    stack_->setCurrentIndex(index);
    // Widgets that mirror session state.
    for (int page : {1, 8, 9, 10, 15, 16})
    for (auto* spin : stack_->widget(page)->findChildren<QDoubleSpinBox*>()) {
        QSignalBlocker b(spin);
        QString role = spin->property("role").toString();
        if (role == "size") spin->setValue(session_->brushSettings.diameter);
        else if (role == "hardness") spin->setValue(session_->brushSettings.hardness * 100);
        else if (role == "opacity") spin->setValue(session_->brushSettings.opacity * 100);
        else if (role == "smoothing") spin->setValue(session_->brushSmoothing.stabilizer);
    }
    // A MyPaint preset brings its own hardness.
    for (auto* spin : stack_->widget(1)->findChildren<QDoubleSpinBox*>())
        if (spin->property("role").toString() == "hardness") spin->setEnabled(session_->brushPreset.isEmpty());
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
    controlsCheck_ = controls;
    controls->setChecked(session_->showsTransformControls);
    connect(controls, &QCheckBox::toggled, this, [this](bool on) { session_->showsTransformControls = on; emit session_->transformChanged(); });
    h->addWidget(controls);
    auto* lock = new QCheckBox(tr("Keep Ratio"));
    ratioCheck_ = lock;
    lock->setToolTip(tr("Corner handles keep the proportions (Shift reverses)"));
    lock->setChecked(session_->locksTransformRatio);
    connect(lock, &QCheckBox::toggled, this, [this](bool on) { session_->locksTransformRatio = on; });
    h->addWidget(lock);
    h->addWidget(separator());

    transformFields_ = new QWidget;
    auto* fields = new QHBoxLayout(transformFields_);
    fields->setContentsMargins(0, 0, 0, 0);
    fields->setSpacing(4);
    auto add = [&](const QString& label, QDoubleSpinBox* f) {
        if (!label.isEmpty()) { auto* l = new QLabel(label); l->setStyleSheet(hintStyle()); fields->addSpacing(6); fields->addWidget(l); }
        fields->addWidget(f);
    };
    xField_ = numberField(-1000000, 1000000, 1, " px", tr("Left"));
    yField_ = numberField(-1000000, 1000000, 1, " px", tr("Top"));
    wField_ = numberField(1, 300000, 1, " px", tr("Width"));
    hField_ = numberField(1, 300000, 1, " px", tr("Height"));
    wPercent_ = numberField(0.01, 100000, 2, "%", tr("Width as a percentage of the pixels"));
    hPercent_ = numberField(0.01, 100000, 2, "%", tr("Height as a percentage of the pixels"));
    angleField_ = numberField(-100000, 100000, 1, "°", tr("Rotation, clockwise"));
    add("X", xField_); add("Y", yField_); add("W", wField_); add("H", hField_); add("", wPercent_); add("", hPercent_); add(tr("Angle"), angleField_);
    fields->addSpacing(6);
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

void ToolOptionsBar::resizeEvent(QResizeEvent* e) {
    QToolBar::resizeEvent(e);
    int level = width() >= 1150 ? 0 : width() >= 900 ? 1 : 2;
    if (level != compact_) { compact_ = level; applyCompact(); }
}

void ToolOptionsBar::applyCompact() {
    wPercent_->setVisible(compact_ == 0);
    hPercent_->setVisible(compact_ == 0);
    transformFields_->setVisible(compact_ < 2);
    if (controlsCheck_) controlsCheck_->setText(compact_ ? tr("Controls") : tr("Transform Controls"));
    if (ratioCheck_) ratioCheck_->setText(compact_ ? tr("Ratio") : tr("Keep Ratio"));
}

void ToolOptionsBar::syncTransformFields() {
    const Layer* active = session_->activeLayer();
    bool enabled = active && session_->canTransform();
    transformFields_->setEnabled(enabled);
    for (auto* b : transformFields_->findChildren<QPushButton*>()) b->setVisible(session_->transformEdit().has_value());
    // A layer that cannot be transformed right now (hidden, say) still shows where and how big it is.
    if (!active || active->isGroup) return;
    syncingFields_ = true;
    LayerTransform t = session_->editedTransform(*active);
    auto pixels = session_->transformPixelSize();
    if (!pixels && !enabled && active->asset && active->asset->image) pixels = Size(active->asset->image.width(), active->asset->image.height());
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
    auto* picker = new BrushPicker;
    picker->setPreset(session_->brushPreset);
    connect(picker, &BrushPicker::presetChosen, this, [this](const QString& id) {
        session_->brushPreset = id;
        // A preset starts at its own size; Size then scales it like any brush.
        if (const BrushPreset* preset = BrushLibrary::find(id)) session_->brushSettings.diameter = preset->diameter;
        emit session_->toolChanged();
    });
    connect(picker, &BrushPicker::importRequested, this, [this] { importBrushesInteractively(window(), session_); });
    syncers_.push_back([this, picker] {
        if (picker->preset() != session_->brushPreset) picker->setPreset(session_->brushPreset);
    });
    h->addWidget(picker);
    h->addWidget(separator());
    auto* paint = new QToolButton;
    paint->setText(tr("Paint"));
    paint->setIcon(toolIcon("paintbrush", 16));
    paint->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    paint->setCheckable(true);
    paint->setAutoRaise(true);
    paint->setProperty("role", "paint");
    auto* erase = new QToolButton;
    erase->setText(tr("Erase"));
    erase->setIcon(toolIcon("eraser", 16));
    erase->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    erase->setCheckable(true);
    erase->setAutoRaise(true);
    erase->setProperty("role", "erase");
    auto* group = new QButtonGroup(w);
    group->addButton(paint);
    group->addButton(erase);
    group->setExclusive(true);
    paint->setChecked(true);
    connect(paint, &QToolButton::toggled, this, [this](bool on) { if (session_->brushErase == on) { session_->brushErase = !on; emit session_->toolChanged(); } });
    h->addWidget(paint);
    h->addWidget(erase);
    h->addWidget(separator());
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
    // Photoshop's Smoothing: the stabiliser's amount; its modes and the other two filters are in Dynamics.
    spin(tr("Smoothing"), "smoothing", 0, 100, session_->brushSmoothing.stabilizer, "%", [this](double v) { session_->brushSmoothing.stabilizer = v; });
    // The smoothing options, and a tip brush's pressure curves, density and mouse options.
    auto* dynamics = new QToolButton;
    dynamics->setText(BrushDynamicsDialog::buttonText());
    dynamics->setAutoRaise(true);
    dynamics->setToolTip(BrushDynamicsDialog::unavailableText());
    connect(dynamics, &QToolButton::clicked, this, [this] {
        BrushDynamicsDialog::edit(window(), session_->brushPreset, &session_->brushSmoothing);
        emit session_->toolChanged();
    });
    h->addWidget(dynamics);
    h->addStretch();
    return w;
}

QWidget* ToolOptionsBar::buildHealingOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    auto* mode = new QComboBox;
    mode->addItems({tr("Content-Aware"), tr("Create Texture"), tr("Proximity Match"), tr("Sampled (Healing Brush)"), tr("Patch"), tr("Content-Aware Move")});
    mode->setToolTip(tr("Sampled heals from where you Alt-click, like Clone Stamp; Patch: drag the selection to the area to copy from; "
                        "Content-Aware Move: drag the selection (or lasso one) to where it should go"));
    mode->setCurrentIndex(std::clamp(session_->spotHealingMode, 0, 5));
    h->addWidget(new QLabel(tr("Type")));
    h->addWidget(mode);
    // Content-Aware Move's own options, shown while it is the type.
    auto* moveOptions = new QWidget;
    auto* mh = new QHBoxLayout(moveOptions);
    mh->setContentsMargins(0, 0, 0, 0);
    auto* moveMode = new QComboBox;
    moveMode->addItems({tr("Move"), tr("Extend")});
    moveMode->setToolTip(tr("Move fills where the selection was; Extend leaves it and adds a copy"));
    moveMode->setCurrentIndex(session_->contentMoveExtend ? 1 : 0);
    connect(moveMode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { session_->contentMoveExtend = i == 1; });
    auto* adaptation = new QComboBox;
    adaptation->addItems({tr("Very Strict"), tr("Strict"), tr("Medium"), tr("Loose"), tr("Very Loose")});
    adaptation->setToolTip(tr("How far the moved patch adapts to its new place: its tone, and how wide a seam is blended"));
    adaptation->setCurrentIndex(std::clamp(session_->contentMoveAdaptation, 0, 4));
    connect(adaptation, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { session_->contentMoveAdaptation = i; });
    mh->addWidget(new QLabel(tr("Mode")));
    mh->addWidget(moveMode);
    mh->addWidget(new QLabel(tr("Adaptation")));
    mh->addWidget(adaptation);
    h->addWidget(moveOptions);
    moveOptions->setVisible(session_->spotHealingMode == 5);
    connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, moveOptions](int i) { session_->spotHealingMode = i; moveOptions->setVisible(i == 5); });
    syncers_.push_back([this, mode] { if (mode->currentIndex() != session_->spotHealingMode) mode->setCurrentIndex(std::clamp(session_->spotHealingMode, 0, 5)); });
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
    auto* edgeAware = new QCheckBox(tr("Edge Aware"));
    edgeAware->setToolTip(tr("Follow the image: shading and texture stay in, edges between similar colours hold. "
                             "Change the tolerance right after a click to adjust that selection."));
    edgeAware->setChecked(session_->wandEdgeAware);
    connect(edgeAware, &QCheckBox::toggled, this, [this](bool on) { session_->wandEdgeAware = on; });
    h->addWidget(edgeAware);
    auto* refine = new QCheckBox(tr("Refine Edge"));
    refine->setToolTip(tr("Unmix the edge: the fringe of a line is partly selected, and Delete right after leaves the line its own colour, without a rim of the background's"));
    refine->setChecked(session_->wandRefineEdge);
    connect(refine, &QCheckBox::toggled, this, [this](bool on) { session_->wandRefineEdge = on; });
    h->addWidget(refine);
    auto* all = new QCheckBox(tr("Sample All Layers"));
    all->setChecked(session_->cloneSampleAll);
    connect(all, &QCheckBox::toggled, this, [this](bool on) { session_->cloneSampleAll = on; });
    h->addWidget(all);
    addBrushTipFields(h);
    h->addStretch();
    return w;
}

void ToolOptionsBar::addBrushTipFields(QHBoxLayout* h, const QString& strength) {
    h->addWidget(separator());
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
    spin(strength.isEmpty() ? tr("Opacity") : strength, "opacity", 1, 100, session_->brushSettings.opacity * 100, "%", [this](double v) { session_->brushSettings.opacity = v / 100; });
}

QWidget* ToolOptionsBar::buildSmudgeOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    auto* mode = new QComboBox;
    mode->addItems({tr("Liquify"), tr("Blur"), tr("Smudge"), tr("Sharpen")});
    mode->setCurrentIndex(int(session_->blurMode));
    connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { session_->blurMode = BlurToolMode(i); });
    syncers_.push_back([this, mode] { QSignalBlocker b(mode); mode->setCurrentIndex(int(session_->blurMode)); });
    h->addWidget(new QLabel(tr("Mode")));
    h->addWidget(mode);
    addBrushTipFields(h);
    h->addStretch();
    return w;
}

QWidget* ToolOptionsBar::buildToningOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    auto* kind = new QComboBox;
    kind->addItems({tr("Dodge"), tr("Burn"), tr("Sponge")});
    kind->setCurrentIndex(int(session_->toning.kind));
    auto* range = new QComboBox;
    range->addItems({tr("Shadows"), tr("Midtones"), tr("Highlights")});
    range->setCurrentIndex(int(session_->toning.range));
    auto* protect = new QCheckBox(tr("Protect Tones"));
    protect->setChecked(session_->toning.protectTones);
    auto* sponge = new QComboBox;
    sponge->addItems({tr("Desaturate"), tr("Saturate")});
    sponge->setCurrentIndex(session_->toning.saturate ? 1 : 0);
    auto* rangeLabel = new QLabel(tr("Range"));
    auto* spongeLabel = new QLabel(tr("Mode"));
    auto show = [=, this] {
        const bool isSponge = session_->toning.kind == compositor::ToningKind::Sponge;
        rangeLabel->setVisible(!isSponge); range->setVisible(!isSponge); protect->setVisible(!isSponge);
        spongeLabel->setVisible(isSponge); sponge->setVisible(isSponge);
    };
    connect(kind, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, show](int i) { session_->toning.kind = compositor::ToningKind(i); show(); });
    syncers_.push_back([this, kind, show] { { QSignalBlocker b(kind); kind->setCurrentIndex(int(session_->toning.kind)); } show(); });
    connect(range, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { session_->toning.range = compositor::ToneRange(i); });
    connect(protect, &QCheckBox::toggled, this, [this](bool on) { session_->toning.protectTones = on; });
    connect(sponge, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { session_->toning.saturate = i == 1; });
    h->addWidget(new QLabel(tr("Tool")));
    h->addWidget(kind);
    h->addWidget(rangeLabel);
    h->addWidget(range);
    h->addWidget(protect);
    h->addWidget(spongeLabel);
    h->addWidget(sponge);
    addBrushTipFields(h, tr("Exposure"));
    h->addStretch();
    show();
    return w;
}

QComboBox* ToolOptionsBar::pathOperationBox(bool withNewLayer) {
    // Photoshop's path operations menu; New Layer only where an outline can start a layer of its own.
    // Short names in the bar, Photoshop's in the tooltips.
    auto* box = new QComboBox;
    box->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    auto item = [box](const QString& shortName, const QString& name, int op) { box->addItem(shortName, op); box->setItemData(box->count() - 1, name, Qt::ToolTipRole); };
    if (withNewLayer) item(tr("New Layer"), tr("New Layer"), -1);
    item(tr("Combine"), tr("Combine Shapes"), int(VectorPath::Op::Add));
    item(tr("Subtract"), tr("Subtract Front Shape"), int(VectorPath::Op::Subtract));
    item(tr("Intersect"), tr("Intersect Shape Areas"), int(VectorPath::Op::Intersect));
    item(tr("Exclude"), tr("Exclude Overlapping Shapes"), int(VectorPath::Op::Xor));
    box->setToolTip(tr("How the next outline combines with the active shape layer (or the targeted vector mask, or the path)"));
    return box;
}

QWidget* ToolOptionsBar::buildPenOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    auto* mode = new QComboBox;
    mode->addItems({tr("Shape"), tr("Path")});
    mode->setToolTip(tr("Shape makes a vector shape layer; Path draws into the Paths panel's path (or a new Work Path)"));
    // The Pen: the next outline's operation. Direct Selection: the picked subpath's component's.
    auto* op = pathOperationBox(true);
    auto* merge = new QPushButton(tr("Merge Shape Components"));
    merge->setToolTip(tr("Flatten the target path's components into one outline (curves become corner points)"));
    connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { session_->penMode = i == 1 ? EditorSession::PenMode::Path : EditorSession::PenMode::Shape; });
    connect(op, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, op](int) {
        const int v = op->currentData().toInt();
        if (session_->tool() == Tool::DirectSelect) { if (v >= 0) session_->setSelectedSubpathOp(VectorPath::Op(v)); return; }
        session_->pathOp = v < 0 ? std::nullopt : std::optional(VectorPath::Op(v));
    });
    connect(merge, &QPushButton::clicked, this, [this] { session_->mergeTargetComponents(); });
    auto* autoAdd = new QCheckBox(tr("Auto Add/Delete"));
    autoAdd->setToolTip(tr("Click the target path's outline to add an anchor, an anchor to delete it"));
    autoAdd->setChecked(session_->penAutoAddDelete);
    connect(autoAdd, &QCheckBox::toggled, this, [this](bool on) { session_->penAutoAddDelete = on; });
    auto* title = new QLabel(tr("Pen"));
    syncers_.push_back([this, mode, op, title, autoAdd, merge] {
        QSignalBlocker b1(mode), b2(op);
        const bool direct = session_->tool() == Tool::DirectSelect;
        title->setText(direct ? tr("Direct Selection") : tr("Pen"));
        mode->setVisible(!direct);
        autoAdd->setVisible(!direct);
        mode->setCurrentIndex(session_->penMode == EditorSession::PenMode::Path ? 1 : 0);
        std::optional<VectorPath::Op> shown = session_->pathOp;
        bool enabled = true;
        if (direct) {
            auto path = session_->targetPath();
            const auto picked = session_->selectedSubpath();
            enabled = path && picked && *picked >= 0 && *picked < int(path->subpaths.size());
            shown = enabled ? std::optional(path->subpaths[size_t(*picked)].op) : std::nullopt;
            op->setToolTip(tr("The picked subpath's component: how it combines with the ones before it"));
        } else op->setToolTip(tr("How the next outline combines with the active shape layer (or the targeted vector mask, or the path)"));
        op->setEnabled(enabled);
        const int index = op->findData(shown ? int(*shown) : -1);
        op->setCurrentIndex(index >= 0 ? index : 0);
        merge->setEnabled(session_->targetPath().has_value());
    });
    h->addWidget(title);
    h->addWidget(mode);
    h->addWidget(op);
    h->addWidget(merge);
    h->addWidget(autoAdd);
    h->addStretch();
    return w;
}

QWidget* ToolOptionsBar::buildBoxOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    auto* title = new QLabel;
    // Artboard: the active artboard's background, as Photoshop's Artboard options.
    auto* backgroundLabel = new QLabel(tr("Background"));
    auto* background = new QComboBox;
    background->addItems({tr("White"), tr("Black"), tr("Transparent"), tr("Other…")});
    background->setToolTip(tr("The active artboard's background"));
    connect(background, QOverload<int>::of(&QComboBox::activated), this, [this](int i) {
        const Layer* l = session_->activeLayer();
        if (!l || !l->artboard) return;
        Artboard a = *l->artboard;
        a.background = i + 1;
        if (a.background == Artboard::Other) {
            const QColor c = QColorDialog::getColor(QColor::fromRgbF(float(a.red), float(a.green), float(a.blue)), this, tr("Artboard Background"));
            if (!c.isValid()) { emit session_->layersChanged(); return; }
            a.red = c.redF(); a.green = c.greenF(); a.blue = c.blueF();
        }
        session_->setArtboard(l->id, a, false);
    });
    // Slice: remove them all.
    auto* clear = new QPushButton(tr("Delete All Slices"));
    connect(clear, &QPushButton::clicked, this, [this] {
        if (!session_->document()) return;
        std::vector<uint32_t> ids;
        for (const Slice& s : session_->document()->slices) ids.push_back(s.id);
        if (ids.empty()) return;
        session_->beginEdit(QT_TRANSLATE_NOOP("History", "Delete Slices"));
        for (uint32_t id : ids) session_->deleteSlice(id);
        session_->endEdit();
    });
    syncers_.push_back([this, title, backgroundLabel, background, clear] {
        const bool artboard = session_->tool() == Tool::Artboard;
        title->setText(artboard ? tr("Artboard") : tr("Slice"));
        const Layer* l = session_->activeLayer();
        const bool onArtboard = artboard && l && l->artboard;
        backgroundLabel->setVisible(artboard);
        background->setVisible(artboard);
        background->setEnabled(onArtboard);
        if (onArtboard) { QSignalBlocker b(background); background->setCurrentIndex(std::clamp(l->artboard->background, 1, 4) - 1); }
        clear->setVisible(!artboard);
    });
    h->addWidget(title);
    h->addWidget(backgroundLabel);
    h->addWidget(background);
    h->addWidget(clear);
    h->addStretch();
    return w;
}

QWidget* ToolOptionsBar::buildBucketOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    h->addWidget(new QLabel(tr("Opacity")));
    auto* opacity = numberField(1, 100, 0, "%", tr("Opacity"));
    opacity->setProperty("role", "opacity");
    opacity->setValue(session_->brushSettings.opacity * 100);
    connect(opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) { session_->brushSettings.opacity = v / 100; emit session_->toolChanged(); });
    h->addWidget(opacity);
    h->addWidget(new QLabel(tr("Tolerance")));
    auto* tolerance = numberField(0, 255, 0, QString(), tr("Tolerance"));
    tolerance->setValue(session_->bucket.tolerance);
    connect(tolerance, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) { session_->bucket.tolerance = int(v); });
    h->addWidget(tolerance);
    auto check = [&](const QString& label, bool& value) {
        auto* box = new QCheckBox(label);
        box->setChecked(value);
        connect(box, &QCheckBox::toggled, this, [&value](bool on) { value = on; });
        h->addWidget(box);
    };
    check(tr("Anti-alias"), session_->bucket.antialias);
    check(tr("Contiguous"), session_->bucket.contiguous);
    check(tr("All Layers"), session_->bucket.allLayers);
    h->addStretch();
    return w;
}

QWidget* ToolOptionsBar::buildGradientOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    auto* shape = new QComboBox;
    shape->addItems({tr("Linear"), tr("Radial")});
    connect(shape, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { session_->gradientSettings.shape = i == 1 ? GradientShape::Radial : GradientShape::Linear; session_->refreshGradient(); });
    h->addWidget(shape);
    // The two built-in ramps, then the gradients imported from .grd files (PresetLibrary).
    auto* style = new QComboBox;
    style->setToolTip(tr("Gradient preset (File ▸ Import Presets… adds Photoshop .grd gradients)"));
    auto fillStyles = [this, style] {
        QSignalBlocker b(style);
        QStringList names;
        for (const auto& g : PresetLibrary::instance().gradients()) names << QString::fromStdString(g.name);
        QStringList shown;
        for (int i = 2; i < style->count(); i++) shown << style->itemData(i).toString();
        if (style->count() < 2 || shown != names) {   // rebuilt only when the library changed
            style->clear();
            style->addItems({tr("Foreground to Transparent"), tr("Foreground to Background")});
            for (const QString& name : names) style->addItem(name, name);
        }
        const GradientSettings& s = session_->gradientSettings;
        const int preset = s.preset.isEmpty() ? -1 : style->findData(s.preset);
        style->setCurrentIndex(preset >= 0 ? preset : s.style == GradientStyle::ForegroundToBackground ? 1 : 0);
    };
    fillStyles();
    connect(&PresetLibrary::instance(), &PresetLibrary::changed, style, fillStyles);
    syncers_.push_back(fillStyles);
    connect(style, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, style](int i) {
        session_->gradientSettings.style = i == 1 ? GradientStyle::ForegroundToBackground : GradientStyle::ForegroundToTransparent;
        session_->gradientSettings.preset = i >= 2 ? style->itemData(i).toString() : QString();
        session_->refreshGradient();
    });
    h->addWidget(style);
    auto* reverse = new QCheckBox(tr("Reverse"));
    connect(reverse, &QCheckBox::toggled, this, [this](bool on) { session_->gradientSettings.reversed = on; session_->refreshGradient(); });
    h->addWidget(reverse);
    h->addWidget(separator());
    h->addWidget(new QLabel(tr("Opacity")));
    auto* opacity = numberField(1, 100, 0, "%", tr("Opacity"));
    opacity->setProperty("role", "gradientOpacity");
    opacity->setValue(session_->gradientSettings.opacity * 100);
    connect(opacity, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) { session_->gradientSettings.opacity = v / 100; session_->refreshGradient(); });
    syncers_.push_back([this, opacity] { QSignalBlocker b(opacity); opacity->setValue(session_->gradientSettings.opacity * 100); });
    h->addWidget(opacity);
    auto* apply = new QPushButton(tr("Apply"));
    connect(apply, &QPushButton::clicked, this, [this] { session_->commitGradient(); });
    h->addWidget(apply);
    h->addStretch();
    return w;
}

QWidget* ToolOptionsBar::buildShapeOptions() {
    // New shapes take these settings; with a vector shape layer active, Fill and Stroke edit it (as Photoshop's bar does).
    QWidget* w = row();
    auto* h = layoutOf(w);
    ShapeToolSettings& st = session_->shapeTool;
    auto* kind = new QComboBox;
    kind->addItems({tr("Rectangle"), tr("Ellipse"), tr("Polygon"), tr("Line"), tr("Custom Shape")});
    h->addWidget(kind);
    // Per-kind fields.
    auto* radiusLabel = new QLabel(tr("Radius"));
    auto* radius = numberField(0, 5000, 0, " px", tr("Corner radius"));
    auto* sidesLabel = new QLabel(tr("Sides"));
    auto* sides = numberField(3, 100, 0, QString(), tr("Sides (points, for a star)"));
    sides->setValue(st.sides);
    auto* starLabel = new QLabel(tr("Star"));
    auto* star = numberField(0, 99, 0, "%", tr("How far a star's inner points come in; 0 for a polygon"));
    auto* weightLabel = new QLabel(tr("Weight"));
    auto* weight = numberField(0.5, 1000, 1, " px", tr("Line weight"));
    weight->setValue(st.lineWeight);
    auto* custom = new QComboBox;
    for (const auto& n : compositor::customShapeNames()) custom->addItem(QString::fromStdString(n));
    for (QWidget* x : std::initializer_list<QWidget*>{radiusLabel, radius, sidesLabel, sides, starLabel, star, weightLabel, weight, custom}) h->addWidget(x);
    auto showKind = [=, this] {
        const VectorShapeKind k = session_->shapeTool.kind;
        radiusLabel->setVisible(k == VectorShapeKind::Rectangle); radius->setVisible(k == VectorShapeKind::Rectangle);
        for (QWidget* x : {static_cast<QWidget*>(sidesLabel), static_cast<QWidget*>(sides), static_cast<QWidget*>(starLabel), static_cast<QWidget*>(star)}) x->setVisible(k == VectorShapeKind::Polygon);
        weightLabel->setVisible(k == VectorShapeKind::Line); weight->setVisible(k == VectorShapeKind::Line);
        custom->setVisible(k == VectorShapeKind::Custom);
    };
    connect(kind, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, showKind](int i) { session_->shapeTool.kind = VectorShapeKind(i); showKind(); });
    connect(radius, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) { session_->shapeTool.cornerRadius = v; });
    connect(sides, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) { session_->shapeTool.sides = int(v); });
    connect(star, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) { session_->shapeTool.starInset = v / 100; });
    connect(weight, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) { session_->shapeTool.lineWeight = v; });
    connect(custom, &QComboBox::currentTextChanged, this, [this](const QString& n) { session_->shapeTool.custom = n.toStdString(); });
    // The path operation: a new layer, or a component of the active shape layer.
    auto* op = pathOperationBox(true);
    connect(op, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, op](int) {
        const int v = op->currentData().toInt();
        session_->pathOp = v < 0 ? std::nullopt : std::optional(VectorPath::Op(v));
    });
    auto* merge = new QToolButton;
    merge->setText(tr("Merge"));
    merge->setToolTip(tr("Merge Shape Components: flatten the active shape's components into one outline"));
    connect(merge, &QToolButton::clicked, this, [this] { session_->mergeTargetComponents(); });
    h->addWidget(op);
    h->addWidget(merge);
    h->addWidget(separator());

    // Fill and stroke.
    auto swatch = [](QToolButton* b, QColor c) { b->setStyleSheet(QStringLiteral("QToolButton { background: %1; min-width: 22px; border: 1px solid #888; }").arg(c.name())); };
    auto* fill = new QCheckBox(tr("Fill"));
    fill->setChecked(st.fill);
    auto* fillColour = new QToolButton;
    fillColour->setToolTip(tr("The active shape's fill colour (new shapes take the foreground colour)"));
    auto* stroke = new QCheckBox(tr("Stroke"));
    stroke->setChecked(st.stroke.enabled);
    auto* strokeColour = new QToolButton;
    strokeColour->setToolTip(tr("Stroke colour"));
    auto* strokeWidth = numberField(0, 1000, 1, " px", tr("Stroke width"));
    strokeWidth->setValue(st.stroke.width);
    auto* align = new QComboBox;
    align->addItems({tr("Inside"), tr("Center"), tr("Outside")});
    align->setCurrentIndex(1);
    auto* dash = new QComboBox;
    dash->addItems({tr("Solid"), tr("Dashed"), tr("Dotted")});
    // Colour, gradient or pattern, for the fill and for the stroke (Photoshop's fill-type picker): a gradient from
    // the presets (foreground to background by default), a pattern from the document's.
    auto* fillType = new QComboBox;
    fillType->addItems({tr("Color"), tr("Gradient"), tr("Pattern")});
    fillType->setToolTip(tr("Fill with a colour, a gradient or a pattern"));
    auto* fillSource = new QComboBox;
    fillSource->setToolTip(tr("The fill's gradient preset or document pattern"));
    auto* strokeType = new QComboBox;
    strokeType->addItems({tr("Color"), tr("Gradient"), tr("Pattern")});
    strokeType->setToolTip(tr("Stroke with a colour, a gradient or a pattern"));
    auto* strokeSource = new QComboBox;
    strokeSource->setToolTip(tr("The stroke's gradient preset or document pattern"));
    for (QComboBox* c : {fillType, fillSource, strokeType, strokeSource}) c->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    fillSource->setMaximumWidth(180);
    strokeSource->setMaximumWidth(180);
    for (QWidget* x : std::initializer_list<QWidget*>{fill, fillType, fillColour, fillSource, stroke, strokeType, strokeColour, strokeSource, strokeWidth, align, dash}) h->addWidget(x);
    // The sources a type offers: gradient presets, or the document's patterns (kept when unchanged).
    auto sources = [this](QComboBox* type, QComboBox* source) {
        QSignalBlocker b(source);
        std::vector<std::pair<QString, QString>> items;   // data, label
        if (type->currentIndex() == 1) {
            items.push_back({QString(), tr("Foreground to Background")});
            for (const auto& g : PresetLibrary::instance().gradients()) items.push_back({QString::fromStdString(g.name), QString::fromStdString(g.name)});
        } else if (type->currentIndex() == 2 && session_->document())
            for (const auto& [id, name] : documentPatternList(*session_->document())) items.push_back({QString::fromStdString(id), QString::fromStdString(name)});
        bool same = int(items.size()) == source->count();
        for (int i = 0; same && i < source->count(); i++) same = source->itemData(i).toString() == items[size_t(i)].first && source->itemText(i) == items[size_t(i)].second;
        if (!same) {
            const QString kept = source->currentData().toString();
            source->clear();
            for (auto& [data, label] : items) source->addItem(label, data);
            const int at = source->findData(kept);
            if (at >= 0) source->setCurrentIndex(at);
        }
        if (type->currentIndex() == 2 && items.empty()) { source->addItem(tr("No patterns in the document")); source->setEnabled(false); }
        else source->setEnabled(true);
        source->setVisible(type->currentIndex() != 0);
    };
    auto paintOf = [this](QComboBox* type, QComboBox* source) {
        VectorPaint p;
        if (type->currentIndex() == 1) { p.kind = VectorPaint::Kind::Gradient; p.gradient = shapeGradient(source->currentData().toString(), session_->foregroundColor, session_->backgroundColor); }
        else if (type->currentIndex() == 2 && !source->currentData().toString().isEmpty()) { p.kind = VectorPaint::Kind::Pattern; p.pattern.id = source->currentData().toString().toStdString(); }
        return p;
    };
    // Each change goes to the settings and, when a vector shape layer is active, to it as one step.
    auto apply = [this](const QString& name, std::function<void(VectorShape&)> change) {
        if (auto shape = session_->activeVectorShape()) { change(*shape); session_->setActiveVectorShape(*shape, name); }
    };
    connect(fill, &QCheckBox::toggled, this, [this, apply](bool on) { session_->shapeTool.fill = on; apply(tr("Shape Fill"), [on](VectorShape& s) { s.fill = on; }); });
    connect(fillColour, &QToolButton::clicked, this, [this, apply, swatch, fillColour] {
        auto shape = session_->activeVectorShape();
        const QColor start = shape ? QColor(shape->r, shape->g, shape->b) : session_->foregroundColor;
        const QColor c = QColorDialog::getColor(start, this, tr("Fill"));
        if (!c.isValid()) return;
        swatch(fillColour, c);
        if (shape) apply(tr("Shape Fill"), [c](VectorShape& s) { s.r = uint8_t(c.red()); s.g = uint8_t(c.green()); s.b = uint8_t(c.blue()); s.fill = true; s.fillPaint = {}; });
        else { session_->foregroundColor = c; emit session_->toolChanged(); }
    });
    connect(stroke, &QCheckBox::toggled, this, [this, apply](bool on) { session_->shapeTool.stroke.enabled = on; apply(tr("Shape Stroke"), [on](VectorShape& s) { s.stroke.enabled = on; }); });
    connect(strokeColour, &QToolButton::clicked, this, [this, apply, swatch, strokeColour] {
        compositor::VectorStroke& t = session_->shapeTool.stroke;
        const QColor c = QColorDialog::getColor(QColor(t.r, t.g, t.b), this, tr("Stroke"));
        if (!c.isValid()) return;
        t.r = uint8_t(c.red()); t.g = uint8_t(c.green()); t.b = uint8_t(c.blue());
        swatch(strokeColour, c);
        apply(tr("Shape Stroke"), [c](VectorShape& s) { s.stroke.r = uint8_t(c.red()); s.stroke.g = uint8_t(c.green()); s.stroke.b = uint8_t(c.blue()); s.stroke.enabled = true; s.stroke.paint = {}; });
    });
    auto applyFillPaint = [this, apply, fillType, fillSource, paintOf] {
        const VectorPaint paint = paintOf(fillType, fillSource);
        session_->shapeTool.fillPaint = paint;
        apply(tr("Shape Fill"), [paint](VectorShape& s) { s.fillPaint = paint; s.fill = true; });
    };
    auto applyStrokePaint = [this, apply, strokeType, strokeSource, paintOf] {
        const VectorPaint paint = paintOf(strokeType, strokeSource);
        session_->shapeTool.stroke.paint = paint;
        apply(tr("Shape Stroke"), [paint](VectorShape& s) { s.stroke.paint = paint; s.stroke.enabled = true; });
    };
    connect(fillType, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [sources, fillType, fillSource, fillColour, applyFillPaint](int i) {
        sources(fillType, fillSource); fillColour->setVisible(i == 0); applyFillPaint();
    });
    connect(fillSource, QOverload<int>::of(&QComboBox::activated), this, [applyFillPaint](int) { applyFillPaint(); });
    connect(strokeType, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [sources, strokeType, strokeSource, strokeColour, applyStrokePaint](int i) {
        sources(strokeType, strokeSource); strokeColour->setVisible(i == 0); applyStrokePaint();
    });
    connect(strokeSource, QOverload<int>::of(&QComboBox::activated), this, [applyStrokePaint](int) { applyStrokePaint(); });
    connect(strokeWidth, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, apply](double v) {
        session_->shapeTool.stroke.width = v; apply(tr("Shape Stroke"), [v](VectorShape& s) { s.stroke.width = v; });
    });
    connect(align, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, apply](int i) {
        const auto a = i == 0 ? compositor::VectorStroke::Align::Inside : i == 2 ? compositor::VectorStroke::Align::Outside : compositor::VectorStroke::Align::Center;
        session_->shapeTool.stroke.align = a; apply(tr("Shape Stroke"), [a](VectorShape& s) { s.stroke.align = a; });
    });
    connect(dash, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, apply](int i) {
        // Photoshop's presets, in stroke widths: dashes 4 on 2 off; dots 0 on 2 off with round caps.
        auto set = [i](compositor::VectorStroke& t) {
            t.dashes = i == 1 ? std::vector<double>{4, 2} : i == 2 ? std::vector<double>{0, 2} : std::vector<double>{};
            t.cap = i == 2 ? compositor::VectorStroke::Cap::Round : compositor::VectorStroke::Cap::Butt;
        };
        set(session_->shapeTool.stroke); apply(tr("Shape Stroke"), [set](VectorShape& s) { set(s.stroke); });
    });
    // Properties: the active shape's live rectangle or ellipse (Photoshop's Properties panel for a live shape).
    auto* propertiesLabel = new QLabel(tr("Properties"));
    auto* liveW = numberField(1, 300000, 1, " px", tr("Width"));
    auto* liveH = numberField(1, 300000, 1, " px", tr("Height"));
    auto* liveX = numberField(-300000, 300000, 1, " px", tr("Left"));
    auto* liveY = numberField(-300000, 300000, 1, " px", tr("Top"));
    std::array<QDoubleSpinBox*, 4> liveR{};
    const QString corners[4] = {tr("Top-left corner radius"), tr("Top-right corner radius"), tr("Bottom-right corner radius"), tr("Bottom-left corner radius")};
    for (int i = 0; i < 4; i++) liveR[size_t(i)] = numberField(0, 150000, 1, " px", corners[i]);
    h->addWidget(separator());
    h->addWidget(propertiesLabel);
    // Compact fields, each after a one-letter label (W, H, X, Y; the radii clockwise from the top left).
    std::vector<QLabel*> liveLabels;
    const char* names[] = {"W", "H", "X", "Y", "R"};
    int n = 0;
    for (QDoubleSpinBox* f : {liveW, liveH, liveX, liveY, liveR[0], liveR[1], liveR[2], liveR[3]}) {
        f->setKeyboardTracking(false);
        f->setSuffix(QString());
        f->setDecimals(1);
        f->setButtonSymbols(QAbstractSpinBox::NoButtons);
        f->setFixedWidth(56);
        if (n <= 4) { auto* label = new QLabel(tr(names[n])); liveLabels.push_back(label); h->addWidget(label); }
        h->addWidget(f);
        n++;
    }
    // The live shape shown: the one of the subpath Direct Selection picked, else the first.
    auto liveShown = [this]() -> std::optional<LiveShape> {
        const auto list = session_->activeLiveShapes();
        if (list.empty()) return std::nullopt;
        if (auto picked = session_->selectedSubpath(); picked)
            if (auto path = session_->targetPath(); path && *picked >= 0 && *picked < int(path->subpaths.size()))
                for (const LiveShape& l : list) if (l.group == path->subpaths[size_t(*picked)].group) return l;
        return list.front();
    };
    auto applyLive = [this, liveShown](std::function<void(LiveShape&)> change) {
        auto live = liveShown();
        if (!live) return;
        change(*live);
        session_->setActiveLiveShape(*live);
    };
    connect(liveW, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [applyLive](double v) { applyLive([v](LiveShape& l) { l.box.width = v; }); });
    connect(liveH, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [applyLive](double v) { applyLive([v](LiveShape& l) { l.box.height = v; }); });
    connect(liveX, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [applyLive](double v) { applyLive([v](LiveShape& l) { l.box.x = v; }); });
    connect(liveY, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [applyLive](double v) { applyLive([v](LiveShape& l) { l.box.y = v; }); });
    for (size_t i = 0; i < 4; i++)
        connect(liveR[i], QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [applyLive, i](double v) { applyLive([v, i](LiveShape& l) { l.radii[i] = v; }); });
    // The bar shows the active shape's fill and stroke; otherwise the settings new shapes take.
    syncers_.push_back([=, this] {
        QSignalBlocker b1(kind), b2(fill), b3(stroke), b4(strokeWidth), b5(align), b6(dash), b7(op), b8(fillType), b9(strokeType);
        kind->setCurrentIndex(int(session_->shapeTool.kind));
        showKind();
        const int opIndex = op->findData(session_->pathOp ? int(*session_->pathOp) : -1);
        op->setCurrentIndex(opIndex >= 0 ? opIndex : 0);
        const auto shape = session_->activeVectorShape();
        merge->setEnabled(shape.has_value());
        const VectorPaint& fp = shape ? shape->fillPaint : session_->shapeTool.fillPaint;
        const VectorPaint& sp = shape ? shape->stroke.paint : session_->shapeTool.stroke.paint;
        fillType->setCurrentIndex(int(fp.kind));
        strokeType->setCurrentIndex(int(sp.kind));
        sources(fillType, fillSource);
        sources(strokeType, strokeSource);
        if (fp.kind == VectorPaint::Kind::Pattern) { QSignalBlocker b(fillSource); const int at = fillSource->findData(QString::fromStdString(fp.pattern.id)); if (at >= 0) fillSource->setCurrentIndex(at); }
        if (sp.kind == VectorPaint::Kind::Pattern) { QSignalBlocker b(strokeSource); const int at = strokeSource->findData(QString::fromStdString(sp.pattern.id)); if (at >= 0) strokeSource->setCurrentIndex(at); }
        fillColour->setVisible(fp.kind == VectorPaint::Kind::Solid);
        strokeColour->setVisible(sp.kind == VectorPaint::Kind::Solid);
        const auto live = liveShown();
        propertiesLabel->setVisible(live.has_value());
        for (size_t i = 0; i < liveLabels.size(); i++) liveLabels[i]->setVisible(live && (i < 4 || live->kind == LiveShape::Kind::Rectangle));
        for (QDoubleSpinBox* f : {liveW, liveH, liveX, liveY}) f->setVisible(live.has_value());
        for (QDoubleSpinBox* f : liveR) f->setVisible(live && live->kind == LiveShape::Kind::Rectangle);
        if (live) {
            propertiesLabel->setText(live->kind == LiveShape::Kind::Ellipse ? tr("Ellipse") : tr("Rectangle"));
            QSignalBlocker c1(liveW), c2(liveH), c3(liveX), c4(liveY), c5(liveR[0]), c6(liveR[1]), c7(liveR[2]), c8(liveR[3]);
            liveW->setValue(live->box.width); liveH->setValue(live->box.height); liveX->setValue(live->box.x); liveY->setValue(live->box.y);
            for (size_t i = 0; i < 4; i++) liveR[i]->setValue(live->radii[i]);
        }
        const compositor::VectorStroke& t = shape ? shape->stroke : session_->shapeTool.stroke;
        fill->setChecked(shape ? shape->fill : session_->shapeTool.fill);
        swatch(fillColour, shape ? QColor(shape->r, shape->g, shape->b) : session_->foregroundColor);
        stroke->setChecked(t.enabled);
        swatch(strokeColour, QColor(t.r, t.g, t.b));
        strokeWidth->setValue(t.width);
        align->setCurrentIndex(t.align == compositor::VectorStroke::Align::Inside ? 0 : t.align == compositor::VectorStroke::Align::Outside ? 2 : 1);
        dash->setCurrentIndex(t.dashes.empty() ? 0 : t.dashes.front() == 0 ? 2 : 1);
    });
    h->addStretch();
    return w;
}

QWidget* ToolOptionsBar::buildTextOptions() {
    // The style new text starts with; a live text layer that is active takes each change straight away.
    QWidget* w = row();
    auto* h = layoutOf(w);
    auto applyStyle = [this](std::function<void(LayerText&)> change) {
        const LayerText before = session_->textStyle;
        change(session_->textStyle);
        if (canvas_->typeEditing()) {
            // Typing on the canvas: the selected letters (or the ones typed next) take the change.
            const LayerText& after = session_->textStyle;
            canvas_->applyTypeStyle(CanvasWidget::typePatch(before, after), after.alignment != before.alignment ? std::optional(after.alignment) : std::nullopt);
            return;
        }
        const Layer* layer = session_->activeLayer();
        if (layer && layer->isLiveText() && !session_->textEditing()) {
            LayerText text = *layer->text;
            change(text);
            session_->setLayerText(layer->id, text);
        }
    };
    auto* family = new FontPicker;
    family->setMinimumWidth(150);
    family->setMaximumWidth(240);
    connect(family, &FontPicker::familyChanged, this, [applyStyle](const QString& f) { applyStyle([f](LayerText& t) { t.fontFamily = f.toStdString(); }); });
    syncers_.push_back([this, family] { QSignalBlocker b(family); family->setFamily(fontFor(session_->textStyle).family()); });
    h->addWidget(family);
    auto* size = numberField(1, 2000, 0, " px", tr("Size, in document pixels"));
    connect(size, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [applyStyle](double v) { applyStyle([v](LayerText& t) { t.fontSize = v; }); });
    syncers_.push_back([this, size] { QSignalBlocker b(size); size->setValue(session_->textStyle.fontSize); });
    h->addWidget(new QLabel(tr("Size", "font size")));
    h->addWidget(size);
    auto* bold = new QToolButton;
    bold->setText(tr("B")); bold->setCheckable(true); bold->setToolTip(tr("Bold"));
    { QFont f = bold->font(); f.setBold(true); bold->setFont(f); }
    connect(bold, &QToolButton::toggled, this, [applyStyle](bool on) { applyStyle([on](LayerText& t) { t.bold = on; }); });
    syncers_.push_back([this, bold] { QSignalBlocker b(bold); bold->setChecked(session_->textStyle.bold); });
    h->addWidget(bold);
    auto* italic = new QToolButton;
    italic->setText(tr("I")); italic->setCheckable(true); italic->setToolTip(tr("Italic"));
    { QFont f = italic->font(); f.setItalic(true); italic->setFont(f); }
    connect(italic, &QToolButton::toggled, this, [applyStyle](bool on) { applyStyle([on](LayerText& t) { t.italic = on; }); });
    syncers_.push_back([this, italic] { QSignalBlocker b(italic); italic->setChecked(session_->textStyle.italic); });
    h->addWidget(italic);
    auto* align = new QComboBox;
    align->addItems({tr("Left"), tr("Centre"), tr("Right")});
    align->setToolTip(tr("Alignment of the lines"));
    connect(align, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [applyStyle](int i) { applyStyle([i](LayerText& t) { t.alignment = i; }); });
    syncers_.push_back([this, align] { QSignalBlocker b(align); align->setCurrentIndex(std::clamp(session_->textStyle.alignment, 0, 2)); });
    h->addWidget(align);
    // The text colour, as Photoshop's swatch in this bar; with no text to change it sets the foreground colour,
    // which new type takes.
    auto* colour = new QToolButton;
    colour->setToolTip(tr("Set the text colour"));
    colour->setFixedSize(28, 22);
    auto showColour = [colour](const QColor& c) { colour->setStyleSheet(QStringLiteral("QToolButton { background: %1; border: 1px solid palette(mid); }").arg(c.name())); };
    connect(colour, &QToolButton::clicked, this, [this, applyStyle, showColour] {
        const LayerText& s = session_->textStyle;
        const QColor current = canvas_->typeEditing() || (session_->activeLayer() && session_->activeLayer()->isLiveText())
            ? QColor::fromRgbF(float(s.red), float(s.green), float(s.blue)) : session_->foregroundColor;
        const QColor chosen = QColorDialog::getColor(current, window(), tr("Text Colour"));
        if (!chosen.isValid()) return;
        const bool onText = canvas_->typeEditing() || (session_->activeLayer() && session_->activeLayer()->isLiveText());
        applyStyle([chosen](LayerText& t) { t.red = chosen.redF(); t.green = chosen.greenF(); t.blue = chosen.blueF(); });
        if (!onText) { session_->foregroundColor = chosen; emit session_->toolChanged(); }
        showColour(chosen);
    });
    syncers_.push_back([this, showColour] {
        const LayerText& s = session_->textStyle;
        const bool onText = canvas_->typeEditing() || (session_->activeLayer() && session_->activeLayer()->isLiveText());
        showColour(onText ? QColor::fromRgbF(float(s.red), float(s.green), float(s.blue)) : session_->foregroundColor);
    });
    h->addWidget(colour);
    // While typing: commit (Ctrl+Enter) and cancel (Esc), as the check and cross at the end of Photoshop's bar.
    auto* commit = new QToolButton;
    commit->setText(QStringLiteral("\u2713"));
    commit->setToolTip(tr("Commit the text (Ctrl+Enter)"));
    auto* cancel = new QToolButton;
    cancel->setText(QStringLiteral("\u2715"));
    cancel->setToolTip(tr("Cancel the text edit (Esc)"));
    connect(commit, &QToolButton::clicked, this, [this] { canvas_->commitType(); canvas_->setFocus(); });
    connect(cancel, &QToolButton::clicked, this, [this] { canvas_->cancelType(); canvas_->setFocus(); });
    syncers_.push_back([this, commit, cancel] { commit->setVisible(canvas_->typeEditing()); cancel->setVisible(canvas_->typeEditing()); });
    // The bar shows the style at the caret.
    connect(canvas_, &CanvasWidget::typeEditChanged, this, [this] {
        if (auto run = canvas_->typeStyleAtCaret()) {
            LayerText& s = session_->textStyle;
            s.fontFamily = run->fontFamily; s.fontSize = run->fontSize; s.bold = run->bold; s.italic = run->italic;
            s.red = run->red; s.green = run->green; s.blue = run->blue;
        }
        if (auto a = canvas_->typeAlignment()) session_->textStyle.alignment = *a;
        for (auto& sync : syncers_) sync();
    });
    auto* edit = new QPushButton(tr("Edit Text…"));
    edit->setToolTip(tr("Open the editor for the active text layer"));
    connect(edit, &QPushButton::clicked, this, [this] { const Layer* l = session_->activeLayer(); if (l && l->isLiveText()) session_->requestTextEdit(l->id); });
    syncers_.push_back([this, edit] { const Layer* l = session_->activeLayer(); edit->setEnabled(l && l->isLiveText() && !canvas_->typeEditing()); });
    h->addWidget(edit);
    h->addWidget(commit);
    h->addWidget(cancel);
    h->addStretch();
    return w;
}

QWidget* ToolOptionsBar::buildMarqueeOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    auto* kind = new QComboBox;
    kind->addItems({tr("Rectangle"), tr("Ellipse")});
    connect(kind, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { session_->marqueeKind = i == 1 ? MarqueeKind::Ellipse : MarqueeKind::Rectangle; });
    syncers_.push_back([this, kind] { QSignalBlocker b(kind); kind->setCurrentIndex(session_->marqueeKind == MarqueeKind::Ellipse ? 1 : 0); });
    h->addWidget(kind);
    auto* aa = new QCheckBox(tr("Anti-alias"));
    aa->setChecked(session_->selectionAntialiased);
    connect(aa, &QCheckBox::toggled, this, [this](bool on) { session_->selectionAntialiased = on; });
    h->addWidget(aa);
    h->addStretch();
    return w;
}

QWidget* ToolOptionsBar::buildLassoOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    auto* kind = new QComboBox;
    kind->addItems({tr("Freehand"), tr("Polygonal")});
    connect(kind, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { session_->lassoKind = i == 1 ? LassoKind::Polygonal : LassoKind::Freehand; canvas_->cancelLasso(); emit session_->toolChanged(); });
    kind->setProperty("role", "lassoKind");
    syncers_.push_back([this, kind] { QSignalBlocker b(kind); kind->setCurrentIndex(session_->lassoKind == LassoKind::Polygonal ? 1 : 0); });
    h->addWidget(kind);
    h->addStretch();
    return w;
}

QWidget* ToolOptionsBar::buildScribbleOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    h->addWidget(new QLabel(tr("Engine")));
    auto* engine = new QComboBox;
    engine->addItems({tr("Scribble"), tr("Click")});
    engine->setToolTip(tr("Scribble: strokes segmented by GrabCut, no model. Click: EfficientSAM finds the object under a click; Alt-click marks what is not it, a drag draws a box; up to six prompts count"));
    engine->setCurrentIndex(session_->quickSelectClicks ? 1 : 0);
    h->addWidget(engine);
    auto* fetch = new QPushButton(tr("Download model (48 MB)"));
    fetch->setToolTip(tr("Meta's EfficientSAM as packaged by OpenCV's model zoo (Apache-2.0), kept in the models folder next to the Remove Background ones; nothing is uploaded"));
    fetch->setVisible(session_->quickSelectClicks && !ModelStore::promptReady());
    connect(engine, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, fetch](int i) { session_->setQuickSelectClicks(i == 1); fetch->setVisible(session_->quickSelectClicks && !ModelStore::promptReady()); });
    connect(fetch, &QPushButton::clicked, this, [this, fetch] {
        fetch->setEnabled(false);
        ModelStore::download(ModelStore::promptModel(), fetch,
            [fetch](qint64 received, qint64 total) { fetch->setText(tr("Downloading… %1%").arg(total > 0 ? int(received * 100 / total) : 0)); },
            [this, fetch](QString, QString error) {
                fetch->setEnabled(true);
                fetch->setText(tr("Download model (48 MB)"));
                if (!error.isEmpty()) QMessageBox::warning(this, tr("Download failed"), error);
                fetch->setVisible(session_->quickSelectClicks && !ModelStore::promptReady());
            });
    });
    h->addWidget(fetch);
    h->addWidget(new QLabel(tr("Mode")));
    auto* mode = new QComboBox;
    mode->addItems({tr("Subject"), tr("Background")});
    mode->setToolTip(tr("What a stroke marks; Alt flips it for one stroke"));
    mode->setCurrentIndex(session_->scribbleBackground ? 1 : 0);
    connect(mode, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { session_->scribbleBackground = i == 1; });
    h->addWidget(mode);
    h->addWidget(new QLabel(tr("Size")));
    auto* size = new QSpinBox;
    size->setRange(1, 500);
    size->setButtonSymbols(QAbstractSpinBox::NoButtons);
    size->setAlignment(Qt::AlignRight);
    size->setFixedWidth(48);
    size->setValue(session_->scribbleSize);
    connect(size, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) { session_->scribbleSize = v; });
    h->addWidget(size);
    h->addWidget(new QLabel(tr("Refine")));
    auto* refine = new QSpinBox;
    refine->setRange(0, 40);
    refine->setToolTip(tr("Pulls the selection onto the image's own edges by this many pixels; 0 keeps the segmentation as it is"));
    refine->setButtonSymbols(QAbstractSpinBox::NoButtons);
    refine->setAlignment(Qt::AlignRight);
    refine->setFixedWidth(40);
    refine->setValue(session_->scribbleRefine);
    connect(refine, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) { session_->scribbleRefine = v; session_->startQuickSelectJob(); });
    h->addWidget(refine);
    auto* clear = new QPushButton(tr("Clear"));
    clear->setToolTip(tr("Forgets the strokes and clicks (Esc); Backspace takes back the last one"));
    connect(clear, &QPushButton::clicked, this, [this] { session_->clearScribbles(); session_->clearClickPrompts(); });
    h->addWidget(clear);
    h->addStretch();
    return w;
}

QWidget* ToolOptionsBar::buildWandOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    h->addWidget(new QLabel(tr("Tolerance")));
    auto* tol = new QSpinBox;
    tol->setRange(0, 255);
    tol->setButtonSymbols(QAbstractSpinBox::NoButtons);
    tol->setAlignment(Qt::AlignRight);
    tol->setFixedWidth(48);
    tol->setValue(session_->wandTolerance);
    // Right after a click, a new tolerance redoes that click's selection at once (the field is kept).
    connect(tol, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v) { session_->wandTolerance = v; if (session_->wandEdgeAware) session_->retolerateWand(v); });
    h->addWidget(tol);
    auto* contiguous = new QCheckBox(tr("Contiguous"));
    contiguous->setChecked(session_->wandContiguous);
    connect(contiguous, &QCheckBox::toggled, this, [this](bool on) { session_->wandContiguous = on; });
    h->addWidget(contiguous);
    auto* edgeAware = new QCheckBox(tr("Edge Aware"));
    edgeAware->setToolTip(tr("Follow the image: shading and texture stay in, edges between similar colours hold. "
                             "Change the tolerance right after a click to adjust that selection."));
    edgeAware->setChecked(session_->wandEdgeAware);
    connect(edgeAware, &QCheckBox::toggled, this, [this](bool on) { session_->wandEdgeAware = on; });
    h->addWidget(edgeAware);
    auto* refine = new QCheckBox(tr("Refine Edge"));
    refine->setToolTip(tr("Unmix the edge: the fringe of a line is partly selected, and Delete right after leaves the line its own colour, without a rim of the background's"));
    refine->setChecked(session_->wandRefineEdge);
    connect(refine, &QCheckBox::toggled, this, [this](bool on) { session_->wandRefineEdge = on; });
    h->addWidget(refine);
    auto* all = new QCheckBox(tr("Sample All Layers"));
    all->setChecked(session_->wandSampleAll);
    connect(all, &QCheckBox::toggled, this, [this](bool on) { session_->wandSampleAll = on; });
    h->addWidget(all);
    h->addWidget(new QLabel(tr("Sample")));
    auto* sample = new QComboBox;
    sample->addItems({tr("Point Sample"), tr("3 by 3 Average"), tr("5 by 5 Average")});
    sample->setCurrentIndex(session_->wandSampleRadius);
    connect(sample, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) { session_->wandSampleRadius = i; });
    h->addWidget(sample);
    h->addStretch();
    return w;
}

QWidget* ToolOptionsBar::buildCropOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    // Photoshop's crop presets: Ratio (free unless W and H are typed), Original Ratio, then the common ones.
    auto* ratio = new QComboBox;
    ratio->setToolTip(tr("The shape the crop box keeps"));
    struct Preset { const char* name; double w, h; };
    static const Preset presets[] = {{QT_TR_NOOP("Ratio"), 0, 0}, {QT_TR_NOOP("Original Ratio"), -1, -1}, {"1 : 1", 1, 1}, {"4 : 5 (8 : 10)", 4, 5},
                                     {"5 : 7", 5, 7}, {"2 : 3 (4 : 6)", 2, 3}, {"3 : 2", 3, 2}, {"4 : 3", 4, 3}, {"16 : 9", 16, 9}, {"9 : 16", 9, 16}};
    for (const Preset& p : presets) ratio->addItem(p.w == 0 || p.w < 0 ? tr(p.name) : QString::fromLatin1(p.name));
    auto ratioField = [](const QString& tip) {
        auto* f = numberField(0, 100000, 3, QString(), tip);
        f->setSpecialValueText(QStringLiteral(" "));   // 0 shows empty: no ratio
        f->setFixedWidth(64);
        return f;
    };
    auto* ratioW = ratioField(tr("Width of the ratio (empty for a free crop)"));
    auto* ratioH = ratioField(tr("Height of the ratio (empty for a free crop)"));
    auto* swapRatio = new QToolButton;
    swapRatio->setText(QStringLiteral("\u21c4"));
    swapRatio->setToolTip(tr("Swap height and width (X)"));
    auto* clearRatio = new QPushButton(tr("Clear"));
    clearRatio->setToolTip(tr("Clear the ratio"));
    auto applyFields = [this, ratioW, ratioH] { canvas_->setCropRatio(ratioW->value(), ratioH->value()); };
    connect(ratio, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) {
        if (i < 0 || i >= int(std::size(presets))) return;
        double w = presets[i].w, h = presets[i].h;
        if (w < 0) { w = session_->hasDocument() ? session_->document()->width : 0; h = session_->hasDocument() ? session_->document()->height : 0; }
        canvas_->setCropRatio(w, h);
    });
    connect(ratioW, &QDoubleSpinBox::editingFinished, this, applyFields);
    connect(ratioH, &QDoubleSpinBox::editingFinished, this, applyFields);
    connect(swapRatio, &QToolButton::clicked, this, [this] { canvas_->swapCropOrientation(); });
    connect(clearRatio, &QPushButton::clicked, this, [this, ratio] { if (ratio->currentIndex() == 0) canvas_->setCropRatio(0, 0); else ratio->setCurrentIndex(0); });
    // The fields and the preset follow the canvas (a preset, typed values, X).
    connect(canvas_, &CanvasWidget::cropRatioChanged, this, [this, ratio, ratioW, ratioH] {
        const double w = canvas_->cropRatioWidth(), h = canvas_->cropRatioHeight();
        { QSignalBlocker b1(ratioW), b2(ratioH); ratioW->setValue(w); ratioH->setValue(h); }
        int match = 0;
        for (int i = 2; i < int(std::size(presets)); i++) if (presets[i].w == w && presets[i].h == h) match = i;
        if (match == 0 && w > 0 && session_->hasDocument() && w == session_->document()->width && h == session_->document()->height) match = 1;
        QSignalBlocker b(ratio);
        ratio->setCurrentIndex(match);
    });
    h->addWidget(ratio);
    h->addWidget(ratioW);
    h->addWidget(swapRatio);
    h->addWidget(ratioH);
    h->addWidget(clearRatio);
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
    h->addStretch();
    return w;
}

QWidget* ToolOptionsBar::buildEyedropperOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    h->addStretch();
    return w;
}

} // namespace app
