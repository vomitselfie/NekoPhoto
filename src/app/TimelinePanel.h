// Window > Timeline: Photoshop's frame animation timeline for the current tab. A strip of frame thumbnails (click
// to select; the layers then show that frame), each frame's delay, New (duplicates the selected frame), Delete,
// move left and right, first / previous / play / next, the loop count and Make Frames From Layers. Playback
// previews frames without touching history (EditorSession::previewFrame).
#pragma once
#include <QPointer>
#include <QTimer>
#include <QWidget>

class QComboBox;
class QLabel;
class QButtonGroup;
class QHBoxLayout;
class QSpinBox;
class QStackedWidget;
class QToolButton;

namespace app {

class EditorSession;

class TimelinePanel : public QWidget {
    Q_OBJECT
public:
    explicit TimelinePanel(QWidget* parent = nullptr);
    void setSession(EditorSession* session);
    bool playing() const { return playTimer_.isActive(); }

private:
    void refresh();
    void refreshThumbnails();
    void play(bool on);
    void step();
    QPointer<EditorSession> session_;
    QStackedWidget* stack_;
    /// The frame cells, left to right: a checkable button each (thumbnail over number and delay).
    QHBoxLayout* strip_;
    QButtonGroup* cells_;
    void selectCell(int index);
    QSpinBox* delay_;
    QComboBox* loops_;
    QToolButton* playButton_;
    QLabel* info_;
    QTimer playTimer_, thumbTimer_;
    int playFrame_ = 0, loopsLeft_ = 0;
    bool refreshing_ = false;
    QList<QMetaObject::Connection> connections_;
};

} // namespace app
