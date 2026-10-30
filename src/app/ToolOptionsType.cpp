// The Type tool's options (ToolOptionsBar.h): the font, size, colour and alignment of the letters being typed.
#include "ToolOptionsBar.h"
#include "ToolOptionsParts.h"
#include "EditorSession.h"
#include <QColorDialog>
#include "FontPicker.h"
#include "CanvasWidget.h"
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <algorithm>

using namespace nekophoto;

namespace app {

using namespace tooloptions;

void ToolOptionsBar::applyTextStyle(const std::function<void(LayerText&)>& change) {
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
}

QWidget* ToolOptionsBar::buildTextOptions() {
    // The style new text starts with; a live text layer that is active takes each change straight away.
    QWidget* w = row();
    auto* h = layoutOf(w);
    auto applyStyle = [this](std::function<void(LayerText&)> change) { applyTextStyle(change); };
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

} // namespace app
