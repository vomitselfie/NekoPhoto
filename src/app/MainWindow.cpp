#include "MainWindow.h"
#include "BrushImporter.h"
#include "CanvasFrame.h"
#include "Autosave.h"
#include "LayersPanel.h"
#include "AdjustmentsPanel.h"
#include "Automation.h"
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
    // The saved state remembers each tab's options bar by name, and only the current tab's is visible when the
    // window closes; restoring it could hide the bar of the tab this launch shows. The current tab owns the bar.
    for (size_t i = 0; i < tabs_.size(); i++) tabs_[i].options->setVisible(int(i) == current_);
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
        tr("Compositor did not close properly last time. Recover %n document(s) with unsaved changes?", nullptr, int(recovered.size())), QMessageBox::NoButton, this);
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
        tab.session->adoptDocument(project->document, name);
        tab.defaultName = name;
        tab.session->markUnsaved();   // it exists nowhere else now
        refreshTabTitles();
    }
    if (!failed.isEmpty()) { autosave_->releaseClaimed(); showError(tr("Some documents could not be recovered"), failed.join('\n')); return; }
    autosave_->discardClaimed();
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
    case Tool::Scribble: return tr("Scribble over the subject (Alt: the background), or with the Click engine click it (Alt-click what is not it, drag a box); Backspace takes one back, Esc clears");
    case Tool::Crop: return tr("Drag the crop, then press Enter or double-click; Shift squares, Alt grows from the centre");
    case Tool::Brush: return erase ? tr("Drag to erase; [ and ] change the size; Shift-click erases a straight line")
                                   : tr("Drag to paint; [ and ] change the size, digits set the opacity; Shift-click paints a straight line");
    case Tool::SpotHealing: return tr("Paint over a blemish and it is filled from its surroundings");
    case Tool::CloneStamp: return tr("Alt-click sets the source, then paint");
    case Tool::Smudge: return tr("Opacity is the strength; Liquify pushes pixels, Smudge drags colour, Blur softens");
    case Tool::Gradient: return tr("Drag a line; drag again to redo it; Enter applies, Esc discards; Shift snaps the angle");
    case Tool::Shape: return tr("Drag a shape in the foreground colour; Shift squares, Alt grows from the centre; Shift-U switches kind");
    case Tool::Text: return tr("Click to add text in the foreground colour, or click a text layer to edit it; the options bar sets the font");
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
    sessionConnections_.push_back(connect(session_, &EditorSession::textEditRequested, this, [this](Uuid id) { (new TextDialog(session_, id, this))->show(); }));
    sessionConnections_.push_back(connect(session_, &EditorSession::titleChanged, this, &MainWindow::refreshTitle));
    sessionConnections_.push_back(connect(session_, &EditorSession::quickSelectBusyChanged, this, [this](bool busy) { if (busy) statusBar()->showMessage(tr("Finding the subject…")); else statusBar()->clearMessage(); }));
    sessionConnections_.push_back(connect(session_, &EditorSession::quickSelectFailed, this, [this](const QString& error) { statusBar()->showMessage(error, 6000); }));
    sessionConnections_.push_back(connect(session_, &EditorSession::projectPathChanged, this, &MainWindow::refreshTitle));
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
    for (auto* a : documentActions_) a->setEnabled(has);
    if (mergeAction_) { mergeAction_->setText(tr("&%1").arg(session_->mergeTitle())); mergeAction_->setEnabled(has && session_->canMergeLayers()); }
    if (editTextAction_) { const Layer* l = has ? session_->activeLayer() : nullptr; editTextAction_->setEnabled(l && l->isLiveText()); }
    undoAction_->setEnabled(session_->canUndo());
    redoAction_->setEnabled(session_->canRedo());
    undoAction_->setText(session_->canUndo() ? tr("&Undo %1").arg(session_->undoName()) : tr("&Undo"));
    redoAction_->setText(session_->canRedo() ? tr("&Redo %1").arg(session_->redoName()) : tr("&Redo"));
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

void MainWindow::showWelcome() {
    auto* welcome = new WelcomeDialog(this);
    welcome->setAttribute(Qt::WA_DeleteOnClose);
    connect(welcome, &WelcomeDialog::openRequested, this, &MainWindow::openFiles);
    connect(welcome, &WelcomeDialog::newCanvasRequested, this, &MainWindow::newDocument);
    connect(welcome, &WelcomeDialog::importBrushesRequested, this, [this] { importBrushesInteractively(this, session_); });
    welcome->open();
}

} // namespace app
