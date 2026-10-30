// The Colour Ring (ColorRing.h): the foreground colour's hue, saturation and brightness on the compass keys, the
// default and swapped colours, and the recent ones.
#include "ColorRing.h"
#include "EditorSession.h"
#include "ColorManagement.h"
#include "CommandRegistry.h"
#include "MainWindow.h"
#include "ToolRing.h"
#include <QAction>
#include <QKeySequence>
#include <QMenu>
#include <QSettings>
#include <QStringList>
#include <algorithm>
#include <cmath>

namespace app {

namespace {

const QString recentKey = QStringLiteral("colorRing/recent");

/// A colour as QSettings keeps it: #rrggbb, or #rrrrggggbbbb when 8 bits would not say it.
QString colorName(const QColor& c) {
    if (QColor(c.name()) == c) return c.name();
    const QRgba64 deep = c.rgba64();
    return QStringLiteral("#%1%2%3").arg(deep.red(), 4, 16, QLatin1Char('0')).arg(deep.green(), 4, 16, QLatin1Char('0')).arg(deep.blue(), 4, 16, QLatin1Char('0'));
}

/// `now` moved onto the next multiple of `step` above or below it (a value between steps goes to the nearest one
/// in that direction first, as the Tool Ring's opacity does).
double stepOnto(double now, double step, bool more) {
    return more ? std::floor(now / step + 1e-9) * step + step : std::ceil(now / step - 1e-9) * step - step;
}

} // namespace

ColorRing::ColorRing(MainWindow* window) : RingController(window, new ToolRingOverlay(window)) {
    ringOverlay_ = static_cast<ToolRingOverlay*>(RingController::overlay_);
    ringOverlay_->setObjectName(QStringLiteral("colorRing"));
    for (const QString& name : QSettings().value(recentKey).toStringList()) {
        const QColor c(name);
        if (c.isValid() && recent_.size() < recentCount) recent_.append(c);
    }
}

void ColorRing::registerCommands(CommandRegistry& registry, QMenu* viewMenu) {
    registry_ = &registry;
    Command c;
    c.id = QStringLiteral("view.colorRing");
    c.label = tr("Colo&ur Ring");
    c.shortcuts = {QKeySequence(QStringLiteral("Alt+C"))};
    c.run = [this] { toggle(); };
    registry.add(viewMenu, std::move(c))->setObjectName(QStringLiteral("command.view.colorRing"));
}

QRect ColorRing::keepClear() const { return ringOverlay_->compassRect(); }

void ColorRing::connectSession(EditorSession& session) {
    // A colour the document was edited with is a recent one (a stroke, a fill), as Photoshop's recent swatches.
    follow(connect(&session, &EditorSession::historyChanged, this, [this] { if (session_ && !changing_) note(session_->foregroundColor); }));
}

ColorRing::Hsb ColorRing::current() const {
    const QColor fg = session_ ? session_->foregroundColor : QColor(Qt::black);
    if (hsbFor_.isValid() && fg == hsbFor_) return hsb_;
    const QColor hsv = fg.toHsv();
    Hsb now;
    // A grey has no hue (nor black a saturation): the ring keeps the hue it had, so more saturation brings it back.
    now.h = hsv.hsvHueF() < 0 ? hsb_.h : hsv.hsvHueF() * 360;
    now.s = hsv.hsvSaturationF() * 100;
    now.b = hsv.valueF() * 100;
    return now;
}

QColor ColorRing::stepped(const Hsb& now, char key, Hsb* after) {
    Hsb n = now;
    switch (key) {
    case 'F': case 'S': {
        n.h = stepOnto(n.h, hueStep, key == 'F');
        if (n.h >= 360 - 1e-9) n.h -= 360;
        if (n.h < 0) n.h += 360;
        break;
    }
    case 'E': case 'X': n.s = std::clamp(stepOnto(n.s, saturationStep, key == 'E'), 0.0, 100.0); break;
    case 'R': case 'V': n.b = std::clamp(stepOnto(n.b, brightnessStep, key == 'R'), 0.0, 100.0); break;
    default: break;
    }
    if (after) *after = n;
    // The colour picker's HSB to RGB, in whole 8-bit levels as its fields give.
    const QColor c = QColor::fromHsvF(float(n.h / 360), float(n.s / 100), float(n.b / 100));
    return QColor(c.red(), c.green(), c.blue());
}

std::optional<QColor> ColorRing::nextRecent() const {
    const QColor fg = session_ ? session_->foregroundColor : QColor();
    // From the one W recalled last (the colour it opened with, at first), the next that is not the colour now.
    for (int i = 0; i < recent_.size(); ++i) {
        const QColor& c = recent_[(recall_ + i) % recent_.size()];
        if (c != fg) return c;
    }
    return std::nullopt;
}

void ColorRing::note(const QColor& color) {
    if (!color.isValid()) return;
    recent_.removeAll(color);
    recent_.prepend(color);
    while (recent_.size() > recentCount) recent_.removeLast();
    recall_ = 0;
    QStringList names;
    for (const QColor& c : recent_) names.append(colorName(c));
    QSettings().setValue(recentKey, names);
}

QColor ColorRing::shown(const QColor& color) const {
    if (!session_) return color;
    if (session_->paintsMask()) {
        const double g = EditorSession::maskGrey(color);
        return QColor::fromRgbF(float(g), float(g), float(g));
    }
    return color::displayColor(color, session_->hasDocument() ? session_->document()->profile : nekophoto::ColorProfile{});
}

void ColorRing::setForeground(const QColor& color) {
    changing_ = true;
    window_->setColors(color, session_->backgroundColor, true, false);
    changing_ = false;
}

void ColorRing::opening() {
    if (!session_) return;
    note(session_->foregroundColor);
    wheelKey_ = 'F';
    const Hsb now = current();
    ringOverlay_->showCaption(tr("H %1°  S %2%  B %3%").arg(std::lround(now.h)).arg(std::lround(now.s)).arg(std::lround(now.b)));
}

void ColorRing::closed(bool) {
    // The colour it was left at is the newest recent one.
    if (session_) note(session_->foregroundColor);
}

void ColorRing::refresh() {
    if (!session_) return;
    const Hsb now = current();
    std::vector<ToolRingOverlay::View> views;
    auto value = [&](char key, const QString& text) {
        ToolRingOverlay::View v;
        v.key = key;
        v.value = text;
        v.chip = shown(stepped(now, key));
        v.direction = toolring::isMoreKey(key) ? 1 : -1;
        views.push_back(v);
    };
    const QString hue = tr("H %1°").arg(std::lround(now.h)), saturation = tr("S %1%").arg(std::lround(now.s)), brightness = tr("B %1%").arg(std::lround(now.b));
    {
        ToolRingOverlay::View w;
        w.key = 'W';
        w.icon = QStringLiteral("history");
        if (const auto recent = nextRecent()) w.chip = shown(*recent);
        else w.enabled = false;
        views.push_back(w);
    }
    value('E', saturation);
    value('R', brightness);
    value('S', hue);
    value('F', hue);
    value('X', saturation);
    {
        ToolRingOverlay::View c;
        c.key = 'C';
        c.icon = QStringLiteral("transfer");
        views.push_back(c);
    }
    value('V', brightness);
    ringOverlay_->setViews(views, {});
    ringOverlay_->setCentreSwatches(std::pair(shown(session_->foregroundColor), shown(session_->backgroundColor)));
}

void ColorRing::press(char key, bool autoRepeat) {
    if (!session_) return;
    restartIdle();
    const bool numeric = toolring::oppositeKey(key) != 0;
    if (autoRepeat && !numeric) return;   // a held key repeats values, not actions
    switch (key) {
    case 'D': {
        // Default colours; on a mask white paints and black is the background, as in Photoshop.
        changing_ = true;
        if (session_->paintsMask()) window_->setColors(Qt::white, Qt::black);
        else window_->setColors(Qt::black, Qt::white);
        changing_ = false;
        ringOverlay_->showCaption(tr("Default colours"));
        break;
    }
    case 'C':
        // The centre's swatches trade places rather than turning to each other's colour.
        ringOverlay_->swapSwatches();
        changing_ = true;
        window_->setColors(session_->backgroundColor, session_->foregroundColor);
        changing_ = false;
        ringOverlay_->showCaption(tr("Swap colours"));
        break;
    case 'W': {
        const auto recent = nextRecent();
        if (!recent) {
            ringOverlay_->pulse(key, false);
            ringOverlay_->showCaption(tr("No recent colours yet: they are kept as you paint with them"), true);
            return;
        }
        recall_ = int(recent_.indexOf(*recent));
        setForeground(*recent);
        ringOverlay_->showCaption(tr("Recent colour %1 of %2").arg(recall_ + 1).arg(recent_.size()));
        break;
    }
    default: {
        if (!numeric) return;
        Hsb after;
        setForeground(stepped(current(), key, &after));
        hsb_ = after;
        hsbFor_ = session_->foregroundColor;
        wheelKey_ = key;
        ringOverlay_->showCaption(tr("H %1°  S %2%  B %3%").arg(std::lround(after.h)).arg(std::lround(after.s)).arg(std::lround(after.b)));
        break;
    }
    }
    if (!isOpen()) return;
    ringOverlay_->pulse(key, true);
    refresh();
}

void ColorRing::wheelStep(bool more) {
    press(more == toolring::isMoreKey(wheelKey_) ? wheelKey_ : toolring::oppositeKey(wheelKey_), false);
}

} // namespace app
