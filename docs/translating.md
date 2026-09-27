# Translating NekoPhoto

The interface is translated with Qt Linguist. English is the source language (the strings in the code);
Japanese is complete and kept complete by a test. Each language is one file, `translations/nekophoto_<code>.ts`
(`<code>` is the ISO 639-1 language code: `ja`, `de`, `fr`), compiled to a `.qm` at build time and embedded
in the program, so every package carries it without install paths.

## Choosing the language

Edit > Preferences > Language: System default (the desktop's language when there is a translation for it,
else English), English, or a translated language. It applies at the next launch. `nekophoto --lang ja` sets
it for one run (screenshots, testing). Qt's own strings (standard buttons, file dialogs) come from
`qtbase_<code>.qm`, found beside the program (`translations/`, which the AppImage and the Windows zip carry)
or in Qt's install.

What stays English on purpose: the automation API (JSON-RPC method names, parameters, results and error
messages; requests on the socket run in English whatever the interface language), undo-step names as
`history.list` reports them (the Edit menu shows them translated), file format identifiers, settings keys,
log output and command-line help, brush preset and G'MIC filter names (content), and messages that come
from the portable core (`src/core`, which has no Qt) such as some import notes.

## Adding a language

1. Build once with Qt LinguistTools installed (`qt6-l10n-tools` and `qt6-tools-dev` on Debian/Ubuntu,
   `qt6-tools` on Arch and MSYS2); configure prints `Translations: ...`.
2. Add the file to `qt_add_translations` in `src/app/CMakeLists.txt`
   (`TS_FILES ${CMAKE_SOURCE_DIR}/translations/nekophoto_ja.ts ${CMAKE_SOURCE_DIR}/translations/nekophoto_de.ts`),
   and the code to `language::available()` and `nativeName()` in `src/app/Language.cpp`.
3. Create or refresh the `.ts` from the sources:
   ```bash
   cmake --build build --target update_translations
   # or directly: lupdate -no-obsolete -locations none src/app -ts translations/nekophoto_de.ts
   ```
4. Translate in Qt Linguist (`linguist translations/nekophoto_de.ts`), marking each entry finished.
   Contexts are the C++ classes; `Names` holds blend modes, adjustment and filter names and new layers'
   names, `History` the undo steps. Keep `%1`, `%n`, HTML tags and the `&` mnemonic letter; for languages
   written without Latin letters put the mnemonic at the end as Photoshop does (`ファイル(&F)`).
   Prefer the terms of Photoshop's own translation for that language: people come from it.
5. The packaging scripts copy Qt's `qtbase_ja.qm`; add the new language's file in
   `tools/package-appimage.sh` and `tools/package-windows.sh`.

## The check

`ctest -R translations_check` (and `tools/check_translations.py`) runs lupdate over `src/app` into a copy
of every `.ts` and fails when a string is unfinished or no longer in the code, listing them. So a new
`tr()` string fails the test until it is translated in every language: after adding UI text, run
`update_translations`, translate the new entries, and commit the `.ts` with the code.

## Writing translatable code

- Every user-visible string goes through `tr()` (or `QCoreApplication::translate("Context", ...)` outside
  a QObject); strings kept in tables are marked with `QT_TRANSLATE_NOOP` and translated where shown.
- Build messages with `arg()`, never by concatenating pieces; counts use `tr("%n layer(s)", nullptr, n)`.
- Names the API also uses (blend modes, adjustment kinds) stay English in the core and go through
  `names::` (`src/app/Names.h`) for display. Undo-step names are English (`QT_TRANSLATE_NOOP("History", ...)`)
  and displayed with `names::history()`.
- Don't size widgets for English text: translations are often longer, and CJK text is taller.
