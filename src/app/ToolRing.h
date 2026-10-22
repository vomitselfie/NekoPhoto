// The Tool Ring (View > Tool Ring, Alt+D): a compass of quick actions for the current tool, driven from the keyboard
// and never in the mouse's way. Nine circles sit as the keys do around D,
//
//      W  E  R
//      S (D) F
//      X  C  V
//
// the centre showing the tool, each satellite its key, an icon and, for a value, the value now. While the ring is open
// those nine letters (and Esc) belong to it; every other key and every modifier works as usual. It takes no mouse
// input, never takes the focus, sits near the pointer on the side away from where it is heading, inside the canvas,
// and moves only when the pointer would run into it. It closes on Esc, Alt+D again, another tool, another tab, the
// window losing the keyboard, or a few seconds after the last ring key (Preferences).
//
// The rings are data (ToolRingCommands.cpp): for each tool group, a command id of the registry per key and the value
// it shows. Values follow one compass rule: towards F and R (right, up) is more, towards S and V (left, down) less, so
// a value takes one of the opposite pairs S/F, X/E, V/R. A held key repeats; the wheel, while the ring is open, steps
// the value used last. Tool option values are not undo steps (as in Photoshop); commands that edit the document go
// through the command path, one step each.
#pragma once
#include "EditorSession.h"
#include <QHash>
#include <QPixmap>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QWidget>
#include <optional>
#include <vector>

class QAction;
class QMenu;
class QVariantAnimation;

namespace app {

class CommandRegistry;
class MainWindow;
class CanvasWidget;

namespace toolring {

/// The ring's settings (QSettings toolRing/...): whether Alt+D opens it at all, how long it stays open after the last
/// ring key (0: until closed), which side of the pointer it prefers, and the pen's side button that opens it.
enum class Corner { UpRight, UpLeft, DownRight, DownLeft };
bool enabled();
void setEnabled(bool on);
int idleMs();
void setIdleMs(int ms);
Corner corner();
void setCorner(Corner corner);
/// The tablet pen's side button that toggles the ring (0 none, the default; 1 the lower button, which pans otherwise;
/// 2 the upper one).
int penButton();
void setPenButton(int button);
/// The Qt button `penButton` names (Qt::NoButton for none).
Qt::MouseButton penMouseButton();

/// One satellite: its key, the registry command it runs, its icon (src/app/icons), and the value or state it shows
/// (empty: none; ToolRing::valueText reads it).
struct Slot {
    char key = 0;
    QString command;
    QString icon;
    QString value;
};

/// A ring: an id, the tools it serves (an Eraser is the Brush with erase on), its satellites, and what the centre (D)
/// runs (empty: it shows the tool's name).
struct Ring {
    QString id;
    std::vector<Slot> satellites;
    QString centre;
};

/// Every ring (the data, in one place) and the one for a tool.
const std::vector<Ring>& rings();
const Ring& ringFor(Tool tool);

/// The opposite key of a value pair (S and F, X and E, V and R), and whether a key is the pair's "more" end.
char oppositeKey(char key);
bool isMoreKey(char key);

} // namespace toolring

/// What the ring paints: the satellites as the controller fills them in. Transparent for the mouse, never focused.
class ToolRingOverlay : public QWidget {
    Q_OBJECT
public:
    struct View {
        char key = 0;
        QString icon;
        QString value;      // the live value ("24 px", "80%"), empty for an action
        bool enabled = true;
        bool checked = false;   // a state that is on (Add to Selection, Auto-Select)
        bool empty = false;     // no command on this key
        int direction = 0;      // -1 less, +1 more, 0 neither
    };
    explicit ToolRingOverlay(QWidget* parent);
    void setViews(const std::vector<View>& views, const QString& centreIcon);
    /// A brief highlight on the circle of `key`; muted for a greyed one.
    void pulse(char key, bool enabled);
    /// A line under the compass for a moment: the tool's name, a command's name, or why a greyed one cannot run.
    void showCaption(const QString& text);
    QString caption() const { return caption_; }
    /// The compass's own circle area within the widget (what the pointer must keep clear of).
    QRect compassRect() const;
    /// Where a key's circle is drawn, in widget coordinates (for tests).
    QPoint centreOf(char key) const;
    static QSize preferredSize();

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QPixmap icon(const QString& name, const QColor& color, int size);
    std::vector<View> views_;
    QString centreIcon_;
    QString caption_;
    QTimer captionTimer_;
    char pulseKey_ = 0;
    bool pulseEnabled_ = true;
    double pulse_ = 0;
    QVariantAnimation* pulseAnimation_ = nullptr;
    QHash<QString, QPixmap> icons_;
};

/// The ring's behaviour over the window: opens and closes it, owns the keys while it is open, follows the pointer.
class ToolRing : public QObject {
    Q_OBJECT
public:
    explicit ToolRing(MainWindow* window);

    bool isOpen() const { return open_; }
    /// Alt+D: opens the ring at the pointer (the canvas's centre when the pointer is elsewhere), or closes it.
    void toggle();
    void open();
    void close();
    /// Registers the ring's commands (view.toolRing and the tool option steps the rings use) in `registry`, adding
    /// View > Tool Ring to `viewMenu`.
    void registerCommands(CommandRegistry& registry, QMenu* viewMenu);
    /// The tab on screen changed (and at the start): the ring closes and follows that tab's session.
    void sessionChanged();
    ToolRingOverlay* overlay() const { return overlay_; }
    const toolring::Ring* ring() const { return ring_; }
    /// The key of the value the wheel steps (the last one used), or 0.
    char wheelKey() const { return wheelKey_; }
    /// The text a slot's `value` shows now ("24 px", "80%", a font), or empty; for a state, "on" or empty.
    QString valueText(const QString& value) const;
    bool valueChecked(const QString& value) const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// Runs the ring's key: its command when it can run, else its reason; repeats only values.
    void press(char key, bool autoRepeat);
    void refresh();
    /// Places the ring by the pointer at `pointer` (window coordinates); `heading` is where the pointer was moving.
    void place(QPoint pointer, QPointF heading);
    /// The pointer moved to `pointer` (window coordinates): the ring moves away only when it would be run into.
    void pointerMoved(QPoint pointer);
    QRect canvasArea() const;
    /// How far around the pointer the ring keeps clear: the brush outline's radius on screen, for a brush.
    int clearance() const;
    /// The tool and whether it erases: switching either closes the ring (unless the ring did it, as C does).
    std::pair<Tool, bool> toolKey() const;
    bool typing() const;

    MainWindow* window_;
    ToolRingOverlay* overlay_;
    CommandRegistry* registry_ = nullptr;
    const toolring::Ring* ring_ = nullptr;
    bool open_ = false;
    bool ringChanging_ = false;   // the ring's own command is running (C switching to the Eraser keeps it open)
    std::pair<Tool, bool> openedFor_{Tool::Move, false};
    char wheelKey_ = 0;
    double wheelRemainder_ = 0;
    QSet<int> heldKeys_;          // ring keys whose press the ring took (their releases are its too)
    QTimer idle_;
    QPoint lastPointer_;
    QPointF heading_;
    toolring::Corner placed_ = toolring::Corner::UpRight;
    QPointer<EditorSession> session_;
    std::vector<QMetaObject::Connection> sessionConnections_;
    /// The Brush preset before the current one, for W (Previous Brush Preset; empty is the round tip).
    QString presetNow_;
    std::optional<QString> presetBefore_;
    /// The commands registerCommands made, greyed and ungreyed as the tool changes.
    std::vector<QAction*> ownCommands_;
};

} // namespace app
