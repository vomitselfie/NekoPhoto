// color-ring: View > Colour Ring (Alt+C), through synthesised key, mouse and wheel events: Alt then C opens it without
// leaving the held Eyedropper or the menu bar behind, S/F step the foreground's hue, X/E its saturation and V/R its
// brightness by the ring's steps (held keys repeating, the wheel stepping the value used last), C swaps the colours,
// D sets the defaults, W recalls the recent colours, none of it is an undo step, the mouse goes through it, a stroke
// in progress carries on, its letters are the tools' again once it is closed, and it and the Tool Ring take turns.
#include "SelfTestSupport.h"
#include "CanvasWidget.h"
#include "ColorRing.h"
#include "EditorSession.h"
#include "MainWindow.h"
#include "ToolRing.h"
#include <QApplication>
#include <QKeyEvent>
#include <QMenuBar>
#include <QMouseEvent>
#include <QSettings>
#include <QWheelEvent>
#include <cmath>
#include <cstdio>

// QtGui's shortcut pass for a key press (SelfTestKeys.cpp has the same).
Q_GUI_EXPORT bool qt_sendShortcutOverrideEvent(QObject* o, ulong timestamp, int k, Qt::KeyboardModifiers mods, const QString& text, bool autorep, ushort count);

using namespace nekophoto;

namespace app {

using namespace selftest;

int colorRing(MainWindow& w) {
    EditorSession& s = *w.session();
    buildDemoDocument(s);
    w.resize(1400, 900);
    w.show();
    w.activateWindow();
    QApplication::processEvents();
    s.fitView();
    QApplication::processEvents();
    CanvasWidget* canvas = w.canvasAt(w.currentTabIndex());
    ColorRing* ring = w.colorRing();
    if (!canvas || !ring) { std::fprintf(stderr, "colour ring: no canvas or no ring\n"); return 1; }
    canvas->setFocus();
    QApplication::processEvents();
    QSettings settings;
    settings.setValue(QStringLiteral("toolRing/enabled"), true);
    settings.setValue(QStringLiteral("toolRing/idleMs"), 0);
    settings.setValue(QStringLiteral("toolRing/corner"), 0);
    int failures = 0;
    auto expect = [&](bool ok, const char* what) { if (!ok) { std::fprintf(stderr, "colour ring: %s\n", what); failures++; } };
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
    auto alt = [&](int key, const char* text) {
        press(canvas, Qt::Key_Alt, Qt::AltModifier);
        const bool taken = press(canvas, key, Qt::AltModifier, QString::fromLatin1(text));
        release(canvas, key, Qt::AltModifier);
        release(canvas, Qt::Key_Alt, Qt::NoModifier);
        return taken;
    };
    auto mouse = [canvas](QEvent::Type type, QPointF view, Qt::MouseButtons buttons = Qt::NoButton) {
        const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
        QMouseEvent e(type, view, canvas->mapToGlobal(view), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(canvas, &e);
    };
    auto wheel = [canvas](QPointF view, int notches) {
        QWheelEvent e(view, canvas->mapToGlobal(view), QPoint(), QPoint(0, 120 * notches), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(canvas, &e);
    };
    auto docView = [&](double fx, double fy) { return canvas->viewPointForTest(QPointF(s.document()->width * fx, s.document()->height * fy)); };
    // The foreground's HSB as the colour picker shows it (whole degrees and percent).
    auto hue = [&] { return std::lround(s.foregroundColor.toHsv().hsvHueF() * 360); };
    auto saturation = [&] { return std::lround(s.foregroundColor.toHsv().hsvSaturationF() * 100); };
    auto brightness = [&] { return std::lround(s.foregroundColor.toHsv().valueF() * 100); };

    s.addBlankLayer();
    s.selectTool(Tool::Brush);
    s.brushErase = false;
    s.brushPreset.clear();
    s.brushSettings.diameter = 30;
    emit s.toolChanged();
    // Two recent colours: green (the document edited with it), then the red the ring opens with.
    s.foregroundColor = QColor(0, 200, 0);
    s.addBlankLayer();
    s.foregroundColor = QColor(255, 0, 0);
    s.backgroundColor = QColor(0, 0, 255);
    const QPointF where = docView(0.3, 0.4);
    mouse(QEvent::MouseMove, where);
    const size_t steps = s.undoNames().size();
    const uint64_t revision = s.historyRevision();

    expect(alt(Qt::Key_C, "c"), "Alt+C was not taken by its shortcut");
    expect(ring->isOpen() && ring->overlay()->isVisible(), "Alt+C did not open the colour ring");
    expect(!canvas->heldTool() && s.tool() == Tool::Brush, "Alt+C left the Eyedropper held");
    expect(!w.menuBar()->hasFocus() && !QApplication::activePopupWidget(), "Alt+C left the menu bar with the keyboard");
    expect(QApplication::focusWidget() == canvas, "the colour ring took the keyboard from the canvas");
    expect(!ring->overlay()->geometry().contains(canvas->mapTo(&w, where.toPoint())), "the colour ring opened under the pointer");

    // Hue round the wheel: F up 10 degrees, S down, through 0 to 350.
    expect(!tap(Qt::Key_F, "f"), "F went to a shortcut while the colour ring was open");
    expect(hue() == 10 && saturation() == 100 && brightness() == 100, "F did not turn the hue to 10 degrees");
    tap(Qt::Key_S, "s");
    expect(s.foregroundColor == QColor(255, 0, 0), "S did not turn the hue back to pure red");
    tap(Qt::Key_S, "s");
    expect(hue() == 350, "S did not go round the wheel to 350 degrees");
    tap(Qt::Key_F, "f");
    expect(s.foregroundColor == QColor(255, 0, 0) && s.tool() == Tool::Brush, "F did not come back to red, or switched tools");
    // Saturation: X down, E up; brightness: V down, R up (the colour picker's HSB, in 5% steps).
    tap(Qt::Key_X, "x");
    expect(saturation() == 95 && hue() == 0 && brightness() == 100 && s.backgroundColor == QColor(0, 0, 255), "X did not lower the saturation to 95% (or swapped the colours)");
    tap(Qt::Key_E, "e");
    expect(s.foregroundColor == QColor(255, 0, 0) && !s.brushErase, "E did not raise the saturation back (or picked the Eraser)");
    tap(Qt::Key_V, "v");
    expect(brightness() == 95 && saturation() == 100 && s.tool() == Tool::Brush, "V did not lower the brightness to 95% (or picked the Move tool)");
    tap(Qt::Key_R, "r");
    expect(s.foregroundColor == QColor(255, 0, 0), "R did not raise the brightness back");
    // A held key repeats; the wheel steps the value used last (the hue) the other way.
    press(canvas, Qt::Key_F, Qt::NoModifier, QStringLiteral("f"));
    press(canvas, Qt::Key_F, Qt::NoModifier, QStringLiteral("f"), true);
    release(canvas, Qt::Key_F, Qt::NoModifier);
    expect(hue() == 20, "a held F did not repeat (0, 10, 20 degrees)");
    const QPointF origin = canvas->viewPointForTest(QPointF(0, 0));
    wheel(where, -1);
    expect(hue() == 10, "the wheel did not step the hue the ring used last");
    expect(canvas->viewPointForTest(QPointF(0, 0)) == origin, "the wheel zoomed or panned while the colour ring was open");
    tap(Qt::Key_V, "v");
    wheel(where, 1);
    expect(brightness() == 100 && hue() == 10, "the wheel did not step the brightness after V");
    tap(Qt::Key_S, "s");
    // A grey keeps its hue: no saturation, then saturation back, is the same red.
    for (int i = 0; i < 20; ++i) tap(Qt::Key_X, "x");
    expect(saturation() == 0 && brightness() == 100, "twenty X did not take the saturation to 0");
    tap(Qt::Key_F, "f");
    for (int i = 0; i < 20; ++i) tap(Qt::Key_E, "e");
    expect(hue() == 10 && saturation() == 100, "the hue set on a grey was not kept as the saturation came back");
    tap(Qt::Key_S, "s");
    expect(s.foregroundColor == QColor(255, 0, 0), "the colour did not come back to red");

    // C swaps; D sets black and white; W recalls red, then green.
    tap(Qt::Key_C, "c");
    expect(s.foregroundColor == QColor(0, 0, 255) && s.backgroundColor == QColor(255, 0, 0) && ring->isOpen(), "C did not swap the colours");
    tap(Qt::Key_C, "c");
    expect(s.foregroundColor == QColor(255, 0, 0) && s.backgroundColor == QColor(0, 0, 255), "C again did not swap them back");
    tap(Qt::Key_D, "d");
    expect(s.foregroundColor == QColor(Qt::black) && s.backgroundColor == QColor(Qt::white) && ring->isOpen(), "D did not set the default colours");
    tap(Qt::Key_W, "w");
    expect(s.foregroundColor == QColor(255, 0, 0), "W did not recall the colour the ring opened with");
    tap(Qt::Key_W, "w");
    expect(s.foregroundColor == QColor(0, 200, 0), "W again did not recall the colour the document was edited with");
    expect(s.backgroundColor == QColor(Qt::white), "W changed the background colour");
    expect(s.undoNames().size() == steps && s.historyRevision() == revision, "a colour ring key made an undo step");

    // The mouse goes through it: what is under each circle is the canvas.
    for (char key : {'D', 'W', 'F', 'C', 'V'}) {
        const QPoint at = ring->overlay()->mapTo(&w, ring->overlay()->centreOf(key));
        expect(w.childAt(at) == canvas, "the colour ring takes the mouse (what is under one of its circles is not the canvas)");
    }
    // A stroke carries on through a ring key; the stroke is one step, the colour changed for the next.
    {
        const QPointF a = docView(0.3, 0.5), b = docView(0.4, 0.55), c = docView(0.5, 0.6);
        mouse(QEvent::MouseMove, a);
        mouse(QEvent::MouseButtonPress, a, Qt::LeftButton);
        mouse(QEvent::MouseMove, b, Qt::LeftButton);
        const QColor before = s.foregroundColor;
        tap(Qt::Key_F, "f");
        expect(s.brushActive(), "a colour ring key ended the stroke under way");
        expect(s.foregroundColor != before, "F did not change the colour during a stroke");
        mouse(QEvent::MouseMove, c, Qt::LeftButton);
        expect(s.brushActive() && ring->isOpen(), "the stroke did not carry on after the ring key, or the ring closed");
        mouse(QEvent::MouseButtonRelease, c);
        expect(!s.brushActive() && s.undoNames().size() == steps + 1, "the stroke with a colour ring key in it was not one step");
    }

    // Esc closes it; the letters are the tools' again.
    tap(Qt::Key_Escape, nullptr);
    expect(!ring->isOpen() && !ring->overlay()->isVisible(), "Esc did not close the colour ring");
    expect(tap(Qt::Key_X, "x"), "X did not swap the colours once the colour ring was closed");
    tap(Qt::Key_X, "x");
    // Alt+C again closes it; the Tool Ring and the Colour Ring take turns.
    alt(Qt::Key_C, "c");
    expect(ring->isOpen(), "Alt+C did not open the colour ring again");
    alt(Qt::Key_C, "c");
    expect(!ring->isOpen(), "Alt+C again did not close the colour ring");
    alt(Qt::Key_D, "d");
    expect(w.toolRing()->isOpen(), "Alt+D did not open the Tool Ring");
    alt(Qt::Key_C, "c");
    expect(ring->isOpen() && !w.toolRing()->isOpen(), "Alt+C over the Tool Ring did not take its place");
    // Another tool closes it.
    s.selectTool(Tool::Marquee);
    emit s.toolChanged();
    expect(!ring->isOpen(), "another tool did not close the colour ring");

    std::printf("colour ring: %s\n", failures ? "FAILED" : "ok");
    return failures;
}

} // namespace app
