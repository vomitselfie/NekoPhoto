#include "TextDialog.h"
#include "RichText.h"
#include "TextLayer.h"
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextEdit>
#include <QToolButton>
#include <QVBoxLayout>

using namespace compositor;

namespace app {

namespace {

const int kWeights[] = {0, 100, 200, 300, 400, 500, 600, 700, 800, 900};

QToolButton* toggle(const QString& label, const QString& tip, void (*style)(QFont&)) {
    auto* b = new QToolButton;
    b->setText(label);
    b->setCheckable(true);
    b->setToolTip(tip);
    QFont f = b->font();
    style(f);
    b->setFont(f);
    return b;
}

QDoubleSpinBox* spin(double lo, double hi, int decimals, const QString& suffix, const QString& tip) {
    auto* s = new QDoubleSpinBox;
    s->setRange(lo, hi);
    s->setDecimals(decimals);
    s->setSuffix(suffix);
    s->setToolTip(tip);
    s->setKeyboardTracking(false);
    return s;
}

/// A value the runs share, or nothing when they differ.
template <typename T, typename F> std::optional<T> shared(const std::vector<TextRun>& runs, F field) {
    if (runs.empty()) return std::nullopt;
    const T first = field(runs.front());
    for (const TextRun& r : runs) if (!(field(r) == first)) return std::nullopt;
    return first;
}

void showNumber(QDoubleSpinBox* s, std::optional<double> v) {
    if (v) s->setValue(*v);
    else s->clear();   // mixed: blank
}

} // namespace

TextDialog::TextDialog(EditorSession* session, Uuid layerId, QWidget* parent)
    : QDialog(parent), session_(session), layerId_(std::move(layerId)) {
    setWindowTitle(tr("Text"));
    setModal(false);
    setAttribute(Qt::WA_DeleteOnClose);
    std::optional<LayerText> current = session_->layerText(layerId_);
    original_ = current ? *current : session_->textStyle;
    session_->beginTextEdit(layerId_);
    const std::vector<TextRun> runs = textRuns(original_);
    fallback_ = runs.empty() ? baseTextRun(original_) : runs.front();
    if (fallback_.fontFamily.empty()) fallback_.fontFamily = original_.fontFamily;
    // The editor shows the sizes relative to each other, the largest at about 36 pixels at most.
    double largest = original_.fontSize;
    for (const TextRun& r : runs) largest = std::max(largest, r.fontSize);
    displayScale_ = largest > 36 ? 36 / largest : 1;

    auto* layout = new QVBoxLayout(this);
    editor_ = new QTextEdit;
    editor_->setAcceptRichText(false);   // pasted text takes the style where it lands
    editor_->setMinimumSize(420, 130);
    editor_->setTabChangesFocus(true);
    editor_->setStyleSheet("QTextEdit { background: #f4f4f4; }");   // the text's own colours, readable
    fillTextDocument(*editor_->document(), original_, displayScale_);
    editor_->document()->clearUndoRedoStacks();
    layout->addWidget(editor_);

    auto* character = new QGroupBox(tr("Character"));
    auto* grid = new QGridLayout(character);
    family_ = new FontPicker;
    family_->setToolTip(tr("Font family"));
    grid->addWidget(family_, 0, 0, 1, 3);
    size_ = spin(1, 2000, 1, " px", tr("Size, in document pixels"));
    grid->addWidget(size_, 0, 3);
    weight_ = new QComboBox;
    weight_->addItems({tr("Auto weight"), tr("Thin"), tr("Extra Light"), tr("Light"), tr("Regular"), tr("Medium"), tr("Semibold"), tr("Bold"), tr("Extra Bold"), tr("Black")});
    weight_->setToolTip(tr("The face's weight (Auto: regular, or bold with B)"));
    grid->addWidget(weight_, 0, 4);
    auto* styles = new QHBoxLayout;
    bold_ = toggle(tr("B"), tr("Bold"), [](QFont& f) { f.setBold(true); });
    italic_ = toggle(tr("I"), tr("Italic"), [](QFont& f) { f.setItalic(true); });
    underline_ = toggle(tr("U"), tr("Underline"), [](QFont& f) { f.setUnderline(true); });
    strike_ = toggle(tr("S"), tr("Strikethrough"), [](QFont& f) { f.setStrikeOut(true); });
    for (QToolButton* b : {bold_, italic_, underline_, strike_}) styles->addWidget(b);
    styles->addStretch();
    grid->addLayout(styles, 0, 5);

    grid->addWidget(new QLabel(tr("Tracking")), 1, 0);
    tracking_ = spin(-100, 500, 2, " px", tr("Extra space after each letter, in pixels (Photoshop's tracking)"));
    grid->addWidget(tracking_, 1, 1);
    grid->addWidget(new QLabel(tr("Baseline")), 1, 2);
    baseline_ = spin(-1000, 1000, 1, " px", tr("Baseline shift, in pixels up"));
    grid->addWidget(baseline_, 1, 3);
    grid->addWidget(new QLabel(tr("Leading")), 2, 0);
    leading_ = spin(0, 5000, 1, " px", tr("Baseline to baseline for lines holding these letters (the largest on a line wins); Auto: 1.2 x size"));
    leading_->setSpecialValueText(tr("Auto"));
    grid->addWidget(leading_, 2, 1);
    caps_ = new QComboBox;
    caps_->addItems({tr("Normal case"), tr("Small Caps"), tr("All Caps")});
    caps_->setToolTip(tr("Capitals"));
    grid->addWidget(caps_, 2, 2, 1, 2);
    colour_ = new QPushButton(tr("Colour"));
    colour_->setToolTip(tr("The letters' colour"));
    grid->addWidget(colour_, 1, 4, 2, 2);
    layout->addWidget(character);

    auto* paragraph = new QHBoxLayout;
    alignment_ = new QComboBox;
    alignment_->addItems({tr("Left"), tr("Centre"), tr("Right")});
    alignment_->setCurrentIndex(std::clamp(original_.alignment, 0, 2));
    alignment_->setToolTip(tr("Alignment of the lines"));
    paragraph->addWidget(alignment_);
    paragraph->addWidget(new QLabel(tr("Line spacing")));
    lineSpacing_ = spin(0.5, 5, 2, "", tr("Line spacing, as a multiple of the font's line height"));
    lineSpacing_->setSingleStep(0.1);
    lineSpacing_->setValue(original_.lineSpacing);
    paragraph->addWidget(lineSpacing_);
    paragraph->addStretch();
    layout->addLayout(paragraph);
    auto* note = new QLabel(tr("Select letters to style them; with nothing selected, a change applies to all the text."));
    note->setWordWrap(true);
    layout->addWidget(note);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    debounce_.setSingleShot(true);
    debounce_.setInterval(60);
    connect(&debounce_, &QTimer::timeout, this, &TextDialog::apply);
    auto schedule = [this] { if (!syncing_) debounce_.start(); };
    connect(editor_->document(), &QTextDocument::contentsChanged, this, schedule);
    connect(editor_, &QTextEdit::cursorPositionChanged, this, &TextDialog::showCharacter);
    connect(editor_, &QTextEdit::selectionChanged, this, &TextDialog::showCharacter);
    auto patchWith = [this](auto set) { return [this, set](auto value) { if (syncing_) return; TextRunPatch p; set(p, value); restyle(p); }; };
    connect(family_, &FontPicker::familyChanged, this, patchWith([](TextRunPatch& p, const QString& f) { p.fontFamily = f.toStdString(); }));
    connect(size_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, patchWith([](TextRunPatch& p, double v) { p.fontSize = v; }));
    connect(weight_, QOverload<int>::of(&QComboBox::activated), this, patchWith([](TextRunPatch& p, int i) { p.weight = kWeights[std::clamp(i, 0, 9)]; }));
    connect(bold_, &QToolButton::clicked, this, patchWith([](TextRunPatch& p, bool on) { p.bold = on; }));
    connect(italic_, &QToolButton::clicked, this, patchWith([](TextRunPatch& p, bool on) { p.italic = on; }));
    connect(underline_, &QToolButton::clicked, this, patchWith([](TextRunPatch& p, bool on) { p.underline = on; }));
    connect(strike_, &QToolButton::clicked, this, patchWith([](TextRunPatch& p, bool on) { p.strikethrough = on; }));
    connect(tracking_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, patchWith([](TextRunPatch& p, double v) { p.letterSpacing = v; }));
    connect(baseline_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, patchWith([](TextRunPatch& p, double v) { p.baselineShift = v; }));
    connect(leading_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, patchWith([](TextRunPatch& p, double v) { p.leading = v; }));
    connect(caps_, QOverload<int>::of(&QComboBox::activated), this, patchWith([](TextRunPatch& p, int i) { p.caps = TextRun::Caps(std::clamp(i, 0, 2)); }));
    connect(alignment_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, schedule);
    connect(lineSpacing_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, schedule);
    connect(colour_, &QPushButton::clicked, this, &TextDialog::pickColour);
    connect(session_, &EditorSession::layersChanged, this, [this] { if (!finished_ && session_ && !session_->layerText(layerId_)) reject(); });
    connect(session_, &QObject::destroyed, this, [this] { finished_ = true; close(); });
    editor_->setFocus();
    editor_->selectAll();
    showCharacter();
}

TextDialog::~TextDialog() { if (!finished_ && session_) session_->endTextEdit(false); }

void TextDialog::showCharacter() {
    const QTextCursor cursor = editor_->textCursor();
    std::vector<TextRun> runs = cursor.hasSelection() ? documentRuns(*editor_->document(), fallback_, cursor.selectionStart(), cursor.selectionEnd())
                                                      : std::vector<TextRun>{runFromFormat(editor_->currentCharFormat(), fallback_)};
    if (runs.empty()) runs = {fallback_};
    syncing_ = true;
    const auto family = shared<std::string>(runs, [](const TextRun& r) { return r.fontFamily; });
    family_->setFamily(family ? (family->empty() ? defaultTextFamily() : QString::fromStdString(*family)) : QString());
    showNumber(size_, shared<double>(runs, [](const TextRun& r) { return r.fontSize; }));
    const auto weight = shared<int>(runs, [](const TextRun& r) { return r.weight; });
    int weightIndex = -1;
    if (weight) for (int i = 0; i < 10; i++) if (kWeights[i] == *weight) weightIndex = i;
    weight_->setCurrentIndex(weightIndex);
    bold_->setChecked(shared<bool>(runs, [](const TextRun& r) { return r.bold; }).value_or(false));
    italic_->setChecked(shared<bool>(runs, [](const TextRun& r) { return r.italic; }).value_or(false));
    underline_->setChecked(shared<bool>(runs, [](const TextRun& r) { return r.underline; }).value_or(false));
    strike_->setChecked(shared<bool>(runs, [](const TextRun& r) { return r.strikethrough; }).value_or(false));
    showNumber(tracking_, shared<double>(runs, [](const TextRun& r) { return r.letterSpacing; }));
    showNumber(baseline_, shared<double>(runs, [](const TextRun& r) { return r.baselineShift; }));
    showNumber(leading_, shared<double>(runs, [](const TextRun& r) { return r.leading; }));
    const auto caps = shared<TextRun::Caps>(runs, [](const TextRun& r) { return r.caps; });
    caps_->setCurrentIndex(caps ? int(*caps) : -1);
    const auto colour = shared<std::array<double, 3>>(runs, [](const TextRun& r) { return std::array<double, 3>{r.red, r.green, r.blue}; });
    if (colour) {
        QPixmap swatch(14, 14);
        swatch.fill(QColor::fromRgbF(float(std::clamp((*colour)[0], 0.0, 1.0)), float(std::clamp((*colour)[1], 0.0, 1.0)), float(std::clamp((*colour)[2], 0.0, 1.0))));
        colour_->setIcon(QIcon(swatch));
    } else colour_->setIcon(QIcon());
    syncing_ = false;
}

void TextDialog::restyle(const TextRunPatch& patch) {
    QTextDocument* doc = editor_->document();
    QTextCursor selection = editor_->textCursor();
    int from = 0, to = doc->characterCount() - 1;
    if (selection.hasSelection()) { from = selection.selectionStart(); to = selection.selectionEnd(); }
    QTextCursor cursor(doc);
    cursor.beginEditBlock();
    if (from == to) {
        // No text: what typing will take.
        TextRun r = runFromFormat(doc->begin().charFormat(), fallback_);
        patch.applyTo(r);
        cursor.setBlockCharFormat(charFormatFor(r, displayScale_));
        editor_->setCurrentCharFormat(charFormatFor(r, displayScale_));
    }
    int at = from;
    for (TextRun r : documentRuns(*doc, fallback_, from, to)) {
        const int length = r.length;
        patch.applyTo(r);
        cursor.setPosition(at);
        cursor.setPosition(at + length, QTextCursor::KeepAnchor);
        cursor.setCharFormat(charFormatFor(r, displayScale_));
        at += length;
    }
    cursor.endEditBlock();
    if (!selection.hasSelection() && from != to) {
        // The cursor's own format follows (typing at it takes the new style).
        TextRun r = runFromFormat(editor_->currentCharFormat(), fallback_);
        patch.applyTo(r);
        editor_->setCurrentCharFormat(charFormatFor(r, displayScale_));
    }
    showCharacter();
    debounce_.start();
}

void TextDialog::apply() {
    if (!session_) return;
    LayerText text = layerTextFromDocument(*editor_->document(), original_, fallback_);
    text.alignment = alignment_->currentIndex();
    text.lineSpacing = lineSpacing_->value();
    session_->setLayerText(layerId_, text);
    // The style carries over to the next text.
    LayerText next = text;
    next.text.clear();
    next.runs.clear();
    session_->textStyle = next;
}

void TextDialog::pickColour() {
    const TextRun at = runFromFormat(editor_->currentCharFormat(), fallback_);
    QColor chosen = QColorDialog::getColor(QColor::fromRgbF(float(at.red), float(at.green), float(at.blue)), this, tr("Text Colour"));
    if (!chosen.isValid()) return;
    TextRunPatch p;
    p.color = std::array<double, 3>{chosen.redF(), chosen.greenF(), chosen.blueF()};
    restyle(p);
}

void TextDialog::done(int result) {
    if (finished_) { QDialog::done(result); return; }
    finished_ = true;
    if (debounce_.isActive()) { debounce_.stop(); apply(); }
    if (session_) session_->endTextEdit(result == QDialog::Accepted);
    QDialog::done(result);
}

} // namespace app
