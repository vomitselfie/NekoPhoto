// The brush presets the Brush tool offers besides its round tip: the MyPaint collection compiled into the
// app (src/app/brushes/mypaint, in the order and groups of its order.conf), any .myb files in the user's
// brushes folder, and the tip brushes imported from Photoshop, Procreate and other applications, each a
// folder under brushes/imported/<set>/.
#pragma once
#include "compositor/tipbrush.h"
#include <QIcon>
#include <QString>
#include <QStringList>
#include <string>
#include <vector>

namespace app {

struct BrushPreset {
    enum class Engine { MyPaint, Tip };
    Engine engine = Engine::MyPaint;
    QString id;          // "classic/pencil"; a user preset is "user/<file name>", an imported one "imported/<set>/<brush>"
    QString group;       // the group's display name
    QString name;        // the display name
    std::string json;    // MyPaint: the .myb text
    QString folder;      // Tip: the preset's folder
    QString previewPath; // the preset's preview image, if it has one
    double diameter = 4; // the preset's own size, in pixels
    bool eraser = false;

    QIcon icon() const;
    /// Tip: the tip and its settings, read from the folder on first use.
    std::shared_ptr<const compositor::TipPreset> tip() const;

private:
    mutable std::shared_ptr<const compositor::TipPreset> tip_;
};

class BrushLibrary {
public:
    /// Every preset, grouped and ordered as the collection orders them; loaded once.
    static const std::vector<BrushPreset>& presets();
    static const BrushPreset* find(const QString& id);
    /// The group names in their order.
    static QStringList groups();
    /// Where the user's own .myb presets are read from.
    static QString userFolder();
    /// Where imported tip brushes are kept: one folder per set, one per brush inside it.
    static QString importFolder();
    /// Reads the folders again, after an import.
    static void reload();
};

} // namespace app
