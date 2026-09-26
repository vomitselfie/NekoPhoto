#include "TextLayer.h"
#include "ImageConvert.h"
#include <QFontDatabase>
#include <QFontInfo>
#include <QHash>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QRawFont>
#include <QTextLayout>
#include <algorithm>
#include <limits>
#include <cmath>

namespace app {

QString defaultTextFamily() { return QFontDatabase::systemFont(QFontDatabase::GeneralFont).family(); }

QFont fontFor(const compositor::LayerText& text) {
    QFont font(text.fontFamily.empty() ? defaultTextFamily() : QString::fromStdString(text.fontFamily));
    font.setPixelSize(std::max(1, int(std::lround(text.fontSize))));
    font.setBold(text.bold);
    font.setItalic(text.italic);
    font.setLetterSpacing(QFont::AbsoluteSpacing, text.letterSpacing);
    return font;
}

namespace {

QFont runFont(const compositor::LayerText& text, const compositor::TextRun& run) {
    compositor::LayerText t = text;
    t.fontFamily = run.fontFamily; t.fontSize = run.fontSize; t.bold = run.bold; t.italic = run.italic; t.letterSpacing = run.letterSpacing;
    QFont font = fontFor(t);
    if (run.weight > 0) font.setWeight(QFont::Weight(std::clamp(run.weight, 100, 900)));
    if (run.caps != compositor::TextRun::Caps::Normal) font.setCapitalization(run.caps == compositor::TextRun::Caps::Small ? QFont::SmallCaps : QFont::AllUppercase);
    return font;
}

/// Text in several styles, one QTextLayout a line, each run its own format.
struct RunLayout {
    std::vector<std::unique_ptr<QTextLayout>> lines;
    std::vector<double> widths;
    std::vector<double> baselines;   // each line's, below the first
    double width = 0, firstAscent = 0, lastDescent = 0, pitch = 0;
    std::vector<QFont> fonts;   // each run's
};

RunLayout layoutRuns(const compositor::LayerText& text) {
    RunLayout out;
    const std::vector<compositor::TextRun> runs = compositor::textRuns(text);
    struct Span { int start, end; QTextCharFormat format; double leading; };
    std::vector<Span> spans;
    int at = 0;
    for (const compositor::TextRun& run : runs) {
        const QFont font = runFont(text, run);
        out.fonts.push_back(font);
        QTextCharFormat f;
        f.setFont(font);
        f.setForeground(QColor::fromRgbF(float(std::clamp(run.red, 0.0, 1.0)), float(std::clamp(run.green, 0.0, 1.0)), float(std::clamp(run.blue, 0.0, 1.0))));
        f.setFontUnderline(run.underline);
        f.setFontStrikeOut(run.strikethrough);
        if (run.baselineShift != 0) {
            const double height = QFontMetricsF(font).height();
            if (height > 0) f.setBaselineOffset(run.baselineShift / height * 100);
        }
        // Photoshop's leading, or the font's own line height as NekoPhoto spaces plain text.
        const double leading = run.leading > 0 ? run.leading : QFontMetricsF(font).height() * std::clamp(text.lineSpacing, 0.1, 10.0);
        spans.push_back({at, at + run.length, f, leading});
        at += run.length;
    }
    const QFont base = out.fonts.empty() ? fontFor(text) : out.fonts.front();
    out.pitch = QFontMetricsF(base).height() * std::clamp(text.lineSpacing, 0.1, 10.0);
    const QString content = QString::fromStdString(text.text);
    const QStringList lines = content.split('\n');
    int lineStart = 0;
    for (int i = 0; i < lines.size(); i++) {
        const QString& line = lines[i];
        auto layout = std::make_unique<QTextLayout>(line, base);
        QList<QTextLayout::FormatRange> formats;
        for (const Span& sp : spans) {
            const int s0 = std::max(sp.start, lineStart), s1 = std::min(sp.end, lineStart + int(line.size()));
            if (s1 > s0) formats.append({s0 - lineStart, s1 - s0, sp.format});
        }
        // An empty line keeps the height of the style it sits in.
        if (line.isEmpty())
            for (const Span& sp : spans) if (sp.start <= lineStart && lineStart <= sp.end) { layout->setFont(sp.format.font()); break; }
        layout->setFormats(formats);
        QTextOption option;
        option.setWrapMode(QTextOption::NoWrap);
        option.setTabStopDistance(36);   // Photoshop's default tab stops: every half inch, 36 points
        layout->setTextOption(option);
        layout->beginLayout();
        QTextLine l = layout->createLine();
        if (l.isValid()) l.setLineWidth(1e7);
        layout->endLayout();
        // The line sits the largest leading among its runs below the one before (Photoshop's rule).
        double lineLeading = 0;
        for (const Span& sp : spans) {
            const bool touches = line.isEmpty() ? (sp.start <= lineStart && lineStart <= sp.end) : (sp.start < lineStart + int(line.size()) && sp.end > lineStart);
            if (touches) lineLeading = std::max(lineLeading, sp.leading);
        }
        if (lineLeading <= 0) lineLeading = out.pitch;
        out.baselines.push_back(i == 0 ? 0 : out.baselines.back() + lineLeading);
        const double w = l.isValid() ? l.naturalTextWidth() : 0;
        out.widths.push_back(w);
        out.width = std::max(out.width, w);
        if (i == 0) out.firstAscent = l.isValid() ? l.ascent() : QFontMetricsF(base).ascent();
        out.lastDescent = l.isValid() ? l.descent() : QFontMetricsF(base).descent();
        out.lines.push_back(std::move(layout));
        lineStart += int(line.size()) + 1;
    }
    return out;
}

std::shared_ptr<compositor::Image> renderRuns(const compositor::LayerText& text) {
    RunLayout laid = layoutRuns(text);
    const int w = std::max(1, int(std::ceil(laid.width)) + 2 * textPadding);
    const double lastBaseline = laid.firstAscent + laid.baselines.back();
    const int h = std::max(1, int(std::ceil(lastBaseline + laid.lastDescent)) + 2 * textPadding);
    if ((long long)w * h > compositor::Document::pixelBudget) return nullptr;
    QImage image(w, h, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    for (size_t i = 0; i < laid.lines.size(); i++) {
        QTextLayout& layout = *laid.lines[i];
        if (layout.lineCount() == 0) continue;
        const double x = textPadding + (text.alignment == 1 ? (laid.width - laid.widths[i]) / 2 : text.alignment == 2 ? laid.width - laid.widths[i] : 0);
        const double baseline = textPadding + laid.firstAscent + laid.baselines[i];
        layout.draw(&painter, QPointF(x, baseline - layout.lineAt(0).ascent()));
    }
    painter.end();
    return fromQImage(image);
}

} // namespace

std::shared_ptr<compositor::Image> renderTextLayer(const compositor::LayerText& text) {
    if (!text.runs.empty()) return renderRuns(text);
    const QFont font = fontFor(text);
    const QFontMetricsF metrics(font);
    QString content = QString::fromStdString(text.text);
    if (content.isEmpty()) content = QStringLiteral(" ");
    const QStringList lines = content.split('\n');
    const double lineHeight = metrics.height() * std::clamp(text.lineSpacing, 0.1, 10.0);
    double width = 0;
    for (const QString& line : lines) width = std::max(width, metrics.horizontalAdvance(line));
    const int w = std::max(1, int(std::ceil(width)) + 2 * textPadding);
    const int h = std::max(1, int(std::ceil(lineHeight * lines.size())) + 2 * textPadding);
    if ((long long)w * h > compositor::Document::pixelBudget) return nullptr;
    QImage image(w, h, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setFont(font);
    painter.setPen(QColor::fromRgbF(float(std::clamp(text.red, 0.0, 1.0)), float(std::clamp(text.green, 0.0, 1.0)), float(std::clamp(text.blue, 0.0, 1.0))));
    for (int i = 0; i < lines.size(); i++) {
        const double lineWidth = metrics.horizontalAdvance(lines[i]);
        const double x = textPadding + (text.alignment == 1 ? (width - lineWidth) / 2 : text.alignment == 2 ? width - lineWidth : 0);
        const double baseline = textPadding + i * lineHeight + metrics.ascent();
        painter.drawText(QPointF(x, baseline), lines[i]);
    }
    painter.end();
    return fromQImage(image);
}

namespace {

/// The face's PostScript name (name ID 6) from its 'name' table: Windows Unicode records first, then Mac Roman.
QString postScriptName(const QRawFont& raw) {
    const QByteArray table = raw.fontTable("name");
    auto u16 = [&](int at) { return at + 2 <= table.size() ? int(uint8_t(table[at])) << 8 | uint8_t(table[at + 1]) : -1; };
    const int count = u16(2), strings = u16(4);
    if (count < 0 || strings < 0) return {};
    QString mac;
    for (int i = 0; i < count; i++) {
        const int at = 6 + i * 12;
        const int platform = u16(at), nameId = u16(at + 6), length = u16(at + 8), offset = u16(at + 10);
        if (nameId != 6 || length <= 0 || strings + offset + length > table.size()) continue;
        const char* data = table.constData() + strings + offset;
        if (platform == 3 || platform == 0) {
            QString name;
            for (int k = 0; k + 1 < length; k += 2) name.append(QChar(char16_t(uint8_t(data[k]) << 8 | uint8_t(data[k + 1]))));
            if (!name.isEmpty()) return name;
        } else if (platform == 1 && mac.isEmpty()) mac = QString::fromLatin1(data, length);
    }
    return mac;
}

} // namespace

std::optional<compositor::PsdTextMetrics> psdTextMetrics(const compositor::LayerText& text) {
    if (!text.runs.empty()) {
        if (text.text.empty()) return std::nullopt;
        const RunLayout laid = layoutRuns(text);
        compositor::PsdTextMetrics m;
        m.fontSize = laid.fonts.front().pixelSize();
        m.ascent = laid.firstAscent;
        // The block's height over its lines (the writer spaces the bounds by one pitch; each run keeps its own leading).
        m.lineHeight = laid.lines.size() > 1 ? laid.baselines.back() / double(laid.lines.size() - 1) : laid.pitch;
        if (!(m.lineHeight > 0)) m.lineHeight = laid.pitch;
        m.blockWidth = laid.width;
        m.blockLeft = m.blockTop = textPadding;
        m.lines = int(laid.lines.size());
        const std::vector<compositor::TextRun> runs = compositor::textRuns(text);
        for (size_t i = 0; i < runs.size(); i++) {
            const QRawFont raw = QRawFont::fromFont(laid.fonts[i]);
            compositor::PsdTextMetrics::RunFace face;
            face.postScriptName = postScriptName(raw).toStdString();
            if (face.postScriptName.empty()) face.postScriptName = QString(laid.fonts[i].family()).remove(' ').toStdString();
            face.fauxBold = runs[i].bold && runs[i].weight == 0 && raw.weight() < QFont::DemiBold;
            face.fauxItalic = runs[i].italic && raw.style() == QFont::StyleNormal;
            m.runs.push_back(face);
        }
        m.postScriptName = m.runs.front().postScriptName;
        m.fauxBold = m.runs.front().fauxBold;
        m.fauxItalic = m.runs.front().fauxItalic;
        return m;
    }
    // Exactly renderTextLayer's layout.
    const QFont font = fontFor(text);
    const QFontMetricsF metrics(font);
    QString content = QString::fromStdString(text.text);
    if (content.isEmpty()) return std::nullopt;
    const QStringList lines = content.split('\n');
    compositor::PsdTextMetrics m;
    m.fontSize = font.pixelSize();
    m.ascent = metrics.ascent();
    m.lineHeight = metrics.height() * std::clamp(text.lineSpacing, 0.1, 10.0);
    for (const QString& line : lines) m.blockWidth = std::max(m.blockWidth, metrics.horizontalAdvance(line));
    m.blockLeft = m.blockTop = textPadding;
    m.lines = int(lines.size());
    const QRawFont raw = QRawFont::fromFont(font);
    m.postScriptName = postScriptName(raw).toStdString();
    if (m.postScriptName.empty()) m.postScriptName = QString(font.family()).remove(' ').toStdString();
    // Bold or italic asked for, but the face found is neither: Qt synthesised it, as Photoshop's faux styles do.
    m.fauxBold = text.bold && raw.weight() < QFont::DemiBold;
    m.fauxItalic = text.italic && raw.style() == QFont::StyleNormal;
    return m;
}

namespace {

QString squashed(QString s) { return s.remove(' ').remove('-').remove('_').toLower(); }

/// The installed family a PostScript name belongs to: "ArialMT" is Arial, "NotoSans-Bold" is Noto Sans.
QString familyForPostScriptName(const QString& postScriptName) {
    static QHash<QString, QString> cache;
    if (auto it = cache.find(postScriptName); it != cache.end()) return *it;
    QString base = postScriptName.section('-', 0, 0);
    for (const char* tail : {"PSMT", "MT", "PS"}) if (base.endsWith(tail) && base.size() > int(strlen(tail)) + 2) { base.chop(int(strlen(tail))); break; }
    const QString wanted = squashed(base);
    QString found;
    for (const QString& family : QFontDatabase::families()) if (squashed(family) == wanted) { found = family; break; }
    if (found.isEmpty()) {
        // Not installed: the name spelled out ("TimesNewRoman" is Times New Roman), for fontconfig to match.
        for (int i = 0; i < base.size(); i++) {
            if (i > 0 && base[i].isUpper() && base[i - 1].isLower()) found += ' ';
            found += base[i];
        }
    }
    cache.insert(postScriptName, found);
    return found;
}

bool installed(const QString& family) {
    for (const QString& f : QFontDatabase::families()) if (f.compare(family, Qt::CaseInsensitive) == 0) return true;
    return false;
}

} // namespace

void finishPsdText(compositor::PsdImport& imported) {
    for (const compositor::PsdImportedText& t : imported.texts) {
        compositor::Layer* layer = imported.document.find(t.layer);
        if (!layer || !layer->text) continue;
        compositor::LayerText& text = *layer->text;
        const QString family = familyForPostScriptName(QString::fromStdString(t.postScriptName));
        text.fontFamily = family.toStdString();
        std::vector<std::string> missing;
        auto check = [&](const std::string& postScript, const QString& fam) {
            if (!fam.isEmpty() && !installed(fam) && std::find(missing.begin(), missing.end(), postScript) == missing.end()) missing.push_back(postScript);
        };
        check(t.postScriptName, family);
        for (size_t i = 0; i < text.runs.size() && i < t.runPostScriptNames.size(); i++) {
            const QString runFamily = familyForPostScriptName(QString::fromStdString(t.runPostScriptNames[i]));
            text.runs[i].fontFamily = runFamily.toStdString();
            check(t.runPostScriptNames[i], runFamily);
        }
        for (const std::string& postScript : missing)
            imported.notes.push_back("Layer \"" + layer->name + "\": the font " + postScript + " is not installed; the text keeps Photoshop's pixels until you edit it, then uses the closest match, " +
                                     QFontInfo(fontFor(text)).family().toStdString() + ".");
        const QFontMetricsF metrics(fontFor(text));
        const double leading = t.leading > 0 ? t.leading : t.autoLeading * text.fontSize;
        if (metrics.height() > 0) text.lineSpacing = std::clamp(leading / metrics.height(), 0.1, 10.0);
    }
}

compositor::PsdImportOptions psdImportOptions() {
    compositor::PsdImportOptions options;
    options.decodeImage = [](const std::vector<uint8_t>& bytes, const std::string&, const std::string&) -> compositor::ImagePtr {
        QImage image;
        if (bytes.size() > size_t(std::numeric_limits<int>::max()) || !image.loadFromData(bytes.data(), int(bytes.size()))) return nullptr;
        if ((long long)image.width() * image.height() > compositor::Document::pixelBudget) return nullptr;
        return fromQImage(image);
    };
    return options;
}

compositor::PsdExportOptions psdExportOptions() {
    compositor::PsdExportOptions options;
    options.textMetrics = psdTextMetrics;
    return options;
}

} // namespace app
