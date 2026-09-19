// The application window: menus, the tool rail, the canvas, the Layers panel
// and the status bar.
#pragma once
#include "EditorSession.h"
#include <QMainWindow>
#include <QStringList>

class QLabel;
class QMenu;
class QToolButton;

namespace app {

class CanvasWidget;
class LayersPanel;
class ToolOptionsBar;

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

    EditorSession* session_;
    CanvasWidget* canvas_;
    LayersPanel* layers_;
    ToolOptionsBar* options_;
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
