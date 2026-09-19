#include "TextLayer.h"
#include "ImageConvert.h"
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <algorithm>
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

} // namespace app
