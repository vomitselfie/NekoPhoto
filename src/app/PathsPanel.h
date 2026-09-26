// The Paths panel (Photoshop's): the document's Work Path and saved paths. Choosing one makes it the Pen's and
// Direct Selection's target and shows it on the canvas; the buttons fill it, stroke it with the brush, load it as a
// selection, make a work path from the selection, or make a shape layer from it.
#pragma once
#include "EditorSession.h"
#include <QPointer>
#include <QWidget>

class QListWidget;

namespace app {

class PathsPanel : public QWidget {
    Q_OBJECT
public:
    explicit PathsPanel(EditorSession* session, QWidget* parent = nullptr);

private:
    void refresh();
    std::optional<uint16_t> chosen() const;
    QPointer<EditorSession> session_;
    QListWidget* list_;
    bool refreshing_ = false;
};

} // namespace app
