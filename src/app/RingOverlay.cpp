// The rings' overlays: the pop, the close, the glide and the caption (RingOverlay.h).
#include "RingOverlay.h"
#include "Motion.h"
#include <QPainter>
#include <algorithm>
#include <cmath>

namespace app {

namespace {
QColor withAlpha(QColor c, double alpha) { c.setAlphaF(float(std::clamp(alpha, 0.0, 1.0))); return c; }
} // namespace

RingOverlay::RingOverlay(QWidget* parent) : QWidget(parent) {
    // Never in the way: the mouse goes through it to the canvas, and it never takes the keyboard.
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_NoMousePropagation);
    setFocusPolicy(Qt::NoFocus);
    hide();
    appear_ = new motion::Tween(this, [this] { update(); });
    appear_->finished = [this] {
        if (closing_ && appear_->value() <= 0) {
            closing_ = false;
            hide();
            glide_->jump(1);
            captionTimer_.stop();
            caption_.clear();
            hidden();
        }
    };
    glide_ = new motion::Tween(this, [this] {
        const double e = motion::easeOut(glide_->value());
        move(QPointF(motion::lerp(glideFrom_.x(), target_.x(), e), motion::lerp(glideFrom_.y(), target_.y(), e)).toPoint());
    });
    glide_->jump(1);
    captionFade_ = new motion::Tween(this, [this] { update(); });
    captionFade_->finished = [this] { if (captionFade_->value() <= 0) caption_.clear(); };
    captionRise_ = new motion::Tween(this, [this] { update(); });
    captionRise_->jump(1);
    captionTimer_.setSingleShot(true);
    connect(&captionTimer_, &QTimer::timeout, this, [this] {
        captionFade_->run(captionFade_->value(), 0, reduced_ ? motion::quickFadeMs : motion::captionOutMs);
    });
    hold_ = new motion::Tween(this, {});
    hold_->finished = [this] { startClose(); };
}

double RingOverlay::appearance() const { return isVisible() ? appear_->value() : 0; }
bool RingOverlay::opening() const { return !closing_ && appear_->running(); }
bool RingOverlay::animates() const { return isVisible() && !closing_ && !reduced_ && !motion::instant(); }
bool RingOverlay::gliding() const { return glide_->running(); }

QColor RingOverlay::glow() const { return glow_.isValid() ? glow_ : palette().color(QPalette::Highlight); }

void RingOverlay::present() {
    hold_->jump(0);
    reduced_ = motion::reduced();
    closing_ = hopping_ = false;
    if (isHidden()) {
        appear_->jump(0);
        captionFade_->jump(caption_.isEmpty() ? 0 : 1);
        show();
    }
    raise();
    // From where a close had got to: the pop picks up there rather than starting again from nothing.
    appear_->retarget(1, reduced_ ? motion::quickFadeMs : motion::popMs);
}

void RingOverlay::dismiss(bool slow, int afterMs) {
    if (isHidden()) return;
    closing_ = true;
    slow_ = slow;
    hold_->jump(0);
    if (afterMs > 0 && !reduced_ && !motion::instant()) {
        if (appear_->target() > 0) appear_->finish();   // a pop under way ends first
        hold_->run(0, 1, afterMs);
        return;
    }
    startClose();
}

void RingOverlay::startClose() {
    if (!closing_) return;
    const int ms = reduced_ ? motion::quickFadeMs : slow_ ? motion::idleCloseMs : motion::closeMs;
    // A close in the middle of the pop fades from how far the pop had got.
    appear_->retarget(0, ms);
}

void RingOverlay::finishMotion() {
    hold_->finish();
    appear_->finish();
    glide_->finish();
    captionFade_->finish();
    captionRise_->finish();
}

void RingOverlay::moveTo(QPoint pos, QPoint pointer) {
    if (pos == target_ && (gliding() || this->pos() == pos)) return;
    const QPoint now = this->pos();
    target_ = pos;
    if (!animates()) {
        glide_->jump(1);
        move(pos);
        return;
    }
    // Gliding towards the pointer would carry it across the pointer (and the stroke under it): it hops instead,
    // there at once with a short pop. Away from the pointer, it glides.
    const QPointF way = QPointF(pos - now);
    const QPointF toPointer = QPointF(pointer) - QRectF(QRect(now, size())).center();
    if (way.x() * toPointer.x() + way.y() * toPointer.y() > 0) {
        glide_->jump(1);
        move(pos);
        if (!opening()) {
            hopping_ = true;
            appear_->run(0.25, 1, int(motion::popMs * 0.75));
        }
        return;
    }
    glideFrom_ = now;
    glide_->run(0, 1, motion::glideMs);
}

void RingOverlay::showCaption(const QString& text, bool emphasise) {
    const bool was = !caption_.isEmpty() && captionFade_->target() > 0;
    caption_ = text;
    captionTimer_.start(text.size() > 40 ? 3200 : 1800);
    const int in = reduced_ ? motion::quickFadeMs : motion::captionInMs;
    if (emphasise && animates()) {
        captionFade_->run(0, 1, in);
        captionRise_->run(0, 1, in);
    } else if (!was || captionFade_->value() < 1) {
        captionFade_->retarget(1, isVisible() ? in : 0);
    }
    update();
}

double RingOverlay::captionOpacity() const { return caption_.isEmpty() ? 0 : motion::easeOut(captionFade_->value()); }

void RingOverlay::applyPop(QPainter& p, QPointF anchor, bool overshoot) const {
    const double a = appear_->value();
    double opacity = 1, scale = 1;
    if (reduced_) {
        opacity = a;
    } else if (closing_) {
        // Closing: fades as it shrinks back a little.
        const double t = 1 - a;
        opacity = 1 - motion::easeIn(t);
        scale = 1 - 0.08 * motion::easeIn(t);
    } else if (a < 1) {
        opacity = motion::easeOut(motion::window(a, 0, 0.6));
        const double grow = overshoot ? motion::backOut(a) : motion::easeOut(a);
        scale = motion::popScale + (1 - motion::popScale) * grow;
    }
    p.setOpacity(opacity);
    if (scale != 1) {
        p.translate(anchor);
        p.scale(scale, scale);
        p.translate(-anchor);
    }
}

double RingOverlay::fan(int index) const {
    if (reduced_ || closing_ || hopping_) return 1;
    const double a = appear_->value();
    if (a >= 1) return 1;
    const double from = (motion::fanDelayMs + index * motion::fanStaggerMs) / double(motion::popMs);
    return motion::window(a, from, motion::fanMs / double(motion::popMs));
}

void RingOverlay::paintCaption(QPainter& p, double top, int maxWidth, int maxHeight) {
    if (caption_.isEmpty()) return;
    const double opacity = captionOpacity();
    if (opacity <= 0) return;
    const QPalette pal = palette();
    const QColor fill = pal.color(QPalette::Window), text = pal.color(QPalette::WindowText), line = pal.color(QPalette::Link);
    QFont captionFont = font();
    captionFont.setPixelSize(11);
    p.save();
    p.setOpacity(p.opacity() * opacity);
    p.setFont(captionFont);
    const QFontMetrics fm(captionFont);
    QRect box = fm.boundingRect(QRect(0, 0, maxWidth - 12, maxHeight * 2), Qt::TextWordWrap | Qt::AlignHCenter, caption_);
    box.setHeight(std::min(box.height(), maxHeight - 4));
    // Rising the last few pixels into place as it fades in.
    const double rise = (1 - motion::easeOut(captionRise_->value())) * 5 + (1 - motion::easeOut(captionFade_->value())) * 2;
    const QRectF pill((width() - box.width()) / 2.0 - 6, top + rise, box.width() + 12, box.height() + 4);
    p.setPen(QPen(withAlpha(line, 0.6), 1));
    p.setBrush(withAlpha(fill, 0.88));
    p.drawRoundedRect(pill, 7, 7);
    p.setPen(text);
    p.drawText(pill.adjusted(6, 2, -6, -2), Qt::TextWordWrap | Qt::AlignCenter, caption_);
    p.restore();
}

} // namespace app
