// The main window's menus, tool rail and colour swatches.
#include "MainWindow.h"
#include <QDialog>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QSpinBox>
#include <QCheckBox>
#include <QFormLayout>
#include <QDialogButtonBox>
#include "LayerStyleDialog.h"
#include "compositor/raw.h"
#include "WarpDialog.h"
#include "CanvasFrame.h"
#include "Dialogs.h"
#include "ImageConvert.h"
#include "CameraRawDialog.h"
#include "FilterDialog.h"
#include "GmicDialog.h"
#include "ColorSwatches.h"
#include "PreferencesDialog.h"
#include "BrushImporter.h"
#include "PresetLibrary.h"
#include "compositor/project.h"
#include <QActionGroup>
#include <QApplication>
#include <QColorDialog>
#include <QDockWidget>
#include <QStatusBar>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenuBar>
#include <QMessageBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QSettings>
#include <QToolBar>

using namespace compositor;

namespace app {

void MainWindow::buildToolRail() {
    auto* rail = new QToolBar(tr("Tools"), this);
    rail->setObjectName("toolRail");
    rail->setOrientation(Qt::Vertical);
    rail->setMovable(false);
    rail->setIconSize(QSize(22, 22));
    rail->setToolButtonStyle(Qt::ToolButtonIconOnly);
    auto* group = new QActionGroup(this);
    group->setExclusive(true);
    auto tool = [&](Tool t, const QString& label, const QString& iconName, const QKeySequence& key) {
        QAction* a = rail->addAction(toolIcon(iconName), label);
        a->setToolTip(label + " (" + key.toString() + ")");
        a->setCheckable(true);
        a->setShortcut(key);
        a->setShortcutContext(Qt::WindowShortcut);
        group->addAction(a);
        connect(a, &QAction::triggered, this, [this, t] { session_->selectTool(t); if (t == Tool::Brush) { session_->brushErase = false; emit session_->toolChanged(); } canvas_->setFocus(); });
        toolActions_[t] = a;
        return a;
    };
    tool(Tool::Move, tr("Move / Transform"), "move", QKeySequence("V"))->setChecked(true);
    tool(Tool::Marquee, tr("Marquee"), "square-dashed", QKeySequence("M"));
    tool(Tool::Lasso, tr("Lasso"), "lasso", QKeySequence("L"));
    tool(Tool::Wand, tr("Magic Wand"), "wand-sparkles", QKeySequence("W"));
    tool(Tool::Scribble, tr("Quick Select"), "scribble", QKeySequence("Shift+W"));   // Photoshop's W group
    tool(Tool::Crop, tr("Crop"), "crop", QKeySequence("C"));
    rail->addSeparator();
    tool(Tool::Brush, tr("Brush"), "paintbrush", QKeySequence("B"));
    eraserAction_ = rail->addAction(toolIcon("eraser"), tr("Eraser"));
    eraserAction_->setToolTip(tr("Eraser (E)"));
    eraserAction_->setCheckable(true);
    eraserAction_->setShortcut(QKeySequence("E"));
    group->addAction(eraserAction_);
    connect(eraserAction_, &QAction::triggered, this, [this] { session_->brushErase = true; session_->selectTool(Tool::Brush); emit session_->toolChanged(); canvas_->setFocus(); });
    tool(Tool::SpotHealing, tr("Spot Healing Brush"), "bandage", QKeySequence("J"));
    tool(Tool::CloneStamp, tr("Clone Stamp (Alt-click sets the source)"), "stamp", QKeySequence("S"));
    tool(Tool::Smudge, tr("Liquify / Blur / Smudge"), "droplet", QKeySequence("R"));
    tool(Tool::Dodge, tr("Dodge / Burn / Sponge"), "lollipop", QKeySequence("O"));
    tool(Tool::Gradient, tr("Gradient"), "blend", QKeySequence("G"));
    tool(Tool::PaintBucket, tr("Paint Bucket"), "paint-bucket", QKeySequence("Shift+G"));   // Photoshop's G group
    tool(Tool::Pen, tr("Pen (click corners, drag curves; click the first point or Enter to finish)"), "pen-tool", QKeySequence("P"));
    tool(Tool::DirectSelect, tr("Direct Selection (drag points, handles or a whole path; Alt-click converts a point)"), "mouse-pointer-2", QKeySequence("A"));
    tool(Tool::Shape, tr("Shape (Shift-U steps through Rectangle, Ellipse, Polygon, Line, Custom)"), "shapes", QKeySequence("U"));
    tool(Tool::Text, tr("Text"), "type", QKeySequence("T"));
    tool(Tool::Eyedropper, tr("Eyedropper"), "pipette", QKeySequence("I"));
    rail->addSeparator();
    tool(Tool::Hand, tr("Hand"), "hand", QKeySequence("H"));
    tool(Tool::Zoom, tr("Zoom"), "zoom-in", QKeySequence("Z"));
    rail->addSeparator();
    swatches_ = new ColorSwatches;
    connect(swatches_, &ColorSwatches::foregroundClicked, this, [this] { chooseColor(false); });
    connect(swatches_, &ColorSwatches::backgroundClicked, this, [this] { chooseColor(true); });
    auto* swap = new QAction(tr("Swap colours"), this);
    swap->setShortcut(QKeySequence("X"));
    connect(swap, &QAction::triggered, this, [this] { std::swap(session_->foregroundColor, session_->backgroundColor); updateColorSwatches(); });
    addAction(swap);
    connect(swatches_, &ColorSwatches::swapRequested, swap, &QAction::trigger);
    auto* defaults = new QAction(tr("Default colours"), this);
    defaults->setShortcut(QKeySequence("D"));
    connect(defaults, &QAction::triggered, this, [this] { session_->foregroundColor = Qt::black; session_->backgroundColor = Qt::white; updateColorSwatches(); });
    addAction(defaults);
    connect(swatches_, &ColorSwatches::defaultsRequested, defaults, &QAction::trigger);
    rail->addWidget(swatches_);
    addToolBar(Qt::LeftToolBarArea, rail);
    // Shift-letter switches a tool's kind without leaving it.
    auto kindKey = [this](const QString& key, auto slot) { auto* a = new QAction(this); a->setShortcut(QKeySequence(key)); connect(a, &QAction::triggered, this, slot); addAction(a); };
    kindKey("Shift+M", [this] { session_->marqueeKind = session_->marqueeKind == MarqueeKind::Rectangle ? MarqueeKind::Ellipse : MarqueeKind::Rectangle; session_->selectTool(Tool::Marquee); emit session_->toolChanged(); });
    kindKey("Shift+L", [this] { session_->lassoKind = session_->lassoKind == LassoKind::Freehand ? LassoKind::Polygonal : LassoKind::Freehand; canvas_->cancelLasso(); session_->selectTool(Tool::Lasso); emit session_->toolChanged(); });
    kindKey("Shift+U", [this] { session_->selectTool(Tool::Shape); session_->toggleShapeKind(); });
}

void MainWindow::updateColorSwatches() {
    swatches_->setColors(session_->foregroundColor, session_->backgroundColor);
}

void MainWindow::chooseColor(bool background) {
    QColor current = background ? session_->backgroundColor : session_->foregroundColor;
    QColor c = QColorDialog::getColor(current, this, background ? tr("Background Colour") : tr("Foreground Colour"));
    if (!c.isValid()) return;
    (background ? session_->backgroundColor : session_->foregroundColor) = c;
    updateColorSwatches();
}

void MainWindow::buildMenus() {
    auto needsDocument = [this](QAction* a) { documentActions_ << a; return a; };
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&New…"), QKeySequence::New, this, &MainWindow::newDocument);
    file->addAction(tr("&Open…"), QKeySequence::Open, this, &MainWindow::openFiles);
    file->addAction(tr("Open &Project…"), this, &MainWindow::openProject);
    recentMenu_ = file->addMenu(tr("Open &Recent"));
    file->addAction(tr("Import &File…"), QKeySequence("Ctrl+Shift+O"), this, &MainWindow::importFiles);
    file->addAction(tr("Import &Brushes…"), this, [this] { importBrushesInteractively(this, session_); });
    file->addAction(tr("Import Pre&sets…"), this, [this] { importPresetsInteractively(this, session_); });
    needsDocument(file->addAction(tr("Place &Embedded…"), this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Place Embedded"), QSettings().value("lastDir").toString(),
                                                          tr("Images, Photoshop and Affinity documents (*.psd *.psb *.afphoto *.afdesign *.afpub *.af *.png *.jpg *.jpeg *.tif *.tiff *.webp *.bmp *.gif %1)").arg(compositor::rawSupported() ? QStringLiteral("*.cr2 *.cr3 *.crw *.nef *.nrw *.arw *.srf *.sr2 *.raf *.orf *.rw2 *.rwl *.pef *.dng *.3fr *.iiq *.erf *.kdc *.dcr *.mrw *.srw *.x3f") : QString()));
        if (path.isEmpty()) return;
        QSettings().setValue("lastDir", QFileInfo(path).path());
        QString error;
        if (!session_->placeEmbedded(path, &error)) showError(tr("Couldn’t place %1").arg(QFileInfo(path).fileName()), error);
    }));
    file->addSeparator();
    needsDocument(file->addAction(tr("&Save"), QKeySequence::Save, this, [this] { save(false); }));
    needsDocument(file->addAction(tr("Save &As…"), QKeySequence::SaveAs, this, [this] { save(true); }));
    file->addSeparator();
    needsDocument(file->addAction(tr("Export as Photoshop &Document (PSD)…"), this, &MainWindow::exportPsd));
    needsDocument(file->addAction(tr("Export &PNG…"), QKeySequence("Ctrl+Shift+E"), this, &MainWindow::exportPng));
    needsDocument(file->addAction(tr("Export &JPEG…"), QKeySequence("Ctrl+Alt+Shift+S"), this, &MainWindow::exportJpeg));
    if (canWriteImageFormat("webp")) needsDocument(file->addAction(tr("Export &WebP…"), this, &MainWindow::exportWebp));
    if (canWriteImageFormat("tiff")) needsDocument(file->addAction(tr("Export &TIFF…"), this, &MainWindow::exportTiff));
    needsDocument(file->addAction(tr("Export T&GA…"), this, &MainWindow::exportTga));
    needsDocument(file->addAction(tr("Export &Icon (ICO)…"), this, &MainWindow::exportIco));
    file->addSeparator();
    file->addAction(tr("&Close Tab"), QKeySequence::Close, this, [this] { closeTab(current_); });
    file->addAction(tr("New &Tab"), QKeySequence::AddTab, this, [this] { addTab(false); });
    file->addAction(tr("Next Tab"), QKeySequence("Ctrl+Tab"), this, [this] { if (tabs_.size() > 1) switchTo((current_ + 1) % int(tabs_.size())); });
    file->addAction(tr("Previous Tab"), QKeySequence("Ctrl+Shift+Tab"), this, [this] { if (tabs_.size() > 1) switchTo((current_ + int(tabs_.size()) - 1) % int(tabs_.size())); });
    file->addAction(tr("&Quit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    undoAction_ = edit->addAction(tr("&Undo"), QKeySequence::Undo, this, [this] { session_->undo(); });
    redoAction_ = edit->addAction(tr("&Redo"), QKeySequence("Ctrl+Shift+Z"), this, [this] { session_->redo(); });
    edit->addSeparator();
    needsDocument(edit->addAction(tr("Cu&t"), QKeySequence::Cut, this, [this] { session_->cutSelection(); }));
    needsDocument(edit->addAction(tr("&Copy"), QKeySequence::Copy, this, [this] { session_->copySelection(); }));
    needsDocument(edit->addAction(tr("Copy &Merged"), QKeySequence("Ctrl+Shift+C"), this, [this] { session_->copyMerged(); }));
    needsDocument(edit->addAction(tr("&Paste"), QKeySequence::Paste, this, [this] { session_->paste(); }));
    edit->addSeparator();
    needsDocument(edit->addAction(tr("Free &Transform"), QKeySequence("Ctrl+T"), this, [this] { session_->transformCommand(); }));
    needsDocument(edit->addAction(tr("&Warp…"), this, [this] { WarpDialog(session_, this).exec(); }));
    needsDocument(edit->addAction(tr("Warp &Cage"), this, [this] {
        QString error;
        if (!session_->beginWarpCage(&error)) showError(tr("Warp Cage"), error);
        else statusBar()->showMessage(tr("Drag the cage's points; Enter applies, Esc cancels."), 8000);
    }));
    needsDocument(edit->addAction(tr("Fill with Foreground"), QKeySequence("Alt+Backspace"), this, [this] { session_->fillSelection(session_->foregroundColor); }));
    needsDocument(edit->addAction(tr("Fill with Background"), QKeySequence("Ctrl+Backspace"), this, [this] { session_->fillSelection(session_->backgroundColor); }));
    QAction* clear = needsDocument(edit->addAction(tr("Clear"), QKeySequence(Qt::Key_Delete), this, [this] { if (session_->document() && session_->document()->selection) session_->clearSelectionPixels(); else deleteSelectedLayers(); }));
    clear->setShortcuts({QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace)});
    needsDocument(edit->addAction(tr("Content-Aware Fill"), QKeySequence("Shift+F5"), this, [this] { QString error; if (!session_->contentAwareFill(&error)) showError(tr("Content-Aware Fill"), error); }));

    edit->addSeparator();
    edit->addAction(tr("&Preferences…"), QKeySequence::Preferences, this, &MainWindow::showPreferences);

    QMenu* image = menuBar()->addMenu(tr("&Image"));
    needsDocument(image->addAction(tr("&Canvas Size…"), QKeySequence("Ctrl+Alt+C"), this, [this] {
        auto o = askCanvasSize(this, session_->document()->width, session_->document()->height);
        if (o) session_->resizeCanvas(o->width, o->height, o->anchorX, o->anchorY);
    }));
    needsDocument(image->addAction(tr("&Image Size…"), QKeySequence("Ctrl+Alt+I"), this, [this] {
        auto o = askImageSize(this, session_->document()->width, session_->document()->height, session_->document()->resolution);
        if (o) session_->resizeImage(o->width, o->height, o->resolution, o->sampling);
    }));
    needsDocument(image->addAction(tr("&Trim…"), this, [this] {
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
        if (!session_->trim(o)) showError(tr("Trim"), tr("There is nothing to trim: the canvas already ends at its content, or nothing would remain."));
    }));
    needsDocument(image->addAction(tr("Crop to Selection"), this, [this] {
        const auto& d = session_->document();
        if (d && d->selection) { Rect b = d->selection->bounds(); if (!b.isEmpty()) { session_->cropTo(QRectF(b.x, b.y, b.width, b.height)); session_->deselect(); } }
    }));
    image->addSeparator();
    QMenu* adjustments = image->addMenu(tr("&Adjustments"));
    auto pixelAdjustment = [this, adjustments, &needsDocument](const QString& label, const QKeySequence& key, AdjustmentKind kind) {
        needsDocument(adjustments->addAction(label, key, this, [this, kind] {
            if (session_->smartObjectBlocksPixels(true)) return;
            if (!session_->canAdjustPixels()) { showError(tr("Adjustments"), tr("Select a visible image layer (not a mask) to adjust its pixels.")); return; }
            (new PixelAdjustmentDialog(session_, kind, this))->show();
        }));
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
    needsDocument(adjustments->addAction(tr("&Invert"), QKeySequence("Ctrl+I"), this, [this] { session_->invertActive(); }));
    image->addSeparator();
    needsDocument(image->addAction(tr("Flip Canvas Horizontal"), this, [this] { session_->flipCanvas(true); }));
    needsDocument(image->addAction(tr("Flip Canvas Vertical"), this, [this] { session_->flipCanvas(false); }));

    QMenu* layer = menuBar()->addMenu(tr("&Layer"));
    needsDocument(layer->addAction(tr("&New Layer"), QKeySequence("Ctrl+Shift+N"), this, [this] { session_->addBlankLayer(); }));
    needsDocument(layer->addAction(tr("New Layer &Below"), this, [this] { session_->addBlankLayer(true); }));
    needsDocument(layer->addAction(tr("New &Folder"), this, [this] { session_->addGroup(); }));
    needsDocument(layer->addAction(tr("&Group Layers"), QKeySequence("Ctrl+G"), this, [this] { session_->groupSelectedLayers(); }));
    needsDocument(layer->addAction(tr("Layer via &Copy"), QKeySequence("Ctrl+J"), this, [this] { session_->layerViaCopy(); }));
    needsDocument(layer->addAction(tr("&Duplicate Layer"), this, [this] { session_->duplicateActiveLayer(); }));
    needsDocument(layer->addAction(tr("De&lete Layer"), this, [this] { deleteSelectedLayers(); }));
    mergeAction_ = needsDocument(layer->addAction(tr("Merge &Down"), QKeySequence("Ctrl+E"), this, [this] { session_->mergeLayers(); }));
    editTextAction_ = needsDocument(layer->addAction(tr("Edit &Text…"), this, [this] { const Layer* l = session_->activeLayer(); if (l && l->isLiveText()) session_->requestTextEdit(l->id); }));
    needsDocument(layer->addAction(tr("&Rename Layer…"), this, [this] {
        const Layer* active = session_->activeLayer();
        if (!active) return;
        bool ok;
        QString name = QInputDialog::getText(this, tr("Rename Layer"), tr("Name"), QLineEdit::Normal, QString::fromStdString(active->name), &ok);
        if (ok) session_->renameLayer(active->id, name);
    }));
    needsDocument(layer->addAction(tr("Move &Out of Folder"), QKeySequence("Ctrl+Shift+["), this, [this] { session_->moveActiveLayerOutOfGroup(); }));
    QMenu* adjustmentLayers = layer->addMenu(tr("New &Adjustment Layer"));
    for (int i = 0; i < adjustmentKindCount; i++) {
        AdjustmentKind kind = AdjustmentKind(i);
        needsDocument(adjustmentLayers->addAction(QString::fromUtf8(adjustmentKindName(kind)), this, [this, kind] { session_->addAdjustmentLayer(kind); }));
    }
    QMenu* styles = layer->addMenu(tr("Layer St&yle"));
    const char* const stylePages[] = {"Blending Options…", "Bevel & Emboss…", "Stroke…", "Inner Shadow…", "Inner Glow…", "Satin…", "Color Overlay…",
                                      "Gradient Overlay…", "Pattern Overlay…", "Outer Glow…", "Drop Shadow…"};
    for (int page = 0; page < int(std::size(stylePages)); page++) {
        needsDocument(styles->addAction(tr(stylePages[page]), this, [this, page] {
            if (session_->activeLayerId()) LayerStyleDialog(session_, *session_->activeLayerId(), this, page).exec();
        }));
        if (page == 0) styles->addSeparator();
    }
    styles->addSeparator();
    needsDocument(styles->addAction(tr("&Copy Layer Style"), this, [this] { session_->copyLayerStyle(); }));
    needsDocument(styles->addAction(tr("&Paste Layer Style"), this, [this] { session_->pasteLayerStyle(); }));
    needsDocument(styles->addAction(tr("C&lear Layer Style"), this, [this] { session_->clearLayerStyle(); }));
    styles->addSeparator();
    // Imported style presets (.asl): the submenu lists the library as it is when it opens.
    QMenu* applyStyle = styles->addMenu(tr("&Apply Style"));
    needsDocument(applyStyle->menuAction());
    connect(applyStyle, &QMenu::aboutToShow, this, [this, applyStyle] {
        applyStyle->clear();
        const auto& presets = PresetLibrary::instance().styles();
        if (presets.empty()) applyStyle->addAction(tr("No styles yet: Import Styles…"))->setEnabled(false);
        for (const auto& preset : presets) {
            const QString name = QString::fromStdString(preset.name);
            applyStyle->addAction(name, this, [this, name] {
                const StylePreset* p = PresetLibrary::instance().findStyle(name);
                if (!p || !session_->activeLayerId()) return;
                const StylePreset copy = *p;
                if (!session_->applyStylePreset(*session_->activeLayerId(), copy.style, PresetLibrary::instance().patternsFor(copy.style)))
                    showError(tr("Couldn’t apply the style"), tr("This layer cannot have effects."));
            });
        }
    });
    styles->addAction(tr("&Import Styles…"), this, [this] { importPresetsInteractively(this, session_); });
    QMenu* smart = layer->addMenu(tr("S&mart Objects"));
    needsDocument(smart->addAction(tr("&Convert to Smart Object"), this, [this] {
        QString error;
        if (!session_->convertToSmartObject(&error) && !error.isEmpty()) showError(tr("Couldn’t convert to a smart object"), error);
    }));
    needsDocument(smart->addAction(tr("&Edit Contents"), this, [this] { editSmartObjectContents(); }));
    needsDocument(smart->addAction(tr("&Replace Contents…"), this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Replace Contents"), QSettings().value("lastDir").toString(),
                                                          tr("Images, Photoshop and Affinity documents (*.psd *.psb *.afphoto *.afdesign *.afpub *.af *.png *.jpg *.jpeg *.tif *.tiff *.webp *.bmp *.gif %1)").arg(compositor::rawSupported() ? QStringLiteral("*.cr2 *.cr3 *.crw *.nef *.nrw *.arw *.srf *.sr2 *.raf *.orf *.rw2 *.rwl *.pef *.dng *.3fr *.iiq *.erf *.kdc *.dcr *.mrw *.srw *.x3f") : QString()));
        if (path.isEmpty()) return;
        QString error;
        if (!session_->replaceSmartObjectContents(path, &error)) showError(tr("Couldn’t replace the contents"), error);
    }));
    needsDocument(smart->addAction(tr("R&asterize"), this, [this] { session_->rasterizeSmartObject(); }));
    layer->addSeparator();
    QMenu* mask = layer->addMenu(tr("Layer &Mask"));
    needsDocument(mask->addAction(tr("Reveal All"), this, [this] { session_->addLayerMask(true); }));
    needsDocument(mask->addAction(tr("Hide All"), this, [this] { session_->addLayerMask(false); }));
    needsDocument(mask->addAction(tr("From Selection (Reveal)"), this, [this] { session_->addMaskFromSelection(true); }));
    needsDocument(mask->addAction(tr("From Selection (Hide)"), this, [this] { session_->addMaskFromSelection(false); }));
    mask->addSeparator();
    needsDocument(mask->addAction(tr("Enable / Disable"), this, [this] { session_->toggleLayerMask(); }));
    needsDocument(mask->addAction(tr("Invert"), this, [this] { session_->invertMask(); }));
    needsDocument(mask->addAction(tr("Apply"), this, [this] { session_->applyMask(); }));
    needsDocument(mask->addAction(tr("Delete"), this, [this] { session_->deleteLayerMask(); }));
    needsDocument(layer->addAction(tr("Create / Release &Clipping Mask"), QKeySequence("Ctrl+Alt+G"), this, [this] { if (session_->activeLayerId()) session_->toggleClippingMask(*session_->activeLayerId()); }));
    layer->addSeparator();
    needsDocument(layer->addAction(tr("Bring Forward"), QKeySequence("Ctrl+]"), this, [this] { session_->moveActiveLayer(1); }));
    needsDocument(layer->addAction(tr("Send Backward"), QKeySequence("Ctrl+["), this, [this] { session_->moveActiveLayer(-1); }));
    layer->addSeparator();
    needsDocument(layer->addAction(tr("Flip Layer Horizontal"), this, [this] { session_->flipLayer(true); }));
    needsDocument(layer->addAction(tr("Flip Layer Vertical"), this, [this] { session_->flipLayer(false); }));
    QMenu* sampling = layer->addMenu(tr("Resampling"));
    needsDocument(sampling->addAction(tr("High Quality"), this, [this] { session_->setLayerSampling(Sampling::High); }));
    needsDocument(sampling->addAction(tr("Smooth"), this, [this] { session_->setLayerSampling(Sampling::Smooth); }));
    needsDocument(sampling->addAction(tr("Nearest Neighbour"), this, [this] { session_->setLayerSampling(Sampling::Nearest); }));

    // Everything about the selection in one place, as Photoshop's Select menu: the whole-canvas commands,
    // then Modify, then loading a layer's pixels or mask as the selection.
    QMenu* select = menuBar()->addMenu(tr("&Select"));
    needsDocument(select->addAction(tr("&All"), QKeySequence::SelectAll, this, [this] { session_->selectAll(); }));
    needsDocument(select->addAction(tr("&Deselect"), QKeySequence("Ctrl+D"), this, [this] { session_->deselect(); }));
    needsDocument(select->addAction(tr("&Inverse"), QKeySequence("Ctrl+Shift+I"), this, [this] { session_->invertSelection(); }));
    needsDocument(select->addAction(tr("Edit in &Quick Mask Mode"), QKeySequence("Q"), this, [this] { session_->toggleQuickMask(); }));
    select->addSeparator();
    QMenu* modify = select->addMenu(tr("&Modify"));
    needsDocument(modify->addAction(tr("&Expand…"), this, [this] { bool ok; int n = QInputDialog::getInt(this, tr("Expand Selection"), tr("Pixels"), 1, 1, 500, 1, &ok); if (ok) session_->selectionExpand(n); }));
    needsDocument(modify->addAction(tr("&Contract…"), this, [this] { bool ok; int n = QInputDialog::getInt(this, tr("Contract Selection"), tr("Pixels"), 1, 1, 500, 1, &ok); if (ok) session_->selectionContract(n); }));
    needsDocument(modify->addAction(tr("&Feather…"), QKeySequence("Shift+F6"), this, [this] { bool ok; double r = QInputDialog::getDouble(this, tr("Feather Selection"), tr("Radius (pixels)"), 5, 0.1, 250, 1, &ok); if (ok) session_->selectionFeather(r); }));
    needsDocument(modify->addAction(tr("&Smooth…"), this, [this] { bool ok; int n = QInputDialog::getInt(this, tr("Smooth Selection"), tr("Sample radius (pixels)"), 3, 1, 100, 1, &ok); if (ok) session_->selectionSmooth(n); }));
    needsDocument(modify->addAction(tr("&Border…"), this, [this] { bool ok; int n = QInputDialog::getInt(this, tr("Border Selection"), tr("Width (pixels)"), 4, 1, 200, 1, &ok); if (ok) session_->selectionBorder(n); }));
    select->addSeparator();
    QMenu* load = select->addMenu(tr("&Load as Selection"));
    needsDocument(load->addAction(tr("Layer Pixels"), this, [this] { if (session_->activeLayerId()) session_->loadLayerAsSelection(*session_->activeLayerId(), false, SelectionMode::Replace); }));
    needsDocument(load->addAction(tr("Layer Mask"), this, [this] { if (session_->activeLayerId()) session_->loadLayerAsSelection(*session_->activeLayerId(), true, SelectionMode::Replace); }));
    load->addSeparator();
    needsDocument(load->addAction(tr("Add Layer Pixels"), this, [this] { if (session_->activeLayerId()) session_->loadLayerAsSelection(*session_->activeLayerId(), false, SelectionMode::Add); }));
    needsDocument(load->addAction(tr("Subtract Layer Pixels"), this, [this] { if (session_->activeLayerId()) session_->loadLayerAsSelection(*session_->activeLayerId(), false, SelectionMode::Subtract); }));
    needsDocument(load->addAction(tr("Intersect with Layer Pixels"), this, [this] { if (session_->activeLayerId()) session_->loadLayerAsSelection(*session_->activeLayerId(), false, SelectionMode::Intersect); }));

    QMenu* filter = menuBar()->addMenu(tr("Filte&r"));
    auto filterAction = [this, filter, &needsDocument](const QString& label, FilterKind kind) {
        needsDocument(filter->addAction(label, this, [this, kind] {
            // On a smart object the blurs and noise go on as Smart Filters, as in Photoshop.
            if (kind != FilterKind::LensCorrection && session_->canAddSmartFilter()) { (new FilterDialog(session_, kind, this, true))->show(); return; }
            if (session_->smartObjectBlocksPixels(true)) return;
            if (!session_->canAdjustPixels()) { showError(tr("Filters"), tr("Select a visible image layer (not a mask) to filter its pixels.")); return; }
            (new FilterDialog(session_, kind, this))->show();
        }));
    };
    filterAction(tr("&Gaussian Blur…"), FilterKind::GaussianBlur);
    filterAction(tr("&Motion Blur…"), FilterKind::MotionBlur);
    filterAction(tr("Add &Noise…"), FilterKind::AddNoise);
    filterAction(tr("&Lens Correction…"), FilterKind::LensCorrection);
    filter->addSeparator();
    // Photoshop's shortcut. A destructive filter here: on a smart object it asks first, like the others.
    needsDocument(filter->addAction(tr("Camera &Raw Filter…"), QKeySequence("Shift+Ctrl+A"), this, [this] {
        if (session_->smartObjectBlocksPixels(true)) return;
        if (!session_->canAdjustPixels()) { showError(tr("Camera Raw Filter"), tr("Select a visible image layer (not a mask) to filter its pixels.")); return; }
        (new CameraRawDialog(session_, this))->show();
    }));
    filter->addSeparator();
    needsDocument(filter->addAction(tr("&G'MIC…"), QKeySequence("Ctrl+Shift+G"), this, [this] {
        if (session_->smartObjectBlocksPixels(true)) return;
            if (!session_->canAdjustPixels()) { showError(tr("G'MIC"), tr("Select a layer with pixels first.")); return; }
        (new GmicDialog(session_, this))->show();
    }));
    removeBackgroundAction_ = needsDocument(filter->addAction(tr("Remove &Background…"), this, [this] {
        if (!ModelStore::ready()) {
            // Off, or no model yet: the preferences page is where it gets turned on and fetched.
            auto answer = QMessageBox::question(this, tr("Remove Background"),
                ModelStore::supported() ? tr("AI background removal is turned off. Open Preferences to enable it and download the model?")
                                        : tr("This build was made without OpenCV, which runs the segmentation model."),
                ModelStore::supported() ? (QMessageBox::Yes | QMessageBox::Cancel) : QMessageBox::Ok);
            if (answer == QMessageBox::Yes) showPreferences();
            return;
        }
        if (session_->smartObjectBlocksPixels(true)) return;
            if (!session_->canAdjustPixels()) { showError(tr("Remove Background"), tr("Select a visible image layer (not a mask) to remove its background.")); return; }
        const ModelInfo* quick = ModelStore::modelById("pphumanseg");
        QString quickPath = quick && ModelStore::isPresent(*quick) ? ModelStore::pathFor(*quick) : QString();
        (new BackgroundDialog(session_, ModelStore::pathFor(ModelStore::selected()), quickPath, this))->show();
    }));
    refreshBackgroundAction();

    QMenu* view = menuBar()->addMenu(tr("&View"));
    needsDocument(view->addAction(tr("Zoom &In"), QKeySequence::ZoomIn, this, [this] { session_->zoomTo(session_->viewport.zoom * 1.25); }));
    needsDocument(view->addAction(tr("Zoom &Out"), QKeySequence::ZoomOut, this, [this] { session_->zoomTo(session_->viewport.zoom / 1.25); }));
    needsDocument(view->addAction(tr("&Fit on Screen"), QKeySequence("Ctrl+0"), this, [this] { session_->fitView(); }));
    needsDocument(view->addAction(tr("&Actual Pixels"), QKeySequence("Ctrl+1"), this, [this] { session_->zoomTo(1); }));
    view->addSeparator();
    rulersAction_ = view->addAction(tr("&Rulers"), QKeySequence("Ctrl+R"), this, [this](bool on) {
        for (auto& tab : tabs_) tab.frame->setRulersVisible(on);
        QSettings().setValue("view/rulers", on);
    });
    rulersAction_->setCheckable(true);
    rulersAction_->setChecked(QSettings().value("view/rulers", true).toBool());
    for (auto& tab : tabs_) tab.frame->setRulersVisible(rulersAction_->isChecked());
    view->addSeparator();
    layersDock_->toggleViewAction()->setText(tr("&Layers Panel"));
    adjustDock_->toggleViewAction()->setText(tr("&Adjustments Panel"));
    view->addAction(layersDock_->toggleViewAction());
    pathsDock_->toggleViewAction()->setText(tr("&Paths Panel"));
    view->addAction(pathsDock_->toggleViewAction());
    view->addAction(adjustDock_->toggleViewAction());
    view->addSeparator();
    QAction* grid = view->addAction(tr("Pixel &Grid"), this, [this](bool on) { session_->showsPixelGrid = on; canvas_->update(); });
    grid->setCheckable(true);
    grid->setChecked(true);
    QAction* controls = view->addAction(tr("Transform &Controls"), QKeySequence("Ctrl+H"), this, [this](bool on) { session_->showsTransformControls = on; emit session_->transformChanged(); });
    controls->setCheckable(true);
    controls->setChecked(true);

    QMenu* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("&Welcome to NekoPhoto"), this, &MainWindow::showWelcome);
    help->addAction(tr("&About NekoPhoto"), this, [this] {
        QMessageBox::about(this, tr("About NekoPhoto"), tr("<b>NekoPhoto</b> %3<br>A layered photo editor and painting app for Linux. "
            "It began as a Linux port of <a href=\"https://github.com/robbietilton/Compositor\">Compositor</a> for macOS, and still opens its projects.<br><br>"
            "Qt %1 &middot; project format version %2<br><br>"
            "Free software under the GNU General Public License, version 3 or later, with ABSOLUTELY NO WARRANTY. "
            "Compositor's own code is MIT licensed by Wonder Assembly LLC; the licences of the bundled components "
            "are in THIRD-PARTY-NOTICES.md, installed with the program.").arg(QT_VERSION_STR).arg(projectFormatVersion).arg(QApplication::applicationVersion()));
    });
    refreshRecent();
}


} // namespace app
