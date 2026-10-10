// full-screen: View ▸ Screen Mode ▸ Full Screen (FullScreen.h) through synthesised keys and pointer moves. F enters it
// with nothing on screen but the canvas; each edge's hot zone slides its flyout out and the flyout slides back once
// the pointer has left it (after the injected delay); a button held on the canvas keeps the zones off; the Layers
// panel works from the right flyout; F10 and Alt open the menus over the canvas; Tab and Shift+Tab pin the edges and
// leave fields and typing alone; F7 slides out the right edge on the Layers tab; Esc slides the edges back, then
// leaves; leaving gives back saveState() byte for byte. No pixel sizes or wall-clock timing are assumed: the slides
// are immediate, the waits generous.
#include "SelfTest.h"
#include "CanvasWidget.h"
#include "EditorSession.h"
#include "FullScreen.h"
#include "LayersPanel.h"
#include "MainWindow.h"
#include "Theme.h"
#include <QAbstractSpinBox>
#include <QApplication>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QThread>
#include <QToolBar>
#include <QToolButton>
#include <cstdio>
#include <functional>

// QtGui's shortcut pass for a key press (SelfTest.cpp): true when a shortcut took the key.
Q_GUI_EXPORT bool qt_sendShortcutOverrideEvent(QObject* o, ulong timestamp, int k, Qt::KeyboardModifiers mods, const QString& text, bool autorep, ushort count);

using namespace compositor;

namespace app {

void buildDemoDocument(EditorSession& session);   // main.cpp

namespace {

void pump(int ms) {
    QElapsedTimer t;
    t.start();
    do {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    } while (t.elapsed() < ms);
}

bool waitFor(const std::function<bool()>& done, int timeoutMs = 8000) {
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < timeoutMs) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(2);
    }
    return done();
}

} // namespace

int fullScreen(MainWindow& w) {
    EditorSession& s = *w.session();
    buildDemoDocument(s);
    w.resize(1400, 900);
    w.show();
    w.activateWindow();
    pump(100);
    s.fitView();
    FullScreenMode* fs = w.fullScreenMode();
    CanvasWidget* canvas = w.canvasAt(w.currentTabIndex());
    if (!fs || !canvas) { std::fprintf(stderr, "full screen: no controller or canvas\n"); return 1; }
    fs->setPolling(false);    // the pointer is what the test sends, not wherever the platform's cursor is
    fs->setTimings(0, 0);     // slides immediate, no wait once the pointer has left
    s.selectTool(Tool::Hand); // presses on the canvas change nothing
    canvas->setFocus();
    pump(50);
    int failures = 0;
    auto expect = [&](bool ok, const char* what) { if (!ok) { std::fprintf(stderr, "full screen: %s\n", what); failures++; } };
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
    // A pointer event at a window point, to the widget there (or to `grab`, which holds the pointer during a drag).
    auto pointer = [&w](QEvent::Type type, QPoint at, Qt::MouseButtons buttons = Qt::NoButton, QWidget* grab = nullptr) {
        QWidget* target = grab ? grab : w.childAt(at);
        if (!target) target = &w;
        const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
        QMouseEvent e(type, target->mapFrom(&w, QPointF(at)), w.mapToGlobal(QPointF(at)), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(target, &e);
    };
    auto move = [&](QPoint at) { pointer(QEvent::MouseMove, at); };
    auto out = [fs](FullScreenMode::Edge e) { return fs->isOut(e) && fs->flyout(e)->isVisible(); };
    auto away = [fs](FullScreenMode::Edge e) { return !fs->isOut(e) && !fs->flyout(e)->isVisible(); };
    auto noneOut = [&] { return away(FullScreenMode::Left) && away(FullScreenMode::Top) && away(FullScreenMode::Right) && away(FullScreenMode::Bottom); };
    auto* layersDock = w.findChild<QDockWidget*>(QStringLiteral("layersDock"));
    auto* channelsDock = w.findChild<QDockWidget*>(QStringLiteral("channelsDock"));
    auto* timelineDock = w.findChild<QDockWidget*>(QStringLiteral("timelineDock"));
    if (!layersDock || !channelsDock || !timelineDock) { std::fprintf(stderr, "full screen: the docks are missing\n"); return 1; }

    const QByteArray before = w.saveState();
    expect(!fs->active() && w.menuBar()->isVisible() && layersDock->isVisible(), "the window did not start in the standard screen mode");

    // F: full screen, nothing on screen but the canvas.
    expect(press(canvas, Qt::Key_F, Qt::NoModifier, QStringLiteral("f")), "no shortcut took F");
    expect(fs->active(), "F did not enter full screen");
    pump(100);
    expect(w.isFullScreen(), "the window is not full screen");
    expect(!w.menuBar()->isVisible() && !w.statusBar()->isVisible(), "the menu bar or the status bar is on screen");
    for (QDockWidget* d : w.findChildren<QDockWidget*>())
        if (d->isVisible()) { std::fprintf(stderr, "full screen: the %s dock is on screen\n", qPrintable(d->objectName())); failures++; }
    for (QToolBar* t : w.findChildren<QToolBar*>())
        if (t->isVisible()) { std::fprintf(stderr, "full screen: the %s toolbar is on screen\n", qPrintable(t->objectName())); failures++; }
    // (Qt parks the dock tab bars it no longer uses outside the window: those are not on screen.)
    for (QWidget* c : w.findChildren<QWidget*>(Qt::FindDirectChildrenOnly))
        if (c->isVisible() && c != w.centralWidget() && c->geometry().intersects(w.rect())) { std::fprintf(stderr, "full screen: %s %s is on screen\n", c->metaObject()->className(), qPrintable(c->objectName())); failures++; }
    for (QTabBar* bar : w.centralWidget()->findChildren<QTabBar*>())
        if (bar->isVisible()) { std::fprintf(stderr, "full screen: the document tabs are on screen\n"); failures++; }
    expect(canvas->isVisible() && canvas->width() > w.width() / 2 && canvas->height() > w.height() / 2, "the canvas does not fill the screen");
    if (themeSetting() == QLatin1String("gothkitty"))
        expect(fs->flyout(FullScreenMode::Right)->cornerRadius() > 0 && fs->flyout(FullScreenMode::Right)->shadow() > 0, "Goth Kitty's flyouts have no rounded corners or shadow");

    // Each edge: into its hot zone the flyout slides out, over the canvas (the canvas keeps its size); the pointer in
    // it keeps it out; away, it slides back.
    // `middle` lies between where the side columns slide to, below the top bar: on the canvas whatever is out.
    const int W = w.width(), H = w.height();
    const QPoint middle((fs->shownGeometry(FullScreenMode::Left).right() + fs->shownGeometry(FullScreenMode::Right).left()) / 2, H / 2);
    expect(middle.x() > fs->shownGeometry(FullScreenMode::Left).right() && middle.x() < fs->shownGeometry(FullScreenMode::Right).left(), "the side columns cover the whole screen");
    const QSize canvasSize = canvas->size();
    struct EdgeCase { FullScreenMode::Edge edge; QPoint at; const char* name; };
    for (const EdgeCase& c : {EdgeCase{FullScreenMode::Left, QPoint(0, H / 2), "left"}, EdgeCase{FullScreenMode::Top, QPoint(W / 2, 0), "top"},
                              EdgeCase{FullScreenMode::Right, QPoint(W - 1, H / 2), "right"}}) {
        move(middle);
        pump(20);
        move(c.at);
        if (!waitFor([&] { return out(c.edge); })) { std::fprintf(stderr, "full screen: the %s edge did not slide out\n", c.name); failures++; continue; }
        for (auto other : {FullScreenMode::Left, FullScreenMode::Top, FullScreenMode::Right, FullScreenMode::Bottom})
            if (other != c.edge && !away(other)) { std::fprintf(stderr, "full screen: the %s edge brought another out\n", c.name); failures++; }
        const QRect g = fs->flyout(c.edge)->geometry();
        expect(w.rect().contains(g.center()) && (c.edge != FullScreenMode::Left || g.left() == 0) && (c.edge != FullScreenMode::Top || g.top() == 0)
               && (c.edge != FullScreenMode::Right || g.right() == W - 1), "a flyout is not flush with its edge");
        expect(canvas->size() == canvasSize, "a flyout resized the canvas instead of lying over it");
        move(g.center());
        pump(150);
        expect(out(c.edge), "a flyout slid back with the pointer in it");
        move(middle);
        if (!waitFor([&] { return away(c.edge); })) { std::fprintf(stderr, "full screen: the %s edge did not slide back\n", c.name); failures++; }
    }
    // The left, right and top flyouts hold the tool rail, the panels and the options bar.
    expect(fs->flyout(FullScreenMode::Left)->findChild<QToolBar*>(QStringLiteral("toolRail")), "the left flyout has no tool rail");
    expect(fs->flyout(FullScreenMode::Top)->findChild<QToolBar*>(), "the top flyout has no options bar");
    expect(fs->edgeHolding(layersDock) == FullScreenMode::Right && fs->edgeHolding(channelsDock) == FullScreenMode::Right, "Layers and Channels are not in the right flyout");
    expect(fs->tabsHolding(layersDock) && fs->tabsHolding(layersDock) == fs->tabsHolding(channelsDock), "Layers and Channels are not tabs of one group");
    // The delay before a flyout slides back is the injected one: with a long one it is still out right after leaving.
    fs->setTimings(0, 60000);
    move(QPoint(0, H / 2));
    waitFor([&] { return out(FullScreenMode::Left); });
    move(middle);
    pump(150);
    expect(out(FullScreenMode::Left), "the left flyout slid back before its delay");
    fs->setTimings(0, 0);
    move(QPoint(middle.x() + 1, middle.y()));
    expect(waitFor([&] { return away(FullScreenMode::Left); }), "the left flyout did not slide back after its delay");
    // The bottom edge has nothing while the Timeline is closed.
    move(QPoint(W / 2, H - 1));
    pump(150);
    expect(away(FullScreenMode::Bottom), "the bottom edge slid out with the Timeline closed");
    move(middle);

    // A button held on the canvas (a stroke running to the edge) keeps every hot zone off, until it is let go and the
    // pointer comes back to the edge.
    pointer(QEvent::MouseButtonPress, middle, Qt::LeftButton, canvas);
    for (QPoint p : {QPoint(W / 4, H / 2), QPoint(0, H / 2), QPoint(W / 2, 0), QPoint(W - 1, H / 3)}) pointer(QEvent::MouseMove, p, Qt::LeftButton, canvas);
    pump(150);
    expect(noneOut(), "an edge slid out while a button was held on the canvas");
    pointer(QEvent::MouseButtonRelease, QPoint(W - 1, H / 3), Qt::NoButton, canvas);
    pump(100);
    expect(away(FullScreenMode::Right), "letting go at the edge slid it out");
    move(middle);
    move(QPoint(W - 1, H / 3));
    expect(waitFor([&] { return out(FullScreenMode::Right); }), "after the stroke the right edge did not slide out");

    // The Layers panel works in the right flyout: an eye clicked there hides the layer, as one undo step.
    {
        LayersPanel* layers = w.layersPanelAt(w.currentTabIndex());
        expect(layers && fs->flyout(FullScreenMode::Right)->isAncestorOf(layers) && layers->isVisible(), "the Layers panel is not on screen in the right flyout");
        const Layer* paint = nullptr;
        for (const Layer& l : s.document()->layers) if (l.name == "Paint") paint = &l;
        QToolButton* eye = nullptr;
        if (paint && layers)
            for (QToolButton* b : layers->findChildren<QToolButton*>())
                if (b->property("eye").toBool() && b->property("layerId").toString() == QString::fromStdString(paint->id) && b->isVisible()) eye = b;
        if (!paint || !eye) { std::fprintf(stderr, "full screen: no eye for the Paint layer in the flyout\n"); failures++; }
        else {
            const Uuid id = paint->id;
            const size_t steps = s.undoNames().size();
            const QPoint at = eye->mapTo(&w, eye->rect().center());
            move(at);
            pointer(QEvent::MouseButtonPress, at, Qt::LeftButton, eye);
            pointer(QEvent::MouseButtonRelease, at, Qt::NoButton, eye);
            pump(50);
            const Layer* after = s.document()->find(id);
            expect(after && !after->visible && s.undoNames().size() == steps + 1, "the eye in the right flyout did not hide the layer as one step");
            expect(out(FullScreenMode::Right), "the right flyout slid back under the pointer");
        }
        move(middle);
        expect(waitFor([&] { return away(FullScreenMode::Right); }), "the right flyout did not slide back after the eye");
    }

    // Menu commands keep their keys without the menu bar: Ctrl+0 (Fit on Screen) answers.
    canvas->setFocus();
    expect(press(canvas, Qt::Key_0, Qt::ControlModifier, QStringLiteral("0")), "Ctrl+0 did nothing in full screen");
    // So do the tool rail's letters, with the rail out of sight in its flyout: E picks the Eraser, B the Brush.
    expect(press(canvas, Qt::Key_E, Qt::NoModifier, QStringLiteral("e")) && s.tool() == Tool::Brush && s.brushErase, "E did not pick the Eraser in full screen");
    expect(press(canvas, Qt::Key_B, Qt::NoModifier, QStringLiteral("b")) && s.tool() == Tool::Brush && !s.brushErase, "B did not pick the Brush in full screen");
    s.selectTool(Tool::Hand);

    // F10, and Alt alone: the menu bar over the canvas with a menu open; it goes when the menu closes.
    canvas->setFocus();
    for (int way = 0; way < 2; way++) {
        if (way == 0) expect(press(canvas, Qt::Key_F10, Qt::NoModifier), "no shortcut took F10");
        else {
            press(canvas, Qt::Key_Alt, Qt::AltModifier);
            release(canvas, Qt::Key_Alt, Qt::NoModifier);
        }
        const bool opened = waitFor([&] { return fs->menuStrip() && fs->menuStrip()->isVisible() && qobject_cast<QMenu*>(QApplication::activePopupWidget()); });
        expect(opened, way == 0 ? "F10 did not open a menu over the canvas" : "Alt did not open a menu over the canvas");
        expect(!w.menuBar()->isVisible(), "the window's own menu bar came back");
        if (QWidget* popup = QApplication::activePopupWidget()) popup->close();
        expect(waitFor([&] { return !QApplication::activePopupWidget() && fs->menuStrip() && !fs->menuStrip()->isVisible(); }), "the menu bar stayed after the menu closed");
        w.activateWindow();
        waitFor([&] { return QApplication::activeWindow() == &w; });
        canvas->setFocus();
    }
    canvas->setFocus();
    // The offscreen platform leaves no window active once a popup closes (a desktop gives the window back).
    w.activateWindow();
    waitFor([&] { return QApplication::activeWindow() == &w; });
    canvas->setFocus();
    expect(press(canvas, Qt::Key_0, Qt::ControlModifier, QStringLiteral("0")), "Ctrl+0 did nothing after the menus");

    // Tab pins every edge out (the bottom has nothing), Tab again hides them; Shift+Tab all but the tools.
    expect(press(canvas, Qt::Key_Tab, Qt::NoModifier), "no shortcut took Tab");
    move(middle);
    pump(150);
    expect(out(FullScreenMode::Left) && out(FullScreenMode::Top) && out(FullScreenMode::Right) && away(FullScreenMode::Bottom), "Tab did not pin the edges out");
    expect(fs->isPinned(FullScreenMode::Left) && fs->isPinned(FullScreenMode::Right), "Tab did not pin the edges");
    // Tab and F in a field stay with the field.
    {
        QWidget* field = nullptr;
        for (QWidget* c : fs->flyout(FullScreenMode::Right)->findChildren<QWidget*>())
            if ((qobject_cast<QLineEdit*>(c) || qobject_cast<QAbstractSpinBox*>(c)) && c->isVisible() && c->isEnabled()) { field = c; break; }
        if (!field) { std::fprintf(stderr, "full screen: no field in the right flyout\n"); failures++; }
        else {
            field->setFocus();
            pump(20);
            expect(!press(QApplication::focusWidget(), Qt::Key_Tab, Qt::NoModifier), "a shortcut took Tab from a field");
            expect(fs->active() && fs->isPinned(FullScreenMode::Left), "Tab in a field changed the edges");
            if (QWidget* f = QApplication::focusWidget(); f && (f->inherits("QLineEdit") || f->inherits("QAbstractSpinBox")))
                expect(!press(f, Qt::Key_F, Qt::NoModifier, QStringLiteral("f")) && fs->active(), "F in a field left full screen");
        }
        canvas->setFocus();
        pump(20);
    }
    expect(press(canvas, Qt::Key_Tab, Qt::NoModifier), "no shortcut took Tab again");
    expect(waitFor(noneOut), "Tab again did not hide the edges");
    expect(press(canvas, Qt::Key_Backtab, Qt::ShiftModifier), "no shortcut took Shift+Tab");
    pump(100);
    expect(away(FullScreenMode::Left) && out(FullScreenMode::Top) && out(FullScreenMode::Right), "Shift+Tab did not pin all but the tools");
    press(canvas, Qt::Key_Backtab, Qt::ShiftModifier);
    expect(waitFor(noneOut), "Shift+Tab again did not hide the edges");

    // Typing on the canvas keeps F and Tab; Esc cancels the typing, not full screen.
    s.selectTool(Tool::Text);
    if (canvas->startNewType(QPointF(100, 100), std::nullopt)) {
        canvas->setFocus();
        expect(!press(canvas, Qt::Key_F, Qt::NoModifier, QStringLiteral("f")) && fs->active(), "F while typing on the canvas left full screen");
        expect(!press(canvas, Qt::Key_Tab, Qt::NoModifier, QStringLiteral("\t")) && noneOut(), "Tab while typing on the canvas pinned the edges");
        press(canvas, Qt::Key_Escape, Qt::NoModifier);
        expect(!canvas->typeEditing() && fs->active(), "Esc while typing did not cancel the typing alone");
    } else expect(false, "could not start typing");
    s.selectTool(Tool::Hand);

    // F7: the right edge, on the Layers tab (from Channels); F7 again slides it back.
    if (QTabWidget* tabs = fs->tabsHolding(channelsDock)) {
        for (int i = 0; i < tabs->count(); i++) if (tabs->tabText(i) == channelsDock->windowTitle()) tabs->setCurrentIndex(i);
        expect(tabs->tabText(tabs->currentIndex()) == channelsDock->windowTitle(), "could not bring Channels to the front");
        canvas->setFocus();
        expect(press(canvas, Qt::Key_F7, Qt::NoModifier), "no shortcut took F7");
        pump(100);
        expect(out(FullScreenMode::Right), "F7 did not slide out the right edge");
        expect(tabs->currentWidget() && tabs->currentWidget()->isAncestorOf(w.layersPanelAt(w.currentTabIndex())), "F7 did not bring Layers to the front");
        expect(!layersDock->isVisible(), "F7 showed the Layers dock in the window");
        move(middle);
        pump(150);
        expect(out(FullScreenMode::Right), "the edge F7 brought out slid back before the pointer visited it");
        press(canvas, Qt::Key_F7, Qt::NoModifier);
        expect(waitFor([&] { return away(FullScreenMode::Right); }), "F7 again did not slide the right edge back");
    } else expect(false, "Channels is in no tab group");

    // Window ▸ Timeline: the bottom edge, with the Timeline.
    if (const Command* timeline = w.commandRegistry().find(QStringLiteral("window.timeline")); timeline && timeline->action) {
        timeline->action->trigger();
        pump(100);
        expect(out(FullScreenMode::Bottom) && fs->edgeHolding(timelineDock) == FullScreenMode::Bottom, "Window > Timeline did not slide out the bottom edge");
        expect(!timelineDock->isVisible(), "Window > Timeline showed its dock in the window");
    } else expect(false, "no Window > Timeline command");

    // Window ▸ Actions, closed when full screen began: it joins the right column, in front.
    if (auto* actionsDock = w.findChild<QDockWidget*>(QStringLiteral("actionsDock")); actionsDock && actionsDock->isHidden()) {
        if (const Command* actions = w.commandRegistry().find(QStringLiteral("window.actions")); actions && actions->action) actions->action->trigger();
        pump(50);
        QTabWidget* tabs = fs->tabsHolding(actionsDock);
        expect(out(FullScreenMode::Right) && fs->edgeHolding(actionsDock) == FullScreenMode::Right && tabs && tabs->tabText(tabs->currentIndex()) == actionsDock->windowTitle(),
               "Window > Actions did not bring the Actions panel out on the right");
        expect(!actionsDock->isVisible(), "Window > Actions showed its dock in the window");
    } else expect(false, "no closed Actions dock");

    // A tab opened in full screen: its options bar goes in the top flyout, nothing appears in the window.
    {
        fs->closeFlyouts();
        const int before = w.currentTabIndex();
        const int added = w.newTab();
        pump(50);
        bool shown = false;
        for (QToolBar* t : w.findChildren<QToolBar*>()) if (t->isVisible() && !fs->flyout(FullScreenMode::Top)->isAncestorOf(t) && !fs->flyout(FullScreenMode::Left)->isAncestorOf(t)) shown = true;
        expect(!shown, "a new tab's options bar showed in the window");
        move(middle);
        move(QPoint(middle.x(), 0));
        expect(waitFor([&] { return out(FullScreenMode::Top); }), "the top edge did not slide out on the new tab");
        const QList<QToolBar*> bars = fs->flyout(FullScreenMode::Top)->findChildren<QToolBar*>();
        int visibleBars = 0;
        for (QToolBar* t : bars) if (t->isVisible()) visibleBars++;
        expect(visibleBars == 1, "the top flyout does not hold exactly the new tab's options bar");
        move(middle);
        w.closeTabAt(added);
        w.selectTab(std::min(before, w.tabCount() - 1));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);   // the closed tab's bar, as the event loop would
        pump(50);
        canvas = w.canvasAt(w.currentTabIndex());
    }

    // Esc: first the edges slide back (the right one, brought out by F7), then full screen ends.
    canvas->setFocus();
    press(canvas, Qt::Key_F7, Qt::NoModifier);
    expect(waitFor([&] { return out(FullScreenMode::Right); }), "F7 did not bring the right edge out before Esc");
    press(canvas, Qt::Key_Escape, Qt::NoModifier);
    pump(50);
    expect(fs->active() && noneOut(), "Esc did not slide the edges back first");
    press(canvas, Qt::Key_Escape, Qt::NoModifier);
    expect(waitFor([&] { return !fs->active() && !w.isFullScreen(); }), "Esc did not leave full screen");
    pump(300);
    expect(w.menuBar()->isVisible() && w.statusBar()->isVisible() && layersDock->isVisible() && !timelineDock->isVisible(), "leaving did not bring the window's bars and panels back");
    expect(layersDock->widget() && channelsDock->widget() && !channelsDock->widget()->isHidden(), "a panel did not go back to its dock");
    expect(w.saveState() == before, "leaving full screen did not restore saveState() byte for byte");
    // View ▸ Screen Mode and F again: in and out, the same state.
    if (const Command* full = w.commandRegistry().find(QStringLiteral("view.screenMode.fullScreen")); full && full->action) full->action->trigger();
    pump(50);
    expect(fs->active(), "View > Screen Mode > Full Screen did not enter full screen");
    press(canvas, Qt::Key_F, Qt::NoModifier, QStringLiteral("f"));
    expect(waitFor([&] { return !fs->active(); }), "F did not leave full screen");
    pump(300);
    expect(w.saveState() == before, "a second round did not restore saveState() byte for byte");
    std::printf("full screen: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}

} // namespace app
