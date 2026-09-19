#include "TextDialog.h"
#include "TextLayer.h"
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

using namespace compositor;

namespace app {

TextDialog::TextDialog(EditorSession* session, Uuid layerId, QWidget* parent)
    : QDialog(parent), session_(session), layerId_(std::move(layerId)) {
    setWindowTitle(tr("Text"));
    setModal(false);
    setAttribute(Qt::WA_DeleteOnClose);
    std::optional<LayerText> current = session_->layerText(layerId_);
    text_ = current ? *current : session_->textStyle;
    session_->beginTextEdit(layerId_);

    auto* layout = new QVBoxLayout(this);
    editor_ = new QPlainTextEdit;
    editor_->setPlainText(QString::fromStdString(text_.text));
    editor_->setMinimumSize(360, 110);
    editor_->setTabChangesFocus(true);
    layout->addWidget(editor_);

    auto* fontRow = new QHBoxLayout;
    family_ = new FontPicker;
    family_->setFamily(fontFor(text_).family());
    fontRow->addWidget(family_, 1);
    size_ = new QDoubleSpinBox;
    size_->setRange(1, 2000);
    size_->setDecimals(0);
    size_->setSuffix(" px");
    size_->setValue(text_.fontSize);
    size_->setToolTip(tr("Size, in document pixels"));
    fontRow->addWidget(size_);
    bold_ = new QToolButton;
    bold_->setText(tr("B"));
    bold_->setCheckable(true);
    bold_->setChecked(text_.bold);
    bold_->setToolTip(tr("Bold"));
    QFont b = bold_->font(); b.setBold(true); bold_->setFont(b);
    fontRow->addWidget(bold_);
    italic_ = new QToolButton;
    italic_->setText(tr("I"));
    italic_->setCheckable(true);
    italic_->setChecked(text_.italic);
    italic_->setToolTip(tr("Italic"));
    QFont it = italic_->font(); it.setItalic(true); italic_->setFont(it);
    fontRow->addWidget(italic_);
    layout->addLayout(fontRow);

    auto* styleRow = new QHBoxLayout;
    alignment_ = new QComboBox;
    alignment_->addItems({tr("Left"), tr("Centre"), tr("Right")});
    alignment_->setCurrentIndex(std::clamp(text_.alignment, 0, 2));
    alignment_->setToolTip(tr("Alignment of the lines"));
    styleRow->addWidget(alignment_);
    styleRow->addWidget(new QLabel(tr("Line")));
    lineSpacing_ = new QDoubleSpinBox;
    lineSpacing_->setRange(0.5, 5);
    lineSpacing_->setSingleStep(0.1);
    lineSpacing_->setValue(text_.lineSpacing);
    lineSpacing_->setToolTip(tr("Line spacing, as a multiple of the font's line height"));
    styleRow->addWidget(lineSpacing_);
    styleRow->addWidget(new QLabel(tr("Letter")));
    letterSpacing_ = new QDoubleSpinBox;
    letterSpacing_->setRange(-20, 100);
    letterSpacing_->setSuffix(" px");
    letterSpacing_->setValue(text_.letterSpacing);
    letterSpacing_->setToolTip(tr("Extra space between letters, in pixels"));
    styleRow->addWidget(letterSpacing_);
    colour_ = new QPushButton(tr("Colour"));
    colour_->setToolTip(tr("The text's colour"));
    styleRow->addWidget(colour_);
    layout->addLayout(styleRow);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    debounce_.setSingleShot(true);
    debounce_.setInterval(60);
    connect(&debounce_, &QTimer::timeout, this, &TextDialog::apply);
    auto schedule = [this] { debounce_.start(); };
    connect(editor_, &QPlainTextEdit::textChanged, this, schedule);
    connect(family_, &FontPicker::familyChanged, this, schedule);
    connect(size_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, schedule);
    connect(bold_, &QToolButton::toggled, this, schedule);
    connect(italic_, &QToolButton::toggled, this, schedule);
    connect(alignment_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, schedule);
    connect(lineSpacing_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, schedule);
    connect(letterSpacing_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, schedule);
    connect(colour_, &QPushButton::clicked, this, &TextDialog::pickColour);
    connect(session_, &EditorSession::layersChanged, this, [this] { if (!finished_ && session_ && !session_->layerText(layerId_)) reject(); });
    connect(session_, &QObject::destroyed, this, [this] { finished_ = true; close(); });
    editor_->setFocus();
    editor_->selectAll();
}

TextDialog::~TextDialog() { if (!finished_ && session_) session_->endTextEdit(false); }

void TextDialog::apply() {
    if (!session_) return;
    text_.text = editor_->toPlainText().toStdString();
    text_.fontFamily = family_->family().toStdString();
    text_.fontSize = size_->value();
    text_.bold = bold_->isChecked();
    text_.italic = italic_->isChecked();
    text_.alignment = alignment_->currentIndex();
    text_.lineSpacing = lineSpacing_->value();
    text_.letterSpacing = letterSpacing_->value();
    session_->setLayerText(layerId_, text_);
    // The style carries over to the next text.
    LayerText next = text_;
    next.text.clear();
    session_->textStyle = next;
}

void TextDialog::pickColour() {
    QColor chosen = QColorDialog::getColor(QColor::fromRgbF(float(text_.red), float(text_.green), float(text_.blue)), this, tr("Text Colour"));
    if (!chosen.isValid()) return;
    text_.red = chosen.redF(); text_.green = chosen.greenF(); text_.blue = chosen.blueF();
    apply();
}

void TextDialog::done(int result) {
    if (finished_) { QDialog::done(result); return; }
    finished_ = true;
    if (debounce_.isActive()) { debounce_.stop(); apply(); }
    if (session_) session_->endTextEdit(result == QDialog::Accepted);
    QDialog::done(result);
}

} // namespace app
