// What the keyboard rings share (the Tool Ring, Alt+D; the Colour Ring, Alt+C; the Layer Picker, Alt+A): an overlay
// next to the pointer that never takes the mouse or the focus, opened and closed as a toggle, owning a few keys while
// it is open and leaving every other key and modifier alone. The controller places the overlay near the pointer on
// the side away from where it is heading, inside the canvas and clear of the brush outline, moves it only when the
// pointer would run into it, and closes it on Esc, another tool, another tab, the window losing the keyboard, typing
// starting, or the idle timeout (Edit > Preferences > Tool Ring's, shared by all of them). One ring is open at a time.
// A close lets go of the keys at once; the overlay's fade (RingOverlay.h) is only paint.
#pragma once
#include "EditorSession.h"
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QPointF>
#include <QRect>
#include <QSet>
#include <QTimer>
#include <utility>
#include <vector>

class QKeyEvent;
class QWidget;

namespace app {

class CanvasWidget;
class MainWindow;
class RingOverlay;

/// The keys around D (W E R / S D F / X C V) with no modifier, as the rings read them; 0 for any other key.
char compassKey(const QKeyEvent* e);
/// The direction of each of those keys from D, as they sit on the keyboard (unit length; D is 0, 0).
QPointF compassDirection(char key);

class RingController : public QObject {
    Q_OBJECT
public:
    RingController(MainWindow* window, RingOverlay* overlay);
    ~RingController() override;

    bool isOpen() const { return open_; }
    void toggle();
    /// Opens it by the pointer (the canvas's centre when the pointer is elsewhere); any other ring closes first.
    void open();
    void close();
    /// The tab on screen changed (and at the start): it closes, and follows that tab's session.
    void sessionChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

    // ---- What each ring decides
    /// Whether it opens at all now (a setting, a document); the shared checks come first.
    virtual bool mayOpen() const { return true; }
    /// Just before it shows: read what it needs, fill the overlay in (its size included).
    virtual void opening() {}
    /// It closed; `cancelled` when by Esc.
    virtual void closed(bool cancelled) { Q_UNUSED(cancelled); }
    /// Fill the overlay in again from the session.
    virtual void refresh() = 0;
    /// A key of its own (compassKey's, unless it takes others): 0 when the key is not its.
    virtual char keyOf(const QKeyEvent* e) const { return compassKey(e); }
    virtual void press(char key, bool autoRepeat) = 0;
    /// Esc: closes by default (the Layer Picker puts the layer back first).
    virtual void escape() { close(true); }
    /// A wheel notch over the canvas (true: more): false when the ring has nothing on the wheel now.
    virtual bool wantsWheel() const { return false; }
    virtual void wheelStep(bool more) { Q_UNUSED(more); }
    /// A click or pen press on the canvas (it goes on to the canvas as always).
    virtual void canvasPressed() {}
    /// The tool changed while open: closes by default.
    virtual void toolSwitched() { close(); }
    /// Connections of its own to the session, made before the shared ones (each through `follow`).
    virtual void connectSession(EditorSession& session) { Q_UNUSED(session); }
    /// The part of the overlay the pointer must keep clear of (overlay coordinates).
    virtual QRect keepClear() const;

    void close(bool cancelled);
    /// The idle timeout starts again (a ring key was used).
    void restartIdle();
    void follow(QMetaObject::Connection c) { sessionConnections_.push_back(c); }
    /// Places it again by the last pointer position (its size changed).
    void replace();
    /// The tool and whether it erases (an Eraser is the Brush with erase on): a change closes the ring.
    std::pair<Tool, bool> toolKey() const;
    bool typing() const;
    CanvasWidget* canvas() const;
    EditorSession* currentSession() const;
    /// Where the pointer was over the canvas when it opened (view coordinates), if it was.
    std::optional<QPointF> openedAt() const { return openedAt_; }

    MainWindow* window_;
    RingOverlay* overlay_;
    QPointer<EditorSession> session_;
    std::pair<Tool, bool> openedFor_{Tool::Move, false};

private:
    /// Places the overlay by the pointer at `pointer` (window coordinates); `heading` is where the pointer was moving.
    void place(QPoint pointer, QPointF heading);
    /// The pointer moved to `pointer` (window coordinates): the overlay moves away only when it would be run into.
    void pointerMoved(QPoint pointer);
    QRect canvasArea() const;
    /// How far around the pointer it keeps clear: the brush outline's radius on screen, for a brush.
    int clearance() const;

    bool open_ = false;
    bool idleClose_ = false;   // the close under way is the idle timeout's (a slower fade)
    std::optional<QPointF> openedAt_;
    double wheelRemainder_ = 0;
    QSet<int> heldKeys_;          // keys whose press the ring took (their releases are its too)
    QTimer idle_;
    QPoint lastPointer_;
    QPointF heading_;
    std::vector<QMetaObject::Connection> sessionConnections_;
};

} // namespace app
