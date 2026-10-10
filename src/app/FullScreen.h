// View ▸ Screen Mode ▸ Full Screen (F): nothing on screen but the canvas. Each screen edge is an invisible hot zone;
// the pointer reaching one slides that edge's interface out over the canvas, as an overlay inside the window (child
// widgets, never windows of their own): the left edge the tool rail, the top the options bar, the right one column
// with every panel docked on the right (in their tab groups), the bottom the Timeline when it is open. The menu bar
// shows only while a menu is open (F10 or Alt). Leaving restores the window and its dock layout exactly.
#pragma once
#include <QByteArray>
#include <QColor>
#include <QPointer>
#include <QWidget>
#include <algorithm>
#include <array>
#include <vector>

class QBoxLayout;
class QDockWidget;
class QMenuBar;
class QAction;
class QSplitter;
class QTabBar;
class QTabWidget;
class QTimer;
class QToolBar;
class QVariantAnimation;

namespace app {

class MainWindow;

/// One edge's slide-out panel: a card flush with the screen edge, its inner corners rounded and a soft shadow on its
/// inner sides when the theme asks (Goth Kitty's stylesheet sets cornerRadius and shadow; other themes leave them 0).
class Flyout : public QWidget {
    Q_OBJECT
    Q_PROPERTY(int cornerRadius READ cornerRadius WRITE setCornerRadius)
    Q_PROPERTY(int shadow READ shadow WRITE setShadow)
    Q_PROPERTY(QColor fill READ fill WRITE setFill)
    Q_PROPERTY(QColor border READ border WRITE setBorder)
public:
    Flyout(Qt::Edge edge, QWidget* parent);
    Qt::Edge edge() const { return edge_; }
    QBoxLayout* body() const { return body_; }
    int cornerRadius() const { return radius_; }
    void setCornerRadius(int r) { radius_ = std::max(0, r); update(); }
    int shadow() const { return shadow_; }
    void setShadow(int s);
    QColor fill() const { return fill_; }
    void setFill(const QColor& c) { fill_ = c; update(); }
    QColor border() const { return border_; }
    void setBorder(const QColor& c) { border_ = c; update(); }
    /// The card without its shadow, in the flyout's own coordinates.
    QRect cardRect() const;
protected:
    void paintEvent(QPaintEvent*) override;
private:
    void updateMargins();
    Qt::Edge edge_;
    QBoxLayout* body_;
    int radius_ = 0, shadow_ = 0;
    QColor fill_, border_;
};

class FullScreenMode : public QObject {
    Q_OBJECT
public:
    enum Edge { Left, Top, Right, Bottom };
    static constexpr int edgeCount = 4;

    explicit FullScreenMode(MainWindow* window);
    ~FullScreenMode() override;

    bool active() const { return active_; }
    void setActive(bool on) { if (on) enter(); else leave(); }
    void enter();
    void leave();

    /// The window's state and geometry from before full screen (what closing the window saves meanwhile).
    QByteArray normalState() const { return savedState_; }
    QByteArray normalGeometry() const { return savedGeometry_; }

    /// Whether an edge's flyout is out (sliding out, or out), and whether it is pinned out (Tab, Shift+Tab).
    bool isOut(Edge edge) const { return edges_[size_t(edge)].target; }
    bool isPinned(Edge edge) const { return edges_[size_t(edge)].pinned; }
    /// Whether an edge has anything to show (the bottom only while a panel docked there is open).
    bool hasContent(Edge edge) const;
    Flyout* flyout(Edge edge) const { return edges_[size_t(edge)].flyout; }
    /// The tab group holding `dock`'s panel while in full screen, and which edge holds it (-1: none).
    QTabWidget* tabsHolding(QDockWidget* dock) const;
    int edgeHolding(QDockWidget* dock) const;
    /// The menu bar shown over the canvas while a menu is open.
    QMenuBar* menuStrip() const { return menuStrip_; }

    /// Tab: every edge out and pinned, or (when they all are) back to hidden. Shift+Tab: the same without the tools.
    void togglePinned(bool withTools);
    /// Every edge pinned out (`withTools`: the tools' too), or none; view.screenMode's `panels`.
    void pin(bool on, bool withTools);
    /// Whether exactly those edges are pinned (what Tab, or Shift+Tab, would hide again).
    bool pinnedAll(bool withTools) const;
    /// F7 and the Window menu's panels: slides out the edge holding `dock` with its tab in front; again while it is out
    /// in front, slides it back.
    void revealPanel(QDockWidget* dock);
    /// F10 or Alt: the menu bar over the canvas with its first menu open; it hides again when the menus close.
    void showMenus();
    /// Esc: slides back every edge that is out (pinned too); false when none was.
    bool closeFlyouts();
    /// Slides `edge` out as a key would (it stays until the pointer has been in it), for screenshots.
    void showEdge(Edge edge);

    /// The tab on screen changed: its options bar goes in the top flyout.
    void currentTabChanged();

    // For tests: how long a slide takes and how long a flyout waits once the pointer has left it (ms), whether the
    // pointer's position is polled (besides the pointer's events), and how wide the hot zones are (logical px).
    void setTimings(int slideMs, int closeDelayMs) { slideMs_ = std::max(0, slideMs); closeDelayMs_ = std::max(0, closeDelayMs); }
    void setPolling(bool on) { polling_ = on; }
    int hotZone() const { return zone_; }
    /// The flyout's geometry it slides to, in window coordinates.
    QRect shownGeometry(Edge edge) const;

signals:
    void activeChanged(bool active);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Group {
        QTabWidget* tabs = nullptr;
        std::vector<QDockWidget*> docks;
    };
    struct EdgeState {
        Flyout* flyout = nullptr;
        QVariantAnimation* slide = nullptr;
        QTimer* closeTimer = nullptr;
        double progress = 0;   // 0 hidden, 1 out
        bool target = false;   // sliding out or out
        bool pinned = false;
        bool inZone = false;   // the pointer was in its hot zone at the last look
        bool held = false;     // opened from the keyboard: stays until the pointer has been in it
        int extent = 0;        // the right column's width, the bottom row's height, from the normal window
        QSplitter* splitter = nullptr;
        std::vector<Group> groups;
    };
    struct Moved {
        QPointer<QDockWidget> dock;
        QPointer<QWidget> content;
    };

    void build();
    void slide(Edge edge, bool out);
    void relayout();
    void pointerAt(QPoint windowPosition);
    void evaluate();
    bool keepsOut(Edge edge) const;
    int edgeOf(const QWidget* widget) const;
    bool inZone(Edge edge, QPoint p) const;
    Group& addGroup(Edge edge, const std::vector<QDockWidget*>& docks, QDockWidget* current);
    void placeOptionsBar();
    void hideMenuStripLater();
    void moveFocusOut(Edge edge);
    void addWindowShortcuts();

    MainWindow* window_;
    bool active_ = false;
    std::array<EdgeState, edgeCount> edges_;
    std::vector<Moved> moved_;
    QPointer<QToolBar> rail_;
    std::vector<QPointer<QToolBar>> optionBars_;   // every tab's options bar, out of the window's layout
    QPointer<QToolBar> shownOptions_;
    QList<QAction*> addedShortcuts_;              // menu commands given to the window so their keys work without the menu bar
    QMenuBar* menuStrip_ = nullptr;
    QTimer* tick_ = nullptr;
    QByteArray savedState_, savedGeometry_;
    Qt::WindowStates savedWindowState_;
    bool menuBarShown_ = true, statusBarShown_ = true, tabBarShown_ = true;
    bool reapplyState_ = false;
    QSize normalSize_;
    QPoint lastPointer_;
    bool pointerKnown_ = false;
    QPoint lastPolled_{-100000, -100000};
    int pressEdge_ = -1;          // the flyout a held button went down in (-1: the canvas, or none)
    Qt::MouseButtons buttons_;    // the buttons held, from the pointer's events
    bool tabletDown_ = false;
    bool altArmed_ = false;
    int slideMs_ = 150, closeDelayMs_ = 500, zone_ = 4;
    bool polling_ = true;
};

} // namespace app
