// Tool icons from Lucide (ISC), bundled as SVG and tinted to the palette so
// they read on light and dark themes; the checked state uses the highlight's
// text colour.
#pragma once
#include <QApplication>
#include <QDebug>
#include <QSet>
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
    if (!file.open(QIODevice::ReadOnly)) {
        // The resource is missing (a build without the icon bundle): fall back to the name's initial so the button is never blank.
        static QSet<QString> warned;
        if (!warned.contains(name)) { warned.insert(name); qWarning("icon %s not found in resources", qPrintable(name)); }
        QPainter p(&pixmap);
        p.setPen(color);
        QFont f = p.font();
        f.setPixelSize(int(size * 0.7));
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRect(0, 0, size, size), Qt::AlignCenter, name.left(1).toUpper());
        return pixmap;
    }
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
