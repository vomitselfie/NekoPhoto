// The Brush and Eraser's options (ToolOptionsBar.h), and the tip fields the painting tools share.
#include "ToolOptionsBar.h"
#include "ToolOptionsParts.h"
#include "EditorSession.h"
#include "BrushImporter.h"
#include "BrushDynamicsDialog.h"
#include "BrushLibrary.h"
#include "BrushPicker.h"
#include "Icons.h"
#include <QButtonGroup>
#include <QLabel>
#include <QHBoxLayout>
#include <QToolButton>

using namespace nekophoto;

namespace app {

using namespace tooloptions;

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

} // namespace app
