#include "MainWindow.h"
#include "CanvasFrame.h"
#include "CanvasWidget.h"
#include "Dialogs.h"
#include "ImageConvert.h"
#include "LayersPanel.h"
#include "AdjustmentsPanel.h"
#include "Automation.h"
#include "FilterDialog.h"
#include "ColorSwatches.h"
#include "Icons.h"
#include "ModelStore.h"
#include "PreferencesDialog.h"
#include "compositor/subject.h"
#include "Style.h"
#include "ToolOptionsBar.h"
#include "compositor/png.h"
#include "compositor/project.h"
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
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
#include <QLineEdit>
#include <QLabel>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QStackedWidget>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QTimer>
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

ProjectTabBar::ProjectTabBar(QWidget* parent) : QTabBar(parent) {
    setAcceptDrops(true);
    setExpanding(false);
    setMovable(true);
    setTabsClosable(true);
    setDocumentMode(true);
    setElideMode(Qt::ElideRight);
}

void ProjectTabBar::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasFormat("application/x-compositor-linux-layer")) e->acceptProposedAction(); else QTabBar::dragEnterEvent(e);
}

void ProjectTabBar::dragMoveEvent(QDragMoveEvent* e) {
    if (e->mimeData()->hasFormat("application/x-compositor-linux-layer")) { e->acceptProposedAction(); return; }
    QTabBar::dragMoveEvent(e);
}

void ProjectTabBar::dropEvent(QDropEvent* e) {
    if (!e->mimeData()->hasFormat("application/x-compositor-linux-layer")) { QTabBar::dropEvent(e); return; }
    int index = tabAt(e->position().toPoint());
    emit layerDropped(index, QString::fromUtf8(e->mimeData()->data("application/x-compositor-linux-layer")));
    e->acceptProposedAction();
}

MainWindow::MainWindow() {
    setAcceptDrops(true);
    resize(1400, 900);

    tabBar_ = new ProjectTabBar;
    canvasStack_ = new QStackedWidget;
    auto* central = new QWidget;
    auto* centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);
    centralLayout->addWidget(tabBar_);
    centralLayout->addWidget(canvasStack_, 1);
    setCentralWidget(central);
    connect(tabBar_, &QTabBar::currentChanged, this, [this](int index) { if (index >= 0 && index != current_) switchTo(index); });
    connect(tabBar_, &QTabBar::tabCloseRequested, this, [this](int index) { closeTab(index); });
    connect(tabBar_, &QTabBar::tabMoved, this, [this](int from, int to) { std::swap(tabs_[size_t(from)], tabs_[size_t(to)]); current_ = tabBar_->currentIndex(); });
    connect(tabBar_, &ProjectTabBar::layerDropped, this, &MainWindow::copyLayerFromPayload);

    auto* dock = new QDockWidget(tr("Layers"), this);
    dock->setObjectName("layersDock");
    dock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable);
    layersStack_ = new QStackedWidget;
    layersStack_->setMinimumWidth(200);
    dock->setWidget(layersStack_);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    auto* adjustDock = new QDockWidget(tr("Adjustments"), this);
    adjustDock->setObjectName("adjustmentsDock");
    adjustDock->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable);
    adjustStack_ = new QStackedWidget;
    // Scrolled, so a tall editor (Levels with its histogram) never forces the dock column taller than the screen.
    auto* adjustScroll = new QScrollArea;
    adjustScroll->setWidgetResizable(true);
    adjustScroll->setFrameShape(QFrame::NoFrame);
    adjustScroll->setWidget(adjustStack_);
    adjustDock->setWidget(adjustScroll);
    addDockWidget(Qt::RightDockWidgetArea, adjustDock);
    splitDockWidget(dock, adjustDock, Qt::Vertical);
    layersDock_ = dock;
    adjustDock_ = adjustDock;

    zoomBox_ = new QComboBox;
    zoomBox_->setEditable(true);
    zoomBox_->setInsertPolicy(QComboBox::NoInsert);
    zoomBox_->setToolTip(tr("Zoom (Ctrl+0 fits, Ctrl+1 is 100%)"));
    for (int z : {25, 50, 100, 200, 400, 800}) zoomBox_->addItem(QStringLiteral("%1%").arg(z), z);
    zoomBox_->addItem(tr("Fit"), 0);
    zoomBox_->setMinimumContentsLength(6);
    zoomBox_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    connect(zoomBox_, QOverload<int>::of(&QComboBox::activated), this, [this](int index) {
        int z = zoomBox_->itemData(index).toInt();
        if (z == 0) session_->fitView(); else session_->zoomTo(z / 100.0);
        canvas_->setFocus();
    });
    connect(zoomBox_->lineEdit(), &QLineEdit::returnPressed, this, [this] {
        QString text = zoomBox_->lineEdit()->text().trimmed();
        if (text.compare(tr("Fit"), Qt::CaseInsensitive) == 0) { session_->fitView(); canvas_->setFocus(); return; }
        text.remove('%');
        bool ok = false;
        double z = text.toDouble(&ok);
        if (ok && z > 0) session_->zoomTo(z / 100.0); else refreshZoom();
        canvas_->setFocus();
    });
    positionLabel_ = new QLabel;
    sizeLabel_ = new QLabel;
    hintLabel_ = new QLabel;
    hintLabel_->setStyleSheet(hintStyle());
    statusBar()->addWidget(zoomBox_);
    statusBar()->addWidget(sizeLabel_);
    statusBar()->addWidget(hintLabel_, 1);
    statusBar()->addPermanentWidget(positionLabel_);

    buildToolRail();
    buildMenus();
    addTab(false);
    switchTo(0);
    // Size to the screen: the default 1400x900, or less on small or scaled displays, and never off-screen.
    QSettings settings;
    bool restored = restoreGeometry(settings.value("window/geometry").toByteArray());
    bool onScreen = false;
    for (QScreen* sc : QApplication::screens()) {
        QRect avail = sc->availableGeometry();
        if (avail.intersects(frameGeometry()) && width() <= avail.width() && height() <= avail.height()) onScreen = true;
    }
    if (!restored || !onScreen) {
        QScreen* screen = QApplication::primaryScreen();
        QRect avail = screen ? screen->availableGeometry() : QRect(0, 0, 1400, 900);
        QSize target = QSize(1400, 900).boundedTo(QSize(int(avail.width() * 0.92), int(avail.height() * 0.92)));
        resize(target);
        move(avail.center() - QPoint(target.width() / 2, target.height() / 2));
    }
    // COMPOSITOR_WINDOW_SIZE=WxH forces the initial size, for checking layouts at other sizes.
    QStringList forced = qEnvironmentVariable("COMPOSITOR_WINDOW_SIZE").split('x');
    if (forced.size() == 2) resize(forced[0].toInt(), forced[1].toInt());
    if (!restoreState(settings.value("window/state").toByteArray()))
        QTimer::singleShot(0, this, [this] { resizeDocks({layersDock_, adjustDock_}, {3, 1}, Qt::Vertical); });
}

MainWindow::Tab& MainWindow::addTab(bool reuseEmpty) {
    if (reuseEmpty && current_ >= 0 && !currentTab().session->hasDocument()) return currentTab();
    Tab tab;
    tab.defaultName = tabs_.empty() ? tr("Untitled") : tr("Untitled %1").arg(nextNumber_++);
    tab.session = new EditorSession(this);
    tab.canvas = new CanvasWidget(tab.session);
    tab.frame = new CanvasFrame(tab.session, tab.canvas);
    tab.frame->setRulersVisible(rulersAction_ && rulersAction_->isChecked());
    tab.layers = new LayersPanel(tab.session);
    tab.adjustments = new AdjustmentsPanel(tab.session);
    tab.options = new ToolOptionsBar(tab.session, tab.canvas);
    addToolBar(Qt::TopToolBarArea, tab.options);
    tab.options->setVisible(false);
    canvasStack_->addWidget(tab.frame);
    layersStack_->addWidget(tab.layers);
    adjustStack_->addWidget(tab.adjustments);
    tabs_.push_back(tab);
    int index = int(tabs_.size()) - 1;
    { QSignalBlocker b(tabBar_); tabBar_->addTab(tab.defaultName); }
    connect(tab.canvas, &CanvasWidget::cursorMoved, this, [this](QPointF p) { positionLabel_->setText(QStringLiteral("%1, %2").arg(int(std::floor(p.x()))).arg(int(std::floor(p.y())))); });
    connect(tab.session, &EditorSession::projectPathChanged, this, &MainWindow::refreshTabTitles);
    connect(tab.session, &EditorSession::titleChanged, this, &MainWindow::refreshTabTitles);
    tabBar_->setCurrentIndex(index);
    if (current_ != index) switchTo(index);
    return tabs_[size_t(index)];
}

void MainWindow::switchTo(int index) {
    if (index < 0 || index >= int(tabs_.size())) return;
    if (current_ >= 0 && current_ < int(tabs_.size()) && current_ != index) {
        currentTab().session->commitTransform();
        currentTab().session->resolveGradient();
        currentTab().options->setVisible(false);
    }
    current_ = index;
    Tab& tab = currentTab();
    session_ = tab.session;
    canvas_ = tab.canvas;
    layers_ = tab.layers;
    options_ = tab.options;
    canvasStack_->setCurrentWidget(tab.frame);
    layersStack_->setCurrentWidget(tab.layers);
    adjustStack_->setCurrentWidget(tab.adjustments);
    tab.options->setVisible(true);
    { QSignalBlocker b(tabBar_); tabBar_->setCurrentIndex(index); }
    connectSession();
    refreshTitle();
    refreshActions();
    updateColorSwatches();
    if (toolActions_.contains(session_->tool())) toolActions_[session_->tool()]->setChecked(true);
    refreshZoom();
    refreshHint();
    canvas_->setFocus();
}

void MainWindow::refreshHint() { hintLabel_->setText(toolHint(session_->tool(), session_->brushErase)); }

QString MainWindow::toolHint(Tool tool, bool erase) {
    switch (tool) {
    case Tool::Move: return tr("Drag to move; handles scale, just outside a corner rotates; Ctrl-drag a handle distorts; Ctrl-click picks a layer");
    case Tool::Marquee: return tr("Drag to select; Shift adds, Alt subtracts; drag inside a selection to move its outline");
    case Tool::Lasso: return tr("Freehand: drag around an area. Polygonal: click points, double-click or Enter closes, Backspace removes the last");
    case Tool::Wand: return tr("Click a colour to select it; Shift adds, Alt subtracts; Tolerance widens the match");
    case Tool::Crop: return tr("Drag the crop, then press Enter or double-click; Shift squares, Alt grows from the centre");
    case Tool::Brush: return erase ? tr("Drag to erase; [ and ] change the size; Shift-click erases a straight line")
                                   : tr("Drag to paint; [ and ] change the size, digits set the opacity; Shift-click paints a straight line");
    case Tool::SpotHealing: return tr("Paint over a blemish and it is filled from its surroundings");
    case Tool::CloneStamp: return tr("Alt-click sets the source, then paint");
    case Tool::Smudge: return tr("Opacity is the strength; Liquify pushes pixels, Smudge drags colour, Blur softens");
    case Tool::Gradient: return tr("Drag a line; drag again to redo it; Enter applies, Esc discards; Shift snaps the angle");
    case Tool::Shape: return tr("Drag a shape in the foreground colour; Shift squares, Alt grows from the centre; Shift-U switches kind");
    case Tool::Eyedropper: return tr("Click sets the foreground colour, Alt-click the background");
    case Tool::Hand: return tr("Drag to pan; hold Space to pan from any tool");
    case Tool::Zoom: return tr("Click zooms in, Alt-click out, drag a box to zoom to it; Ctrl-wheel zooms anywhere");
    }
    return {};
}

void MainWindow::refreshZoom() {
    zoomBox_->lineEdit()->setText(QStringLiteral("%1%").arg(session_->viewport.zoom * 100, 0, 'f', session_->viewport.zoom < 0.1 ? 1 : 0));
}

void MainWindow::connectSession() {
    for (auto& c : sessionConnections_) disconnect(c);
    sessionConnections_.clear();
    sessionConnections_.push_back(connect(session_, &EditorSession::viewportChanged, this, &MainWindow::refreshZoom));
    sessionConnections_.push_back(connect(session_, &EditorSession::titleChanged, this, &MainWindow::refreshTitle));
    sessionConnections_.push_back(connect(session_, &EditorSession::projectPathChanged, this, &MainWindow::refreshTitle));
    sessionConnections_.push_back(connect(session_, &EditorSession::historyChanged, this, &MainWindow::refreshActions));
    sessionConnections_.push_back(connect(session_, &EditorSession::documentChanged, this, [this] { emit automationEvent("document"); }));
    sessionConnections_.push_back(connect(session_, &EditorSession::layersChanged, this, [this] { emit automationEvent("layers"); }));
    sessionConnections_.push_back(connect(session_, &EditorSession::selectionChanged, this, [this] { emit automationEvent("selection"); }));
    sessionConnections_.push_back(connect(session_, &EditorSession::historyChanged, this, [this] { emit automationEvent("history"); }));
    sessionConnections_.push_back(connect(session_, &EditorSession::toolChanged, this, [this] { emit automationEvent("tool"); }));
    sessionConnections_.push_back(connect(session_, &EditorSession::viewportChanged, this, [this] { emit automationEvent("view"); }));
    emit automationEvent("tabs");
    sessionConnections_.push_back(connect(session_, &EditorSession::layersChanged, this, &MainWindow::refreshActions));
    sessionConnections_.push_back(connect(session_, &EditorSession::toolChanged, this, [this] {
        if (toolActions_.contains(session_->tool())) toolActions_[session_->tool()]->setChecked(true);
        eraserAction_->setChecked(session_->tool() == Tool::Brush && session_->brushErase);
        updateColorSwatches();
        refreshHint();
    }));
    sessionConnections_.push_back(connect(session_, &EditorSession::error, this, [this](QString message) { showError(tr("compositor-linux"), message); }));
}

void MainWindow::refreshTabTitles() {
    for (size_t i = 0; i < tabs_.size(); i++) {
        Tab& tab = tabs_[i];
        QString name = tab.session->projectPath().isEmpty() ? tab.defaultName : QFileInfo(tab.session->projectPath()).completeBaseName();
        if (tab.session->isModified()) name += " *";
        tabBar_->setTabText(int(i), name);
        tabBar_->setTabToolTip(int(i), tab.session->projectPath());
    }
}

bool MainWindow::confirmDiscard(int index) {
    if (index < 0 || index >= int(tabs_.size()) || skipConfirm_) return true;
    EditorSession* s = tabs_[size_t(index)].session;
    if (!s->hasDocument() || !s->isModified()) return true;
    if (index != current_) switchTo(index);
    auto answer = QMessageBox::warning(this, tr("Unsaved Changes"), tr("Save the changes to %1?").arg(s->title()),
                                       QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Cancel) return false;
    if (answer == QMessageBox::Save) return save(false);
    return true;
}

void MainWindow::closeTab(int index) {
    if (index < 0 || index >= int(tabs_.size()) || !confirmDiscard(index)) return;
    Tab tab = tabs_[size_t(index)];
    tabs_.erase(tabs_.begin() + index);
    { QSignalBlocker b(tabBar_); tabBar_->removeTab(index); }
    canvasStack_->removeWidget(tab.frame);
    layersStack_->removeWidget(tab.layers);
    adjustStack_->removeWidget(tab.adjustments);
    removeToolBar(tab.options);
    tab.frame->deleteLater(); tab.layers->deleteLater(); tab.adjustments->deleteLater(); tab.options->deleteLater();
    tab.session->deleteLater();
    current_ = -1;
    if (tabs_.empty()) { addTab(false); return; }
    switchTo(std::min(index, int(tabs_.size()) - 1));
}

void MainWindow::copyLayerFromPayload(int tabIndex, const QString& payload) {
    QStringList parts = payload.split(':');
    if (parts.size() != 2) return;
    quintptr key = parts[0].toULongLong();
    EditorSession* source = nullptr;
    for (auto& t : tabs_) if (quintptr(t.session) == key) source = t.session;
    if (!source) return;
    Tab* target = nullptr;
    if (tabIndex < 0) target = &addTab(false); else target = &tabs_[size_t(tabIndex)];
    if (target->session == source) return;
    QString error;
    if (!target->session->copyLayerFrom(*source, parts[1].toStdString(), std::nullopt, &error)) { if (!error.isEmpty()) showError(tr("Copy Layer"), error); return; }
    int index = int(target - tabs_.data());
    if (index != current_) switchTo(index);
}

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
    tool(Tool::Gradient, tr("Gradient"), "blend", QKeySequence("G"));
    tool(Tool::Shape, tr("Shape (Shift-U switches Rectangle / Ellipse)"), "shapes", QKeySequence("U"));
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
    needsDocument(edit->addAction(tr("Select &All"), QKeySequence::SelectAll, this, [this] { session_->selectAll(); }));
    needsDocument(edit->addAction(tr("&Deselect"), QKeySequence("Ctrl+D"), this, [this] { session_->deselect(); }));
    needsDocument(edit->addAction(tr("&Inverse"), QKeySequence("Ctrl+Shift+I"), this, [this] { session_->invertSelection(); }));
    needsDocument(edit->addAction(tr("Expand Selection…"), this, [this] { bool ok; int n = QInputDialog::getInt(this, tr("Expand Selection"), tr("Pixels"), 1, 1, 500, 1, &ok); if (ok) session_->selectionExpand(n); }));
    needsDocument(edit->addAction(tr("Contract Selection…"), this, [this] { bool ok; int n = QInputDialog::getInt(this, tr("Contract Selection"), tr("Pixels"), 1, 1, 500, 1, &ok); if (ok) session_->selectionContract(n); }));
    edit->addSeparator();
    needsDocument(edit->addAction(tr("Free &Transform"), QKeySequence("Ctrl+T"), this, [this] { session_->transformCommand(); }));
    needsDocument(edit->addAction(tr("Fill with Foreground"), QKeySequence("Alt+Backspace"), this, [this] { session_->fillSelection(session_->foregroundColor); }));
    needsDocument(edit->addAction(tr("Fill with Background"), QKeySequence("Ctrl+Backspace"), this, [this] { session_->fillSelection(session_->backgroundColor); }));
    QAction* clear = needsDocument(edit->addAction(tr("Clear"), QKeySequence(Qt::Key_Delete), this, [this] { if (session_->document() && session_->document()->selection) session_->clearSelectionPixels(); else deleteSelectedLayers(); }));
    clear->setShortcuts({QKeySequence(Qt::Key_Delete), QKeySequence(Qt::Key_Backspace)});
    QMenu* load = edit->addMenu(tr("Load as Selection"));
    needsDocument(load->addAction(tr("Layer Pixels"), this, [this] { if (session_->activeLayerId()) session_->loadLayerAsSelection(*session_->activeLayerId(), false, SelectionMode::Replace); }));
    needsDocument(load->addAction(tr("Layer Mask"), this, [this] { if (session_->activeLayerId()) session_->loadLayerAsSelection(*session_->activeLayerId(), true, SelectionMode::Replace); }));
    needsDocument(load->addAction(tr("Add Layer Pixels"), this, [this] { if (session_->activeLayerId()) session_->loadLayerAsSelection(*session_->activeLayerId(), false, SelectionMode::Add); }));
    needsDocument(load->addAction(tr("Subtract Layer Pixels"), this, [this] { if (session_->activeLayerId()) session_->loadLayerAsSelection(*session_->activeLayerId(), false, SelectionMode::Subtract); }));
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
    needsDocument(layer->addAction(tr("New Layer &Below"), this, [this] { session_->addBlankLayer(true); }));
    needsDocument(layer->addAction(tr("New &Folder"), this, [this] { session_->addGroup(); }));
    needsDocument(layer->addAction(tr("&Group Layers"), QKeySequence("Ctrl+G"), this, [this] { session_->groupSelectedLayers(); }));
    needsDocument(layer->addAction(tr("Layer via &Copy"), QKeySequence("Ctrl+J"), this, [this] { session_->layerViaCopy(); }));
    needsDocument(layer->addAction(tr("&Duplicate Layer"), this, [this] { session_->duplicateActiveLayer(); }));
    needsDocument(layer->addAction(tr("De&lete Layer"), this, [this] { deleteSelectedLayers(); }));
    mergeAction_ = needsDocument(layer->addAction(tr("Merge &Down"), QKeySequence("Ctrl+E"), this, [this] { session_->mergeLayers(); }));
    needsDocument(layer->addAction(tr("&Rename Layer…"), this, [this] {
        const Layer* active = session_->activeLayer();
        if (!active) return;
        bool ok;
        QString name = QInputDialog::getText(this, tr("Rename Layer"), tr("Name"), QLineEdit::Normal, QString::fromStdString(active->name), &ok);
        if (ok) session_->renameLayer(active->id, name);
    }));
    needsDocument(layer->addAction(tr("Move &Out of Folder"), QKeySequence("Ctrl+Shift+["), this, [this] { session_->moveActiveLayerOutOfGroup(); }));
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
    filter->addSeparator();
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
        if (!session_->canAdjustPixels()) { showError(tr("Remove Background"), tr("Select a visible image layer (not a mask) to remove its background.")); return; }
        (new BackgroundDialog(session_, ModelStore::pathFor(ModelStore::selected()), this))->show();
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
    view->addAction(adjustDock_->toggleViewAction());
    view->addSeparator();
    QAction* grid = view->addAction(tr("Pixel &Grid"), this, [this](bool on) { session_->showsPixelGrid = on; canvas_->update(); });
    grid->setCheckable(true);
    grid->setChecked(true);
    QAction* controls = view->addAction(tr("Transform &Controls"), QKeySequence("Ctrl+H"), this, [this](bool on) { session_->showsTransformControls = on; emit session_->transformChanged(); });
    controls->setCheckable(true);
    controls->setChecked(true);

    QMenu* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("&About compositor-linux"), this, [this] {
        QMessageBox::about(this, tr("About compositor-linux"), tr("<b>compositor-linux</b> %3<br>A small, focused image compositor. "
            "A Linux port of <a href=\"https://github.com/robbietilton/Compositor\">Compositor</a> for macOS.<br><br>"
            "Qt %1 &middot; project format version %2<br>MIT licence.").arg(QT_VERSION_STR).arg(projectFormatVersion).arg(QApplication::applicationVersion()));
    });
    refreshRecent();
}

void MainWindow::deleteSelectedLayers() {
    if (!session_->hasDocument()) return;
    std::vector<Uuid> ids;
    for (auto& l : session_->document()->layers) if (session_->selectedLayerIds().count(l.id)) ids.push_back(l.id);
    if (ids.empty() && session_->activeLayerId()) ids.push_back(*session_->activeLayerId());
    if (ids.empty()) return;
    auto dependents = session_->clippingDependents(ids);
    if (dependents.empty()) { session_->deleteLayersResolvingClipping(ids, false); return; }
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setText(ids.size() == 1 ? tr("This layer supplies a clipping mask") : tr("These layers supply clipping masks"));
    box.setInformativeText(tr("Bake keeps the current masked appearance in the dependent layers’ pixels. Remove Links reveals their pixels. You can undo either choice."));
    QPushButton* bake = box.addButton(tr("Bake and Delete"), QMessageBox::AcceptRole);
    QPushButton* remove = box.addButton(tr("Remove Links and Delete"), QMessageBox::DestructiveRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(bake);
    box.exec();
    if (box.clickedButton() == bake) session_->deleteLayersResolvingClipping(ids, true);
    else if (box.clickedButton() == remove) session_->deleteLayersResolvingClipping(ids, false);
}

void MainWindow::showPreferences() {
    auto* dialog = new PreferencesDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &PreferencesDialog::backgroundRemovalChanged, this, &MainWindow::refreshBackgroundAction);
    dialog->show();
}

void MainWindow::refreshBackgroundAction() {
    if (!removeBackgroundAction_) return;
    QString tip;
    if (!ModelStore::supported()) tip = tr("Unavailable: this build has no OpenCV");
    else if (!ModelStore::enabled()) tip = tr("Off: enable it in Edit > Preferences");
    else if (!ModelStore::isPresent(ModelStore::selected())) tip = tr("The model isn’t downloaded yet: see Edit > Preferences");
    else tip = tr("Hide the background of the active layer with a mask (%1)").arg(ModelStore::selected().label);
    removeBackgroundAction_->setToolTip(tip);
    removeBackgroundAction_->setText(ModelStore::ready() ? tr("Remove &Background…") : tr("Remove &Background (off)…"));
}

void MainWindow::refreshActions() {
    bool has = session_->hasDocument();
    for (auto* a : documentActions_) a->setEnabled(has);
    if (mergeAction_) { mergeAction_->setText(tr("&%1").arg(session_->mergeTitle())); mergeAction_->setEnabled(has && session_->canMergeLayers()); }
    undoAction_->setEnabled(session_->canUndo());
    redoAction_->setEnabled(session_->canRedo());
    undoAction_->setText(session_->canUndo() ? tr("&Undo %1").arg(session_->undoName()) : tr("&Undo"));
    redoAction_->setText(session_->canRedo() ? tr("&Redo %1").arg(session_->redoName()) : tr("&Redo"));
    if (has) sizeLabel_->setText(QStringLiteral("%1 × %2 px").arg(session_->document()->width).arg(session_->document()->height));
    else sizeLabel_->clear();
}

void MainWindow::refreshTitle() {
    if (!session_) return;
    setWindowTitle(session_->title() + (session_->hasDocument() ? QStringLiteral(" — compositor-linux") : QString()));
    setWindowModified(session_->isModified());
    refreshTabTitles();
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

bool MainWindow::confirmDiscard() { return confirmDiscard(current_); }

void MainWindow::closeEvent(QCloseEvent* e) {
    // The tab on screen first, then the rest left to right.
    std::vector<int> order{current_};
    for (int i = 0; i < int(tabs_.size()); i++) if (i != current_) order.push_back(i);
    for (int i : order) if (!confirmDiscard(i)) { e->ignore(); return; }
    QSettings settings;
    settings.setValue("window/geometry", saveGeometry());
    settings.setValue("window/state", saveState());
    e->accept();
}

void MainWindow::showError(const QString& title, const QString& message) {
    if (errorSink_) { *errorSink_ += title + ": " + message + "\n"; return; }
    QMessageBox::warning(this, title, message);
}

QString MainWindow::tabTitle(int i) const {
    const Tab& tab = tabs_[size_t(i)];
    return tab.session->projectPath().isEmpty() ? tab.defaultName : QFileInfo(tab.session->projectPath()).completeBaseName();
}

bool MainWindow::startAutomation(const QString& socketPath) {
    if (automation_) return true;
    automation_ = new AutomationServer(this);
    QString path = socketPath.isEmpty() ? AutomationServer::defaultSocketPath() : socketPath, error;
    if (!automation_->listen(path, &error)) {
        qWarning("automation: couldn't listen on %s: %s", qPrintable(path), qPrintable(error));
        automation_->deleteLater();
        automation_ = nullptr;
        return false;
    }
    qInfo("automation: listening on %s", qPrintable(path));
    automationLabel_ = new QLabel;
    automationLabel_->setToolTip(tr("An agent is connected to the automation socket at %1").arg(path));
    automationLabel_->setStyleSheet(QStringLiteral("color: palette(highlight); font-weight: bold;"));
    automationLabel_->setVisible(false);
    statusBar()->addPermanentWidget(automationLabel_);
    connect(automation_, &AutomationServer::clientsChanged, this, [this](int count) {
        automationLabel_->setText(count > 0 ? tr("Agent connected") : QString());
        automationLabel_->setVisible(count > 0);
    });
    return true;
}

void MainWindow::newDocument() {
    auto options = askNewDocument(this, {});
    if (!options) return;
    Tab& tab = addTab(true);
    tab.session->createDocument(options->width, options->height, options->resolution, true);
}

void MainWindow::openPath(const QString& path) {
    if (isProjectPath(path)) {
        QString canonical = QFileInfo(path).canonicalFilePath();
        for (size_t i = 0; i < tabs_.size(); i++)
            if (!tabs_[i].session->projectPath().isEmpty() && QFileInfo(tabs_[i].session->projectPath()).canonicalFilePath() == canonical) { switchTo(int(i)); return; }
        // Load into a fresh session first, so a failed open never disturbs a tab.
        auto* probe = new EditorSession(this);
        QString error;
        bool ok = probe->openProject(path, &error);
        probe->deleteLater();
        if (!ok) { showError(tr("Couldn’t open the project"), error); return; }
        Tab& tab = addTab(true);
        if (!tab.session->openProject(path, &error)) { showError(tr("Couldn’t open the project"), error); return; }
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
    QString error;
    if (!importImageFile(path, at, &error)) showError(tr("Couldn’t import %1").arg(QFileInfo(path).fileName()), error);
}

bool MainWindow::importImageFile(const QString& path, std::optional<QPointF> at, QString* error) {
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QImage image = reader.read();
    if (image.isNull()) { if (error) *error = reader.errorString(); return false; }
    if (image.width() > 30000 || image.height() > 30000) { if (error) *error = tr("Images up to 30,000 pixels per side are supported."); return false; }
    session_->insertImage(fromQImage(image), QFileInfo(path).completeBaseName(), at);
    addRecent(path);
    return true;
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
