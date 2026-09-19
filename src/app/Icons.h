// Tool icons from Lucide (ISC), bundled as SVG and tinted to the palette so
// they read on light and dark themes; the checked state uses the highlight's
// text colour.
#pragma once
#include <QApplication>
#include <algorithm>
#include <cmath>
#include <QDebug>
#include <QSet>
#include <QFile>
#include <QIcon>
#include <QIconEngine>
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

/// Renders a bundled SVG at whatever size and scale is asked for, tinted for the mode and state, so
/// the icon is never empty at unusual scale factors and follows palette changes.
class TintedSvgIconEngine : public QIconEngine {
public:
    explicit TintedSvgIconEngine(QString name) : name_(std::move(name)) {}

    static QColor colorFor(QIcon::Mode mode, QIcon::State state) {
        QPalette palette = QApplication::palette();
        if (mode == QIcon::Disabled) return palette.color(QPalette::Disabled, QPalette::ButtonText);
        if (state == QIcon::On) return palette.color(QPalette::HighlightedText);
        return palette.color(QPalette::ButtonText);
    }

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State state) override {
        double dpr = painter->device() ? painter->device()->devicePixelRatio() : 1.0;
        int size = std::min(rect.width(), rect.height());
        QPixmap pm = renderIcon(name_, colorFor(mode, state), size, dpr);
        painter->drawPixmap(QRect(rect.x() + (rect.width() - size) / 2, rect.y() + (rect.height() - size) / 2, size, size), pm);
    }
    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override {
        return renderIcon(name_, colorFor(mode, state), std::min(size.width(), size.height()), 1.0);
    }
    QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode, QIcon::State state, qreal scale) override {
        // `size` is in device pixels; render the logical size at `scale` so the result matches.
        int logical = std::max(1, int(std::lround(std::min(size.width(), size.height()) / std::max(scale, 0.01))));
        return renderIcon(name_, colorFor(mode, state), logical, scale);
    }
    QSize actualSize(const QSize& size, QIcon::Mode, QIcon::State) override { int s = std::min(size.width(), size.height()); return {s, s}; }
    QIconEngine* clone() const override { return new TintedSvgIconEngine(name_); }
    QString key() const override { return QStringLiteral("compositor-tinted-svg"); }
    QString iconName() override { return name_; }

private:
    QString name_;
};

/// An icon for a tool button: palette text normally, highlighted text when checked, at any scale.
inline QIcon toolIcon(const QString& name, int = 20) { return QIcon(new TintedSvgIconEngine(name)); }

} // namespace app
