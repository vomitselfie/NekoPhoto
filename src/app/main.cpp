#include "MainWindow.h"
#include "ImageConvert.h"
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
    session.selectLayer(session.document()->layers[1].id);
    session.selectAll();
    session.deselect();
}

} // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName("Compositor");
    QApplication::setApplicationName("Compositor");
    QApplication::setApplicationVersion("1.0.4");
    QApplication::setDesktopFileName("compositor");
    QCommandLineParser parser;
    parser.setApplicationDescription("A small, focused image compositor.");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("file", "A .comp project or an image to open.");
    QCommandLineOption demo("demo", "Build a layered demo document (optionally from the given image).");
    QCommandLineOption screenshot("screenshot", "Grab the window to <file> after opening, then quit.", "file");
    QCommandLineOption saveAs("save-as", "Save the document as the .comp package <path> before quitting (with --screenshot).", "path");
    parser.addOption(demo);
    parser.addOption(screenshot);
    parser.addOption(saveAs);
    parser.process(app);
    app::MainWindow window;
    window.show();
    QStringList files = parser.positionalArguments();
    if (parser.isSet(demo)) buildDemo(*window.session(), files.isEmpty() ? QString() : QDir::current().absoluteFilePath(files.first()));
    else for (const QString& path : files) window.openPath(QDir::current().absoluteFilePath(path));
    if (parser.isSet(screenshot)) {
        QString target = parser.value(screenshot), savePath = parser.value(saveAs);
        QTimer::singleShot(400, &window, [&window, target, savePath] {
            window.grab().save(target);
            if (!savePath.isEmpty()) { QString error; window.session()->saveProject(savePath, &error); if (!error.isEmpty()) qWarning("%s", qPrintable(error)); }
            QApplication::quit();
        });
    }
    return app.exec();
}
