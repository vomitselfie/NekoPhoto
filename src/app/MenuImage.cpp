// The Image menu: Mode, Adjustments, the canvas and the image's size (MainWindow::buildMenus, MainWindowMenus.h).
#include "MainWindowMenus.h"
#include "nekophoto/trim.h"
#include "CommandPalette.h"
#include <QLabel>
#include <QComboBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QFormLayout>
#include "CanvasFrame.h"
#include "Dialogs.h"
#include "FilterDialog.h"
#include <QActionGroup>
#include <QMenuBar>
#include <QMessageBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QVBoxLayout>
#include <algorithm>

using namespace nekophoto;

namespace app {

using namespace menus;

void MainWindow::buildImageMenu(MenuContext& m) {
    QMenu* image = menuBar()->addMenu(tr("&Image"));
    // Photoshop's Image > Mode: the document's bits per channel (docs/bit-depth.md).
    QMenu* mode = image->addMenu(tr("&Mode"));
    m.reg.adopt(nullptr, mode->menuAction(), Spec("image.mode", plainText(mode->title())).document("document.mode").command);
    // The colour modes (docs/color-modes.md): RGB, CMYK and Lab, as Photoshop lists them above the depths. 32 bits is
    // RGB only, as in Photoshop: CMYK Color and Lab Color are greyed in a 32-bit document, 32 Bits/Channel in a CMYK
    // or Lab one.
    auto rgbOnlyAt32 = [this] { return session_->sampleType() == SampleType::F32 ? session_->unavailableTip("mode.cmykLab") : QString(); };
    auto* colorModes = new QActionGroup(this);
    modeRgbAction_ = m.add(mode, Spec("image.mode.rgb", tr("&RGB Color")).document("document.mode").runs([this] { convertColorMode(ColorMode::RGB); }, "image.mode"));
    modeCmykAction_ = m.add(mode, Spec("image.mode.cmyk", tr("&CMYK Color")).document("document.mode").runs([this] { convertColorMode(ColorMode::CMYK); }, "image.mode").when(rgbOnlyAt32));
    modeLabAction_ = m.add(mode, Spec("image.mode.lab", tr("&Lab Color")).document("document.mode").runs([this] { convertColorMode(ColorMode::Lab); }, "image.mode").when(rgbOnlyAt32));
    for (QAction* a : {modeRgbAction_, modeCmykAction_, modeLabAction_}) { a->setCheckable(true); colorModes->addAction(a); }
    modeRgbAction_->setChecked(true);
    mode->addSeparator();
    auto* depths = new QActionGroup(this);
    mode8Action_ = m.add(mode, Spec("image.mode.bits8", tr("&8 Bits/Channel")).document("document.mode").runs([this] { convertMode(SampleType::U8); }, "image.mode"));
    mode16Action_ = m.add(mode, Spec("image.mode.bits16", tr("&16 Bits/Channel")).document("document.mode").runs([this] { convertMode(SampleType::U16); }, "image.mode"));
    mode32Action_ = m.add(mode, Spec("image.mode.bits32", tr("&32 Bits/Channel")).document("document.mode").runs([this] { convertMode(SampleType::F32); }, "image.mode").when([this] {
        return session_->colorMode() != ColorMode::RGB ? session_->unavailableTip("document.mode") : QString();
    }));
    for (QAction* a : {mode8Action_, mode16Action_, mode32Action_}) { a->setCheckable(true); depths->addAction(a); }
    mode8Action_->setChecked(true);
    image->addSeparator();
    m.add(image, Spec("image.canvasSize", tr("&Canvas Size…"), {QKeySequence("Ctrl+Alt+C")}).document("canvas.size").runs([this] {
        auto o = askCanvasSize(this, session_->document()->width, session_->document()->height);
        if (!o) return;
        runCommand("canvas.resize", {{"width", o->width}, {"height", o->height}, {"anchorX", o->anchorX}, {"anchorY", o->anchorY}}, tr("Canvas Size"));
    }, "canvas.resize"));
    m.add(image, Spec("image.imageSize", tr("&Image Size…"), {QKeySequence("Ctrl+Alt+I")}).document("edit.imageSize").runs([this] {
        auto o = askImageSize(this, session_->document()->width, session_->document()->height, session_->document()->resolution);
        if (!o) return;
        runCommand("image.resize", {{"width", o->width}, {"height", o->height}, {"resolution", o->resolution},
                                    {"sampling", o->sampling == 0 ? "nearest" : o->sampling == 1 ? "smooth" : "high"}}, tr("Image Size"));
    }, "image.resize"));
    m.add(image, Spec("image.trim", tr("&Trim…")).document("edit.crop").runs([this] {
        // Photoshop's dialog: what to trim by, and which sides.
        QDialog dialog(this);
        dialog.setWindowTitle(tr("Trim"));
        auto* layout = new QVBoxLayout(&dialog);
        auto* basedOn = new QComboBox;
        basedOn->addItems({tr("Transparent Pixels"), tr("Top Left Pixel Color"), tr("Bottom Right Pixel Color")});
        auto* form = new QFormLayout;
        form->addRow(tr("Based on"), basedOn);
        auto* tolerance = new QSpinBox;
        tolerance->setRange(0, 255);
        tolerance->setToolTip(tr("How far a pixel's channels may be from the corner's and still be trimmed"));
        form->addRow(tr("Tolerance"), tolerance);
        layout->addLayout(form);
        auto* sides = new QHBoxLayout;
        QCheckBox* side[4];
        const QString names[4] = {tr("Top"), tr("Left"), tr("Bottom"), tr("Right")};
        for (int i = 0; i < 4; i++) { side[i] = new QCheckBox(names[i]); side[i]->setChecked(true); sides->addWidget(side[i]); }
        layout->addWidget(new QLabel(tr("Trim away")));
        layout->addLayout(sides);
        auto sync = [&] { tolerance->setEnabled(basedOn->currentIndex() != 0); };
        connect(basedOn, QOverload<int>::of(&QComboBox::currentIndexChanged), &dialog, sync);
        sync();
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);
        if (dialog.exec() != QDialog::Accepted) return;
        TrimOptions o;
        o.basedOn = TrimOptions::BasedOn(basedOn->currentIndex());
        o.top = side[0]->isChecked(); o.left = side[1]->isChecked(); o.bottom = side[2]->isChecked(); o.right = side[3]->isChecked();
        o.tolerance = uint8_t(tolerance->value());
        static const char* const bases[] = {"transparent", "topLeft", "bottomRight"};
        trimCommand({{"basedOn", bases[std::clamp(basedOn->currentIndex(), 0, 2)]}, {"top", o.top}, {"left", o.left}, {"bottom", o.bottom}, {"right", o.right}, {"tolerance", int(o.tolerance)}});
    }, "image.trim"));
    m.add(image, Spec("image.cropToSelection", tr("Crop to Selection")).document("edit.crop").runs([this] {
        const auto& d = session_->document();
        if (d && d->selection) {
            Rect b = d->selection->bounds();
            if (!b.isEmpty()) {
                if (runCommand("canvas.crop", {{"x", b.x}, {"y", b.y}, {"width", b.width}, {"height", b.height}}, tr("Crop to Selection")))
                    runCommand("selection.none", {}, tr("Crop to Selection"));
            }
        }
    }, "canvas.crop"))->setObjectName("command.canvas.crop");
    image->addSeparator();
    QMenu* adjustments = image->addMenu(tr("&Adjustments"));
    auto pixelAdjustment = [this, adjustments, &m](const QString& label, const QKeySequence& key, AdjustmentKind kind) {
        const QString id = QStringLiteral("image.adjustments.") + commandSlug(QString::fromUtf8(adjustmentKindName(kind)));
        m.add(adjustments, Spec(id, label, key.isEmpty() ? QList<QKeySequence>{} : QList<QKeySequence>{key}).document((std::string("adjustment.") + adjustmentKindName(kind)).c_str()).runs([this, kind] {
            if (session_->smartObjectBlocksPixels(true)) return;
            if (!session_->canAdjustPixels()) { showError(tr("Adjustments"), tr("Select a visible image layer (not a mask) to adjust its pixels.")); return; }
            (new PixelAdjustmentDialog(session_, kind, this))->show();
        }, "pixels.adjust"));
    };
    pixelAdjustment(tr("&Levels…"), QKeySequence("Ctrl+L"), AdjustmentKind::Levels);
    pixelAdjustment(tr("&Curves…"), QKeySequence("Ctrl+M"), AdjustmentKind::Curves);
    pixelAdjustment(tr("&Hue/Saturation…"), QKeySequence("Ctrl+U"), AdjustmentKind::HueSaturation);
    pixelAdjustment(tr("&Exposure…"), QKeySequence(), AdjustmentKind::Exposure);
    pixelAdjustment(tr("&Gradient Map…"), QKeySequence(), AdjustmentKind::GradientMap);
    pixelAdjustment(tr("G&rain…"), QKeySequence(), AdjustmentKind::Grain);
    adjustments->addSeparator();
    pixelAdjustment(tr("&Brightness/Contrast…"), QKeySequence(), AdjustmentKind::BrightnessContrast);
    pixelAdjustment(tr("&Vibrance…"), QKeySequence(), AdjustmentKind::Vibrance);
    pixelAdjustment(tr("Color &Balance…"), QKeySequence("Ctrl+B"), AdjustmentKind::ColorBalance);
    pixelAdjustment(tr("Black && &White…"), QKeySequence("Alt+Shift+Ctrl+B"), AdjustmentKind::BlackWhite);
    pixelAdjustment(tr("&Photo Filter…"), QKeySequence(), AdjustmentKind::PhotoFilter);
    pixelAdjustment(tr("Channel &Mixer…"), QKeySequence(), AdjustmentKind::ChannelMixer);
    pixelAdjustment(tr("&Selective Color…"), QKeySequence(), AdjustmentKind::SelectiveColor);
    pixelAdjustment(tr("P&osterize…"), QKeySequence(), AdjustmentKind::Posterize);
    pixelAdjustment(tr("&Threshold…"), QKeySequence(), AdjustmentKind::Threshold);
    adjustments->addSeparator();
    m.add(adjustments, Spec("image.adjustments.invert", tr("&Invert"), {QKeySequence("Ctrl+I")}).document("adjustment.Invert")
                         .request("pixels.invert", [] { return QJsonObject{}; }, tr("Invert")))->setObjectName("command.pixels.invert");
    image->addSeparator();
    m.add(image, Spec("image.flipCanvasHorizontal", tr("Flip Canvas Horizontal")).document("canvas.flip")
                   .request("canvas.flip", [] { return QJsonObject{}; }, tr("Flip Canvas Horizontal")))->setObjectName("command.canvas.flip");
    m.add(image, Spec("image.flipCanvasVertical", tr("Flip Canvas Vertical")).document("canvas.flip")
                   .request("canvas.flip", [] { return QJsonObject{{"vertical", true}}; }, tr("Flip Canvas Vertical")));
}

} // namespace app
