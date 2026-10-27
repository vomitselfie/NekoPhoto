// The interface's motion in one place: how long the rings' pop, the Layer Picker's glide, the edges' slides and the
// page flips take, the curves they follow, Edit > Preferences' Reduce motion, and the hooks the tests and the preview
// clips use. Nothing here holds up input: every animation only paints (an opacity, a scale, a progress the widget
// reads in its paintEvent, or a move of an overlay that takes no mouse), the state it shows is already in place when
// it starts, and a key or the pointer during one acts at once, the animation finishing or turning towards the new
// state from where it is. With Reduce motion a pop or a close is a quick fade and the rest happens at once.
#pragma once
#include <QObject>
#include <functional>

class QVariantAnimation;

namespace app::motion {

// ---- Timings (ms) and sizes: tune them here ----------------------------------------------------------------------

// The rings (Tool Ring, Colour Ring) and the Layer Picker.
constexpr int popMs = 150;              // opening: the compass grows from popScale with a slight overshoot and fades in
constexpr double popScale = 0.85;
constexpr int fanDelayMs = 10;          // the satellites fan out from the centre: the first starts this late
constexpr int fanStaggerMs = 10;        // each one after the one before, clockwise from E
constexpr int fanMs = 70;               // one satellite's own way out
constexpr int closeMs = 90;             // closing: a quick fade as it shrinks back a little
constexpr int idleCloseMs = 320;        // closing on the idle timeout: slower, so it is seen to go
constexpr int pressMs = 260;            // a key's circle dips and bounces back as a ripple in the accent spreads
constexpr int tickMs = 140;             // a value's number rolls to the new one
constexpr int shakeMs = 300;            // a greyed key shakes its head
constexpr double shakePx = 4;
constexpr int captionInMs = 130, captionOutMs = 200;
constexpr int glideMs = 140;            // a ring moving out of the pointer's way; the picker's highlight and its list
constexpr int swatchMs = 80;            // the Colour Ring's centre swatch turning to the new colour
constexpr int swapMs = 200;             // its foreground and background trading places on a little arc
constexpr int resetMs = 220;            // its default colours: a pulse of the swatches
// The edges.
constexpr int slideMs = 150;            // a flyout sliding out or back
constexpr int peekStaggerMs = 25;       // hold Tab: left, top, right, bottom, this far apart
constexpr int flipMs = 180;             // a page turn
constexpr int stripMs = 160;            // the page strip's pill sliding to the new page
// Reduce motion.
constexpr int quickFadeMs = 70;         // a ring's or the picker's open and close, a flip (as a crossfade: flipFadeMs)
constexpr int flipFadeMs = 90;

// ---- Curves (t in 0..1) --------------------------------------------------------------------------------------------

double clamp01(double t);
double easeOut(double t);       // cubic
double easeIn(double t);        // quadratic
double easeInOut(double t);     // cubic
/// Out-back: past 1 by about 4% (the default overshoot) and back.
double backOut(double t, double overshoot = 1.0);
/// The part of 0..1 between `from` and `from + length`, as 0..1 (the stagger of one element of a group).
double window(double t, double from, double length);
double lerp(double a, double b, double t);

// ---- Preferences and hooks -------------------------------------------------------------------------------------------

/// Edit > Preferences > Reduce motion (edges::reduceMotion; read when an animation starts, never per frame).
bool reduced();
/// For tests: every animation of this file's kind ends at once (the self-tests run with it on, but for the motion
/// check), or (freeze in 0..1) holds at that point of its time, for screenshots (NEKOPHOTO_MOTION_FREEZE does the
/// same; NEKOPHOTO_EDGE_FLIP_FREEZE holds the page flips alone).
void setInstant(bool on);
bool instant();
void setFreeze(double at);
double freeze();
/// For tests and the preview clips: the animations started through start() (every one of the interface's) advance
/// only when `advance` says, so a frame can be taken at each step.
void setManualClock(bool on);
bool manualClock();
void advance(int ms);
/// Starts `anim` (held for advance() while the clock is manual), and whether it is under way (running, or held).
void start(QVariantAnimation* anim);
bool running(const QVariantAnimation* anim);

/// A progress over time, 0..1 and linear (the painter applies the curves, each part of a group its own window of
/// it), on a QVariantAnimation so the manual clock steps it. It calls `changed` at each step and `finished` at the
/// end; run() from where it is turns it round at once.
class Tween : public QObject {
public:
    Tween(QObject* parent, std::function<void()> changed);
    /// From `from` to `to` in `ms` (0, or the instant hook: at once; the freeze hook: held part of the way).
    void run(double from, double to, int ms);
    /// From the value now to `to`, in `ms` scaled by how far it has to go.
    void retarget(double to, int ms);
    void jump(double value);
    void finish();
    double value() const { return value_; }
    double target() const { return to_; }
    bool running() const;
    std::function<void()> finished;

private:
    QVariantAnimation* anim_;
    std::function<void()> changed_;
    double from_ = 0, to_ = 0, value_ = 0;
};

} // namespace app::motion
