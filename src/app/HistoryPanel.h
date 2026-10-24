// Window > History: the document's states as Photoshop lists them, the state it opened in first, then each step,
// the ones undone below the current one, greyed. A click on a state goes back (or forward) to it, through
// history.undo and history.redo. It follows the tab on screen.
#pragma once
#include <QPointer>
#include <QWidget>
#include <functional>

class QListWidget;

namespace app {

class EditorSession;

class HistoryPanel : public QWidget {
    Q_OBJECT
public:
    /// `step` undoes (negative) or redoes (positive) that many steps through the command path.
    explicit HistoryPanel(std::function<void(int steps)> step, QWidget* parent = nullptr);
    void setSession(EditorSession* session);
    QListWidget* list() const { return list_; }
    /// The row of the state on screen.
    int currentRow() const { return undoCount_; }

private:
    void refresh();
    std::function<void(int)> step_;
    QPointer<EditorSession> session_;
    QMetaObject::Connection watch_;
    QListWidget* list_;
    int undoCount_ = 0;
};

} // namespace app
