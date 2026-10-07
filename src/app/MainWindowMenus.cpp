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
#include "HdrDialogs.h"
#include "ImageConvert.h"
#include "CameraRawDialog.h"
#include "FilterDialog.h"
#include "GridFilters.h"
#include "compositor/smartfilter.h"
#include <random>
#include "MoshDialog.h"
#include "GmicDialog.h"
#include "ColorDialogs.h"
#include "ColorManagement.h"
#include "ColorSwatches.h"
#include "PreferencesDialog.h"
#include "BrushImporter.h"
#include "PresetLibrary.h"
#include "ActionLibrary.h"
#include "ViewOptions.h"
#include <QSignalBlocker>
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
    // Actions the canvas's context menu shows again (MainWindowCanvasMenu.cpp), so their shortcuts and enabled state match.
    auto nameAction = [this](const char* key, QAction* a) { named_[QString::fromLatin1(key)] = a; return a; };
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(tr("&New…"), QKeySequence::New, this, &MainWindow::newDocument);
    file->addAction(tr("&Open…"), QKeySequence::Open, this, &MainWindow::openFiles);
    file->addAction(tr("Open Project Folder…"), this, &MainWindow::openProject);   // a .comp folder; .nekophoto files open with Open
    recentMenu_ = file->addMenu(tr("Open &Recent"));
    file->addAction(tr("Import &File…"), QKeySequence("Ctrl+Shift+O"), this, &MainWindow::importFiles);
    file->addAction(tr("Import &Brushes…"), this, [this] { importBrushesInteractively(this, session_); });
    file->addAction(tr("I&mport Presets…"), this, [this] { importPresetsInteractively(this, session_); });
    needsDocument(file->addAction(tr("Place &Embedded…"), this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Place Embedded"), QSettings().value("lastDir").toString(),
                                                          tr("Images, Photoshop and Affinity documents (*.psd *.psb *.afphoto *.afdesign *.afpub *.af *.png *.jpg *.jpeg *.tif *.tiff *.webp *.bmp *.gif %1)").arg(compositor::rawSupported() ? QStringLiteral("*.cr2 *.cr3 *.crw *.nef *.nrw *.arw *.srf *.sr2 *.raf *.orf *.rw2 *.rwl *.pef *.dng *.3fr *.iiq *.erf *.kdc *.dcr *.mrw *.srw *.x3f") : QString()));
        if (path.isEmpty()) return;
        QSettings().setValue("lastDir", QFileInfo(path).path());
        runCommand("smartObject.place", {{"path", path}}, tr("Couldn’t place %1").arg(QFileInfo(path).fileName()));
    }), "edit.smartObject");
    file->addSeparator();
    needsDocument(file->addAction(tr("&Save"), QKeySequence::Save, this, [this] { save(false); }), "document.save");
    needsDocument(file->addAction(tr("Save &As…"), QKeySequence::SaveAs, this, [this] { save(true); }), "document.save");
    file->addSeparator();
    needsDocument(file->addAction(tr("Export as Photoshop &Document (PSD)…"), this, &MainWindow::exportPsd), "export.psd");
    needsDocument(file->addAction(tr("Export &PNG…"), this, &MainWindow::exportPng), "export.png");
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
    // The pixel clipboard runs pixels.cut, pixels.copy, pixels.copyMerged and pixels.paste (layers.copy and
    // layers.paste for whole layers); with nothing to copy or paste the items do nothing, as before.
    nameAction("edit.cut", needsDocument(edit->addAction(tr("Cu&t"), QKeySequence::Cut, this, [this] {
        if (!session_->document()->selection || !session_->canCopyPixels()) return;
        // A smart object: copied, then the question about rasterizing it (pixels.cut refuses one).
        if (session_->smartObjectBlocksPixels()) { session_->cutSelection(); return; }
        runCommand("pixels.cut", {}, tr("Cut"));
    }), "edit.clipboard"))->setObjectName("command.pixels.cut");
    nameAction("edit.copy", needsDocument(edit->addAction(tr("&Copy"), QKeySequence::Copy, this, [this] {
        // As Photoshop: with no selection, the selected layers themselves (pasted whole in any document).
        if (!session_->document()->selection && !session_->selectedLayerIds().empty()) { runCommand("layers.copy", {}, tr("Copy")); return; }
        if (session_->canCopyPixels()) runCommand("pixels.copy", {}, tr("Copy"));
    }), "edit.clipboard"))->setObjectName("command.pixels.copy");
    needsDocument(edit->addAction(tr("Copy &Merged"), QKeySequence("Ctrl+Shift+C"), this, [this] {
        if (session_->canEditLayers() && !(session_->document()->selection && session_->document()->selection->isEmpty())) runCommand("pixels.copyMerged", {}, tr("Copy Merged"));
    }), "edit.clipboard")->setObjectName("command.pixels.copyMerged");
    nameAction("edit.paste", needsDocument(edit->addAction(tr("&Paste"), QKeySequence::Paste, this, [this] {
        if (EditorSession::hasLayerClipboard()) { if (session_->canPaste()) runCommand("layers.paste", {}, tr("Paste")); return; }
        if (session_->hasPixelsToPaste()) runCommand("pixels.paste", {}, tr("Paste"));
    }), "edit.clipboard"))->setObjectName("command.pixels.paste");
    edit->addSeparator();
    // Free Transform is interactive: Ctrl+T invokes it, the canvas updates it, and Enter or Apply commits it through
    // the command layers.setTransform (EditorSession::commitTransformCommand).
    nameAction("edit.freeTransform", needsDocument(edit->addAction(tr("&Free Transform"), QKeySequence("Ctrl+T"), this, [this] { session_->transformCommand(); }), "layers.transform"))->setObjectName("command.transform");
    nameAction("edit.warp", needsDocument(edit->addAction(tr("&Warp…"), this, [this] { WarpDialog(session_, this).exec(); }), "edit.distort"));
    needsDocument(edit->addAction(tr("Warp Ca&ge"), this, [this] {
        QString error;
        if (!session_->beginWarpCage(&error)) showError(tr("Warp Cage"), error);
        else statusBar()->showMessage(tr("Drag the cage's points; Enter applies, Esc cancels."), 8000);
    }), "edit.distort");
    nameAction("edit.fillForeground", needsDocument(edit->addAction(tr("Fill with Foreground"), QKeySequence("Alt+Backspace"), this, [this] { fillWith(session_->foregroundColor); }), "edit.fill"))->setObjectName("command.pixels.fill");
    nameAction("edit.fillBackground", needsDocument(edit->addAction(tr("Fill with Background"), QKeySequence("Ctrl+Backspace"), this, [this] { fillWith(session_->backgroundColor); }), "edit.fill"));
    QAction* clear = needsDocument(edit->addAction(tr("Clear"), QKeySequence(Qt::Key_Delete), this, [this] {
        if (session_->document() && session_->document()->selection) runCommand("pixels.clear", {}, tr("Clear"));
        else deleteLayersCommand();
    }), "layers.structure");
    clear->setObjectName("command.pixels.clear");
    clear->setShortcuts({QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace)});
    nameAction("edit.contentAwareFill", needsDocument(edit->addAction(tr("Content-Aware Fill…"), QKeySequence("Shift+F5"), this, [this] {
        if (!session_->canAdjustPixels() || !session_->document()->selection || !session_->document()->selection->coverage) { showError(tr("Content-Aware Fill"), tr("Select a visible image layer and an area to fill.")); return; }
        (new ContentFillDialog(session_, this))->show();
    }), "edit.fill"));
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
        if (!profile) return;
        // A profile the method names runs as the command; an ICC file chosen from disk is assigned directly (and
        // not recorded: the method takes a key or a path, and the dialog keeps no path).
        if (const QString key = profileKey(*profile); !key.isEmpty()) runCommand("document.profile", {{"action", "assign"}, {"profile", key}}, tr("Assign Profile"));
        else session_->assignProfile(*profile);
    }), "document.profile");
    needsDocument(edit->addAction(tr("Convert to Profile…"), this, [this, profileKey] {
        auto choice = color::askConvertProfile(this, session_->document()->profile);
        if (!choice) return;
        const QString key = profileKey(choice->profile);
        QApplication::setOverrideCursor(Qt::WaitCursor);
        if (!key.isEmpty()) {
            runCommand("document.profile", {{"action", "convert"}, {"profile", key}, {"intent", QString::fromLatin1(compositor::renderingIntentKey(choice->options.intent))},
                                            {"blackPointCompensation", choice->options.blackPointCompensation}}, tr("Convert to Profile"));
            QApplication::restoreOverrideCursor();
            return;
        }
        QString error;
        const bool ok = session_->convertToProfile(choice->profile, choice->options, &error);
        QApplication::restoreOverrideCursor();
        if (!ok) showError(tr("Convert to Profile"), error);
    }), "document.profile");

    edit->addSeparator();
    // Photoshop's Edit > Search (Ctrl+F): every command, tool and G'MIC filter by name.
    searchAction_ = edit->addAction(tr("&Search…"), QKeySequence("Ctrl+F"), this, [this] { showCommandPalette(); });
    searchAction_->setObjectName("edit.search");
    // Photoshop's Ctrl+K, and the platform's own Preferences key where it has one.
    QAction* preferences = edit->addAction(tr("Prefere&nces…"), this, &MainWindow::showPreferences);
    QList<QKeySequence> preferenceKeys{QKeySequence("Ctrl+K")};
    for (const QKeySequence& key : QKeySequence::keyBindings(QKeySequence::Preferences)) if (!preferenceKeys.contains(key)) preferenceKeys << key;
    preferences->setShortcuts(preferenceKeys);

    QMenu* image = menuBar()->addMenu(tr("&Image"));
    // Photoshop's Image > Mode: the document's bits per channel (docs/bit-depth.md).
    QMenu* mode = image->addMenu(tr("&Mode"));
    needsDocument(mode->menuAction(), "document.mode");
    // The colour modes (docs/color-modes.md): RGB, CMYK and Lab, as Photoshop lists them above the depths.
    auto* colorModes = new QActionGroup(this);
    modeRgbAction_ = needsDocument(mode->addAction(tr("&RGB Color"), this, [this] { convertColorMode(ColorMode::RGB); }), "document.mode");
    modeCmykAction_ = needsDocument(mode->addAction(tr("&CMYK Color"), this, [this] { convertColorMode(ColorMode::CMYK); }), "document.mode");
    modeLabAction_ = needsDocument(mode->addAction(tr("&Lab Color"), this, [this] { convertColorMode(ColorMode::Lab); }), "document.mode");
    for (QAction* a : {modeRgbAction_, modeCmykAction_, modeLabAction_}) { a->setCheckable(true); colorModes->addAction(a); }
    modeRgbAction_->setChecked(true);
    mode->addSeparator();
    auto* depths = new QActionGroup(this);
    mode8Action_ = needsDocument(mode->addAction(tr("&8 Bits/Channel"), this, [this] { convertMode(SampleType::U8); }), "document.mode");
    mode16Action_ = needsDocument(mode->addAction(tr("&16 Bits/Channel"), this, [this] { convertMode(SampleType::U16); }), "document.mode");
    mode32Action_ = needsDocument(mode->addAction(tr("&32 Bits/Channel"), this, [this] { convertMode(SampleType::F32); }), "document.mode");
    for (QAction* a : {mode8Action_, mode16Action_, mode32Action_}) { a->setCheckable(true); depths->addAction(a); }
    mode8Action_->setChecked(true);
    image->addSeparator();
    needsDocument(image->addAction(tr("&Canvas Size…"), QKeySequence("Ctrl+Alt+C"), this, [this] {
        auto o = askCanvasSize(this, session_->document()->width, session_->document()->height);
        if (!o) return;
        runCommand("canvas.resize", {{"width", o->width}, {"height", o->height}, {"anchorX", o->anchorX}, {"anchorY", o->anchorY}}, tr("Canvas Size"));
    }), "canvas.size");
    needsDocument(image->addAction(tr("&Image Size…"), QKeySequence("Ctrl+Alt+I"), this, [this] {
        auto o = askImageSize(this, session_->document()->width, session_->document()->height, session_->document()->resolution);
        if (!o) return;
        runCommand("image.resize", {{"width", o->width}, {"height", o->height}, {"resolution", o->resolution},
                                    {"sampling", o->sampling == 0 ? "nearest" : o->sampling == 1 ? "smooth" : "high"}}, tr("Image Size"));
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
        static const char* const bases[] = {"transparent", "topLeft", "bottomRight"};
        trimCommand({{"basedOn", bases[std::clamp(basedOn->currentIndex(), 0, 2)]}, {"top", o.top}, {"left", o.left}, {"bottom", o.bottom}, {"right", o.right}, {"tolerance", int(o.tolerance)}});
    }), "edit.crop");
    needsDocument(image->addAction(tr("Crop to Selection"), this, [this] {
        const auto& d = session_->document();
        if (d && d->selection) {
            Rect b = d->selection->bounds();
            if (!b.isEmpty()) {
                if (runCommand("canvas.crop", {{"x", b.x}, {"y", b.y}, {"width", b.width}, {"height", b.height}}, tr("Crop to Selection")))
                    runCommand("selection.none", {}, tr("Crop to Selection"));
            }
        }
    }), "edit.crop")->setObjectName("command.canvas.crop");
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
    needsDocument(adjustments->addAction(tr("&Invert"), QKeySequence("Ctrl+I"), this, [this] { runCommand("pixels.invert", {}, tr("Invert")); }), "adjustment.Invert")->setObjectName("command.pixels.invert");
    image->addSeparator();
    needsDocument(image->addAction(tr("Flip Canvas Horizontal"), this, [this] { runCommand("canvas.flip", {}, tr("Flip Canvas Horizontal")); }), "canvas.flip")->setObjectName("command.canvas.flip");
    needsDocument(image->addAction(tr("Flip Canvas Vertical"), this, [this] { runCommand("canvas.flip", {{"vertical", true}}, tr("Flip Canvas Vertical")); }), "canvas.flip");

    QMenu* layer = menuBar()->addMenu(tr("&Layer"));
    // Converted to the command path (CONTRIBUTING.md, "Commands"): the item runs the automation method itself.
    nameAction("layer.new", needsDocument(layer->addAction(tr("&New Layer"), QKeySequence("Ctrl+Shift+N"), this, [this] { runCommand("layers.add", {}, tr("New Layer")); }), "layers.structure"))->setObjectName("command.layers.add");
    needsDocument(layer->addAction(tr("New Layer &Below"), this, [this] { runCommand("layers.add", {{"below", true}}, tr("New Layer Below")); }), "layers.structure")->setObjectName("command.layers.addBelow");
    needsDocument(layer->addAction(tr("New &Folder"), this, [this] { runCommand("layers.add", {{"kind", "group"}}, tr("New Folder")); }), "layers.structure")->setObjectName("command.layers.addGroup");
    needsDocument(layer->addAction(tr("&Group Layers"), QKeySequence("Ctrl+G"), this, [this] { runCommand("layers.group", {}, tr("Group Layers")); }), "layers.structure")->setObjectName("command.layers.group");
    nameAction("layer.viaCopy", needsDocument(layer->addAction(tr("Layer via &Copy"), QKeySequence("Ctrl+J"), this, [this] {
        const Layer* l = session_->activeLayer();
        if (l && !l->isGroup) runCommand("layers.viaCopy", {}, tr("Layer via Copy"));
    }), "edit.clipboard"));
    nameAction("layer.duplicate", needsDocument(layer->addAction(tr("&Duplicate Layer"), this, [this] { runCommand("layers.duplicate", {}, tr("Duplicate Layer")); }), "layers.structure"))->setObjectName("command.layers.duplicate");
    nameAction("layer.delete", needsDocument(layer->addAction(tr("De&lete Layer"), this, [this] { deleteLayersCommand(); }), "layers.structure"))->setObjectName("command.layers.delete");
    mergeAction_ = needsDocument(layer->addAction(tr("Merge Do&wn"), QKeySequence("Ctrl+E"), this, [this] { runCommand("layers.merge", {}, mergeAction_->text().remove('&')); }), "layers.merge");
    mergeAction_->setObjectName("command.layers.merge");
    named_["layer.merge"] = mergeAction_;
    mergeVisibleAction_ = needsDocument(layer->addAction(tr("Merge &Visible"), QKeySequence("Ctrl+Shift+E"), this, [this] { runCommand("layers.merge", {{"visible", true}}, tr("Merge Visible")); }), "layers.merge");
    mergeVisibleAction_->setObjectName("command.layers.mergeVisible");
    editTextAction_ = nameAction("layer.editText", needsDocument(layer->addAction(tr("Edit &Text…"), this, [this] { const Layer* l = session_->activeLayer(); if (l && l->isLiveText()) session_->requestTextEdit(l->id); }), "edit.text"));
    nameAction("layer.rename", needsDocument(layer->addAction(tr("&Rename Layer…"), this, [this] {
        const Layer* active = session_->activeLayer();
        if (!active) return;
        bool ok;
        QString name = QInputDialog::getText(this, tr("Rename Layer"), tr("Name"), QLineEdit::Normal, QString::fromStdString(active->name), &ok);
        if (ok) runCommand("layers.set", {{"name", name}}, tr("Rename Layer"));
    }), "layers.structure"));
    needsDocument(layer->addAction(tr("Move &Out of Folder"), QKeySequence("Ctrl+Shift+["), this, [this] { session_->moveActiveLayerOutOfGroup(); }), "layers.structure");
    QMenu* adjustmentLayers = layer->addMenu(tr("New &Adjustment Layer"));
    for (int i = 0; i < adjustmentKindCount; i++) {
        AdjustmentKind kind = AdjustmentKind(i);
        needsDocument(adjustmentLayers->addAction(names::adjustmentKind(kind), this, [this, kind] { runCommand("layers.add", {{"kind", "adjustment"}, {"adjustmentKind", QString::fromUtf8(adjustmentKindName(kind))}}, tr("New Adjustment Layer")); }),
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
    nameAction("smart.convert", needsDocument(smart->addAction(tr("&Convert to Smart Object"), this, [this] {
        runCommand("smartObject.convert", {}, tr("Couldn’t convert to a smart object"));
    }), "edit.smartObject"));
    nameAction("smart.viaCopy", needsDocument(smart->addAction(tr("New Smart Object via &Copy"), this, [this] {
        const Layer* l = session_->activeLayer();
        if (l && l->isLiveSmartObject()) runCommand("smartObject.viaCopy", {}, tr("Couldn’t copy the smart object"));
    }), "edit.smartObject"));
    nameAction("smart.edit", needsDocument(smart->addAction(tr("&Edit Contents"), this, [this] { editSmartObjectContents(); }), "edit.smartObject"));
    nameAction("smart.replace", needsDocument(smart->addAction(tr("&Replace Contents…"), this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Replace Contents"), QSettings().value("lastDir").toString(),
                                                          tr("Images, Photoshop and Affinity documents (*.psd *.psb *.afphoto *.afdesign *.afpub *.af *.png *.jpg *.jpeg *.tif *.tiff *.webp *.bmp *.gif %1)").arg(compositor::rawSupported() ? QStringLiteral("*.cr2 *.cr3 *.crw *.nef *.nrw *.arw *.srf *.sr2 *.raf *.orf *.rw2 *.rwl *.pef *.dng *.3fr *.iiq *.erf *.kdc *.dcr *.mrw *.srw *.x3f") : QString()));
        if (path.isEmpty()) return;
        runCommand("smartObject.replace", {{"path", path}}, tr("Couldn’t replace the contents"));
    }), "edit.smartObject"));
    nameAction("smart.rasterize", needsDocument(smart->addAction(tr("R&asterize"), this, [this] {
        const Layer* l = session_->activeLayer();
        if (l && l->smartObject) runCommand("smartObject.rasterize", {}, tr("Rasterize"));
    }), "edit.smartObject"));
    layer->addSeparator();
    QMenu* mask = layer->addMenu(tr("Layer &Mask"));
    nameAction("mask.add", needsDocument(mask->addAction(tr("Reveal All"), this, [this] { maskCommand({{"action", "add"}}); }), "layers.mask"));
    needsDocument(mask->addAction(tr("Hide All"), this, [this] { maskCommand({{"action", "add"}, {"revealing", false}}); }), "layers.mask");
    needsDocument(mask->addAction(tr("From Selection (Reveal)"), this, [this] { maskCommand({{"action", "addFromSelection"}}); }), "edit.selection");
    needsDocument(mask->addAction(tr("From Selection (Hide)"), this, [this] { maskCommand({{"action", "addFromSelection"}, {"revealing", false}}); }), "edit.selection");
    mask->addSeparator();
    nameAction("mask.toggle", needsDocument(mask->addAction(tr("Enable / Disable"), this, [this] { maskCommand({{"action", "toggle"}}); }), "layers.mask"));
    nameAction("mask.invert", needsDocument(mask->addAction(tr("Invert"), this, [this] { maskCommand({{"action", "invert"}}); }), "layers.mask"));
    nameAction("mask.apply", needsDocument(mask->addAction(tr("Apply"), this, [this] { maskCommand({{"action", "apply"}}); }), "layers.applyMask"));
    nameAction("mask.delete", needsDocument(mask->addAction(tr("Delete"), this, [this] { maskCommand({{"action", "delete"}}); }), "layers.mask"));
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
    nameAction("layer.clipping", needsDocument(layer->addAction(tr("Create / Release Cl&ipping Mask"), QKeySequence("Ctrl+Alt+G"), this, [this] {
        const Layer* l = session_->activeLayer();
        if (l && session_->canToggleClippingMask(l->id)) runCommand("layers.set", {{"clipping", !l->maskSourceId.has_value()}}, tr("Clipping Mask"));
    }), "layers.structure"))->setObjectName("command.layers.clipping");
    layer->addSeparator();
    needsDocument(layer->addAction(tr("Bring Forward"), QKeySequence("Ctrl+]"), this, [this] { if (session_->canMoveActiveLayer(1)) runCommand("layers.reorder", {{"offset", 1}}, tr("Bring Forward")); }), "layers.structure")->setObjectName("command.layers.forward");
    needsDocument(layer->addAction(tr("Send Backward"), QKeySequence("Ctrl+["), this, [this] { if (session_->canMoveActiveLayer(-1)) runCommand("layers.reorder", {{"offset", -1}}, tr("Send Backward")); }), "layers.structure");
    layer->addSeparator();
    needsDocument(layer->addAction(tr("Flip Layer Horizontal"), this, [this] { runCommand("layers.flip", {}, tr("Flip Layer Horizontal")); }), "layers.transform")->setObjectName("command.layers.flip");
    needsDocument(layer->addAction(tr("Flip Layer Vertical"), this, [this] { runCommand("layers.flip", {{"vertical", true}}, tr("Flip Layer Vertical")); }), "layers.transform");
    QMenu* sampling = layer->addMenu(tr("Resampling"));
    needsDocument(sampling->addAction(tr("High Quality"), this, [this] { samplingCommand(Sampling::High); }), "layers.structure");
    needsDocument(sampling->addAction(tr("Smooth"), this, [this] { samplingCommand(Sampling::Smooth); }), "layers.structure");
    needsDocument(sampling->addAction(tr("Nearest Neighbour"), this, [this] { samplingCommand(Sampling::Nearest); }), "layers.structure");

    // Photoshop's Type menu: the active text layer's outlines as a path or a shape.
    QMenu* type = menuBar()->addMenu(tr("&Type"));
    auto fromText = [this](bool shape) {
        QString error;
        const auto id = session_->activeLayerId();
        const bool ok = id && (shape ? session_->textToShape(*id, &error) : session_->textToWorkPath(*id, &error));
        if (!ok) QMessageBox::information(this, shape ? tr("Convert to Shape") : tr("Create Work Path"), error.isEmpty() ? tr("Choose a text layer first.") : error);
    };
    nameAction("type.workPath", needsDocument(type->addAction(tr("Create &Work Path"), this, [fromText] { fromText(false); }), "edit.text"));
    nameAction("type.shape", needsDocument(type->addAction(tr("Convert to &Shape"), this, [fromText] { fromText(true); }), "edit.paint"));

    // Everything about the selection in one place, as Photoshop's Select menu: the whole-canvas commands,
    // then Modify, then loading a layer's pixels or mask as the selection.
    QMenu* select = menuBar()->addMenu(tr("&Select"));
    nameAction("select.all", needsDocument(select->addAction(tr("&All"), QKeySequence::SelectAll, this, [this] { runCommand("selection.all", {}, tr("Select All")); }), "edit.selection"));
    nameAction("select.deselect", needsDocument(select->addAction(tr("&Deselect"), QKeySequence("Ctrl+D"), this, [this] { runCommand("selection.none", {}, tr("Deselect")); }), "edit.selection"));
    nameAction("select.reselect", needsDocument(select->addAction(tr("&Reselect"), QKeySequence("Shift+Ctrl+D"), this, [this] { if (session_->canReselect()) runCommand("selection.reselect", {}, tr("Reselect")); }), "edit.selection"));
    nameAction("select.inverse", needsDocument(select->addAction(tr("&Inverse"), QKeySequence("Ctrl+Shift+I"), this, [this] { runCommand("selection.invert", {}, tr("Inverse")); }), "edit.selection"));
    needsDocument(select->addAction(tr("Edit in &Quick Mask Mode"), QKeySequence("Q"), this, [this] { session_->toggleQuickMask(); }), "edit.selection");
    select->addSeparator();
    QMenu* modify = select->addMenu(tr("&Modify"));
    needsDocument(modify->addAction(tr("&Expand…"), this, [this] { bool ok; int n = QInputDialog::getInt(this, tr("Expand Selection"), tr("Pixels"), 1, 1, 500, 1, &ok); if (ok) runCommand("selection.grow", {{"amount", n}}, tr("Expand Selection")); }), "edit.selection");
    needsDocument(modify->addAction(tr("&Contract…"), this, [this] { bool ok; int n = QInputDialog::getInt(this, tr("Contract Selection"), tr("Pixels"), 1, 1, 500, 1, &ok); if (ok) runCommand("selection.grow", {{"amount", -n}}, tr("Contract Selection")); }), "edit.selection");
    nameAction("select.feather", needsDocument(modify->addAction(tr("&Feather…"), QKeySequence("Shift+F6"), this, [this] { bool ok; double r = QInputDialog::getDouble(this, tr("Feather Selection"), tr("Radius (pixels)"), 5, 0.1, 250, 1, &ok); if (ok) runCommand("selection.feather", {{"radius", r}}, tr("Feather Selection")); }), "edit.selection"));
    needsDocument(modify->addAction(tr("&Smooth…"), this, [this] { bool ok; int n = QInputDialog::getInt(this, tr("Smooth Selection"), tr("Sample radius (pixels)"), 3, 1, 100, 1, &ok); if (ok) runCommand("selection.smooth", {{"radius", n}}, tr("Smooth Selection")); }), "edit.selection");
    needsDocument(modify->addAction(tr("&Border…"), this, [this] { bool ok; int n = QInputDialog::getInt(this, tr("Border Selection"), tr("Width (pixels)"), 4, 1, 200, 1, &ok); if (ok) runCommand("selection.border", {{"width", n}}, tr("Border Selection")); }), "edit.selection");
    select->addSeparator();
    QMenu* load = select->addMenu(tr("&Load as Selection"));
    needsDocument(load->addAction(tr("Layer Pixels"), this, [this] { if (session_->activeLayerId()) runCommand("selection.fromLayer", {}, tr("Load as Selection")); }), "edit.selection");
    needsDocument(load->addAction(tr("Layer Mask"), this, [this] { if (session_->activeLayerId()) runCommand("selection.fromLayer", {{"mask", true}}, tr("Load as Selection")); }), "edit.selection");
    load->addSeparator();
    needsDocument(load->addAction(tr("Add Layer Pixels"), this, [this] { if (session_->activeLayerId()) runCommand("selection.fromLayer", {{"mode", "add"}}, tr("Load as Selection")); }), "edit.selection");
    needsDocument(load->addAction(tr("Subtract Layer Pixels"), this, [this] { if (session_->activeLayerId()) runCommand("selection.fromLayer", {{"mode", "subtract"}}, tr("Load as Selection")); }), "edit.selection");
    needsDocument(load->addAction(tr("Intersect with Layer Pixels"), this, [this] { if (session_->activeLayerId()) runCommand("selection.fromLayer", {{"mode", "intersect"}}, tr("Load as Selection")); }), "edit.selection");
    // Selections kept as alpha channels (the Channels panel; docs/channels.md).
    needsDocument(select->addAction(tr("Load Selection…"), this, [this] { (new LoadSelectionDialog(session_, this))->open(); }), "edit.channels");
    nameAction("select.save", needsDocument(select->addAction(tr("Save Selection…"), this, [this] {
        if (!session_->document()->selection) { showError(tr("Save Selection"), tr("Make a selection first.")); return; }
        (new SaveSelectionDialog(session_, this))->open();
    }), "edit.channels"));
    // Photoshop's channel keys: Ctrl+2 the composite, then one key per colour channel (Ctrl+3, 4, 5 red, green and
    // blue; Ctrl+3 to 6 cyan to black in CMYK), then the first alpha channels up to Ctrl+9; with Alt, the channel is
    // loaded as a selection instead.
    auto channelKey = [this](int n, bool load) {
        if (!session_->hasDocument()) return;
        const auto& channels = session_->document()->channels;
        const int firstAlpha = 3 + colorModeColorChannels(session_->document()->colorMode);
        if (n >= firstAlpha && size_t(n - firstAlpha) >= channels.size()) return;
        if (load) {
            SelectionSource source;
            source.kind = n == 2 ? SelectionSource::Composite : n == 3 ? SelectionSource::Red : n == 4 ? SelectionSource::Green : n == 5 ? SelectionSource::Blue
                          : n < firstAlpha ? SelectionSource::Black : SelectionSource::AlphaChannel;
            if (n >= firstAlpha) source.id = channels[size_t(n - firstAlpha)].id;
            session_->loadSelectionFromSource(source, false, SelectionMode::Replace);
        } else if (n < firstAlpha) session_->selectColorChannels(n == 2 ? session_->allColors() : 1u << (n - 3));
        else session_->selectAlphaChannel(channels[size_t(n - firstAlpha)].id);
    };
    for (int n = 2; n <= 9; n++)
        for (bool load : {false, true}) {
            auto* key = needsDocument(new QAction(load ? tr("Load Channel %1 as Selection").arg(n - 1) : tr("Select Channel %1").arg(n - 1), this), "edit.channels");
            key->setShortcut(QKeySequence(QString::fromLatin1(load ? "Ctrl+Alt+%1" : "Ctrl+%1").arg(n)));
            connect(key, &QAction::triggered, this, [channelKey, n, load] { channelKey(n, load); });
            addAction(key);
        }

    QMenu* filter = menuBar()->addMenu(tr("Filte&r"));
    // Photoshop's filters on a smart object go on as Smart Filters: the blurs, noise and the ones the Smart Filter
    // kernels draw. The rest change pixels, so on a smart object they ask to rasterize first.
    auto smartCapable = [](FilterKind kind) {
        return kind == FilterKind::GaussianBlur || kind == FilterKind::MotionBlur || kind == FilterKind::AddNoise
               || compositor::smartFilterParametersFor(kind, compositor::FilterSettings::defaults(kind)).has_value();
    };
    auto filterAction = [this, &needsDocument, smartCapable](QMenu* menu, const QString& label, FilterKind kind) {
        needsDocument(menu->addAction(label, this, [this, kind, smartCapable] {
            if (smartCapable(kind) && session_->canAddSmartFilter()) { (new FilterDialog(session_, kind, this, true))->show(); return; }
            if (session_->smartObjectBlocksPixels(true)) return;
            if (!session_->canAdjustPixels()) { showError(tr("Filters"), tr("Select a visible image layer (not a mask) to filter its pixels.")); return; }
            if (compositor::filterRunsDirectly(kind)) {
                // No dialog, as in Photoshop: Clouds draws a new pattern each time.
                const uint32_t seed = uint32_t(std::random_device{}() % 1000000000u);
                runCommand("pixels.filter", gridFilterStep(kind, compositor::FilterSettings::defaults(kind), seed), names::filterKind(kind));
                return;
            }
            (new FilterDialog(session_, kind, this))->show();
        }), (std::string("filter.") + filterKindName(kind)).c_str());
    };
    // Photoshop's places: the top-level filters, then the submenus in its order, each in its own order.
    // Photoshop's shortcut. A destructive filter here: on a smart object it asks first, like the others.
    needsDocument(filter->addAction(tr("Camera &Raw Filter…"), QKeySequence("Shift+Ctrl+A"), this, [this] {
        if (session_->smartObjectBlocksPixels(true)) return;
        if (!session_->canAdjustPixels()) { showError(tr("Camera Raw Filter"), tr("Select a visible image layer (not a mask) to filter its pixels.")); return; }
        (new CameraRawDialog(session_, this))->show();
    }), "filter.Camera Raw");
    filterAction(filter, tr("&Lens Correction…"), FilterKind::LensCorrection);
    filter->addSeparator();
    QMenu* blurMenu = filter->addMenu(tr("&Blur"));
    filterAction(blurMenu, tr("&Box Blur…"), FilterKind::BoxBlur);
    filterAction(blurMenu, tr("&Gaussian Blur…"), FilterKind::GaussianBlur);
    filterAction(blurMenu, tr("&Motion Blur…"), FilterKind::MotionBlur);
    filterAction(blurMenu, tr("&Radial Blur…"), FilterKind::RadialBlur);
    filterAction(blurMenu, tr("&Surface Blur…"), FilterKind::SurfaceBlur);
    QMenu* distortMenu = filter->addMenu(tr("&Distort"));
    filterAction(distortMenu, tr("&Pinch…"), FilterKind::Pinch);
    filterAction(distortMenu, tr("P&olar Coordinates…"), FilterKind::PolarCoordinates);
    filterAction(distortMenu, tr("&Ripple…"), FilterKind::Ripple);
    filterAction(distortMenu, tr("S&hear…"), FilterKind::Shear);
    filterAction(distortMenu, tr("&Spherize…"), FilterKind::Spherize);
    filterAction(distortMenu, tr("&Twirl…"), FilterKind::Twirl);
    filterAction(distortMenu, tr("&Wave…"), FilterKind::Wave);
    filterAction(distortMenu, tr("&ZigZag…"), FilterKind::ZigZag);
    QMenu* noiseMenu = filter->addMenu(tr("&Noise"));
    filterAction(noiseMenu, tr("Add &Noise…"), FilterKind::AddNoise);
    filterAction(noiseMenu, tr("&Dust && Scratches…"), FilterKind::DustAndScratches);
    filterAction(noiseMenu, tr("&Median…"), FilterKind::Median);
    QMenu* pixelateMenu = filter->addMenu(tr("&Pixelate"));
    filterAction(pixelateMenu, tr("&Mosaic…"), FilterKind::Mosaic);
    QMenu* renderMenu = filter->addMenu(tr("R&ender"));
    filterAction(renderMenu, tr("&Clouds"), FilterKind::Clouds);
    filterAction(renderMenu, tr("&Difference Clouds"), FilterKind::DifferenceClouds);
    QMenu* sharpenMenu = filter->addMenu(tr("S&harpen"));
    filterAction(sharpenMenu, tr("&Unsharp Mask…"), FilterKind::UnsharpMask);
    QMenu* stylizeMenu = filter->addMenu(tr("St&ylize"));
    filterAction(stylizeMenu, tr("&Emboss…"), FilterKind::Emboss);
    filterAction(stylizeMenu, tr("&Find Edges"), FilterKind::FindEdges);
    QMenu* otherMenu = filter->addMenu(tr("O&ther"));
    filterAction(otherMenu, tr("&High Pass…"), FilterKind::HighPass);
    filterAction(otherMenu, tr("Ma&ximum…"), FilterKind::Maximum);
    filterAction(otherMenu, tr("M&inimum…"), FilterKind::Minimum);
    filterAction(otherMenu, tr("&Offset…"), FilterKind::Offset);
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
    filter->addSeparator();
    gmicAction_ = needsDocument(filter->addAction(tr("&G'MIC…"), QKeySequence("Ctrl+Shift+G"), this, [this] { openGmic(); }), "filter.G'MIC");
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
    // Photoshop's guides and snapping: View > Show > Guides (Ctrl+;) and Smart Guides, Snap (Shift+Ctrl+;) and Snap To,
    // Lock Guides (Alt+Ctrl+;), Clear Guides and New Guide. The switches are the app's (ViewOptions), the guides the
    // document's: adding and clearing them are undo steps, run as the guides.* commands.
    auto viewOption = [this](QMenu* menu, const QString& label, const QKeySequence& key, bool ViewOptions::*field) {
        QAction* a = menu->addAction(label);
        if (!key.isEmpty()) a->setShortcut(key);
        a->setCheckable(true);
        a->setChecked(ViewOptions::get().*field);
        connect(a, &QAction::toggled, this, [this, field](bool on) {
            ViewOptions::get().*field = on;
            ViewOptions::get().save();
            for (auto& tab : tabs_) tab.canvas->update();
        });
        connect(menu, &QMenu::aboutToShow, a, [a, field] { a->setChecked(ViewOptions::get().*field); });
        return a;
    };
    QMenu* show = view->addMenu(tr("S&how"));
    QAction* showGuides = viewOption(show, tr("&Guides"), QKeySequence("Ctrl+;"), &ViewOptions::showGuides);
    viewOption(show, tr("&Smart Guides"), QKeySequence(), &ViewOptions::smartGuides);
    addAction(showGuides);   // the shortcut works with the menu closed
    QAction* snap = viewOption(view, tr("Sn&ap"), QKeySequence("Shift+Ctrl+;"), &ViewOptions::snap);
    addAction(snap);
    QMenu* snapTo = view->addMenu(tr("Snap &To"));
    QList<QAction*> snapTargets{viewOption(snapTo, tr("&Guides"), QKeySequence(), &ViewOptions::snapToGuides),
                                viewOption(snapTo, tr("&Layers"), QKeySequence(), &ViewOptions::snapToLayers),
                                viewOption(snapTo, tr("&Document Bounds"), QKeySequence(), &ViewOptions::snapToBounds)};
    snapTo->addSeparator();
    auto setAllTargets = [snapTargets](bool on) {
        ViewOptions& o = ViewOptions::get();
        o.snapToGuides = o.snapToLayers = o.snapToBounds = on;
        o.save();
        for (QAction* a : snapTargets) { QSignalBlocker block(a); a->setChecked(on); }
    };
    snapTo->addAction(tr("&All", "snap to"), this, [setAllTargets] { setAllTargets(true); });
    snapTo->addAction(tr("&None", "snap to"), this, [setAllTargets] { setAllTargets(false); });
    view->addSeparator();
    QAction* lockGuides = viewOption(view, tr("Lock Gu&ides"), QKeySequence("Alt+Ctrl+;"), &ViewOptions::lockGuides);
    addAction(lockGuides);
    needsDocument(view->addAction(tr("C&lear Guides"), this, [this] { if (!session_->guides().empty()) runCommand("guides.delete", {{"all", true}}, tr("Clear Guides")); }));
    needsDocument(view->addAction(tr("&New Guide…"), this, [this] {
        auto guide = askNewGuide(this);
        if (!guide) return;
        runCommand("guides.add", {{"orientation", guide->vertical() ? "vertical" : "horizontal"}, {"position", guide->position}}, tr("New Guide"));
    }));
    view->addSeparator();
    layersDock_->toggleViewAction()->setText(tr("&Layers Panel"));
    adjustDock_->toggleViewAction()->setText(tr("&Adjustments Panel"));
    view->addAction(layersDock_->toggleViewAction());
    pathsDock_->toggleViewAction()->setText(tr("&Paths Panel"));
    view->addAction(pathsDock_->toggleViewAction());
    view->addAction(adjustDock_->toggleViewAction());
    view->addSeparator();
    // A 32-bit document's view (docs/bit-depth.md, "32 bits"): not an undo step; the canvas shows it as it changes.
    previewOptionsAction_ = view->addAction(tr("32-bit Pre&view Options…"), this, [this] {
        const View32 before = session_->view32();
        auto chosen = askPreviewOptions(this, before, [this](const View32& v) { session_->setView32(v); });
        session_->setView32(chosen ? *chosen : before);
    });
    // Soft proofing (docs/color-management.md): Photoshop's Proof Setup, Proof Colors and Gamut Warning.
    QMenu* proofSetup = view->addMenu(tr("Proof Set&up"));
    proofSetup->addAction(tr("Custom…"), this, [this] { color::showProofSetup(this); });
    proofSetup->addSeparator();
    // Photoshop's default proof: the press the Working CMYK describes.
    QAction* proofCmyk = proofSetup->addAction(tr("Working CMYK"), this, [] { color::Settings s = color::settings(); s.proofProfile = QStringLiteral("working-cmyk"); color::setSettings(s); });
    proofCmyk->setCheckable(true);
    proofCmyk->setChecked(color::settings().proofProfile == QLatin1String("working-cmyk"));
    QAction* proof = view->addAction(tr("Proof Colo&rs"), QKeySequence("Ctrl+Y"), this, [](bool on) { color::Settings s = color::settings(); s.proofColors = on; color::setSettings(s); });
    QAction* gamut = view->addAction(tr("Gamut Wa&rning"), QKeySequence("Ctrl+Shift+Y"), this, [](bool on) { color::Settings s = color::settings(); s.gamutWarning = on; color::setSettings(s); });
    for (QAction* a : {proof, gamut}) a->setCheckable(true);
    connect(color::notifier(), &color::Notifier::changed, this, [this, proof, gamut, proofCmyk] {
        proof->setChecked(color::settings().proofColors);
        gamut->setChecked(color::settings().gamutWarning);
        proofCmyk->setChecked(color::settings().proofProfile == QLatin1String("working-cmyk"));
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
    histogramDock_->toggleViewAction()->setText(tr("&Histogram"));
    window->addAction(histogramDock_->toggleViewAction());
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

void MainWindow::convertColorMode(ColorMode colorMode) {
    if (!session_->hasDocument() || session_->document()->colorMode == colorMode) { refreshDepthGating(); return; }
    if (runCommand("image.mode", {{"colorMode", QString::fromLatin1(colorModeKey(colorMode))}}, tr("Mode"))) updateColorSwatches();
    refreshActions();
}

void MainWindow::convertMode(SampleType type) {
    if (!session_->hasDocument() || session_->sampleType() == type) { refreshDepthGating(); return; }
    // From 32 bits: HDR Toning, previewed on the canvas through the view (docs/bit-depth.md, "32 bits").
    std::optional<View32> toning;
    if (session_->sampleType() == SampleType::F32) {
        const View32 before = session_->view32();
        toning = askHdrToning(this, type == SampleType::U16 ? 16 : 8, View32(), [this](const View32& v) { session_->setView32(v); });
        session_->setView32(before);
        if (!toning) { refreshDepthGating(); return; }
    }
    QJsonObject params{{"bits", type == SampleType::F32 ? 32 : type == SampleType::U16 ? 16 : 8}};
    if (toning && !toning->isDefault()) {
        params["method"] = QString::fromLatin1(toneMethodKey(toning->method));
        params["exposure"] = toning->exposure;
        params["gamma"] = toning->gamma;
    }
    runCommand("image.mode", params, tr("Mode"));
    refreshActions();
}

void MainWindow::refreshDepthGating() {
    const bool has = session_ && session_->hasDocument();
    const bool deep = has && session_->featuresGated();
    // An action greyed for the depth says why ("... in 32-bit mode" for what Photoshop lacks there, else "... yet");
    // its own tooltip comes back at 8 bits.
    auto gate = [&](QAction* a, bool allowed, bool enabled, const std::string& feature = {}) {
        a->setEnabled(enabled && allowed);
        if (!allowed) {
            if (!a->property("depthTip").isValid()) a->setProperty("depthTip", a->toolTip());
            a->setToolTip(has ? session_->unavailableTip(feature) : QString());
        } else if (a->property("depthTip").isValid()) {
            a->setToolTip(a->property("depthTip").toString());
            a->setProperty("depthTip", QVariant());
        }
    };
    for (QAction* a : documentActions_) {
        const QString feature = actionFeatures_.value(a);
        gate(a, !deep || (!feature.isEmpty() && session_->supportsFeature(feature.toStdString())), has, feature.toStdString());
    }
    for (auto it = toolActions_.begin(); it != toolActions_.end(); ++it) gate(it.value(), !deep || session_->toolSupportedAtDepth(it.key()), true, EditorSession::toolFeature(it.key()));
    gate(eraserAction_, !deep || session_->toolSupportedAtDepth(Tool::Brush), true, "tool.brush");
    // Menus show their items' tooltips while something in them is greyed for the depth.
    for (QMenu* menu : menuBar()->findChildren<QMenu*>()) menu->setToolTipsVisible(deep);
    const SampleType depth = has ? session_->sampleType() : SampleType::U8;
    const ColorMode colorMode = has ? session_->document()->colorMode : ColorMode::RGB;
    if (mode8Action_) {
        mode8Action_->setChecked(depth == SampleType::U8);
        mode16Action_->setChecked(depth == SampleType::U16);
        mode32Action_->setChecked(depth == SampleType::F32);
    }
    if (modeRgbAction_) {
        modeRgbAction_->setChecked(colorMode == ColorMode::RGB);
        modeCmykAction_->setChecked(colorMode == ColorMode::CMYK);
        modeLabAction_->setChecked(colorMode == ColorMode::Lab);
    }
    // 32 bits is RGB only, as in Photoshop: 32 Bits/Channel is greyed in a CMYK or Lab document, and CMYK Color and
    // Lab Color in a 32-bit one.
    if (has && mode32Action_ && colorMode != ColorMode::RGB) gate(mode32Action_, false, true, "document.mode");
    if (has && modeCmykAction_ && depth == SampleType::F32) for (QAction* a : {modeCmykAction_, modeLabAction_}) gate(a, false, true, "mode.cmykLab");
    if (previewOptionsAction_) previewOptionsAction_->setEnabled(depth == SampleType::F32);
    refreshExposure();
}


} // namespace app
