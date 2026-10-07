// Where a document came from and how it was opened: the file File > Revert reads again, through the same reader and
// with the same choices (a PSD's merged image, a PDF's page, Camera Raw's settings, the profile decision).
#pragma once
#include "ColorManagement.h"
#include "compositor/cameraraw.h"
#include <QJsonObject>
#include <QString>
#include <optional>

namespace app {

struct DocumentSource {
    enum class Kind {
        None,      // made here (File > New, a paste) or a smart object's contents: nothing to revert to
        Project,   // a .nekophoto file or a .comp folder, opened or last saved
        Layered,   // PSD/PSB, Clip Studio, Affinity, Aseprite, ICO, GIF, SVG, PDF (MainWindow::openLayeredFile)
        Image,     // PNG, JPEG, WebP, TIFF, TGA, ... opened as a document of its own
        Raw,       // camera RAW, developed through Camera Raw
    };
    Kind kind = Kind::None;
    QString path;
    bool mergedOnly = false;                 // a PSD or PSB opened as its merged image
    int pdfPage = 1;                         // a PDF: the page and the resolution it was rendered at
    double pdfResolution = 150;
    compositor::CameraRawSettings raw;       // camera RAW: the settings it was developed with
    bool rawAsObject = false;                // Open Object: a smart object keeping the RAW file
    int rawBits = 16;
    /// The profile the file embedded and what was decided for it as it opened (asked once, not again on Revert while
    /// the file embeds the same profile). Not kept across a crash: a recovered document decides again, without asking.
    std::optional<compositor::ColorProfile> embedded;
    std::optional<color::OpenDecision> colour;

    bool valid() const { return kind != Kind::None && !path.isEmpty(); }
    static DocumentSource project(const QString& path) { DocumentSource s; s.kind = Kind::Project; s.path = path; return s; }
    /// "project", "layered", "image", "raw" (automation replies); empty for None.
    QString kindName() const;
    QJsonObject toJson() const;
    static DocumentSource fromJson(const QJsonObject& json);
};

} // namespace app
