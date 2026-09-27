// The interface language: Edit > Preferences stores a choice ("system", "en", "ja"), --lang overrides it for one
// run, and install() loads our translation plus Qt's own (standard buttons, dialogs) before any window exists.
#pragma once
#include <QString>
#include <QStringList>

namespace app::language {

/// The stored choice: "system" (follow the desktop locale), or a language code such as "en" or "ja".
QString setting();
void setSetting(const QString& code);
/// Language codes with a translation, English first (it needs none).
QStringList available();
/// The language's own name, for the Preferences list ("English", "日本語").
QString nativeName(const QString& code);
/// Installs the translators for `override` (a --lang value) or, when empty, the stored choice. Returns the
/// language code in effect ("en" when no translation matched).
QString install(const QString& override = {});
/// The code install() settled on.
QString current();

/// While one exists, tr() answers in English: the automation socket's replies (errors, names, labels) are API and
/// stay English whatever the interface language. Nests.
class EnglishScope {
public:
    EnglishScope();
    ~EnglishScope();
    EnglishScope(const EnglishScope&) = delete;
    EnglishScope& operator=(const EnglishScope&) = delete;
};

} // namespace app::language
