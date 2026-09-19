#include "MainWindow.h"
#include "ImageConvert.h"
#include "ModelStore.h"
#include "PreferencesDialog.h"
#include <cstdio>
#include "compositor/filters.h"
#include "compositor/selection.h"
#include "compositor/subject.h"
#include <QFileInfo>
#include "compositor/warp.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QImageReader>
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
    QApplication app(argc, argv);
    QApplication::setOrganizationName("compositor-linux");
    QApplication::setApplicationName("compositor-linux");
    QApplication::setApplicationVersion("1.0.4");
    QApplication::setDesktopFileName("compositor-linux");
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
    app::MainWindow window;
    window.show();
    QStringList files = parser.positionalArguments();
    if (parser.isSet(demo)) buildDemo(*window.session(), files.isEmpty() ? QString() : QDir::current().absoluteFilePath(files.first()));
    else for (const QString& path : files) window.openPath(QDir::current().absoluteFilePath(path));
    app::PreferencesDialog* preferences = nullptr;
    if (parser.isSet(prefs)) { preferences = new app::PreferencesDialog(&window); preferences->show(); }
    if (parser.isSet(screenshot)) {
        QString target = parser.value(screenshot), savePath = parser.value(saveAs);
        QTimer::singleShot(400, &window, [&window, target, savePath, preferences] {
            (preferences ? preferences->grab() : window.grab()).save(target);
            if (!savePath.isEmpty()) { QString error; window.session()->saveProject(savePath, &error); if (!error.isEmpty()) qWarning("%s", qPrintable(error)); }
            QApplication::quit();
        });
    }
    return app.exec();
}
