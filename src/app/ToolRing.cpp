// The Tool Ring: settings, the overlay that paints it and the controller that drives it (ToolRing.h). The rings and
// their commands are in ToolRingCommands.cpp.
#include "ToolRing.h"
#include "CanvasWidget.h"
#include "CommandPalette.h"
#include "CommandRegistry.h"
#include "Icons.h"
#include "MainWindow.h"
#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QSettings>
#include <QTabletEvent>
#include <QTextEdit>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <QWindow>
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

/// The direction of each satellite from the centre, as the keys sit around D.
QPointF directionOf(char key) {
    constexpr double d = 0.70710678118654752;
    switch (key) {
    case 'W': return {-d, -d};
    case 'E': return {0, -1};
    case 'R': return {d, -d};
    case 'S': return {-1, 0};
    case 'F': return {1, 0};
    case 'X': return {-d, d};
    case 'C': return {0, 1};
    case 'V': return {d, d};
    default: return {0, 0};
    }
}

/// The ring's key for a key event: one of the nine letters with no modifier (the keypad's aside), else 0.
char ringKey(const QKeyEvent* e) {
    if (e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) return 0;
    switch (e->key()) {
    case Qt::Key_W: return 'W';
    case Qt::Key_E: return 'E';
    case Qt::Key_R: return 'R';
    case Qt::Key_S: return 'S';
    case Qt::Key_D: return 'D';
    case Qt::Key_F: return 'F';
    case Qt::Key_X: return 'X';
    case Qt::Key_C: return 'C';
    case Qt::Key_V: return 'V';
    default: return 0;
    }
}

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
    return (centre + directionOf(key) * orbit).toPoint();
}

void ToolRingOverlay::setViews(const std::vector<View>& views, const QString& centreIcon) {
    views_ = views;
    centreIcon_ = centreIcon;
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
        const QPointF dir = directionOf(v.key);
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
            p.drawPixmap(QPointF(at.x() - iconSize / 2.0, iconY), icon(v.icon, QColor(tint.rgb()), iconSize));
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
    if (!centreIcon_.isEmpty()) p.drawPixmap(QPointF(centre.x() - 11, centre.y() - 11), icon(centreIcon_, QColor(iconColor.rgb()), 22));
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

ToolRing::ToolRing(MainWindow* window) : QObject(window), window_(window) {
    overlay_ = new ToolRingOverlay(window);
    idle_.setSingleShot(true);
    connect(&idle_, &QTimer::timeout, this, &ToolRing::close);
}

std::pair<Tool, bool> ToolRing::toolKey() const {
    const EditorSession* s = window_->session_;
    if (!s) return {Tool::Move, false};
    return {s->tool(), s->tool() == Tool::Brush && s->brushErase};
}

bool ToolRing::typing() const {
    if (window_->canvas_ && window_->canvas_->typeEditing()) return true;
    // A text field has the keyboard (a layer's name, a filter box); a number field does not count, letters mean nothing there.
    QWidget* focus = QApplication::focusWidget();
    if (!focus) return false;
    if (focus->inherits("QTextEdit") || focus->inherits("QPlainTextEdit")) return true;
    return focus->inherits("QLineEdit") && !qobject_cast<QAbstractSpinBox*>(focus->parentWidget());
}

void ToolRing::sessionChanged() {
    close();
    for (const auto& c : sessionConnections_) disconnect(c);
    sessionConnections_.clear();
    session_ = window_->session_;
    if (!session_) return;
    presetNow_ = session_->brushPreset;
    presetBefore_.reset();
    sessionConnections_.push_back(connect(session_, &EditorSession::toolChanged, this, [this] {
        // The brush preset before this one, for Previous Brush Preset.
        if (session_->brushPreset != presetNow_) { presetBefore_ = presetNow_; presetNow_ = session_->brushPreset; }
        // The ring's own commands follow the tool (greyed where it has no such option).
        if (registry_)
            for (QAction* a : ownCommands_)
                if (const Command* c = registry_->forAction(a)) a->setEnabled(registry_->disabledReason(*c).isEmpty());
        if (!open_) return;
        if (toolKey() != openedFor_) {
            // Another tool closes the ring, unless the ring switched it (C: Brush and Eraser), which keeps it open.
            if (!ringChanging_) { close(); return; }
            openedFor_ = toolKey();
            ring_ = &toolring::ringFor(openedFor_.first);
        }
        refresh();
    }));
    for (auto signal : {&EditorSession::layersChanged, &EditorSession::selectionChanged, &EditorSession::historyChanged, &EditorSession::viewportChanged})
        sessionConnections_.push_back(connect(session_, signal, this, [this] { if (open_) refresh(); }));
    if (CanvasWidget* canvas = window_->canvas_)
        sessionConnections_.push_back(connect(canvas, &CanvasWidget::typeEditChanged, this, [this] { if (open_ && typing()) close(); }));
}

void ToolRing::toggle() {
    if (open_) close();
    else open();
}

void ToolRing::open() {
    if (open_ || !toolring::enabled() || !window_->canvas_ || !window_->session_ || typing()) return;
    CanvasWidget* canvas = window_->canvas_;
    if (!canvas->isVisible()) return;
    if (session_ != window_->session_) sessionChanged();
    openedFor_ = toolKey();
    ring_ = &toolring::ringFor(openedFor_.first);
    // The wheel steps the ring's first value (its S/F pair, else the first it has) until another is used.
    wheelKey_ = 0;
    for (char key : {'F', 'R', 'E'})
        for (const toolring::Slot& s : ring_->satellites)
            if (!wheelKey_ && s.key == key && isNumericValue(s.value)) wheelKey_ = key;
    wheelRemainder_ = 0;
    heldKeys_.clear();
    open_ = true;
    refresh();
    // By the pointer when it is over the canvas, else in the middle of it.
    const std::optional<QPointF> hover = canvas->hoverPoint();
    const QPoint pointer = hover ? canvas->mapTo(window_, hover->toPoint()) : canvasArea().center();
    lastPointer_ = pointer;
    heading_ = {};
    place(pointer, {});
    overlay_->show();
    overlay_->raise();
    if (const Command* c = window_->toolActions_.contains(openedFor_.first) ? registry_->forAction(openedFor_.second ? window_->eraserAction_ : window_->toolActions_.value(openedFor_.first)) : nullptr)
        overlay_->showCaption(plainText(c->label));
    qApp->installEventFilter(this);
    if (toolring::idleMs() > 0) idle_.start(toolring::idleMs());
}

void ToolRing::close() {
    if (!open_) return;
    open_ = false;
    qApp->removeEventFilter(this);
    idle_.stop();
    heldKeys_.clear();
    overlay_->hide();
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
    overlay_->setViews(views, toolIconName(openedFor_.first, openedFor_.second));
}

void ToolRing::press(char key, bool autoRepeat) {
    if (!ring_ || !registry_) return;
    if (toolring::idleMs() > 0) idle_.start(toolring::idleMs());
    const bool centre = key == 'D';
    const auto slot = std::find_if(ring_->satellites.begin(), ring_->satellites.end(), [key](const toolring::Slot& s) { return s.key == key; });
    const QString id = centre ? ring_->centre : slot == ring_->satellites.end() ? QString() : slot->command;
    const bool numeric = !centre && slot != ring_->satellites.end() && isNumericValue(slot->value);
    if (autoRepeat && !numeric) return;   // a held key repeats values, not actions
    const Command* c = id.isEmpty() ? nullptr : registry_->find(id);
    if (!c) {
        if (centre) {
            // D with nothing of its own: the tool's name.
            overlay_->pulse('D', true);
            QAction* toolAction = openedFor_.second ? window_->eraserAction_ : window_->toolActions_.value(openedFor_.first);
            if (const Command* t = toolAction ? registry_->forAction(toolAction) : nullptr) overlay_->showCaption(plainText(t->label));
        }
        return;
    }
    if (const QString why = registry_->disabledReason(*c); !why.isEmpty()) {
        overlay_->pulse(key, false);
        overlay_->showCaption(why);
        return;
    }
    ringChanging_ = true;
    registry_->execute(*c);
    ringChanging_ = false;
    if (!open_) return;   // the command closed it (a dialog took the keyboard)
    overlay_->pulse(key, true);
    if (numeric) wheelKey_ = key;
    else overlay_->showCaption(plainText(c->label));
    refresh();
}

QRect ToolRing::canvasArea() const {
    CanvasWidget* canvas = window_->canvas_;
    if (!canvas) return window_->rect();
    const QRect area(canvas->mapTo(window_, QPoint(0, 0)), canvas->size());
    return area.intersected(window_->rect()).adjusted(8, 8, -8, -8);
}

int ToolRing::clearance() const {
    const EditorSession* s = window_->session_;
    if (!s) return 0;
    double radius = 0;
    const Tool t = s->tool();
    if (t == Tool::Brush || t == Tool::SpotHealing || t == Tool::CloneStamp || t == Tool::Smudge || t == Tool::Dodge) radius = s->brushSettings.diameter / 2;
    else if (t == Tool::Scribble) radius = s->scribbleSize / 2.0;
    return int(std::clamp(radius * s->viewport.pointsPerPixel(), 0.0, 160.0));
}

void ToolRing::place(QPoint pointer, QPointF heading) {
    using toolring::Corner;
    const QRect area = canvasArea();
    const QSize size = overlay_->size();
    const QRect compass = overlay_->compassRect();
    // The compass's nearest edge this far from the pointer, beyond the brush's outline.
    const int gap = clearance() + 18;
    auto rectFor = [&](Corner c) {
        const bool right = c == Corner::UpRight || c == Corner::DownRight, up = c == Corner::UpRight || c == Corner::UpLeft;
        const int left = right ? pointer.x() + gap - compass.left() : pointer.x() - gap - compass.right() - 1;
        const int top = up ? pointer.y() - gap - size.height() : pointer.y() + gap - compass.top();
        return QRect(QPoint(left, top), size);
    };
    // Away from where the pointer is heading: moving right puts the ring on the left, moving down puts it above.
    std::vector<Corner> order;
    const Corner preferred = toolring::corner();
    bool right = preferred == Corner::UpRight || preferred == Corner::DownRight, up = preferred == Corner::UpRight || preferred == Corner::UpLeft;
    if (!heading.isNull() && overlay_->isVisible()) {
        // Along an axis the pointer is not moving on, the ring keeps its side.
        const QPoint at = overlay_->geometry().center();
        right = std::abs(heading.x()) > 1.5 ? heading.x() < 0 : at.x() > pointer.x();
        up = std::abs(heading.y()) > 1.5 ? heading.y() > 0 : at.y() < pointer.y();
    }
    auto cornerOf = [](bool r, bool u) { return u ? (r ? Corner::UpRight : Corner::UpLeft) : (r ? Corner::DownRight : Corner::DownLeft); };
    order = {cornerOf(right, up), cornerOf(right, !up), cornerOf(!right, up), cornerOf(!right, !up)};
    QRect chosen;
    for (Corner c : order) {
        if (area.contains(rectFor(c))) { chosen = rectFor(c); placed_ = c; break; }
    }
    if (chosen.isNull()) {
        // Nowhere it fits whole (a small canvas): the corner that keeps most of it inside, pushed in.
        int best = -1;
        for (Corner c : order) {
            const QRect r = rectFor(c).intersected(area);
            const int shown = r.isValid() ? r.width() * r.height() : 0;
            if (shown > best) { best = shown; chosen = rectFor(c); placed_ = c; }
        }
        chosen.moveLeft(std::clamp(chosen.left(), area.left(), std::max(area.left(), area.right() - size.width() + 1)));
        chosen.moveTop(std::clamp(chosen.top(), area.top(), std::max(area.top(), area.bottom() - size.height() + 1)));
    }
    overlay_->move(chosen.topLeft());
    overlay_->raise();
}

void ToolRing::pointerMoved(QPoint pointer) {
    heading_ = heading_ * 0.6 + QPointF(pointer - lastPointer_) * 0.4;
    lastPointer_ = pointer;
    // Hysteresis: the ring stays put until the pointer (with the brush's outline) would reach it.
    const int margin = clearance() + 6;
    QRect compass = overlay_->compassRect().translated(overlay_->pos());
    compass.setBottom(overlay_->geometry().bottom());   // the caption under it too
    const bool collides = compass.adjusted(-margin, -margin, margin, margin).contains(pointer);
    const bool outside = !canvasArea().contains(overlay_->geometry());
    if (collides || outside) place(pointer, heading_);
}

bool ToolRing::eventFilter(QObject* watched, QEvent* event) {
    if (!open_) return false;
    // Only what reaches this window; another window's keys are its own.
    auto ours = [this, watched] {
        if (auto* w = qobject_cast<QWidget*>(watched)) return w->window() == window_;
        return watched == static_cast<QObject*>(window_->windowHandle());
    };
    switch (event->type()) {
    case QEvent::ShortcutOverride: {
        // The ring's letters and Esc are not shortcuts while it is open (no tool switch, no colours, no screen mode).
        auto* ke = static_cast<QKeyEvent*>(event);
        if (!ours() || typing()) break;
        if (ringKey(ke) || (ke->key() == Qt::Key_Escape && ke->modifiers() == Qt::NoModifier)) { ke->accept(); return true; }
        break;
    }
    case QEvent::KeyPress: {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (!ours() || typing()) break;
        if (ke->key() == Qt::Key_Escape && ke->modifiers() == Qt::NoModifier) { close(); return true; }
        if (const char key = ringKey(ke)) {
            heldKeys_.insert(ke->key());
            press(key, ke->isAutoRepeat());
            return true;
        }
        break;
    }
    case QEvent::KeyRelease: {
        auto* ke = static_cast<QKeyEvent*>(event);
        if (heldKeys_.contains(ke->key())) {
            if (!ke->isAutoRepeat()) heldKeys_.remove(ke->key());
            return true;
        }
        break;
    }
    case QEvent::Wheel: {
        // Over the canvas, with no key held: the value used last, a notch a step (the pointer stays where it is).
        auto* we = static_cast<QWheelEvent*>(event);
        auto* w = qobject_cast<QWidget*>(watched);
        CanvasWidget* canvas = window_->canvas_;
        if (!wheelKey_ || !w || !canvas || (w != canvas && !canvas->isAncestorOf(w)) || we->modifiers() != Qt::NoModifier || we->buttons() != Qt::NoButton) break;
        const QPoint delta = we->angleDelta();
        wheelRemainder_ += delta.y() ? delta.y() / 120.0 : delta.x() / 120.0;
        if (we->angleDelta().isNull()) wheelRemainder_ += we->pixelDelta().y() / 40.0;
        while (std::abs(wheelRemainder_) >= 1) {
            const bool more = wheelRemainder_ > 0;
            wheelRemainder_ -= more ? 1 : -1;
            const char key = more == toolring::isMoreKey(wheelKey_) ? wheelKey_ : toolring::oppositeKey(wheelKey_);
            press(key, false);
            if (!open_) break;
        }
        return true;
    }
    case QEvent::MouseMove: {
        if (!ours()) break;
        pointerMoved(window_->mapFromGlobal(static_cast<QMouseEvent*>(event)->globalPosition().toPoint()));
        break;
    }
    case QEvent::TabletMove: {
        if (!ours()) break;
        pointerMoved(window_->mapFromGlobal(static_cast<QTabletEvent*>(event)->globalPosition().toPoint()));
        break;
    }
    case QEvent::WindowDeactivate:
        // A dialog, another window: the keyboard went elsewhere, and so does the ring.
        if (watched == window_) close();
        break;
    case QEvent::Resize:
        if (watched == window_->canvas_) place(lastPointer_, {});
        break;
    default:
        break;
    }
    return false;
}

} // namespace app
