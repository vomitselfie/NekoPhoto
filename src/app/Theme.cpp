#include "Theme.h"
#include <QApplication>
#include <QFile>
#include <QGuiApplication>
#include <QPainter>
#include <QPixmap>
#include <QScreen>
#include <QSvgRenderer>
#include <QWidget>
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

/// Goth Kitty: plum-black panels, candy-pink highlights and lavender accents (gothkitty.qss draws the rest).
QPalette gothKittyPalette() {
    QPalette p;
    const QColor window(27, 19, 32), base(18, 12, 22), text(246, 232, 242), disabled(110, 90, 115), pink(255, 126, 182), lavender(185, 163, 255);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, QColor(35, 26, 43));
    p.setColor(QPalette::ToolTipBase, QColor(42, 31, 51));
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::PlaceholderText, disabled);
    p.setColor(QPalette::Button, QColor(46, 33, 56));
    p.setColor(QPalette::ButtonText, QColor(255, 196, 225));   // the tool icons are tinted with this: pink on plum
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Link, lavender);
    p.setColor(QPalette::Highlight, pink);
    p.setColor(QPalette::HighlightedText, window);
    p.setColor(QPalette::Light, QColor(74, 53, 86));
    p.setColor(QPalette::Midlight, QColor(61, 44, 71));
    p.setColor(QPalette::Mid, QColor(42, 31, 51));
    p.setColor(QPalette::Dark, QColor(21, 14, 25));
    p.setColor(QPalette::Shadow, QColor(10, 6, 12));
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) p.setColor(QPalette::Disabled, role, disabled);
    p.setColor(QPalette::Disabled, QPalette::Highlight, QColor(61, 44, 71));
    p.setColor(QPalette::Disabled, QPalette::HighlightedText, disabled);
    return p;
}

QString gothKittyStyleSheet() {
    // COMPOSITOR_THEME_QSS reads the stylesheet from a file instead, for working on the theme without a rebuild.
    const QString override = qEnvironmentVariable("COMPOSITOR_THEME_QSS");
    QFile file(override.isEmpty() ? QStringLiteral(":/gothkitty/gothkitty.qss") : override);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
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

bool gothKitty() { return themeSetting() == QLatin1String("gothkitty"); }

/// A cursor drawn from one of the theme's SVGs, 32 pixels across at the screen's scale, with its hot spot in those pixels.
QCursor svgCursor(const QString& name, QPoint hotSpot) {
    const qreal dpr = QGuiApplication::primaryScreen() ? QGuiApplication::primaryScreen()->devicePixelRatio() : 1.0;
    QPixmap pixmap(QSize(32, 32) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QSvgRenderer svg(QStringLiteral(":/gothkitty/%1.svg").arg(name));
    QPainter p(&pixmap);
    svg.render(&p, QRectF(0, 0, 32, 32));
    p.end();
    return QCursor(pixmap, hotSpot.x(), hotSpot.y());
}

/// Gives every window the theme's pointer as it is shown (its widgets inherit it unless they choose their own, as the
/// canvas's tool cursors and the text fields' I-beam do), and takes it back when the theme changes.
class WindowCursors : public QObject {
public:
    bool eventFilter(QObject* object, QEvent* event) override {
        if (event->type() == QEvent::Show) if (auto* w = qobject_cast<QWidget*>(object); w && w->isWindow()) dress(w);
        return false;
    }
    static void dress(QWidget* w) {
        if (w->testAttribute(Qt::WA_SetCursor) && !w->property("themeCursor").toBool()) return;   // its own cursor
        w->setCursor(themedCursor(Qt::ArrowCursor));
        w->setProperty("themeCursor", true);
    }
    static void undress(QWidget* w) {
        if (!w->property("themeCursor").toBool()) return;
        w->unsetCursor();
        w->setProperty("themeCursor", false);
    }
};

void applyWindowCursors(bool on) {
    static WindowCursors* filter = nullptr;
    if (on && !filter) { filter = new WindowCursors; qApp->installEventFilter(filter); }
    if (!on && filter) { qApp->removeEventFilter(filter); delete filter; filter = nullptr; }
    for (QWidget* w : QApplication::topLevelWidgets()) on ? WindowCursors::dress(w) : WindowCursors::undress(w);
}

bool paletteIsDark(const QPalette& p) { return p.color(QPalette::Window).lightness() < 128; }

} // namespace

QString themeSetting() {
    QString env = qEnvironmentVariable("COMPOSITOR_THEME");
    if (env == "dark" || env == "light" || env == "system" || env == "gothkitty") return env;
    QString v = QSettings().value("appearance/theme", "system").toString();
    return v == "dark" || v == "light" || v == "gothkitty" ? v : QStringLiteral("system");
}

void setThemeSetting(const QString& value) { QSettings().setValue("appearance/theme", value); }

void applyTheme() {
    static const QPalette original = QApplication::palette();
    static const QString originalStyle = QApplication::style()->objectName();
    QString choice = themeSetting();
    // Only Goth Kitty draws with a stylesheet; every other choice clears it.
    qApp->setStyleSheet(choice == "gothkitty" ? gothKittyStyleSheet() : QString());
    qApp->setProperty("canvasBackdrop", choice == "gothkitty" ? QVariant(QColor(33, 23, 40)) : QVariant());
    applyWindowCursors(choice == "gothkitty");
    if (choice == "gothkitty") {
        if (QApplication::style()->objectName() != QLatin1String("fusion")) QApplication::setStyle(QStyleFactory::create("Fusion"));
        QApplication::setPalette(gothKittyPalette());
        return;
    }
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

QCursor themedCursor(Qt::CursorShape shape) {
    if (!gothKitty()) return QCursor(shape);
    switch (shape) {
    case Qt::ArrowCursor: return svgCursor(QStringLiteral("cursor"), QPoint(4, 3));
    case Qt::BusyCursor:
    case Qt::WaitCursor: return svgCursor(QStringLiteral("busy"), QPoint(16, 16));
    case Qt::ForbiddenCursor: return svgCursor(QStringLiteral("forbidden"), QPoint(16, 16));
    default: return QCursor(shape);
    }
}

} // namespace app
