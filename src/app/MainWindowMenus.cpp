// The main window's menus, tool rail and colour swatches.
#include "ContentFillDialog.h"
#include "ChannelDialogs.h"
#include "Names.h"
#include "ContentAwareScaleDialog.h"
#include "MainWindow.h"
#include <algorithm>
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
#include "MoshDialog.h"
#include "GmicDialog.h"
#include "ColorDialogs.h"
#include "ColorManagement.h"
#include "ColorSwatches.h"
#include "PreferencesDialog.h"
#include "BrushImporter.h"
#include "PresetLibrary.h"
#include "ActionLibrary.h"
#include <QJsonObject>
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
        connect(a, &QAction::triggered, this, [this, t] { session_->selectTool(t); if (t == Tool::Brush) { session_->brushErase = false; emit session_->toolChanged(); } canvas_->setFocus(); groupLast_[toolGroupKey(t)] = t; });
        toolActions_[t] = a;
        return a;
    };
    tool(Tool::Move, tr("Move / Transform"), "move", QKeySequence("V"))->setChecked(true);
    tool(Tool::Marquee, tr("Marquee"), "square-dashed", QKeySequence("M"));
    tool(Tool::Lasso, tr("Lasso"), "lasso", QKeySequence("L"));
    tool(Tool::Wand, tr("Magic Wand"), "wand-sparkles", QKeySequence("W"));
    tool(Tool::Scribble, tr("Quick Select"), "scribble", QKeySequence("Shift+W"));   // Photoshop's W group (Shift+W steps through it)
    tool(Tool::Crop, tr("Crop"), "crop", QKeySequence("C"));
    tool(Tool::Slice, tr("Slice (drag a slice; drag inside to move it, an edge to resize)"), "slice", QKeySequence("Shift+C"));   // Photoshop's C group
    tool(Tool::Artboard, tr("Artboard (drag a new artboard; drag inside to move it with its contents, an edge to resize)"), "frame", QKeySequence("Shift+V"));   // Photoshop's V group
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
    connect(swap, &QAction::triggered, this, [this] {
        // With the Crop tool, X turns the crop box (Photoshop's Swap Height and Width).
        if (session_->tool() == Tool::Crop && canvas_->cropRect()) { canvas_->swapCropOrientation(); return; }
        std::swap(session_->foregroundColor, session_->backgroundColor);
        updateColorSwatches();
        recordAction("colors.set", {{"foreground", session_->foregroundColor.name()}, {"background", session_->backgroundColor.name()}});
    });
    addAction(swap);
    connect(swatches_, &ColorSwatches::swapRequested, swap, &QAction::trigger);
    auto* defaults = new QAction(tr("Default colours"), this);
    defaults->setShortcut(QKeySequence("D"));
    connect(defaults, &QAction::triggered, this, [this] {
        session_->foregroundColor = Qt::black; session_->backgroundColor = Qt::white; updateColorSwatches();
        recordAction("colors.set", {{"foreground", "#000000"}, {"background", "#ffffff"}});
    });
    addAction(defaults);
    connect(swatches_, &ColorSwatches::defaultsRequested, defaults, &QAction::trigger);
    rail->addWidget(swatches_);
    addToolBar(Qt::LeftToolBarArea, rail);
    // Shift-letter switches a tool's kind without leaving it.
    auto kindKey = [this](const QString& key, auto slot) { auto* a = new QAction(this); a->setShortcut(QKeySequence(key)); connect(a, &QAction::triggered, this, slot); addAction(a); };
    kindKey("Shift+M", [this] { session_->marqueeKind = session_->marqueeKind == MarqueeKind::Rectangle ? MarqueeKind::Ellipse : MarqueeKind::Rectangle; session_->selectTool(Tool::Marquee); emit session_->toolChanged(); });
    kindKey("Shift+L", [this] { session_->lassoKind = session_->lassoKind == LassoKind::Freehand ? LassoKind::Polygonal : LassoKind::Freehand; canvas_->cancelLasso(); session_->selectTool(Tool::Lasso); emit session_->toolChanged(); });
    kindKey("Shift+U", [this] { session_->selectTool(Tool::Shape); session_->toggleShapeKind(); });
    // Photoshop's Shift+letter tool switch: the next tool of the letter's group (from another tool, the one after
    // the group's last used). Groups of one tool select it; tools that hold a group as kinds step the kind, as
    // Shift+M, Shift+L and Shift+U above do.
    for (Tool t : {Tool::Scribble, Tool::Slice, Tool::Artboard, Tool::PaintBucket}) toolActions_[t]->setShortcut(QKeySequence());
    auto cycleTools = [this](std::vector<Tool> tools) {
        const Tool current = session_->tool();
        auto at = std::find(tools.begin(), tools.end(), current);
        if (at == tools.end()) at = std::find(tools.begin(), tools.end(), groupLast_.value(toolGroupKey(tools.front()), tools.front()));
        const Tool next = at == tools.end() ? tools.front() : *(++at == tools.end() ? tools.begin() : at);
        toolActions_[next]->trigger();
    };
    kindKey("Shift+V", [cycleTools] { cycleTools({Tool::Move, Tool::Artboard}); });
    kindKey("Shift+W", [cycleTools] { cycleTools({Tool::Wand, Tool::Scribble}); });
    kindKey("Shift+C", [cycleTools] { cycleTools({Tool::Crop, Tool::Slice}); });
    kindKey("Shift+G", [cycleTools] { cycleTools({Tool::Gradient, Tool::PaintBucket}); });
    kindKey("Shift+P", [this] { toolActions_[Tool::Pen]->trigger(); });
    kindKey("Shift+T", [this] { toolActions_[Tool::Text]->trigger(); });
    // The J group: Spot Healing (its three types), Healing Brush, Patch, Content-Aware Move.
    kindKey("Shift+J", [this] {
        int& mode = session_->spotHealingMode;
        if (mode <= 2) { spotHealingType_ = mode; mode = 3; }
        else mode = mode >= 5 ? spotHealingType_ : mode + 1;
        toolActions_[Tool::SpotHealing]->trigger();
        emit session_->toolChanged();
    });
    // The O group: Dodge, Burn, Sponge.
    kindKey("Shift+O", [this] {
        session_->toning.kind = compositor::ToningKind((int(session_->toning.kind) + 1) % 3);
        toolActions_[Tool::Dodge]->trigger();
        emit session_->toolChanged();
    });
    // The R tool's modes in the order of Photoshop's Blur, Sharpen, Smudge group, then Liquify.
    kindKey("Shift+R", [this] {
        static const BlurToolMode order[] = {BlurToolMode::Blur, BlurToolMode::Sharpen, BlurToolMode::Smudge, BlurToolMode::Liquify};
        const auto at = std::find(std::begin(order), std::end(order), session_->blurMode);
        session_->blurMode = at == std::end(order) || at + 1 == std::end(order) ? order[0] : *(at + 1);
        toolActions_[Tool::Smudge]->trigger();
        emit session_->toolChanged();
    });
}

void MainWindow::updateColorSwatches() {
    // Document values, shown through the monitor profile (the identity when none is known).
    const compositor::ColorProfile profile = session_->hasDocument() ? session_->document()->profile : compositor::ColorProfile{};
    swatches_->setColors(color::displayColor(session_->foregroundColor, profile), color::displayColor(session_->backgroundColor, profile));
}

void MainWindow::chooseColor(bool background) {
    QColor current = background ? session_->backgroundColor : session_->foregroundColor;
    QColor c = QColorDialog::getColor(current, this, background ? tr("Background Colour") : tr("Foreground Colour"));
    if (!c.isValid()) return;
    (background ? session_->backgroundColor : session_->foregroundColor) = c;
    updateColorSwatches();
    recordAction("colors.set", {{background ? "background" : "foreground", c.name()}});
}

void MainWindow::buildMenus() {
    // A document action names the supports() feature it is; without one it is 8-bit only (greyed in a 16-bit document).
    auto needsDocument = [this](QAction* a, const char* feature = nullptr) { documentActions_ << a; if (feature) actionFeatures_[a] = QString::fromLatin1(feature); return a; };
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&New…"), QKeySequence::New, this, &MainWindow::newDocument);
    file->addAction(tr("&Open…"), QKeySequence::Open, this, &MainWindow::openFiles);
    file->addAction(tr("Open Project…"), this, &MainWindow::openProject);
    recentMenu_ = file->addMenu(tr("Open &Recent"));
    file->addAction(tr("Import &File…"), QKeySequence("Ctrl+Shift+O"), this, &MainWindow::importFiles);
    file->addAction(tr("Import &Brushes…"), this, [this] { importBrushesInteractively(this, session_); });
    file->addAction(tr("I&mport Presets…"), this, [this] { importPresetsInteractively(this, session_); });
    needsDocument(file->addAction(tr("Place &Embedded…"), this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Place Embedded"), QSettings().value("lastDir").toString(),
                                                          tr("Images, Photoshop and Affinity documents (*.psd *.psb *.afphoto *.afdesign *.afpub *.af *.png *.jpg *.jpeg *.tif *.tiff *.webp *.bmp *.gif %1)").arg(compositor::rawSupported() ? QStringLiteral("*.cr2 *.cr3 *.crw *.nef *.nrw *.arw *.srf *.sr2 *.raf *.orf *.rw2 *.rwl *.pef *.dng *.3fr *.iiq *.erf *.kdc *.dcr *.mrw *.srw *.x3f") : QString()));
        if (path.isEmpty()) return;
        QSettings().setValue("lastDir", QFileInfo(path).path());
        QString error;
        if (!session_->placeEmbedded(path, &error)) showError(tr("Couldn’t place %1").arg(QFileInfo(path).fileName()), error);
    }), "edit.smartObject");
    file->addSeparator();
    needsDocument(file->addAction(tr("&Save"), QKeySequence::Save, this, [this] { save(false); }), "document.save");
    needsDocument(file->addAction(tr("Save &As…"), QKeySequence::SaveAs, this, [this] { save(true); }), "document.save");
    file->addSeparator();
    needsDocument(file->addAction(tr("Export as Photoshop &Document (PSD)…"), this, &MainWindow::exportPsd), "export.psd");
    needsDocument(file->addAction(tr("Export &PNG…"), QKeySequence("Ctrl+Shift+E"), this, &MainWindow::exportPng), "export.png");
    needsDocument(file->addAction(tr("Export &JPEG…"), QKeySequence("Ctrl+Alt+Shift+S"), this, &MainWindow::exportJpeg), "export.jpeg");
    needsDocument(file->addAction(tr("Export S&VG…"), this, &MainWindow::exportSvg), "export.svg");
    if (canWriteImageFormat("webp")) needsDocument(file->addAction(tr("Export &WebP…"), this, &MainWindow::exportWebp), "export.webp");
    if (canWriteImageFormat("tiff")) needsDocument(file->addAction(tr("Export &TIFF…"), this, &MainWindow::exportTiff), "export.tiff");
    needsDocument(file->addAction(tr("Export T&GA…"), this, &MainWindow::exportTga), "export.tga");
    needsDocument(file->addAction(tr("Export &Icon (ICO)…"), this, &MainWindow::exportIco), "export.ico");
    needsDocument(file->addAction(tr("Export Artboards to Files…"), this, [this] { exportBoxes(false); }), "export.artboards");
    needsDocument(file->addAction(tr("Export S&lices…"), this, [this] { exportBoxes(true); }), "export.slices");
    needsDocument(file->addAction(tr("E&xport Animated GIF…"), this, &MainWindow::exportGif), "export.gif");
    file->addSeparator();
    QMenu* automate = file->addMenu(tr("A&utomate"));
    automate->addAction(tr("&Batch…"), this, [this] { showBatchDialog(); });
    file->addSeparator();
    file->addAction(tr("&Close Tab"), QKeySequence::Close, this, [this] { closeTab(current_); });
    file->addAction(tr("New Tab"), QKeySequence::AddTab, this, [this] { addTab(false); });
    file->addAction(tr("Next Tab"), QKeySequence("Ctrl+Tab"), this, [this] { if (tabs_.size() > 1) switchTo((current_ + 1) % int(tabs_.size())); });
    file->addAction(tr("Previous Tab"), QKeySequence("Ctrl+Shift+Tab"), this, [this] { if (tabs_.size() > 1) switchTo((current_ + int(tabs_.size()) - 1) % int(tabs_.size())); });
    file->addAction(tr("&Quit"), QKeySequence::Quit, this, &QWidget::close);

    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    undoAction_ = edit->addAction(tr("&Undo"), QKeySequence::Undo, this, [this] { session_->undo(); });
    redoAction_ = edit->addAction(tr("&Redo"), QKeySequence("Ctrl+Shift+Z"), this, [this] { session_->redo(); });
    edit->addSeparator();
    needsDocument(edit->addAction(tr("Cu&t"), QKeySequence::Cut, this, [this] { session_->cutSelection(); }), "edit.clipboard");
    needsDocument(edit->addAction(tr("&Copy"), QKeySequence::Copy, this, [this] {
        // As Photoshop: with no selection, the selected layers themselves (pasted whole in any document).
        if (!session_->document()->selection && !session_->selectedLayerIds().empty() && session_->copyLayers()) return;
        session_->copySelection();
    }), "edit.clipboard");
    needsDocument(edit->addAction(tr("Copy &Merged"), QKeySequence("Ctrl+Shift+C"), this, [this] { session_->copyMerged(); }), "edit.clipboard");
    needsDocument(edit->addAction(tr("&Paste"), QKeySequence::Paste, this, [this] { session_->paste(); }), "edit.clipboard");
    edit->addSeparator();
    needsDocument(edit->addAction(tr("&Free Transform"), QKeySequence("Ctrl+T"), this, [this] { session_->transformCommand(); }), "layers.transform");
    needsDocument(edit->addAction(tr("&Warp…"), this, [this] { WarpDialog(session_, this).exec(); }), "edit.distort");
    needsDocument(edit->addAction(tr("Warp Ca&ge"), this, [this] {
        QString error;
        if (!session_->beginWarpCage(&error)) showError(tr("Warp Cage"), error);
        else statusBar()->showMessage(tr("Drag the cage's points; Enter applies, Esc cancels."), 8000);
    }), "edit.distort");
    needsDocument(edit->addAction(tr("Fill with Foreground"), QKeySequence("Alt+Backspace"), this, [this] { session_->fillSelection(session_->foregroundColor); recordAction("pixels.fill", {{"color", session_->foregroundColor.name()}}); }), "edit.fill");
    needsDocument(edit->addAction(tr("Fill with Background"), QKeySequence("Ctrl+Backspace"), this, [this] { session_->fillSelection(session_->backgroundColor); recordAction("pixels.fill", {{"color", session_->backgroundColor.name()}}); }), "edit.fill");
    QAction* clear = needsDocument(edit->addAction(tr("Clear"), QKeySequence(Qt::Key_Delete), this, [this] {
        if (session_->document() && session_->document()->selection) { session_->clearSelectionPixels(); recordAction("pixels.clear"); }
        else { deleteSelectedLayers(); recordAction("layers.delete"); }
    }), "layers.structure");
    clear->setShortcuts({QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace)});
    needsDocument(edit->addAction(tr("Content-Aware Fill…"), QKeySequence("Shift+F5"), this, [this] {
        if (!session_->canAdjustPixels() || !session_->document()->selection || !session_->document()->selection->coverage) { showError(tr("Content-Aware Fill"), tr("Select a visible image layer and an area to fill.")); return; }
        (new ContentFillDialog(session_, this))->show();
    }), "edit.fill");
    needsDocument(edit->addAction(tr("Content-Aware Scale…"), QKeySequence("Ctrl+Alt+Shift+C"), this, [this] { (new ContentAwareScaleDialog(session_, this))->show(); }), "edit.contentAware");

    // Colour management (docs/color-management.md), as in Photoshop's Edit menu.
    edit->addSeparator();
    edit->addAction(tr("Color Settings…"), QKeySequence("Ctrl+Shift+K"), this, [this] { color::showColorSettings(this); });
    auto profileKey = [](const compositor::ColorProfile& p) -> QString {
        if (p.empty()) return QStringLiteral("none");
        auto s = compositor::matchingWorkingSpace(p);
        return s && p == compositor::builtinProfile(*s) ? QString::fromLatin1(compositor::workingSpaceKey(*s)) : QString();
    };
    needsDocument(edit->addAction(tr("Assign Profile…"), this, [this, profileKey] {
        auto profile = color::askAssignProfile(this, session_->document()->profile);
        if (!profile || !session_->assignProfile(*profile)) return;
        if (const QString key = profileKey(*profile); !key.isEmpty()) recordAction("document.profile", {{"action", "assign"}, {"profile", key}});
    }), "document.profile");
    needsDocument(edit->addAction(tr("Convert to Profile…"), this, [this, profileKey] {
        auto choice = color::askConvertProfile(this, session_->document()->profile);
        if (!choice) return;
        QString error;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const bool ok = session_->convertToProfile(choice->profile, choice->options, &error);
        QApplication::restoreOverrideCursor();
        if (!ok) { showError(tr("Convert to Profile"), error); return; }
        if (const QString key = profileKey(choice->profile); !key.isEmpty())
            recordAction("document.profile", {{"action", "convert"}, {"profile", key}, {"intent", QString::fromLatin1(compositor::renderingIntentKey(choice->options.intent))},
                                              {"blackPointCompensation", choice->options.blackPointCompensation}});
    }), "document.profile");

    edit->addSeparator();
    edit->addAction(tr("Prefere&nces…"), QKeySequence::Preferences, this, &MainWindow::showPreferences);

    QMenu* image = menuBar()->addMenu(tr("&Image"));
    // Photoshop's Image > Mode: the document's bits per channel (docs/bit-depth.md).
    QMenu* mode = image->addMenu(tr("&Mode"));
    needsDocument(mode->menuAction(), "document.mode");
    auto* depths = new QActionGroup(this);
    mode8Action_ = needsDocument(mode->addAction(tr("&8 Bits/Channel"), this, [this] { convertMode(SampleType::U8); }), "document.mode");
    mode16Action_ = needsDocument(mode->addAction(tr("&16 Bits/Channel"), this, [this] { convertMode(SampleType::U16); }), "document.mode");
    for (QAction* a : {mode8Action_, mode16Action_}) { a->setCheckable(true); depths->addAction(a); }
    mode8Action_->setChecked(true);
    image->addSeparator();
    needsDocument(image->addAction(tr("&Canvas Size…"), QKeySequence("Ctrl+Alt+C"), this, [this] {
        auto o = askCanvasSize(this, session_->document()->width, session_->document()->height);
        if (!o) return;
        session_->resizeCanvas(o->width, o->height, o->anchorX, o->anchorY);
        recordAction("canvas.resize", {{"width", o->width}, {"height", o->height}, {"anchorX", o->anchorX}, {"anchorY", o->anchorY}});
    }), "canvas.size");
    needsDocument(image->addAction(tr("&Image Size…"), QKeySequence("Ctrl+Alt+I"), this, [this] {
        auto o = askImageSize(this, session_->document()->width, session_->document()->height, session_->document()->resolution);
        if (!o) return;
        session_->resizeImage(o->width, o->height, o->resolution, o->sampling);
        recordAction("image.resize", {{"width", o->width}, {"height", o->height}, {"resolution", o->resolution},
                                      {"sampling", o->sampling == 0 ? "nearest" : o->sampling == 1 ? "smooth" : "high"}});
    }), "edit.imageSize");
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
        if (!session_->trim(o)) { showError(tr("Trim"), tr("There is nothing to trim: the canvas already ends at its content, or nothing would remain.")); return; }
        static const char* const bases[] = {"transparent", "topLeft", "bottomRight"};
        recordAction("image.trim", {{"basedOn", bases[std::clamp(basedOn->currentIndex(), 0, 2)]}, {"top", o.top}, {"left", o.left}, {"bottom", o.bottom}, {"right", o.right}, {"tolerance", int(o.tolerance)}});
    }), "edit.crop");
    needsDocument(image->addAction(tr("Crop to Selection"), this, [this] {
        const auto& d = session_->document();
        if (d && d->selection) {
            Rect b = d->selection->bounds();
            if (!b.isEmpty()) {
                session_->cropTo(QRectF(b.x, b.y, b.width, b.height));
                session_->deselect();
                recordAction("canvas.crop", {{"x", b.x}, {"y", b.y}, {"width", b.width}, {"height", b.height}});
                recordAction("selection.none");
            }
        }
    }), "edit.crop");
    image->addSeparator();
    QMenu* adjustments = image->addMenu(tr("&Adjustments"));
    auto pixelAdjustment = [this, adjustments, &needsDocument](const QString& label, const QKeySequence& key, AdjustmentKind kind) {
        needsDocument(adjustments->addAction(label, key, this, [this, kind] {
            if (session_->smartObjectBlocksPixels(true)) return;
            if (!session_->canAdjustPixels()) { showError(tr("Adjustments"), tr("Select a visible image layer (not a mask) to adjust its pixels.")); return; }
            (new PixelAdjustmentDialog(session_, kind, this))->show();
        }), (std::string("adjustment.") + adjustmentKindName(kind)).c_str());
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
    needsDocument(adjustments->addAction(tr("&Invert"), QKeySequence("Ctrl+I"), this, [this] { session_->invertActive(); recordAction("pixels.invert"); }), "adjustment.Invert");
    image->addSeparator();
    needsDocument(image->addAction(tr("Flip Canvas Horizontal"), this, [this] { session_->flipCanvas(true); recordAction("canvas.flip"); }), "canvas.flip");
    needsDocument(image->addAction(tr("Flip Canvas Vertical"), this, [this] { session_->flipCanvas(false); recordAction("canvas.flip", {{"vertical", true}}); }), "canvas.flip");

    QMenu* layer = menuBar()->addMenu(tr("&Layer"));
    needsDocument(layer->addAction(tr("&New Layer"), QKeySequence("Ctrl+Shift+N"), this, [this] { session_->addBlankLayer(); recordAction("layers.add"); }), "layers.structure");
    needsDocument(layer->addAction(tr("New Layer &Below"), this, [this] { session_->addBlankLayer(true); recordAction("layers.add", {{"below", true}}); }), "layers.structure");
    needsDocument(layer->addAction(tr("New &Folder"), this, [this] { session_->addGroup(); recordAction("layers.add", {{"kind", "group"}}); }), "layers.structure");
    needsDocument(layer->addAction(tr("&Group Layers"), QKeySequence("Ctrl+G"), this, [this] { session_->groupSelectedLayers(); recordAction("layers.group"); }), "layers.structure");
    needsDocument(layer->addAction(tr("Layer via &Copy"), QKeySequence("Ctrl+J"), this, [this] { session_->layerViaCopy(); }), "edit.clipboard");
    needsDocument(layer->addAction(tr("&Duplicate Layer"), this, [this] { session_->duplicateActiveLayer(); recordAction("layers.duplicate"); }), "layers.structure");
    needsDocument(layer->addAction(tr("De&lete Layer"), this, [this] { deleteSelectedLayers(); recordAction("layers.delete"); }), "layers.structure");
    mergeAction_ = needsDocument(layer->addAction(tr("Merge Do&wn"), QKeySequence("Ctrl+E"), this, [this] { session_->mergeLayers(); recordAction("layers.merge"); }), "layers.merge");
    editTextAction_ = needsDocument(layer->addAction(tr("Edit &Text…"), this, [this] { const Layer* l = session_->activeLayer(); if (l && l->isLiveText()) session_->requestTextEdit(l->id); }), "edit.text");
    needsDocument(layer->addAction(tr("&Rename Layer…"), this, [this] {
        const Layer* active = session_->activeLayer();
        if (!active) return;
        bool ok;
        QString name = QInputDialog::getText(this, tr("Rename Layer"), tr("Name"), QLineEdit::Normal, QString::fromStdString(active->name), &ok);
        if (ok) session_->renameLayer(active->id, name);
    }), "layers.structure");
    needsDocument(layer->addAction(tr("Move &Out of Folder"), QKeySequence("Ctrl+Shift+["), this, [this] { session_->moveActiveLayerOutOfGroup(); }), "layers.structure");
    QMenu* adjustmentLayers = layer->addMenu(tr("New &Adjustment Layer"));
    for (int i = 0; i < adjustmentKindCount; i++) {
        AdjustmentKind kind = AdjustmentKind(i);
        needsDocument(adjustmentLayers->addAction(names::adjustmentKind(kind), this, [this, kind] { session_->addAdjustmentLayer(kind); recordAction("layers.add", {{"kind", "adjustment"}, {"adjustmentKind", QString::fromUtf8(adjustmentKindName(kind))}}); }),
                      (std::string("adjustment.") + adjustmentKindName(kind)).c_str());
    }
    QMenu* styles = layer->addMenu(tr("Layer St&yle"));
    const char* const stylePages[] = {QT_TRANSLATE_NOOP("app::MainWindow", "Blending Options…"), QT_TRANSLATE_NOOP("app::MainWindow", "Bevel & Emboss…"), QT_TRANSLATE_NOOP("app::MainWindow", "Stroke…"), QT_TRANSLATE_NOOP("app::MainWindow", "Inner Shadow…"), QT_TRANSLATE_NOOP("app::MainWindow", "Inner Glow…"), QT_TRANSLATE_NOOP("app::MainWindow", "Satin…"), QT_TRANSLATE_NOOP("app::MainWindow", "Color Overlay…"),
                                      QT_TRANSLATE_NOOP("app::MainWindow", "Gradient Overlay…"), QT_TRANSLATE_NOOP("app::MainWindow", "Pattern Overlay…"), QT_TRANSLATE_NOOP("app::MainWindow", "Outer Glow…"), QT_TRANSLATE_NOOP("app::MainWindow", "Drop Shadow…")};
    for (int page = 0; page < int(std::size(stylePages)); page++) {
        needsDocument(styles->addAction(tr(stylePages[page]), this, [this, page] {
            if (session_->activeLayerId()) LayerStyleDialog(session_, *session_->activeLayerId(), this, page).exec();
        }), "edit.style");
        if (page == 0) styles->addSeparator();
    }
    styles->addSeparator();
    needsDocument(styles->addAction(tr("&Copy Layer Style"), this, [this] { session_->copyLayerStyle(); }), "edit.style");
    needsDocument(styles->addAction(tr("&Paste Layer Style"), this, [this] { session_->pasteLayerStyle(); }), "edit.style");
    needsDocument(styles->addAction(tr("C&lear Layer Style"), this, [this] { session_->clearLayerStyle(); }), "edit.style");
    styles->addSeparator();
    // Imported style presets (.asl): the submenu lists the library as it is when it opens.
    QMenu* applyStyle = styles->addMenu(tr("&Apply Style"));
    needsDocument(applyStyle->menuAction(), "edit.style");
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
    QMenu* smart = layer->addMenu(tr("Smart Ob&jects"));
    needsDocument(smart->addAction(tr("&Convert to Smart Object"), this, [this] {
        QString error;
        if (!session_->convertToSmartObject(&error) && !error.isEmpty()) showError(tr("Couldn’t convert to a smart object"), error);
    }), "edit.smartObject");
    needsDocument(smart->addAction(tr("&Edit Contents"), this, [this] { editSmartObjectContents(); }), "edit.smartObject");
    needsDocument(smart->addAction(tr("&Replace Contents…"), this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Replace Contents"), QSettings().value("lastDir").toString(),
                                                          tr("Images, Photoshop and Affinity documents (*.psd *.psb *.afphoto *.afdesign *.afpub *.af *.png *.jpg *.jpeg *.tif *.tiff *.webp *.bmp *.gif %1)").arg(compositor::rawSupported() ? QStringLiteral("*.cr2 *.cr3 *.crw *.nef *.nrw *.arw *.srf *.sr2 *.raf *.orf *.rw2 *.rwl *.pef *.dng *.3fr *.iiq *.erf *.kdc *.dcr *.mrw *.srw *.x3f") : QString()));
        if (path.isEmpty()) return;
        QString error;
        if (!session_->replaceSmartObjectContents(path, &error)) showError(tr("Couldn’t replace the contents"), error);
    }), "edit.smartObject");
    needsDocument(smart->addAction(tr("R&asterize"), this, [this] { session_->rasterizeSmartObject(); }), "edit.smartObject");
    layer->addSeparator();
    QMenu* mask = layer->addMenu(tr("Layer &Mask"));
    needsDocument(mask->addAction(tr("Reveal All"), this, [this] { session_->addLayerMask(true); recordAction("layers.mask", {{"action", "add"}}); }), "layers.mask");
    needsDocument(mask->addAction(tr("Hide All"), this, [this] { session_->addLayerMask(false); recordAction("layers.mask", {{"action", "add"}, {"revealing", false}}); }), "layers.mask");
    needsDocument(mask->addAction(tr("From Selection (Reveal)"), this, [this] { session_->addMaskFromSelection(true); recordAction("layers.mask", {{"action", "addFromSelection"}}); }), "edit.selection");
    needsDocument(mask->addAction(tr("From Selection (Hide)"), this, [this] { session_->addMaskFromSelection(false); recordAction("layers.mask", {{"action", "addFromSelection"}, {"revealing", false}}); }), "edit.selection");
    mask->addSeparator();
    needsDocument(mask->addAction(tr("Enable / Disable"), this, [this] { session_->toggleLayerMask(); recordAction("layers.mask", {{"action", "toggle"}}); }), "layers.mask");
    needsDocument(mask->addAction(tr("Invert"), this, [this] { session_->invertMask(); recordAction("layers.mask", {{"action", "invert"}}); }), "layers.mask");
    needsDocument(mask->addAction(tr("Apply"), this, [this] { session_->applyMask(); recordAction("layers.mask", {{"action", "apply"}}); }), "layers.applyMask");
    needsDocument(mask->addAction(tr("Delete"), this, [this] { session_->deleteLayerMask(); recordAction("layers.mask", {{"action", "delete"}}); }), "layers.mask");
    // Photoshop's Layer > Vector Mask: a path that cuts the layer, edited with the Pen and Direct Selection.
    QMenu* vectorMask = layer->addMenu(tr("&Vector Mask"));
    auto addVector = [this](EditorSession::VectorMaskKind kind) {
        QString error;
        if (!session_->addVectorMask(kind, &error) && !error.isEmpty()) QMessageBox::information(this, tr("Vector Mask"), error);
    };
    needsDocument(vectorMask->addAction(tr("Reveal All"), this, [addVector] { addVector(EditorSession::VectorMaskKind::RevealAll); }), "edit.vector");
    needsDocument(vectorMask->addAction(tr("Hide All"), this, [addVector] { addVector(EditorSession::VectorMaskKind::HideAll); }), "edit.vector");
    needsDocument(vectorMask->addAction(tr("Current Path"), this, [addVector] { addVector(EditorSession::VectorMaskKind::CurrentPath); }), "edit.vector");
    vectorMask->addSeparator();
    needsDocument(vectorMask->addAction(tr("Edit"), this, [this] { if (session_->activeLayerId()) session_->targetVectorMask(*session_->activeLayerId()); }), "edit.vector");
    needsDocument(vectorMask->addAction(tr("Delete"), this, [this] { session_->deleteVectorMask(); }), "edit.vector");
    needsDocument(layer->addAction(tr("Create / Release Cl&ipping Mask"), QKeySequence("Ctrl+Alt+G"), this, [this] { if (session_->activeLayerId()) session_->toggleClippingMask(*session_->activeLayerId()); }), "layers.structure");
    layer->addSeparator();
    needsDocument(layer->addAction(tr("Bring Forward"), QKeySequence("Ctrl+]"), this, [this] { session_->moveActiveLayer(1); }), "layers.structure");
    needsDocument(layer->addAction(tr("Send Backward"), QKeySequence("Ctrl+["), this, [this] { session_->moveActiveLayer(-1); }), "layers.structure");
    layer->addSeparator();
    needsDocument(layer->addAction(tr("Flip Layer Horizontal"), this, [this] { session_->flipLayer(true); recordAction("layers.flip"); }), "layers.transform");
    needsDocument(layer->addAction(tr("Flip Layer Vertical"), this, [this] { session_->flipLayer(false); recordAction("layers.flip", {{"vertical", true}}); }), "layers.transform");
    QMenu* sampling = layer->addMenu(tr("Resampling"));
    needsDocument(sampling->addAction(tr("High Quality"), this, [this] { session_->setLayerSampling(Sampling::High); }), "layers.structure");
    needsDocument(sampling->addAction(tr("Smooth"), this, [this] { session_->setLayerSampling(Sampling::Smooth); }), "layers.structure");
    needsDocument(sampling->addAction(tr("Nearest Neighbour"), this, [this] { session_->setLayerSampling(Sampling::Nearest); }), "layers.structure");

    // Photoshop's Type menu: the active text layer's outlines as a path or a shape.
    QMenu* type = menuBar()->addMenu(tr("&Type"));
    auto fromText = [this](bool shape) {
        QString error;
        const auto id = session_->activeLayerId();
        const bool ok = id && (shape ? session_->textToShape(*id, &error) : session_->textToWorkPath(*id, &error));
        if (!ok) QMessageBox::information(this, shape ? tr("Convert to Shape") : tr("Create Work Path"), error.isEmpty() ? tr("Choose a text layer first.") : error);
    };
    needsDocument(type->addAction(tr("Create &Work Path"), this, [fromText] { fromText(false); }), "edit.text");
    needsDocument(type->addAction(tr("Convert to &Shape"), this, [fromText] { fromText(true); }), "edit.paint");

    // Everything about the selection in one place, as Photoshop's Select menu: the whole-canvas commands,
    // then Modify, then loading a layer's pixels or mask as the selection.
    QMenu* select = menuBar()->addMenu(tr("&Select"));
    needsDocument(select->addAction(tr("&All"), QKeySequence::SelectAll, this, [this] { session_->selectAll(); recordAction("selection.all"); }), "edit.selection");
    needsDocument(select->addAction(tr("&Deselect"), QKeySequence("Ctrl+D"), this, [this] { session_->deselect(); recordAction("selection.none"); }), "edit.selection");
    needsDocument(select->addAction(tr("&Inverse"), QKeySequence("Ctrl+Shift+I"), this, [this] { session_->invertSelection(); recordAction("selection.invert"); }), "edit.selection");
    needsDocument(select->addAction(tr("Edit in &Quick Mask Mode"), QKeySequence("Q"), this, [this] { session_->toggleQuickMask(); }), "edit.selection");
    select->addSeparator();
    QMenu* modify = select->addMenu(tr("&Modify"));
    needsDocument(modify->addAction(tr("&Expand…"), this, [this] { bool ok; int n = QInputDialog::getInt(this, tr("Expand Selection"), tr("Pixels"), 1, 1, 500, 1, &ok); if (ok) { session_->selectionExpand(n); recordAction("selection.grow", {{"amount", n}}); } }), "edit.selection");
    needsDocument(modify->addAction(tr("&Contract…"), this, [this] { bool ok; int n = QInputDialog::getInt(this, tr("Contract Selection"), tr("Pixels"), 1, 1, 500, 1, &ok); if (ok) { session_->selectionContract(n); recordAction("selection.grow", {{"amount", -n}}); } }), "edit.selection");
    needsDocument(modify->addAction(tr("&Feather…"), QKeySequence("Shift+F6"), this, [this] { bool ok; double r = QInputDialog::getDouble(this, tr("Feather Selection"), tr("Radius (pixels)"), 5, 0.1, 250, 1, &ok); if (ok) { session_->selectionFeather(r); recordAction("selection.feather", {{"radius", r}}); } }), "edit.selection");
    needsDocument(modify->addAction(tr("&Smooth…"), this, [this] { bool ok; int n = QInputDialog::getInt(this, tr("Smooth Selection"), tr("Sample radius (pixels)"), 3, 1, 100, 1, &ok); if (ok) { session_->selectionSmooth(n); recordAction("selection.smooth", {{"radius", n}}); } }), "edit.selection");
    needsDocument(modify->addAction(tr("&Border…"), this, [this] { bool ok; int n = QInputDialog::getInt(this, tr("Border Selection"), tr("Width (pixels)"), 4, 1, 200, 1, &ok); if (ok) { session_->selectionBorder(n); recordAction("selection.border", {{"width", n}}); } }), "edit.selection");
    select->addSeparator();
    QMenu* load = select->addMenu(tr("&Load as Selection"));
    needsDocument(load->addAction(tr("Layer Pixels"), this, [this] { if (session_->activeLayerId()) { session_->loadLayerAsSelection(*session_->activeLayerId(), false, SelectionMode::Replace); recordAction("selection.fromLayer"); } }), "edit.selection");
    needsDocument(load->addAction(tr("Layer Mask"), this, [this] { if (session_->activeLayerId()) { session_->loadLayerAsSelection(*session_->activeLayerId(), true, SelectionMode::Replace); recordAction("selection.fromLayer", {{"mask", true}}); } }), "edit.selection");
    load->addSeparator();
    needsDocument(load->addAction(tr("Add Layer Pixels"), this, [this] { if (session_->activeLayerId()) session_->loadLayerAsSelection(*session_->activeLayerId(), false, SelectionMode::Add); }), "edit.selection");
    needsDocument(load->addAction(tr("Subtract Layer Pixels"), this, [this] { if (session_->activeLayerId()) session_->loadLayerAsSelection(*session_->activeLayerId(), false, SelectionMode::Subtract); }), "edit.selection");
    needsDocument(load->addAction(tr("Intersect with Layer Pixels"), this, [this] { if (session_->activeLayerId()) session_->loadLayerAsSelection(*session_->activeLayerId(), false, SelectionMode::Intersect); }), "edit.selection");
    // Selections kept as alpha channels (the Channels panel; docs/channels.md).
    needsDocument(select->addAction(tr("Load Selection…"), this, [this] { (new LoadSelectionDialog(session_, this))->open(); }), "edit.channels");
    needsDocument(select->addAction(tr("Save Selection…"), this, [this] {
        if (!session_->document()->selection) { showError(tr("Save Selection"), tr("Make a selection first.")); return; }
        (new SaveSelectionDialog(session_, this))->open();
    }), "edit.channels");
    // Photoshop's channel keys: Ctrl+2 the composite, Ctrl+3, 4, 5 red, green and blue, Ctrl+6 to 9 the first four alpha
    // channels; with Alt, the channel is loaded as a selection instead.
    auto channelKey = [this](int n, bool load) {
        if (!session_->hasDocument()) return;
        const auto& channels = session_->document()->channels;
        if (n >= 6 && size_t(n - 6) >= channels.size()) return;
        if (load) {
            SelectionSource source;
            source.kind = n == 2 ? SelectionSource::Composite : n == 3 ? SelectionSource::Red : n == 4 ? SelectionSource::Green : n == 5 ? SelectionSource::Blue : SelectionSource::AlphaChannel;
            if (n >= 6) source.id = channels[size_t(n - 6)].id;
            session_->loadSelectionFromSource(source, false, SelectionMode::Replace);
        } else if (n <= 5) session_->selectColorChannels(n == 2 ? colorChannelsAll : 1u << (n - 3));
        else session_->selectAlphaChannel(channels[size_t(n - 6)].id);
    };
    for (int n = 2; n <= 9; n++)
        for (bool load : {false, true}) {
            auto* key = needsDocument(new QAction(load ? tr("Load Channel %1 as Selection").arg(n - 1) : tr("Select Channel %1").arg(n - 1), this), "edit.channels");
            key->setShortcut(QKeySequence(QString::fromLatin1(load ? "Ctrl+Alt+%1" : "Ctrl+%1").arg(n)));
            connect(key, &QAction::triggered, this, [channelKey, n, load] { channelKey(n, load); });
            addAction(key);
        }

    QMenu* filter = menuBar()->addMenu(tr("Filte&r"));
    auto filterAction = [this, filter, &needsDocument](const QString& label, FilterKind kind) {
        needsDocument(filter->addAction(label, this, [this, kind] {
            // On a smart object the blurs and noise go on as Smart Filters, as in Photoshop.
            if (kind != FilterKind::LensCorrection && session_->canAddSmartFilter()) { (new FilterDialog(session_, kind, this, true))->show(); return; }
            if (session_->smartObjectBlocksPixels(true)) return;
            if (!session_->canAdjustPixels()) { showError(tr("Filters"), tr("Select a visible image layer (not a mask) to filter its pixels.")); return; }
            (new FilterDialog(session_, kind, this))->show();
        }), (std::string("filter.") + filterKindName(kind)).c_str());
    };
    filterAction(tr("&Gaussian Blur…"), FilterKind::GaussianBlur);
    filterAction(tr("&Motion Blur…"), FilterKind::MotionBlur);
    filterAction(tr("Add &Noise…"), FilterKind::AddNoise);
    filterAction(tr("&Lens Correction…"), FilterKind::LensCorrection);
    filter->addSeparator();
    // OpenMosh's glitch, distortion and retro effects (docs/mosh.md), a submenu per category.
    QMenu* moshMenu = filter->addMenu(tr("M&osh"));
    for (int c = 0; c < compositor::mosh::categoryCount; c++) {
        const auto category = compositor::mosh::Category(c);
        QMenu* sub = nullptr;
        for (const compositor::mosh::EffectSpec& spec : compositor::mosh::effects()) {
            if (spec.category != category) continue;
            if (!sub) sub = moshMenu->addMenu(names::mosh(compositor::mosh::categoryName(category)));
            const compositor::mosh::EffectSpec* effect = &spec;
            needsDocument(sub->addAction(tr("%1…").arg(names::mosh(spec.name)), this, [this, effect] {
                if (session_->smartObjectBlocksPixels(true)) return;
                if (!session_->canAdjustPixels()) { showError(tr("Filters"), tr("Select a visible image layer (not a mask) to filter its pixels.")); return; }
                (new MoshDialog(session_, *effect, this))->show();
            }), "filter.Mosh");
        }
    }
    // Photoshop's shortcut. A destructive filter here: on a smart object it asks first, like the others.
    needsDocument(filter->addAction(tr("Camera &Raw Filter…"), QKeySequence("Shift+Ctrl+A"), this, [this] {
        if (session_->smartObjectBlocksPixels(true)) return;
        if (!session_->canAdjustPixels()) { showError(tr("Camera Raw Filter"), tr("Select a visible image layer (not a mask) to filter its pixels.")); return; }
        (new CameraRawDialog(session_, this))->show();
    }), "filter.Camera Raw");
    filter->addSeparator();
    needsDocument(filter->addAction(tr("&G'MIC…"), QKeySequence("Ctrl+Shift+G"), this, [this] {
        if (session_->smartObjectBlocksPixels(true)) return;
            if (!session_->canAdjustPixels()) { showError(tr("G'MIC"), tr("Select a layer with pixels first.")); return; }
        (new GmicDialog(session_, this))->show();
    }), "filter.G'MIC");
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
    }), "edit.removeBackground");
    refreshBackgroundAction();

    QMenu* view = menuBar()->addMenu(tr("&View"));
    needsDocument(view->addAction(tr("Zoom &In"), QKeySequence::ZoomIn, this, [this] { session_->zoomTo(session_->viewport.zoom * 1.25); }), "view");
    needsDocument(view->addAction(tr("Zoom &Out"), QKeySequence::ZoomOut, this, [this] { session_->zoomTo(session_->viewport.zoom / 1.25); }), "view");
    needsDocument(view->addAction(tr("&Fit on Screen"), QKeySequence("Ctrl+0"), this, [this] { session_->fitView(); }), "view");
    needsDocument(view->addAction(tr("&Actual Pixels"), QKeySequence("Ctrl+1"), this, [this] { session_->zoomTo(1); }), "view");
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
    // Soft proofing (docs/color-management.md): Photoshop's Proof Setup, Proof Colors and Gamut Warning.
    QMenu* proofSetup = view->addMenu(tr("Proof Set&up"));
    proofSetup->addAction(tr("Custom…"), this, [this] { color::showProofSetup(this); });
    QAction* proof = view->addAction(tr("Proof Colo&rs"), QKeySequence("Ctrl+Y"), this, [](bool on) { color::Settings s = color::settings(); s.proofColors = on; color::setSettings(s); });
    QAction* gamut = view->addAction(tr("Gamut Wa&rning"), QKeySequence("Ctrl+Shift+Y"), this, [](bool on) { color::Settings s = color::settings(); s.gamutWarning = on; color::setSettings(s); });
    for (QAction* a : {proof, gamut}) a->setCheckable(true);
    connect(color::notifier(), &color::Notifier::changed, this, [this, proof, gamut] {
        proof->setChecked(color::settings().proofColors);
        gamut->setChecked(color::settings().gamutWarning);
        updateColorSwatches();
    });
    view->addSeparator();
    QAction* grid = view->addAction(tr("Pixel &Grid"), this, [this](bool on) { session_->showsPixelGrid = on; canvas_->update(); });
    grid->setCheckable(true);
    grid->setChecked(true);
    QAction* controls = view->addAction(tr("Transform &Controls"), QKeySequence("Ctrl+H"), this, [this](bool on) { session_->showsTransformControls = on; emit session_->transformChanged(); });
    controls->setCheckable(true);
    controls->setChecked(true);

    // Photoshop's Window menu: every panel.
    QMenu* window = menuBar()->addMenu(tr("&Window"));
    actionsDock_->toggleViewAction()->setText(tr("&Actions"));
    actionsDock_->toggleViewAction()->setShortcut(QKeySequence("Alt+F9"));
    window->addAction(actionsDock_->toggleViewAction());
    window->addAction(adjustDock_->toggleViewAction());
    window->addAction(layersDock_->toggleViewAction());
    channelsDock_->toggleViewAction()->setText(tr("&Channels"));
    window->addAction(channelsDock_->toggleViewAction());
    window->addAction(pathsDock_->toggleViewAction());
    timelineDock_->toggleViewAction()->setText(tr("&Timeline"));
    window->addAction(timelineDock_->toggleViewAction());

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

void MainWindow::convertMode(SampleType type) {
    if (!session_->hasDocument() || session_->sampleType() == type) { refreshDepthGating(); return; }
    QString error;
    if (!session_->convertMode(type, &error)) showError(tr("Mode"), error.isEmpty() ? tr("The document could not be converted.") : error);
    else recordAction("image.mode", {{"bits", type == SampleType::U16 ? 16 : 8}});
    refreshActions();
}

void MainWindow::refreshDepthGating() {
    const bool has = session_ && session_->hasDocument();
    const bool deep = has && session_->sampleType() != SampleType::U8;
    const QString notYet = tr("Not available in 16-bit yet");
    // An action greyed for the depth says why; its own tooltip comes back at 8 bits.
    auto gate = [&](QAction* a, bool allowed, bool enabled) {
        a->setEnabled(enabled && allowed);
        if (!allowed) {
            if (!a->property("depthTip").isValid()) a->setProperty("depthTip", a->toolTip());
            a->setToolTip(notYet);
        } else if (a->property("depthTip").isValid()) {
            a->setToolTip(a->property("depthTip").toString());
            a->setProperty("depthTip", QVariant());
        }
    };
    for (QAction* a : documentActions_) {
        const QString feature = actionFeatures_.value(a);
        gate(a, !deep || (!feature.isEmpty() && session_->supportsFeature(feature.toStdString())), has);
    }
    for (auto it = toolActions_.begin(); it != toolActions_.end(); ++it) gate(it.value(), !deep || session_->toolSupportedAtDepth(it.key()), true);
    gate(eraserAction_, !deep || session_->toolSupportedAtDepth(Tool::Brush), true);
    // Menus show their items' tooltips while something in them is greyed for the depth.
    for (QMenu* menu : menuBar()->findChildren<QMenu*>()) menu->setToolTipsVisible(deep);
    if (mode8Action_) { mode8Action_->setChecked(!deep); mode16Action_->setChecked(deep); }
}


} // namespace app
