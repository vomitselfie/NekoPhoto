#include "MainWindow.h"
#include "ImageConvert.h"
#include "ModelStore.h"
#include "PreferencesDialog.h"
#include "Automation.h"
#include "Dialogs.h"
#include "Theme.h"
#include "FilterDialog.h"
#include "ImageConvert.h"
#include <QDialog>
#include <cstdio>
#include <cstring>
#include "compositor/filters.h"
#include "compositor/selection.h"
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
#include <QTextStream>
#include <QTimer>

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
    session.shapeKind = ShapeKind::Rectangle;
    session.shapeCornerRadius = 24;
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
    session.shapeKind = ShapeKind::Ellipse;
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
            if (auto mask = subjectMask(*source, path.toStdString(), &error)) session.applySubjectMask(refineMatte(*mask, *source, MatteSettings{}, 0));
            else qWarning("Remove Background: %s", error.c_str());
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    // --headless: no window on screen; the automation socket is the only way in. Must be decided before QApplication.
    // --call and --batch never show a window either, so they must work without a display.
    bool headless = false;
    for (int i = 1; i < argc; i++) if (std::strcmp(argv[i], "--headless") == 0 || std::strcmp(argv[i], "--call") == 0 || std::strcmp(argv[i], "--batch") == 0) headless = true;
    if (headless && !qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QApplication::setOrganizationName("compositor-linux");
    QApplication::setApplicationName("compositor-linux");
    QApplication::setApplicationVersion(QStringLiteral(COMPOSITOR_VERSION));
    // The desktop entry gives Wayland the app id and icon; naming it when it isn't installed only makes the portal complain.
    if (!QStandardPaths::locate(QStandardPaths::ApplicationsLocation, "compositor-linux.desktop").isEmpty()) QApplication::setDesktopFileName("compositor-linux");
    app.setWindowIcon(QIcon(QStringLiteral(":/app/icon.svg")));
    app::applyTheme();
    QCommandLineParser parser;
    parser.setApplicationDescription("compositor-linux: a small, focused image compositor.");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("file", "A .comp project or an image to open.");
    QCommandLineOption demo("demo", "Build a layered demo document (optionally from the given image).");
    QCommandLineOption screenshot("screenshot", "Grab the window to <file> after opening, then quit.", "file");
    QCommandLineOption saveAs("save-as", "Save the document as the .comp package <path> before quitting (with --screenshot).", "path");
    QCommandLineOption prefs("preferences", "Open the Preferences dialog too (with --screenshot, grab it instead of the window).");
    QCommandLineOption fetch("download-model", "Download model <id> (isnet or u2netp) into the models folder, report, and quit.", "id");
    parser.addOption(demo);
    parser.addOption(screenshot);
    parser.addOption(saveAs);
    parser.addOption(prefs);
    QCommandLineOption toolOption("tool", "Select tool <name> after opening (move, marquee, lasso, wand, crop, brush, healing, clone, smudge, gradient, shape, eyedropper, hand, zoom).", "name");
    parser.addOption(toolOption);
    QCommandLineOption dialogOption("dialog", "Open dialog <name> after opening, for screenshots: new, canvas-size, image-size, jpeg, levels, curves, hue, exposure, gradient-map, grain, blur, motion-blur, noise, lens.", "name");
    parser.addOption(dialogOption);
    QCommandLineOption rpc("rpc", "Listen on the automation socket (JSON-RPC over a local socket, for the MCP bridge). Also on when the automation preference is set.");
    QCommandLineOption rpcSocket("rpc-socket", "Socket path for --rpc (default: $XDG_RUNTIME_DIR/compositor-linux.sock, or $COMPOSITOR_RPC_SOCKET).", "path");
    QCommandLineOption headlessOption("headless", "Run without a visible window (offscreen) with the automation socket on; implies --rpc.");
    parser.addOption(rpc);
    parser.addOption(rpcSocket);
    parser.addOption(headlessOption);
    QCommandLineOption callOption("call", "Send one request to a running instance's socket and print the result: --call layers.list [--params '{...}']. Exit 1 on an error reply, 2 when nothing is listening.", "method");
    QCommandLineOption paramsOption("params", "JSON object of parameters for --call.", "json");
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
        socket.connectToServer(path);
        if (!socket.waitForConnected(3000)) { std::fprintf(stderr, "nothing is listening at %s (start compositor-linux --rpc, or --headless)\n", qPrintable(path)); return 2; }
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
    app::MainWindow window;
    window.show();
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
            QJsonObject response = server.handle(request);
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
    if (parser.isSet(toolOption)) {
        static const QMap<QString, app::Tool> tools{{"move", app::Tool::Move}, {"marquee", app::Tool::Marquee}, {"lasso", app::Tool::Lasso}, {"wand", app::Tool::Wand},
            {"crop", app::Tool::Crop}, {"brush", app::Tool::Brush}, {"healing", app::Tool::SpotHealing}, {"clone", app::Tool::CloneStamp}, {"smudge", app::Tool::Smudge},
            {"gradient", app::Tool::Gradient}, {"shape", app::Tool::Shape}, {"eyedropper", app::Tool::Eyedropper}, {"hand", app::Tool::Hand}, {"zoom", app::Tool::Zoom}};
        QString name = parser.value(toolOption).toLower();
        if (tools.contains(name)) window.session()->selectTool(tools.value(name)); else qWarning("unknown tool: %s", qPrintable(name));
    }
    if (parser.isSet(dialogOption)) {
        QString name = parser.value(dialogOption).toLower();
        QTimer::singleShot(50, &window, [&window, name] {
            using compositor::AdjustmentKind; using compositor::FilterKind;
            static const QMap<QString, AdjustmentKind> adjustments{{"levels", AdjustmentKind::Levels}, {"curves", AdjustmentKind::Curves}, {"hue", AdjustmentKind::HueSaturation},
                {"exposure", AdjustmentKind::Exposure}, {"gradient-map", AdjustmentKind::GradientMap}, {"grain", AdjustmentKind::Grain}};
            static const QMap<QString, FilterKind> filters{{"blur", FilterKind::GaussianBlur}, {"motion-blur", FilterKind::MotionBlur}, {"noise", FilterKind::AddNoise}, {"lens", FilterKind::LensCorrection}};
            app::EditorSession* s = window.session();
            if (adjustments.contains(name)) (new app::PixelAdjustmentDialog(s, adjustments.value(name), &window))->show();
            else if (filters.contains(name)) (new app::FilterDialog(s, filters.value(name), &window))->show();
            else if (name == "new") app::askNewDocument(&window, {});
            else if (name == "canvas-size") app::askCanvasSize(&window, s->hasDocument() ? s->document()->width : 1920, s->hasDocument() ? s->document()->height : 1080);
            else if (name == "image-size") app::askImageSize(&window, s->hasDocument() ? s->document()->width : 1920, s->hasDocument() ? s->document()->height : 1080, 72);
            else if (name == "jpeg") { auto flat = s->hasDocument() ? s->flattened() : nullptr; if (flat) app::askJpegExport(&window, app::toQImage(*flat)); }
            else qWarning("unknown dialog: %s", qPrintable(name));
        });
    }
    app::PreferencesDialog* preferences = nullptr;
    if (parser.isSet(prefs)) { preferences = new app::PreferencesDialog(&window); preferences->show(); }
    if (parser.isSet(screenshot)) {
        QString target = parser.value(screenshot), savePath = parser.value(saveAs);
        QTimer::singleShot(400, &window, [&window, target, savePath, preferences] {
            QWidget* subject = preferences;
            if (!subject) for (QWidget* w : QApplication::topLevelWidgets()) if (w->isVisible() && qobject_cast<QDialog*>(w)) subject = w;
            (subject ? subject->grab() : window.grab()).save(target);
            if (!savePath.isEmpty()) { QString error; window.session()->saveProject(savePath, &error); if (!error.isEmpty()) qWarning("%s", qPrintable(error)); }
            // exit() rather than quit(): newer Qt closes the windows on quit(), and the unsaved demo would prompt.
            QCoreApplication::exit(0);
        });
    }
    return app.exec();
}
