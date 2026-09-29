// The Channels panel (Photoshop's): the composite and the red, green and blue channels as views, then the alpha and
// spot channels, then Quick Mask while it is on. Clicking a row makes it the target (Shift-click adds a colour
// channel), the eye shows or hides it, Ctrl-clicking a thumbnail loads it as a selection (Shift adds, Alt subtracts,
// both intersect), and the buttons load a channel, save the selection as a channel, make a new one and delete one.
// See docs/channels.md.
#pragma once
#include "EditorSession.h"
#include <QPointer>
#include <QPixmap>
#include <QWidget>
#include <map>

class QListWidget;
class QListWidgetItem;
class QTimer;

namespace app {

class ChannelsPanel : public QWidget {
    Q_OBJECT
public:
    explicit ChannelsPanel(EditorSession* session, QWidget* parent = nullptr);

    /// Rows: the composite, red, green, blue, an alpha or spot channel, Quick Mask.
    enum Row { Composite, Red, Green, Blue, Alpha, QuickMask };

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void rebuild();
    void refreshThumbnails();
    void clicked(QListWidgetItem* item);
    void loadAsSelection(int row, const QString& id, Qt::KeyboardModifiers modifiers);
    void showOptions(const QString& id);
    void showMenu(const QPoint& at);
    void rowsMoved();
    QWidget* makeRow(int row, const QString& id, const QString& name, const QString& shortcut, bool visible, bool italic);
    QPointer<EditorSession> session_;
    QListWidget* list_;
    QTimer* thumbnails_;
    std::map<QString, QPixmap> thumbCache_;   // by row and channel id, shown until the next refresh
    bool rebuilding_ = false;
};

} // namespace app
