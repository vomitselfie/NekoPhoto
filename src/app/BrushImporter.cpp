#include "BrushImporter.h"
#include "BrushLibrary.h"
#include "EditorSession.h"
#include "ImageConvert.h"
#include "compositor/brushimport.h"
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QImageReader>
#include <QMessageBox>
#include <QSettings>

namespace app {

QString brushFileFilter() {
    return QObject::tr("Brushes (*.abr *.brushset *.brush *.sut *.png *.jpg *.jpeg *.webp *.tif *.tiff *.bmp);;All files (*)");
}

BrushImportResult importBrushFiles(const QStringList& paths) {
    BrushImportResult result;
    const std::string root = BrushLibrary::importFolder().toStdString();
    for (const QString& path : paths) {
        const QFileInfo info(path);
        std::string error;
        std::optional<compositor::BrushImport> import = compositor::importBrushFile(path.toStdString(), &error);
        // A raster image the core does not decode itself becomes one tip through Qt's readers.
        if (!import && !QImageReader::imageFormat(path).isEmpty()) {
            QImageReader reader(path);
            reader.setAutoTransform(true);
            const QImage image = reader.read();
            if (!image.isNull()) {
                if (auto preset = compositor::presetFromImage(*fromQImage(image), info.completeBaseName().toStdString()))
                    import = compositor::BrushImport{"Images", {*preset}, {}};
                else error = "nothing in the image would paint";
            }
        }
        if (!import) { result.errors << QObject::tr("%1: %2").arg(info.fileName(), QString::fromStdString(error)); continue; }
        std::vector<std::string> written;
        if (!compositor::saveBrushImport(*import, root, &written, &error)) { result.errors << QObject::tr("%1: %2").arg(info.fileName(), QString::fromStdString(error)); continue; }
        for (const std::string& folder : written) {
            const QFileInfo brush(QString::fromStdString(folder));
            result.ids << QStringLiteral("imported/%1/%2").arg(brush.dir().dirName(), brush.fileName());
        }
        for (const std::string& note : import->notes) result.notes << QObject::tr("%1: %2").arg(info.fileName(), QString::fromStdString(note));
    }
    if (!result.ids.isEmpty()) BrushLibrary::reload();
    return result;
}

void importBrushesInteractively(QWidget* parent, EditorSession* session) {
    QSettings settings;
    const QStringList paths = QFileDialog::getOpenFileNames(parent, QObject::tr("Import Brushes"), settings.value("brushes/importDir", QDir::homePath()).toString(), brushFileFilter());
    if (paths.isEmpty()) return;
    settings.setValue("brushes/importDir", QFileInfo(paths.first()).absolutePath());
    const BrushImportResult result = importBrushFiles(paths);
    QString text;
    if (!result.ids.isEmpty()) text = QObject::tr("Imported %n brush(es). They are in the Brush tool's picker.", nullptr, int(result.ids.size()));
    if (!result.notes.isEmpty()) text += "\n\n" + QObject::tr("Approximated:") + "\n" + result.notes.join('\n');
    if (!result.errors.isEmpty()) text += (text.isEmpty() ? QString() : QStringLiteral("\n\n")) + QObject::tr("Not imported:") + "\n" + result.errors.join('\n');
    if (result.ids.isEmpty()) QMessageBox::warning(parent, QObject::tr("Import Brushes"), text);
    else QMessageBox::information(parent, QObject::tr("Import Brushes"), text);
    if (!result.ids.isEmpty() && session) {
        session->brushPreset = result.ids.first();
        if (const BrushPreset* preset = BrushLibrary::find(result.ids.first())) session->brushSettings.diameter = preset->diameter;
        session->selectTool(Tool::Brush);
        emit session->toolChanged();
    }
}

} // namespace app
