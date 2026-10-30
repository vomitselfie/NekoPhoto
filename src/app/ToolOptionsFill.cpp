// The Gradient tool's and the Paint Bucket's options (ToolOptionsBar.h).
#include "ToolOptionsBar.h"
#include "ToolOptionsParts.h"
#include "EditorSession.h"
#include "PresetLibrary.h"
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>

using namespace nekophoto;

namespace app {

using namespace tooloptions;

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
    syncers_.push_back([this, tolerance] { QSignalBlocker b(tolerance); tolerance->setValue(session_->bucket.tolerance); });
    h->addWidget(tolerance);
    auto check = [&](const QString& label, bool& value) {
        auto* box = new QCheckBox(label);
        box->setChecked(value);
        connect(box, &QCheckBox::toggled, this, [&value](bool on) { value = on; });
        syncers_.push_back([box, &value] { QSignalBlocker b(box); box->setChecked(value); });
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
    syncers_.push_back([this, reverse] { QSignalBlocker b(reverse); reverse->setChecked(session_->gradientSettings.reversed); });
    h->addWidget(reverse);
    // Photoshop's Method menu, in its order: Perceptual, Linear, Classic.
    h->addWidget(new QLabel(tr("Method")));
    auto* method = new QComboBox;
    method->setProperty("role", "gradientMethod");
    method->setToolTip(tr("How the colours blend: Perceptual and Linear as Photoshop 2023 and later, Classic as before"));
    method->addItems({tr("Perceptual"), tr("Linear"), tr("Classic")});
    auto methodIndex = [this] { const GradientMethod m = session_->gradientSettings.method; return m == GradientMethod::Perceptual ? 0 : m == GradientMethod::Linear ? 1 : 2; };
    method->setCurrentIndex(methodIndex());
    connect(method, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i) {
        session_->gradientSettings.method = i == 0 ? GradientMethod::Perceptual : i == 1 ? GradientMethod::Linear : GradientMethod::Classic;
        session_->refreshGradient();
    });
    syncers_.push_back([method, methodIndex] { QSignalBlocker b(method); method->setCurrentIndex(methodIndex()); });
    h->addWidget(method);
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

} // namespace app
