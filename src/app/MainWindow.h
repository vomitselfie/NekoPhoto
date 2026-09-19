// The application window: project tabs, menus, the tool rail, the canvas, the
// Layers and Adjustments panels and the status bar. Each tab is one project
// with its own session, canvas and panels; the menus act on the current one.
#pragma once
#include "EditorSession.h"
#include <QMainWindow>
#include <QStringList>
#include <QTabBar>
#include <vector>

class QLabel;
class QMenu;
class QToolButton;
class QStackedWidget;

namespace app {

class CanvasWidget;
class LayersPanel;
class AdjustmentsPanel;
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
    EditorSession* session() const { return session_; }

protected:
    void closeEvent(QCloseEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dropEvent(QDropEvent*) override;

private:
    struct Tab {
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
    void importImages();
    bool save(bool asNew);
    void exportPng();
    void exportJpeg();
    bool confirmDiscard();
    void importFile(const QString& path, std::optional<QPointF> at = std::nullopt);
    void addRecent(const QString& path);
    void refreshRecent();
    void refreshTitle();
    void refreshActions();
    void chooseColor(bool background);
    void deleteSelectedLayers();
    void updateColorSwatches();
    void showError(const QString& title, const QString& message);
    void copyLayerFromPayload(int tabIndex, const QString& payload);
    void showPreferences();
    void refreshBackgroundAction();
    QAction* removeBackgroundAction_ = nullptr;
    QAction* mergeAction_ = nullptr;

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
    QLabel* zoomLabel_;
    QLabel* positionLabel_;
    QLabel* sizeLabel_;
    QToolButton* foregroundButton_;
    QToolButton* backgroundButton_;
    QAction* undoAction_;
    QAction* redoAction_;
    QList<QAction*> documentActions_;
    QMap<Tool, QAction*> toolActions_;
    QAction* eraserAction_;
};

} // namespace app
