// The window's edges. View ▸ Screen Mode ▸ Full Screen (F) puts nothing on screen but the canvas, and Edit ▸
// Preferences ▸ Edges can autohide any edge of the standard window too (the tools on the left, the options bar at the
// top, the panels on the right, the Timeline and the status bar at the bottom): the canvas then takes that side of the
// window. Each such edge is an invisible hot zone; the pointer reaching one slides that edge's flyout out over the
// canvas, as an overlay inside the window (child widgets, never windows of their own), and it slides back once the
// pointer has left it, never while a stroke begun on the canvas runs into it. Each edge holds a few pages
// (EdgePages.h): Tab and Shift+Tab flip the edge that is out, as does the wheel over its page strip; with no edge
// out, a tap of Tab is View ▸ Screen Mode's Show or Hide Panels as before, and holding Tab brings every hidden edge
// out until it is let go. In full screen the menu bar shows only while a menu is open (F10 or Alt), and leaving it
// restores the window and its dock layout exactly. In the standard window the menus and the document tabs stay.
#pragma once
#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QKeySequence>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QWidget>
#include <algorithm>
#include <array>
#include <optional>
#include <vector>

class QBoxLayout;
class QDockWidget;
class QKeyEvent;
class QMenuBar;
class QAction;
class QSplitter;
class QTimer;
class QToolBar;
class QVariantAnimation;

namespace app {

class MainWindow;
class PageDeck;
class PageStrip;
class BrushList;
class ColourPage;
class DocumentsPage;
class InfoPanel;
class EditorSession;

/// One edge's slide-out panel: a card flush with the edge, its inner corners rounded and a soft shadow on its inner
/// sides when the theme asks (Goth Kitty's stylesheet sets cornerRadius and shadow; other themes leave them 0).
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
    void setCornerRadius(int r) { radius_ = std::max(0, r); updateMargins(); update(); }
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

    // ---- Full screen
    bool active() const { return active_; }
    void setActive(bool on) { if (on) enter(); else leave(); }
    void enter();
    void leave();
    /// The window's state and geometry from before full screen (what closing the window saves meanwhile).
    QByteArray normalState() const { return savedState_; }
    QByteArray normalGeometry() const { return savedGeometry_; }

    // ---- Autohide in the standard window
    /// Reads Edit ▸ Preferences ▸ Edges and applies it (again whenever the preferences change).
    void applyPreferences();
    /// `edge` autohides (or stays) in the standard window, for this run (screenshots and tests; the preference is
    /// edges::setAutohide).
    void setAutohide(Edge edge, bool on);
    bool autohides(Edge edge) const { return autohide_[size_t(edge)]; }
    /// Whether an edge's part of the interface is in its flyout now: every edge in full screen, the autohiding ones
    /// in the standard window.
    bool claims(Edge edge) const { return edges_[size_t(edge)].claimed; }
    /// Whether `widget` (a dock, the tool rail, an options bar) is in a flyout's page rather than in the window.
    bool claimsWidget(const QWidget* widget) const;
    /// The part of the window the flyouts lie over and whose sides are the hot zones, in window coordinates: the
    /// whole window in full screen, the canvas's area in the standard window.
    QRect area() const;
    /// Gives the autohiding edges' parts back to the window while its layout is saved or restored, and takes them
    /// again on resume (nested pairs count; in full screen, nothing).
    void suspend();
    void resume();

    /// Whether an edge's flyout is out (sliding out, or out), and whether it is pinned out (Tab in full screen).
    bool isOut(Edge edge) const { return edges_[size_t(edge)].target; }
    bool isPinned(Edge edge) const { return edges_[size_t(edge)].pinned; }
    /// Whether an edge has anything to show (it is claimed and one of its pages is there).
    bool hasContent(Edge edge) const;
    Flyout* flyout(Edge edge) const { return edges_[size_t(edge)].flyout; }
    /// Which edge holds `dock`'s panel while claimed (-1: none), and the page it is on.
    int edgeHolding(QDockWidget* dock) const;
    QString pageHolding(QDockWidget* dock) const;
    /// The menu bar shown over the canvas while a menu is open.
    QMenuBar* menuStrip() const { return menuStrip_; }

    // ---- Pages
    /// The ids of the pages an edge shows now, in order ("tools", "brushes", "colour"; "options", "documents";
    /// "layers", "properties", "channels", "history"; "timeline", "info"). The Timeline's only while the document has
    /// frames or the Timeline was asked for.
    QStringList pages(Edge edge) const;
    /// Every page an edge has, shown or not, and each one's short label.
    QStringList allPages(Edge edge) const;
    QString pageLabel(Edge edge, const QString& id) const;
    /// The page on screen (or that the edge opens on).
    QString page(Edge edge) const;
    PageStrip* strip(Edge edge) const { return edges_[size_t(edge)].strip; }
    PageDeck* deck(Edge edge) const { return edges_[size_t(edge)].deck; }
    /// Flips `edge` by `step` pages, round; false when it has one page or none.
    bool flip(Edge edge, int step);
    /// Puts page `id` on `edge`, with the flip when the edge is out; `remember` keeps it as the edge's last page.
    bool showPage(Edge edge, const QString& id, bool remember = true);
    /// The edge Tab and Shift+Tab flip: one out and not pinned, under the pointer or else the last to come out; -1
    /// when none is.
    int flipTarget() const;
    /// Holding Tab (or Shift+Tab, all but the tools): every hidden edge out while `on`.
    void peek(bool on, bool withTools);
    bool peeking() const { return peeking_; }

    /// Tab: every edge out and pinned, or (when they all are) back to hidden. Shift+Tab: the same without the tools.
    void togglePinned(bool withTools);
    /// Every edge pinned out (`withTools`: the tools' too), or none; view.screenMode's `panels`.
    void pin(bool on, bool withTools);
    /// Whether exactly those edges are pinned (what Tab, or Shift+Tab, would hide again).
    bool pinnedAll(bool withTools) const;
    /// F7 and the Window menu's panels while their edge is claimed: slides that edge out on the panel's page; again
    /// while it is out on that page, slides it back.
    void revealPanel(QDockWidget* dock);
    /// F10 or Alt: the menu bar over the canvas with its first menu open; it hides again when the menus close.
    void showMenus();
    /// Esc: slides back every edge that is out (pinned too); false when none was.
    bool closeFlyouts();
    /// Slides `edge` out as a key would (it stays until the pointer has been in it), for screenshots.
    void showEdge(Edge edge);

    /// The tab on screen changed: its options bar goes in the top flyout.
    void currentTabChanged();
    /// The foreground or background colour changed (MainWindow::updateColorSwatches), and the pointer moved over the
    /// canvas (document coordinates): the Colour and Info pages follow.
    void coloursChanged();
    void pointerMoved(QPointF documentPoint);

    // For tests: how long a slide takes, how long a flyout waits once the pointer has left it, and how long the
    // pointer must rest in a hot zone of the standard window (ms); how long Tab is held before it peeks; whether the
    // pointer's position is polled (besides the pointer's events); how wide the hot zones are (logical px).
    void setTimings(int slideMs, int closeDelayMs, int dwellMs = 0) { slideMs_ = std::max(0, slideMs); closeDelayMs_ = std::max(0, closeDelayMs); dwellMs_ = std::max(0, dwellMs); }
    void setHoldDelay(int ms) { holdMs_ = std::max(1, ms); }
    void setPolling(bool on) { polling_ = on; }
    int hotZone() const { return zone_; }
    /// The flyout's geometry it slides to, in window coordinates.
    QRect shownGeometry(Edge edge) const;

signals:
    void activeChanged(bool active);
    /// The standard window's dock layout was given back (on leaving, and again as the window comes back to its size).
    void normalLayoutRestored();
    /// An edge went to another page.
    void pageChanged(int edge, const QString& id);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Page {
        QString id, label;
        QWidget* box = nullptr;            // in the deck
        QBoxLayout* layout = nullptr;
        QSplitter* split = nullptr;        // two panels side by side or one over the other
        QStringList docks;                 // the docks whose panels it holds, by object name
        QStringList guests;                // and the ones it holds while their own edge keeps them in the window
        bool fill = true;                  // a side page takes the edge's whole length
    };
    struct EdgeState {
        Flyout* flyout = nullptr;
        PageStrip* strip = nullptr;
        PageDeck* deck = nullptr;
        std::vector<Page> pages;
        int current = 0;
        QVariantAnimation* slide = nullptr;
        QTimer* closeTimer = nullptr;
        QTimer* dwell = nullptr;
        double progress = 0;   // 0 hidden, 1 out
        bool target = false;   // sliding out or out
        bool pinned = false;
        bool inZone = false;   // the pointer was in its hot zone at the last look
        bool held = false;     // opened from the keyboard: stays until the pointer has been in it
        bool peeked = false;   // out while Tab is held
        bool claimed = false;
        bool hidStatusBar = false;
        int extent = 0;        // the right column's width, the bottom row's height, from the window
        quint64 opened = 0;    // when it last came out, to find the last one
    };
    struct Moved {
        int edge = -1;
        QPointer<QDockWidget> dock;
        QPointer<QWidget> content;
        bool wasVisible = false;
    };

    void buildPages(Edge edge);
    void fillOwnPages();
    void claim(Edge edge);
    void unclaim(Edge edge);
    void engageNormal();
    bool anyClaimed() const;
    int pageIndex(Edge edge, const QString& id) const;
    bool pageAvailable(Edge edge, int index) const;
    void showPageIndex(Edge edge, int index, bool forwards, bool remember);
    void refreshStrip(Edge edge);
    void refreshPage(Edge edge);
    void refreshPagesLater();
    void followWindow(const QString& kind);
    int pageLength(Edge edge) const;
    void slide(Edge edge, bool out);
    void relayout();
    void pointerAt(QPoint windowPosition);
    void evaluate();
    bool keepsOut(Edge edge) const;
    int edgeOf(const QWidget* widget) const;
    bool inZone(Edge edge, QPoint p) const;
    bool atScreenEdge(Edge edge) const;
    void placeOptionsBar();
    void hideMenuStripLater();
    void moveFocusOut(Edge edge);
    void lendShortcuts();
    void takeBackShortcuts();
    // Tab and Shift+Tab: a flip while an edge is out, else a tap (Show or Hide Panels) or a hold (peek).
    bool edgeKey(QObject* watched, QKeyEvent* event);
    bool keyFree() const;
    QList<QKeySequence> commandKeys(const char* id) const;

    MainWindow* window_;
    bool active_ = false;
    std::array<EdgeState, edgeCount> edges_;
    std::array<bool, edgeCount> autohide_{};
    std::vector<Moved> moved_;
    QPointer<QToolBar> rail_;
    bool railWasVisible_ = true;
    std::vector<QPointer<QToolBar>> optionBars_;   // every tab's options bar, out of the window's layout
    QPointer<QToolBar> shownOptions_;
    QList<QAction*> addedShortcuts_;              // commands given to the window so their keys work with their bars hidden
    QMenuBar* menuStrip_ = nullptr;
    QTimer* tick_ = nullptr;
    QTimer* refreshTimer_ = nullptr;
    QByteArray savedState_, savedGeometry_;
    Qt::WindowStates savedWindowState_;
    bool menuBarShown_ = true, statusBarShown_ = true, tabBarShown_ = true;
    bool reapplyState_ = false;
    int suspended_ = 0;
    QSize normalSize_;
    QPoint lastPointer_;
    bool pointerKnown_ = false;
    QPoint lastPolled_{-100000, -100000};
    int pressEdge_ = -1;          // the flyout a held button went down in (-1: the canvas, or none)
    Qt::MouseButtons buttons_;    // the buttons held, from the pointer's events
    bool tabletDown_ = false;
    bool altArmed_ = false;
    int slideMs_ = 150, closeDelayMs_ = 500, dwellMs_ = 140, holdMs_ = 250, zone_ = 4;
    bool polling_ = true;
    bool reduceMotion_ = false;
    quint64 openCount_ = 0;
    // Tab held: which key, since when it was pressed (its timer), and whether it has brought the edges out.
    QTimer* holdTimer_ = nullptr;
    int heldKey_ = 0;            // 0: none, 1: Tab (Show or Hide Panels), 2: Shift+Tab (Except Tools)
    int heldCode_ = 0;           // the key held (Qt::Key_Tab)
    bool peeking_ = false;
    // The pages of the edges' own (EdgePageWidgets.h) and what the window last showed in them.
    BrushList* brushes_ = nullptr;
    ColourPage* colours_ = nullptr;
    DocumentsPage* documents_ = nullptr;
    InfoPanel* info_ = nullptr;
    bool timelineWanted_ = false;
    QColor lastForeground_;
    std::optional<bool> painting_;
    QPointer<EditorSession> layersOf_;
    QSet<QString> knownLayers_;
    QSet<EditorSession*> staleThumbnails_;
    std::vector<std::pair<QPointer<EditorSession>, QImage>> thumbnails_;
};

} // namespace app
