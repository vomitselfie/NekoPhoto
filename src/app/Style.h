// Small shared styling helpers.
#pragma once
#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QString>

namespace app {

/// A stylesheet colour for hint text: the text colour faded toward the window, so it reads on light and dark themes
/// (palette(mid) is nearly the window colour under some dark styles).
inline QString hintStyle(const QString& extra = {}) {
    QPalette p = QApplication::palette();
    QColor text = p.color(QPalette::Text), window = p.color(QPalette::Window);
    QColor mix((text.red() * 6 + window.red() * 4) / 10, (text.green() * 6 + window.green() * 4) / 10, (text.blue() * 6 + window.blue() * 4) / 10);
    return QStringLiteral("color: %1;%2").arg(mix.name(), extra);
}

} // namespace app
