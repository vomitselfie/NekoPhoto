// The main window's file handling: new, open, import, save, export, and files dropped on the window.
#include "TextLayer.h"
#include "MainWindow.h"
#include "ActionLibrary.h"
#include "Automation.h"
#include "ColorManagement.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLineEdit>
#include <QProgressDialog>
#include "compositor/raw.h"
#include "compositor/psd_writer.h"
#include "CanvasWidget.h"
#include "Dialogs.h"
#include "VectorFiles.h"
#include "compositor/svg.h"
#include "ImageConvert.h"
#include "compositor/aseprite.h"
#include "compositor/affinity.h"
#include "compositor/clip.h"
#include "compositor/gif.h"
#include "compositor/ico.h"
#include "compositor/tga.h"
#include "compositor/png.h"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QSpinBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QImageReader>
#include <QMessageBox>
#include <QPushButton>
#include <QMimeData>
#include <QPainter>
#include <QSettings>
#include <QStatusBar>

using namespace compositor;

namespace app {

namespace {

QString imageFilter() {
    QStringList patterns;
    for (auto& format : QImageReader::supportedImageFormats()) patterns << "*." + QString::fromLatin1(format);
    return QObject::tr("Images (%1)").arg(patterns.join(' '));
}

/// Everything File > Open and Import File take: Photoshop, Clip Studio and Aseprite files, images (TGA, ICO and GIF
/// through the core's own readers), and a project's manifest.json.
QString openFilter() {
    QStringList patterns = {"*.psd", "*.psb", "*.clip", "*.ase", "*.aseprite", "*.tga", "*.ico", "*.cur", "*.gif", "*.afphoto", "*.afdesign", "*.afpub", "*.af", "*.svg", "*.svgz", "manifest.json"};
    if (pdfSupported()) patterns << "*.pdf";
    for (auto& format : QImageReader::supportedImageFormats()) patterns << "*." + QString::fromLatin1(format);
    patterns.removeDuplicates();
    return QObject::tr("Images, layered files and projects (%1)").arg(patterns.join(' ')) + ";;" + imageFilter() + ";;"
        + QObject::tr("Photoshop files (*.psd *.psb)") + ";;" + QObject::tr("Clip Studio files (*.clip)") + ";;"
        + QObject::tr("Aseprite files (*.ase *.aseprite)") + ";;" + QObject::tr("Icons (*.ico *.cur)") + ";;" + QObject::tr("TGA images (*.tga)") + ";;"
        + QObject::tr("Affinity files (*.afphoto *.afdesign *.afpub *.af)") + ";;" + QObject::tr("SVG files (*.svg *.svgz)")
        + (pdfSupported() ? ";;" + QObject::tr("PDF files (*.pdf)") : QString());
}

bool isProjectPath(const QString& path) { return path.endsWith(".comp", Qt::CaseInsensitive) && QFileInfo(path).isDir(); }

bool hasSuffix(const QString& path, std::initializer_list<const char*> suffixes) {
    for (const char* s : suffixes) if (path.endsWith(QLatin1String(s), Qt::CaseInsensitive)) return true;
    return false;
}

/// SVG and PDF: opened through VectorFiles.h (shape layers, a rendered page).
bool isVectorFilePath(const QString& path) {
    return path.endsWith(".svg", Qt::CaseInsensitive) || path.endsWith(".svgz", Qt::CaseInsensitive) || path.endsWith(".pdf", Qt::CaseInsensitive);
}

} // namespace

bool isLayeredPath(const QString& path) {
    if (hasSuffix(path, {".psd", ".psb", ".clip", ".ico", ".cur", ".ase", ".aseprite", ".afphoto", ".afdesign", ".afpub", ".af"}) || isVectorFilePath(path)) return true;
    // An animated GIF's frames become layers; a still one opens as an image.
    return path.endsWith(".gif", Qt::CaseInsensitive) && QFileInfo(path).isFile() && compositor::gifFrameCount(path.toStdString()) > 1;
}

void MainWindow::newDocument() {
    auto options = askNewDocument(this, {});
    if (!options) return;
    Tab& tab = addTab(true);
    tab.session->createDocument(options->width, options->height, options->resolution, true);
    tab.session->adoptProfile(color::newDocumentProfile());   // the working space (Edit > Color Settings)
}

void MainWindow::openAsDocument(const QString& path) {
    if (isLayeredPath(path) || isProjectPath(path)) { openPath(path); return; }
    QString error;
    if (!openImageAsDocument(path, &error)) showError(tr("Couldn’t open the file"), error.isEmpty() ? path : error);
}

void MainWindow::openPath(const QString& path) {
    if (isLayeredPath(path)) { openLayeredFile(path); return; }
    if (isProjectPath(path)) {
        QString canonical = QFileInfo(path).canonicalFilePath();
        for (size_t i = 0; i < tabs_.size(); i++)
            if (!tabs_[i].session->projectPath().isEmpty() && QFileInfo(tabs_[i].session->projectPath()).canonicalFilePath() == canonical) { switchTo(int(i)); return; }
        // Read once, then give it a tab: a failed open never disturbs one.
        QString error;
        auto project = EditorSession::readProject(path, &error);
        if (!project) { showError(tr("Couldn’t open the project"), error); return; }
        addTab(true).session->installProject(std::move(*project));
        addRecent(path);
        return;
    }
    importFile(path);
}

void MainWindow::openLayeredFile(const QString& path) {
    // The import reads the whole file; a big one takes a moment.
    QApplication::setOverrideCursor(Qt::BusyCursor);
    std::string error;
    const std::string file = path.toStdString();
    const bool affinity = hasSuffix(path, {".afphoto", ".afdesign", ".afpub", ".af"});
    const bool clip = hasSuffix(path, {".clip"}), ase = hasSuffix(path, {".ase", ".aseprite"}), psd = hasSuffix(path, {".psd", ".psb"});
    const bool vector = isVectorFilePath(path);
    std::optional<PsdImport> imported;
    if (vector) {
        // SVG as shape layers, PDF as a rendered page (VectorFiles.h).
        QString message;
        imported = path.endsWith(".pdf", Qt::CaseInsensitive) ? app::importPdfDocument(path, &message, isVisible() ? this : nullptr) : app::importSvgDocument(path, &message);
        error = message.toStdString();
    } else if (clip) imported = compositor::importClip(file, &error);
    else if (affinity) imported = compositor::importAffinity(file, &error, app::affinityImportOptions());
    else if (ase) imported = compositor::importAseprite(file, &error);
    else if (hasSuffix(path, {".ico", ".cur"})) imported = compositor::importIco(file, &error);
    else if (hasSuffix(path, {".gif"})) imported = compositor::importGif(file, &error);
    else imported = compositor::importPsd(file, &error, app::psdImportOptions());
    QApplication::restoreOverrideCursor();
    if (!imported) {
        if (!error.empty() || !vector) showError(tr("Couldn’t open %1").arg(QFileInfo(path).fileName()), QString::fromStdString(error));   // empty: the page choice was cancelled
        return;
    }
    // The same budgets as every other way in (each importer also stops early on its own limits).
    if (const compositor::BudgetCheck check = compositor::Document::withinBudget(imported->document); !check) {
        showError(tr("Couldn’t open %1").arg(QFileInfo(path).fileName()), EditorSession::budgetText(check));
        return;
    }
    if (psd) {
        app::finishPsdText(*imported);
        // The file's profile (resource 1039) by Color Settings' policy: kept, converted to the working space, or dropped.
        const compositor::ColorProfile embedded = imported->document.profile;
        color::applyToDocument(imported->document, color::decideOnOpen(embedded.empty() ? std::nullopt : std::optional(embedded), this));
    }
    if (affinity) app::finishPendingText(*imported);
    Tab& tab = addTab(true);
    tab.session->adoptDocument(imported->document, QFileInfo(path).completeBaseName());
    addRecent(path);
    lastImportNotes_.clear();
    for (const std::string& note : imported->notes) lastImportNotes_ << QString::fromStdString(note);
    if (!lastImportNotes_.isEmpty()) {
        // A bar over the canvas, not a dialog: the document is usable at once, Details lists every note.
        const int layers = int(imported->document.layers.size());
        const QString heading = vector ? tr("%n layer(s) imported. Some things in the file were approximated or drawn as pixels:", nullptr, layers)
            : clip ? tr("%n layer(s) imported. Some things Clip Studio keeps have no counterpart here:", nullptr, layers)
            : ase ? tr("%n layer(s) imported. Some things Aseprite keeps have no counterpart here:", nullptr, layers)
            : affinity ? tr("%n layer(s) imported. Some things Affinity keeps have no counterpart here:", nullptr, layers)
            : psd ? tr("%n layer(s) imported. Some things Photoshop keeps have no counterpart here:", nullptr, layers)
                  : tr("%n layer(s) imported, with notes:", nullptr, layers);
        showImportNotes(tr("Opened with %n change(s): %1", nullptr, int(lastImportNotes_.size())), tr("Imported %1").arg(QFileInfo(path).fileName()),
                        heading, lastImportNotes_, tab.session);
    }
}

void MainWindow::openProject() {
    // A .comp project is a folder, so the picker chooses a directory.
    QString path = QFileDialog::getExistingDirectory(this, tr("Open Project (a .comp folder)"), QSettings().value("lastDir").toString());
    if (path.isEmpty()) return;
    if (!path.endsWith(".comp", Qt::CaseInsensitive)) { showError(tr("Not a project"), tr("Choose a folder ending in .comp.")); return; }
    QSettings().setValue("lastDir", QFileInfo(path).path());
    openPath(path);
}

void MainWindow::importFile(const QString& path, std::optional<QPointF> at) {
    QString error;
    if (!importImageFile(path, at, &error)) showError(tr("Couldn’t import %1").arg(QFileInfo(path).fileName()), error);
}

namespace {
/// An image file's pixels: 16-bit PNG and TIFF files at 16 bits (they open as 16-bit documents), the rest at 8; with
/// the profile the file embeds (PNG iCCP, JPEG APP2, TIFF), when it has a usable one.
compositor::AnyImage readImageFileUnchecked(const QString& path, QString* error, std::optional<compositor::ColorProfile>* embedded) {
    if (compositor::isRawPath(path.toStdString())) {
        // A camera RAW file, developed through LibRaw (a few seconds for a large sensor).
        QApplication::setOverrideCursor(Qt::BusyCursor);
        std::string message;
        auto developed = compositor::decodeRaw(path.toStdString(), &message);
        QApplication::restoreOverrideCursor();
        if (!developed && error) *error = QString::fromStdString(message);
        return developed;
    }
    // TGA through the core's reader, whichever Qt image plugins are installed.
    if (path.endsWith(".tga", Qt::CaseInsensitive)) {
        std::string why;
        auto image = compositor::readTgaImage(path.toStdString(), &why);
        if (!image && error) *error = QString::fromStdString(why);
        return image;
    }
    if (path.endsWith(".png", Qt::CaseInsensitive)) {
        compositor::PngInfo info;
        if (compositor::readPngInfo(path.toStdString(), info) && info.bitDepth == 16) {
            *embedded = color::embeddedProfile(QByteArray(reinterpret_cast<const char*>(info.icc.data()), qsizetype(info.icc.size())));
            std::string why;
            auto deep = compositor::readPngImage16(path.toStdString(), &why);
            if (!deep && error) *error = QString::fromStdString(why);
            return compositor::Image16Ptr(deep);
        }
    }
    QImageReader reader(path);
    reader.setAutoTransform(true);
    // The size from the header, when the format gives one, before anything is decoded (at 8 bits, the larger budget:
    // the decoded image is checked again at its own depth).
    if (const QSize size = reader.size(); size.isValid()) {
        if (const compositor::BudgetCheck check = compositor::Document::canCreate(size.width(), size.height(), compositor::SampleType::U8); !check) {
            if (error) *error = EditorSession::budgetText(check);
            return nullptr;
        }
    }
    QImage image = reader.read();
    if (image.isNull()) { if (error) *error = reader.errorString(); return nullptr; }
    if (image.colorSpace().isValid()) *embedded = color::embeddedProfile(image.colorSpace().iccProfile());
    // 16 bits per channel through Qt (a 16-bit TIFF): kept at 16 bits.
    if (image.depth() == 64) return compositor::Image16Ptr(fromQImage16(image));
    return compositor::ImagePtr(fromQImage(image));
}

/// An image file's pixels, refused (saying why) when they break the budgets for a canvas or image at their depth.
compositor::AnyImage readImageFile(const QString& path, QString* error, std::optional<compositor::ColorProfile>* embedded) {
    compositor::AnyImage image = readImageFileUnchecked(path, error, embedded);
    if (!image) return image;
    if (const compositor::BudgetCheck check = compositor::Document::canCreate(image.width(), image.height(), image.sampleType()); !check) {
        if (error) *error = EditorSession::budgetText(check);
        return nullptr;
    }
    return image;
}
} // namespace

bool MainWindow::importImageFile(const QString& path, std::optional<QPointF> at, QString* error) {
    std::optional<compositor::ColorProfile> embedded;
    auto image = readImageFile(path, error, &embedded);
    if (!image) return false;
    // Into a document: converted from the file's profile to the document's. A first image makes the document, and
    // its profile follows Color Settings' policy.
    const bool first = !session_->hasDocument();
    color::OpenDecision decision;
    if (first) { decision = color::decideOnOpen(embedded, this); image = color::applyToImage(image, decision); }
    else image = color::convertForDocument(image, embedded, session_->document()->profile);
    if (!session_->insertImage(image, QFileInfo(path).completeBaseName(), at, error)) return false;   // at the document's depth
    if (first) session_->adoptProfile(decision.profile);
    addRecent(path);
    return true;
}

bool MainWindow::openImageAsDocument(const QString& path, QString* error) {
    std::optional<compositor::ColorProfile> embedded;
    auto image = readImageFile(path, error, &embedded);
    if (!image) return false;
    const color::OpenDecision decision = color::decideOnOpen(embedded, this);
    image = color::applyToImage(image, decision);
    Tab& tab = addTab(true);
    tab.session->insertImage(image, QFileInfo(path).completeBaseName(), std::nullopt);   // a first image makes the canvas
    tab.session->adoptProfile(decision.profile);
    tab.defaultName = QFileInfo(path).completeBaseName();
    refreshTabTitles();
    addRecent(path);
    return true;
}

bool MainWindow::overTabStrip(const QPointF& windowPosition) const {
    return tabBar_->rect().contains(tabBar_->mapFrom(this, windowPosition.toPoint()));
}

void MainWindow::openFiles() {
    // Photoshop and Clip Studio files, images, and a project picked by its manifest.json (a .comp is a folder, which a file
    // picker cannot choose); each opens in its own tab.
    QStringList paths = QFileDialog::getOpenFileNames(this, tr("Open"), QSettings().value("lastDir").toString(), openFilter());
    if (paths.isEmpty()) return;
    QSettings().setValue("lastDir", QFileInfo(paths.first()).path());
    for (QString path : paths) {
        const QFileInfo info(path);
        if (info.fileName() == "manifest.json" && isProjectPath(info.path())) path = info.path();
        openAsDocument(path);
    }
}

void MainWindow::importFiles() {
    // Images land as layers in the open document; layered files, or anything with no document open, get a tab.
    QStringList paths = QFileDialog::getOpenFileNames(this, tr("Import File"), QSettings().value("lastDir").toString(), openFilter());
    if (paths.isEmpty()) return;
    QSettings().setValue("lastDir", QFileInfo(paths.first()).path());
    for (const QString& path : paths) {
        if (isLayeredPath(path) || !session_->hasDocument()) openAsDocument(path);
        else importFile(path);
    }
}

bool MainWindow::editSmartObjectContents(QString* errorOut) {
    QString error;
    auto contents = session_->smartObjectContentsForEditing(&error);
    if (!contents) {
        if (errorOut) *errorOut = error; else showError(tr("Couldn’t open the contents"), error);
        return false;
    }
    EditorSession* parent = session_;
    const Layer* layer = parent->activeLayer();
    const QString parentName = parent->title().remove(QStringLiteral(" *"));
    const QString fileName = layer && layer->smartObject && parent->document()->smartObjects.count(layer->smartObject->sourceId)
        ? QString::fromStdString(parent->document()->smartObjects.at(layer->smartObject->sourceId)->fileName) : tr("Contents");
    Tab& tab = addTab(false);
    tab.session->adoptDocument(contents->first, tr("%1 (in %2)").arg(fileName, parentName));
    tab.session->setSmartObjectParent(parent, contents->second);
    statusBar()->showMessage(tr("Editing the contents of %1: Save puts them back.").arg(fileName), 8000);
    return true;
}

bool MainWindow::save(bool asNew) {
    if (!session_->hasDocument()) return false;
    session_->endTemporaryLayers();
    // A smart object's contents go back to it (Save As saves them as a project of their own instead).
    if (!asNew && session_->smartObjectParent()) {
        QString error;
        if (!session_->commitToSmartObjectParent(&error)) { showError(tr("Couldn’t put the contents back"), error); return false; }
        refreshTabTitles();
        statusBar()->showMessage(tr("Contents saved into the smart object."), 5000);
        return true;
    }
    QString path = session_->projectPath();
    if (asNew || path.isEmpty()) {
        QString suggested = QDir(QSettings().value("lastDir").toString()).filePath((path.isEmpty() ? QStringLiteral("Untitled") : QFileInfo(path).completeBaseName()) + ".comp");
        path = QFileDialog::getSaveFileName(this, tr("Save Project"), suggested, tr("Compositor project (*.comp)"));
        if (path.isEmpty()) return false;
        if (!path.endsWith(".comp", Qt::CaseInsensitive)) path += ".comp";
        QSettings().setValue("lastDir", QFileInfo(path).path());
    }
    QString error;
    if (!session_->saveProject(path, &error)) { showError(tr("Couldn’t save the project"), error); return false; }
    addRecent(path);
    if (!session_->document()->fitsMacBudget())
        statusBar()->showMessage(tr("Saved. With %1 megapixels of layers this project is larger than Compositor for macOS opens (100); it opens here.")
                                     .arg(std::max(session_->document()->layerPixels(), session_->document()->maskPixels()) / 1000000), 10000);
    return true;
}

void MainWindow::exportPng() {
    session_->endTemporaryLayers();   // the Quick Mask and filter-mask layers are never written
    if (!session_->hasDocument()) return;
    QString suggested = QDir(QSettings().value("lastDir").toString()).filePath((session_->projectPath().isEmpty() ? QStringLiteral("Untitled") : QFileInfo(session_->projectPath()).completeBaseName()) + ".png");
    QString path = QFileDialog::getSaveFileName(this, tr("Export PNG"), suggested, tr("PNG image (*.png)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(".png", Qt::CaseInsensitive)) path += ".png";
    std::string error;
    // The document's profile embedded (iCCP), or the pixels converted to sRGB for the web.
    const Document& doc = *session_->document();
    const auto convert = color::askConvertToSrgb(this, doc, false);
    if (!convert) return;
    const color::ExportPlan plan = color::exportPlan(doc, *convert);
    const std::vector<uint8_t>* icc = plan.icc.empty() ? nullptr : &plan.icc;
    // A 16-bit document as a 16-bit PNG; a 32-bit one too, tone-mapped at exposure 0.
    if (session_->sampleType() != SampleType::U8) {
        auto deep = color::flatten16(doc, plan);
        if (!deep || !writePngImage16(path.toStdString(), *deep, doc.resolution, &error, icc)) showError(tr("Couldn’t export PNG"), QString::fromStdString(error));
        else if (session_->sampleType() == SampleType::F32)
            statusBar()->showMessage(tr("Exported %1 at 16 bits per channel, tone-mapped from 32 bits at exposure 0 (values above white are clipped).").arg(QFileInfo(path).fileName()), 8000);
        return;
    }
    auto image = color::flatten8(doc, plan);
    if (!image || !writePngImage(path.toStdString(), *image, doc.resolution, &error, icc)) showError(tr("Couldn’t export PNG"), QString::fromStdString(error));
}

void MainWindow::noteDitheredExport(const QString& path) {
    // The 8-bit formats take a 16-bit document dithered down to 8 bits per channel (EditorSession::flattened), a
    // 32-bit one tone-mapped at exposure 0.
    if (session_->sampleType() == SampleType::F32)
        statusBar()->showMessage(tr("Exported %1 at 8 bits per channel, tone-mapped from 32 bits at exposure 0 (values above white are clipped).").arg(QFileInfo(path).fileName()), 8000);
    else if (session_->sampleType() != SampleType::U8)
        statusBar()->showMessage(tr("Exported %1, reduced from 16 to 8 bits per channel with dithering.").arg(QFileInfo(path).fileName()), 8000);
}

void MainWindow::exportPsd() {
    session_->endTemporaryLayers();   // the Quick Mask and filter-mask layers are never written
    if (!session_->hasDocument()) return;
    const compositor::Document& doc = *session_->document();
    if (doc.width > compositor::psdMaxSide || doc.height > compositor::psdMaxSide) {
        showError(tr("Couldn’t export PSD"), tr("This document is larger than PSD allows (%1 pixels a side). PSB export is not supported yet.").arg(compositor::psdMaxSide));
        return;
    }
    // What the file will hold, and what will not look or behave the same in Photoshop, before choosing where.
    const compositor::PsdExportSummary plan = compositor::planPsdExport(doc, app::psdExportOptions());
    if (!plan.warnings.empty()) {
        QString counts = tr("%n layer(s)", "", plan.layers + plan.adjustments);
        if (plan.folders) counts += tr(", %n folder(s)", "", plan.folders);
        if (plan.masks) counts += tr(", %n mask(s)", "", plan.masks);
        if (plan.clipped) counts += tr(", %n clipped", "", plan.clipped);
        QString list;
        for (const std::string& w : plan.warnings) list += "<li>" + QString::fromStdString(w).toHtmlEscaped() + "</li>";
        QMessageBox box(QMessageBox::Information, tr("Export Photoshop Document"),
                        tr("<p>%1 will be written.</p><p>Some things will look or behave differently in Photoshop:</p><ul>%2</ul>").arg(counts, list),
                        QMessageBox::NoButton, this);
        if (!plan.notes.empty()) {
            QString notes;
            for (const std::string& n : plan.notes) notes += QString::fromStdString(n) + "\n";
            box.setDetailedText(notes.trimmed());
        }
        QPushButton* go = box.addButton(tr("Export…"), QMessageBox::AcceptRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(go);
        box.exec();
        if (box.clickedButton() != static_cast<QAbstractButton*>(go)) return;
    }
    QString path = askExportPath(tr("Export Photoshop Document"), tr("Photoshop document (*.psd);;Photoshop large document (*.psb)"), {"psd", "psb"});
    if (path.isEmpty()) return;
    QSettings().setValue("lastDir", QFileInfo(path).absolutePath());
    QApplication::setOverrideCursor(Qt::WaitCursor);
    compositor::PsdExportSummary summary;
    std::string error;
    compositor::PsdExportOptions options = app::psdExportOptions();
    options.large = path.endsWith(".psb", Qt::CaseInsensitive);
    const bool ok = compositor::exportPsd(doc, path.toStdString(), options, &summary, &error);
    QApplication::restoreOverrideCursor();
    if (!ok) { showError(tr("Couldn’t export PSD"), QString::fromStdString(error)); return; }
    statusBar()->showMessage(tr("Exported %1").arg(QFileInfo(path).fileName()), 5000);
}

void MainWindow::exportSvg() {
    session_->endTemporaryLayers();   // the Quick Mask and filter-mask layers are never written
    if (!session_->hasDocument()) return;
    QString path = askExportPath(tr("Export SVG"), tr("SVG image (*.svg)"), {"svg"});
    if (path.isEmpty()) return;
    QSettings().setValue("lastDir", QFileInfo(path).absolutePath());
    QApplication::setOverrideCursor(Qt::WaitCursor);
    compositor::SvgExportSummary summary;
    std::string error;
    const bool ok = compositor::exportSvg(*session_->document(), path.toStdString(), &summary, &error);
    QApplication::restoreOverrideCursor();
    if (!ok) { showError(tr("Couldn’t export SVG"), QString::fromStdString(error)); return; }
    statusBar()->showMessage(summary.images ? tr("Exported %1 (%n image(s) for what SVG cannot draw as paths)", nullptr, summary.images).arg(QFileInfo(path).fileName())
                                            : tr("Exported %1").arg(QFileInfo(path).fileName()), 5000);
}

QString MainWindow::askExportPath(const QString& title, const QString& filter, const QStringList& suffixes) {
    QString suggested = QDir(QSettings().value("lastDir").toString()).filePath((session_->projectPath().isEmpty() ? QStringLiteral("Untitled") : QFileInfo(session_->projectPath()).completeBaseName()) + "." + suffixes.first());
    QString path = QFileDialog::getSaveFileName(this, title, suggested, filter);
    if (path.isEmpty()) return path;
    if (std::none_of(suffixes.begin(), suffixes.end(), [&](const QString& s) { return path.endsWith("." + s, Qt::CaseInsensitive); })) path += "." + suffixes.first();
    return path;
}

void MainWindow::exportJpeg() {
    session_->endTemporaryLayers();   // the Quick Mask and filter-mask layers are never written
    if (!session_->hasDocument()) return;
    const auto convert = color::askConvertToSrgb(this, *session_->document(), false);
    if (!convert) return;
    const color::ExportPlan plan = color::exportPlan(*session_->document(), *convert);
    auto flattened = color::flatten8(*session_->document(), plan);
    if (!flattened) return;
    QImage image = toQImage(*flattened);
    auto options = askJpegExport(this, image);
    if (!options) return;
    QString path = askExportPath(tr("Export JPEG"), tr("JPEG image (*.jpg *.jpeg)"), {"jpg", "jpeg"});
    if (path.isEmpty()) return;
    QImage flat(image.size(), QImage::Format_RGB32);
    flat.fill(options->background);
    QPainter p(&flat);
    p.drawImage(0, 0, image);
    p.end();
    QString error;
    if (!writeQtImage(path, "jpeg", flat, options->quality, session_->document()->resolution, &error, plan.iccBytes())) showError(tr("Couldn’t export JPEG"), error);
    else noteDitheredExport(path);
}

void MainWindow::exportWebp() {
    session_->endTemporaryLayers();   // the Quick Mask and filter-mask layers are never written
    if (!session_->hasDocument()) return;
    const auto convert = color::askConvertToSrgb(this, *session_->document(), false);
    if (!convert) return;
    const color::ExportPlan plan = color::exportPlan(*session_->document(), *convert);
    auto flattened = color::flatten8(*session_->document(), plan);
    if (!flattened) return;
    QImage image = toQImage(*flattened);
    auto options = askJpegExport(this, image, true);
    if (!options) return;
    QString path = askExportPath(tr("Export WebP"), tr("WebP image (*.webp)"), {"webp"});
    if (path.isEmpty()) return;
    QString error;
    if (!writeQtImage(path, "webp", image, options->quality, session_->document()->resolution, &error, plan.iccBytes())) showError(tr("Couldn’t export WebP"), error);
    else noteDitheredExport(path);
}

void MainWindow::exportTiff() {
    session_->endTemporaryLayers();   // the Quick Mask and filter-mask layers are never written
    if (!session_->hasDocument()) return;
    auto flattened = session_->flattened();
    if (!flattened) return;
    QString path = askExportPath(tr("Export TIFF"), tr("TIFF image (*.tif *.tiff)"), {"tif", "tiff"});
    if (path.isEmpty()) return;
    QString error;
    // A 16-bit document as a 16-bit TIFF, when Qt's TIFF plugin writes one.
    const bool deep = session_->sampleType() != SampleType::U8 && canWriteDeepTiff();
    // The document's profile embedded.
    const QByteArray icc = color::exportPlan(*session_->document(), false).iccBytes();
    if (!writeQtImage(path, "tiff", deep ? toQImage16(*session_->flattened16()) : toQImage(*flattened), 100, session_->document()->resolution, &error, icc)) showError(tr("Couldn’t export TIFF"), error);
    else if (deep && session_->sampleType() == SampleType::F32)
        statusBar()->showMessage(tr("Exported %1 at 16 bits per channel, tone-mapped from 32 bits at exposure 0 (values above white are clipped).").arg(QFileInfo(path).fileName()), 8000);
    else if (!deep) noteDitheredExport(path);
}

void MainWindow::exportTga() {
    session_->endTemporaryLayers();   // the Quick Mask and filter-mask layers are never written
    if (!session_->hasDocument()) return;
    auto flattened = session_->flattened();
    if (!flattened) return;
    QString path = askExportPath(tr("Export TGA"), tr("TGA image (*.tga)"), {"tga"});
    if (path.isEmpty()) return;
    std::string error;
    if (!writeTgaImage(path.toStdString(), *flattened, &error)) showError(tr("Couldn’t export TGA"), QString::fromStdString(error));
    else noteDitheredExport(path);
}

void MainWindow::exportIco() {
    session_->endTemporaryLayers();   // the Quick Mask and filter-mask layers are never written
    if (!session_->hasDocument()) return;
    auto flattened = session_->flattened();
    if (!flattened) return;
    QString path = askExportPath(tr("Export Icon"), tr("Windows icon (*.ico)"), {"ico"});
    if (path.isEmpty()) return;
    std::string error;
    if (!writeIco(path.toStdString(), *flattened, defaultIcoSizes, &error, &*session_->document())) showError(tr("Couldn’t export the icon"), QString::fromStdString(error));
    else noteDitheredExport(path);
}

void MainWindow::exportGif() {
    session_->endTemporaryLayers();   // the Quick Mask and filter-mask layers are never written
    session_->endFramePreview();
    if (!session_->hasDocument()) return;
    // GIF has no colour profile: converted to sRGB unless the person says otherwise.
    const auto convert = color::askConvertToSrgb(this, *session_->document(), true);
    if (!convert) return;
    QString path = askExportPath(tr("Export Animated GIF"), tr("GIF image (*.gif)"), {"gif"});
    if (path.isEmpty()) return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    std::string error;
    std::optional<Document> converted;
    if (*convert && color::hasNonSrgbProfile(*session_->document())) {
        converted = *session_->document();
        convertDocumentProfile(*converted, {}, {});
    }
    const bool ok = compositor::writeDocumentGif(path.toStdString(), converted ? *converted : *session_->document(), &error);
    QApplication::restoreOverrideCursor();
    if (!ok) showError(tr("Couldn’t export the GIF"), QString::fromStdString(error));
    else if (session_->sampleType() != SampleType::U8) noteDitheredExport(path);
    else statusBar()->showMessage(tr("Exported %n frame(s) to %1", nullptr, std::max(1, int(session_->document()->animation.frames.size()))).arg(QFileInfo(path).fileName()), 6000);
}

void MainWindow::showBatchDialog(const QString& action) {
    // File > Automate > Batch: an action over a folder of files, each exported to another folder.
    if (ActionLibrary::instance().actions().empty()) { showError(tr("Batch"), tr("Record an action first (Window > Actions).")); return; }
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Batch"));
    auto* form = new QFormLayout(&dialog);
    auto* actions = new QComboBox;
    for (const RecordedAction& a : ActionLibrary::instance().actions()) actions->addItem(a.name);
    if (!action.isEmpty()) actions->setCurrentText(action);
    form->addRow(tr("Action"), actions);
    auto folderRow = [&](const QString& key) {
        auto* row = new QHBoxLayout;
        auto* edit = new QLineEdit(QSettings().value(key).toString());
        auto* browse = new QPushButton(tr("Choose…"));
        row->addWidget(edit, 1);
        row->addWidget(browse);
        connect(browse, &QPushButton::clicked, &dialog, [this, edit] {
            const QString dir = QFileDialog::getExistingDirectory(this, tr("Choose a Folder"), edit->text());
            if (!dir.isEmpty()) edit->setText(dir);
        });
        return std::make_pair(row, edit);
    };
    auto [sourceRow, source] = folderRow("batch/source");
    auto [destinationRow, destination] = folderRow("batch/destination");
    form->addRow(tr("Source folder"), sourceRow);
    form->addRow(tr("Destination"), destinationRow);
    auto* format = new QComboBox;
    format->addItems({"png", "jpg", "webp", "tif", "psd", "gif", "tga"});
    format->setCurrentText(QSettings().value("batch/format", "png").toString());
    form->addRow(tr("Save as"), format);
    auto* overwrite = new QCheckBox(tr("Replace files already in the destination"));
    form->addRow(QString(), overwrite);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    if (dialog.exec() != QDialog::Accepted) return;
    QSettings().setValue("batch/source", source->text());
    QSettings().setValue("batch/destination", destination->text());
    QSettings().setValue("batch/format", format->currentText());
    QProgressDialog progress(tr("Processing…"), tr("Stop"), 0, 100, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(300);
    bool stopped = false;
    QJsonObject result;
    try {
        result = automationEngine()->runBatch(actions->currentText(), source->text(), destination->text(), format->currentText(), overwrite->isChecked(),
            [&](int done, int total, const QString& file) {
                progress.setMaximum(std::max(1, total));
                progress.setValue(done);
                progress.setLabelText(file.isEmpty() ? tr("Done") : tr("%1 of %2: %3").arg(done + 1).arg(total).arg(QFileInfo(file).fileName()));
                QApplication::processEvents();
                stopped = stopped || progress.wasCanceled();
                return !stopped;
            });
    } catch (const std::exception& e) {
        showError(tr("Batch"), QString::fromUtf8(e.what()));
        return;
    }
    progress.close();
    QStringList lines{tr("%n file(s) written to %1.", nullptr, int(result.value("written").toArray().size())).arg(result.value("output").toString())};
    for (const QJsonValue& v : result.value("failed").toArray())
        lines << tr("%1: %2 failed: %3").arg(v.toObject().value("file").toString(), v.toObject().value("stage").toString(), v.toObject().value("message").toString());
    if (!result.value("skipped").toArray().isEmpty()) lines << tr("%n skipped (already there, or stopped).", nullptr, int(result.value("skipped").toArray().size()));
    QMessageBox::information(this, tr("Batch"), lines.join("\n"));
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasUrls() || e->mimeData()->hasImage()) e->acceptProposedAction();
}

void MainWindow::dragMoveEvent(QDragMoveEvent* e) {
    // A file over the tab strip will open as its own document; over the canvas it joins this one.
    if (e->mimeData()->hasUrls() && session_->hasDocument())
        statusBar()->showMessage(overTabStrip(e->position()) ? tr("Drop to open as a new document") : tr("Drop to add as a layer (drop on the tab strip to open as a new document)"));
    e->acceptProposedAction();
}

void MainWindow::dragLeaveEvent(QDragLeaveEvent*) { statusBar()->clearMessage(); }

void MainWindow::dropEvent(QDropEvent* e) {
    statusBar()->clearMessage();
    QPointF canvasPoint = canvas_->mapFrom(this, e->position().toPoint());
    std::optional<QPointF> at;
    if (session_->hasDocument() && canvas_->rect().contains(canvasPoint.toPoint())) at = canvas_->documentPoint(canvasPoint);
    const bool asDocument = session_->hasDocument() && overTabStrip(e->position());
    if (e->mimeData()->hasUrls()) {
        for (auto& url : e->mimeData()->urls()) {
            if (!url.isLocalFile()) continue;
            QString path = url.toLocalFile();
            // Projects and layered files open in their own tab; an image dropped on the tab strip does too,
            // while one dropped on the canvas lands as a layer where it was dropped.
            if (isProjectPath(path) || isLayeredPath(path)) openPath(path);
            else if (asDocument) { QString error; if (!openImageAsDocument(path, &error)) showError(tr("Couldn’t open %1").arg(QFileInfo(path).fileName()), error); }
            else importFile(path, at);
        }
        e->acceptProposedAction();
        return;
    }
    if (e->mimeData()->hasImage()) {
        QImage image = qvariant_cast<QImage>(e->mimeData()->imageData());
        if (!image.isNull()) session_->insertImage(fromQImage(image), tr("Dropped Image"), at);
        e->acceptProposedAction();
    }
}


void MainWindow::exportBoxes(bool slices) {
    session_->endTemporaryLayers();   // the Quick Mask and filter-mask layers are never written
    if (!session_->hasDocument()) return;
    const bool none = slices ? session_->document()->slices.empty() : session_->artboards().empty();
    if (none) { showError(slices ? tr("Export Slices") : tr("Export Artboards"), slices ? tr("The document has no slices; draw some with the Slice tool (Shift+C).") : tr("The document has no artboards; draw one with the Artboard tool (Shift+V).")); return; }
    QDialog dialog(this);
    dialog.setWindowTitle(slices ? tr("Export Slices") : tr("Artboards to Files"));
    auto* form = new QFormLayout(&dialog);
    auto* format = new QComboBox;
    format->addItems({"PNG", "JPEG"});
    auto* prefix = new QLineEdit(session_->projectPath().isEmpty() ? QString() : QFileInfo(session_->projectPath()).completeBaseName() + "_");
    auto* quality = new QSpinBox;
    quality->setRange(1, 100);
    quality->setValue(90);
    form->addRow(tr("Format"), format);
    form->addRow(tr("File name prefix"), prefix);
    form->addRow(tr("JPEG quality"), quality);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    if (dialog.exec() != QDialog::Accepted) return;
    const QString directory = QFileDialog::getExistingDirectory(this, tr("Export To"), QSettings().value("lastDir").toString());
    if (directory.isEmpty()) return;
    QSettings().setValue("lastDir", directory);
    QString error;
    const QString fmt = format->currentIndex() == 0 ? QStringLiteral("png") : QStringLiteral("jpeg");
    const QStringList written = slices ? session_->exportSlices(directory, fmt, prefix->text(), quality->value(), &error)
                                       : session_->exportArtboards(directory, fmt, prefix->text(), quality->value(), &error);
    if (!error.isEmpty()) showError(tr("Couldn’t export"), error);
    else if (session_->sampleType() != SampleType::U8 && fmt != "png")
        statusBar()->showMessage(tr("Wrote %n file(s) to %1, reduced from 16 to 8 bits per channel with dithering.", "", int(written.size())).arg(directory), 8000);
    else statusBar()->showMessage(tr("Wrote %n file(s) to %1", "", int(written.size())).arg(directory), 8000);
}

} // namespace app
