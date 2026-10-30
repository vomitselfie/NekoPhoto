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
#include "EditorTool.h"
#include "RingController.h"
#include "RingOverlay.h"
#include <QColor>
#include <QHash>
#include <QPixmap>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QWidget>
#include <array>
#include <optional>
#include <vector>

class QAction;
class QMenu;

namespace app {

class EditorSession;

namespace motion { class Tween; }
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

/// What the ring paints: the satellites as the controller fills them in. Transparent for the mouse, never focused. The
/// Colour Ring paints with it too. Its motion (RingOverlay.h, Motion.h): the satellites fan out from the centre as it
/// pops open, a key's circle dips and bounces back with a ripple in the accent, a greyed key shakes as its reason
/// fades in, a value rolls to the new number, and the Colour Ring's centre turns to the new colour, its swatches
/// trading places on an arc for a swap and pulsing for the default colours.
class ToolRingOverlay : public RingOverlay {
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
        std::optional<QColor> chip;   // a colour in place of the icon (the Colour Ring's)
    };
    explicit ToolRingOverlay(QWidget* parent);
    void setViews(const std::vector<View>& views, const QString& centreIcon);
    /// The centre as the toolbar's swatches instead of an icon: the foreground over the background (the Colour Ring).
    /// A new colour turns the swatch to it; after swapSwatches(), the two trade places instead.
    void setCentreSwatches(std::optional<std::pair<QColor, QColor>> colors);
    void swapSwatches() { swapNext_ = true; }
    /// The key just pressed: its circle bounces with a ripple; muted, and shaking, for a greyed one.
    void pulse(char key, bool enabled);
    /// The compass's own circle area within the widget (what the pointer must keep clear of).
    QRect compassRect() const;
    /// Where a key's circle is drawn, in widget coordinates (for tests).
    QPoint centreOf(char key) const;
    static QSize preferredSize();
    void finishMotion() override;

    // For tests: whether a key's press, shake or value roll is running; the centre swatches as painted now.
    bool pressing(char key) const;
    bool shaking(char key) const;
    bool ticking(char key) const;
    bool swatchMoving() const;
    std::optional<std::pair<QColor, QColor>> shownSwatches() const;

protected:
    void paintEvent(QPaintEvent*) override;
    void hidden() override;

private:
    /// A key's motion: its press (bounce and ripple), its shake, its value rolling from `before`.
    struct KeyMotion {
        motion::Tween* press = nullptr;
        motion::Tween* shake = nullptr;
        motion::Tween* tick = nullptr;
        bool pressEnabled = true;
        QString before;
        int roll = 1;   // +1: the number rolls up (it grew), -1 down
    };
    KeyMotion& motionOf(char key);
    QPixmap icon(const QString& name, const QColor& color, int size);
    std::vector<View> views_;
    QString centreIcon_;
    std::optional<std::pair<QColor, QColor>> centreSwatches_, swatchesFrom_;
    motion::Tween* swatch_ = nullptr;
    motion::Tween* swap_ = nullptr;
    bool swapNext_ = false;
    std::array<KeyMotion, 9> keys_{};
    QHash<QString, QPixmap> icons_;
};

/// The ring's behaviour over the window: opens and closes it, owns the keys while it is open, follows the pointer
/// (RingController.h has what it shares with the Colour Ring and the Layer Picker).
class ToolRing : public RingController {
    Q_OBJECT
public:
    explicit ToolRing(MainWindow* window);

    /// Registers the ring's commands (view.toolRing and the tool option steps the rings use) in `registry`, adding
    /// View > Tool Ring to `viewMenu`.
    void registerCommands(CommandRegistry& registry, QMenu* viewMenu);
    ToolRingOverlay* overlay() const { return ringOverlay_; }
    const toolring::Ring* ring() const { return ring_; }
    /// The key of the value the wheel steps (the last one used), or 0.
    char wheelKey() const { return wheelKey_; }
    /// The text a slot's `value` shows now ("24 px", "80%", a font), or empty; for a state, "on" or empty.
    QString valueText(const QString& value) const;
    bool valueChecked(const QString& value) const;

protected:
    bool mayOpen() const override;
    void opening() override;
    void refresh() override;
    /// Runs the ring's key: its command when it can run, else its reason; repeats only values.
    void press(char key, bool autoRepeat) override;
    bool wantsWheel() const override { return wheelKey_ != 0; }
    void wheelStep(bool more) override;
    void toolSwitched() override;
    void connectSession(EditorSession& session) override;
    QRect keepClear() const override { return ringOverlay_->compassRect(); }

private:
    ToolRingOverlay* ringOverlay_;
    CommandRegistry* registry_ = nullptr;
    const toolring::Ring* ring_ = nullptr;
    bool ringChanging_ = false;   // the ring's own command is running (C switching to the Eraser keeps it open)
    char wheelKey_ = 0;
    /// The Brush preset before the current one, for W (Previous Brush Preset; empty is the round tip).
    QString presetNow_;
    std::optional<QString> presetBefore_;
    /// The commands registerCommands made, greyed and ungreyed as the tool changes.
    std::vector<QAction*> ownCommands_;
};

} // namespace app
