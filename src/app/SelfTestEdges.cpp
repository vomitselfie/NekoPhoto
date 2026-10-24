// edges: Edit ▸ Preferences ▸ Edges and the edges' pages (FullScreen.h, EdgePages.h) through synthesised keys, wheel
// turns and pointer moves. An edge set to autohide leaves the window to the canvas (all four: the canvas fills it under
// the menus and the document tabs); reaching an edge slides its flyout out over the canvas, and away it slides back; Tab
// and Shift+Tab flip the edge that is out, and so does the wheel over its page strip, the new page in place at once
// while the flip draws; with no edge out a tap of Tab still hides the panels (on release), a hold brings every hidden
// edge out until it is let go; a drag begun on the canvas brings none out; a field and typing on the canvas keep Tab;
// the page an edge was on is remembered, the left edge opens on the brushes with the Brush, the right on Properties
// after an adjustment layer is added; full screen has the same pages. The slides are immediate, the waits generous.
#include "SelfTest.h"
#include "CanvasWidget.h"
#include "CommandRegistry.h"
#include "EdgePages.h"
#include "EditorSession.h"
#include "FullScreen.h"
#include "HidePanels.h"
#include "MainWindow.h"
#include "PreferencesDialog.h"
#include "ToolOptionsBar.h"
#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QApplication>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenuBar>
#include <QMouseEvent>
#include <QSettings>
#include <QStatusBar>
#include <QThread>
#include <QToolBar>
#include <QWheelEvent>
#include <cstdio>
#include <functional>

// QtGui's shortcut pass for a key press (SelfTest.cpp): true when a shortcut took the key.
Q_GUI_EXPORT bool qt_sendShortcutOverrideEvent(QObject* o, ulong timestamp, int k, Qt::KeyboardModifiers mods, const QString& text, bool autorep, ushort count);

using namespace nekophoto;

namespace app {

void buildDemoDocument(EditorSession& session);   // main.cpp

namespace {

void settle(int ms) {
    QElapsedTimer t;
    t.start();
    do {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    } while (t.elapsed() < ms);
}

bool until(const std::function<bool()>& done, int timeoutMs = 8000) {
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < timeoutMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    return done();
}

} // namespace

int edgePages(MainWindow& w) {
    using E = FullScreenMode;
    // Held open for the whole test: the settings stay in memory even where the file cannot be written.
    QSettings held;
    held.remove(QStringLiteral("edges"));
    EditorSession& s = *w.session();
    buildDemoDocument(s);
    w.resize(1300, 820);
    w.show();
    w.activateWindow();
    settle(100);
    FullScreenMode* fs = w.fullScreenMode();
    HidePanels* hide = w.hidePanels();
    CanvasWidget* canvas = w.canvasAt(w.currentTabIndex());
    if (!fs || !hide || !canvas) { std::fprintf(stderr, "edges: no controller or canvas\n"); return 1; }
    fs->setPolling(false);       // the pointer is what the test sends
    fs->setTimings(0, 0, 0);     // slides immediate, no wait once the pointer has left, none in a hot zone
    fs->setHoldDelay(150);
    PageDeck::setFlipTiming(0);
    s.selectTool(Tool::Hand);    // presses on the canvas change nothing
    canvas->setFocus();
    settle(50);
    int failures = 0;
    auto expect = [&](bool ok, const char* what) { if (!ok) { std::fprintf(stderr, "edges: %s\n", what); failures++; } };
    auto press = [](QWidget* target, int key, Qt::KeyboardModifiers modifiers, const QString& text = QString()) {
        if (qt_sendShortcutOverrideEvent(target, 0, key, modifiers, text, false, 1)) return true;
        QKeyEvent e(QEvent::KeyPress, key, modifiers, text);
        QApplication::sendEvent(target, &e);
        return false;
    };
    auto release = [](QWidget* target, int key, Qt::KeyboardModifiers modifiers) {
        QKeyEvent e(QEvent::KeyRelease, key, modifiers);
        QApplication::sendEvent(target, &e);
    };
    auto tap = [&](QWidget* target, int key, Qt::KeyboardModifiers modifiers) {
        press(target, key, modifiers);
        release(target, key, modifiers);
    };
    auto pointer = [&w](QEvent::Type type, QPoint at, Qt::MouseButtons buttons = Qt::NoButton, QWidget* grab = nullptr) {
        QWidget* target = grab ? grab : w.childAt(at);
        if (!target) target = &w;
        const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
        QMouseEvent e(type, target->mapFrom(&w, QPointF(at)), w.mapToGlobal(QPointF(at)), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(target, &e);
    };
    auto move = [&](QPoint at) { pointer(QEvent::MouseMove, at); };
    auto out = [fs](E::Edge e) { return fs->isOut(e) && fs->flyout(e)->isVisible(); };
    auto away = [fs](E::Edge e) { return !fs->isOut(e) && !fs->flyout(e)->isVisible(); };
    auto noneOut = [&] { return away(E::Left) && away(E::Top) && away(E::Right) && away(E::Bottom); };
    auto dock = [&w](const char* name) { return w.findChild<QDockWidget*>(QString::fromLatin1(name)); };
    QDockWidget* layersDock = dock("layersDock");
    QDockWidget* adjustDock = dock("adjustmentsDock");
    QDockWidget* histogramDock = dock("histogramDock");
    QDockWidget* timelineDock = dock("timelineDock");
    auto* rail = w.findChild<QToolBar*>(QStringLiteral("toolRail"));
    if (!layersDock || !adjustDock || !histogramDock || !timelineDock || !rail) { std::fprintf(stderr, "edges: the docks or the rail are missing\n"); return 1; }
    auto optionsInWindow = [&w, fs] {
        for (ToolOptionsBar* bar : w.findChildren<ToolOptionsBar*>())
            if (bar->isVisible() && !fs->flyout(E::Top)->isAncestorOf(bar)) return true;
        return false;
    };
    // The commands behind the keys: Tab and Shift+Tab, only while an edge is out.
    {
        const Command* next = w.commandRegistry().find(QStringLiteral("view.edgePage.next"));
        const Command* previous = w.commandRegistry().find(QStringLiteral("view.edgePage.previous"));
        expect(next && previous && next->action && previous->action, "no Next or Previous Edge Page command");
        if (next && previous && next->action && previous->action) {
            expect(next->action->shortcuts().value(0) == QKeySequence(Qt::Key_Tab) && previous->action->shortcuts().value(0) == QKeySequence(Qt::SHIFT | Qt::Key_Tab),
                   "the edge page commands do not have Tab and Shift+Tab");
            expect(!next->keyContext.isEmpty() && !w.actions().contains(next->action), "Next Edge Page's Tab is a window shortcut");
        }
    }

    // ---- The right edge autohides: its panels leave the window, the canvas takes the side.
    const int canvasWidth = canvas->width();
    expect(layersDock->isVisible() && !fs->claims(E::Right), "the window did not start with the panels on the right");
    fs->setAutohide(E::Right, true);
    settle(50);
    expect(fs->claims(E::Right) && !layersDock->isVisible() && !adjustDock->isVisible() && !histogramDock->isVisible(), "autohiding the right edge left a panel in the window");
    expect(canvas->width() > canvasWidth && fs->area().right() >= w.width() - 2, "the canvas did not take the right side");
    expect(fs->pages(E::Right) == QStringList({"layers", "properties", "channels", "history"}), "the right edge's pages are not Layers, Properties, Channels, History");
    // All four: the canvas fills the window under the menus and the document tabs.
    for (E::Edge e : {E::Left, E::Top, E::Bottom}) fs->setAutohide(e, true);
    settle(50);
    {
        const QRect a = fs->area();
        expect(a.left() == 0 && a.right() == w.width() - 1 && a.bottom() == w.height() - 1, "with every edge autohiding the canvas does not fill the window");
        expect(w.menuBar()->isVisible() && a.top() > w.menuBar()->geometry().bottom(), "the menus are not above the canvas");
        expect(!w.statusBar()->isVisible() && !optionsInWindow() && fs->flyout(E::Left)->isAncestorOf(rail), "the status bar, an options bar or the tools stayed in the window");
        expect(fs->pages(E::Left) == QStringList({"tools", "brushes", "colour"}) && fs->pages(E::Top) == QStringList({"options", "documents"}), "the left or top edge's pages are wrong");
        expect(fs->pages(E::Bottom) == QStringList({"info"}), "the bottom edge without frames does not hold only Info");
        expect(fs->pageHolding(histogramDock) == QLatin1String("info"), "the Histogram is not on the bottom edge's Info page");
    }
    const QRect a = fs->area();
    const QPoint middle = a.center();

    // ---- Reaching each edge slides its flyout out over the canvas; away, it slides back.
    struct EdgeCase { E::Edge edge; QPoint at; const char* name; };
    for (const EdgeCase& c : {EdgeCase{E::Left, QPoint(a.left(), middle.y()), "left"}, EdgeCase{E::Top, QPoint(middle.x(), a.top()), "top"},
                              EdgeCase{E::Right, QPoint(a.right(), middle.y()), "right"}, EdgeCase{E::Bottom, QPoint(middle.x(), a.bottom()), "bottom"}}) {
        move(middle);
        settle(20);
        move(c.at);
        if (!until([&] { return out(c.edge); })) { std::fprintf(stderr, "edges: the %s edge did not slide out\n", c.name); failures++; continue; }
        const QRect g = fs->flyout(c.edge)->geometry();
        expect(a.contains(g.center()), "a flyout is not over the canvas");
        expect(fs->strip(c.edge)->isVisible(), "a flyout has no page strip");
        move(g.center());
        settle(100);
        expect(out(c.edge), "a flyout slid back with the pointer in it");
        move(middle);
        if (!until([&] { return away(c.edge); })) { std::fprintf(stderr, "edges: the %s edge did not slide back\n", c.name); failures++; }
    }

    // ---- With the right edge out: Tab and Shift+Tab flip its pages, as does the wheel over its strip.
    canvas->setFocus();
    move(QPoint(a.right(), middle.y()));
    until([&] { return out(E::Right); });
    move(fs->flyout(E::Right)->geometry().center());
    expect(fs->page(E::Right) == QLatin1String("layers"), "the right edge did not open on Layers");
    expect(fs->flipTarget() == E::Right, "the right edge out is not the one Tab flips");
    tap(canvas, Qt::Key_Tab, Qt::NoModifier);
    expect(fs->page(E::Right) == QLatin1String("properties") && out(E::Right), "Tab did not flip the right edge to Properties");
    tap(canvas, Qt::Key_Tab, Qt::NoModifier);
    expect(fs->page(E::Right) == QLatin1String("channels"), "Tab again did not flip to Channels");
    tap(canvas, Qt::Key_Backtab, Qt::ShiftModifier);
    expect(fs->page(E::Right) == QLatin1String("properties"), "Shift+Tab did not flip back to Properties");
    expect(hide->hidden() == HidePanels::None, "Tab with an edge out hid the panels");
    expect(fs->strip(E::Right)->current() == 1 && fs->strip(E::Right)->labels().size() == 4, "the strip does not show Properties as the second of four");
    {
        PageStrip* strip = fs->strip(E::Right);
        const QPointF at = QPointF(strip->rect().center());
        QWheelEvent down(at, strip->mapToGlobal(at), QPoint(), QPoint(0, -120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(strip, &down);
        expect(fs->page(E::Right) == QLatin1String("channels"), "the wheel down over the strip did not flip to the next page");
        QWheelEvent up(at, strip->mapToGlobal(at), QPoint(), QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(strip, &up);
        expect(fs->page(E::Right) == QLatin1String("properties"), "the wheel up over the strip did not flip to the previous page");
    }
    // The flip draws over the new page, which is in place at once (it takes the pointer while the pictures move).
    PageDeck::setFlipTiming(400);
    tap(canvas, Qt::Key_Tab, Qt::NoModifier);
    expect(fs->deck(E::Right)->flipping() && fs->page(E::Right) == QLatin1String("channels"), "a flip did not animate");
    expect(w.findChild<QWidget*>(QStringLiteral("page_channels"))->isVisible(), "the new page was not in place during the flip");
    expect(until([&] { return !fs->deck(E::Right)->flipping(); }), "the flip did not end");
    PageDeck::setFlipTiming(0);
    // Remembered: the right edge comes back on Channels.
    expect(QSettings().value(QStringLiteral("edges/right/page")).toString() == QLatin1String("channels"), "the right edge's page was not kept in the settings");
    move(middle);
    until([&] { return away(E::Right); });
    fs->setAutohide(E::Right, false);
    fs->setAutohide(E::Right, true);
    settle(20);
    expect(fs->page(E::Right) == QLatin1String("channels"), "the right edge did not come back on the page it was on");

    // ---- Typing keeps Tab: in a field of the flyout, and on the canvas.
    move(QPoint(a.right(), middle.y()));
    until([&] { return out(E::Right); });
    fs->showPage(E::Right, QStringLiteral("layers"));
    move(fs->flyout(E::Right)->geometry().center());
    {
        QWidget* field = nullptr;
        for (QWidget* c : fs->flyout(E::Right)->findChildren<QWidget*>())
            if ((qobject_cast<QLineEdit*>(c) || qobject_cast<QAbstractSpinBox*>(c)) && c->isVisible() && c->isEnabled()) { field = c; break; }
        if (!field) expect(false, "no field in the right flyout");
        else {
            field->setFocus();
            settle(20);
            expect(!press(QApplication::focusWidget(), Qt::Key_Tab, Qt::NoModifier, QStringLiteral("\t")), "a shortcut took Tab from a field");
            expect(fs->page(E::Right) == QLatin1String("layers"), "Tab in a field flipped the edge");
        }
    }
    canvas->setFocus();
    move(middle);
    until(noneOut);
    s.selectTool(Tool::Text);
    if (canvas->startNewType(QPointF(100, 100), std::nullopt)) {
        canvas->setFocus();
        move(QPoint(a.right(), middle.y()));
        until([&] { return out(E::Right); });
        move(fs->flyout(E::Right)->geometry().center());
        expect(!press(canvas, Qt::Key_Tab, Qt::NoModifier, QStringLiteral("\t")) && fs->page(E::Right) == QLatin1String("layers"), "Tab while typing on the canvas flipped the edge");
        release(canvas, Qt::Key_Tab, Qt::NoModifier);
        press(canvas, Qt::Key_Escape, Qt::NoModifier);
        expect(!canvas->typeEditing(), "Esc did not cancel the typing");
        move(middle);
        until(noneOut);
    } else expect(false, "could not start typing");
    s.selectTool(Tool::Hand);
    canvas->setFocus();
    settle(20);

    // ---- Holding Tab brings every hidden edge out; letting go hides them, and hides no panel.
    expect(noneOut(), "an edge is out before the hold");
    press(canvas, Qt::Key_Tab, Qt::NoModifier);
    expect(until([&] { return fs->peeking(); }), "holding Tab did not peek");
    expect(out(E::Left) && out(E::Top) && out(E::Right) && out(E::Bottom), "holding Tab did not bring every edge out");
    // Its repeats change nothing.
    {
        QKeyEvent repeat(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier, QString(), true);
        QApplication::sendEvent(canvas, &repeat);
    }
    release(canvas, Qt::Key_Tab, Qt::NoModifier);
    expect(until(noneOut), "letting go of Tab did not hide the edges");
    expect(hide->hidden() == HidePanels::None, "the hold also hid the panels");

    // ---- Tab with no edge out still hides the panels (when let go), and again brings them back.
    for (E::Edge e : {E::Left, E::Top, E::Bottom}) fs->setAutohide(e, false);
    settle(50);
    canvas->setFocus();
    expect(rail->isVisible() && !fs->flyout(E::Left)->isAncestorOf(rail) && w.statusBar()->isVisible() && optionsInWindow(), "the tools, the status bar or the options bar did not come back to the window");
    press(canvas, Qt::Key_Tab, Qt::NoModifier);
    expect(hide->hidden() == HidePanels::None, "Tab hid the panels before it was let go");
    release(canvas, Qt::Key_Tab, Qt::NoModifier);
    settle(20);
    expect(hide->hidden() == HidePanels::All && !rail->isVisible() && !optionsInWindow(), "a tap of Tab did not hide the panels");
    expect(fs->claims(E::Right) && !layersDock->isVisible(), "hiding the panels gave the right edge's panels back to the window");
    tap(canvas, Qt::Key_Tab, Qt::NoModifier);
    settle(20);
    expect(hide->hidden() == HidePanels::None && rail->isVisible() && optionsInWindow() && !layersDock->isVisible() && fs->claims(E::Right), "Tab again did not bring the panels back, with the right edge still autohiding");

    // ---- A drag begun on the canvas brings no edge out, even across one.
    pointer(QEvent::MouseButtonPress, middle, Qt::LeftButton, canvas);
    for (QPoint p : {QPoint(middle.x() + 100, middle.y()), QPoint(a.right(), middle.y()), QPoint(a.right(), middle.y() + 10)}) pointer(QEvent::MouseMove, p, Qt::LeftButton, canvas);
    settle(100);
    expect(away(E::Right), "a drag from the canvas brought the right edge out");
    pointer(QEvent::MouseButtonRelease, QPoint(a.right(), middle.y() + 10), Qt::NoButton, canvas);
    settle(50);
    expect(away(E::Right), "letting go at the edge brought it out");
    move(middle);

    // ---- The left edge opens on the brushes with the Brush, on the tools otherwise; the tools' letters still work
    // with the rail in its hidden flyout.
    fs->setAutohide(E::Left, true);
    settle(20);
    canvas->setFocus();
    expect(press(canvas, Qt::Key_B, Qt::NoModifier, QStringLiteral("b")) && s.tool() == Tool::Brush, "B did not pick the Brush with the tools autohiding");
    settle(150);
    expect(fs->page(E::Left) == QLatin1String("brushes"), "with the Brush the left edge is not on its Brushes page");
    expect(press(canvas, Qt::Key_H, Qt::NoModifier, QStringLiteral("h")) && s.tool() == Tool::Hand, "H did not pick the Hand with the tools autohiding");
    settle(150);
    expect(fs->page(E::Left) == QLatin1String("tools"), "with the Hand the left edge is not on its Tools page");
    s.selectTool(Tool::Hand);
    // The right edge on Properties after an adjustment layer is added.
    fs->showPage(E::Right, QStringLiteral("layers"));
    s.addAdjustmentLayer(AdjustmentKind::Levels);
    settle(200);
    expect(fs->page(E::Right) == QLatin1String("properties"), "adding an adjustment layer did not put the right edge on Properties");
    canvas->setFocus();

    // ---- Full screen: the same pages and keys.
    fs->showPage(E::Right, QStringLiteral("layers"));
    expect(press(canvas, Qt::Key_F, Qt::NoModifier, QStringLiteral("f")) && fs->active(), "F did not enter full screen");
    settle(100);
    expect(fs->pages(E::Right) == QStringList({"layers", "properties", "channels", "history"}) && fs->pages(E::Left) == QStringList({"tools", "brushes", "colour"})
               && fs->pages(E::Top) == QStringList({"options", "documents"}),
           "full screen's edges do not have the same pages");
    expect(fs->page(E::Right) == QLatin1String("layers"), "full screen's right edge is not on the page the standard window's was");
    {
        const QRect full = fs->area();
        move(QPoint(full.left() + full.width() / 3, full.center().y()));
        move(QPoint(full.right(), full.center().y()));
        until([&] { return out(E::Right); });
        move(fs->flyout(E::Right)->geometry().center());
        tap(canvas, Qt::Key_Tab, Qt::NoModifier);
        expect(fs->page(E::Right) == QLatin1String("properties"), "Tab did not flip the right edge in full screen");
        // (Left of the middle: on a small screen the right column takes half of it.)
        move(QPoint(full.left() + full.width() / 3, full.center().y()));
        expect(until(noneOut), "the right edge did not slide back in full screen");
    }
    expect(press(canvas, Qt::Key_F, Qt::NoModifier, QStringLiteral("f")) && !fs->active(), "F did not leave full screen");
    settle(300);
    expect(fs->claims(E::Right) && fs->claims(E::Left) && !layersDock->isVisible() && !fs->claims(E::Top), "leaving full screen did not give back the autohiding edges as they were");

    // ---- Edit > Preferences > Edges: an edge's mode applies as it is chosen, and is kept.
    for (E::Edge e : {E::Left, E::Top, E::Right, E::Bottom}) fs->setAutohide(e, false);
    settle(20);
    {
        auto* dialog = new PreferencesDialog(&w);
        auto* right = dialog->findChild<QComboBox*>(QStringLiteral("edgeMode_right"));
        auto* motion = dialog->findChild<QCheckBox*>(QStringLiteral("edgesReduceMotion"));
        expect(right && motion && right->currentIndex() == 0, "Preferences has no Always shown choice for the right edge, or no Reduce motion");
        if (right) {
            right->setCurrentIndex(1);
            settle(20);
            expect(fs->claims(E::Right) && !layersDock->isVisible() && edges::autohide(E::Right), "choosing Autohide for the right edge in Preferences did not apply it");
            right->setCurrentIndex(0);
            settle(20);
            expect(!fs->claims(E::Right) && layersDock->isVisible() && !edges::autohide(E::Right), "choosing Always shown again did not give the panels back");
        }
        if (motion) {
            motion->setChecked(true);
            expect(edges::reduceMotion(), "Reduce motion was not kept");
            motion->setChecked(false);
        }
        delete dialog;
    }

    // ---- Back to every edge shown: the panels in the window as before.
    for (E::Edge e : {E::Left, E::Top, E::Right, E::Bottom}) fs->setAutohide(e, false);
    settle(50);
    expect(layersDock->isVisible() && adjustDock->isVisible() && rail->isVisible() && w.statusBar()->isVisible() && optionsInWindow() && !timelineDock->isVisible(), "the panels did not come back to the window");
    QSettings().remove(QStringLiteral("edges"));
    std::printf("edges: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}

} // namespace app
