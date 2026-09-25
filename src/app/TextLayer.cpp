#include "TextLayer.h"
#include "ImageConvert.h"
#include <QFontDatabase>
#include <QFontInfo>
#include <QHash>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QRawFont>
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

std::shared_ptr<compositor::Image> renderTextLayer(const compositor::LayerText& text) {
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
        if (!family.isEmpty() && !installed(family))
            imported.notes.push_back("Layer \"" + layer->name + "\": the font " + t.postScriptName + " is not installed; the text keeps Photoshop's pixels until you edit it, then uses the closest match, " +
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
