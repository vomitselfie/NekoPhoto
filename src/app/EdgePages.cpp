// The edges' pages: see EdgePages.h.
#include "EdgePages.h"
#include "Motion.h"
#include <QApplication>
#include <QEasingCurve>
#include <QLayout>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace app {

namespace edges {

const char* key(int edge) {
    static const char* names[] = {"left", "top", "right", "bottom"};
    return names[std::clamp(edge, 0, count - 1)];
}

namespace {
QString settingKey(int edge, const char* what) { return QStringLiteral("edges/%1/%2").arg(QLatin1String(key(edge)), QLatin1String(what)); }
} // namespace

bool autohide(int edge) { return QSettings().value(settingKey(edge, "autohide"), false).toBool(); }

void setAutohide(int edge, bool on) {
    if (autohide(edge) == on) return;
    QSettings().setValue(settingKey(edge, "autohide"), on);
    emit notifier()->changed();
}

bool reduceMotion() { return QSettings().value(QStringLiteral("edges/reduceMotion"), false).toBool(); }

void setReduceMotion(bool on) {
    if (reduceMotion() == on) return;
    QSettings().setValue(QStringLiteral("edges/reduceMotion"), on);
    emit notifier()->changed();
}

QString lastPage(int edge) { return QSettings().value(settingKey(edge, "page")).toString(); }
void setLastPage(int edge, const QString& id) { QSettings().setValue(settingKey(edge, "page"), id); }

QList<QColor> recentColours() {
    QList<QColor> out;
    for (const QString& name : QSettings().value(QStringLiteral("edges/recentColours")).toStringList())
        if (const QColor c(name); c.isValid()) out << c;
    return out;
}

void noteRecentColour(const QColor& color) {
    if (!color.isValid()) return;
    QStringList names{color.name(QColor::HexRgb)};
    for (const QColor& c : recentColours())
        if (c.name(QColor::HexRgb) != names.front() && names.size() < 16) names << c.name(QColor::HexRgb);
    QSettings().setValue(QStringLiteral("edges/recentColours"), names);
}

Notifier* notifier() {
    static Notifier* n = new Notifier;
    return n;
}

} // namespace edges

// ---- PageStrip ------------------------------------------------------------------------------------------------

namespace {

constexpr int dot = 6, gap = 4, spacing = 12, pad = 8;

QFont stripFont(const QFont& base) {
    QFont f = base;
    f.setPointSizeF(std::max(7.0, base.pointSizeF() * 0.85));
    return f;
}

} // namespace

PageStrip::PageStrip(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("pageStrip"));
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    setCursor(Qt::PointingHandCursor);
    flash_ = new QVariantAnimation(this);
    flash_->setStartValue(1.0);
    flash_->setEndValue(0.0);
    flash_->setDuration(700);
    flash_->setEasingCurve(QEasingCurve::OutQuad);
    connect(flash_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) { glow_ = v.toDouble(); update(); });
    slide_ = new motion::Tween(this, [this] { update(); });
    slide_->jump(1);
}

QColor PageStrip::accent() const { return accent_.isValid() ? accent_ : palette().color(QPalette::Highlight); }

QColor PageStrip::idle() const {
    if (idle_.isValid()) return idle_;
    QColor c = palette().color(QPalette::WindowText);
    c.setAlpha(150);
    return c;
}

void PageStrip::setPages(const QStringList& labels, int current) {
    const bool changed = labels != labels_;
    const int now = std::clamp(current, 0, std::max(0, int(labels.size()) - 1));
    // Another page of the same ones: the pill slides over to it from where it is drawn.
    if (!changed && now != current_ && isVisible() && !motion::instant() && !edges::reduceMotion()) {
        slideFrom_ = pillRect();
        current_ = now;
        slide_->run(0, 1, motion::stripMs);
    } else {
        current_ = now;
        slide_->jump(1);
    }
    labels_ = labels;
    if (changed) updateGeometry();
    update();
}

bool PageStrip::sliding() const { return slide_->running(); }

QRectF PageStrip::pillRect() const {
    const QRectF to = QRectF(itemRect(current_)).adjusted(0.5, 0.5, -0.5, -0.5);
    if (!slide_->running() || slideFrom_.isNull()) return to;
    const double e = motion::easeOut(slide_->value());
    return {motion::lerp(slideFrom_.left(), to.left(), e), motion::lerp(slideFrom_.top(), to.top(), e),
            motion::lerp(slideFrom_.width(), to.width(), e), motion::lerp(slideFrom_.height(), to.height(), e)};
}

void PageStrip::flash() {
    flash_->stop();
    glow_ = 1;
    motion::start(flash_);
}

QSize PageStrip::sizeHint() const {
    const QFontMetrics fm(stripFont(font()));
    int w = pad;
    for (int i = 0; i < labels_.size(); i++) w += dot + gap + fm.horizontalAdvance(labels_[i]) + (i + 1 < labels_.size() ? spacing : 0);
    return {w + pad, fm.height() + 8};
}

QSize PageStrip::minimumSizeHint() const {
    const QFontMetrics fm(stripFont(font()));
    int widest = 0;
    for (const QString& l : labels_) widest = std::max(widest, fm.horizontalAdvance(l));
    const int dots = int(labels_.size()) * (dot + gap);
    return {std::max(dots, widest) + 6, dot + 6 + fm.height() + 4};
}

bool PageStrip::compact() const { return width() < sizeHint().width(); }

int PageStrip::heightForWidth(int width) const { return width < sizeHint().width() ? minimumSizeHint().height() : sizeHint().height(); }

QRect PageStrip::itemRect(int index) const {
    const QFontMetrics fm(stripFont(font()));
    if (compact()) {
        const int dots = int(labels_.size()) * (dot + gap) - gap;
        const int x = (width() - dots) / 2 + index * (dot + gap);
        return QRect(x - gap / 2, 0, dot + gap, height());
    }
    int x = pad;
    for (int i = 0; i < index; i++) x += dot + gap + fm.horizontalAdvance(labels_[i]) + spacing;
    const int w = dot + gap + fm.horizontalAdvance(labels_.value(index));
    return QRect(x - 4, 1, w + 8, height() - 2);
}

void PageStrip::paintEvent(QPaintEvent*) {
    if (labels_.isEmpty()) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QFont small = stripFont(font());
    QFont bold = small;
    bold.setBold(true);
    const QColor lit = accent(), dim = idle();
    const QColor onLit = palette().color(QPalette::HighlightedText);
    auto fillDot = [&](QPointF c, bool current) {
        if (current) {
            p.setPen(Qt::NoPen);
            p.setBrush(lit);
            p.drawEllipse(c, dot / 2.0, dot / 2.0);
        } else {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(dim, 1.2));
            p.drawEllipse(c, dot / 2.0 - 0.6, dot / 2.0 - 0.6);
        }
    };
    if (compact()) {
        // The dots in a row, the current page's label under them.
        for (int i = 0; i < labels_.size(); i++) {
            const QRect r = itemRect(i);
            fillDot(QPointF(r.center().x() + 0.5, 3 + dot / 2.0), false);
        }
        // The lit dot, sliding over to the new page.
        fillDot(QPointF(pillRect().center().x() + 0.5, 3 + dot / 2.0), true);
        QColor text = lit;
        if (glow_ > 0) text = QColor::fromRgbF(float(lit.redF() + (1 - lit.redF()) * glow_ * 0.6), float(lit.greenF() + (1 - lit.greenF()) * glow_ * 0.6), float(lit.blueF() + (1 - lit.blueF()) * glow_ * 0.6));
        p.setFont(bold);
        p.setPen(text);
        const QRect line(0, dot + 6, width(), height() - dot - 6);
        p.drawText(line, Qt::AlignHCenter | Qt::AlignTop, QFontMetrics(bold).elidedText(labels_[current_], Qt::ElideRight, width() - 2));
        return;
    }
    // The current page: a pill (sliding over to it after a flip), glowing for a moment after one. The labels under it
    // are drawn again in the pill's text colour, clipped to it, so a label changes colour as the pill passes.
    const QRectF pill = pillRect();
    QPainterPath pillPath;
    pillPath.addRoundedRect(pill, pill.height() / 2, pill.height() / 2);
    if (glow_ > 0) {
        QColor halo = lit;
        for (int ring = 3; ring >= 1; ring--) {
            halo.setAlphaF(float(0.18 * glow_ / ring));
            QPainterPath path;
            path.addRoundedRect(pill.adjusted(-ring * 1.5, -ring, ring * 1.5, ring), pill.height() / 2 + ring, pill.height() / 2 + ring);
            p.fillPath(path, halo);
        }
    }
    if (candy_) {
        QLinearGradient g(pill.topLeft(), pill.topRight());
        g.setColorAt(0, dim.isValid() ? QColor(dim.red(), dim.green(), dim.blue()) : lit);
        g.setColorAt(1, lit);
        p.fillPath(pillPath, g);
    } else {
        p.fillPath(pillPath, lit);
    }
    if (glow_ > 0) {
        QColor shine(255, 255, 255);
        shine.setAlphaF(float(0.35 * glow_));
        p.fillPath(pillPath, shine);
    }
    auto labels = [&](bool onPill) {
        for (int i = 0; i < labels_.size(); i++) {
            const QRect r = itemRect(i);
            const int x = r.left() + 4;
            if (onPill) {
                p.setPen(Qt::NoPen);
                p.setBrush(onLit);
                p.drawEllipse(QPointF(x + dot / 2.0, height() / 2.0), dot / 2.0, dot / 2.0);
            } else {
                fillDot(QPointF(x + dot / 2.0, height() / 2.0), false);
            }
            p.setFont(i == current_ ? bold : small);
            p.setPen(onPill ? onLit : dim);
            p.drawText(QRect(x + dot + gap, 0, r.right() - x - dot - gap + 1, height()), Qt::AlignVCenter | Qt::AlignLeft, labels_[i]);
        }
    };
    QPainterPath all;
    all.addRect(QRectF(rect()));
    p.save();
    p.setClipPath(all - pillPath);
    labels(false);
    p.restore();
    p.save();
    p.setClipPath(pillPath);
    labels(true);
    p.restore();
}

void PageStrip::wheelEvent(QWheelEvent* e) {
    // A notch (120) a page; a touchpad's small steps add up to one.
    const QPoint d = e->angleDelta();
    wheel_ += std::abs(d.y()) >= std::abs(d.x()) ? d.y() : d.x();
    while (wheel_ >= 120) { wheel_ -= 120; emit stepRequested(-1); }
    while (wheel_ <= -120) { wheel_ += 120; emit stepRequested(1); }
    e->accept();
}

void PageStrip::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    for (int i = 0; i < labels_.size(); i++)
        if (itemRect(i).contains(e->position().toPoint())) { emit pageClicked(i); return; }
    // Compact: a click on either half steps that way.
    if (compact()) emit stepRequested(e->position().x() < width() / 2.0 ? -1 : 1);
}

// ---- PageDeck -------------------------------------------------------------------------------------------------

namespace {
int flipMs = -1;          // -1: Motion.h's (a crossfade with Reduce motion)
double flipFreeze = -2;   // -2: not read from the environment yet; < 0 off

double freezeAt() {
    if (flipFreeze == -2) {
        bool ok = false;
        const double v = qEnvironmentVariable("NEKOPHOTO_EDGE_FLIP_FREEZE").toDouble(&ok);
        flipFreeze = ok && v >= 0 && v <= 1 ? v : -1;
    }
    return flipFreeze >= 0 ? flipFreeze : motion::freeze();
}
} // namespace

void PageDeck::setFlipTiming(int ms, double freeze) {
    flipMs = ms;
    flipFreeze = freeze >= 0 && freeze <= 1 ? freeze : -1;
}

/// The two pictures in motion over the deck (the pointer passes through it to the page underneath).
class PageDeck::Overlay : public QWidget {
public:
    explicit Overlay(PageDeck* deck) : QWidget(deck), deck_(deck) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        anim_ = new QVariantAnimation(this);
        anim_->setStartValue(0.0);
        anim_->setEndValue(1.0);
        QObject::connect(anim_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) { t_ = v.toDouble(); update(); });
        QObject::connect(anim_, &QVariantAnimation::finished, this, [this] { finish(); });
        hide();
    }
    void start(const QPixmap& from, const QPixmap& to, bool forwards) {
        anim_->stop();
        from_ = from;
        to_ = to;
        forwards_ = forwards;
        fade_ = edges::reduceMotion();
        setGeometry(deck_->rect());
        raise();
        const int ms = flipMs >= 0 ? flipMs : motion::instant() ? 0 : fade_ ? motion::flipFadeMs : motion::flipMs;
        if (const double frozen = freezeAt(); frozen >= 0) {
            // Held part-way, for a screenshot of the motion.
            t_ = frozen;
            show();
            update();
            return;
        }
        if (ms <= 0 || from_.isNull() || to_.isNull()) { finish(); return; }
        t_ = 0;
        show();
        anim_->setDuration(ms);
        motion::start(anim_);
    }
    void finish() {
        anim_->stop();
        if (isHidden()) return;
        hide();
        from_ = to_ = QPixmap();
        emit deck_->flipFinished();
    }
    bool running() const { return isVisible(); }
    double eased() const { return fade_ ? t_ : QEasingCurve(QEasingCurve::OutCubic).valueForProgress(t_); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const QRectF r = rect();
        p.fillRect(r, deck_->base_.isValid() ? deck_->base_ : palette().color(QPalette::Window));
        const double t = eased();
        if (fade_) {
            // Reduce motion: the new page shows through the old one as it fades.
            p.drawPixmap(QRectF(QPointF(0, 0), to_.deviceIndependentSize()), to_, to_.rect());
            p.setOpacity(1 - t);
            p.drawPixmap(QRectF(QPointF(0, 0), from_.deviceIndependentSize()), from_, from_.rect());
            return;
        }
        // Forwards the old page leaves into the edge over the new one rising; backwards the new one comes out of the
        // edge over the old one sinking.
        const QPixmap& under = forwards_ ? to_ : from_;
        const QPixmap& over = forwards_ ? from_ : to_;
        // Towards the screen edge, and how far the page on top travels to leave through it (its own size, and its
        // shadow with it).
        QPointF toward;
        const QSizeF overSize = over.deviceIndependentSize();
        double length = 16;
        switch (deck_->edge_) {
        case Qt::LeftEdge: toward = {-1, 0}; length += overSize.width(); break;
        case Qt::RightEdge: toward = {1, 0}; length += overSize.width(); break;
        case Qt::TopEdge: toward = {0, -1}; length += overSize.height(); break;
        case Qt::BottomEdge: toward = {0, 1}; length += overSize.height(); break;
        }
        const double rise = forwards_ ? t : 1 - t;   // 1: the card underneath is up, in place
        const double out = forwards_ ? t : 1 - t;    // 1: the card on top is all the way into the edge
        // Forwards the new page underneath lifts into place on a soft shadow that settles as it arrives, while the old
        // one on top dims and settles back a little as it goes; backwards the old page underneath sinks and the new
        // one comes out over it, growing the last little way to its size.
        const double lift = forwards_ ? std::sin(M_PI * rise) : 0;
        drawCard(p, under, 0.94 + 0.06 * rise, QPointF(0, 0), 0.55 * (1 - rise), false, toward, lift);
        if (out < 1) drawCard(p, over, 1.0 - 0.035 * out, toward * (out * length), forwards_ ? 0.3 * out : 0, true, toward);
    }

private:
    /// A page's picture as a card: scaled about its centre, moved by `offset`, darkened by `shade`, with a soft shadow
    /// on the side away from the edge when it is the one in motion on top.
    void drawCard(QPainter& p, const QPixmap& pix, double scale, QPointF offset, double shade, bool shadow, QPointF toward, double lift = 0) {
        if (pix.isNull()) return;
        const QSizeF size = pix.deviceIndependentSize();
        QRectF card(QPointF(0, 0), size * scale);
        card.moveCenter(QPointF(size.width() / 2, size.height() / 2) + offset);
        const double radius = 10;
        if (lift > 0.01) {
            // Lifting: a soft shadow all round, a little lower than the card.
            for (int i = 8; i >= 1; i--) {
                QPainterPath ring;
                ring.addRoundedRect(card.adjusted(-i, -i + 3, i, i + 3), (radius + i) * scale, (radius + i) * scale);
                p.fillPath(ring, QColor(0, 0, 0, int(std::lround(9 * lift))));
            }
        }
        if (shadow) {
            // The trailing side, away from the edge it slides into.
            const double depth = 14;
            QRectF band;
            QPointF a, b;
            if (toward.x() > 0) { band = QRectF(card.left() - depth, card.top(), depth, card.height()); a = band.topRight(); b = band.topLeft(); }
            else if (toward.x() < 0) { band = QRectF(card.right(), card.top(), depth, card.height()); a = band.topLeft(); b = band.topRight(); }
            else if (toward.y() > 0) { band = QRectF(card.left(), card.top() - depth, card.width(), depth); a = band.bottomLeft(); b = band.topLeft(); }
            else { band = QRectF(card.left(), card.bottom(), card.width(), depth); a = band.topLeft(); b = band.bottomLeft(); }
            QLinearGradient g(a, b);
            g.setColorAt(0, QColor(0, 0, 0, 110));
            g.setColorAt(1, QColor(0, 0, 0, 0));
            p.fillRect(band, g);
        }
        QPainterPath clip;
        clip.addRoundedRect(card, radius * scale, radius * scale);
        p.save();
        p.setClipPath(clip);
        p.drawPixmap(card, pix, pix.rect());
        if (shade > 0) p.fillRect(card, QColor(0, 0, 0, int(std::lround(255 * shade))));
        p.restore();
    }

    PageDeck* deck_;
    QVariantAnimation* anim_;
    QPixmap from_, to_;
    bool forwards_ = true, fade_ = false;
    double t_ = 0;
};

PageDeck::PageDeck(Qt::Edge edge, QWidget* parent) : QWidget(parent), edge_(edge) {
    setObjectName(QStringLiteral("pageDeck"));
    overlay_ = new Overlay(this);
}

int PageDeck::addPage(QWidget* page) {
    page->setParent(this);
    page->setGeometry(rect());
    page->hide();
    pages_.push_back(page);
    if (current_ < 0) setCurrent(0);
    overlay_->raise();
    return int(pages_.size()) - 1;
}

void PageDeck::setCurrent(int index) {
    if (index < 0 || index >= count()) return;
    finishFlip();
    for (int i = 0; i < count(); i++)
        if (i != index) pages_[size_t(i)]->hide();
    current_ = index;
    QWidget* page = pages_[size_t(index)];
    page->setGeometry(rect());
    page->show();
    updateGeometry();
}

void PageDeck::flip(int index, bool forwards, const std::function<void()>& relayout) {
    if (index < 0 || index >= count() || index == current_) { if (relayout) relayout(); return; }
    finishFlip();
    QWidget* from = current_ >= 0 ? pages_[size_t(current_)] : nullptr;
    const bool shown = isVisible() && from && from->isVisible() && !rect().isEmpty();
    // The page leaving as it is drawn now: a layout still waiting (a panel just moved onto it) is done first, so its
    // picture is not an empty card.
    if (shown) if (QLayout* l = from->layout()) l->activate();
    QPixmap before = shown ? from->grab() : QPixmap();
    setCurrent(index);
    if (relayout) relayout();
    if (!shown) return;
    QWidget* to = pages_[size_t(index)];
    to->setGeometry(rect());
    if (QLayout* l = to->layout()) l->activate();
    overlay_->start(before, to->grab(), forwards);
}

bool PageDeck::flipping() const { return overlay_->running(); }
double PageDeck::flipProgress() const { return overlay_->running() ? overlay_->eased() : 1.0; }
void PageDeck::finishFlip() { overlay_->finish(); }

QSize PageDeck::sizeHint() const { return current_ >= 0 ? pages_[size_t(current_)]->sizeHint() : QSize(0, 0); }
QSize PageDeck::minimumSizeHint() const { return current_ >= 0 ? pages_[size_t(current_)]->minimumSizeHint() : QSize(0, 0); }

void PageDeck::resizeEvent(QResizeEvent*) {
    if (current_ >= 0) pages_[size_t(current_)]->setGeometry(rect());
    overlay_->setGeometry(rect());
}

} // namespace app
