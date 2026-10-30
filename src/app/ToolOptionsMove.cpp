// The Move tool's options (ToolOptionsBar.h): Auto-Select, the transform controls and the transform's fields.
#include "ToolOptionsBar.h"
#include "ToolOptionsParts.h"
#include "EditorSession.h"
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>
#include <algorithm>

using namespace nekophoto;

namespace app {

using namespace tooloptions;

QWidget* ToolOptionsBar::buildMoveOptions() {
    QWidget* w = row();
    auto* h = layoutOf(w);
    auto* autoSelect = new QCheckBox(tr("Auto-Select"));
    autoSelect->setToolTip(tr("Click picks the layer under the pointer (or hold Ctrl)"));
    autoSelect->setChecked(session_->transformAutoSelect);
    connect(autoSelect, &QCheckBox::toggled, this, [this](bool on) { session_->transformAutoSelect = on; });
    syncers_.push_back([this, autoSelect] { QSignalBlocker b(autoSelect); autoSelect->setChecked(session_->transformAutoSelect); });
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
    connect(apply, &QPushButton::clicked, this, [this] { session_->commitTransformCommand(); });
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

} // namespace app
