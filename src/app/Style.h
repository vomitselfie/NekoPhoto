// Small shared styling helpers.
#pragma once
#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QString>
#include <QVariant>

namespace app {

/// A stylesheet colour for hint text: the text colour faded toward the window, so it reads on light and dark themes
/// (palette(mid) is nearly the window colour under some dark styles).
inline QColor hintColor(int textTenths = 6) {
    QPalette p = QApplication::palette();
    QColor text = p.color(QPalette::Text), window = p.color(QPalette::Window);
    int t = textTenths, w = 10 - textTenths;
    return QColor((text.red() * t + window.red() * w) / 10, (text.green() * t + window.green() * w) / 10, (text.blue() * t + window.blue() * w) / 10);
}

inline QString hintStyle(const QString& extra = {}) {
    return QStringLiteral("color: %1;%2").arg(hintColor().name(), extra);
}

/// The grey behind the canvas and image previews (Photoshop's pasteboard), or the theme's own: Goth Kitty sets a plum
/// through the application's "canvasBackdrop" property (Theme.cpp).
inline QColor canvasBackdrop() {
    const QVariant v = qApp->property("canvasBackdrop");
    return v.isValid() ? v.value<QColor>() : QColor(46, 46, 46);
}

} // namespace app
