#include "WarpDialog.h"
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QMessageBox>
#include <QSpinBox>
#include <QVBoxLayout>

namespace app {

const std::vector<std::pair<const char*, const char*>>& WarpDialog::styles() {
    static const std::vector<std::pair<const char*, const char*>> list = {
        {QT_TRANSLATE_NOOP("WarpDialog", "Arc"), "warpArc"}, {QT_TRANSLATE_NOOP("WarpDialog", "Arc Lower"), "warpArcLower"},
        {QT_TRANSLATE_NOOP("WarpDialog", "Arc Upper"), "warpArcUpper"}, {QT_TRANSLATE_NOOP("WarpDialog", "Arch"), "warpArch"},
        {QT_TRANSLATE_NOOP("WarpDialog", "Bulge"), "warpBulge"}, {QT_TRANSLATE_NOOP("WarpDialog", "Shell Lower"), "warpShellLower"},
        {QT_TRANSLATE_NOOP("WarpDialog", "Shell Upper"), "warpShellUpper"}, {QT_TRANSLATE_NOOP("WarpDialog", "Flag"), "warpFlag"},
        {QT_TRANSLATE_NOOP("WarpDialog", "Wave"), "warpWave"}, {QT_TRANSLATE_NOOP("WarpDialog", "Fish"), "warpFish"},
        {QT_TRANSLATE_NOOP("WarpDialog", "Rise"), "warpRise"}, {QT_TRANSLATE_NOOP("WarpDialog", "Fisheye"), "warpFisheye"},
        {QT_TRANSLATE_NOOP("WarpDialog", "Inflate"), "warpInflate"}, {QT_TRANSLATE_NOOP("WarpDialog", "Squeeze"), "warpSqueeze"},
        {QT_TRANSLATE_NOOP("WarpDialog", "Twist"), "warpTwist"},
    };
    return list;
}

WarpDialog::WarpDialog(EditorSession* session, QWidget* parent) : QDialog(parent), session_(session) {
    setWindowTitle(tr("Warp"));
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    style_ = new QComboBox;
    // Text can lose its warp; other layers are bent for good (or baked into a smart object).
    const compositor::Layer* layer = session->activeLayer();
    const bool text = layer && layer->isLiveText();
    if (text) style_->addItem(tr("None"), QString());
    for (auto& [name, id] : styles()) style_->addItem(tr(name), QString::fromLatin1(id));
    if (text && layer->text->warp.active()) {
        const int at = style_->findData(QString::fromStdString(layer->text->warp.style));
        if (at >= 0) style_->setCurrentIndex(at);
    } else if (text) style_->setCurrentIndex(1);
    form->addRow(tr("Style"), style_);
    orientation_ = new QComboBox;
    orientation_->addItems({tr("Horizontal"), tr("Vertical")});
    form->addRow(tr("Orientation"), orientation_);
    auto spin = [](int value) { auto* s = new QSpinBox; s->setRange(-100, 100); s->setSuffix(" %"); s->setValue(value); return s; };
    const compositor::TextWarp current = text ? layer->text->warp : compositor::TextWarp{};
    bend_ = spin(current.active() ? int(current.bend) : 50);
    horizontal_ = spin(int(current.horizontal));
    vertical_ = spin(int(current.vertical));
    if (current.verticalOrientation) orientation_->setCurrentIndex(1);
    form->addRow(tr("Bend"), bend_);
    form->addRow(tr("Horizontal distortion"), horizontal_);
    form->addRow(tr("Vertical distortion"), vertical_);
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void WarpDialog::accept() {
    if (!session_) { QDialog::accept(); return; }
    compositor::TextWarp warp;
    warp.style = style_->currentData().toString().toStdString();
    warp.bend = bend_->value();
    warp.horizontal = horizontal_->value();
    warp.vertical = vertical_->value();
    warp.verticalOrientation = orientation_->currentIndex() == 1;
    QString error;
    if (!session_->warpActiveLayer(warp, &error)) { QMessageBox::warning(this, tr("Warp"), error); return; }
    QDialog::accept();
}

} // namespace app
