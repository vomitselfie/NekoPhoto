// The Tool Ring: settings, the overlay that paints it and the controller that drives it (ToolRing.h). The rings and
// their commands are in ToolRingCommands.cpp.
#include "ToolRing.h"
#include "CommandPalette.h"
#include "CommandRegistry.h"
#include "Icons.h"
#include "MainWindow.h"
#include "Motion.h"
#include <QAction>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QRegularExpression>
#include <QSettings>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>

namespace app {

// ---- Settings ------------------------------------------------------------------------------

namespace toolring {

bool enabled() { return QSettings().value(QStringLiteral("toolRing/enabled"), true).toBool(); }
void setEnabled(bool on) { QSettings().setValue(QStringLiteral("toolRing/enabled"), on); }
int idleMs() { return std::clamp(QSettings().value(QStringLiteral("toolRing/idleMs"), 3000).toInt(), 0, 60000); }
void setIdleMs(int ms) { QSettings().setValue(QStringLiteral("toolRing/idleMs"), std::clamp(ms, 0, 60000)); }
Corner corner() { return Corner(std::clamp(QSettings().value(QStringLiteral("toolRing/corner"), 0).toInt(), 0, 3)); }
void setCorner(Corner c) { QSettings().setValue(QStringLiteral("toolRing/corner"), int(c)); }
int penButton() { return std::clamp(QSettings().value(QStringLiteral("toolRing/penButton"), 0).toInt(), 0, 2); }
void setPenButton(int button) { QSettings().setValue(QStringLiteral("toolRing/penButton"), std::clamp(button, 0, 2)); }
Qt::MouseButton penMouseButton() {
    // Qt reports the pen's lower side button as the middle button and the upper one as the right (X11 and Wayland).
    switch (penButton()) {
    case 1: return Qt::MiddleButton;
    case 2: return Qt::RightButton;
    default: return Qt::NoButton;
    }
}

} // namespace toolring

namespace {

constexpr char ringKeys[] = {'W', 'E', 'R', 'S', 'F', 'X', 'C', 'V'};

/// The values a ring steps (the rest of a slot's `value` names a state that is on or off).
bool isNumericValue(const QString& value) {
    static const QStringList numeric = {QStringLiteral("size"), QStringLiteral("hardness"), QStringLiteral("opacity"), QStringLiteral("feather"),
                                        QStringLiteral("tolerance"), QStringLiteral("zoom"), QStringLiteral("typeSize"), QStringLiteral("font")};
    return numeric.contains(value);
}

/// The tool rail's icon of a tool (MainWindow::buildToolRail).
QString toolIconName(Tool tool, bool erase) {
    switch (tool) {
    case Tool::Move: return QStringLiteral("move");
    case Tool::Marquee: return QStringLiteral("square-dashed");
    case Tool::Lasso: return QStringLiteral("lasso");
    case Tool::Wand: return QStringLiteral("wand-sparkles");
    case Tool::Scribble: return QStringLiteral("scribble");
    case Tool::Crop: return QStringLiteral("crop");
    case Tool::Slice: return QStringLiteral("slice");
    case Tool::Artboard: return QStringLiteral("frame");
    case Tool::Brush: return erase ? QStringLiteral("eraser") : QStringLiteral("paintbrush");
    case Tool::SpotHealing: return QStringLiteral("bandage");
    case Tool::CloneStamp: return QStringLiteral("stamp");
    case Tool::Smudge: return QStringLiteral("droplet");
    case Tool::Dodge: return QStringLiteral("lollipop");
    case Tool::Gradient: return QStringLiteral("blend");
    case Tool::PaintBucket: return QStringLiteral("paint-bucket");
    case Tool::Pen: return QStringLiteral("pen-tool");
    case Tool::DirectSelect: return QStringLiteral("mouse-pointer-2");
    case Tool::Shape: return QStringLiteral("shapes");
    case Tool::Text: return QStringLiteral("type");
    case Tool::Eyedropper: return QStringLiteral("pipette");
    case Tool::Hand: return QStringLiteral("hand");
    case Tool::Zoom: return QStringLiteral("zoom-in");
    }
    return QStringLiteral("move");
}

// The compass's geometry in logical pixels: the orbit, the satellites' and the centre's radii, the key badges.
constexpr double orbit = 64, satellite = 23, hub = 27, badge = 8;
constexpr int captionHeight = 34;

QColor withAlpha(QColor c, double alpha) { c.setAlphaF(float(std::clamp(alpha, 0.0, 1.0))); return c; }

} // namespace

// ---- The overlay --------------------------------------------------------------------------------

namespace {

constexpr char slotKeys[] = "WERSDFXCV";
/// A key's place in keys_ (W E R S D F X C V), and its satellite's place in the fan, clockwise from E.
size_t slotOf(char key) {
    const char* at = key ? std::strchr(slotKeys, key) : nullptr;
    return at ? size_t(at - slotKeys) : 4;
}
int fanOrder(char key) {
    static const char order[] = "ERFVCXSW";
    const char* at = key ? std::strchr(order, key) : nullptr;
    return at ? int(at - order) : 0;
}

/// The first number in a value ("24 px": 24), to know which way it rolls.
std::optional<double> leadingNumber(const QString& text) {
    static const QRegularExpression number(QStringLiteral("-?\\d+(?:[.,]\\d+)?"));
    const QRegularExpressionMatch m = number.match(text);
    if (!m.hasMatch()) return std::nullopt;
    bool ok = false;
    const double v = m.captured(0).replace(QLatin1Char(','), QLatin1Char('.')).toDouble(&ok);
    return ok ? std::optional<double>(v) : std::nullopt;
}

QColor mix(const QColor& a, const QColor& b, double t) {
    return QColor::fromRgbF(float(motion::lerp(a.redF(), b.redF(), t)), float(motion::lerp(a.greenF(), b.greenF(), t)), float(motion::lerp(a.blueF(), b.blueF(), t)));
}

/// A press: the circle dips to 90% and springs back a little past its size.
double bounce(double t) {
    if (t <= 0 || t >= 1) return 1;
    if (t < 0.25) return 1 - 0.1 * motion::easeOut(t / 0.25);
    return 0.9 + 0.1 * motion::backOut((t - 0.25) / 0.75, 1.6);
}

} // namespace

ToolRingOverlay::ToolRingOverlay(QWidget* parent) : RingOverlay(parent) {
    setObjectName(QStringLiteral("toolRing"));
    resize(preferredSize());
    swatch_ = new motion::Tween(this, [this] { update(); });
    swatch_->jump(1);
    swap_ = new motion::Tween(this, [this] { update(); });
    swap_->jump(1);
}

ToolRingOverlay::KeyMotion& ToolRingOverlay::motionOf(char key) {
    KeyMotion& k = keys_[slotOf(key)];
    if (!k.press) {
        k.press = new motion::Tween(this, [this] { update(); });
        k.shake = new motion::Tween(this, [this] { update(); });
        k.tick = new motion::Tween(this, [this] { update(); });
        k.press->jump(1);
        k.shake->jump(1);
        k.tick->jump(1);
    }
    return k;
}

QSize ToolRingOverlay::preferredSize() {
    const int side = int(std::ceil(2 * (orbit + satellite + badge))) + 8;
    return {side + 24, side + captionHeight};
}

QRect ToolRingOverlay::compassRect() const {
    const int side = int(std::ceil(2 * (orbit + satellite + badge))) + 8;
    return {(width() - side) / 2, 0, side, side};
}

QPoint ToolRingOverlay::centreOf(char key) const {
    const QRect c = compassRect();
    const QPointF centre = QRectF(c).center();
    return (centre + compassDirection(key) * orbit).toPoint();
}

void ToolRingOverlay::setViews(const std::vector<View>& views, const QString& centreIcon) {
    // A value that changed rolls to the new number: up when it grew, down when it fell.
    if (animates()) {
        for (const View& v : views) {
            const auto old = std::find_if(views_.begin(), views_.end(), [&](const View& o) { return o.key == v.key; });
            if (old == views_.end() || old->value == v.value || old->value.isEmpty() || v.value.isEmpty()) continue;
            KeyMotion& k = motionOf(v.key);
            const auto before = leadingNumber(old->value), after = leadingNumber(v.value);
            k.before = old->value;
            k.roll = before && after && *after < *before ? -1 : 1;
            k.tick->run(0, 1, motion::tickMs);
        }
    }
    views_ = views;
    centreIcon_ = centreIcon;
    update();
}

std::optional<std::pair<QColor, QColor>> ToolRingOverlay::shownSwatches() const {
    if (!centreSwatches_ || !swatchesFrom_ || !swatch_->running()) return centreSwatches_;
    const double e = motion::easeOut(swatch_->value());
    return std::pair(mix(swatchesFrom_->first, centreSwatches_->first, e), mix(swatchesFrom_->second, centreSwatches_->second, e));
}

void ToolRingOverlay::setCentreSwatches(std::optional<std::pair<QColor, QColor>> colors) {
    const bool swap = std::exchange(swapNext_, false);
    if (colors && centreSwatches_ && *colors != *centreSwatches_ && animates()) {
        if (swap) {
            // The two trade places on an arc (the colours stay theirs).
            swatch_->jump(1);
            swap_->run(0, 1, motion::swapMs);
        } else {
            // The swatch turns to the new colour, from the one it shows now (mid-turn too).
            swatchesFrom_ = shownSwatches();
            swatch_->run(0, 1, motion::swatchMs);
        }
    }
    centreSwatches_ = colors;
    update();
}

void ToolRingOverlay::pulse(char key, bool enabled) {
    KeyMotion& k = motionOf(key);
    k.pressEnabled = enabled;
    // With Reduce motion only the ripple's fade, in place.
    k.press->run(0, 1, reducedMotion() ? motion::quickFadeMs * 2 : key == 'D' && centreSwatches_ ? motion::resetMs : motion::pressMs);
    if (!enabled && animates()) k.shake->run(0, 1, motion::shakeMs);
}

bool ToolRingOverlay::pressing(char key) const { const KeyMotion& k = keys_[slotOf(key)]; return k.press && k.press->running(); }
bool ToolRingOverlay::shaking(char key) const { const KeyMotion& k = keys_[slotOf(key)]; return k.shake && k.shake->running(); }
bool ToolRingOverlay::ticking(char key) const { const KeyMotion& k = keys_[slotOf(key)]; return k.tick && k.tick->running(); }
bool ToolRingOverlay::swatchMoving() const { return swatch_->running() || swap_->running(); }

void ToolRingOverlay::finishMotion() {
    RingOverlay::finishMotion();
    for (KeyMotion& k : keys_)
        if (k.press) { k.press->finish(); k.shake->finish(); k.tick->finish(); }
    swatch_->finish();
    swap_->finish();
}

void ToolRingOverlay::hidden() {
    for (KeyMotion& k : keys_)
        if (k.press) { k.press->jump(1); k.shake->jump(1); k.tick->jump(1); }
    swatch_->jump(1);
    swap_->jump(1);
}

QPixmap ToolRingOverlay::icon(const QString& name, const QColor& color, int size) {
    const double dpr = devicePixelRatioF();
    const QString key = QStringLiteral("%1|%2|%3|%4").arg(name, color.name(QColor::HexArgb)).arg(size).arg(dpr);
    auto it = icons_.find(key);
    if (it == icons_.end()) it = icons_.insert(key, renderIcon(name, color, size, dpr));
    return *it;
}

void ToolRingOverlay::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QPalette pal = palette();
    // Mostly see-through, in the theme's colours: Goth Kitty's plum, candy pink and lavender; the others' own.
    const QColor fill = pal.color(QPalette::Window), base = pal.color(QPalette::Base), text = pal.color(QPalette::WindowText);
    const QColor accent = pal.color(QPalette::Highlight), onAccent = pal.color(QPalette::HighlightedText), line = pal.color(QPalette::Link);
    const QColor iconColor = pal.color(QPalette::ButtonText), halo = glow();
    const QRect compass = compassRect();
    const QPointF centre = QRectF(compass).center();
    const bool still = reducedMotion();
    QFont small = font();
    small.setPixelSize(10);
    QFont letter = font();
    letter.setPixelSize(10);
    letter.setBold(true);
    // Popping open: growing from a little smaller about the centre as it fades in.
    applyPop(p, centre);

    // The orbit, faint.
    p.setPen(QPen(withAlpha(line, 0.28), 1));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(centre, orbit, orbit);

    auto drawBadge = [&](QPointF at, char key, double alpha) {
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(accent, 0.92 * alpha));
        p.drawEllipse(at, badge, badge);
        p.setPen(withAlpha(onAccent, alpha));
        p.setFont(letter);
        p.drawText(QRectF(at.x() - badge, at.y() - badge, 2 * badge, 2 * badge), Qt::AlignCenter, QString(QChar(key)));
    };
    // A key just pressed: a soft halo in the glow colour behind it and a ring in the accent that swells and fades;
    // muted for a greyed key, and in place with Reduce motion.
    auto drawRipple = [&](QPointF at, double radius, const KeyMotion& k) {
        if (!k.press || !k.press->running()) return;
        const double t = k.press->value(), fade = 1 - t;
        if (k.pressEnabled) {
            QRadialGradient g(at, radius + 14);
            g.setColorAt(radius / (radius + 14), withAlpha(halo, 0.5 * fade));
            g.setColorAt(1, withAlpha(halo, 0));
            p.setPen(Qt::NoPen);
            p.setBrush(g);
            p.drawEllipse(at, radius + 14, radius + 14);
        }
        const double swell = still ? 3 : 9 * motion::easeOut(t);
        p.setPen(QPen(withAlpha(k.pressEnabled ? accent : text, fade * (k.pressEnabled ? 0.95 : 0.5)), 2.5));
        p.setBrush(withAlpha(k.pressEnabled ? accent : fill, fade * 0.3));
        p.drawEllipse(at, radius + swell, radius + swell);
    };

    for (const View& v : views_) {
        const QPointF dir = compassDirection(v.key);
        const KeyMotion& k = keys_[slotOf(v.key)];
        // Fanning out from the centre as it opens (each a little after the one before); shaking for a greyed key.
        const double f = fan(fanOrder(v.key)), shown = motion::easeOut(std::min(1.0, f * 1.6));
        if (shown <= 0) continue;
        const double shake = k.shake && k.shake->running() ? motion::shakePx * std::sin(k.shake->value() * 6 * M_PI) * (1 - k.shake->value()) : 0;
        const QPointF at = centre + dir * orbit * (0.3 + 0.7 * motion::backOut(f, 0.6)) + QPointF(shake, 0);
        const double grow = (0.6 + 0.4 * motion::easeOut(f)) * (k.press && !still && k.pressEnabled ? bounce(k.press->value()) : 1);
        const double alpha = v.empty ? 0.28 : v.enabled ? 1.0 : 0.45;
        p.save();
        p.setOpacity(p.opacity() * shown);
        drawRipple(at, satellite * grow, k);
        p.translate(at);
        p.scale(grow, grow);
        // The circle: plum glass, pink when its state is on.
        if (v.empty) {
            p.setPen(QPen(withAlpha(line, 0.35), 1, Qt::DashLine));
            p.setBrush(withAlpha(fill, 0.25));
        } else {
            p.setPen(QPen(withAlpha(v.checked ? accent : line, v.enabled ? 0.85 : 0.4), v.checked ? 2 : 1.2));
            p.setBrush(v.checked ? withAlpha(accent, 0.42) : withAlpha(fill, 0.55));
        }
        p.drawEllipse(QPointF(0, 0), satellite, satellite);
        if (!v.empty) {
            const int iconSize = 18;
            const QColor tint = withAlpha(v.checked ? text : iconColor, alpha);
            const double iconY = v.value.isEmpty() ? -iconSize / 2.0 : -iconSize / 2.0 - 6;
            const double opacity = p.opacity();
            p.setOpacity(opacity * alpha);
            if (v.chip) {
                // A colour: a chip, ringed so that black and white show on the plum.
                const QPointF chipAt(0, iconY + iconSize / 2.0);
                p.setPen(QPen(withAlpha(text, 0.75), 1.2));
                p.setBrush(QColor(v.chip->rgb()));
                p.drawEllipse(chipAt, 8.5, 8.5);
            } else {
                p.drawPixmap(QPointF(-iconSize / 2.0, iconY), icon(v.icon, QColor(tint.rgb()), iconSize));
            }
            p.setOpacity(opacity);
            if (!v.value.isEmpty()) {
                p.setFont(small);
                const QRectF valueBox(-satellite + 3, 4, 2 * satellite - 6, 13);
                const QFontMetrics fm(small);
                if (k.tick && k.tick->running()) {
                    // The number rolls: the old one out, the new one in from the other side.
                    const double e = motion::easeOut(k.tick->value()), travel = valueBox.height() * 0.8;
                    p.save();
                    p.setClipRect(valueBox.adjusted(0, -1, 0, 1));
                    p.setPen(withAlpha(text, alpha * (1 - e)));
                    p.drawText(valueBox.translated(0, -k.roll * travel * e), Qt::AlignCenter, fm.elidedText(k.before, Qt::ElideRight, int(valueBox.width())));
                    p.setPen(withAlpha(text, alpha * e));
                    p.drawText(valueBox.translated(0, k.roll * travel * (1 - e)), Qt::AlignCenter, fm.elidedText(v.value, Qt::ElideRight, int(valueBox.width())));
                    p.restore();
                } else {
                    p.setPen(withAlpha(text, alpha));
                    p.drawText(valueBox, Qt::AlignCenter, fm.elidedText(v.value, Qt::ElideRight, int(valueBox.width())));
                }
            }
            // The compass rule, spelt out: more towards F and R, less towards S and V.
            if (v.direction) {
                p.setFont(letter);
                p.setPen(withAlpha(line, alpha));
                const QPointF signAt = dir * (satellite + 1) + QPointF(-dir.y(), dir.x()) * (badge + 6);   // beside the key
                p.drawText(QRectF(signAt.x() - 6, signAt.y() - 7, 12, 14), Qt::AlignCenter, v.direction > 0 ? QStringLiteral("+") : QStringLiteral("−"));
            }
        }
        drawBadge(dir * (satellite + 1), v.key, v.empty ? 0.35 : alpha);
        p.restore();
    }

    // The centre: the tool, its D; it bounces when D is pressed.
    const KeyMotion& d = keys_[slotOf('D')];
    const double hubGrow = d.press && !still && d.pressEnabled ? bounce(d.press->value()) : 1;
    drawRipple(centre, hub * hubGrow, d);
    p.save();
    p.translate(centre);
    p.scale(hubGrow, hubGrow);
    p.translate(-centre);
    p.setPen(QPen(withAlpha(line, 0.9), 1.5));
    p.setBrush(withAlpha(base, 0.78));
    p.drawEllipse(centre, hub, hub);
    if (const auto swatches = shownSwatches()) {
        // The toolbar's swatches: the background behind, down and right, the foreground over it. A swap carries each
        // to the other's place on an arc; the default colours (D) pulse them.
        const QPointF front = centre + QPointF(-13, -13), back = centre + QPointF(-4, -4);
        const double pulseT = d.press && d.press->running() && !still ? d.press->value() : 1;
        const double scale = 1 + 0.14 * std::sin(M_PI * pulseT);
        auto box = [&](QPointF topLeft, const QColor& c) {
            const QRectF r(centre + (topLeft - centre) * scale, QSizeF(17, 17) * scale);
            p.setPen(QPen(withAlpha(text, 0.8), 1.2));
            p.setBrush(QColor(c.rgb()));
            p.drawRoundedRect(r, 3, 3);
        };
        if (swap_->running()) {
            const double e = motion::easeInOut(swap_->value()), arc = std::sin(M_PI * e) * 9;
            const QPointF side(0.70710678, -0.70710678);
            const QPointF toFront = back + (front - back) * e + side * arc;   // the new foreground, coming from behind
            const QPointF toBack = front + (back - front) * e - side * arc;   // the new background, going behind
            if (e < 0.5) { box(toFront, swatches->first); box(toBack, swatches->second); }
            else { box(toBack, swatches->second); box(toFront, swatches->first); }
        } else {
            box(back, swatches->second);
            box(front, swatches->first);
        }
    } else if (!centreIcon_.isEmpty()) {
        p.drawPixmap(QPointF(centre.x() - 11, centre.y() - 11), icon(centreIcon_, QColor(iconColor.rgb()), 22));
    }
    drawBadge(centre + QPointF(-hub * 0.72, -hub * 0.72), 'D', 1);
    p.restore();

    // The caption under the compass: the tool's name, a command's, or why a greyed one cannot run.
    paintCaption(p, compass.bottom() + 1, width() - 8, captionHeight);
}

// ---- The controller ----------------------------------------------------------------------------

ToolRing::ToolRing(MainWindow* window) : RingController(window, new ToolRingOverlay(window)) {
    ringOverlay_ = static_cast<ToolRingOverlay*>(RingController::overlay_);
}

void ToolRing::connectSession(EditorSession& session) {
    presetNow_ = session.brushPreset;
    presetBefore_.reset();
    follow(connect(&session, &EditorSession::toolChanged, this, [this] {
        // The brush preset before this one, for Previous Brush Preset.
        if (session_->brushPreset != presetNow_) { presetBefore_ = presetNow_; presetNow_ = session_->brushPreset; }
        // The ring's own commands follow the tool (greyed where it has no such option).
        if (registry_)
            for (QAction* a : ownCommands_)
                if (const Command* c = registry_->forAction(a)) a->setEnabled(registry_->disabledReason(*c).isEmpty());
    }));
}

void ToolRing::toolSwitched() {
    // Another tool closes the ring, unless the ring switched it (C: Brush and Eraser), which keeps it open.
    if (!ringChanging_) { close(); return; }
    openedFor_ = toolKey();
    ring_ = &toolring::ringFor(openedFor_.first);
}

bool ToolRing::mayOpen() const { return toolring::enabled() && registry_; }

void ToolRing::opening() {
    ring_ = &toolring::ringFor(openedFor_.first);
    // The wheel steps the ring's first value (its S/F pair, else the first it has) until another is used.
    wheelKey_ = 0;
    for (char key : {'F', 'R', 'E'})
        for (const toolring::Slot& s : ring_->satellites)
            if (!wheelKey_ && s.key == key && isNumericValue(s.value)) wheelKey_ = key;
    if (const Command* c = window_->toolActions_.contains(openedFor_.first) ? registry_->forAction(openedFor_.second ? window_->eraserAction_ : window_->toolActions_.value(openedFor_.first)) : nullptr)
        ringOverlay_->showCaption(plainText(c->label));
}

void ToolRing::refresh() {
    if (!ring_ || !registry_) return;
    std::vector<ToolRingOverlay::View> views;
    for (char key : ringKeys) {
        ToolRingOverlay::View v;
        v.key = key;
        const auto slot = std::find_if(ring_->satellites.begin(), ring_->satellites.end(), [key](const toolring::Slot& s) { return s.key == key; });
        const Command* c = slot == ring_->satellites.end() ? nullptr : registry_->find(slot->command);
        if (!c) { v.empty = true; views.push_back(v); continue; }
        v.icon = slot->icon;
        v.enabled = registry_->disabledReason(*c).isEmpty();
        if (isNumericValue(slot->value)) {
            v.value = valueText(slot->value);
            if (toolring::oppositeKey(key)) v.direction = toolring::isMoreKey(key) ? 1 : -1;
        } else if (!slot->value.isEmpty()) {
            v.checked = valueChecked(slot->value);
            v.value = valueText(slot->value);   // a state with words of its own (the alignment)
        }
        views.push_back(v);
    }
    ringOverlay_->setViews(views, toolIconName(openedFor_.first, openedFor_.second));
}

void ToolRing::press(char key, bool autoRepeat) {
    if (!ring_ || !registry_) return;
    restartIdle();
    const bool centre = key == 'D';
    const auto slot = std::find_if(ring_->satellites.begin(), ring_->satellites.end(), [key](const toolring::Slot& s) { return s.key == key; });
    const QString id = centre ? ring_->centre : slot == ring_->satellites.end() ? QString() : slot->command;
    const bool numeric = !centre && slot != ring_->satellites.end() && isNumericValue(slot->value);
    if (autoRepeat && !numeric) return;   // a held key repeats values, not actions
    const Command* c = id.isEmpty() ? nullptr : registry_->find(id);
    if (!c) {
        if (centre) {
            // D with nothing of its own: the tool's name.
            ringOverlay_->pulse('D', true);
            QAction* toolAction = openedFor_.second ? window_->eraserAction_ : window_->toolActions_.value(openedFor_.first);
            if (const Command* t = toolAction ? registry_->forAction(toolAction) : nullptr) ringOverlay_->showCaption(plainText(t->label));
        }
        return;
    }
    if (const QString why = registry_->disabledReason(*c); !why.isEmpty()) {
        ringOverlay_->pulse(key, false);
        ringOverlay_->showCaption(why, true);
        return;
    }
    ringChanging_ = true;
    registry_->execute(*c);
    ringChanging_ = false;
    if (!isOpen()) return;   // the command closed it (a dialog took the keyboard)
    ringOverlay_->pulse(key, true);
    if (numeric) wheelKey_ = key;
    else ringOverlay_->showCaption(plainText(c->label));
    refresh();
}

void ToolRing::wheelStep(bool more) {
    // The value used last, a notch a step.
    press(more == toolring::isMoreKey(wheelKey_) ? wheelKey_ : toolring::oppositeKey(wheelKey_), false);
}

} // namespace app
