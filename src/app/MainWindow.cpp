#include "MainWindow.h"
#include "Scrub.h"
#include "ImportBanner.h"
#include "Names.h"
#include "BrushImporter.h"
#include "CanvasFrame.h"
#include "Autosave.h"
#include "LayersPanel.h"
#include "PathsPanel.h"
#include "ChannelsPanel.h"
#include "AdjustmentsPanel.h"
#include "Automation.h"
#include "ActionLibrary.h"
#include "ActionsPanel.h"
#include "TimelinePanel.h"
#include "HistogramPanel.h"
#include "TextDialog.h"
#include "PreferencesDialog.h"
#include "ToolOptionsBar.h"
#include "WelcomeDialog.h"
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDockWidget>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QLineEdit>
#include <QLabel>
#include <QHBoxLayout>
#include <QSlider>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QStackedWidget>
#include <QStatusBar>
#include <QVBoxLayout>
#include <QTimer>

using namespace compositor;

namespace app {

ProjectTabBar::ProjectTabBar(QWidget* parent) : QTabBar(parent) {
    setAcceptDrops(true);
    setExpanding(false);
    setMovable(true);
    setTabsClosable(true);
    setDocumentMode(true);
    setElideMode(Qt::ElideRight);
}

void ProjectTabBar::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasFormat("application/x-nekophoto-layer")) e->acceptProposedAction(); else QTabBar::dragEnterEvent(e);
}

void ProjectTabBar::dragMoveEvent(QDragMoveEvent* e) {
    if (e->mimeData()->hasFormat("application/x-nekophoto-layer")) { e->acceptProposedAction(); return; }
    QTabBar::dragMoveEvent(e);
}

void ProjectTabBar::dropEvent(QDropEvent* e) {
    if (!e->mimeData()->hasFormat("application/x-nekophoto-layer")) { QTabBar::dropEvent(e); return; }
    int index = tabAt(e->position().toPoint());
    emit layerDropped(index, QString::fromUtf8(e->mimeData()->data("application/x-nekophoto-layer")));
    e->acceptProposedAction();
}

MainWindow::MainWindow() {
    setAcceptDrops(true);
    // A drag on a scrubby label is one undo step, whatever the field it moves records.
    scrub::setUndoGroupHooks(
        [this](const QString& name) {
            if (!session_ || !session_->canEditLayers()) return false;
            scrubSession_ = session_;
            session_->beginEdit(name);
            return true;
        },
        [this] {
            if (!scrubSession_) return;
            scrubSession_->endEdit();
            emit scrubSession_->historyChanged();
            emit scrubSession_->titleChanged();
            scrubSession_.clear();
        });
    resize(1400, 900);

    tabBar_ = new ProjectTabBar;
    canvasStack_ = new QStackedWidget;
    auto* central = new QWidget;
    auto* centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);
    centralLayout->addWidget(tabBar_);
    importBanner_ = new ImportBanner;
    centralLayout->addWidget(importBanner_);
    centralLayout->addWidget(canvasStack_, 1);
    connect(importBanner_, &ImportBanner::dismissed, this, [this] { bannerSession_.clear(); });
    connect(importBanner_, &ImportBanner::undoRequested, this, [this] {
        // Undo Open: the document's tab closes (asking first if it was changed since).
        for (size_t i = 0; i < tabs_.size(); i++)
            if (tabs_[i].session == bannerSession_) { bannerSession_.clear(); closeTab(int(i)); break; }
    });
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
    // Paths, tabbed with Layers as in Photoshop.
    pathsDock_ = new QDockWidget(tr("Paths"), this);
    pathsDock_->setObjectName("pathsDock");
    pathsDock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable);
    pathsStack_ = new QStackedWidget;
    pathsDock_->setWidget(pathsStack_);
    addDockWidget(Qt::RightDockWidgetArea, pathsDock_);
    // Channels, between Layers and Paths as in Photoshop.
    channelsDock_ = new QDockWidget(tr("Channels"), this);
    channelsDock_->setObjectName("channelsDock");
    channelsDock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable);
    channelsStack_ = new QStackedWidget;
    channelsDock_->setWidget(channelsStack_);
    addDockWidget(Qt::RightDockWidgetArea, channelsDock_);
    tabifyDockWidget(dock, channelsDock_);
    tabifyDockWidget(channelsDock_, pathsDock_);
    dock->raise();   // Layers is the tab in front
    adjustDock_ = adjustDock;
    // Histogram (Window menu), a tab beside Adjustments: it shares that column's width rather than widening it.
    histogramDock_ = new QDockWidget(tr("Histogram"), this);
    histogramDock_->setObjectName("histogramDock");
    histogramDock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetFloatable);
    histogram_ = new HistogramPanel;
    // Scrolled too, so the Expanded View's statistics never make the column taller or squeeze the Layers panel.
    auto* histogramScroll = new QScrollArea;
    histogramScroll->setWidgetResizable(true);
    histogramScroll->setFrameShape(QFrame::NoFrame);
    histogramScroll->setWidget(histogram_);
    histogramDock_->setWidget(histogramScroll);
    addDockWidget(Qt::RightDockWidgetArea, histogramDock_);
    tabifyDockWidget(adjustDock_, histogramDock_);
    adjustDock_->raise();
    // Actions and Timeline (Window menu), hidden until asked for.
    actionsDock_ = new QDockWidget(tr("Actions"), this);
    actionsDock_->setObjectName("actionsDock");
    actionsDock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetFloatable);
    auto* actions = new ActionsPanel([this](const QString& name) { return playAction(name); });
    connect(actions, &ActionsPanel::batchRequested, this, [this](const QString& name) { showBatchDialog(name); });
    actionsDock_->setWidget(actions);
    addDockWidget(Qt::RightDockWidgetArea, actionsDock_);
    actionsDock_->hide();
    timelineDock_ = new QDockWidget(tr("Timeline"), this);
    timelineDock_->setObjectName("timelineDock");
    timelineDock_->setFeatures(QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetFloatable);
    timeline_ = new TimelinePanel;
    timelineDock_->setWidget(timeline_);
    addDockWidget(Qt::BottomDockWidgetArea, timelineDock_);
    timelineDock_->hide();
    connect(timelineDock_, &QDockWidget::visibilityChanged, this, [this](bool shown) { if (shown && timeline_) timeline_->setSession(session_); });

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
    // A 32-bit document's exposure (docs/bit-depth.md, "32 bits"), Photoshop's status-bar slider: the view, not the
    // pixels. Hidden for 8- and 16-bit documents.
    exposureBox_ = new QWidget;
    auto* exposureLayout = new QHBoxLayout(exposureBox_);
    exposureLayout->setContentsMargins(0, 0, 0, 0);
    exposureSlider_ = new QSlider(Qt::Horizontal);
    exposureSlider_->setRange(int(View32::minExposure * 100), int(View32::maxExposure * 100));
    exposureSlider_->setFixedWidth(120);
    exposureSlider_->setToolTip(tr("Exposure of the 32-bit preview, in stops (the pixels are not changed)"));
    exposureLabel_ = new QLabel;
    exposureLabel_->setMinimumWidth(QFontMetrics(exposureLabel_->font()).horizontalAdvance(QStringLiteral("-20.00")) + 4);
    exposureLayout->addWidget(new QLabel(tr("Exposure")));
    exposureLayout->addWidget(exposureSlider_);
    exposureLayout->addWidget(exposureLabel_);
    connect(exposureSlider_, &QSlider::valueChanged, this, [this](int value) {
        View32 v = session_->view32();
        if (std::lround(v.exposure * 100) == value) return;
        v.exposure = value / 100.0;
        session_->setView32(v);
    });
    exposureBox_->hide();
    statusBar()->addWidget(exposureBox_);
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
    // A layout saved before the Paths panel existed places it on its own: once, it joins Layers as a tab.
    if (settings.value("window/pathsDockPlaced").toInt() < 1) {
        tabifyDockWidget(layersDock_, pathsDock_);
        pathsDock_->show();
        layersDock_->raise();
        settings.setValue("window/pathsDockPlaced", 1);
    }
    // Likewise a layout saved before the Channels panel existed: it joins Layers as a tab once.
    if (settings.value("window/channelsDockPlaced").toInt() < 1) {
        tabifyDockWidget(layersDock_, channelsDock_);
        tabifyDockWidget(channelsDock_, pathsDock_);   // Photoshop's order: Layers, Channels, Paths
        channelsDock_->show();
        layersDock_->raise();
        settings.setValue("window/channelsDockPlaced", 1);
    }
    // And one saved before the Histogram panel: it joins Adjustments as a tab once, behind it.
    if (settings.value("window/histogramDockPlaced").toInt() < 1) {
        tabifyDockWidget(adjustDock_, histogramDock_);
        histogramDock_->show();
        adjustDock_->raise();
        settings.setValue("window/histogramDockPlaced", 1);
    }
    // The saved state remembers each tab's options bar by name, and only the current tab's is visible when the
    // window closes; restoring it could hide the bar of the tab this launch shows. The current tab owns the bar.
    for (size_t i = 0; i < tabs_.size(); i++) tabs_[i].options->setVisible(int(i) == current_);
}

MainWindow::Tab& MainWindow::addTab(bool reuseEmpty) {
    if (reuseEmpty && current_ >= 0 && !currentTab().session->hasDocument()) return currentTab();
    Tab tab;
    tab.defaultName = tabs_.empty() ? tr("Untitled") : tr("Untitled %1").arg(nextNumber_++);
    tab.session = new EditorSession(this);
    // The command path (EditorSession::runCommand): edits that menus, dialogs and the canvas make through the
    // automation registry, for the tab on screen.
    tab.session->commandRunner = [this](const QString& method, const QJsonObject& params) { return runCommand(method, params); };
    tab.session->commandReady = [this, session = tab.session] { return session == session_; };
    tab.canvas = new CanvasWidget(tab.session);
    connect(tab.canvas, &CanvasWidget::contextMenuRequested, this, [this, canvas = tab.canvas](QPointF at) { if (canvas == canvas_) showCanvasMenu(at); });
    tab.frame = new CanvasFrame(tab.session, tab.canvas);
    tab.frame->setRulersVisible(rulersAction_ && rulersAction_->isChecked());
    tab.layers = new LayersPanel(tab.session);
    // A pixel edit on a smart object: its contents instead, or pixels (Photoshop's choice).
    connect(tab.session, &EditorSession::smartObjectPixelsRequested, this, [this, session = tab.session](const Uuid& id) {
        if (session != session_) return;
        const Layer* layer = session_->document()->find(id);
        if (!layer) return;
        QMessageBox box(QMessageBox::Question, tr("Smart object"),
                        tr("“%1” is a smart object. Painting or filtering it would replace its contents with pixels.").arg(QString::fromStdString(layer->name)),
                        QMessageBox::NoButton, this);
        box.setInformativeText(layer->smartObject->locked() ? tr("Rasterize it to work on its pixels.") : tr("Edit its contents instead, or rasterize it to work on its pixels."));
        QPushButton* edit = layer->smartObject->locked() ? nullptr : box.addButton(tr("Edit Contents"), QMessageBox::AcceptRole);
        QPushButton* rasterize = box.addButton(tr("Rasterize"), QMessageBox::DestructiveRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(edit ? edit : rasterize);
        box.exec();
        if (edit && box.clickedButton() == edit) editSmartObjectContents();
        else if (box.clickedButton() == rasterize) { session_->rasterizeSmartObject(); statusBar()->showMessage(tr("Rasterized: paint again to work on its pixels."), 5000); }
    });
    connect(tab.layers, &LayersPanel::smartObjectContentsRequested, this, [this, session = tab.session](const Uuid& id) {
        if (session != session_) return;
        session_->selectLayer(id);
        editSmartObjectContents();
    });
    tab.adjustments = new AdjustmentsPanel(tab.session);
    tab.options = new ToolOptionsBar(tab.session, tab.canvas);
    addToolBar(Qt::TopToolBarArea, tab.options);
    tab.options->setVisible(false);
    canvasStack_->addWidget(tab.frame);
    layersStack_->addWidget(tab.layers);
    tab.paths = new PathsPanel(tab.session);
    pathsStack_->addWidget(tab.paths);
    tab.channels = new ChannelsPanel(tab.session);
    channelsStack_->addWidget(tab.channels);
    adjustStack_->addWidget(tab.adjustments);
    tabs_.push_back(tab);
    int index = int(tabs_.size()) - 1;
    { QSignalBlocker b(tabBar_); tabBar_->addTab(tab.defaultName); }
    connect(tab.canvas, &CanvasWidget::cursorMoved, this, [this](QPointF p) { positionLabel_->setText(QStringLiteral("%1, %2").arg(int(std::floor(p.x()))).arg(int(std::floor(p.y())))); });
    connect(tab.session, &EditorSession::projectPathChanged, this, &MainWindow::refreshTabTitles);
    connect(tab.session, &EditorSession::titleChanged, this, &MainWindow::refreshTabTitles);
    if (autosave_) watchForRecovery(tab.session);
    tabBar_->setCurrentIndex(index);
    if (current_ != index) switchTo(index);
    return tabs_[size_t(index)];
}

void MainWindow::watchForRecovery(EditorSession* session) {
    autosave_->watch(session, [this, session] {
        for (size_t i = 0; i < tabs_.size(); i++) if (tabs_[i].session == session) return tabTitle(int(i));
        return QString();
    });
}

void MainWindow::enableAutosave() {
    if (autosave_) return;
    autosave_ = new Autosave(this);
    for (const Tab& tab : tabs_) watchForRecovery(tab.session);
    QTimer::singleShot(0, this, &MainWindow::offerRecovery);
}

void MainWindow::offerRecovery() {
    const auto recovered = autosave_->claimOrphans();
    if (recovered.empty()) return;
    QStringList lines;
    for (const auto& r : recovered)
        lines << tr("%1, saved %2").arg(r.title.isEmpty() ? tr("Untitled") : r.title, QLocale().toString(r.saved.toLocalTime(), QLocale::ShortFormat));
    QMessageBox box(QMessageBox::Question, tr("Recover Unsaved Work"),
        tr("NekoPhoto did not close properly last time. Recover %n document(s) with unsaved changes?", nullptr, int(recovered.size())), QMessageBox::NoButton, this);
    box.setInformativeText(lines.join('\n'));
    QPushButton* recover = box.addButton(tr("Recover"), QMessageBox::AcceptRole);
    QPushButton* discard = box.addButton(tr("Discard"), QMessageBox::DestructiveRole);
    QPushButton* later = box.addButton(tr("Later"), QMessageBox::RejectRole);
    box.setDefaultButton(recover);
    // COMPOSITOR_RECOVERY_ANSWER (recover, discard or later) answers without asking, for tests.
    const QString answer = qEnvironmentVariable("COMPOSITOR_RECOVERY_ANSWER");
    if (answer == "recover") recover->click();
    else if (answer == "discard") discard->click();
    else if (!answer.isEmpty()) later->click();
    else box.exec();
    if (box.clickedButton() == discard) { autosave_->discardClaimed(); return; }
    if (box.clickedButton() != recover) { autosave_->releaseClaimed(); return; }
    QStringList failed;
    for (const auto& r : recovered) {
        QString error;
        auto project = EditorSession::readProject(r.project, &error);
        if (!project) { failed << (r.title + ": " + error); continue; }
        Tab& tab = addTab(true);
        const QString name = tr("%1 (recovered)").arg(r.title.isEmpty() ? tr("Untitled") : r.title);
        tab.session->adoptRecovered(project->document, name, r.originalPath);   // Save goes back to its own file
        if (project->activeLayer && tab.session->document()->find(*project->activeLayer)) tab.session->selectLayer(*project->activeLayer);
        tab.defaultName = name;
        refreshTabTitles();
    }
    if (!failed.isEmpty()) { autosave_->releaseClaimed(); showError(tr("Some documents could not be recovered"), failed.join('\n')); return; }
    autosave_->discardClaimed();
}

void MainWindow::switchTo(int index) {
    if (index < 0 || index >= int(tabs_.size())) return;
    if (current_ >= 0 && current_ < int(tabs_.size()) && current_ != index) {
        currentTab().canvas->commitType();
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
    pathsStack_->setCurrentWidget(tab.paths);
    channelsStack_->setCurrentWidget(tab.channels);
    adjustStack_->setCurrentWidget(tab.adjustments);
    if (timeline_) timeline_->setSession(session_);
    if (histogram_) histogram_->setSession(session_);
    tab.options->setVisible(true);
    // The import bar shows over its own document only.
    if (importBanner_) importBanner_->setVisible(bannerSession_ && bannerSession_ == session_ && !importBanner_->notes().isEmpty());
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

void MainWindow::showImportNotes(const QString& summary, const QString& title, const QString& heading, const QStringList& notes, EditorSession* session) {
    if (notes.isEmpty()) return;
    bannerSession_ = session ? session : session_;
    importBanner_->present(summary, title, heading, notes, session != nullptr);
    importBanner_->setVisible(bannerSession_ == session_);
}

void MainWindow::refreshHint() { hintLabel_->setText(toolHint(session_->tool(), session_->brushErase)); }

QString MainWindow::toolHint(Tool tool, bool erase) {
    switch (tool) {
    case Tool::Move: return tr("Drag to move; handles scale, just outside a corner rotates; Ctrl-drag a handle distorts; Ctrl-click picks a layer");
    case Tool::Marquee: return tr("Drag to select; Shift adds, Alt subtracts; drag inside a selection to move its outline");
    case Tool::Lasso: return tr("Freehand: drag around an area. Polygonal: click points, double-click or Enter closes, Backspace removes the last");
    case Tool::Wand: return tr("Click to select a region; then Shift-click more of it, Alt-click what should stay out, or change Tolerance to adjust it");
    case Tool::Scribble: return tr("Scribble over the subject (Alt: the background), or with the Click engine click it (Alt-click what is not it, drag a box); Backspace takes one back, Esc clears");
    case Tool::Crop: return tr("Drag the crop, then press Enter or double-click; Shift squares, Alt grows from the centre");
    case Tool::Brush: return erase ? tr("Drag to erase; [ and ] change the size; Shift-click erases a straight line")
                                   : tr("Drag to paint; [ and ] change the size, digits set the opacity; Shift-click paints a straight line");
    case Tool::SpotHealing: return tr("Paint over a blemish and it is filled from its surroundings");
    case Tool::CloneStamp: return tr("Alt-click sets the source, then paint");
    case Tool::Smudge: return tr("Opacity is the strength; Liquify pushes pixels, Smudge drags colour, Blur softens");
    case Tool::Gradient: return tr("Drag a line; drag again to redo it; Enter applies, Esc discards; Shift snaps the angle");
    case Tool::Pen: return tr("Click for corners, drag for curves; click the first point to close, Enter leaves the path open, Esc cancels; hold Ctrl for Direct Selection, Alt-click a point to convert it");
    case Tool::DirectSelect: return tr("Drag a point, a handle (Alt: just that one) or a path; Ctrl-drag moves the whole path; Alt-click a point to convert it; Delete removes the chosen point");
    case Tool::Dodge: return tr("Opacity is the Exposure (Dodge, Burn) or Flow (Sponge); a stroke never goes past one full pass");
    case Tool::PaintBucket: return tr("Click to fill pixels like the one clicked with the foreground colour, inside the selection");
    case Tool::Shape: return tr("Drag a shape in the foreground colour; Shift squares, Alt grows from the centre; Shift-U switches kind");
    case Tool::Text: return tr("Click to type, or drag a box for paragraph text; click text to edit it. Ctrl+Enter commits, Esc cancels; the options bar styles the selected letters");
    case Tool::Eyedropper: return tr("Click or drag to set the foreground colour, with Alt the background");
    case Tool::Hand: return tr("Drag to pan; hold Space to pan from any tool");
    case Tool::Zoom: return tr("Click zooms in, Alt-click out, drag a box to zoom to it; Ctrl-wheel zooms anywhere");
    case Tool::Artboard: return tr("Drag out an artboard; drag inside one to move it with its contents, an edge or corner to resize it");
    case Tool::Slice: return tr("Drag out a slice; drag inside one to move it, an edge or corner to resize it; File ▸ Export Slices writes them");
    }
    return {};
}

void MainWindow::refreshExposure() {
    if (!exposureBox_) return;
    const bool shown = session_ && session_->hasDocument() && session_->sampleType() == SampleType::F32;
    exposureBox_->setVisible(shown);
    if (!shown) return;
    const double exposure = session_->view32().exposure;
    const QSignalBlocker block(exposureSlider_);
    exposureSlider_->setValue(int(std::lround(exposure * 100)));
    exposureLabel_->setText(QStringLiteral("%1%2").arg(exposure > 0 ? QStringLiteral("+") : QString()).arg(exposure, 0, 'f', 2));
}

void MainWindow::refreshZoom() {
    zoomBox_->lineEdit()->setText(QStringLiteral("%1%").arg(session_->viewport.zoom * 100, 0, 'f', session_->viewport.zoom < 0.1 ? 1 : 0));
}

void MainWindow::connectSession() {
    for (auto& c : sessionConnections_) disconnect(c);
    sessionConnections_.clear();
    sessionConnections_.push_back(connect(session_, &EditorSession::viewportChanged, this, &MainWindow::refreshZoom));
    sessionConnections_.push_back(connect(session_, &EditorSession::view32Changed, this, &MainWindow::refreshExposure));
    sessionConnections_.push_back(connect(session_, &EditorSession::textEditRequested, this, [this](Uuid id) { canvas_->commitType(); (new TextDialog(session_, id, this))->show(); }));
    sessionConnections_.push_back(connect(session_, &EditorSession::titleChanged, this, &MainWindow::refreshTitle));
    sessionConnections_.push_back(connect(session_, &EditorSession::quickSelectBusyChanged, this, [this](bool busy) { if (busy) statusBar()->showMessage(tr("Finding the subject…")); else statusBar()->clearMessage(); }));
    sessionConnections_.push_back(connect(session_, &EditorSession::quickSelectFailed, this, [this](const QString& error) { statusBar()->showMessage(error, 6000); }));
    sessionConnections_.push_back(connect(session_, &EditorSession::notice, this, [this](const QString& text) { statusBar()->showMessage(text, 6000); }));
    sessionConnections_.push_back(connect(session_, &EditorSession::projectPathChanged, this, &MainWindow::refreshTitle));
    // The open project changed on disk while there is unsaved work here (a window nobody sees keeps it).
    sessionConnections_.push_back(connect(session_, &EditorSession::externalChangeConflict, this, [this](const QString& path) {
        EditorSession* s = session_;
        if (!isVisible()) { s->resolveExternalChange(false); return; }
        QMessageBox box(QMessageBox::Warning, tr("Changed on Disk"),
                        tr("“%1” was changed on disk by another app.").arg(QFileInfo(path).fileName()), QMessageBox::NoButton, this);
        box.setInformativeText(tr("You can revert to the version on disk, losing your unsaved changes, or keep what you have."));
        QPushButton* revert = box.addButton(tr("Revert"), QMessageBox::DestructiveRole);
        box.addButton(tr("Keep Mine"), QMessageBox::RejectRole);
        box.exec();
        s->resolveExternalChange(box.clickedButton() == revert);
    }));
    sessionConnections_.push_back(connect(session_, &EditorSession::reloadedFromDisk, this, [this] {
        statusBar()->showMessage(tr("Reloaded: the project changed on disk."), 4000);
        refreshTitle();
    }));
    sessionConnections_.push_back(connect(session_, &EditorSession::historyChanged, this, &MainWindow::refreshActions));
    sessionConnections_.push_back(connect(session_, &EditorSession::documentChanged, this, [this] { emit automationEvent("document"); }));
    sessionConnections_.push_back(connect(session_, &EditorSession::documentChangedAsShown, this, [this] { emit automationEvent("document"); }));
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
    sessionConnections_.push_back(connect(session_, &EditorSession::error, this, [this](QString message) { showError(tr("NekoPhoto"), message); }));
}

void MainWindow::refreshTabTitles() {
    for (size_t i = 0; i < tabs_.size(); i++) {
        Tab& tab = tabs_[i];
        QString name = !tab.session->projectPath().isEmpty() ? QFileInfo(tab.session->projectPath()).completeBaseName()
                     : !tab.session->importedName().isEmpty() ? tab.session->importedName() : tab.defaultName;
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
    pathsStack_->removeWidget(tab.paths);
    tab.paths->deleteLater();
    channelsStack_->removeWidget(tab.channels);
    tab.channels->deleteLater();
    adjustStack_->removeWidget(tab.adjustments);
    removeToolBar(tab.options);
    tab.frame->deleteLater(); tab.layers->deleteLater(); tab.adjustments->deleteLater(); tab.options->deleteLater();
    if (autosave_) autosave_->forget(tab.session);
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

void MainWindow::deleteLayersCommand() {
    if (!session_->hasDocument()) return;
    // One layer that supplies no clipping mask is layers.delete; several layers, or the question about clipped
    // layers, stay with the window (the method's step names no layers, so it records as before).
    const auto active = session_->activeLayerId();
    const auto& selected = session_->selectedLayerIds();
    const bool single = active && (selected.empty() || (selected.size() == 1 && selected.count(*active)));
    if (single && session_->clippingDependents({*active}).empty()) { runCommand("layers.delete", {}, tr("Delete Layer")); return; }
    deleteSelectedLayers();
    recordAction("layers.delete");
}

void MainWindow::fillWith(const QColor& color) {
    // pixels.fill takes #rrggbb; a colour finer than that (a 16-bit pick) fills directly, so the pixels do not change.
    if (QColor(color.name()) == color) { runCommand("pixels.fill", {{"color", color.name()}}, tr("Fill")); return; }
    session_->fillSelection(color);
    recordAction("pixels.fill", {{"color", color.name()}});
}

void MainWindow::maskCommand(const QJsonObject& params) {
    if (session_->activeLayerId()) runCommand("layers.mask", params, tr("Layer Mask"));
}

void MainWindow::samplingCommand(compositor::Sampling sampling) {
    if (session_->activeLayerId()) runCommand("layers.set", {{"sampling", QString::fromUtf8(compositor::samplingName(sampling))}}, tr("Resampling"));
}

void MainWindow::trimCommand(const QJsonObject& params) {
    const auto result = runCommand("image.trim", params, tr("Trim"));
    if (result && !result->toObject().value("trimmed").toBool())
        showError(tr("Trim"), tr("There is nothing to trim: the canvas already ends at its content, or nothing would remain."));
}

void MainWindow::showPreferences() {
    auto* dialog = new PreferencesDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &PreferencesDialog::backgroundRemovalChanged, this, &MainWindow::refreshBackgroundAction);
    connect(dialog, &QObject::destroyed, this, [this] { if (autosave_) autosave_->restart(); });
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
    refreshDepthGating();
    const bool eightBit = !session_->featuresGated();
    if (mergeAction_) { mergeAction_->setText(tr("&%1").arg(names::history(session_->mergeTitle()))); mergeAction_->setEnabled(has && session_->supportsFeature("layers.merge") && session_->canMergeLayers()); }
    if (mergeVisibleAction_) mergeVisibleAction_->setEnabled(has && session_->supportsFeature("layers.merge") && session_->canMergeVisible());
    if (editTextAction_) { const Layer* l = has ? session_->activeLayer() : nullptr; editTextAction_->setEnabled(eightBit && l && l->isLiveText()); }
    undoAction_->setEnabled(session_->canUndo());
    redoAction_->setEnabled(session_->canRedo());
    if (toggleStateAction_) toggleStateAction_->setEnabled(session_->canUndo() || session_->canRedo());
    if (revertAction_) revertAction_->setEnabled(has && !session_->projectPath().isEmpty());
    undoAction_->setText(session_->canUndo() ? tr("&Undo %1").arg(names::history(session_->undoName())) : tr("&Undo"));
    redoAction_->setText(session_->canRedo() ? tr("&Redo %1").arg(names::history(session_->redoName())) : tr("&Redo"));
    if (has) sizeLabel_->setText(QStringLiteral("%1 × %2 px").arg(session_->document()->width).arg(session_->document()->height));
    else sizeLabel_->clear();
}

void MainWindow::refreshTitle() {
    if (!session_) return;
    // The session's title already carries the modified marker; Qt's own needs a "[*]" placeholder we don't use.
    setWindowTitle(session_->title() + (session_->hasDocument() ? QStringLiteral(" — NekoPhoto") : QString()));
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
    if (autosave_) autosave_->finish();   // a clean quit leaves nothing to recover
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

void MainWindow::showPanel(const QString& name) {
    if (name == "actions") { actionsDock_->show(); actionsDock_->raise(); }
    else if (name == "channels") { channelsDock_->show(); channelsDock_->raise(); }
    else if (name == "histogram") { histogramDock_->show(); histogramDock_->raise(); }
    else if (name == "timeline") {
        timelineDock_->show();
        if (session_->hasDocument() && session_->document()->animation.empty()) session_->timelineFramesFromLayers();
    } else if (name == "batch") {
        if (ActionLibrary::instance().actions().empty()) {
            RecordedAction sample{tr("Web Thumbnail"), {{"image.resize", QJsonObject{{"width", 400}}, true}, {"document.export", QJsonObject{{"path", "/tmp/thumb.png"}, {"overwrite", true}}, false}}};
            ActionLibrary::instance().put(sample);
        }
        showBatchDialog();
    }
}

std::optional<QJsonValue> MainWindow::runCommand(const QString& method, const QJsonObject& params, const QString& title) {
    // The same request the socket, --call and --batch send: parameters checked against the method's description,
    // the depth gate, one undo step, and one Actions step when an action is recording.
    const QJsonObject reply = automationEngine()->handle(QJsonObject{{"jsonrpc", "2.0"}, {"id", 0}, {"method", method}, {"params", params}});
    if (reply.contains("error")) {
        showError(title.isEmpty() ? tr("Couldn’t do that") : title, reply.value("error").toObject().value("message").toString());
        return std::nullopt;
    }
    return reply.value("result");
}

AutomationServer* MainWindow::automationEngine() {
    if (automation_) return automation_;
    if (!engine_) engine_ = new AutomationServer(this);
    return engine_;
}

bool MainWindow::confirmActionWrites(const QString& name) {
    const RecordedAction* action = ActionLibrary::instance().find(name);
    if (!action || !action->confirmWrites) return true;
    const QStringList writes = ActionLibrary::writtenFiles(*action);
    if (writes.isEmpty()) { ActionLibrary::instance().confirm(name); return true; }
    QMessageBox box(QMessageBox::Warning, tr("Play Imported Action"),
                    tr("“%1” was imported from a file, and this action writes files:").arg(name), QMessageBox::NoButton, this);
    box.setInformativeText(writes.join('\n'));
    QPushButton* play = box.addButton(tr("Play"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    if (box.clickedButton() != play) return false;
    ActionLibrary::instance().confirm(name);
    return true;
}

QString MainWindow::playAction(const QString& name) {
    if (!confirmActionWrites(name)) return tr("“%1” was not played.").arg(name);
    try {
        const QJsonObject reply = automationEngine()->playAction(name);
        if (reply.value("completed").toBool()) return {};
        const QJsonObject error = reply.value("error").toObject();
        return tr("“%1” stopped at step %2 (%3): %4").arg(name).arg(error.value("index").toInt() + 1).arg(error.value("method").toString(), error.value("message").toString());
    } catch (const std::exception& e) {
        return QString::fromUtf8(e.what());
    }
}

void MainWindow::showWelcome() {
    auto* welcome = new WelcomeDialog(this);
    welcome->setAttribute(Qt::WA_DeleteOnClose);
    connect(welcome, &WelcomeDialog::openRequested, this, &MainWindow::openFiles);
    connect(welcome, &WelcomeDialog::newCanvasRequested, this, &MainWindow::newDocument);
    connect(welcome, &WelcomeDialog::importBrushesRequested, this, [this] { importBrushesInteractively(this, session_); });
    welcome->open();
}

} // namespace app
