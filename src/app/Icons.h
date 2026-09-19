// Tool icons from Lucide (ISC), bundled as SVG and tinted to the palette so
// they read on light and dark themes; the checked state uses the highlight's
// text colour.
#pragma once
#include <QApplication>
#include <QFile>
#include <QIcon>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QSvgRenderer>
#include <QString>

namespace app {

inline QPixmap renderIcon(const QString& name, const QColor& color, int size, double dpr) {
    QFile file(QStringLiteral(":/icons/%1.svg").arg(name));
    QPixmap pixmap(int(size * dpr), int(size * dpr));
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    if (!file.open(QIODevice::ReadOnly)) return pixmap;
    QByteArray svg = file.readAll();
    svg.replace("currentColor", color.name(QColor::HexRgb).toLatin1());
    QSvgRenderer renderer(svg);
    QPainter p(&pixmap);
    renderer.render(&p, QRectF(0, 0, size, size));
    return pixmap;
}

/// An icon for a checkable tool button: palette text normally, highlighted text when checked.
inline QIcon toolIcon(const QString& name, int size = 20) {
    QPalette palette = QApplication::palette();
    QIcon icon;
    double dpr = qApp->devicePixelRatio();
    for (double scale : {1.0, 2.0}) {
        if (scale < dpr && dpr != 2.0) continue;
        icon.addPixmap(renderIcon(name, palette.color(QPalette::ButtonText), size, scale), QIcon::Normal, QIcon::Off);
        icon.addPixmap(renderIcon(name, palette.color(QPalette::HighlightedText), size, scale), QIcon::Normal, QIcon::On);
        icon.addPixmap(renderIcon(name, palette.color(QPalette::HighlightedText), size, scale), QIcon::Active, QIcon::On);
        icon.addPixmap(renderIcon(name, palette.color(QPalette::HighlightedText), size, scale), QIcon::Selected, QIcon::On);
    }
    return icon;
}

} // namespace app
