// The file commands through their dialogs (part of command-path) and export-as: File > Export > Export As, Quick
// Export and Layer > Export As.
#include "SelfTestSupport.h"
#include "ActionLibrary.h"
#include "ColorManagement.h"
#include "EditorSession.h"
#include "ExportAsDialog.h"
#include "MainWindow.h"
#include <QApplication>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QImageReader>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>
#include <cmath>
#include <cstdio>
#include <functional>

using namespace nekophoto;

namespace app {

using namespace selftest;

/// File > New, Open, Import File, Save As and Layer > Smart Objects > Edit Contents through their dialogs: each runs its
/// method (document.new, document.open, document.import, document.save, smartObject.editContents), recorded once, and
/// gives what the window's own calls gave before.
int fileCommands(MainWindow& w) {
    int failures = 0;
    auto expect = [&](bool ok, const QString& what) { if (!ok) { std::fprintf(stderr, "file commands: %s\n", qPrintable(what)); failures++; } };
    QTemporaryDir dir;
    const QString png = dir.filePath(QStringLiteral("opened.png"));
    {
        QImage image(40, 30, QImage::Format_ARGB32);
        for (int y = 0; y < 30; y++) for (int x = 0; x < 40; x++) image.setPixel(x, y, qRgba(x * 6, y * 8, 120, 255));
        image.save(png);
    }
    auto pick = [](const QString& path) {
        return [path](QDialog* d) { if (auto* f = qobject_cast<QFileDialog*>(d)) f->selectFile(path); else std::fprintf(stderr, "not a file dialog: %s\n", d->metaObject()->className()); };
    };
    // Qt's own file dialog, whatever the desktop's platform theme offers, so the test can answer it.
    const bool nativeDialogs = !QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    // Untagged images open in the working space without the question, as a person who turned it off sees.
    const color::Settings colours = color::settings();
    {
        color::Settings quiet = colours;
        quiet.askMissing = quiet.askMismatch = false;
        color::setSettings(quiet);
    }
    // A dialog nobody answers (a question the test does not expect) fails the test instead of waiting for ever.
    QTimer watchdog;
    QPointer<QWidget> waiting;
    int waited = 0;
    QObject::connect(&watchdog, &QTimer::timeout, [&] {
        QWidget* modal = QApplication::activeModalWidget();
        if (!modal || modal != waiting) { waiting = modal; waited = 0; return; }
        if (++waited < 50) return;
        std::fprintf(stderr, "file commands: an unexpected %s (\"%s\") was left open\n", modal->metaObject()->className(), qPrintable(modal->windowTitle()));
        failures++;
        if (auto* d = qobject_cast<QDialog*>(modal)) d->reject(); else modal->close();
    });
    watchdog.start(100);
    auto run = [&](const QStringList& path, std::function<void(QDialog*)> answer) {
        QAction* a = menuItem(w, path);
        if (!a || !a->isEnabled()) { expect(false, path.join(" > ") + " is not there or is greyed"); return; }
        if (answer) answerModal(answer);
        a->trigger();
        QApplication::processEvents();
    };
    ActionLibrary::instance().startRecording(QStringLiteral("file-commands self-test"));
    // New: the dialog's size, in a tab of its own (this one holds a document).
    const int tabs = w.tabCount();
    run({"File", "New…"}, [](QDialog* d) { nth<QSpinBox>(d, 0)->setValue(120); nth<QSpinBox>(d, 1)->setValue(80); });
    expect(w.tabCount() == tabs + 1 && w.session()->hasDocument() && w.session()->document()->width == 120 && w.session()->document()->height == 80, "New did not make a 120 x 80 document in a new tab");
    const QString made = describe(*w.session());
    // Open: the image as a document of its own, in another tab.
    run({"File", "Open…"}, pick(png));
    expect(w.tabCount() == tabs + 2 && w.session()->document()->width == 40, "Open did not open the image in a tab of its own");
    const QString opened = describe(*w.session());
    // Import File: into the document on screen, as a layer.
    const size_t layersBefore = w.session()->document()->layers.size();
    run({"File", "Import File…"}, pick(png));
    expect(w.session()->document()->layers.size() == layersBefore + 1, "Import File did not add a layer");
    const QString imported = describe(*w.session());
    // Save As: a project file the dialog names.
    const QString project = dir.filePath(QStringLiteral("saved.nekophoto"));
    run({"File", "Save As…"}, pick(project));
    expect(QFileInfo::exists(project) && w.session()->projectPath() == project, "Save As did not save " + project);
    // Edit Contents: a smart object's contents, in a tab of their own.
    w.session()->convertToSmartObject(nullptr);
    const int beforeContents = w.tabCount();
    run({"Layer", "Smart Objects", "Edit Contents"}, {});
    expect(w.tabCount() == beforeContents + 1 && w.session()->smartObjectParent(), "Edit Contents did not open the contents");
    const QString contents = describe(*w.session());
    ActionLibrary::instance().stopRecording();
    QStringList recorded;
    if (const RecordedAction* r = ActionLibrary::instance().find(QStringLiteral("file-commands self-test")))
        for (const ActionStep& step : r->steps) recorded << step.method;
    ActionLibrary::instance().remove(QStringLiteral("file-commands self-test"));
    // (The smart object was made by a session call, which records nothing.)
    const QStringList wanted{"document.new", "document.open", "document.import", "document.save", "smartObject.editContents"};
    expect(recorded == wanted, "recorded [" + recorded.join(", ") + "], expected [" + wanted.join(", ") + "]");

    // The same through the window's own calls, as before the command path.
    w.newTab();
    w.session()->createDocument(120, 80, 72, true);
    w.session()->adoptProfile(color::newDocumentProfile());
    expect(describe(*w.session()) == made, "New gives another document than before");
    QString error;
    expect(w.openImageAsDocument(png, &error), "could not open the image: " + error);
    expect(describe(*w.session()) == opened, "Open gives another document than before");
    expect(w.importImageFile(png, std::nullopt, &error), "could not import the image: " + error);
    expect(describe(*w.session()) == imported, "Import File gives another document than before");
    w.session()->convertToSmartObject(nullptr);
    expect(w.editSmartObjectContents(&error), "could not open the contents: " + error);
    expect(describe(*w.session()) == contents, "Edit Contents gives other contents than before");
    watchdog.stop();
    color::setSettings(colours);
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs, !nativeDialogs);
    std::printf("file commands: %s\n", failures ? "FAILED" : "ok");
    return failures;
}

/// File > Export > Export As, Quick Export and Layer > Export As: the dialog's estimate is the file's size, a scale
/// gives the file that size, each format's settings come back next time, and a layer exports cropped to its pixels.
int exportAs(MainWindow& w) {
    EditorSession& s = *w.session();
    buildDemoDocument(s);
    int failures = 0;
    auto expect = [&](bool ok, const QString& what) { if (!ok) { std::fprintf(stderr, "%s\n", qPrintable(what)); failures++; } };
    QTemporaryDir dir;
    if (!dir.isValid()) { std::fprintf(stderr, "no temporary folder\n"); return 1; }
    // Held open for the whole test: the settings stay in memory even where the file cannot be written.
    QSettings held;
    held.remove("export");
    // Errors the window would show in a dialog are collected and fail the test instead.
    QString errors;
    w.setErrorSink(&errors);
    const Document& doc = *s.document();
    // Writes what the dialog chose through document.export, as File > Export > Export As does.
    auto write = [&](ExportAsDialog& dialog, const QString& name) -> QString {
        QJsonObject params = dialog.commandParams();
        params["path"] = dir.filePath(name);
        params["overwrite"] = true;
        const auto reply = w.runCommand("document.export", params);
        w.setErrorSink(&errors);   // the request clears it when it ends
        expect(reply.has_value(), "document.export refused " + name);
        return dir.filePath(name);
    };
    {
        ExportAsDialog dialog(&s, false, QStringLiteral("png"), &w);
        expect(dialog.ready() && dialog.sourceSize() == QSize(doc.width, doc.height), "Export As does not start at the document's size");
        dialog.refreshNow();
        const QString png = write(dialog, "full.png");
        expect(dialog.estimateExact() && dialog.estimatedBytes() == QFileInfo(png).size(),
               QString("PNG estimate %1 is not the file's %2 bytes").arg(dialog.estimatedBytes()).arg(QFileInfo(png).size()));
        // JPEG at quality 40 and half the size: the estimate is still the file, and the file is that size.
        dialog.setFormat("jpg");
        if (auto* quality = dialog.findChild<QSlider*>("quality")) quality->setValue(40);
        dialog.setScalePercent(50);
        const int halfWidth = int(std::lround(doc.width * 0.5)), halfHeight = int(std::lround(doc.height * 0.5));
        expect(dialog.outputWidth() == halfWidth && dialog.outputHeight() == halfHeight, "50% does not halve the size");
        dialog.refreshNow();
        const QString jpg = write(dialog, "half.jpg");
        const QSize jpgSize = QImageReader(jpg).size();
        expect(jpgSize == QSize(halfWidth, halfHeight), QString("the JPEG is %1x%2").arg(jpgSize.width()).arg(jpgSize.height()));
        expect(dialog.estimatedBytes() == QFileInfo(jpg).size(), QString("JPEG estimate %1 is not the file's %2 bytes").arg(dialog.estimatedBytes()).arg(QFileInfo(jpg).size()));
        // Nearest neighbour at 200%: every pixel doubled.
        dialog.setFormat("png");
        if (auto* resample = dialog.findChild<QComboBox*>("resample")) resample->setCurrentIndex(resample->findData("nearest"));
        dialog.setScalePercent(200);
        const QString big = write(dialog, "double.png");
        const QImage doubled = QImage(big).convertToFormat(QImage::Format_ARGB32);
        const QImage single = QImage(png).convertToFormat(QImage::Format_ARGB32);
        expect(doubled.size() == single.size() * 2 && doubled.pixel(21, 33) == single.pixel(10, 16), "nearest neighbour at 200% does not double the pixels");
        dialog.setFormat("jpg");
        dialog.rememberSettings();
    }
    {
        // The next Export As opens on JPEG with its quality; PNG kept its own choices; the size is the image's own again.
        ExportAsDialog again(&s, false, {}, &w);
        expect(again.format() == "jpg" && again.settings().quality == 40, QString("remembered %1 at %2").arg(again.format()).arg(again.settings().quality));
        expect(exportas::remembered("jpg").quality == 40 && exportas::remembered("png").resample == exportas::Resample::Bicubic, "the settings were not kept per format");
        expect(again.outputWidth() == doc.width, "the size was remembered; it starts at the image's own");
    }
    {
        // Layer > Export As: the Red layer alone, cropped to its pixels.
        select(s, "Red");
        ExportAsDialog dialog(&s, true, QStringLiteral("png"), &w);
        const Layer* red = s.activeLayer();
        const Rect bounds = red->transform.bounds().integral().intersection(doc.rect());
        expect(dialog.ready() && dialog.sourceSize().width() <= int(bounds.width) && dialog.sourceSize().height() <= int(bounds.height) && dialog.sourceSize().width() < doc.width,
               QString("the layer exports at %1x%2, its bounds are %3x%4").arg(dialog.sourceSize().width()).arg(dialog.sourceSize().height()).arg(bounds.width).arg(bounds.height));
        const QString layerPng = write(dialog, "red.png");
        expect(QImageReader(layerPng).size() == dialog.sourceSize(), "the layer's file is not its visible size");
    }
    {
        // Quick Export: into the assets folder next to the saved document, in Preferences' format, with that format's remembered settings.
        QString error;
        expect(s.saveProject(dir.filePath("Quick.nekophoto"), &error), "couldn't save the project: " + error);
        exportas::setQuickExportFormat("jpg");
        exportas::setQuickExportAsks(false);   // the assets folder; asking would open a Save dialog
        w.setErrorSink(&errors);
        if (QAction* quick = action(w, "export.quick")) quick->trigger();
        const QString quick = dir.filePath("Quick-assets/Quick.jpg");
        expect(QFileInfo::exists(quick) && QImageReader(quick).size() == QSize(doc.width, doc.height), "Quick Export did not write Quick-assets/Quick.jpg at full size");
        exportas::setQuickExportAsks(true);
    }
    w.setErrorSink(nullptr);
    expect(errors.isEmpty(), "errors: " + errors);
    std::printf("export-as: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}

} // namespace app
