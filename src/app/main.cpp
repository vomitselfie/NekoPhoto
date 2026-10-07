#include "ContentFillDialog.h"
#include <QButtonGroup>
#include <QAbstractButton>
#include "ContentAwareScaleDialog.h"
#include "MainWindow.h"
#include "CanvasWidget.h"
#include "SelfTest.h"
#include "ChannelDialogs.h"
#include "LayersPanel.h"
#include "WelcomeDialog.h"
#include "Bench.h"
#include "BrushLibrary.h"
#include "ImageConvert.h"
#include "ModelStore.h"
#include "PreferencesDialog.h"
#include "Automation.h"
#include "Dialogs.h"
#include "ExportAsDialog.h"
#include "Theme.h"
#include "CameraRawDialog.h"
#include "RawDevelopDialog.h"
#include "compositor/raw.h"
#include "LayerStyleDialog.h"
#include "FilterDialog.h"
#include "HistogramPanel.h"
#include "GmicDialog.h"
#include "ImportBanner.h"
#include "MoshDialog.h"
#include "BrushDynamicsDialog.h"
#include "BrushPicker.h"
#include "FontPicker.h"
#include "SingleInstance.h"
#include "Platform.h"
#include "AutomationGuard.h"
#include <QElapsedTimer>
#include "CpuPower.h"
#include "Language.h"
#include "Scrub.h"
#include "ImageConvert.h"
#include <QDialog>
#include <cstdio>
#include <cstring>
#include "compositor/filters.h"
#include "compositor/selection.h"
#include "compositor/matte.h"
#include "compositor/subject.h"
#include <QFileInfo>
#include "compositor/warp.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QImageReader>
#include <QIcon>
#include <QMap>
#include <QSettings>
#include <QStandardPaths>
#include <QToolBar>
#include <QDockWidget>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QListWidget>
#include <QTextStream>
#include <QTimer>
#include <QPushButton>
#include <QComboBox>

namespace {

// A layered document for smoke tests and screenshots: an image, a painted layer, a
// masked layer in a folder, a clipped layer and a blend mode, so every render path runs.
void buildDemo(app::EditorSession& session, const QString& imagePath) {
    using namespace compositor;
    QImage base;
    if (!imagePath.isEmpty()) { QImageReader reader(imagePath); reader.setAutoTransform(true); base = reader.read(); }
    if (base.isNull()) {
        base = QImage(640, 420, QImage::Format_RGBA8888_Premultiplied);
        for (int y = 0; y < base.height(); y++) for (int x = 0; x < base.width(); x++) base.setPixelColor(x, y, QColor(40 + x * 180 / base.width(), 60 + y * 150 / base.height(), 120));
    }
    session.insertImage(app::fromQImage(base), "Background");
    session.addGroup();
    session.renameLayer(*session.activeLayerId(), "Folder");
    auto tint = std::make_shared<Image>(base.width() / 2, base.height() / 2);
    tint->fill(220, 40, 40, 255);
    session.insertImage(tint, "Red", QPointF(base.width() * 0.4, base.height() * 0.45));
    {
        auto mask = std::make_shared<GrayImage>(tint->width(), tint->height(), 0);
        for (int y = 0; y < mask->height(); y++) for (int x = 0; x < mask->width(); x++) {
            double d = std::hypot(x - mask->width() / 2.0, y - mask->height() / 2.0);
            mask->at(x, y) = uint8_t(std::clamp((mask->width() / 2.0 - d) * 6, 0.0, 255.0));
        }
        session.addLayerMask(true);
        // Replace the solid mask with the soft circle through the public edit path.
        session.beginEdit("Demo Mask");
        auto& doc = const_cast<Document&>(*session.document());
        doc.find(*session.activeLayerId())->mask->asset = MaskAsset::make(mask);
        session.endEdit();
    }
    session.setLayerBlendMode(BlendMode::Multiply);
    session.setLayerOpacity(0.85);
    auto stripes = std::make_shared<Image>(base.width(), base.height());
    for (int y = 0; y < stripes->height(); y++) for (int x = 0; x < stripes->width(); x++) {
        uint8_t* p = stripes->pixel(x, y);
        bool on = ((x + y) / 24) % 2 == 0;
        p[0] = on ? 255 : 0; p[1] = on ? 255 : 0; p[2] = on ? 255 : 0; p[3] = on ? 255 : 0;
    }
    session.insertImage(stripes, "Stripes", QPointF(base.width() / 2.0, base.height() / 2.0));
    session.toggleClippingMask(*session.activeLayerId());
    session.setLayerOpacity(0.5);
    session.addBlankLayer();
    session.renameLayer(*session.activeLayerId(), "Paint");
    session.brushSettings.diameter = 36;
    session.brushSettings.hardness = 0.4;
    session.foregroundColor = QColor(255, 230, 40);
    session.beginBrush(QPointF(60, 60), false);
    for (int i = 1; i <= 40; i++) session.continueBrush(QPointF(60 + i * 12, 60 + std::sin(i / 4.0) * 40));
    session.endBrush();
    // An adjustment layer over everything, and a blurred copy of the stripes.
    session.selectLayer(session.document()->layers.back().id);
    session.addAdjustmentLayer(AdjustmentKind::HueSaturation);
    {
        auto settings = session.adjustmentSettings(*session.activeLayerId());
        settings->hsv.adjustments[0] = {90, 20, 0};
        session.setAdjustment(*session.activeLayerId(), *settings);
    }
    session.selectLayer(session.document()->layers[3].id); // the stripes
    {
        LayerTransform grown;
        auto source = session.adjustmentSource(int(std::ceil(blurMargin(FilterKind::GaussianBlur, FilterSettings{}))) + 6, grown);
        if (source) {
            auto out = std::make_shared<Image>(*source);
            FilterSettings fs; fs.radius = 3;
            applyFilter(FilterKind::GaussianBlur, *out, fs);
            LayerTransform placed;
            auto trimmed = trimToPixels(*out, grown, placed);
            session.commitPixels(trimmed, placed, "Gaussian Blur");
        }
    }
    // The newer tools: a rounded shape, a gradient on it, a distorted copy, a smudge, and moved pixels.
    session.selectLayer(session.document()->layers.back().id);
    session.shapeTool.kind = app::VectorShapeKind::Rectangle;
    session.shapeTool.cornerRadius = 24;
    session.foregroundColor = QColor(40, 200, 255);
    session.beginShape(QPointF(base.width() * 0.62, base.height() * 0.6));
    session.dragShape(QPointF(base.width() * 0.95, base.height() * 0.92), false, false);
    session.finishShape();
    session.gradientSettings.style = app::GradientStyle::ForegroundToBackground;
    session.backgroundColor = QColor(255, 60, 160);
    session.loadLayerAsSelection(*session.activeLayerId(), false, SelectionMode::Replace);
    session.beginGradient(QPointF(base.width() * 0.62, base.height() * 0.6));
    session.moveGradient(QPointF(base.width() * 0.95, base.height() * 0.92));
    session.commitGradient();
    session.beginTransform(true);
    session.beginDistort();
    if (auto& edit = session.transformEdit()) {
        Corners c = *edit->corners;
        c[0].y += 30; c[1].y -= 30;
        session.previewCorners(c);
    }
    session.commitTransform();
    session.deselect();
    session.shapeTool.kind = app::VectorShapeKind::Ellipse;
    session.foregroundColor = QColor(255, 255, 255);
    session.beginShape(QPointF(base.width() * 0.05, base.height() * 0.55));
    session.dragShape(QPointF(base.width() * 0.25, base.height() * 0.75), true, false);
    session.finishShape();
    session.selectLayer(session.document()->layers[0].id);
    session.blurMode = app::BlurToolMode::Liquify;
    session.brushSettings.diameter = 60;
    session.beginWarp(QPointF(base.width() * 0.2, base.height() * 0.8));
    session.continueWarp(QPointF(base.width() * 0.3, base.height() * 0.7));
    session.continueWarp(QPointF(base.width() * 0.4, base.height() * 0.85));
    session.endWarp();
    auto rect = rasterizeRect({base.width() * 0.05, base.height() * 0.05, base.width() * 0.15, base.height() * 0.15}, base.width(), base.height(), true);
    session.applySelectionShape(*rect, SelectionMode::Replace, "Marquee");
    session.beginPixelMove(true);
    session.movePixels(QPointF(base.width() * 0.75, 0));
    session.finishPixelMove();
    // Merge the painted layer with the stripes below it (Merge Layers), then nudge the selection outline.
    {
        std::set<Uuid> pair;
        for (auto& l : session.document()->layers) if (l.name == "Paint" || l.name == "Stripes") pair.insert(l.id);
        session.selectLayers(pair, std::nullopt);
        if (session.canMergeLayers()) session.mergeLayers();
        session.nudgeSelection(0, 12);
    }
    session.selectLayer(session.document()->layers[1].id);
    // With a model available, Remove Background on the base image, as the menu item would.
    QString modelDir = qEnvironmentVariable("COMPOSITOR_MODEL_DIR");
    if (!modelDir.isEmpty() && subjectModelSupported()) {
        QString path = modelDir + "/isnet-general-use.onnx";
        if (!QFileInfo::exists(path)) path = modelDir + "/u2netp.onnx";
        if (QFileInfo::exists(path)) {
            session.selectLayer(session.document()->layers[0].id);
            LayerTransform t;
            auto source = session.adjustmentSource(0, t);
            std::string error;
            if (auto mask = subjectMask(*source, path.toStdString(), &error, app::ModelStore::mirrorAverage())) {
                auto refined = refineMatte(*mask, *source, MatteSettings{}, 0);
                session.applySubjectMask(refined, estimateForeground(*source, *refined));
            }
            else qWarning("Remove Background: %s", error.c_str());
        }
    }
}

} // namespace

/// Until 0.9.1 the app was compositor-linux: its settings and data (the model, imported brushes, the G'MIC
/// catalogue, recovery files) move to NekoPhoto's folders once, on the first launch after the rename, and only
/// when nothing is there yet.
static void migrateFromOldName() {
    auto move = [](const QString& from, const QString& to) {
        if (!QFileInfo::exists(from) || QFileInfo::exists(to)) return;
        QDir().mkpath(QFileInfo(to).path());
        QDir().rename(from, to);
    };
    const QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    const QString config = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    move(data + "/compositor-linux/compositor-linux", QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
    move(config + "/compositor-linux/compositor-linux.conf", config + "/nekophoto/nekophoto.conf");
    QDir().rmdir(data + "/compositor-linux");   // only if nothing else was in it
    QDir().rmdir(config + "/compositor-linux");
}

namespace app {
/// The demo document, for the self-tests (SelfTest.cpp).
void buildDemoDocument(EditorSession& session) { buildDemo(session, QString()); }
} // namespace app

int run(int argc, char** argv) {
    // --headless: no window on screen; the automation socket is the only way in. Must be decided before QApplication.
    // --call and --batch never show a window either, so they must work without a display.
    bool headless = false;
    for (int i = 1; i < argc; i++) if (std::strcmp(argv[i], "--headless") == 0 || std::strcmp(argv[i], "--call") == 0 || std::strcmp(argv[i], "--batch") == 0) headless = true;
    if (headless && !qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    app::platform::attachParentConsole();   // Windows: --version, --help and --call print to the calling console
    QApplication app(argc, argv);
    QApplication::setOrganizationName("nekophoto");
    QApplication::setApplicationName("nekophoto");
    // --self-test runs under ctest: its settings and files go to a scratch place of their own, never the user's (on
    // Windows XDG variables do nothing and QSettings is the registry, so both are redirected here).
    for (int i = 1; i < argc; i++)
        if (std::strcmp(argv[i], "--self-test") == 0) {
            QStandardPaths::setTestModeEnabled(true);
            const QString scratch = QDir::temp().filePath(QStringLiteral("nekophoto-selftest-%1").arg(QCoreApplication::applicationPid()));
            QDir().mkpath(scratch);
            QSettings::setDefaultFormat(QSettings::IniFormat);
            QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, scratch);
            // Removed when the run ends, however it returns.
            static const struct Scratch { QString path; ~Scratch() { QDir(path).removeRecursively(); } } removeAtExit{scratch};
            break;
        }
    QApplication::setApplicationDisplayName("NekoPhoto");
    migrateFromOldName();
    app::cpupower::apply();   // the pool's size and the priority, before anything runs a parallel loop
    QApplication::setApplicationVersion(QStringLiteral(COMPOSITOR_VERSION));
    // The desktop entry gives Wayland the app id and icon; naming it when it isn't installed only makes the portal complain.
    if (!QStandardPaths::locate(QStandardPaths::ApplicationsLocation, "nekophoto.desktop").isEmpty()) QApplication::setDesktopFileName("nekophoto");
    app.setWindowIcon(QIcon(QStringLiteral(":/app/icon.svg")));
    app::applyTheme();
    app::scrub::install(app);
    {
        // The interface language, before any window: --lang for one run, else Edit > Preferences. A headless or
        // scripted run stays English unless --lang asks (the automation API is English either way).
        QString lang;
        for (int i = 1; i < argc; i++) {
            if (std::strcmp(argv[i], "--lang") == 0 && i + 1 < argc) lang = QString::fromLocal8Bit(argv[i + 1]);
            else if (std::strncmp(argv[i], "--lang=", 7) == 0) lang = QString::fromLocal8Bit(argv[i] + 7);
        }
        if (!lang.isEmpty() || !headless) app::language::install(lang);
    }
    QCommandLineParser parser;
    parser.setApplicationDescription("NekoPhoto: a layered photo editor and painting app.");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("file", "A project (.nekophoto file or .comp folder) or an image to open.");
    QCommandLineOption demo("demo", "Build a layered demo document (optionally from the given image).");
    QCommandLineOption screenshot("screenshot", "Grab the window to <file> after opening, then quit.", "file");
    QCommandLineOption benchBrush("bench-brush", "Developer benchmark: paint strokes with brush preset <id> (or \"round\") through the canvas, print press, move and release latency, then quit.", "id");
    QCommandLineOption benchSize("bench-size", "Document size for --bench-brush, WxH (default 2000x2000).", "size");
    QCommandLineOption benchType("bench-type", "Developer benchmark: type three paragraphs on the canvas with the Type tool, print each keystroke's latency, then quit (with --screenshot, grab the window while typing).");
    QCommandLineOption benchView("bench-view", "Developer benchmark: zoom, pan and undo on a large multi-layer document (--bench-size, default 4096x4096), print the times, then quit.");
    QCommandLineOption benchAdjust("bench-adjust", "Developer benchmark: drag adjustment-layer sliders on a large multi-layer document (--bench-size, --bench-depth, --bench-mode), print each tick's time to repaint, then quit.");
    QCommandLineOption benchDepth("bench-depth", "With --bench-adjust, the document depth: 8, 16 or 32.", "bits");
    QCommandLineOption benchMode("bench-mode", "With --bench-adjust, the colour mode: rgb, cmyk or lab.", "mode");
    QCommandLineOption benchOpaque("bench-opaque", "With --bench-brush, paint on the opaque image layer rather than a blank layer.");
    QCommandLineOption benchBrushSize("bench-brush-size", "With --bench-brush, the brush diameter in document pixels.", "pixels");
    QCommandLineOption benchEraser("bench-eraser", "With --bench-brush, erase instead of painting.");
    QCommandLineOption benchMoves("bench-moves", "With --bench-brush, pointer moves per stroke (default 80).", "count");
    QCommandLineOption benchZoom("bench-zoom", "With --bench-brush, the view zoom (1 = 100%; default: fit).", "zoom");
    QCommandLineOption benchBurst("bench-burst", "With --bench-brush, pointer moves per repaint (default 1; a 1000 Hz mouse gives ~16 per frame).", "count");
    QCommandLineOption benchHardness("bench-hardness", "With --bench-brush, the brush hardness 0..1.", "hardness");
    QCommandLineOption benchSmoothing("bench-smoothing", "With --bench-brush, smoothing at 50%: input, stabilizer, pulled, pressure or all.", "mode");
    QCommandLineOption benchReach("bench-reach", "With --bench-brush, the stroke's half-width as a fraction of the view (default 0.35).", "fraction");
    QCommandLineOption saveAs("save-as", "Save the document as a project at <path> before quitting (with --screenshot): a .nekophoto file, or a .comp folder when <path> ends in .comp.", "path");
    QCommandLineOption prefs("preferences", "Open the Preferences dialog too (with --screenshot, grab it instead of the window).");
    QCommandLineOption fetch("download-model", "Download model <id> (isnet or u2netp) into the models folder, report, and quit.", "id");
    parser.addOption(demo);
    parser.addOption(screenshot);
    parser.addOption(benchBrush);
    parser.addOption(benchSize);
    parser.addOption(benchOpaque);
    parser.addOption(benchBrushSize);
    parser.addOption(benchEraser);
    parser.addOption(benchMoves);
    parser.addOption(benchReach);
    parser.addOption(benchSmoothing);
    parser.addOption(benchZoom);
    parser.addOption(benchBurst);
    parser.addOption(benchHardness);
    parser.addOption(benchView);
    parser.addOption(benchAdjust);
    parser.addOption(benchDepth);
    parser.addOption(benchMode);
    parser.addOption(benchType);
    parser.addOption(saveAs);
    parser.addOption(prefs);
    QCommandLineOption langOption("lang", "Interface language for this run: en, ja or system (default: the Preferences choice).", "code");
    parser.addOption(langOption);
    QCommandLineOption toolOption("tool", "Select tool <name> after opening (move, marquee, lasso, wand, crop, brush, healing, clone, smudge, gradient, shape, eyedropper, hand, zoom).", "name");
    parser.addOption(toolOption);
    QCommandLineOption dialogOption("dialog", "Open dialog <name> after opening, for screenshots: welcome (or welcome:N for page N), new, canvas-size, image-size, export-as (or export-as:<format>: png, jpg, gif, webp, tif, tga), export-layer-as, levels, curves, hue, exposure, gradient-map, grain, blur, motion-blur, noise, lens, cameraraw (or cameraraw:N for panel N), raw:<file> (the Camera Raw dialog a RAW file opens in), gmic, mosh (or mosh:<effect id>), content-fill, background, levels-clip and curves-clip (the clipping display), histogram and histogram-compact (the panel), text, fonts, brushes, brush-dynamics (the first imported tip brush), actions, timeline (frames made from the layers when there are none), batch, layers-menu (the active layer's context menu), search or search:<query> (Edit > Search with the query typed), guides (two ruler guides, and a smart guide as a snap shows it).", "name");
    parser.addOption(dialogOption);
    QCommandLineOption contextMenuOption("context-menu", "Open the canvas's context menu at document point <x,y> after opening (with --tool, for screenshots); x,y,transform or x,y,type first starts a free transform or typing there.", "x,y");
    parser.addOption(contextMenuOption);
    QCommandLineOption selfTestOption("self-test", "Developer check: run in-app test <name> (command-path, guides, canvas-menus, search, held-keys, export-as) on a demo document, print the result and quit with its status.", "name");
    parser.addOption(selfTestOption);
    QCommandLineOption rpc("rpc", "Listen on the automation socket (JSON-RPC over a local socket, for the MCP bridge). Also on when the automation preference is set.");
    QCommandLineOption rpcSocket("rpc-socket", "Socket path for --rpc (default: $XDG_RUNTIME_DIR/nekophoto.sock, or $COMPOSITOR_RPC_SOCKET; on Windows the named pipe nekophoto-<user>).", "path");
    QCommandLineOption headlessOption("headless", "Run without a visible window (offscreen) with the automation socket on; implies --rpc.");
    QCommandLineOption rpcWriteRoot("rpc-write-root", "Let automation requests write files only inside <dir> (repeatable; adds to the automation/writeRoots setting). Without any, they may write anywhere you can.", "dir");
    QCommandLineOption rpcLog("rpc-log", "Append one line per automation request to <file>: time, method, the paths it names, ok or the error, and how long it took (also the automation/log setting).", "file");
    parser.addOption(rpc);
    parser.addOption(rpcSocket);
    parser.addOption(rpcWriteRoot);
    parser.addOption(rpcLog);
    parser.addOption(headlessOption);
    QCommandLineOption callOption("call", "Send one request to a running instance's socket and print the result: --call layers.list [--params '{...}']. Exit 1 on an error reply, 2 when nothing is listening.", "method");
    QCommandLineOption paramsOption("params", "JSON object of parameters for --call.", "json");
    QCommandLineOption newWindow("new-window", "Open in a separate process instead of handing the files to the running editor.");
    parser.addOption(newWindow);
    QCommandLineOption batchOption("batch", "Run JSON-RPC requests from <file> (one object per line; '-' is stdin) in this instance and print one response per line, then quit. Pairs with --headless.", "file");
    parser.addOption(callOption);
    parser.addOption(paramsOption);
    parser.addOption(batchOption);
    parser.addOption(fetch);
    parser.process(app);
    if (parser.isSet(fetch)) {
        const app::ModelInfo* model = app::ModelStore::modelById(parser.value(fetch));
        if (!model) { qWarning("unknown model id"); return 2; }
        int status = 1;
        app::ModelStore::download(*model, &app, [](qint64 r, qint64 t) { std::fprintf(stderr, "\r%lld / %lld", (long long)r, (long long)t); }, [&](QString path, QString error) {
            std::fprintf(stderr, "\n%s\n", error.isEmpty() ? qPrintable("saved " + path) : qPrintable("failed: " + error));
            status = error.isEmpty() && !path.isEmpty() ? 0 : 1;
            QApplication::exit(status);
        });
        app.exec();
        return status;
    }
    if (parser.isSet(callOption)) {
        // A client, not the editor: one request over the socket, the result on stdout.
        QString path = parser.value(rpcSocket).isEmpty() ? app::AutomationServer::defaultSocketPath() : parser.value(rpcSocket);
        QLocalSocket socket;
        socket.connectToServer(app::platform::localServerName(path));
        if (!socket.waitForConnected(3000)) { std::fprintf(stderr, "nothing is listening at %s (start nekophoto --rpc, or --headless)\n", qPrintable(path)); return 2; }
        QJsonObject params;
        if (parser.isSet(paramsOption)) {
            QJsonParseError parseError;
            QJsonDocument doc = QJsonDocument::fromJson(parser.value(paramsOption).toUtf8(), &parseError);
            if (parseError.error != QJsonParseError::NoError || !doc.isObject()) { std::fprintf(stderr, "--params must be a JSON object: %s\n", qPrintable(parseError.errorString())); return 2; }
            params = doc.object();
        }
        QJsonObject request{{"jsonrpc", "2.0"}, {"id", 1}, {"method", parser.value(callOption)}, {"params", params}};
        socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact) + "\n");
        socket.flush();
        QByteArray received;
        while (true) {
            if (!socket.waitForReadyRead(600000)) { std::fprintf(stderr, "no reply\n"); return 2; }
            received += socket.readAll();
            int newline;
            while ((newline = received.indexOf('\n')) >= 0) {
                QByteArray l = received.left(newline).trimmed();
                received.remove(0, newline + 1);
                if (l.isEmpty()) continue;
                QJsonObject reply = QJsonDocument::fromJson(l).object();
                if (reply.value("method").toString() == "event") continue;   // notifications may precede the reply
                if (reply.contains("error")) { std::fprintf(stderr, "error: %s\n", qPrintable(reply["error"].toObject()["message"].toString())); return 1; }
                QJsonValue result = reply.value("result");
                QTextStream(stdout) << (result.isArray() ? QJsonDocument(result.toArray()) : QJsonDocument(result.toObject())).toJson(QJsonDocument::Indented);
                return 0;
            }
        }
    }
    // One editor per user: a launch that carries nothing but file names hands them to the running editor and
    // quits; anything that asks for a process of its own (screenshots, automation, --new-window) keeps one.
    QStringList handoff;
    for (const QString& path : parser.positionalArguments()) handoff << QDir::current().absoluteFilePath(path);
    const bool ownProcess = parser.isSet(newWindow) || parser.isSet(benchBrush) || parser.isSet(benchView) || parser.isSet(benchAdjust) || parser.isSet(benchType) || parser.isSet(screenshot) || parser.isSet(headlessOption) || parser.isSet(batchOption) || parser.isSet(dialogOption) || parser.isSet(saveAs) || parser.isSet(prefs) || parser.isSet(langOption) || parser.isSet(demo) || parser.isSet(toolOption) || parser.isSet(contextMenuOption) || parser.isSet(selfTestOption);
    const QString rpcRequested = parser.isSet(rpc) || parser.isSet(rpcSocket) ? (parser.value(rpcSocket).isEmpty() ? app::AutomationServer::defaultSocketPath() : parser.value(rpcSocket)) : QString();
    if (!ownProcess && app::SingleInstance::handOff(handoff, rpcRequested)) return 0;
    {
        // Where automation may write, and its audit log (SECURITY.md, docs/automation.md).
        QSettings settings;
        app::automation::setWriteRoots(settings.value("automation/writeRoots").toStringList() + parser.values(rpcWriteRoot));
        const QString log = parser.isSet(rpcLog) ? parser.value(rpcLog) : settings.value("automation/log").toString();
        if (!log.isEmpty()) app::automation::setAuditLog(QDir::current().absoluteFilePath(log));
    }
    app::MainWindow window;
    window.show();
    app::SingleInstance instance;
    if (!ownProcess) instance.serve(window);   // best effort; without it the window still runs
    if (parser.isSet(selfTestOption)) return app::runSelfTest(window, parser.value(selfTestOption));
    if (parser.isSet(batchOption)) {
        // Requests from a file, handled in this instance without a socket; responses one per line.
        app::AutomationServer server(&window);
        QFile file(parser.value(batchOption));
        bool ok = parser.value(batchOption) == "-" ? file.open(stdin, QIODevice::ReadOnly) : file.open(QIODevice::ReadOnly);
        if (!ok) { std::fprintf(stderr, "couldn't read %s\n", qPrintable(parser.value(batchOption))); return 2; }
        QTextStream out(stdout);
        int status = 0, id = 0;
        while (!file.atEnd()) {
            QByteArray line = file.readLine().trimmed();
            if (line.isEmpty() || line.startsWith('#')) continue;
            QJsonParseError parseError;
            QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
            if (parseError.error != QJsonParseError::NoError || !doc.isObject()) { std::fprintf(stderr, "bad request line: %s\n", qPrintable(parseError.errorString())); return 2; }
            QJsonObject request = doc.object();
            if (!request.contains("id")) request["id"] = ++id;
            QElapsedTimer timer;
            timer.start();
            QJsonObject response = server.handle(request);
            app::automation::audit(request.value("method").toString(), request.value("params").toObject(), response, timer.elapsed());
            out << QJsonDocument(response).toJson(QJsonDocument::Compact) << "\n";
            out.flush();
            QApplication::processEvents();
            if (response.contains("error")) { status = 1; if (!qEnvironmentVariableIsSet("COMPOSITOR_BATCH_CONTINUE")) break; }
        }
        return status;
    }
    if (qEnvironmentVariableIsSet("COMPOSITOR_DEBUG_LAYOUT")) {
        // What is forcing the window's minimum size: the main window and each toolbar, dock and central child.
        auto report = [](QWidget* w, const char* tag) { QSize m = w->minimumSizeHint(), mm = w->minimumSize(); std::fprintf(stderr, "layout: %-28s %-22s hint %dx%d min %dx%d size %dx%d\n", tag, qPrintable(w->objectName().isEmpty() ? w->windowTitle() : w->objectName()), m.width(), m.height(), mm.width(), mm.height(), w->width(), w->height()); };
        report(&window, "window");
        for (QToolBar* t : window.findChildren<QToolBar*>()) report(t, "toolbar");
        for (QDockWidget* d : window.findChildren<QDockWidget*>()) { report(d, "dock"); if (d->widget()) report(d->widget(), "  dock widget"); }
        if (window.centralWidget()) report(window.centralWidget(), "central");
        for (QWidget* c : window.centralWidget()->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly)) report(c, "  central child");
    }
    if (parser.isSet(rpc) || parser.isSet(headlessOption) || QSettings().value("automation/enabled", false).toBool()) {
        if (!window.startAutomation(parser.value(rpcSocket)) && parser.isSet(headlessOption)) return 3;
    }
    QStringList files = parser.positionalArguments();
    if (parser.isSet(demo)) buildDemo(*window.session(), files.isEmpty() ? QString() : QDir::current().absoluteFilePath(files.first()));
    else for (const QString& path : files) window.openPath(QDir::current().absoluteFilePath(path));
    // Crash recovery in an ordinary launch only; screenshots, demos, batches and headless runs leave nothing behind.
    // The introduction, once: on the first ordinary launch that opens no file.
    if (!ownProcess && files.isEmpty() && !app::WelcomeDialog::shown())
        QTimer::singleShot(250, &window, [&window] { window.showWelcome(); });
    if (!ownProcess || (parser.isSet(newWindow) && !parser.isSet(demo))) {
        window.enableAutosave();
        // libmypaint's one-time setup, done while the window settles instead of in the first stroke's press.
        QTimer::singleShot(300, &window, [] { app::warmBrushEngines(); });
    }
    if (parser.isSet(toolOption)) {
        static const QMap<QString, app::Tool> tools{{"move", app::Tool::Move}, {"marquee", app::Tool::Marquee}, {"lasso", app::Tool::Lasso}, {"wand", app::Tool::Wand}, {"scribble", app::Tool::Scribble},
            {"crop", app::Tool::Crop}, {"brush", app::Tool::Brush}, {"healing", app::Tool::SpotHealing}, {"clone", app::Tool::CloneStamp}, {"smudge", app::Tool::Smudge}, {"dodge", app::Tool::Dodge}, {"bucket", app::Tool::PaintBucket}, {"pen", app::Tool::Pen}, {"directselect", app::Tool::DirectSelect},
            {"gradient", app::Tool::Gradient}, {"shape", app::Tool::Shape}, {"eyedropper", app::Tool::Eyedropper}, {"hand", app::Tool::Hand}, {"zoom", app::Tool::Zoom}, {"text", app::Tool::Text}, {"artboard", app::Tool::Artboard}, {"slice", app::Tool::Slice}};
        QString name = parser.value(toolOption).toLower();
        if (tools.contains(name)) window.session()->selectTool(tools.value(name)); else qWarning("unknown tool: %s", qPrintable(name));
    }
    if (parser.isSet(dialogOption)) {
        QString name = parser.value(dialogOption).toLower();
        QTimer::singleShot(50, &window, [&window, name, given = parser.value(dialogOption)] {
            using compositor::AdjustmentKind; using compositor::FilterKind;
            static const QMap<QString, AdjustmentKind> adjustments{{"levels", AdjustmentKind::Levels}, {"curves", AdjustmentKind::Curves}, {"hue", AdjustmentKind::HueSaturation},
                {"exposure", AdjustmentKind::Exposure}, {"gradient-map", AdjustmentKind::GradientMap}, {"grain", AdjustmentKind::Grain},
                {"brightness-contrast", AdjustmentKind::BrightnessContrast}, {"posterize", AdjustmentKind::Posterize}, {"threshold", AdjustmentKind::Threshold},
                {"black-white", AdjustmentKind::BlackWhite}, {"color-balance", AdjustmentKind::ColorBalance}, {"vibrance", AdjustmentKind::Vibrance},
                {"photo-filter", AdjustmentKind::PhotoFilter}, {"channel-mixer", AdjustmentKind::ChannelMixer}, {"selective-color", AdjustmentKind::SelectiveColor}};
            static const QMap<QString, FilterKind> filters{{"blur", FilterKind::GaussianBlur}, {"motion-blur", FilterKind::MotionBlur}, {"noise", FilterKind::AddNoise}, {"lens", FilterKind::LensCorrection}};
            app::EditorSession* s = window.session();
            if (auto* banner = window.findChild<app::ImportBanner*>()) banner->hide();   // dialog shots show the dialog, not an open's notes
            // Any filter by its name in lower case with dashes ("unsharp-mask", "twirl", "dust-scratches").
            std::optional<FilterKind> filterNamed;
            for (int i = 0; i < compositor::filterKindCount; i++) {
                QString n = QString::fromUtf8(compositor::filterKindName(FilterKind(i))).toLower().remove('&');
                n = n.simplified().replace(' ', '-');
                if (n == name) filterNamed = FilterKind(i);
            }
            if (adjustments.contains(name)) (new app::PixelAdjustmentDialog(s, adjustments.value(name), &window))->show();
            else if (filterNamed && !filters.contains(name)) (new app::FilterDialog(s, *filterNamed, &window))->show();
            else if (name == "levels-clip" || name == "curves-clip") {
                // The clipping display as Alt on the white point (Levels, Input white at 170) or the black point
                // (Curves, moved in to 60) shows it.
                const bool levels = name == "levels-clip";
                for (const auto& l : s->document()->layers) if (l.name == "Background") s->selectLayer(l.id);   // pixels to clip
                auto* dialog = new app::PixelAdjustmentDialog(s, levels ? AdjustmentKind::Levels : AdjustmentKind::Curves, &window);
                dialog->show();
                if (auto* editor = dialog->findChild<app::AdjustmentEditor*>()) {
                    compositor::AdjustmentSettings settings = editor->settings();
                    if (levels) settings.levels.ranges[0].white = 170;
                    else settings.curves.channels[0][0].x = 60;
                    editor->setSettings(settings);
                    editor->showClipping(levels ? 2 : 1);
                }
                dialog->hide();   // the window, with the canvas, is what the screenshot grabs
            }
            else if (name == "histogram" || name == "histogram-compact") {
                window.showPanel("histogram");
                if (auto* panel = window.findChild<app::HistogramPanel*>()) panel->setExpanded(name == "histogram");
            }
            else if (filters.contains(name)) (new app::FilterDialog(s, filters.value(name), &window))->show();
            else if (name == "content-aware-scale") {
                for (const auto& l : s->document()->layers) if (l.name == "Background") s->selectLayer(l.id);   // an image layer to scale
                (new app::ContentAwareScaleDialog(s, &window))->show();
            }
            else if (name == "gmic") (new app::GmicDialog(s, &window))->show();
            else if (name == "32bit") {
                // The document at 32 bits, its view a stop brighter: the status bar's exposure and the greyed tools.
                s->convertMode(compositor::SampleType::F32);
                compositor::View32 view;
                view.exposure = 1;
                s->setView32(view);
            }
            else if (name == "mosh" || name.startsWith("mosh:")) {
                // mosh, or mosh:<effect id> (pixel-sort, vhs, ...): the Mosh dialog on the demo's background
                const QString id = name.section(':', 1).isEmpty() ? QStringLiteral("vhs") : name.section(':', 1);
                if (const auto* spec = compositor::mosh::findEffect(id.toStdString())) {
                    for (const auto& l : s->document()->layers) if (l.name == "Background") s->selectLayer(l.id);
                    (new app::MoshDialog(s, *spec, &window))->show();
                } else qWarning("unknown Mosh effect: %s", qPrintable(id));
            }
            else if (name == "content-fill") {
                // A selection in the middle of the canvas, then the dialog with Wide Area sampling.
                compositor::GrayImage shape(s->document()->width, s->document()->height);
                for (int y = shape.height() * 2 / 5; y < shape.height() * 3 / 5; y++) for (int x = shape.width() * 2 / 5; x < shape.width() * 3 / 5; x++) shape.at(x, y) = 255;
                s->applySelectionShape(shape, compositor::SelectionMode::Replace, "Select");
                auto* dialog = new app::ContentFillDialog(s, &window);
                if (auto* group = dialog->findChild<QButtonGroup*>()) { group->button(1)->click(); }
                dialog->show();
            }
            else if (name == "cameraraw" || name.startsWith("cameraraw:")) {
                // cameraraw, or cameraraw:N to open on panel N (0 Basic ... 7 Calibration)
                auto* dialog = new app::CameraRawDialog(s, &window);
                if (auto* panels = dialog->findChild<QListWidget*>("cameraRawPanels")) panels->setCurrentRow(name.section(':', 1).toInt());
                dialog->show();
            }
            else if (name.startsWith("raw:")) {
                // raw:<file>: the Camera Raw dialog a RAW file opens in (raw:<file>:N for panel N)
                QString file = given.section(QLatin1Char(':'), 1), panel;   // the path as given, not lowercased
                if (const int colon = file.lastIndexOf(':'); colon > 0 && file.mid(colon + 1).toInt() > 0) { panel = file.mid(colon + 1); file.truncate(colon); }
                std::string why;
                auto bytes = std::make_shared<const std::vector<uint8_t>>(compositor::readRawFileBytes(file.toStdString(), &why));
                if (bytes->empty()) qWarning("%s", why.c_str());
                else {
                    auto* dialog = new app::RawDevelopDialog(bytes, QFileInfo(file).fileName(), {}, app::RawDevelopDialog::Purpose::Open, &window);
                    if (auto* panels = dialog->findChild<QListWidget*>("cameraRawPanels")) panels->setCurrentRow(panel.toInt());
                    dialog->show();
                }
            }
            else if (name == "warpcage") {
                // The cage on the demo's ellipse with its bottom-right corner pulled out.
                for (const auto& l : s->document()->layers) if (QString::fromStdString(l.name) == QCoreApplication::translate("Names", "Ellipse") + " 1") s->selectLayer(l.id);
                if (s->beginWarpCage() && s->warpCage()) {
                    compositor::WarpMesh cage = *s->warpCage();
                    cage.xs[15] += 60; cage.ys[15] += 40; cage.xs[5] -= 30;
                    s->setWarpCage(cage);
                }
            }
            else if (name.startsWith("layerstyle")) {
                // layerstyle, or layerstyle:N to open on effect N (1 Bevel & Emboss ... 10 Drop Shadow) switched on
                const int page = name.section(':', 1).toInt();
                if (s->activeLayerId()) {
                    auto* dialog = new app::LayerStyleDialog(s, *s->activeLayerId(), &window, page);
                    if (auto* list = dialog->findChild<QListWidget*>(); list && page > 0 && list->item(page)) list->item(page)->setCheckState(Qt::Checked);
                    dialog->show();
                }
            }
            else if (name.startsWith("welcome")) {
                // welcome, or welcome:N for page N
                window.showWelcome();
                const int page = name.section(':', 1).toInt();
                for (auto* w : window.findChildren<app::WelcomeDialog*>()) for (int i = 0; i < page; i++) for (auto* b : w->findChildren<QPushButton*>()) if (b->text() == QObject::tr("Next")) { b->click(); break; }
            }
            else if (name == "text" || name == "text:styled" || name == "fonts") {
                compositor::LayerText text = s->textStyle;
                text.text = "Hello";
                if (name == "text:styled") {
                    // Letters in several styles, to show the editor's runs and the Character section's mixed values.
                    text.text = "Hello styled\nworld";
                    compositor::TextRunPatch red; red.color = std::array<double, 3>{0.85, 0.1, 0.1}; red.fontSize = text.fontSize * 1.5; red.weight = 800;
                    compositor::styleTextRange(text, 6, 6, red);
                    compositor::TextRunPatch under; under.underline = true; under.italic = true; under.caps = compositor::TextRun::Caps::Small;
                    compositor::styleTextRange(text, 13, 5, under);
                }
                if (s->hasDocument()) s->addTextLayer(QPointF(s->document()->width / 3.0, s->document()->height / 3.0), text, true);
                if (name == "fonts") QTimer::singleShot(100, &window, [] { for (QWidget* w : QApplication::topLevelWidgets()) for (auto* picker : w->findChildren<app::FontPicker*>()) if (w->isVisible()) { picker->showPicker(); return; } });
            }
            else if (name == "brushes") {
                s->selectTool(app::Tool::Brush);
                QTimer::singleShot(100, &window, [] { for (QWidget* w : QApplication::topLevelWidgets()) for (auto* picker : w->findChildren<app::BrushPicker*>()) if (picker->isVisible()) { picker->showPicker(); return; } });
            }
            else if (name == "brush-dynamics") {
                // The first imported tip brush's dynamics.
                for (const app::BrushPreset& preset : app::BrushLibrary::presets())
                    if (preset.engine == app::BrushPreset::Engine::Tip) {
                        s->selectTool(app::Tool::Brush);
                        s->brushPreset = preset.id;
                        emit s->toolChanged();
                        QTimer::singleShot(100, &window, [&window, s, id = preset.id] { app::BrushDynamicsDialog::edit(&window, id, &s->brushSmoothing); });
                        break;
                    }
            }
            else if (name == "background") {
                if (!app::ModelStore::ready()) qWarning("Remove Background is off or its model is missing");
                else {
                    const app::ModelInfo* quick = app::ModelStore::modelById("pphumanseg");
                    QString quickPath = quick && app::ModelStore::isPresent(*quick) ? app::ModelStore::pathFor(*quick) : QString();
                    auto* dialog = new app::BackgroundDialog(s, app::ModelStore::pathFor(app::ModelStore::selected()), quickPath, &window);
                    if (auto* quality = dialog->findChild<QComboBox*>()) quality->setCurrentIndex(1);   // the Advanced panel
                    dialog->show();
                }
            }
            else if (name == "actions" || name == "timeline" || name == "batch") window.showPanel(name);
            else if (name == "channels" || name == "save-selection" || name == "load-selection" || name == "channel-options") {
                // The Channels panel with two alpha channels saved from selections, the second being painted; or one of
                // its dialogs over it.
                window.showPanel("channels");
                if (s->hasDocument()) {
                    const int w = s->document()->width, h = s->document()->height;
                    s->applySelectionShape(*compositor::rasterizeEllipse(compositor::Rect(w * 0.2, h * 0.2, w * 0.45, h * 0.55), w, h, true), compositor::SelectionMode::Replace, "Select");
                    s->saveSelectionToChannel(std::nullopt, QString(), compositor::SelectionMode::Replace);
                    s->applySelectionShape(*compositor::rasterizeRect(compositor::Rect(w * 0.5, h * 0.1, w * 0.4, h * 0.4), w, h, true), compositor::SelectionMode::Replace, "Select");
                    const auto second = s->saveSelectionToChannel(std::nullopt, QString(), compositor::SelectionMode::Replace);
                    if (name == "channels" && second) s->selectAlphaChannel(*second);
                    if (name == "save-selection") (new app::SaveSelectionDialog(s, &window))->show();
                    else if (name == "load-selection") (new app::LoadSelectionDialog(s, &window))->show();
                    else if (name == "channel-options" && second) (new app::ChannelOptionsDialog(s, *second, &window))->show();
                }
            }
            else if (name == "guides") {
                if (s->hasDocument()) {
                    s->addGuide(compositor::Guide{compositor::Guide::Orientation::Vertical, std::round(s->document()->width / 3.0)});
                    s->addGuide(compositor::Guide{compositor::Guide::Orientation::Horizontal, std::round(s->document()->height / 2.0)});
                    s->setSnapGuides({s->document()->width * 0.4}, {});
                }
            }
            else if (name == "search" || name.startsWith("search:")) window.showCommandPalette(given.section(':', 1));
            else if (name == "layers-menu") { if (auto* panel = window.findChild<app::LayersPanel*>()) panel->showActiveLayerMenu(); }
            else if (name == "new") app::askNewDocument(&window, {});
            else if (name == "canvas-size") app::askCanvasSize(&window, s->hasDocument() ? s->document()->width : 1920, s->hasDocument() ? s->document()->height : 1080);
            else if (name == "image-size") app::askImageSize(&window, s->hasDocument() ? s->document()->width : 1920, s->hasDocument() ? s->document()->height : 1080, 72);
            else if (name == "export-as" || name.startsWith("export-as:") || name == "jpeg" || name == "export-layer-as") {
                // File > Export > Export As (export-as:jpg starts on a format; jpeg is the old name for that), or
                // Layer > Export As on the active layer.
                const QString format = name == "jpeg" ? QStringLiteral("jpg") : given.section(':', 1).toLower();
                auto* dialog = new app::ExportAsDialog(s, name == "export-layer-as", format, &window);
                dialog->setAttribute(Qt::WA_DeleteOnClose);
                dialog->show();
            }
            else qWarning("unknown dialog: %s", qPrintable(name));
        });
    }
    if (parser.isSet(contextMenuOption)) {
        const QStringList at = parser.value(contextMenuOption).split(',');
        const QPointF point(at.value(0).toDouble(), at.value(1).toDouble());
        const QString state = at.value(2);   // "transform" or "type": the menu of a transform or of typing in progress
        QTimer::singleShot(120, &window, [&window, point, state] {
            auto* canvas = window.canvasAt(window.currentTabIndex());
            if (!canvas) return;
            if (state == QLatin1String("transform")) window.session()->transformCommand();
            if (state == QLatin1String("type")) canvas->startNewType(point, std::nullopt);
            window.showCanvasMenu(canvas->viewPointForTest(point));
        });
    }
    app::PreferencesDialog* preferences = nullptr;
    if (parser.isSet(prefs)) { preferences = new app::PreferencesDialog(&window); preferences->show(); }
    if (parser.isSet(benchType)) return app::runTypeBench(window, parser.value(screenshot));
    if (parser.isSet(benchView)) {
        app::ViewBenchOptions options;
        const QStringList size = parser.value(benchSize).split('x');
        if (size.size() == 2 && size[0].toInt() > 0 && size[1].toInt() > 0) options.document = QSize(size[0].toInt(), size[1].toInt());
        return app::runViewBench(window, options);
    }
    if (parser.isSet(benchAdjust)) {
        app::AdjustBenchOptions options;
        const QStringList size = parser.value(benchSize).split('x');
        if (size.size() == 2 && size[0].toInt() > 0 && size[1].toInt() > 0) options.document = QSize(size[0].toInt(), size[1].toInt());
        if (parser.isSet(benchDepth)) options.bits = parser.value(benchDepth).toInt();
        if (parser.isSet(benchMode)) options.mode = parser.value(benchMode).toLower();
        return app::runAdjustBench(window, options);
    }
    if (parser.isSet(benchBrush)) {
        app::BrushBenchOptions options;
        options.preset = parser.value(benchBrush);
        const QStringList size = parser.value(benchSize).split('x');
        if (size.size() == 2 && size[0].toInt() > 0 && size[1].toInt() > 0) options.document = QSize(size[0].toInt(), size[1].toInt());
        options.paintOnOpaque = parser.isSet(benchOpaque);
        options.brushSize = parser.value(benchBrushSize).toDouble();
        options.eraser = parser.isSet(benchEraser);
        if (parser.value(benchMoves).toInt() > 0) options.moves = parser.value(benchMoves).toInt();
        if (parser.value(benchReach).toDouble() > 0) options.reach = parser.value(benchReach).toDouble();
        if (parser.value(benchZoom).toDouble() > 0) options.zoom = parser.value(benchZoom).toDouble();
        if (parser.value(benchBurst).toInt() > 0) options.burst = parser.value(benchBurst).toInt();
        if (parser.isSet(benchHardness)) options.hardness = parser.value(benchHardness).toDouble();
        options.smoothing = parser.value(benchSmoothing);
        return app::runBrushBench(window, options);
    }
    if (parser.isSet(screenshot)) {
        QString target = parser.value(screenshot), savePath = parser.value(saveAs);
        // COMPOSITOR_SCREENSHOT_DELAY (ms) waits longer, for a preview that runs in the background.
        const int delay = qEnvironmentVariableIsSet("COMPOSITOR_SCREENSHOT_DELAY") ? qEnvironmentVariableIntValue("COMPOSITOR_SCREENSHOT_DELAY") : 400;
        QTimer::singleShot(delay, &window, [&window, target, savePath, preferences] {
            QWidget* subject = preferences;
            if (!subject) for (QWidget* w : QApplication::topLevelWidgets()) if (w->isVisible() && qobject_cast<QDialog*>(w)) subject = w;
            for (QWidget* w : QApplication::topLevelWidgets()) if (w->isVisible() && w->windowType() == Qt::Popup) subject = w;   // a dropped-down picker wins
            (subject ? subject->grab() : window.grab()).save(target);
            // A dialog's shot also saves the window behind it, its preview on the canvas, as <name>.window.<ext>.
            if (subject) { QFileInfo info(target); window.grab().save(info.path() + "/" + info.completeBaseName() + ".window." + info.suffix()); }
            if (!savePath.isEmpty()) { QString error; window.session()->saveProject(savePath, &error); if (!error.isEmpty()) qWarning("%s", qPrintable(error)); }
            // exit() rather than quit(): newer Qt closes the windows on quit(), and the unsaved demo would prompt.
            QCoreApplication::exit(0);
        });
    }
    return app.exec();
}

int main(int argc, char** argv) {
    const int status = run(argc, argv);   // every Qt object is gone by here
    return app::platform::finishProcess(status);
}
