// Window > Actions: the action library as a tree (actions, their steps with a checkbox each), with Play, Record and
// Stop, and buttons to add, delete, reorder, edit (a step's parameters as JSON), import and export. Plays through the
// window's automation engine, so a step is exactly an automation request.
#pragma once
#include <QWidget>
#include <functional>

class QTreeWidget;
class QTreeWidgetItem;
class QToolButton;
class QLabel;

namespace app {

class ActionsPanel : public QWidget {
    Q_OBJECT
public:
    /// `play` runs an action by name and returns an error message (empty when it completed).
    ActionsPanel(std::function<QString(const QString&)> play, QWidget* parent = nullptr);

signals:
    void batchRequested(const QString& action);

private:
    void rebuild();
    /// The action the selection is in, and the step row (-1 for the action itself).
    QString selectedAction(int* step = nullptr) const;
    void playSelected();
    void toggleRecording();
    void newAction();
    void deleteSelected();
    void moveStep(int by);
    void editStep();
    void importActions();
    void exportActions();
    std::function<QString(const QString&)> play_;
    QTreeWidget* tree_;
    QToolButton* record_;
    QToolButton* playButton_;
    QLabel* status_;
    bool rebuilding_ = false;
};

} // namespace app
