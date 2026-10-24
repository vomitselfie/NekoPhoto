// The Tool Ring: settings, the overlay that paints it and the controller that drives it (ToolRing.h). The rings and
// their commands are in ToolRingCommands.cpp.
#include "ToolRing.h"
#include "CommandPalette.h"
#include "CommandRegistry.h"
#include "Icons.h"
#include "MainWindow.h"
#include <QAction>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QVariantAnimation>
#include <algorithm>
#include <array>
#include <cmath>

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

ToolRingOverlay::ToolRingOverlay(QWidget* parent) : QWidget(parent) {
    // Never in the way: the mouse goes through it to the canvas, and it never takes the keyboard.
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_NoMousePropagation);
    setFocusPolicy(Qt::NoFocus);
    setObjectName(QStringLiteral("toolRing"));
    resize(preferredSize());
    hide();
    captionTimer_.setSingleShot(true);
    connect(&captionTimer_, &QTimer::timeout, this, [this] { caption_.clear(); update(); });
    pulseAnimation_ = new QVariantAnimation(this);
    pulseAnimation_->setStartValue(0.0);
    pulseAnimation_->setEndValue(1.0);
    pulseAnimation_->setDuration(280);
    connect(pulseAnimation_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) { pulse_ = v.toDouble(); update(); });
    connect(pulseAnimation_, &QVariantAnimation::finished, this, [this] { pulseKey_ = 0; update(); });
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
    views_ = views;
    centreIcon_ = centreIcon;
    update();
}

void ToolRingOverlay::setCentreSwatches(std::optional<std::pair<QColor, QColor>> colors) {
    centreSwatches_ = colors;
    update();
}

void ToolRingOverlay::pulse(char key, bool enabled) {
    pulseKey_ = key;
    pulseEnabled_ = enabled;
    pulseAnimation_->stop();
    pulse_ = 0;
    pulseAnimation_->start();
}

void ToolRingOverlay::showCaption(const QString& text) {
    caption_ = text;
    captionTimer_.start(text.size() > 40 ? 3200 : 1800);
    update();
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
    const QColor iconColor = pal.color(QPalette::ButtonText);
    const QRect compass = compassRect();
    const QPointF centre = QRectF(compass).center();
    QFont small = font();
    small.setPixelSize(10);
    QFont letter = font();
    letter.setPixelSize(10);
    letter.setBold(true);

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

    for (const View& v : views_) {
        const QPointF dir = compassDirection(v.key);
        const QPointF at = centre + dir * orbit;
        const double alpha = v.empty ? 0.28 : v.enabled ? 1.0 : 0.45;
        // The circle: plum glass, pink when its state is on.
        if (v.empty) {
            p.setPen(QPen(withAlpha(line, 0.35), 1, Qt::DashLine));
            p.setBrush(withAlpha(fill, 0.25));
        } else {
            p.setPen(QPen(withAlpha(v.checked ? accent : line, v.enabled ? 0.85 : 0.4), v.checked ? 2 : 1.2));
            p.setBrush(v.checked ? withAlpha(accent, 0.42) : withAlpha(fill, 0.55));
        }
        p.drawEllipse(at, satellite, satellite);
        if (!v.empty) {
            const int iconSize = 18;
            const QColor tint = withAlpha(v.checked ? text : iconColor, alpha);
            const double iconY = v.value.isEmpty() ? at.y() - iconSize / 2.0 : at.y() - iconSize / 2.0 - 6;
            p.setOpacity(alpha);
            if (v.chip) {
                // A colour: a chip, ringed so that black and white show on the plum.
                const QPointF chipAt(at.x(), iconY + iconSize / 2.0);
                p.setPen(QPen(withAlpha(text, 0.75), 1.2));
                p.setBrush(QColor(v.chip->rgb()));
                p.drawEllipse(chipAt, 8.5, 8.5);
            } else {
                p.drawPixmap(QPointF(at.x() - iconSize / 2.0, iconY), icon(v.icon, QColor(tint.rgb()), iconSize));
            }
            p.setOpacity(1);
            if (!v.value.isEmpty()) {
                p.setFont(small);
                p.setPen(withAlpha(text, alpha));
                const QRectF valueBox(at.x() - satellite + 3, at.y() + 4, 2 * satellite - 6, 13);
                p.drawText(valueBox, Qt::AlignCenter, QFontMetrics(small).elidedText(v.value, Qt::ElideRight, int(valueBox.width())));
            }
            // The compass rule, spelt out: more towards F and R, less towards S and V.
            if (v.direction) {
                p.setFont(letter);
                p.setPen(withAlpha(line, alpha));
                const QPointF signAt = at + dir * (satellite + 1) + QPointF(-dir.y(), dir.x()) * (badge + 6);   // beside the key
                p.drawText(QRectF(signAt.x() - 6, signAt.y() - 7, 12, 14), Qt::AlignCenter, v.direction > 0 ? QStringLiteral("+") : QStringLiteral("−"));
            }
        }
        drawBadge(at + dir * (satellite + 1), v.key, v.empty ? 0.35 : alpha);
        // The key just pressed: a ring that swells and fades, muted when the command could not run.
        if (pulseKey_ == v.key) {
            p.setPen(QPen(withAlpha(pulseEnabled_ ? accent : text, (1 - pulse_) * (pulseEnabled_ ? 0.95 : 0.5)), 3));
            p.setBrush(withAlpha(pulseEnabled_ ? accent : fill, (1 - pulse_) * 0.35));
            p.drawEllipse(at, satellite + 6 * pulse_, satellite + 6 * pulse_);
        }
    }

    // The centre: the tool, its D.
    p.setPen(QPen(withAlpha(line, 0.9), 1.5));
    p.setBrush(withAlpha(base, 0.78));
    p.drawEllipse(centre, hub, hub);
    if (centreSwatches_) {
        // The toolbar's swatches: the background behind, down and right, the foreground over it.
        p.setPen(QPen(withAlpha(text, 0.8), 1.2));
        p.setBrush(QColor(centreSwatches_->second.rgb()));
        p.drawRoundedRect(QRectF(centre.x() - 4, centre.y() - 4, 17, 17), 3, 3);
        p.setBrush(QColor(centreSwatches_->first.rgb()));
        p.drawRoundedRect(QRectF(centre.x() - 13, centre.y() - 13, 17, 17), 3, 3);
    } else if (!centreIcon_.isEmpty()) {
        p.drawPixmap(QPointF(centre.x() - 11, centre.y() - 11), icon(centreIcon_, QColor(iconColor.rgb()), 22));
    }
    drawBadge(centre + QPointF(-hub * 0.72, -hub * 0.72), 'D', 1);
    if (pulseKey_ == 'D') {
        p.setPen(QPen(withAlpha(accent, (1 - pulse_) * 0.95), 3));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(centre, hub + 6 * pulse_, hub + 6 * pulse_);
    }

    // The caption under the compass: the tool's name, a command's, or why a greyed one cannot run.
    if (!caption_.isEmpty()) {
        QFont captionFont = font();
        captionFont.setPixelSize(11);
        p.setFont(captionFont);
        const QFontMetrics fm(captionFont);
        const int maxWidth = width() - 8;
        QRect box = fm.boundingRect(QRect(0, 0, maxWidth - 12, captionHeight * 2), Qt::TextWordWrap | Qt::AlignHCenter, caption_);
        box.setHeight(std::min(box.height(), captionHeight - 4));
        QRectF pill((width() - box.width()) / 2.0 - 6, compass.bottom() + 1, box.width() + 12, box.height() + 4);
        p.setPen(QPen(withAlpha(line, 0.6), 1));
        p.setBrush(withAlpha(fill, 0.88));
        p.drawRoundedRect(pill, 7, 7);
        p.setPen(text);
        p.drawText(pill.adjusted(6, 2, -6, -2), Qt::TextWordWrap | Qt::AlignCenter, caption_);
    }
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
        ringOverlay_->showCaption(why);
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
