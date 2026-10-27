// The interface's motion (Motion.h): the curves, the preference, the test hooks and the tween.
#include "Motion.h"
#include "EdgePages.h"
#include <QAbstractAnimation>
#include <QCoreApplication>
#include <QPointer>
#include <QVariantAnimation>
#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace app::motion {

double clamp01(double t) { return std::clamp(t, 0.0, 1.0); }
double easeOut(double t) { t = 1 - clamp01(t); return 1 - t * t * t; }
double easeIn(double t) { t = clamp01(t); return t * t; }
double easeInOut(double t) { t = clamp01(t); return t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2; }
double backOut(double t, double overshoot) {
    t = clamp01(t) - 1;
    const double s = 1.70158 * overshoot;
    return 1 + t * t * ((s + 1) * t + s);
}
double window(double t, double from, double length) { return length <= 0 ? (t >= from ? 1 : 0) : clamp01((t - from) / length); }
double lerp(double a, double b, double t) { return a + (b - a) * t; }

bool reduced() { return edges::reduceMotion(); }

namespace {

bool instantOn = false;
double freezeAt = -2;   // -2: NEKOPHOTO_MOTION_FREEZE not read yet; < 0 off

bool manual = false;
/// The animations started through start() while the clock is manual: held paused, stepped by advance().
std::vector<QPointer<QVariantAnimation>>& stepped() {
    static std::vector<QPointer<QVariantAnimation>> all;
    return all;
}

} // namespace

void setInstant(bool on) { instantOn = on; }
bool instant() { return instantOn; }

void setFreeze(double at) { freezeAt = at >= 0 && at <= 1 ? at : -1; }

double freeze() {
    if (freezeAt == -2) {
        bool ok = false;
        const double v = qEnvironmentVariable("NEKOPHOTO_MOTION_FREEZE").toDouble(&ok);
        freezeAt = ok && v >= 0 && v <= 1 ? v : -1;
    }
    return freezeAt;
}

void setManualClock(bool on) {
    manual = on;
    if (on) return;
    // Back on Qt's clock: whatever was held runs on from where it is.
    for (const QPointer<QVariantAnimation>& anim : std::exchange(stepped(), {}))
        if (anim && anim->state() == QAbstractAnimation::Paused) anim->resume();
}

bool manualClock() { return manual; }

void start(QVariantAnimation* anim) {
    anim->start();
    if (!manual) return;
    // Held still, for advance() to step.
    anim->pause();
    auto& all = stepped();
    all.erase(std::remove_if(all.begin(), all.end(), [](const QPointer<QVariantAnimation>& a) { return !a || a->state() == QAbstractAnimation::Stopped; }), all.end());
    if (std::find(all.begin(), all.end(), anim) == all.end()) all.emplace_back(anim);
}

bool running(const QVariantAnimation* anim) { return anim->state() != QAbstractAnimation::Stopped; }

void advance(int ms) {
    QCoreApplication::processEvents();
    // A copy: an animation ending may start another (a close after a glide), which waits for the next step.
    const std::vector<QPointer<QVariantAnimation>> now = stepped();
    for (const QPointer<QVariantAnimation>& anim : now)
        if (anim && anim->state() == QAbstractAnimation::Paused) anim->setCurrentTime(std::min(anim->duration(), anim->currentTime() + std::max(0, ms)));
    QCoreApplication::processEvents();
}

Tween::Tween(QObject* parent, std::function<void()> changed) : QObject(parent), changed_(std::move(changed)) {
    anim_ = new QVariantAnimation(this);
    anim_->setStartValue(0.0);
    anim_->setEndValue(1.0);
    connect(anim_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        value_ = lerp(from_, to_, v.toDouble());
        if (changed_) changed_();
    });
    connect(anim_, &QVariantAnimation::finished, this, [this] {
        value_ = to_;
        if (changed_) changed_();
        if (finished) finished();
    });
}

bool Tween::running() const { return motion::running(anim_); }

void Tween::run(double from, double to, int ms) {
    anim_->stop();
    from_ = from;
    to_ = to;
    if (const double held = freeze(); held >= 0 && ms > 0) {
        value_ = lerp(from, to, held);
        if (changed_) changed_();
        return;
    }
    if (ms <= 0 || instantOn || from == to) {
        value_ = to;
        if (changed_) changed_();
        if (finished) finished();
        return;
    }
    value_ = from;
    anim_->setDuration(ms);
    motion::start(anim_);
}

void Tween::retarget(double to, int ms) {
    const double span = std::abs(to - value_);
    run(value_, to, span <= 0 ? 0 : std::max(1, int(std::lround(ms * std::min(1.0, span)))));
}

void Tween::jump(double value) {
    anim_->stop();
    from_ = to_ = value_ = value;
    if (changed_) changed_();
}

void Tween::finish() {
    if (!running()) return;
    anim_->stop();
    value_ = to_;
    if (changed_) changed_();
    if (finished) finished();
}

} // namespace app::motion
