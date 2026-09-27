#include "Language.h"
#include <QCoreApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QSettings>
#include <QTranslator>

namespace app::language {

namespace {
const char* const kKey = "interface/language";
QString g_current = QStringLiteral("en");
}

QString setting() { return QSettings().value(kKey, QStringLiteral("system")).toString(); }

void setSetting(const QString& code) { QSettings().setValue(kKey, code); }

QStringList available() { return {QStringLiteral("en"), QStringLiteral("ja")}; }

QString nativeName(const QString& code) {
    if (code == QLatin1String("ja")) return QStringLiteral("日本語");
    if (code == QLatin1String("en")) return QStringLiteral("English");
    return QLocale(code).nativeLanguageName();
}

QString current() { return g_current; }

QString install(const QString& override) {
    const QString choice = override.isEmpty() ? setting() : override;
    const QLocale locale = choice == QLatin1String("system") || choice.isEmpty() ? QLocale::system() : QLocale(choice);
    const QString code = locale.name().section('_', 0, 0);
    if (code == QLatin1String("en") || !available().contains(code)) return g_current = QStringLiteral("en");
    // Ours is compiled in (qt_add_translations); Qt's own comes from a "translations" folder beside the program
    // (the Windows zip, the AppImage) or from Qt's install.
    auto* ours = new QTranslator(QCoreApplication::instance());
    if (!ours->load(QStringLiteral(":/i18n/nekophoto_%1.qm").arg(code))) {
        delete ours;
        return g_current = QStringLiteral("en");   // a build without Qt LinguistTools
    }
    QCoreApplication::installTranslator(ours);
    const QStringList qtDirs = {QCoreApplication::applicationDirPath() + QStringLiteral("/translations"),
                                QCoreApplication::applicationDirPath() + QStringLiteral("/../translations"),
                                QLibraryInfo::path(QLibraryInfo::TranslationsPath)};
    for (const QString& dir : qtDirs) {
        auto* qt = new QTranslator(QCoreApplication::instance());
        if (qt->load(QStringLiteral("qtbase_%1").arg(code), dir)) { QCoreApplication::installTranslator(qt); break; }
        delete qt;
    }
    return g_current = code;
}

} // namespace app::language
