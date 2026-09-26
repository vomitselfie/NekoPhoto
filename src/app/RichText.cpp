#include "RichText.h"
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <algorithm>
#include <cmath>

using namespace compositor;

namespace app {

namespace {

enum Property {
    Holds = QTextFormat::UserProperty + 0x5100, Family, Size, Bold, Italic, Weight, Red, Green, Blue,
    LetterSpacing, BaselineShift, Leading, Caps, Underline, Strikethrough,
};

void append(std::vector<TextRun>& runs, TextRun run, int length) {
    if (length <= 0) return;
    run.length = length;
    if (!runs.empty()) {
        TextRun last = runs.back();
        last.length = length;
        if (last == run) { runs.back().length += length; return; }
    }
    runs.push_back(run);
}

} // namespace

QTextCharFormat charFormatFor(const TextRun& r, double displayScale) {
    QTextCharFormat f;
    f.setProperty(Holds, true);
    f.setProperty(Family, QString::fromStdString(r.fontFamily));
    f.setProperty(Size, r.fontSize);
    f.setProperty(Bold, r.bold);
    f.setProperty(Italic, r.italic);
    f.setProperty(Weight, r.weight);
    f.setProperty(Red, r.red);
    f.setProperty(Green, r.green);
    f.setProperty(Blue, r.blue);
    f.setProperty(LetterSpacing, r.letterSpacing);
    f.setProperty(BaselineShift, r.baselineShift);
    f.setProperty(Leading, r.leading);
    f.setProperty(Caps, int(r.caps));
    f.setProperty(Underline, r.underline);
    f.setProperty(Strikethrough, r.strikethrough);
    // How it looks in the editor.
    if (!r.fontFamily.empty()) f.setFontFamilies(QStringList{QString::fromStdString(r.fontFamily)});
    f.setProperty(QTextFormat::FontPixelSize, std::clamp(int(std::lround(r.fontSize * displayScale)), 6, 96));
    f.setFontWeight(r.weight > 0 ? r.weight : r.bold ? int(QFont::Bold) : int(QFont::Normal));
    f.setFontItalic(r.italic);
    f.setForeground(QColor::fromRgbF(float(std::clamp(r.red, 0.0, 1.0)), float(std::clamp(r.green, 0.0, 1.0)), float(std::clamp(r.blue, 0.0, 1.0))));
    f.setFontLetterSpacingType(QFont::AbsoluteSpacing);
    f.setFontLetterSpacing(r.letterSpacing * displayScale);
    f.setFontCapitalization(r.caps == TextRun::Caps::Small ? QFont::SmallCaps : r.caps == TextRun::Caps::All ? QFont::AllUppercase : QFont::MixedCase);
    f.setFontUnderline(r.underline);
    f.setFontStrikeOut(r.strikethrough);
    f.setVerticalAlignment(r.baselineShift > 0 ? QTextCharFormat::AlignSuperScript : r.baselineShift < 0 ? QTextCharFormat::AlignSubScript : QTextCharFormat::AlignNormal);
    return f;
}

TextRun runFromFormat(const QTextCharFormat& f, const TextRun& fallback) {
    TextRun r = fallback;
    r.length = 0;
    if (!f.boolProperty(Holds)) return r;
    r.fontFamily = f.stringProperty(Family).toStdString();
    r.fontSize = f.doubleProperty(Size);
    r.bold = f.boolProperty(Bold);
    r.italic = f.boolProperty(Italic);
    r.weight = f.intProperty(Weight);
    r.red = f.doubleProperty(Red);
    r.green = f.doubleProperty(Green);
    r.blue = f.doubleProperty(Blue);
    r.letterSpacing = f.doubleProperty(LetterSpacing);
    r.baselineShift = f.doubleProperty(BaselineShift);
    r.leading = f.doubleProperty(Leading);
    r.caps = TextRun::Caps(std::clamp(f.intProperty(Caps), 0, 2));
    r.underline = f.boolProperty(Underline);
    r.strikethrough = f.boolProperty(Strikethrough);
    return r;
}

void fillTextDocument(QTextDocument& document, const LayerText& text, double displayScale) {
    document.clear();
    const QString all = QString::fromStdString(text.text);
    const std::vector<TextRun> runs = textRuns(text);
    QTextCursor cursor(&document);
    cursor.beginEditBlock();
    // What typing into empty text takes.
    cursor.setBlockCharFormat(charFormatFor(runs.empty() ? baseTextRun(text) : runs.front(), displayScale));
    int at = 0;
    for (const TextRun& r : runs) {
        // A newline becomes a paragraph break in the run's format (insertText gives the break the format).
        cursor.insertText(all.mid(at, r.length), charFormatFor(r, displayScale));
        at += r.length;
    }
    cursor.endEditBlock();
}

std::vector<TextRun> documentRuns(const QTextDocument& document, const TextRun& fallback, int from, int to) {
    if (to < 0) to = document.characterCount() - 1;
    std::vector<TextRun> runs;
    auto add = [&](int start, int length, const QTextCharFormat& f) {
        const int a = std::max(start, from), b = std::min(start + length, to);
        if (b > a) append(runs, runFromFormat(f, fallback), b - a);
    };
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.isValid()) add(fragment.position(), fragment.length(), fragment.charFormat());
        }
        // The break between this block and the next is held as the next block's character format.
        if (block.next().isValid()) add(block.position() + block.length() - 1, 1, block.next().charFormat());
    }
    return runs;
}

LayerText layerTextFromDocument(const QTextDocument& document, LayerText base, const TextRun& fallback) {
    QString raw = document.toRawText();
    raw.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));
    base.text = raw.toStdString();
    base.runs = documentRuns(document, fallback);
    if (base.runs.empty()) {
        // No text: the plain fields keep the style typing would take.
        base.runs = {runFromFormat(document.begin().charFormat(), fallback)};
        base.runs.front().length = 1;
        settleTextRuns(base);
        base.runs.clear();
        return base;
    }
    settleTextRuns(base);
    return base;
}

} // namespace app
