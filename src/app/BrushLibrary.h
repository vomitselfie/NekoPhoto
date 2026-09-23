// The brush presets the Brush tool offers besides its round tip: the MyPaint collection compiled into the
// app (src/app/brushes/mypaint, in the order and groups of its order.conf) and any .myb files in the user's
// brushes folder.
#pragma once
#include <QIcon>
#include <QString>
#include <QStringList>
#include <string>
#include <vector>

namespace app {

struct BrushPreset {
    QString id;          // "classic/pencil"; a user preset is "user/<file name>"
    QString group;       // the group's display name
    QString name;        // the display name, from the file name
    std::string json;    // the .myb text
    QString previewPath; // the preset's 128-pixel preview, if it has one
    double diameter = 4; // the preset's own size, in pixels
    bool eraser = false;

    QIcon icon() const;
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
};

} // namespace app
