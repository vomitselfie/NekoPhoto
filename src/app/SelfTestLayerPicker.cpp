// layer-picker: View > Layer Picker (Alt+A), through synthesised keys and mouse events: over a point where three
// layers of the test's own sit above two of the demo's (with a hidden one, one whose mask hides the point, one with a
// hole there and one elsewhere around them), it lists exactly those five, top of the stack first;
// F/R step up and S/V down, each layer becoming the active one as it is stepped to, stopping at the ends; Esc puts
// back the layer active when it opened, Alt+A again, Enter and a click on the canvas keep the choice, none of it
// changes the document or its history, the mouse goes through it, a transform under way is left alone, it says
// "No layer here" off the document, and it works in full screen and with the Layers panel hidden.
#include "SelfTestSupport.h"
#include "CanvasWidget.h"
#include "EditorSession.h"
#include "WindowEdges.h"
#include "LayerPicker.h"
#include "MainWindow.h"
#include "nekophoto/document.h"
#include <QApplication>
#include <QDockWidget>
#include <QEventLoop>
#include <QKeyEvent>
#include <QMenuBar>
#include <QMouseEvent>
#include <QSettings>
#include <QTimer>
#include <cstdio>

// QtGui's shortcut pass for a key press (SelfTestKeys.cpp has the same).
Q_GUI_EXPORT bool qt_sendShortcutOverrideEvent(QObject* o, ulong timestamp, int k, Qt::KeyboardModifiers mods, const QString& text, bool autorep, ushort count);

using namespace nekophoto;

namespace app {

using namespace selftest;

int layerPicker(MainWindow& w) {
    EditorSession& s = *w.session();
    buildDemoDocument(s);
    w.resize(1400, 900);
    w.show();
    w.activateWindow();
    QApplication::processEvents();
    s.fitView();
    QApplication::processEvents();
    CanvasWidget* canvas = w.canvasAt(w.currentTabIndex());
    LayerPicker* picker = w.layerPicker();
    if (!canvas || !picker || !s.document()) { std::fprintf(stderr, "layer picker: no canvas, picker or document\n"); return 1; }
    canvas->setFocus();
    QApplication::processEvents();
    QSettings settings;
    settings.setValue(QStringLiteral("toolRing/idleMs"), 0);
    settings.setValue(QStringLiteral("toolRing/corner"), 0);
    int failures = 0;
    auto expect = [&](bool ok, const char* what) { if (!ok) { std::fprintf(stderr, "layer picker: %s\n", what); failures++; } };
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
    auto tap = [&](int key, const char* text) { const bool taken = press(canvas, key, Qt::NoModifier, text ? QString::fromLatin1(text) : QString()); release(canvas, key, Qt::NoModifier); return taken; };
    auto altA = [&] {
        press(canvas, Qt::Key_Alt, Qt::AltModifier);
        const bool taken = press(canvas, Qt::Key_A, Qt::AltModifier, QStringLiteral("a"));
        release(canvas, Qt::Key_A, Qt::AltModifier);
        release(canvas, Qt::Key_Alt, Qt::NoModifier);
        return taken;
    };
    auto mouse = [canvas](QEvent::Type type, QPointF view, Qt::MouseButtons buttons = Qt::NoButton) {
        const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
        QMouseEvent e(type, view, canvas->mapToGlobal(view), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(canvas, &e);
    };
    auto wait = [](int ms) { QEventLoop loop; QTimer::singleShot(ms, &loop, &QEventLoop::quit); loop.exec(); };
    const Document& doc = *s.document();
    auto named = [&](const char* name) -> std::optional<Uuid> {
        for (const Layer& l : s.document()->layers) if (l.name == name) return l.id;
        return std::nullopt;
    };

    // The test's layers, 40 px squares round a point of the demo's left side, each inserted on top. Under the point
    // the demo shows its Background and the merged Paint layer (its stripes are a translucent white there).
    const std::optional<Uuid> background = named("Background"), paint = named("Paint");
    const QPointF point(doc.width * 0.08, doc.height * 0.45);
    const std::vector<Uuid> demo = {paint.value_or(Uuid()), background.value_or(Uuid())};
    expect(s.layersAt(point + QPointF(0.5, 0.5)) == demo, "the demo's layers under the point are not Paint and the Background");
    auto square = [&](const char* name, QPointF centre, bool hole = false) {
        s.selectLayer(s.document()->layers.back().id);
        auto image = std::make_shared<Image>(40, 40);
        image->fill(uint8_t(60 + 40 * (name[0] % 4)), 90, 160, 255);
        if (hole) for (int y = 16; y < 24; ++y) for (int x = 16; x < 24; ++x) std::fill_n(image->pixel(x, y), 4, uint8_t(0));
        s.insertImage(image, QString::fromLatin1(name), centre);
        return *s.activeLayerId();
    };
    const Uuid a = square("A", point), b = square("B", point + QPointF(6, 4));
    const Uuid hidden = square("Hidden", point);
    s.toggleLayerVisibility(hidden);
    const Uuid c = square("C", point + QPointF(-5, 7));
    square("Masked", point);
    s.addLayerMask(false);   // Hide All: the mask hides the point
    square("Hole", point, true);
    square("Elsewhere", point + QPointF(120, 0));
    s.selectLayer(a);
    s.selectTool(Tool::Move);
    emit s.toolChanged();
    QApplication::processEvents();
    const QPointF view = canvas->viewPointForTest(point + QPointF(0.5, 0.5));
    mouse(QEvent::MouseMove, view);
    const uint64_t revision = s.documentRevision(), history = s.historyRevision();
    const size_t steps = s.undoNames().size();
    const bool modified = s.isModified();

    // Alt+A: exactly C, B, A and the Background, top of the stack first; A, active, lit.
    expect(altA(), "Alt+A was not taken by its shortcut");
    expect(picker->isOpen() && picker->overlay()->isVisible(), "Alt+A did not open the layer picker");
    expect(!w.menuBar()->hasFocus() && !QApplication::activePopupWidget() && QApplication::focusWidget() == canvas, "Alt+A left the menu bar or the picker with the keyboard");
    const std::vector<Uuid> want = {c, b, a, demo[0], demo[1]};
    expect(picker->layers() == want, "the picker did not list exactly C, B, A, Paint and the Background, top first");
    if (picker->layers() != want) {
        for (const Uuid& id : picker->layers()) if (const Layer* l = s.document()->find(id)) std::fprintf(stderr, "  listed: %s\n", l->name.c_str());
    }
    const auto& entries = picker->overlay()->entries();
    expect(entries.size() == 5 && entries[2].active && !entries[0].active && entries[0].name == QLatin1String("C"), "the picker's rows are not C, B, A, Paint, Background with A lit");
    expect(!picker->overlay()->geometry().contains(canvas->mapTo(&w, view.toPoint())), "the picker opened under the pointer");
    // The mouse goes through it.
    for (int i = 0; i < int(entries.size()); ++i)
        expect(w.childAt(picker->overlay()->mapTo(&w, picker->overlay()->entryRect(i).center())) == canvas, "the picker takes the mouse (what is under a row is not the canvas)");

    // F/R up, S/V down, live; the ends stop.
    tap(Qt::Key_F, "f");
    expect(s.activeLayerId() == b && picker->overlay()->entries()[1].active, "F did not make B (above A) active");
    tap(Qt::Key_R, "r");
    expect(s.activeLayerId() == c, "R did not make C active");
    tap(Qt::Key_R, "r");
    expect(s.activeLayerId() == c && !picker->overlay()->caption().isEmpty(), "R at the top did not stay on C and say so");
    tap(Qt::Key_S, "s");
    expect(s.activeLayerId() == b, "S did not make B active");
    tap(Qt::Key_V, "v");
    expect(s.activeLayerId() == a && s.tool() == Tool::Move, "V did not make A active (or picked a tool)");
    tap(Qt::Key_V, "v");
    expect(s.activeLayerId() == paint, "V did not make Paint active");
    tap(Qt::Key_V, "v");
    expect(s.activeLayerId() == background, "V did not make the Background active");
    tap(Qt::Key_S, "s");
    expect(s.activeLayerId() == background && picker->isOpen(), "S at the bottom did not stay on the Background");
    // Esc puts A back.
    tap(Qt::Key_Escape, nullptr);
    expect(!picker->isOpen() && !picker->overlay()->isVisible(), "Esc did not close the picker");
    expect(s.activeLayerId() == a && s.selectedLayerIds() == std::set<Uuid>{a}, "Esc did not put back the layer active when it opened");
    // Alt+A again keeps the choice; so does Enter.
    altA();
    tap(Qt::Key_F, "f");
    altA();
    expect(!picker->isOpen() && s.activeLayerId() == b, "Alt+A again did not close the picker keeping B");
    altA();
    tap(Qt::Key_R, "r");
    tap(Qt::Key_Return, nullptr);
    expect(!picker->isOpen() && s.activeLayerId() == c, "Enter did not close the picker keeping C");
    expect(s.documentRevision() == revision && s.historyRevision() == history && s.undoNames().size() == steps && s.isModified() == modified,
           "choosing layers with the picker changed the document or its history");
    // A transform under way is left alone: the picker says why and keeps the layer.
    s.beginTransform(true, false);
    if (s.transformEdit()) {
        altA();
        tap(Qt::Key_S, "s");
        expect(s.activeLayerId() == c && s.transformEdit() && !picker->overlay()->caption().isEmpty(), "a step during a transform chose a layer (applying the transform) or said nothing");
        tap(Qt::Key_Escape, nullptr);
        s.cancelTransform();
    }

    // From a layer not under the pointer, a key enters the list next to it in the stack.
    s.selectLayer(hidden);   // between B and C
    altA();
    tap(Qt::Key_F, "f");
    expect(s.activeLayerId() == c, "F from a layer not listed (between B and C) did not go to C, above it");
    tap(Qt::Key_Escape, nullptr);
    altA();
    tap(Qt::Key_S, "s");
    expect(s.activeLayerId() == b, "S from a layer not listed (between B and C) did not go to B, below it");
    tap(Qt::Key_Escape, nullptr);
    expect(s.activeLayerId() == hidden, "Esc did not put back the hidden layer");

    // Off the document: "No layer here".
    {
        QPointF off(2, canvas->height() / 2.0);
        if (canvas->documentPoint(off).x() >= 0) off = QPointF(canvas->width() / 2.0, 2);
        if (canvas->documentPoint(off).x() < 0 || canvas->documentPoint(off).y() < 0) {
            mouse(QEvent::MouseMove, off);
            altA();
            expect(picker->isOpen() && picker->layers().empty() && picker->overlay()->entries().empty(), "off the document the picker listed layers");
            tap(Qt::Key_F, "f");
            expect(s.activeLayerId() == hidden && picker->overlay()->caption() == QLatin1String("No layer here"), "F off the document chose a layer or did not say No layer here");
            tap(Qt::Key_Escape, nullptr);
        } else {
            std::fprintf(stderr, "layer picker: (no canvas margin to point off the document; skipped)\n");
        }
    }

    // A click on the canvas keeps the choice (and goes on to the canvas).
    mouse(QEvent::MouseMove, view);
    s.selectLayer(a);
    altA();
    tap(Qt::Key_F, "f");
    mouse(QEvent::MouseButtonPress, view, Qt::LeftButton);
    expect(!picker->isOpen(), "a click on the canvas did not close the picker");
    mouse(QEvent::MouseButtonRelease, view);
    expect(s.activeLayerId() == b, "a click on the canvas did not keep B");
    // Another tool closes it, keeping the choice.
    altA();
    tap(Qt::Key_V, "v");
    s.selectTool(Tool::Marquee);
    emit s.toolChanged();
    expect(!picker->isOpen() && s.activeLayerId() == a, "another tool did not close the picker keeping A");
    s.selectTool(Tool::Move);
    emit s.toolChanged();

    // With the Layers panel hidden, and in full screen.
    if (QDockWidget* dock = w.findChild<QDockWidget*>(QStringLiteral("layersDock"))) dock->hide();
    QApplication::processEvents();
    altA();
    tap(Qt::Key_F, "f");
    expect(picker->isOpen() && picker->overlay()->isVisible() && s.activeLayerId() == b, "the picker did not work with the Layers panel hidden");
    tap(Qt::Key_Escape, nullptr);
    w.windowEdges()->setPolling(false);
    w.windowEdges()->setTimings(0, 0);
    w.setScreenMode(true);
    QApplication::processEvents();
    wait(100);
    canvas->setFocus();
    mouse(QEvent::MouseMove, canvas->viewPointForTest(point + QPointF(0.5, 0.5)));
    altA();
    wait(50);
    expect(picker->isOpen() && picker->overlay()->isVisible() && picker->overlay()->parentWidget() == &w && picker->layers() == want, "Alt+A did not show the picker in full screen");
    expect(!(w.windowEdges()->menuStrip() && w.windowEdges()->menuStrip()->isVisible()) && !QApplication::activePopupWidget(), "Alt+A in full screen opened the menus");
    tap(Qt::Key_R, "r");
    expect(s.activeLayerId() == b, "R in full screen did not step up");
    tap(Qt::Key_Escape, nullptr);
    expect(!picker->isOpen() && w.windowEdges()->active() && s.activeLayerId() == a, "Esc in full screen did not close the picker alone and put A back");
    w.setScreenMode(false);
    QApplication::processEvents();

    std::printf("layer picker: %s\n", failures ? "FAILED" : "ok");
    return failures;
}

} // namespace app
