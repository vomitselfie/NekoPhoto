// The application window: project tabs, menus, the tool rail, the canvas, the
// Layers and Adjustments panels and the status bar. Each tab is one project
// with its own session, canvas and panels; the menus act on the current one.
#pragma once
#include "EditorSession.h"
#include <QMainWindow>
#include <QStringList>
#include <QTabBar>
#include <vector>

class QComboBox;
class QDockWidget;
class QLabel;
class QMenu;
class QToolButton;
class QStackedWidget;

namespace app {

/// A file that opens in its own tab with its layers: .psd, .psb, .clip, .ico, .cur, .ase, .aseprite, or a GIF of
/// more than one frame.
bool isLayeredPath(const QString& path);

class Autosave;

class CanvasFrame;
class CanvasWidget;
class ColorSwatches;
class LayersPanel;
class AdjustmentsPanel;
class AutomationServer;
class ToolOptionsBar;

/// The tab strip: accepts a layer dragged from another project's Layers panel.
class ProjectTabBar : public QTabBar {
    Q_OBJECT
public:
    explicit ProjectTabBar(QWidget* parent = nullptr);
signals:
    void layerDropped(int tabIndex, QString payload);
protected:
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dropEvent(QDropEvent*) override;
};

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    void openPath(const QString& path);
    /// The first-run introduction (WelcomeDialog), opened over the window.
    void showWelcome();
    /// Crash recovery for an interactive launch: autosaves each tab's unsaved changes and offers back what
    /// an instance that crashed left behind. Off for headless, batch and screenshot runs.
    void enableAutosave();
    /// A file handed over from another launch: a project or PSD as `openPath` does, an image as a document of
    /// its own rather than a layer of the current one (a double-click in the file manager means "open this").
    void openAsDocument(const QString& path);
    /// A layered file (Photoshop, Clip Studio, Aseprite, an icon's sizes or an animated GIF's frames) in a new
    /// tab, with a note of what did not carry over.
    void openLayeredFile(const QString& path);
    const QStringList& lastImportNotes() const { return lastImportNotes_; }
    EditorSession* session() const { return session_; }

    // For the automation socket (Automation.cpp).
    int tabCount() const { return int(tabs_.size()); }
    int currentTabIndex() const { return current_; }
    /// Opens the active smart object's contents in a tab of their own; saving that tab puts them back. With
    /// `error`, reports instead of showing a dialog (automation).
    bool editSmartObjectContents(QString* error = nullptr);
    EditorSession* sessionAt(int i) const { return tabs_[size_t(i)].session; }
    CanvasWidget* canvasAt(int i) const { return tabs_[size_t(i)].canvas; }
    LayersPanel* layersPanelAt(int i) const { return tabs_[size_t(i)].layers; }
    QString tabTitle(int i) const;
    void selectTab(int i) { switchTo(i); }
    int newTab() { addTab(false); return current_; }
    void closeTabAt(int i) { skipConfirm_ = true; closeTab(i); skipConfirm_ = false; }
    bool importImageFile(const QString& path, std::optional<QPointF> at, QString* error);
    /// An image file as a document of its own, in a new tab (what a drop on the tab strip does).
    bool openImageAsDocument(const QString& path, QString* error);
    /// Whether a window position lies on the tab strip, where a dropped file opens as a new document.
    bool overTabStrip(const QPointF& windowPosition) const;
    void noteRecent(const QString& path) { addRecent(path); }
    /// While set, errors the window would show in a dialog are appended here instead.
    void setErrorSink(QString* sink) { errorSink_ = sink; }
    /// Starts listening on `socketPath` (empty: the default); returns false with a warning on failure.
    bool startAutomation(const QString& socketPath);

signals:
    /// Something an agent may want to know about changed: document, layers, selection, history, tool, view, tabs.
    void automationEvent(QString kind);

protected:
    void closeEvent(QCloseEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dragLeaveEvent(QDragLeaveEvent*) override;
    void dropEvent(QDropEvent*) override;

private:
    struct Tab {
        CanvasFrame* frame = nullptr;
        EditorSession* session = nullptr;
        CanvasWidget* canvas = nullptr;
        LayersPanel* layers = nullptr;
        AdjustmentsPanel* adjustments = nullptr;
        ToolOptionsBar* options = nullptr;
        QString defaultName;
    };
    Tab& addTab(bool reuseEmpty);
    void switchTo(int index);
    void closeTab(int index);
    Tab& currentTab() { return tabs_[size_t(current_)]; }
    bool confirmDiscard(int index);
    void connectSession();
    void refreshTabTitles();
    void buildMenus();
    void buildToolRail();
    void newDocument();
    void openProject();
    void openFiles();
    void importFiles();
    bool save(bool asNew);
    void exportPng();
    void exportJpeg();
    void exportWebp();
    void exportTiff();
    void exportTga();
    /// A multi-size .ico (16, 32, 48 and 256 px).
    void exportIco();
    /// A layered PSD, after a summary of anything Photoshop cannot carry.
    void exportPsd();

    QString askExportPath(const QString& title, const QString& filter, const QStringList& suffixes);
    bool confirmDiscard();
    void importFile(const QString& path, std::optional<QPointF> at = std::nullopt);
    void addRecent(const QString& path);
    void refreshRecent();
    void refreshTitle();
    void refreshZoom();
    void refreshHint();
    static QString toolHint(Tool tool, bool erase);
    void refreshActions();
    void chooseColor(bool background);
    void deleteSelectedLayers();
    void updateColorSwatches();
    void showError(const QString& title, const QString& message);
    void copyLayerFromPayload(int tabIndex, const QString& payload);
    void showPreferences();
    void refreshBackgroundAction();
    QAction* removeBackgroundAction_ = nullptr;
    QString* errorSink_ = nullptr;
    bool skipConfirm_ = false;
    AutomationServer* automation_ = nullptr;
    Autosave* autosave_ = nullptr;
    void offerRecovery();
    void watchForRecovery(EditorSession* session);
    QLabel* automationLabel_ = nullptr;
    QAction* mergeAction_ = nullptr;
    QAction* editTextAction_ = nullptr;
    QStringList lastImportNotes_;   // what the last PSD import could not carry, for automation callers

    std::vector<Tab> tabs_;
    int current_ = -1;
    int nextNumber_ = 2;
    EditorSession* session_ = nullptr;
    CanvasWidget* canvas_ = nullptr;
    LayersPanel* layers_ = nullptr;
    ToolOptionsBar* options_ = nullptr;
    ProjectTabBar* tabBar_;
    QStackedWidget* canvasStack_;
    QStackedWidget* layersStack_;
    QStackedWidget* adjustStack_;
    std::vector<QMetaObject::Connection> sessionConnections_;
    QMenu* recentMenu_;
    QLabel* positionLabel_;
    QLabel* hintLabel_;
    QAction* rulersAction_ = nullptr;
    QLabel* sizeLabel_;
    ColorSwatches* swatches_;
    QDockWidget* layersDock_ = nullptr;
    QDockWidget* adjustDock_ = nullptr;
    QComboBox* zoomBox_;
    QAction* undoAction_;
    QAction* redoAction_;
    QList<QAction*> documentActions_;
    QMap<Tool, QAction*> toolActions_;
    QAction* eraserAction_;
};

} // namespace app
