// What the rings' overlays share (the Tool Ring's and the Colour Ring's compass, the Layer Picker's column): a widget
// over the canvas that takes no mouse and never the focus, and its motion (Motion.h). It pops open (growing from a
// little smaller with a slight overshoot as it fades in), fades out as it closes (slower on the idle timeout, so it is
// seen to go), glides out of the pointer's way (or, when the way would cross the pointer, hops there with a short pop),
// and its caption fades in and out. The controller (RingController.h) has already let go of the keys when a close
// starts: the fade is only paint.
#pragma once
#include <QColor>
#include <QPoint>
#include <QString>
#include <QTimer>
#include <QWidget>

class QPainter;

namespace app {

namespace motion { class Tween; }

class RingOverlay : public QWidget {
    Q_OBJECT
    /// The soft halo of a key just pressed (Goth Kitty: lavender, round the candy-pink ripple); the accent otherwise.
    Q_PROPERTY(QColor glow READ glow WRITE setGlow)
public:
    explicit RingOverlay(QWidget* parent);

    /// Shows it with the pop (from where a close had got to, if it is still fading).
    void present();
    /// Fades it out and hides it; `slow` on the idle timeout; `afterMs` holds it first (the picker's highlight going
    /// back to the layer Esc restores).
    void dismiss(bool slow, int afterMs = 0);
    /// Fading out after a close (it is still visible).
    bool dismissing() const { return closing_; }
    /// How far it is shown: 0 hidden, 1 shown (linear in its time; the painters apply the curves).
    double appearance() const;
    /// Ends every animation of it at once (the instant hook, a test).
    virtual void finishMotion();

    /// Moves it to `pos` (parent coordinates): a glide when it is shown and the way leads away from `pointer`, else
    /// at once with a short pop. Placement reads target(): the place it is going.
    void moveTo(QPoint pos, QPoint pointer);
    QPoint target() const { return target_; }
    QRect targetGeometry() const { return {target_, size()}; }
    bool gliding() const;

    /// A line under it for a moment (a name, why a key cannot run): it fades in, or with `emphasise` fades in again
    /// from nothing and rises into place, so a refusal is seen even over another caption.
    void showCaption(const QString& text, bool emphasise = false);
    QString caption() const { return caption_; }
    double captionOpacity() const;

    QColor glow() const;
    void setGlow(const QColor& c) { glow_ = c; update(); }

protected:
    /// The pop's opacity and scale about `anchor` on the painter (closing: the fade and a slight shrink). `overshoot`
    /// false keeps it inside the widget (the picker's panel fills it).
    void applyPop(QPainter& p, QPointF anchor, bool overshoot = true) const;
    /// How far satellite `index` (0..7, clockwise from the top) has fanned out from the centre: 0..1, eased (1 when
    /// shown, closing, or with Reduce motion).
    double fan(int index) const;
    /// Whether it is popping open now (not closing, not done).
    bool opening() const;
    /// Reduce motion, as it was when it opened (read once, never per frame).
    bool reducedMotion() const { return reduced_; }
    /// Whether a change it shows may animate now: it is on screen, not closing, and Reduce motion is off.
    bool animates() const;
    /// The caption's pill centred at `top`, at most `maxWidth` wide and `maxHeight` high.
    void paintCaption(QPainter& p, double top, int maxWidth, int maxHeight);
    /// Its own motion back to rest as it hides (a key's ripple, the picker's highlight).
    virtual void hidden() {}

private:
    void startClose();

    motion::Tween* appear_;
    motion::Tween* glide_;
    motion::Tween* captionFade_;
    motion::Tween* captionRise_;
    motion::Tween* hold_;      // the wait before a close's fade (on the animations' clock)
    QTimer captionTimer_;
    QPoint glideFrom_, target_;
    bool closing_ = false, slow_ = false, reduced_ = false, hopping_ = false;
    QString caption_;
    QColor glow_;
};

} // namespace app
