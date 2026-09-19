#include "Theme.h"
#include <QApplication>
#include <QPalette>
#include <QSettings>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#ifdef COMPOSITOR_HAVE_DBUS
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusVariant>
#endif

namespace app {

namespace {

QPalette darkPalette() {
    QPalette p;
    QColor window(43, 44, 47), base(31, 32, 34), text(228, 228, 230), disabled(120, 121, 124), highlight(58, 124, 214);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, window);
    p.setColor(QPalette::ToolTipBase, base);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::PlaceholderText, disabled);
    p.setColor(QPalette::Button, window);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Link, highlight);
    p.setColor(QPalette::Highlight, highlight);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Light, QColor(70, 71, 75));
    p.setColor(QPalette::Midlight, QColor(58, 59, 62));
    p.setColor(QPalette::Mid, QColor(38, 39, 41));
    p.setColor(QPalette::Dark, QColor(24, 25, 27));
    p.setColor(QPalette::Shadow, QColor(12, 12, 13));
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) p.setColor(QPalette::Disabled, role, disabled);
    p.setColor(QPalette::Disabled, QPalette::Highlight, QColor(70, 71, 75));
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, disabled);
    return p;
}

/// A desktop colour preference (Qt::ColorScheme arrived in Qt 6.5; CI builds against 6.4).
enum class Scheme { Unknown, Light, Dark };

/// The desktop's preference: Qt's own answer when a platform theme provides it, else the XDG portal's.
Scheme desktopScheme() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    switch (QApplication::styleHints()->colorScheme()) {
    case Qt::ColorScheme::Dark: return Scheme::Dark;
    case Qt::ColorScheme::Light: return Scheme::Light;
    default: break;
    }
#endif
#ifdef COMPOSITOR_HAVE_DBUS
    QDBusInterface portal("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop", "org.freedesktop.portal.Settings");
    portal.setTimeout(500);
    if (portal.isValid()) {
        for (const char* method : {"ReadOne", "Read"}) {
            QDBusReply<QDBusVariant> reply = portal.call(method, "org.freedesktop.appearance", "color-scheme");
            if (!reply.isValid()) continue;
            QVariant v = reply.value().variant();
            while (v.canConvert<QDBusVariant>()) v = v.value<QDBusVariant>().variant();
            uint value = v.toUInt();   // 0 no preference, 1 dark, 2 light
            if (value == 1) return Scheme::Dark;
            if (value == 2) return Scheme::Light;
            return Scheme::Unknown;
        }
    }
#endif
    return Scheme::Unknown;
}

bool paletteIsDark(const QPalette& p) { return p.color(QPalette::Window).lightness() < 128; }

} // namespace

QString themeSetting() {
    QString env = qEnvironmentVariable("COMPOSITOR_THEME");
    if (env == "dark" || env == "light" || env == "system") return env;
    QString v = QSettings().value("appearance/theme", "system").toString();
    return v == "dark" || v == "light" ? v : QStringLiteral("system");
}

void setThemeSetting(const QString& value) { QSettings().setValue("appearance/theme", value); }

void applyTheme() {
    static const QPalette original = QApplication::palette();
    static const QString originalStyle = QApplication::style()->objectName();
    QString choice = themeSetting();
    bool wantDark;
    if (choice == "dark") wantDark = true;
    else if (choice == "light") wantDark = false;
    else {
        Scheme desktop = desktopScheme();
        // The platform theme already gave the desktop look, or the desktop has no preference: leave it alone.
        if (desktop == Scheme::Unknown || paletteIsDark(original) == (desktop == Scheme::Dark)) {
            if (QApplication::style()->objectName() != originalStyle) QApplication::setStyle(QStyleFactory::create(originalStyle));
            QApplication::setPalette(original);
            return;
        }
        wantDark = desktop == Scheme::Dark;
    }
    // Fusion draws both palettes consistently; the platform style may not.
    if (QApplication::style()->objectName() != QLatin1String("fusion")) QApplication::setStyle(QStyleFactory::create("Fusion"));
    QApplication::setPalette(wantDark ? darkPalette() : QStyleFactory::create("Fusion")->standardPalette());
}

} // namespace app
