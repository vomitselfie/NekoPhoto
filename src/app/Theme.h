// Appearance: follow the desktop, or force a dark or light look. Inside an
// AppImage the desktop's Qt theme plugin can't load (it targets the system Qt),
// so "system" asks the XDG desktop portal for the colour scheme instead.
#pragma once
#include <QString>

namespace app {

/// "system", "dark" or "light" (QSettings appearance/theme; COMPOSITOR_THEME overrides for a run).
QString themeSetting();
void setThemeSetting(const QString& value);

/// Applies the current setting to the application: a Fusion dark or light palette, or nothing when the
/// platform theme already provides the desktop's look.
void applyTheme();

} // namespace app
