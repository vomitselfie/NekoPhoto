#include "ChannelDialogs.h"
#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

using namespace compositor;

namespace app {

namespace {

/// The four operations as radio buttons in a group box, ids 0..3 (SelectionMode's order).
QButtonGroup* operationGroup(QWidget* parent, QVBoxLayout* layout, const QString& title, const QStringList& labels) {
    auto* box = new QGroupBox(title, parent);
    auto* v = new QVBoxLayout(box);
    auto* group = new QButtonGroup(parent);
    for (int i = 0; i < labels.size(); i++) {
        auto* b = new QRadioButton(labels[i], box);
        group->addButton(b, i);
        v->addWidget(b);
    }
    group->button(0)->setChecked(true);
    layout->addWidget(box);
    return group;
}

SelectionMode modeFor(int id) {
    switch (id) {
    case 1: return SelectionMode::Add;
    case 2: return SelectionMode::Subtract;
    case 3: return SelectionMode::Intersect;
    default: return SelectionMode::Replace;
    }
}

} // namespace

// ---- Save Selection ------------------------------------------------------------------------------------------

SaveSelectionDialog::SaveSelectionDialog(EditorSession* session, QWidget* parent) : QDialog(parent), session_(session) {
    setWindowTitle(tr("Save Selection"));
    setMinimumWidth(320);
    setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QVBoxLayout(this);
    auto* destination = new QGroupBox(tr("Destination"), this);
    auto* form = new QFormLayout(destination);
    auto* document = new QLabel(tr("This document"), destination);
    form->addRow(tr("Document:"), document);
    channel_ = new QComboBox(destination);
    channel_->addItem(tr("New"), QString());
    if (session_ && session_->document())
        for (const Channel& c : session_->document()->channels)
            if (c.kind == ChannelKind::Alpha) channel_->addItem(QString::fromStdString(c.name), QString::fromStdString(c.id));
    form->addRow(tr("Channel:"), channel_);
    name_ = new QLineEdit(destination);
    if (session_ && session_->document())
        name_->setText(QString::fromStdString(nextChannelName(*session_->document(), QCoreApplication::translate("Names", "Alpha").toStdString())));
    form->addRow(tr("Name:"), name_);
    layout->addWidget(destination);
    operation_ = operationGroup(this, layout, tr("Operation"), {tr("New Channel"), tr("Add to Channel"), tr("Subtract from Channel"), tr("Intersect with Channel")});
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { apply(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(channel_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { refresh(); });
    refresh();
}

void SaveSelectionDialog::refresh() {
    // Into a new channel: only its name matters. Into an existing one: the operation, with Replace as its first.
    const bool isNew = channel_->currentData().toString().isEmpty();
    name_->setEnabled(isNew);
    operation_->button(0)->setText(isNew ? tr("New Channel") : tr("Replace Channel"));
    for (int i = 1; i < 4; i++) operation_->button(i)->setEnabled(!isNew);
    if (isNew) operation_->button(0)->setChecked(true);
}

void SaveSelectionDialog::apply() {
    if (!session_) { reject(); return; }
    const QString id = channel_->currentData().toString();
    QString error;
    const auto saved = session_->saveSelectionToChannel(id.isEmpty() ? std::nullopt : std::optional<Uuid>(id.toStdString()), name_->text(),
                                                        modeFor(operation_->checkedId()), &error);
    if (!saved) { QMessageBox::information(this, windowTitle(), error.isEmpty() ? tr("The selection could not be saved.") : error); return; }
    accept();
}

// ---- Load Selection ------------------------------------------------------------------------------------------

LoadSelectionDialog::LoadSelectionDialog(EditorSession* session, QWidget* parent) : QDialog(parent), session_(session) {
    setWindowTitle(tr("Load Selection"));
    setMinimumWidth(320);
    setAttribute(Qt::WA_DeleteOnClose);
    auto* layout = new QVBoxLayout(this);
    auto* sourceBox = new QGroupBox(tr("Source"), this);
    auto* form = new QFormLayout(sourceBox);
    form->addRow(tr("Document:"), new QLabel(tr("This document"), sourceBox));
    source_ = new QComboBox(sourceBox);
    // The alpha channels, then each layer's transparency and mask (Photoshop's list), the active layer's first.
    if (session_ && session_->document()) {
        const Document& doc = *session_->document();
        for (const Channel& c : doc.channels)
            source_->addItem(QString::fromStdString(c.name), QStringList{"channel", QString::fromStdString(c.id)});
        std::vector<const Layer*> layers;
        for (auto it = doc.layers.rbegin(); it != doc.layers.rend(); ++it)
            if (!it->isGroup && !session_->isChannelProxy(it->id)) layers.push_back(&*it);
        std::stable_partition(layers.begin(), layers.end(), [&](const Layer* l) { return session_->activeLayerId() == l->id; });
        for (const Layer* l : layers) {
            source_->addItem(tr("%1 Transparency").arg(QString::fromStdString(l->name)), QStringList{"transparency", QString::fromStdString(l->id)});
            if (l->mask) source_->addItem(tr("%1 Mask").arg(QString::fromStdString(l->name)), QStringList{"mask", QString::fromStdString(l->id)});
        }
    }
    form->addRow(tr("Channel:"), source_);
    invert_ = new QCheckBox(tr("Invert"), sourceBox);
    form->addRow(QString(), invert_);
    layout->addWidget(sourceBox);
    operation_ = operationGroup(this, layout, tr("Operation"), {tr("New Selection"), tr("Add to Selection"), tr("Subtract from Selection"), tr("Intersect with Selection")});
    // Without a selection there is nothing to add to, subtract from or intersect with.
    const bool hasSelection = session_ && session_->document() && session_->document()->selection;
    for (int i = 1; i < 4; i++) operation_->button(i)->setEnabled(hasSelection);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setEnabled(source_->count() > 0);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { apply(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void LoadSelectionDialog::apply() {
    if (!session_ || source_->currentIndex() < 0) { reject(); return; }
    const QStringList data = source_->currentData().toStringList();
    if (data.size() != 2) { reject(); return; }
    SelectionSource source;
    source.kind = data[0] == "channel" ? SelectionSource::AlphaChannel : data[0] == "mask" ? SelectionSource::LayerMask : SelectionSource::Transparency;
    source.id = data[1].toStdString();
    QString error;
    if (!session_->loadSelectionFromSource(source, invert_->isChecked(), modeFor(operation_->checkedId()), &error)) {
        QMessageBox::information(this, windowTitle(), error.isEmpty() ? tr("The selection could not be loaded.") : error);
        return;
    }
    accept();
}

// ---- Channel Options ------------------------------------------------------------------------------------------

ChannelOptionsDialog::ChannelOptionsDialog(EditorSession* session, const Uuid& channel, QWidget* parent) : QDialog(parent), session_(session), id_(channel) {
    setWindowTitle(tr("Channel Options"));
    setMinimumWidth(300);
    setAttribute(Qt::WA_DeleteOnClose);
    const Channel* c = session_ && session_->document() ? findChannel(*session_->document(), id_) : nullptr;
    const bool spot = c && c->kind == ChannelKind::Spot;
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    name_ = new QLineEdit(c ? QString::fromStdString(c->name) : QString(), this);
    form->addRow(tr("Name:"), name_);
    layout->addLayout(form);
    auto* indicates = new QGroupBox(tr("Color Indicates:"), this);
    auto* v = new QVBoxLayout(indicates);
    masked_ = new QRadioButton(tr("Masked Areas"), indicates);
    selected_ = new QRadioButton(tr("Selected Areas"), indicates);
    auto* spotColor = new QRadioButton(tr("Spot Color"), indicates);
    v->addWidget(masked_);
    v->addWidget(selected_);
    v->addWidget(spotColor);
    // A spot channel's kind is its own (carried from the file, not edited in this version).
    if (spot) { spotColor->setChecked(true); masked_->setEnabled(false); selected_->setEnabled(false); }
    else { (c && c->selectedAreas ? selected_ : masked_)->setChecked(true); spotColor->setEnabled(false); }
    layout->addWidget(indicates);
    auto* colorBox = new QGroupBox(tr("Color"), this);
    auto* colorForm = new QFormLayout(colorBox);
    swatch_ = new QPushButton(colorBox);
    swatch_->setFixedSize(48, 24);
    swatch_->setToolTip(tr("The colour the channel shows in over the image"));
    colorForm->addRow(tr("Color:"), swatch_);
    opacity_ = new QDoubleSpinBox(colorBox);
    opacity_->setRange(0, 100);
    opacity_->setDecimals(0);
    opacity_->setSuffix(QStringLiteral(" %"));
    opacity_->setValue(c ? c->opacity * 100 : 50);
    colorForm->addRow(spot ? tr("Solidity:") : tr("Opacity:"), opacity_);
    layout->addWidget(colorBox);
    setSwatch(c ? QColor::fromRgbF(float(c->color[0]), float(c->color[1]), float(c->color[2])) : QColor(255, 0, 0));
    connect(swatch_, &QPushButton::clicked, this, [this] {
        const QColor chosen = QColorDialog::getColor(color_, this, tr("Channel Color"));
        if (chosen.isValid()) setSwatch(chosen);
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { apply(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void ChannelOptionsDialog::setSwatch(const QColor& color) {
    color_ = color;
    swatch_->setStyleSheet(QStringLiteral("background-color: %1; border: 1px solid palette(mid);").arg(color.name()));
}

void ChannelOptionsDialog::apply() {
    if (session_) session_->setChannelOptions(id_, name_->text(), color_, opacity_->value() / 100.0, selected_->isChecked());
    accept();
}

} // namespace app
