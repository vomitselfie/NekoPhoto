// The File menu (MainWindow::buildMenus, MainWindowMenus.h).
#include "MainWindowMenus.h"
#include "ExportAs.h"
#include "nekophoto/raw.h"
#include "CanvasFrame.h"
#include "BrushImporter.h"
#include "PresetLibrary.h"
#include <QMenuBar>
#include <QFileDialog>
#include <QSettings>
#include <QFileInfo>
#include <QJsonObject>

using namespace nekophoto;

namespace app {

using namespace menus;

void MainWindow::buildFileMenu(MenuContext& m) {
    QMenu* file = menuBar()->addMenu(tr("&File"));
    m.add(file, Spec("file.new", tr("&New…"), {QKeySequence::New}).runs([this] { newDocument(); }, "document.new"));
    m.add(file, Spec("file.open", tr("&Open…"), {QKeySequence::Open}).runs([this] { openFiles(); }, "document.open"));
    m.add(file, Spec("file.openProject", tr("Open Project Folder…")).runs([this] { openProject(); }, "document.open"));   // a .comp folder; .nekophoto files open with Open
    recentMenu_ = file->addMenu(tr("Open &Recent"));
    recentMenu_->setProperty("commandsDynamic", true);   // the recent files, listed as they are
    m.add(file, Spec("file.import", tr("Import &File…"), {QKeySequence("Ctrl+Shift+O")}).runs([this] { importFiles(); }, "document.import"));
    m.add(file, Spec("file.importBrushes", tr("Import &Brushes…")).runs([this] { importBrushesInteractively(this, session_); }, "brush.import"));
    m.add(file, Spec("file.importPresets", tr("I&mport Presets…")).runs([this] { importPresetsInteractively(this, session_); }, "presets.import"));
    m.add(file, Spec("file.placeEmbedded", tr("Place &Embedded…")).document("edit.smartObject").runs([this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Place Embedded"), QSettings().value("lastDir").toString(),
                                                          tr("Images, Photoshop and Affinity documents (*.psd *.psb *.afphoto *.afdesign *.afpub *.af *.png *.jpg *.jpeg *.tif *.tiff *.webp *.bmp *.gif %1)").arg(nekophoto::rawSupported() ? QStringLiteral("*.cr2 *.cr3 *.crw *.nef *.nrw *.arw *.srf *.sr2 *.raf *.orf *.rw2 *.rwl *.pef *.dng *.3fr *.iiq *.erf *.kdc *.dcr *.mrw *.srw *.x3f") : QString()));
        if (path.isEmpty()) return;
        QSettings().setValue("lastDir", QFileInfo(path).path());
        runCommand("smartObject.place", {{"path", path}}, tr("Couldn’t place %1").arg(QFileInfo(path).fileName()));
    }, "smartObject.place"));
    file->addSeparator();
    m.add(file, Spec("file.save", tr("&Save"), {QKeySequence::Save}).document("document.save").runs([this] { save(false); }, "document.save"));
    m.add(file, Spec("file.saveAs", tr("Save &As…"), {QKeySequence::SaveAs}).document("document.save").runs([this] { save(true); }, "document.save"));
    // File > Revert: the file it was opened from or last saved to, read again as one undo step (no question: Undo
    // brings the document back, as in Photoshop CC). Greyed until the document has changed.
    revertAction_ = m.add(file, Spec("file.revert", tr("Re&vert"), {QKeySequence("F12")}).request("document.revert", [] { return QJsonObject{}; }, tr("Revert")).when([this] {
        if (!session_->hasDocument()) return tr("No document is open");
        return session_->canRevert() ? QString() : tr("Nothing to revert to: the document has not changed since it was opened or saved");
    }));
    revertAction_->setObjectName("file.revert");
    revertAction_->installEventFilter(m.fieldGuard);
    file->addSeparator();
    // File > Export, as Photoshop's: Quick Export in the format Preferences choose, Export As for the flat formats
    // (PNG, JPEG, GIF, WebP, TIFF, TGA) with a size and a preview, then the formats with dialogs of their own.
    QMenu* exportMenu = file->addMenu(tr("E&xport"));
    QAction* quickExportAction = m.add(exportMenu, Spec("file.export.quick", tr("Quick Export")).document("export.png").runs([this] { quickExport(false); }, "document.export"));
    quickExportAction->setObjectName("export.quick");
    // Ctrl+Alt+Shift+W is Photoshop's Export As; Ctrl+Alt+Shift+S (its Save for Web) opened Export JPEG here before.
    QAction* exportAsAction = m.add(exportMenu, Spec("file.export.as", tr("Export &As…"), {QKeySequence("Ctrl+Alt+Shift+W"), QKeySequence("Ctrl+Alt+Shift+S")})
                                                  .document("export.png").runs([this] { exportAs(false); }, "document.export"));
    exportAsAction->setObjectName("export.as");
    connect(exportMenu, &QMenu::aboutToShow, this, [quickExportAction] { quickExportAction->setText(MainWindow::tr("Quick Export as %1").arg(exportas::formatLabel(exportas::quickExportFormat()))); });
    quickExportAction->setText(tr("Quick Export as %1").arg(exportas::formatLabel(exportas::quickExportFormat())));
    exportMenu->addSeparator();
    m.add(exportMenu, Spec("file.export.psd", tr("Export as Photoshop &Document (PSD)…")).document("export.psd").runs([this] { exportPsd(); }));
    m.add(exportMenu, Spec("file.export.svg", tr("Export S&VG…")).document("export.svg").runs([this] { exportSvg(); }));
    m.add(exportMenu, Spec("file.export.ico", tr("Export &Icon (ICO)…")).document("export.ico").runs([this] { exportIco(); }));
    m.add(exportMenu, Spec("file.export.gif", tr("E&xport Animated GIF…")).document("export.gif").runs([this] { exportGif(); }));
    exportMenu->addSeparator();
    m.add(exportMenu, Spec("file.export.artboards", tr("Export Artboards to Files…")).document("export.artboards").runs([this] { exportBoxes(false); }));
    m.add(exportMenu, Spec("file.export.slices", tr("Export S&lices…")).document("export.slices").runs([this] { exportBoxes(true); }));
    file->addSeparator();
    QMenu* automate = file->addMenu(tr("A&utomate"));
    m.add(automate, Spec("file.automate.batch", tr("&Batch…")).runs([this] { showBatchDialog(); }, "actions.batch"));
    file->addSeparator();
    m.add(file, Spec("file.closeTab", tr("&Close Tab"), {QKeySequence::Close}).runs([this] { closeTab(current_); }));
    // No default key: Ctrl+T (the platform's New Tab) is Photoshop's Free Transform.
    m.add(file, Spec("file.newTab", tr("New Tab")).runs([this] { addTab(false); }));
    m.add(file, Spec("file.nextTab", tr("Next Tab"), {QKeySequence("Ctrl+Tab")}).runs([this] { if (tabs_.size() > 1) switchTo((current_ + 1) % int(tabs_.size())); }));
    m.add(file, Spec("file.previousTab", tr("Previous Tab"), {QKeySequence("Ctrl+Shift+Tab")}).runs([this] { if (tabs_.size() > 1) switchTo((current_ + int(tabs_.size()) - 1) % int(tabs_.size())); }));
    m.add(file, Spec("file.quit", tr("&Quit"), {QKeySequence::Quit}).runs([this] { close(); }));
}

} // namespace app
