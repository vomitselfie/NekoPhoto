// tool-ring: View > Tool Ring (Alt+D), through synthesised key, mouse and wheel events as held-keys types them: Alt
// then D opens it without leaving the held Eyedropper or the menu bar behind, it sits clear of the pointer inside the
// canvas and moves away (once) when the pointer comes at it, F and S step the brush size by Photoshop's [ ] steps,
// a key pressed while a stroke is painted leaves the stroke whole, its letters do not switch tools or run their
// shortcuts while it is open and do again once it is closed, Esc, Alt+D, another tool and the idle timeout close it,
// the mouse goes through it to the canvas, and the wheel steps the value used last.
#include "SelfTestSupport.h"
#include "CanvasWidget.h"
#include "CommandRegistry.h"
#include "EditorSession.h"
#include "FullScreen.h"
#include "MainWindow.h"
#include "ToolRing.h"
#include <QAction>
#include <QApplication>
#include <QEventLoop>
#include <QKeyEvent>
#include <QMenuBar>
#include <QMouseEvent>
#include <QSettings>
#include <QTimer>
#include <QWheelEvent>
#include <cmath>
#include <cstdio>

// QtGui's shortcut pass for a key press (SelfTestKeys.cpp has the same).
Q_GUI_EXPORT bool qt_sendShortcutOverrideEvent(QObject* o, ulong timestamp, int k, Qt::KeyboardModifiers mods, const QString& text, bool autorep, ushort count);

using namespace nekophoto;

namespace app {

using namespace selftest;

int toolRing(MainWindow& w) {
    EditorSession& s = *w.session();
    buildDemoDocument(s);
    w.resize(1400, 900);
    w.show();
    w.activateWindow();
    QApplication::processEvents();
    s.fitView();
    QApplication::processEvents();
    CanvasWidget* canvas = w.canvasAt(w.currentTabIndex());
    ToolRing* ring = w.toolRing();
    if (!canvas || !ring) { std::fprintf(stderr, "tool ring: no canvas or no ring\n"); return 1; }
    canvas->setFocus();
    QApplication::processEvents();
    QSettings settings;
    settings.setValue(QStringLiteral("toolRing/enabled"), true);
    settings.setValue(QStringLiteral("toolRing/idleMs"), 0);   // no timeout until the timeout's own check
    settings.setValue(QStringLiteral("toolRing/corner"), 0);
    int failures = 0;
    auto expect = [&](bool ok, const char* what) { if (!ok) { std::fprintf(stderr, "tool ring: %s\n", what); failures++; } };

    // A key as a person types it (the shortcuts first, then the widget with the keyboard); true when a shortcut took it.
    auto press = [](QWidget* target, int key, Qt::KeyboardModifiers modifiers, const QString& text = QString(), bool autoRepeat = false) {
        if (qt_sendShortcutOverrideEvent(target, 0, key, modifiers, text, autoRepeat, 1)) return true;
        QKeyEvent e(QEvent::KeyPress, key, modifiers, text, autoRepeat);
        QApplication::sendEvent(target, &e);
        return false;
    };
    auto release = [](QWidget* target, int key, Qt::KeyboardModifiers modifiers) {
        QKeyEvent e(QEvent::KeyRelease, key, modifiers);
        QApplication::sendEvent(target, &e);
    };
    auto tap = [&](int key, const char* text) { const bool taken = press(canvas, key, Qt::NoModifier, QString::fromLatin1(text)); release(canvas, key, Qt::NoModifier); return taken; };
    auto altD = [&] {
        press(canvas, Qt::Key_Alt, Qt::AltModifier);
        const bool taken = press(canvas, Qt::Key_D, Qt::AltModifier, QStringLiteral("d"));
        release(canvas, Qt::Key_D, Qt::AltModifier);
        release(canvas, Qt::Key_Alt, Qt::NoModifier);
        return taken;
    };
    // The pointer over the canvas at a view point, its buttons as given.
    auto mouse = [canvas](QEvent::Type type, QPointF view, Qt::MouseButtons buttons = Qt::NoButton) {
        const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
        QMouseEvent e(type, view, canvas->mapToGlobal(view), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(canvas, &e);
    };
    auto wheel = [canvas](QPointF view, int notches) {
        QWheelEvent e(view, canvas->mapToGlobal(view), QPoint(), QPoint(0, 120 * notches), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(canvas, &e);
    };
    auto wait = [](int ms) { QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec(); };
    auto inWindow = [&](QPointF view) { return canvas->mapTo(&w, view.toPoint()); };
    // A point of the document, as fractions of its size, on the canvas.
    auto docView = [&](double fx, double fy) { return canvas->viewPointForTest(QPointF(s.document()->width * fx, s.document()->height * fy)); };
    ToolRingOverlay* overlay = ring->overlay();
    // The ring keeps clear of the pointer and stays inside the canvas (and so the window).
    auto placedWell = [&](QPointF view, const char* when) {
        const QRect area(canvas->mapTo(&w, QPoint(0, 0)), canvas->size());
        const QRect at = overlay->geometry();
        if (at.contains(inWindow(view)) || !area.contains(at) || !w.rect().contains(at)) {
            std::fprintf(stderr, "tool ring: %s: the ring at %d,%d %dx%d, the pointer at %d,%d, the canvas %d,%d %dx%d\n", when, at.x(), at.y(), at.width(), at.height(),
                         inWindow(view).x(), inWindow(view).y(), area.x(), area.y(), area.width(), area.height());
            failures++;
        }
    };

    s.addBlankLayer();   // something the brush paints without asking (the demo's active layer may be type)
    s.selectTool(Tool::Brush);
    s.brushErase = false;
    s.brushPreset.clear();
    s.brushSettings.diameter = 40;
    s.brushSettings.hardness = 1;
    s.brushSettings.opacity = 1;
    emit s.toolChanged();
    // The pointer near the canvas's top right corner: the ring cannot go up and right of it, and goes down and left.
    const QPointF corner(canvas->width() - 70, 60);
    mouse(QEvent::MouseMove, corner);

    // Alt, then D: the ring, with no Eyedropper held once Alt is let go and no menu bar taking the keyboard.
    expect(altD(), "Alt+D was not taken by its shortcut");
    expect(ring->isOpen() && overlay->isVisible(), "Alt+D did not open the ring");
    expect(!canvas->heldTool() && s.tool() == Tool::Brush, "Alt+D left the Eyedropper held");
    expect(!w.menuBar()->hasFocus() && !QApplication::activePopupWidget(), "Alt+D left the menu bar with the keyboard");
    expect(QApplication::focusWidget() == canvas, "the ring took the keyboard from the canvas");
    placedWell(corner, "opened by the corner");
    expect(overlay->geometry().right() < inWindow(corner).x() && overlay->geometry().top() > inWindow(corner).y(), "the ring by the top right corner is not below and left of the pointer");

    // F and S: Photoshop's [ and ] steps (40 to 48 and back), not Cycle Screen Modes or the Clone Stamp.
    expect(!tap(Qt::Key_F, "f"), "F went to a shortcut while the ring was open");
    expect(s.brushSettings.diameter == 48, "F did not step the brush size up to 48");
    expect(!w.fullScreenMode()->active(), "F changed the screen mode while the ring was open");
    expect(!tap(Qt::Key_S, "s"), "S went to a shortcut while the ring was open");
    expect(s.brushSettings.diameter == 40 && s.tool() == Tool::Brush, "S did not step the brush size back to 40, or switched tools");
    // A held key repeats a value.
    press(canvas, Qt::Key_F, Qt::NoModifier, QStringLiteral("f"));
    press(canvas, Qt::Key_F, Qt::NoModifier, QStringLiteral("f"), true);
    release(canvas, Qt::Key_F, Qt::NoModifier);
    expect(std::abs(s.brushSettings.diameter - 58) < 1e-9, "a held F did not repeat (40, 48, 58)");
    s.brushSettings.diameter = 40;
    // The other letters stay the ring's: V is opacity (not the Move tool), X hardness (not Swap colours), D the centre
    // (not Default colours), E hardness (not the Eraser).
    s.foregroundColor = Qt::red;
    s.backgroundColor = Qt::blue;
    tap(Qt::Key_V, "v");
    expect(s.tool() == Tool::Brush && std::abs(s.brushSettings.opacity - 0.9) < 1e-9, "V did not lower the opacity to 90%, or switched tools");
    tap(Qt::Key_X, "x");
    expect(s.foregroundColor == QColor(Qt::red) && std::abs(s.brushSettings.hardness - 0.75) < 1e-9, "X swapped the colours, or did not lower the hardness");
    tap(Qt::Key_D, "d");
    expect(s.foregroundColor == QColor(Qt::red) && s.backgroundColor == QColor(Qt::blue) && ring->isOpen(), "D reset the colours or closed the ring");
    tap(Qt::Key_E, "e");
    expect(!s.brushErase && s.tool() == Tool::Brush && std::abs(s.brushSettings.hardness - 1) < 1e-9, "E picked the Eraser, or did not raise the hardness");
    // The wheel steps the value used last (the hardness), the view staying where it is.
    tap(Qt::Key_X, "x");
    const QPointF origin = canvas->viewPointForTest(QPointF(0, 0));
    wheel(corner, 1);
    expect(std::abs(s.brushSettings.hardness - 1) < 1e-9, "the wheel did not step the hardness the ring used last");
    tap(Qt::Key_F, "f");
    wheel(corner, -1);
    expect(s.brushSettings.diameter == 40, "the wheel did not step the size back after F");
    expect(canvas->viewPointForTest(QPointF(0, 0)) == origin, "the wheel panned the view while the ring was open");
    // C: the Eraser and back, the ring open across it.
    tap(Qt::Key_C, "c");
    expect(s.brushErase && ring->isOpen(), "C did not switch to the Eraser with the ring open");
    tap(Qt::Key_C, "c");
    expect(!s.brushErase && ring->isOpen(), "C did not switch back to the Brush");
    // A greyed key says why: W (Previous Brush Preset) with none used before.
    tap(Qt::Key_W, "w");
    expect(!overlay->caption().isEmpty(), "W with no earlier preset showed no reason");

    // The mouse goes through the ring: what is under each of its circles is the canvas.
    for (char key : {'D', 'W', 'F', 'C'}) {
        const QPoint at = overlay->mapTo(&w, overlay->centreOf(key));
        expect(w.childAt(at) == canvas, "the ring takes the mouse (what is under one of its circles is not the canvas)");
    }
    {
        // The pointer comes at it: it moves away, once, clear of the pointer; small moves after leave it be.
        const QPointF view = canvas->mapFrom(&w, overlay->mapTo(&w, overlay->centreOf('D')));
        const QPoint before = overlay->pos();
        mouse(QEvent::MouseMove, view);
        expect(overlay->pos() != before, "the ring stayed under the pointer");
        placedWell(view, "after the pointer came at it");
        const QPoint moved = overlay->pos();
        mouse(QEvent::MouseMove, view + QPointF(2, 1));
        mouse(QEvent::MouseMove, view + QPointF(-3, 2));
        expect(overlay->pos() == moved, "the ring moved again for small moves of the pointer");
    }

    // A ring key while a stroke is painted: the stroke carries on and ends as one step; the size waits.
    {
        const size_t before = s.undoNames().size();
        const QPointF a = docView(0.3, 0.5), b = docView(0.4, 0.55), c = docView(0.5, 0.6);
        mouse(QEvent::MouseMove, a);
        mouse(QEvent::MouseButtonPress, a, Qt::LeftButton);
        mouse(QEvent::MouseMove, b, Qt::LeftButton);
        tap(Qt::Key_F, "f");
        expect(s.brushActive(), "a ring key ended the stroke under way");
        expect(s.brushSettings.diameter == 40, "the size changed in the middle of a stroke");
        mouse(QEvent::MouseMove, c, Qt::LeftButton);
        expect(s.brushActive(), "the stroke did not carry on after the ring key");
        mouse(QEvent::MouseButtonRelease, c);
        expect(!s.brushActive() && s.undoNames().size() == before + 1, "the stroke with a ring key in it was not one step");
    }

    // Esc closes it (and cancels nothing); the letters are the tools' again.
    tap(Qt::Key_Escape, nullptr);
    expect(!ring->isOpen() && !overlay->isVisible(), "Esc did not close the ring");
    expect(tap(Qt::Key_V, "v") && s.tool() == Tool::Move, "V did not pick the Move tool once the ring was closed");
    s.selectTool(Tool::Brush);
    emit s.toolChanged();

    // Alt+D in the middle of a stroke: the ring opens and the stroke carries on unbroken.
    {
        const size_t before = s.undoNames().size();
        const QPointF a = docView(0.3, 0.3), b = docView(0.35, 0.32);
        mouse(QEvent::MouseMove, a);
        mouse(QEvent::MouseButtonPress, a, Qt::LeftButton);
        mouse(QEvent::MouseMove, b, Qt::LeftButton);
        altD();
        expect(ring->isOpen(), "Alt+D during a stroke did not open the ring");
        expect(s.brushActive() && !canvas->heldTool(), "Alt+D ended the stroke or held the Eyedropper");
        mouse(QEvent::MouseMove, b + QPointF(30, 10), Qt::LeftButton);
        mouse(QEvent::MouseButtonRelease, b + QPointF(30, 10));
        expect(!s.brushActive() && s.undoNames().size() == before + 1, "the stroke Alt+D came in was not one step");
    }
    // Alt+D again closes it.
    altD();
    expect(!ring->isOpen(), "Alt+D again did not close the ring");

    // Another tool closes it; so does the idle timeout.
    altD();
    expect(ring->isOpen(), "the ring did not open again");
    if (QAction* marquee = w.commandRegistry().find(QStringLiteral("tool.marquee"))->action) marquee->trigger();
    expect(!ring->isOpen(), "another tool did not close the ring");
    // The selection tools' ring: Add (R), Feather (F), back to New (E).
    altD();
    tap(Qt::Key_R, "r");
    tap(Qt::Key_F, "f");
    expect(s.tool() == Tool::Marquee && s.selectionToolMode == SelectionMode::Add && s.selectionToolFeather == 1, "the selection ring's R and F did not set Add and a feather of 1 px");
    tap(Qt::Key_E, "e");
    expect(s.selectionToolMode == SelectionMode::Replace, "the selection ring's E did not go back to New Selection");
    s.selectionToolFeather = 0;
    tap(Qt::Key_Escape, nullptr);
    settings.setValue(QStringLiteral("toolRing/idleMs"), 150);
    altD();
    expect(ring->isOpen(), "the ring did not open for the idle check");
    wait(450);
    expect(!ring->isOpen(), "the ring did not close after the idle timeout");
    settings.setValue(QStringLiteral("toolRing/idleMs"), 3000);
    // Turned off in Preferences, Alt+D does nothing.
    settings.setValue(QStringLiteral("toolRing/enabled"), false);
    altD();
    expect(!ring->isOpen(), "Alt+D opened the ring with it turned off");
    settings.setValue(QStringLiteral("toolRing/enabled"), true);

    // Full screen: the ring over the canvas (no edge flyout holds it), and Alt then D leaves no menu bar behind.
    s.selectTool(Tool::Brush);
    w.fullScreenMode()->setPolling(false);
    w.fullScreenMode()->setTimings(0, 0);
    w.setScreenMode(true);
    QApplication::processEvents();
    wait(100);
    canvas->setFocus();
    mouse(QEvent::MouseMove, QPointF(canvas->width() / 2.0, canvas->height() / 2.0));
    altD();
    wait(100);
    expect(ring->isOpen() && overlay->isVisible() && overlay->parentWidget() == &w, "Alt+D did not show the ring over the canvas in full screen");
    placedWell(QPointF(canvas->width() / 2.0, canvas->height() / 2.0), "in full screen");
    expect(!(w.fullScreenMode()->menuStrip() && w.fullScreenMode()->menuStrip()->isVisible()) && !QApplication::activePopupWidget(), "Alt+D in full screen opened the menus");
    tap(Qt::Key_F, "f");
    expect(w.fullScreenMode()->active(), "F with the ring open left full screen");
    tap(Qt::Key_Escape, nullptr);
    expect(!ring->isOpen() && w.fullScreenMode()->active(), "Esc did not close the ring alone in full screen");
    w.setScreenMode(false);
    QApplication::processEvents();

    std::printf("tool ring: %s\n", failures ? "FAILED" : "ok");
    return failures;
}

} // namespace app
