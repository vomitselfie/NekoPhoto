#include "HdrDialogs.h"
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>
#include <cmath>

namespace app {

namespace {

/// A spin box with a slider beside it (the slider in hundredths).
struct Field {
    QDoubleSpinBox* spin;
    QSlider* slider;
    QWidget* row;
};

Field field(double minimum, double maximum, double value, int decimals) {
    Field f;
    f.row = new QWidget;
    auto* layout = new QHBoxLayout(f.row);
    layout->setContentsMargins(0, 0, 0, 0);
    f.slider = new QSlider(Qt::Horizontal);
    f.slider->setRange(int(std::lround(minimum * 100)), int(std::lround(maximum * 100)));
    f.slider->setValue(int(std::lround(value * 100)));
    f.slider->setMinimumWidth(220);
    f.spin = new QDoubleSpinBox;
    f.spin->setRange(minimum, maximum);
    f.spin->setDecimals(decimals);
    f.spin->setSingleStep(decimals > 1 ? 0.01 : 0.1);
    f.spin->setValue(value);
    QObject::connect(f.slider, &QSlider::valueChanged, f.spin, [spin = f.spin](int v) { if (std::lround(spin->value() * 100) != v) spin->setValue(v / 100.0); });
    QObject::connect(f.spin, qOverload<double>(&QDoubleSpinBox::valueChanged), f.slider, [slider = f.slider](double v) { slider->setValue(int(std::lround(v * 100))); });
    layout->addWidget(f.slider, 1);
    layout->addWidget(f.spin);
    return f;
}

std::optional<compositor::View32> askView32(QWidget* parent, const QString& title, const QString& hint, const compositor::View32& initial,
                                            const std::function<void(const compositor::View32&)>& preview) {
    using compositor::ToneMethod;
    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    auto* layout = new QVBoxLayout(&dialog);
    auto* form = new QFormLayout;
    auto* method = new QComboBox;
    method->addItem(QObject::tr("Exposure and Gamma"), int(ToneMethod::ExposureGamma));
    method->addItem(QObject::tr("Highlight Compression"), int(ToneMethod::HighlightCompression));
    method->setCurrentIndex(initial.method == ToneMethod::HighlightCompression ? 1 : 0);
    form->addRow(QObject::tr("Method"), method);
    Field exposure = field(compositor::View32::minExposure, compositor::View32::maxExposure, initial.exposure, 2);
    Field gamma = field(compositor::View32::minGamma, compositor::View32::maxGamma, initial.gamma, 2);
    form->addRow(QObject::tr("Exposure"), exposure.row);
    form->addRow(QObject::tr("Gamma"), gamma.row);
    layout->addLayout(form);
    if (!hint.isEmpty()) {
        auto* note = new QLabel(hint);
        note->setWordWrap(true);
        note->setEnabled(false);
        layout->addWidget(note);
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QPushButton* reset = buttons->addButton(QObject::tr("Reset"), QDialogButtonBox::ResetRole);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    auto current = [&] {
        compositor::View32 v;
        v.method = ToneMethod(method->currentData().toInt());
        // Highlight Compression takes no settings (Photoshop's): exposure and gamma apply to Exposure and Gamma only.
        if (v.method == ToneMethod::ExposureGamma) { v.exposure = exposure.spin->value(); v.gamma = gamma.spin->value(); }
        return v.clamped();
    };
    // Its fields are greyed under Highlight Compression.
    auto update = [&] {
        const bool manual = method->currentIndex() == 0;
        exposure.row->setEnabled(manual);
        gamma.row->setEnabled(manual);
        if (preview) preview(current());
    };
    QObject::connect(method, qOverload<int>(&QComboBox::currentIndexChanged), &dialog, [&](int) { update(); });
    QObject::connect(exposure.spin, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, [&](double) { update(); });
    QObject::connect(gamma.spin, qOverload<double>(&QDoubleSpinBox::valueChanged), &dialog, [&](double) { update(); });
    QObject::connect(reset, &QPushButton::clicked, &dialog, [&] {
        method->setCurrentIndex(0);
        exposure.spin->setValue(0);
        gamma.spin->setValue(1);
    });
    update();
    if (dialog.exec() != QDialog::Accepted) return std::nullopt;
    return current();
}

} // namespace

std::optional<compositor::View32> askPreviewOptions(QWidget* parent, const compositor::View32& initial, const std::function<void(const compositor::View32&)>& preview) {
    return askView32(parent, QObject::tr("32-bit Preview Options"),
                     QObject::tr("How the canvas shows a 32-bit document's values; the pixels are not changed."), initial, preview);
}

std::optional<compositor::View32> askHdrToning(QWidget* parent, int bits, const compositor::View32& initial, const std::function<void(const compositor::View32&)>& preview) {
    return askView32(parent, QObject::tr("HDR Toning"),
                     QObject::tr("Converting to %1 bits per channel: the canvas shows the result. Layers are kept; values above white are clipped.").arg(bits),
                     initial, preview);
}

} // namespace app
