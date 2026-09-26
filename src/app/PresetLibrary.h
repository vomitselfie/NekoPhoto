// Imported presets: layer styles (.asl), patterns (.pat, and the ones styles carry) and gradients (.grd), kept under
// the app data folder (presets/styles.json, presets/patterns.pat, presets/gradients.grd) so they outlast the session.
// File ▸ Import Presets… and automation's presets.import fill it; Layer ▸ Layer Style ▸ Apply Style, the Gradient
// tool's preset list and the Layer Style dialog's gradient rows read it. See docs/presets.md.
#pragma once
#include "compositor/presets.h"
#include <QObject>
#include <QStringList>

class QWidget;

namespace app {

class EditorSession;

class PresetLibrary : public QObject {
    Q_OBJECT
public:
    static PresetLibrary& instance();
    /// Where the files live (created on the first save).
    static QString folder();

    const std::vector<compositor::StylePreset>& styles();
    const std::vector<compositor::PatternPreset>& patterns();
    const std::vector<compositor::GradientPreset>& gradients();
    const compositor::StylePreset* findStyle(const QString& name);
    const compositor::GradientPreset* findGradient(const QString& name);
    /// The library's patterns a style uses (the ones it has).
    std::vector<compositor::PatternPreset> patternsFor(const compositor::LayerStyle& style);

    struct ImportResult {
        QStringList styles, patterns, gradients;   // names imported
        std::vector<compositor::PatternPreset> importedPatterns;   // the .pat files' patterns (to give the open document)
        QStringList notes, errors;
    };
    /// Imports .asl, .pat and .grd files. A style or gradient with the name of one already there replaces it; a
    /// pattern whose id is already there is kept as it was.
    ImportResult importFiles(const QStringList& paths);
    /// Removes a style or gradient by name; false when there is none.
    bool removeStyle(const QString& name);
    bool removeGradient(const QString& name);
    /// Removes a pattern by id (or by name); styles that use it then draw without it.
    bool removePattern(const QString& idOrName);

signals:
    void changed();

private:
    PresetLibrary() = default;
    void load();
    void save();
    bool loaded_ = false;
    std::vector<compositor::StylePreset> styles_;
    std::vector<compositor::PatternPreset> patterns_;
    std::vector<compositor::GradientPreset> gradients_;
};

/// The file dialog's filter for preset files.
QString presetFileFilter();
/// File ▸ Import Presets…: asks for files, imports them, gives the open document the imported .pat patterns (one undo
/// step) and reports what came in.
void importPresetsInteractively(QWidget* parent, EditorSession* session);

} // namespace app
