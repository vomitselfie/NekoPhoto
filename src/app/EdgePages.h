// The edges' pages (WindowEdges.h): each screen edge holds a small stack of pages (the left the tools, the brushes and
// the colours; the top the options bar and the open documents; the right the panels in four pages; the bottom the
// Timeline and the Info and Histogram), flipped with Tab and Shift+Tab while the edge is out, or with the wheel over
// its page strip. Here: the preferences (each edge Always shown or Autohide in the standard window, Reduce motion,
// the page each edge was last on), the strip naming the pages, and the deck that holds them and animates a flip: the
// page on screen slides back into the edge while the next one rises from underneath it, as a card pulled from under
// a deck (with Reduce motion, a quick crossfade). The animation paints two pictures of the pages over the real one,
// which is already in place under it and takes the pointer: a flip never holds up the pen or the pointer.
#pragma once
#include <QColor>
#include <QList>
#include <QObject>
#include <QPixmap>
#include <QString>
#include <QWidget>
#include <functional>
#include <vector>

class QVariantAnimation;

namespace app {

namespace motion { class Tween; }

namespace edges {

/// The edges, as WindowEdges numbers them.
enum EdgeId { Left, Top, Right, Bottom };
constexpr int count = 4;
/// "left", "top", "right", "bottom": the settings' and the automation's names.
const char* key(int edge);

/// Edit > Preferences > Edges: whether an edge of the standard window shows its part of the interface (as before) or
/// hides it, the canvas taking the window, until the pointer reaches the edge (`edges/<edge>/autohide`).
bool autohide(int edge);
void setAutohide(int edge, bool on);
/// Reduce motion (`edges/reduceMotion`): a flip crossfades, the edges come out and go back at once, and the rings and
/// the Layer Picker fade instead of moving (Motion.h).
bool reduceMotion();
void setReduceMotion(bool on);
/// The page an edge was on when it was last used (`edges/<edge>/page`), by its id; empty for the first.
QString lastPage(int edge);
void setLastPage(int edge, const QString& id);
/// The foreground colours used last, newest first (`edges/recentColours`, at most 16).
QList<QColor> recentColours();
void noteRecentColour(const QColor& color);

/// Tells the window that a preference changed (the dialog applies them as they are set).
class Notifier : public QObject {
    Q_OBJECT
signals:
    void changed();
};
Notifier* notifier();

} // namespace edges

/// The row of page names along a flyout's inner side: a dot and a short label for each page, the current one lit (its
/// pill sliding over to the new page at a flip). Too narrow for the labels (the tool rail), only the dots, with the
/// current page's label under them. The wheel over it, or a click on a page, flips the edge.
class PageStrip : public QWidget {
    Q_OBJECT
    Q_PROPERTY(QColor accent READ accent WRITE setAccent)
    Q_PROPERTY(QColor idle READ idle WRITE setIdle)
    Q_PROPERTY(bool candy READ candy WRITE setCandy)
public:
    explicit PageStrip(QWidget* parent = nullptr);
    void setPages(const QStringList& labels, int current);
    int current() const { return current_; }
    QStringList labels() const { return labels_; }
    /// The label of the page just flipped to glows for a moment.
    void flash();
    /// Whether the pill is sliding to the current page (for tests), and where it is drawn now.
    bool sliding() const;
    QRectF pillRect() const;
    /// Whether the labels fit beside each other (otherwise only the dots, and the current label under them).
    bool compact() const;
    QColor accent() const;
    void setAccent(const QColor& c) { accent_ = c; update(); }
    QColor idle() const;
    void setIdle(const QColor& c) { idle_ = c; update(); }
    /// The current page's pill in a gradient from `idle` to `accent` (Goth Kitty's lavender to pink) instead of flat.
    bool candy() const { return candy_; }
    void setCandy(bool on) { candy_ = on; update(); }
    /// The height the strip takes at `width` (two lines when compact).
    int heightForWidth(int width) const override;
    bool hasHeightForWidth() const override { return true; }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
signals:
    /// The wheel turned a notch over the strip: +1 the next page, -1 the previous.
    void stepRequested(int step);
    void pageClicked(int index);
protected:
    void paintEvent(QPaintEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
private:
    QRect itemRect(int index) const;
    QStringList labels_;
    int current_ = 0;
    QRectF slideFrom_;
    motion::Tween* slide_ = nullptr;
    int wheel_ = 0;
    double glow_ = 0;
    QVariantAnimation* flash_ = nullptr;
    QColor accent_, idle_;
    bool candy_ = false;
};

/// The pages of one edge, one on screen at a time, each filling the deck. A flip draws pictures of the old and the
/// new page over the new one for its animation (see the file's comment).
class PageDeck : public QWidget {
    Q_OBJECT
public:
    /// `edge`: the screen edge the deck lies along, which a page leaving slides into.
    explicit PageDeck(Qt::Edge edge, QWidget* parent = nullptr);
    int addPage(QWidget* page);
    int count() const { return int(pages_.size()); }
    QWidget* page(int index) const { return pages_[size_t(index)]; }
    int current() const { return current_; }
    /// Shows page `index` at once.
    void setCurrent(int index);
    /// Shows page `index` with the flip (forwards: the old page slides into the edge, dimming and settling back, as the
    /// new one rises from under it, lifting on a soft shadow; backwards: the new one comes out of the edge over the old
    /// one, which sinks). `relayout` runs between
    /// taking the old picture and the new one, for the flyout to take the new page's size.
    void flip(int index, bool forwards, const std::function<void()>& relayout);
    /// Whether a flip's animation is running, and how far along (0..1, eased).
    bool flipping() const;
    double flipProgress() const;
    /// Ends a running flip at once.
    void finishFlip();
    /// The colour behind the pages while they move (the flyout's card).
    void setBase(const QColor& c) { base_ = c; }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    /// For tests and screenshots: how long a flip takes (ms; 0 is no animation) and, with `freeze` in 0..1, holds every
    /// flip at that point (NEKOPHOTO_EDGE_FLIP_FREEZE does the same).
    static void setFlipTiming(int ms, double freeze = -1);
signals:
    void flipFinished();
protected:
    void resizeEvent(QResizeEvent*) override;
private:
    class Overlay;
    Qt::Edge edge_;
    std::vector<QWidget*> pages_;
    int current_ = -1;
    QColor base_;
    Overlay* overlay_ = nullptr;
};

} // namespace app
