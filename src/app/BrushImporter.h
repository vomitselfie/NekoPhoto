// Importing brush files into the brush library: Photoshop .abr, Procreate .brushset and .brush, Clip Studio
// .sut, and images as tips. Imported brushes are saved as tip-brush folders under
// BrushLibrary::importFolder(), one folder per file, and appear in the Brush tool's picker.
#pragma once
#include <QString>
#include <QStringList>

class QWidget;

namespace app {

class EditorSession;

struct BrushImportResult {
    QStringList ids;      // the imported presets' BrushLibrary ids, in order
    QStringList notes;    // per file: what was approximated or left out
    QStringList errors;   // per file that could not be imported
};

/// Imports `paths` without asking anything; the library is reloaded when anything was imported.
BrushImportResult importBrushFiles(const QStringList& paths);

/// File > Import Brushes: asks for files, imports them, reports the result, and selects the first imported
/// brush on the Brush tool of `session`.
void importBrushesInteractively(QWidget* parent, EditorSession* session);

/// The file dialog's filter for brush files.
QString brushFileFilter();

} // namespace app
