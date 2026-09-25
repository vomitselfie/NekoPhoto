#pragma once

// Marks a user-facing English literal in Qt-free code so lupdate extracts it
// into the translation catalogs (lupdate runs with
// -tr-function-alias QT_TRANSLATE_NOOP+=PATCHY_TRANSLATE_NOOP). It expands to the
// literal itself: core code keeps returning or throwing plain English, and the UI
// translates at the display boundary with translate_data_text() (src/ui/localization.hpp),
// which looks the text up in the given context (always "QObject" today).
// A lookup miss falls back to the English text, so wrapping never breaks anything.
#define PATCHY_TRANSLATE_NOOP(context, text) text
