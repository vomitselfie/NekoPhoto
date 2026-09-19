#include "MainWindow.h"
#include "CanvasWidget.h"
#include "Dialogs.h"
#include "ImageConvert.h"
#include "LayersPanel.h"
#include "AdjustmentsPanel.h"
#include "FilterDialog.h"
#include "ToolOptionsBar.h"
#include "compositor/png.h"
#include "compositor/project.h"
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QColorDialog>
#include <QDir>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QImageReader>
#include <QImageWriter>
#include <QInputDialog>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QSettings>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>

using namespace compositor;

namespace app {

namespace {

QString imageFilter() {
    QStringList patterns;
    for (auto& format : QImageReader::supportedImageFormats()) patterns << "*." + QString::fromLatin1(format);
    return QObject::tr("Images (%1)").arg(patterns.join(' '));
}

bool isProjectPath(const QString& path) { return path.endsWith(".comp", Qt::CaseInsensitive) && QFileInfo(path).isDir(); }

} // namespace

MainWindow::MainWindow() {
    session_ = new EditorSession(this);
    canvas_ = new CanvasWidget(session_);
    setCentralWidget(canvas_);
    setAcceptDrops(true);
    resize(1400, 900);

    options_ = new ToolOptionsBar(session_, canvas_);
    addToolBar(Qt::TopToolBarArea, options_);

    auto* dock = new QDockWidget(tr("Layers"), this);
    dock->setObjectName("layersDock");
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);
    layers_ = new LayersPanel(session_);
    layers_->setMinimumWidth(280);
    dock->setWidget(layers_);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    auto* adjustDock = new QDockWidget(tr("Adjustments"), this);
    adjustDock->setObjectName("adjustmentsDock");
    adjustDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable | QDockWidget::DockWidgetClosable);
    adjustDock->setWidget(new AdjustmentsPanel(session_));
    addDockWidget(Qt::RightDockWidgetArea, adjustDock);
    splitDockWidget(dock, adjustDock, Qt::Vertical);

    buildToolRail();
    buildMenus();

    zoomLabel_ = new QLabel;
    positionLabel_ = new QLabel;
    sizeLabel_ = new QLabel;
    statusBar()->addWidget(zoomLabel_);
    statusBar()->addWidget(sizeLabel_);
    statusBar()->addPermanentWidget(positionLabel_);
    connect(canvas_, &CanvasWidget::cursorMoved, this, [this](QPointF p) { positionLabel_->setText(QStringLiteral("%1, %2").arg(int(std::floor(p.x()))).arg(int(std::floor(p.y())))); });
    connect(session_, &EditorSession::viewportChanged, this, [this] { zoomLabel_->setText(QStringLiteral("%1%").arg(session_->viewport.zoom * 100, 0, 'f', session_->viewport.zoom < 0.1 ? 1 : 0)); });
    connect(session_, &EditorSession::titleChanged, this, &MainWindow::refreshTitle);
    connect(session_, &EditorSession::projectPathChanged, this, &MainWindow::refreshTitle);
    connect(session_, &EditorSession::historyChanged, this, &MainWindow::refreshActions);
    connect(session_, &EditorSession::layersChanged, this, &MainWindow::refreshActions);
    connect(session_, &EditorSession::toolChanged, this, [this] {
        if (toolActions_.contains(session_->tool())) toolActions_[session_->tool()]->setChecked(true);
        eraserAction_->setChecked(session_->tool() == Tool::Brush && session_->brushErase);
        updateColorSwatches();
    });
    connect(session_, &EditorSession::error, this, [this](QString message) { showError(tr("Compositor"), message); });
    refreshTitle();
    refreshActions();
    updateColorSwatches();
    QSettings settings;
    restoreGeometry(settings.value("window/geometry").toByteArray());
    restoreState(settings.value("window/state").toByteArray());
}

void MainWindow::buildToolRail() {
    auto* rail = new QToolBar(tr("Tools"), this);
    rail->setObjectName("toolRail");
    rail->setOrientation(Qt::Vertical);
    rail->setMovable(false);
    rail->setIconSize(QSize(20, 20));
    rail->setToolButtonStyle(Qt::ToolButtonTextOnly);
    auto* group = new QActionGroup(this);
    group->setExclusive(true);
    auto tool = [&](Tool t, const QString& label, const QString& text, const QKeySequence& key) {
        QAction* a = rail->addAction(text);
        a->setToolTip(label + " (" + key.toString() + ")");
        a->setCheckable(true);
        a->setShortcut(key);
        a->setShortcutContext(Qt::WindowShortcut);
        group->addAction(a);
        connect(a, &QAction::triggered, this, [this, t] { session_->selectTool(t); if (t == Tool::Brush) { session_->brushErase = false; emit session_->toolChanged(); } canvas_->setFocus(); });
        toolActions_[t] = a;
        return a;
    };
    tool(Tool::Move, tr("Move / Transform"), "V", QKeySequence("V"))->setChecked(true);
    tool(Tool::Marquee, tr("Marquee"), "M", QKeySequence("M"));
    tool(Tool::Lasso, tr("Lasso"), "L", QKeySequence("L"));
    tool(Tool::Wand, tr("Magic Wand"), "W", QKeySequence("W"));
    tool(Tool::Crop, tr("Crop"), "C", QKeySequence("C"));
    rail->addSeparator();
    tool(Tool::Brush, tr("Brush"), "B", QKeySequence("B"));
    eraserAction_ = rail->addAction("E");
    eraserAction_->setToolTip(tr("Eraser (E)"));
    eraserAction_->setCheckable(true);
    eraserAction_->setShortcut(QKeySequence("E"));
    group->addAction(eraserAction_);
    connect(eraserAction_, &QAction::triggered, this, [this] { session_->brushErase = true; session_->selectTool(Tool::Brush); emit session_->toolChanged(); canvas_->setFocus(); });
    tool(Tool::SpotHealing, tr("Spot Healing Brush"), "J", QKeySequence("J"));
    tool(Tool::CloneStamp, tr("Clone Stamp (Alt-click sets the source)"), "S", QKeySequence("S"));
    tool(Tool::Eyedropper, tr("Eyedropper"), "I", QKeySequence("I"));
    rail->addSeparator();
    tool(Tool::Hand, tr("Hand"), "H", QKeySequence("H"));
    tool(Tool::Zoom, tr("Zoom"), "Z", QKeySequence("Z"));
    rail->addSeparator();
    foregroundButton_ = new QToolButton;
    foregroundButton_->setToolTip(tr("Foreground colour"));
    foregroundButton_->setFixedSize(30, 24);
    connect(foregroundButton_, &QToolButton::clicked, this, [this] { chooseColor(false); });
    rail->addWidget(foregroundButton_);
    backgroundButton_ = new QToolButton;
    backgroundButton_->setToolTip(tr("Background colour"));
    backgroundButton_->setFixedSize(30, 24);
    connect(backgroundButton_, &QToolButton::clicked, this, [this] { chooseColor(true); });
    rail->addWidget(backgroundButton_);
    QAction* swap = rail->addAction(tr("⇄"));
    swap->setToolTip(tr("Swap colours (X)"));
    swap->setShortcut(QKeySequence("X"));
    connect(swap, &QAction::triggered, this, [this] { std::swap(session_->foregroundColor, session_->backgroundColor); updateColorSwatches(); });
    QAction* defaults = rail->addAction(tr("D"));
    defaults->setToolTip(tr("Default colours (D)"));
    defaults->setShortcut(QKeySequence("D"));
    connect(defaults, &QAction::triggered, this, [this] { session_->foregroundColor = Qt::black; session_->backgroundColor = Qt::white; updateColorSwatches(); });
    addToolBar(Qt::LeftToolBarArea, rail);
}

void MainWindow::updateColorSwatches() {
    auto swatch = [](QToolButton* b, const QColor& c) {
        QPixmap p(22, 16);
        p.fill(c);
        QPainter painter(&p);
        painter.setPen(QColor(0, 0, 0, 120));
        painter.drawRect(0, 0, 21, 15);
        b->setIcon(QIcon(p));
    };
    swatch(foregroundButton_, session_->foregroundColor);
    swatch(backgroundButton_, session_->backgroundColor);
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
    file->addAction(tr("&Open Project…"), QKeySequence::Open, this, &MainWindow::openProject);
    recentMenu_ = file->addMenu(tr("Open &Recent"));
    file->addAction(tr("&Import Images…"), QKeySequence("Ctrl+Shift+O"), this, &MainWindow::importImages);
    file->addSeparator();
    needsDocument(file->addAction(tr("&Save"), QKeySequence::Save, this, [this] { save(false); }));
    needsDocument(file->addAction(tr("Save &As…"), QKeySequence::SaveAs, this, [this] { save(true); }));
    file->addSeparator();
    needsDocument(file->addAction(tr("Export &PNG…"), QKeySequence("Ctrl+Shift+E"), this, &MainWindow::exportPng));
    needsDocument(file->addAction(tr("Export &JPEG…"), QKeySequence("Ctrl+Alt+Shift+S"), this, &MainWindow::exportJpeg));
    file->addSeparator();
    needsDocument(file->addAction(tr("&Close"), QKeySequence::Close, this, [this] { if (confirmDiscard()) session_->closeDocument(); }));
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
    needsDocument(edit->addAction(tr("Select &All"), QKeySequence::SelectAll, this, [this] { session_->selectAll(); }));
    needsDocument(edit->addAction(tr("&Deselect"), QKeySequence("Ctrl+D"), this, [this] { session_->deselect(); }));
    needsDocument(edit->addAction(tr("&Inverse"), QKeySequence("Ctrl+Shift+I"), this, [this] { session_->invertSelection(); }));
    needsDocument(edit->addAction(tr("Expand Selection…"), this, [this] { bool ok; int n = QInputDialog::getInt(this, tr("Expand Selection"), tr("Pixels"), 1, 1, 500, 1, &ok); if (ok) session_->selectionExpand(n); }));
    needsDocument(edit->addAction(tr("Contract Selection…"), this, [this] { bool ok; int n = QInputDialog::getInt(this, tr("Contract Selection"), tr("Pixels"), 1, 1, 500, 1, &ok); if (ok) session_->selectionContract(n); }));
    edit->addSeparator();
    needsDocument(edit->addAction(tr("Free &Transform"), QKeySequence("Ctrl+T"), this, [this] { session_->selectTool(Tool::Move); session_->beginTransform(true); }));
    needsDocument(edit->addAction(tr("Fill with Foreground"), QKeySequence("Alt+Backspace"), this, [this] { session_->fillSelection(session_->foregroundColor); }));
    needsDocument(edit->addAction(tr("Fill with Background"), QKeySequence("Ctrl+Backspace"), this, [this] { session_->fillSelection(session_->backgroundColor); }));
    needsDocument(edit->addAction(tr("Clear"), QKeySequence(Qt::Key_Delete), this, [this] { if (session_->document() && session_->document()->selection) session_->clearSelectionPixels(); else session_->deleteSelectedLayers(); }));
    needsDocument(edit->addAction(tr("Content-Aware Fill"), QKeySequence("Shift+F5"), this, [this] { QString error; if (!session_->contentAwareFill(&error)) showError(tr("Content-Aware Fill"), error); }));

    QMenu* image = menuBar()->addMenu(tr("&Image"));
    needsDocument(image->addAction(tr("&Canvas Size…"), QKeySequence("Ctrl+Alt+C"), this, [this] {
        auto o = askCanvasSize(this, session_->document()->width, session_->document()->height);
        if (o) session_->resizeCanvas(o->width, o->height, o->anchorX, o->anchorY);
    }));
    needsDocument(image->addAction(tr("&Image Size…"), QKeySequence("Ctrl+Alt+I"), this, [this] {
        auto o = askImageSize(this, session_->document()->width, session_->document()->height, session_->document()->resolution);
        if (o) session_->resizeImage(o->width, o->height, o->resolution);
    }));
    needsDocument(image->addAction(tr("Crop to Selection"), this, [this] {
        const auto& d = session_->document();
        if (d && d->selection) { Rect b = d->selection->bounds(); if (!b.isEmpty()) { session_->cropTo(QRectF(b.x, b.y, b.width, b.height)); session_->deselect(); } }
    }));
    image->addSeparator();
    QMenu* adjustments = image->addMenu(tr("&Adjustments"));
    auto pixelAdjustment = [this, adjustments, &needsDocument](const QString& label, const QKeySequence& key, AdjustmentKind kind) {
        needsDocument(adjustments->addAction(label, key, this, [this, kind] {
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
    needsDocument(adjustments->addAction(tr("&Invert"), QKeySequence("Ctrl+I"), this, [this] { session_->invertActive(); }));
    image->addSeparator();
    needsDocument(image->addAction(tr("Flip Canvas Horizontal"), this, [this] { session_->flipCanvas(true); }));
    needsDocument(image->addAction(tr("Flip Canvas Vertical"), this, [this] { session_->flipCanvas(false); }));

    QMenu* layer = menuBar()->addMenu(tr("&Layer"));
    needsDocument(layer->addAction(tr("&New Layer"), QKeySequence("Ctrl+Shift+N"), this, [this] { session_->addBlankLayer(); }));
    needsDocument(layer->addAction(tr("New &Folder"), this, [this] { session_->addGroup(); }));
    needsDocument(layer->addAction(tr("&Group Layers"), QKeySequence("Ctrl+G"), this, [this] { session_->groupSelectedLayers(); }));
    needsDocument(layer->addAction(tr("&Duplicate Layer / Layer via Copy"), QKeySequence("Ctrl+J"), this, [this] { session_->layerViaCopy(); }));
    needsDocument(layer->addAction(tr("De&lete Layer"), this, [this] { session_->deleteSelectedLayers(); }));
    needsDocument(layer->addAction(tr("Merge &Down"), QKeySequence("Ctrl+E"), this, [this] { session_->mergeDown(); }));
    QMenu* adjustmentLayers = layer->addMenu(tr("New &Adjustment Layer"));
    for (int i = 0; i < 6; i++) {
        AdjustmentKind kind = AdjustmentKind(i);
        needsDocument(adjustmentLayers->addAction(QString::fromUtf8(adjustmentKindName(kind)), this, [this, kind] { session_->addAdjustmentLayer(kind); }));
    }
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

    QMenu* filter = menuBar()->addMenu(tr("Filte&r"));
    auto filterAction = [this, filter, &needsDocument](const QString& label, FilterKind kind) {
        needsDocument(filter->addAction(label, this, [this, kind] {
            if (!session_->canAdjustPixels()) { showError(tr("Filters"), tr("Select a visible image layer (not a mask) to filter its pixels.")); return; }
            (new FilterDialog(session_, kind, this))->show();
        }));
    };
    filterAction(tr("&Gaussian Blur…"), FilterKind::GaussianBlur);
    filterAction(tr("&Motion Blur…"), FilterKind::MotionBlur);
    filterAction(tr("Add &Noise…"), FilterKind::AddNoise);
    filterAction(tr("&Lens Correction…"), FilterKind::LensCorrection);

    QMenu* view = menuBar()->addMenu(tr("&View"));
    needsDocument(view->addAction(tr("Zoom &In"), QKeySequence::ZoomIn, this, [this] { session_->zoomTo(session_->viewport.zoom * 1.25); }));
    needsDocument(view->addAction(tr("Zoom &Out"), QKeySequence::ZoomOut, this, [this] { session_->zoomTo(session_->viewport.zoom / 1.25); }));
    needsDocument(view->addAction(tr("&Fit on Screen"), QKeySequence("Ctrl+0"), this, [this] { session_->fitView(); }));
    needsDocument(view->addAction(tr("&Actual Pixels"), QKeySequence("Ctrl+1"), this, [this] { session_->zoomTo(1); }));
    view->addSeparator();
    QAction* grid = view->addAction(tr("Pixel &Grid"), this, [this](bool on) { session_->showsPixelGrid = on; canvas_->update(); });
    grid->setCheckable(true);
    grid->setChecked(session_->showsPixelGrid);
    QAction* controls = view->addAction(tr("Transform &Controls"), QKeySequence("Ctrl+H"), this, [this](bool on) { session_->showsTransformControls = on; emit session_->transformChanged(); });
    controls->setCheckable(true);
    controls->setChecked(true);

    QMenu* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("&About Compositor"), this, [this] {
        QMessageBox::about(this, tr("About Compositor"), tr("<b>Compositor</b> for Linux<br>A small, focused image compositor.<br><br>"
            "Qt %1 &middot; project format version %2<br>MIT licence.").arg(QT_VERSION_STR).arg(projectFormatVersion));
    });
    refreshRecent();
}

void MainWindow::refreshActions() {
    bool has = session_->hasDocument();
    for (auto* a : documentActions_) a->setEnabled(has);
    undoAction_->setEnabled(session_->canUndo());
    redoAction_->setEnabled(session_->canRedo());
    undoAction_->setText(session_->canUndo() ? tr("&Undo %1").arg(session_->undoName()) : tr("&Undo"));
    redoAction_->setText(session_->canRedo() ? tr("&Redo %1").arg(session_->redoName()) : tr("&Redo"));
    if (has) sizeLabel_->setText(QStringLiteral("%1 × %2 px").arg(session_->document()->width).arg(session_->document()->height));
    else sizeLabel_->clear();
}

void MainWindow::refreshTitle() {
    setWindowTitle(session_->title() + (session_->hasDocument() ? QStringLiteral(" — Compositor") : QString()));
    setWindowModified(session_->isModified());
}

void MainWindow::refreshRecent() {
    recentMenu_->clear();
    QStringList recent = QSettings().value("recent").toStringList();
    for (auto& path : recent) recentMenu_->addAction(QFileInfo(path).fileName(), this, [this, path] { openPath(path); });
    recentMenu_->setEnabled(!recent.isEmpty());
}

void MainWindow::addRecent(const QString& path) {
    QSettings settings;
    QStringList recent = settings.value("recent").toStringList();
    recent.removeAll(path);
    recent.prepend(path);
    while (recent.size() > 10) recent.removeLast();
    settings.setValue("recent", recent);
    refreshRecent();
}

bool MainWindow::confirmDiscard() {
    if (!session_->hasDocument() || !session_->isModified()) return true;
    auto answer = QMessageBox::warning(this, tr("Unsaved Changes"), tr("Save the changes to %1?").arg(session_->title()),
                                       QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Cancel) return false;
    if (answer == QMessageBox::Save) return save(false);
    return true;
}

void MainWindow::closeEvent(QCloseEvent* e) {
    if (!confirmDiscard()) { e->ignore(); return; }
    QSettings settings;
    settings.setValue("window/geometry", saveGeometry());
    settings.setValue("window/state", saveState());
    e->accept();
}

void MainWindow::showError(const QString& title, const QString& message) { QMessageBox::warning(this, title, message); }

void MainWindow::newDocument() {
    if (!confirmDiscard()) return;
    auto options = askNewDocument(this, {});
    if (!options) return;
    session_->createDocument(options->width, options->height, options->resolution, true);
}

void MainWindow::openPath(const QString& path) {
    if (isProjectPath(path)) {
        if (!confirmDiscard()) return;
        QString error;
        if (!session_->openProject(path, &error)) { showError(tr("Couldn’t open the project"), error); return; }
        addRecent(path);
        return;
    }
    importFile(path);
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
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QImage image = reader.read();
    if (image.isNull()) { showError(tr("Couldn’t import %1").arg(QFileInfo(path).fileName()), reader.errorString()); return; }
    if (image.width() > 30000 || image.height() > 30000) { showError(tr("Image too large"), tr("Images up to 30,000 pixels per side are supported.")); return; }
    session_->insertImage(fromQImage(image), QFileInfo(path).completeBaseName(), at);
    addRecent(path);
}

void MainWindow::importImages() {
    QStringList paths = QFileDialog::getOpenFileNames(this, tr("Import Images"), QSettings().value("lastDir").toString(), imageFilter());
    if (paths.isEmpty()) return;
    QSettings().setValue("lastDir", QFileInfo(paths.first()).path());
    for (auto& path : paths) importFile(path);
}

bool MainWindow::save(bool asNew) {
    if (!session_->hasDocument()) return false;
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
    return true;
}

void MainWindow::exportPng() {
    if (!session_->hasDocument()) return;
    QString suggested = QDir(QSettings().value("lastDir").toString()).filePath((session_->projectPath().isEmpty() ? QStringLiteral("Untitled") : QFileInfo(session_->projectPath()).completeBaseName()) + ".png");
    QString path = QFileDialog::getSaveFileName(this, tr("Export PNG"), suggested, tr("PNG image (*.png)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(".png", Qt::CaseInsensitive)) path += ".png";
    auto image = session_->flattened();
    std::string error;
    if (!image || !writePngImage(path.toStdString(), *image, session_->document()->resolution, &error)) showError(tr("Couldn’t export PNG"), QString::fromStdString(error));
}

void MainWindow::exportJpeg() {
    if (!session_->hasDocument()) return;
    auto flattened = session_->flattened();
    if (!flattened) return;
    QImage image = toQImage(*flattened);
    auto options = askJpegExport(this, image);
    if (!options) return;
    QString suggested = QDir(QSettings().value("lastDir").toString()).filePath((session_->projectPath().isEmpty() ? QStringLiteral("Untitled") : QFileInfo(session_->projectPath()).completeBaseName()) + ".jpg");
    QString path = QFileDialog::getSaveFileName(this, tr("Export JPEG"), suggested, tr("JPEG image (*.jpg *.jpeg)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(".jpg", Qt::CaseInsensitive) && !path.endsWith(".jpeg", Qt::CaseInsensitive)) path += ".jpg";
    QImage flat(image.size(), QImage::Format_RGB32);
    flat.fill(options->background);
    QPainter p(&flat);
    p.drawImage(0, 0, image);
    p.end();
    int dpm = int(session_->document()->resolution / 0.0254 + 0.5);
    flat.setDotsPerMeterX(dpm);
    flat.setDotsPerMeterY(dpm);
    QImageWriter writer(path, "jpeg");
    writer.setQuality(options->quality);
    if (!writer.write(flat)) showError(tr("Couldn’t export JPEG"), writer.errorString());
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasUrls() || e->mimeData()->hasImage()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* e) {
    QPointF canvasPoint = canvas_->mapFrom(this, e->position().toPoint());
    std::optional<QPointF> at;
    if (session_->hasDocument() && canvas_->rect().contains(canvasPoint.toPoint())) at = canvas_->documentPoint(canvasPoint);
    if (e->mimeData()->hasUrls()) {
        for (auto& url : e->mimeData()->urls()) {
            if (!url.isLocalFile()) continue;
            QString path = url.toLocalFile();
            if (isProjectPath(path)) openPath(path); else importFile(path, at);
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

} // namespace app
