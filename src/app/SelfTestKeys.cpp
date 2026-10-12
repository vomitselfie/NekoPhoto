// held-keys: Photoshop's held tools (Alt, Ctrl, Ctrl+Space), spring-loaded tool letters, F7, F12, Ctrl+Alt+Z and the
// type size keys, through synthesised key and mouse events.
//
// The command registry's checks (every menu item a command with an id of its own, every key one action's; part
// of command-path) and shortcuts: Edit > Keyboard Shortcuts.
#include "SelfTestSupport.h"
#include "CanvasWidget.h"
#include "CommandPalette.h"
#include "CommandRegistry.h"
#include "EditorSession.h"
#include "KeyboardShortcuts.h"
#include "MainWindow.h"
#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <vector>

// QtGui's shortcut pass for a key press (what QTest's key clicks use): the focus widget's ShortcutOverride, then the
// window's shortcuts; true when a shortcut took the key.
Q_GUI_EXPORT bool qt_sendShortcutOverrideEvent(QObject* o, ulong timestamp, int k, Qt::KeyboardModifiers mods, const QString& text, bool autorep, ushort count);

using namespace compositor;

namespace app {

using namespace selftest;

/// Photoshop's held and missing keys, typed as a person would (each press offered to the shortcuts first, as QTest
/// does): Alt with the Brush samples the foreground and lets go back to the Brush, Ctrl moves the layer under the
/// pointer with the Move tool for as long as it is held, Ctrl+Space and Ctrl+Alt+Space zoom, a tool's letter held while
/// the tool is used springs back and a tap does not, F7 shows and hides the Layers panel (not from a text field),
/// Ctrl+Alt+Z toggles the last state, F12 reverts (one undo step), and Ctrl+Shift+> and < size the type being typed.
int heldKeys(MainWindow& w) {
    EditorSession& s = *w.session();
    buildDemoDocument(s);
    // A roomy window with the document fitted: the clicks below land on document points, which a small window under a
    // theme with taller panels (Windows' fonts under Goth Kitty) could leave outside the canvas.
    w.resize(1400, 900);
    w.show();
    w.activateWindow();
    QApplication::processEvents();
    s.fitView();
    QApplication::processEvents();
    CanvasWidget* canvas = w.canvasAt(w.currentTabIndex());
    if (!canvas) { std::fprintf(stderr, "no canvas\n"); return 1; }
    canvas->setFocus();
    QApplication::processEvents();
    int failures = 0;
    auto expect = [&](bool ok, const char* what) { if (!ok) { std::fprintf(stderr, "held keys: %s\n", what); failures++; } };
    // Every point the test clicks must be on the canvas widget, or the presses go nowhere.
    for (QPointF p : {QPointF(30, 400), QPointF(330, 260), QPointF(350, 270), QPointF(320, 210), QPointF(40, 40), QPointF(100, 100)})
        if (!canvas->rect().contains(canvas->viewPointForTest(p).toPoint())) {
            std::fprintf(stderr, "held keys: document point %g,%g is off the canvas (%dx%d)\n", p.x(), p.y(), canvas->width(), canvas->height());
            failures++;
        }
    // A key press goes to the shortcuts first, then (when none took it) to the widget; true when a shortcut took it.
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
    auto mouse = [canvas](QEvent::Type type, QPointF documentPoint, Qt::KeyboardModifiers modifiers) {
        const QPointF local = canvas->viewPointForTest(documentPoint);
        QMouseEvent e(type, local, canvas->mapToGlobal(local), Qt::LeftButton, type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, modifiers);
        QApplication::sendEvent(canvas, &e);
    };
    auto click = [&](QPointF documentPoint, Qt::KeyboardModifiers modifiers) {
        mouse(QEvent::MouseButtonPress, documentPoint, modifiers);
        mouse(QEvent::MouseButtonRelease, documentPoint, modifiers);
    };

    // Alt with the Brush: the Eyedropper, sampling the foreground (not the background, as Alt with the Eyedropper itself).
    s.selectTool(Tool::Brush);
    s.brushErase = false;
    s.foregroundColor = Qt::black;
    s.backgroundColor = Qt::white;
    const QPointF sample(30, 400);
    const QColor expected = s.compositeColorAt(sample).value_or(QColor());
    const size_t stepsBefore = s.undoNames().size();
    press(canvas, Qt::Key_Alt, Qt::AltModifier);
    expect(canvas->heldTool() == Tool::Eyedropper, "Alt with the Brush did not hold the Eyedropper");
    expect(s.tool() == Tool::Brush, "Alt changed the session's tool");
    click(sample, Qt::AltModifier);
    expect(expected.isValid() && expected != QColor(Qt::black) && s.foregroundColor == expected, "the held Eyedropper did not sample the foreground");
    if (s.foregroundColor != expected) {
        // What it saw, for a failure only one platform shows: the colours, where the click landed, what took it.
        const QPointF at = canvas->viewPointForTest(sample);
        QWidget* under = QApplication::widgetAt(canvas->mapToGlobal(at.toPoint()));
        std::fprintf(stderr, "  expected %s, foreground %s, held tool %d, click at %g,%g on a %dx%d canvas, widget there: %s\n",
                     qPrintable(expected.name(QColor::HexArgb)), qPrintable(s.foregroundColor.name(QColor::HexArgb)), int(canvas->heldTool().value_or(Tool::Move)),
                     at.x(), at.y(), canvas->width(), canvas->height(), under ? under->metaObject()->className() : "none");
    }
    expect(s.backgroundColor == QColor(Qt::white), "the held Eyedropper changed the background");
    expect(s.undoNames().size() == stepsBefore, "Alt-click with the Brush painted");
    release(canvas, Qt::Key_Alt, Qt::NoModifier);
    expect(!canvas->heldTool() && s.tool() == Tool::Brush, "letting go of Alt did not go back to the Brush");
    // Dragged, the held Eyedropper keeps sampling: the foreground follows the pointer to a differently coloured spot.
    {
        std::optional<QPointF> other;
        for (int y = 20; y < s.document()->height && !other; y += 37)
            for (int x = 20; x < s.document()->width && !other; x += 41)
                if (auto c = s.compositeColorAt(QPointF(x, y)); c && *c != expected) other = QPointF(x, y);
        if (!other) { std::fprintf(stderr, "held keys: no second colour in the demo document\n"); return 1; }
        const QColor second = s.compositeColorAt(*other).value_or(QColor());
        s.foregroundColor = Qt::black;
        press(canvas, Qt::Key_Alt, Qt::AltModifier);
        mouse(QEvent::MouseButtonPress, sample, Qt::AltModifier);
        expect(s.foregroundColor == expected, "the held Eyedropper's press did not sample");
        mouse(QEvent::MouseMove, *other, Qt::AltModifier);
        expect(s.foregroundColor == second, "the held Eyedropper did not follow the drag");
        expect(canvas->heldTool() == Tool::Eyedropper, "the held Eyedropper let go mid-drag");
        mouse(QEvent::MouseButtonRelease, *other, Qt::AltModifier);
        release(canvas, Qt::Key_Alt, Qt::NoModifier);
        expect(s.undoNames().size() == stepsBefore && s.backgroundColor == QColor(Qt::white), "the held Eyedropper's drag painted or changed the background");
        // The Eyedropper itself does the same, and with Alt into the background.
        s.selectTool(Tool::Eyedropper);
        mouse(QEvent::MouseButtonPress, *other, Qt::NoModifier);
        mouse(QEvent::MouseMove, sample, Qt::NoModifier);
        mouse(QEvent::MouseButtonRelease, sample, Qt::NoModifier);
        expect(s.foregroundColor == expected, "a drag with the Eyedropper did not follow the pointer");
        mouse(QEvent::MouseButtonPress, sample, Qt::AltModifier);
        mouse(QEvent::MouseMove, *other, Qt::AltModifier);
        mouse(QEvent::MouseButtonRelease, *other, Qt::AltModifier);
        expect(s.backgroundColor == second && s.foregroundColor == expected, "an Alt-drag with the Eyedropper did not sample the background");
        s.backgroundColor = Qt::white;
        s.selectTool(Tool::Brush);
    }
    // The Eraser keeps Alt for itself.
    s.brushErase = true;
    press(canvas, Qt::Key_Alt, Qt::AltModifier);
    expect(!canvas->heldTool(), "Alt with the Eraser held the Eyedropper");
    release(canvas, Qt::Key_Alt, Qt::NoModifier);
    s.brushErase = false;

    // Ctrl with the Brush: the Move tool, which picks the layer under the pointer and drags it.
    const QPointF from(330, 260), to(350, 270);
    const std::optional<Uuid> under = s.layerAt(from);
    const Layer* before = under ? s.document()->find(*under) : nullptr;
    const Point origin = before ? before->transform.origin : Point();
    press(canvas, Qt::Key_Control, Qt::ControlModifier);
    expect(canvas->heldTool() == Tool::Move, "Ctrl with the Brush did not hold the Move tool");
    mouse(QEvent::MouseButtonPress, from, Qt::ControlModifier);
    mouse(QEvent::MouseMove, to, Qt::ControlModifier);
    mouse(QEvent::MouseButtonRelease, to, Qt::ControlModifier);
    const Layer* after = under ? s.document()->find(*under) : nullptr;
    expect(after && std::abs(after->transform.origin.x - origin.x - 20) < 0.5 && std::abs(after->transform.origin.y - origin.y - 10) < 0.5,
           "the held Move tool did not move the layer under the pointer by 20, 10");
    if (after) std::printf("held move: %g, %g\n", after->transform.origin.x - origin.x, after->transform.origin.y - origin.y);
    expect(!s.transformEdit(), "the held Move tool left a transform open");
    release(canvas, Qt::Key_Control, Qt::NoModifier);
    expect(!canvas->heldTool() && s.tool() == Tool::Brush, "letting go of Ctrl did not go back to the Brush");
    // Ctrl with the Pen: the Direct Selection tool while held, dragging an anchor of the path.
    {
        s.selectTool(Tool::Pen);
        s.penMode = EditorSession::PenMode::Path;   // the Work Path, so every component lands in one path
        for (QPointF p : {QPointF(100, 100), QPointF(200, 100), QPointF(200, 200)}) s.penPress(p);
        s.penFinish(false);
        const auto knotAt = [&](int sub, int knot) {
            const auto path = s.targetPath();
            return path && sub < int(path->subpaths.size()) && knot < int(path->subpaths[size_t(sub)].knots.size())
                ? std::optional<VectorPath::Knot>(path->subpaths[size_t(sub)].knots[size_t(knot)]) : std::nullopt;
        };
        if (!knotAt(0, 2)) { std::fprintf(stderr, "held keys: the Pen made no path\n"); return 1; }
        press(canvas, Qt::Key_Control, Qt::ControlModifier);
        expect(canvas->heldTool() == Tool::DirectSelect, "Ctrl with the Pen did not hold the Direct Selection tool");
        mouse(QEvent::MouseButtonPress, QPointF(200, 100), Qt::ControlModifier);
        mouse(QEvent::MouseMove, QPointF(210, 120), Qt::ControlModifier);
        mouse(QEvent::MouseButtonRelease, QPointF(210, 120), Qt::ControlModifier);
        auto moved = knotAt(0, 1);
        expect(moved && std::abs(moved->x - 210) < 0.5 && std::abs(moved->y - 120) < 0.5, "Ctrl-drag with the Pen did not move the anchor");
        expect(knotAt(0, 0) && knotAt(0, 0)->x == 100 && !s.penDraft(), "Ctrl-drag with the Pen moved more than the anchor, or added one");
        release(canvas, Qt::Key_Control, Qt::NoModifier);
        expect(!canvas->heldTool() && s.tool() == Tool::Pen, "letting go of Ctrl did not go back to the Pen");
        // A path being drawn: Ctrl-click away from it ends it open, as Photoshop's does.
        s.penPress(QPointF(300, 300));
        s.penPress(QPointF(360, 300));
        press(canvas, Qt::Key_Control, Qt::ControlModifier);
        expect(canvas->heldTool() == Tool::DirectSelect, "Ctrl while drawing a path did not hold the Direct Selection tool");
        click(QPointF(500, 60), Qt::ControlModifier);
        release(canvas, Qt::Key_Control, Qt::NoModifier);
        expect(!s.penDraft() && s.targetPath() && s.targetPath()->subpaths.size() == 2, "Ctrl-click while drawing did not end the path open");
        // Alt over an anchor with the Pen: Convert Point (the corner becomes smooth), no new anchor.
        const auto corner = knotAt(0, 1);
        click(QPointF(210, 120), Qt::AltModifier);
        const auto converted = knotAt(0, 1);
        expect(corner && converted && corner->inX == corner->x && (converted->inX != converted->x || converted->inY != converted->y),
               "Alt-click with the Pen on a corner did not make it smooth");
        expect(!s.undoNames().empty() && s.undoNames().back() == "Convert Point" && !s.penDraft() && s.targetPath()->subpaths[0].knots.size() == 3,
               "Alt-click with the Pen did not convert the point as one step");
        // Ctrl with the Direct Selection tool: Path Selection, the whole component dragged from one of its anchors.
        s.selectTool(Tool::DirectSelect);
        press(canvas, Qt::Key_Control, Qt::ControlModifier);
        expect(!canvas->heldTool(), "Ctrl with the Direct Selection tool held another tool");
        const auto first = knotAt(0, 0), last = knotAt(0, 2);
        mouse(QEvent::MouseButtonPress, QPointF(100, 100), Qt::ControlModifier);
        mouse(QEvent::MouseMove, QPointF(105, 108), Qt::ControlModifier);
        mouse(QEvent::MouseButtonRelease, QPointF(105, 108), Qt::ControlModifier);
        release(canvas, Qt::Key_Control, Qt::NoModifier);
        const auto first2 = knotAt(0, 0), last2 = knotAt(0, 2), other2 = knotAt(1, 0);
        expect(first && last && first2 && last2 && std::abs(first2->x - first->x - 5) < 0.5 && std::abs(last2->y - last->y - 8) < 0.5,
               "Ctrl-drag with the Direct Selection tool did not move the whole component");
        expect(other2 && other2->x == 300, "Ctrl-drag with the Direct Selection tool moved another component");
        s.selectTool(Tool::Brush);
    }

    // Ctrl+Space zooms in where clicked; Ctrl+Alt+Space out.
    const double zoom = s.viewport.zoom;
    press(canvas, Qt::Key_Control, Qt::ControlModifier);
    press(canvas, Qt::Key_Space, Qt::ControlModifier, QStringLiteral(" "));
    expect(canvas->heldTool() == Tool::Zoom, "Ctrl+Space did not hold the Zoom tool");
    click(QPointF(320, 210), Qt::ControlModifier);
    expect(std::abs(s.viewport.zoom - zoom * 2) < 1e-6, "Ctrl+Space click did not zoom in");
    press(canvas, Qt::Key_Alt, Qt::ControlModifier | Qt::AltModifier);
    expect(canvas->heldTool() == Tool::Zoom, "Ctrl+Alt+Space did not hold the Zoom tool");
    click(QPointF(320, 210), Qt::ControlModifier | Qt::AltModifier);
    expect(std::abs(s.viewport.zoom - zoom) < 1e-6, "Ctrl+Alt+Space click did not zoom out");
    release(canvas, Qt::Key_Alt, Qt::ControlModifier);
    release(canvas, Qt::Key_Space, Qt::ControlModifier);
    release(canvas, Qt::Key_Control, Qt::NoModifier);
    expect(!canvas->heldTool() && s.tool() == Tool::Brush, "letting go of Ctrl+Space did not go back to the Brush");

    // A tool's letter held while the tool is used springs back on release; a tap keeps the tool.
    expect(press(canvas, Qt::Key_M, Qt::NoModifier, QStringLiteral("m")), "M was not taken by the Marquee's shortcut");
    expect(s.tool() == Tool::Marquee, "M did not pick the Marquee");
    click(QPointF(40, 40), Qt::NoModifier);
    release(canvas, Qt::Key_M, Qt::NoModifier);
    expect(s.tool() == Tool::Brush, "letting go of M after using the Marquee did not go back to the Brush");
    press(canvas, Qt::Key_M, Qt::NoModifier, QStringLiteral("m"));
    release(canvas, Qt::Key_M, Qt::NoModifier);
    expect(s.tool() == Tool::Marquee, "a tap of M did not keep the Marquee");
    s.selectTool(Tool::Brush);
    // Shift+letter likewise: held while used it springs back to the tool and kind before; a tap keeps the new one.
    {
        s.marqueeKind = MarqueeKind::Rectangle;
        expect(press(canvas, Qt::Key_M, Qt::ShiftModifier, QStringLiteral("M")), "Shift+M was not taken by its shortcut");
        expect(s.tool() == Tool::Marquee && s.marqueeKind == MarqueeKind::Ellipse, "Shift+M did not pick the Elliptical Marquee");
        click(QPointF(40, 40), Qt::ShiftModifier);
        release(canvas, Qt::Key_M, Qt::ShiftModifier);
        expect(s.tool() == Tool::Brush && s.marqueeKind == MarqueeKind::Rectangle, "letting go of Shift+M after using it did not go back to the Brush");
        // On the tool itself only the kind changes, and comes back.
        s.selectTool(Tool::Marquee);
        press(canvas, Qt::Key_M, Qt::ShiftModifier, QStringLiteral("M"));
        click(QPointF(40, 40), Qt::ShiftModifier);
        release(canvas, Qt::Key_M, Qt::ShiftModifier);
        expect(s.tool() == Tool::Marquee && s.marqueeKind == MarqueeKind::Rectangle, "letting go of Shift+M on the Marquee did not go back to the Rectangular Marquee");
        s.selectTool(Tool::Brush);
        press(canvas, Qt::Key_G, Qt::ShiftModifier, QStringLiteral("G"));
        const Tool cycled = s.tool();
        release(canvas, Qt::Key_G, Qt::ShiftModifier);
        expect((cycled == Tool::Gradient || cycled == Tool::PaintBucket) && s.tool() == cycled, "a tap of Shift+G did not keep the tool it picked");
        s.selectTool(Tool::Brush);
        s.marqueeKind = MarqueeKind::Rectangle;
    }

    // F7: the Layers panel, not while a text field has the keys.
    QDockWidget* layers = w.findChild<QDockWidget*>(QStringLiteral("layersDock"));
    if (!layers) { std::fprintf(stderr, "no Layers panel\n"); return 1; }
    const bool shown = !layers->isHidden();
    press(canvas, Qt::Key_F7, Qt::NoModifier);
    expect(layers->isHidden() == shown, "F7 did not toggle the Layers panel");
    press(canvas, Qt::Key_F7, Qt::NoModifier);
    expect(layers->isHidden() != shown, "F7 again did not toggle the Layers panel back");
    {
        auto* field = new QLineEdit(&w);
        field->show();
        field->setFocus();
        QApplication::processEvents();
        if (QApplication::focusWidget() == field) {
            press(field, Qt::Key_F7, Qt::NoModifier);
            expect(layers->isHidden() != shown, "F7 in a text field toggled the Layers panel");
            const size_t layerCount = s.document()->layers.size();
            press(field, Qt::Key_Z, Qt::ControlModifier | Qt::AltModifier);
            expect(s.document()->layers.size() == layerCount, "Ctrl+Alt+Z in a text field changed the document");
        } else {
            std::fprintf(stderr, "held keys: the text field did not take the focus\n");
            failures++;
        }
        delete field;
        canvas->setFocus();
        QApplication::processEvents();
    }

    // Ctrl+Alt+Z: undo the last step, then redo it, then undo it again.
    const size_t layerCount = s.document()->layers.size();
    s.addBlankLayer();
    press(canvas, Qt::Key_Z, Qt::ControlModifier | Qt::AltModifier);
    expect(s.document()->layers.size() == layerCount, "Ctrl+Alt+Z did not undo the last step");
    press(canvas, Qt::Key_Z, Qt::ControlModifier | Qt::AltModifier);
    expect(s.document()->layers.size() == layerCount + 1, "Ctrl+Alt+Z again did not redo it");
    press(canvas, Qt::Key_Z, Qt::ControlModifier | Qt::AltModifier);
    expect(s.document()->layers.size() == layerCount, "Ctrl+Alt+Z a third time did not undo it");

    // F12: File > Revert, without a question (as Photoshop CC: it is one undo step); the saved layers come back, the
    // steps before stay, and Undo brings back the unsaved layer. Greyed while the document is as saved.
    {
        const QString path = QDir(QDir::tempPath()).filePath(QStringLiteral("nekophoto-held-keys-%1.nekophoto").arg(QCoreApplication::applicationPid()));
        QString error;
        if (!s.saveProject(path, &error)) { std::fprintf(stderr, "held keys: could not save %s: %s\n", qPrintable(path), qPrintable(error)); return 1; }
        QApplication::processEvents();
        QAction* revert = w.findChild<QAction*>(QStringLiteral("file.revert"));
        if (!revert) { std::fprintf(stderr, "held keys: no File > Revert\n"); return 1; }
        expect(!revert->isEnabled(), "File > Revert is available with nothing changed since the save");
        const size_t saved = s.document()->layers.size();
        s.addBlankLayer();
        QApplication::processEvents();
        expect(revert->isEnabled(), "File > Revert is greyed after a change");
        std::vector<std::string> steps = s.undoNames();
        bool asked = false;
        QTimer::singleShot(0, [&asked] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) { asked = true; box->reject(); }
        });
        expect(press(canvas, Qt::Key_F12, Qt::NoModifier), "F12 was not taken by File > Revert");
        QApplication::processEvents();
        expect(!asked, "Revert asked a question");
        expect(s.document()->layers.size() == saved && !s.isModified(), "F12 did not revert to the saved project");
        steps.push_back("Revert");
        expect(s.undoNames() == steps, "Revert is not one undo step after the ones before it");
        expect(!revert->isEnabled(), "File > Revert is available right after reverting");
        s.undo();
        expect(s.document()->layers.size() == saved + 1 && s.isModified(), "Undo did not bring back the document as it was before Revert");
        s.redo();
        expect(s.document()->layers.size() == saved && !s.isModified(), "Redo did not revert again");
        QFile::remove(path);
    }

    // Ctrl+Shift+> and < while typing: 2 pixels, 10 with Alt; Ctrl held while typing holds no tool.
    s.selectTool(Tool::Text);
    if (!canvas->startNewType(QPointF(100, 100), std::nullopt)) { std::fprintf(stderr, "held keys: could not start typing\n"); return 1; }
    for (QChar c : QStringLiteral("Hello")) press(canvas, 0, Qt::NoModifier, QString(c));
    press(canvas, Qt::Key_A, Qt::ControlModifier, QStringLiteral("a"));
    const double size = canvas->typeStyleAtCaret() ? canvas->typeStyleAtCaret()->fontSize : 0;
    press(canvas, Qt::Key_Control, Qt::ControlModifier);
    expect(!canvas->heldTool(), "Ctrl while typing held the Move tool");
    expect(!press(canvas, Qt::Key_Greater, Qt::ControlModifier | Qt::ShiftModifier, QStringLiteral(">")), "a shortcut took Ctrl+Shift+> while typing");
    expect(canvas->typeStyleAtCaret() && canvas->typeStyleAtCaret()->fontSize == size + 2, "Ctrl+Shift+> did not make the type 2 pixels larger");
    press(canvas, Qt::Key_Less, Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier, QStringLiteral("<"));
    expect(canvas->typeStyleAtCaret() && canvas->typeStyleAtCaret()->fontSize == size - 8, "Ctrl+Alt+Shift+< did not make the type 10 pixels smaller");
    release(canvas, Qt::Key_Control, Qt::NoModifier);
    std::printf("type size: %g -> %g\n", size, canvas->typeStyleAtCaret() ? canvas->typeStyleAtCaret()->fontSize : 0);
    canvas->commitType();
    expect(!s.document()->layers.empty() && s.activeLayer() && s.activeLayer()->text && s.activeLayer()->text->fontSize == size - 8, "the committed type lost its size");

    // Not typing, with the Move or Type tool: the same keys size every selected type layer, one undo step per press
    // (quick repeats merge into it).
    {
        const Layer* typed = s.activeLayer();
        if (!typed || !typed->text) { std::fprintf(stderr, "held keys: no type layer\n"); return 1; }
        const Uuid first = typed->id;
        LayerText words = *typed->text;
        words.text = "World";
        words.runs.clear();
        words.fontSize = 30;
        const std::optional<Uuid> second = s.addTextLayer(QPointF(100, 200), words, false);
        if (!second) { std::fprintf(stderr, "held keys: could not add a second type layer\n"); return 1; }
        s.selectLayers({first, *second}, *second);
        s.selectTool(Tool::Move);
        canvas->setFocus();
        QApplication::processEvents();
        auto sizeOf = [&](const Uuid& id) { const auto t = s.layerText(id); return t ? t->fontSize : 0.0; };
        const double a = sizeOf(first);
        const size_t steps = s.undoNames().size();
        expect(!press(canvas, Qt::Key_Greater, Qt::ControlModifier | Qt::ShiftModifier, QStringLiteral(">")), "a shortcut took Ctrl+Shift+> with the Move tool");
        expect(sizeOf(first) == a + 2 && sizeOf(*second) == 32, "Ctrl+Shift+> with the Move tool did not make both selected type layers 2 pixels larger");
        expect(s.undoNames().size() == steps + 1 && s.undoNames().back() == "Edit Text", "Ctrl+Shift+> on two layers was not one undo step");
        press(canvas, Qt::Key_Less, Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier, QStringLiteral("<"));
        expect(sizeOf(first) == std::max(1.0, a - 8) && sizeOf(*second) == 22, "Ctrl+Alt+Shift+< did not make both layers 10 pixels smaller");
        expect(s.undoNames().size() == steps + 1, "a quick second press made another undo step");
        std::printf("layer type size: %g, 30 -> %g, %g\n", a, sizeOf(first), sizeOf(*second));
        s.undo();
        expect(sizeOf(first) == a && sizeOf(*second) == 30, "one undo did not take back both presses");
        // The Type tool, nothing typed: the same, on the active layer alone.
        s.selectLayers({*second}, *second);
        s.selectTool(Tool::Text);
        press(canvas, Qt::Key_Period, Qt::ControlModifier | Qt::ShiftModifier, QStringLiteral(">"));
        expect(sizeOf(*second) == 32 && sizeOf(first) == a && !canvas->typeEditing(), "Ctrl+Shift+> with the Type tool did not size the selected layer alone");
        // Another tool leaves the keys alone.
        s.selectTool(Tool::Brush);
        press(canvas, Qt::Key_Greater, Qt::ControlModifier | Qt::ShiftModifier, QStringLiteral(">"));
        expect(sizeOf(*second) == 32, "Ctrl+Shift+> with the Brush changed the type");
    }

    std::printf("held keys: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}

/// The command registry (CommandRegistry.h): every menu item is a command with a well-formed id of its own, and every
/// key the window answers to belongs to one action (an ambiguous key runs neither).
int registry(MainWindow& w) {
    int failures = 0;
    const CommandRegistry& reg = w.commandRegistry();
    static const QRegularExpression form(QStringLiteral("^[a-z][A-Za-z0-9]*(\\.[a-z0-9][A-Za-z0-9]*)+$"));
    std::set<QString> ids;
    for (const Command* c : reg.all()) {
        if (!form.match(c->id).hasMatch()) { std::fprintf(stderr, "registry: badly formed id \"%s\"\n", qPrintable(c->id)); failures++; }
        if (!ids.insert(c->id).second) { std::fprintf(stderr, "registry: id %s twice\n", qPrintable(c->id)); failures++; }
        if (!c->action || c->action->property("commandId").toString() != c->id) { std::fprintf(stderr, "registry: %s has no action\n", qPrintable(c->id)); failures++; }
        if (c->params && c->method.isEmpty()) { std::fprintf(stderr, "registry: %s sends a request without a method\n", qPrintable(c->id)); failures++; }
    }
    // Every item of every menu, the submenus' own items too (a submenu listed as it fills, Open Recent and Apply
    // Style, holds files and presets rather than commands).
    QList<QAction*> answering;
    int items = 0;
    std::function<void(QMenu*, const QString&)> walk = [&](QMenu* menu, const QString& path) {
        if (menu->property("commandsDynamic").toBool()) return;
        emit menu->aboutToShow();
        for (QAction* a : menu->actions()) {
            if (a->isSeparator()) continue;
            const QString here = path + QStringLiteral(" > ") + plainText(a->text());
            if (QMenu* sub = a->menu()) { walk(sub, here); continue; }
            items++;
            answering << a;
            if (!reg.forAction(a)) { std::fprintf(stderr, "registry: the menu item %s has no command\n", qPrintable(here)); failures++; }
        }
    };
    for (QAction* top : w.menuBar()->actions())
        if (QMenu* menu = top->menu()) walk(menu, plainText(top->text()));
    // The keys: the menus', the window's own (the channel keys, X and D, the tool kinds) and the tool rail's.
    answering << w.actions();
    for (QToolBar* bar : w.findChildren<QToolBar*>()) answering << bar->actions();
    std::map<QString, QAction*> keys;
    for (QAction* a : answering)
        for (const QKeySequence& key : a->shortcuts()) {
            if (key.isEmpty()) continue;
            const QString text = key.toString(QKeySequence::PortableText);
            auto [at, added] = keys.emplace(text, a);
            if (!added && at->second != a) {
                std::fprintf(stderr, "registry: %s is the key of both \"%s\" and \"%s\"\n", qPrintable(text), qPrintable(plainText(at->second->text())), qPrintable(plainText(a->text())));
                failures++;
            }
        }
    // The registry's default keys, likewise.
    std::map<QString, QString> defaults;
    for (const Command* c : reg.all())
        for (const QKeySequence& key : c->shortcuts) {
            if (key.isEmpty()) continue;   // a standard key the platform leaves unset (Save As, Quit on a bare Qt)
            auto [at, added] = defaults.emplace(key.toString(QKeySequence::PortableText), c->id);
            if (!added && at->second != c->id) { std::fprintf(stderr, "registry: %s is the default key of %s and %s\n", qPrintable(at->first), qPrintable(at->second), qPrintable(c->id)); failures++; }
        }
    std::printf("command registry: %d commands, %d menu items, %d keys, %s\n", int(ids.size()), items, int(keys.size()), failures ? "FAILED" : "ok");
    return failures;
}

/// Edit > Keyboard Shortcuts: a command and a tool given other keys (which then run them, and Edit > Search and the
/// tool's tooltip show), keys refused, a key another command has taken from it only when asked, Use Default for one
/// command and for all, the changes kept in the settings and applied again from them as at a restart, an export
/// imported back, a hand-edited clash resolved, and the registry's one-action-per-key check passing throughout.
int keyboardShortcuts(MainWindow& w) {
    int failures = 0;
    auto expect = [&](bool ok, const QString& what) { if (!ok) { std::fprintf(stderr, "shortcuts: %s\n", qPrintable(what)); failures++; } };
    const CommandRegistry& reg = w.commandRegistry();
    auto keysOf = [&](const char* id) {
        const Command* c = reg.find(QString::fromLatin1(id));
        QStringList out;
        if (c && c->action) for (const QKeySequence& k : c->action->shortcuts()) out << shortcuts::portable(k);
        return out.join(QStringLiteral(" | "));
    };
    auto dialogKeys = [](const KeyboardShortcutsDialog& d, const char* id) {
        QStringList out;
        for (const QKeySequence& k : d.keys().at(QString::fromLatin1(id))) out << shortcuts::portable(k);
        return out.join(QStringLiteral(" | "));
    };
    // Held open for the whole test: the settings stay in memory even where the file cannot be written.
    QSettings held;
    held.remove(QStringLiteral("shortcuts"));
    w.applyShortcuts();
    EditorSession& s = *w.session();
    buildDemoDocument(s);
    w.show();
    w.activateWindow();
    QApplication::processEvents();
    expect(registry(w) == 0, "the defaults fail the registry's check");

    // The menu item opens the dialog.
    QAction* item = menuItem(w, {"Edit", "Keyboard Shortcuts…"});
    expect(item && shortcuts::portable(item->shortcut()) == QLatin1String("Ctrl+Alt+Shift+K"), "Edit > Keyboard Shortcuts… is not there with Alt+Shift+Ctrl+K");
    if (item) item->trigger();
    QApplication::processEvents();
    auto* dialog = w.findChild<KeyboardShortcutsDialog*>();
    if (!dialog) { std::fprintf(stderr, "shortcuts: the dialog did not open\n"); return 1; }

    // A command's key changed, a tool's letter changed, a second key added.
    dialog->select(QStringLiteral("layer.new"));
    expect(dialog->assign("layer.new", 0, QKeySequence("Ctrl+Alt+Shift+F2")), "Ctrl+Alt+Shift+F2 was not given to New Layer: " + dialog->message());
    expect(dialogKeys(*dialog, "layer.new") == QLatin1String("Ctrl+Alt+Shift+F2"), "New Layer has " + dialogKeys(*dialog, "layer.new"));
    expect(dialog->assign("tool.brush", 0, QKeySequence("Y")), "Y was not given to the Brush");
    expect(dialog->assign("layer.duplicate", 5, QKeySequence("Ctrl+Alt+Shift+F3")), "a key was not added to Duplicate Layer");
    // Refused, saying why, and nothing changed.
    for (const QKeySequence& refused : {QKeySequence(Qt::Key_Escape), QKeySequence(Qt::Key_Control), QKeySequence(Qt::Key_Return), QKeySequence(Qt::Key_Tab),
                                        QKeySequence(Qt::Key_Space), QKeySequence(Qt::Key_5), QKeySequence(Qt::Key_BracketLeft), QKeySequence(Qt::Key_Left),
                                        QKeySequence(Qt::CTRL | Qt::Key_Space), QKeySequence("Ctrl+K, Ctrl+L")}) {
        expect(!dialog->assign("layer.new", 0, refused) && !dialog->message().isEmpty(), refused.toString() + " was not refused with a message");
        expect(dialogKeys(*dialog, "layer.new") == QLatin1String("Ctrl+Alt+Shift+F2"), "a refused key changed New Layer's keys");
    }
    // A key Group Layers has: named, and moved only when taken; cancelled, nothing changes.
    expect(!dialog->assign("layer.viaCopy", 1, QKeySequence("Ctrl+G")) && dialog->conflict() == QLatin1String("layer.group"), "Ctrl+G did not say Group Layers has it");
    expect(dialog->message().contains(QStringLiteral("Group Layers")), "the conflict does not name Group Layers: " + dialog->message());
    dialog->cancelConflict();
    expect(dialogKeys(*dialog, "layer.group") == QLatin1String("Ctrl+G") && dialogKeys(*dialog, "layer.viaCopy") == QLatin1String("Ctrl+J"), "a cancelled conflict changed the keys");
    dialog->assign("layer.viaCopy", 1, QKeySequence("Ctrl+G"));
    dialog->takeConflicting();
    expect(dialogKeys(*dialog, "layer.viaCopy") == QLatin1String("Ctrl+J | Ctrl+G") && dialogKeys(*dialog, "layer.group").isEmpty(),
           "taking Ctrl+G gave Layer via Copy " + dialogKeys(*dialog, "layer.viaCopy") + " and Group Layers " + dialogKeys(*dialog, "layer.group"));
    // Use Default for Group Layers takes Ctrl+G back; then it is moved again, and a key removed.
    dialog->resetCommand(QStringLiteral("layer.group"));
    expect(dialogKeys(*dialog, "layer.group") == QLatin1String("Ctrl+G") && dialogKeys(*dialog, "layer.viaCopy") == QLatin1String("Ctrl+J"), "Use Default did not take Ctrl+G back");
    dialog->assign("layer.viaCopy", 1, QKeySequence("Ctrl+G"));
    dialog->takeConflicting();
    dialog->removeKey(QStringLiteral("edit.colorSettings"), 0);
    expect(dialogKeys(*dialog, "edit.colorSettings").isEmpty(), "Color Settings' key was not removed");
    // The search finds a command by its new key.
    dialog->setFilter(QStringLiteral("F2"));
    int shown = 0;
    if (auto* tree = dialog->findChild<QTreeWidget*>(QStringLiteral("shortcutTree")))
        for (QTreeWidgetItemIterator it(tree); *it; ++it) if (!(*it)->isHidden() && !(*it)->data(0, Qt::UserRole).toString().isEmpty()) shown++;
    expect(shown == 1, QStringLiteral("searching F2 shows %1 commands").arg(shown));
    dialog->setFilter(QString());
    const shortcuts::KeyMap wanted = dialog->overrides();
    expect(wanted.size() == 6, QStringLiteral("%1 commands changed, expected 6").arg(wanted.size()));
    dialog->accept();
    QApplication::processEvents();

    // Applied: the keys run them, the old ones do not; Search and the tooltip show the new keys; no key is two actions'.
    expect(keysOf("layer.new") == QLatin1String("Ctrl+Alt+Shift+F2") && keysOf("tool.brush") == QLatin1String("Y") && keysOf("layer.group").isEmpty()
               && keysOf("layer.viaCopy") == QLatin1String("Ctrl+J | Ctrl+G") && keysOf("layer.duplicate") == QLatin1String("Ctrl+Alt+Shift+F3") && keysOf("edit.colorSettings").isEmpty(),
           "OK did not apply the keys");
    expect(registry(w) == 0, "the person's keys fail the registry's check");
    CanvasWidget* canvas = w.canvasAt(w.currentTabIndex());
    auto press = [](QWidget* target, int key, Qt::KeyboardModifiers modifiers, const QString& text = QString()) {
        if (qt_sendShortcutOverrideEvent(target, 0, key, modifiers, text, false, 1)) return true;
        QKeyEvent e(QEvent::KeyPress, key, modifiers, text);
        QApplication::sendEvent(target, &e);
        return false;
    };
    if (canvas) {
        w.activateWindow();   // back from the dialog
        canvas->setFocus();
        QApplication::processEvents();
        const size_t layers = s.document()->layers.size();
        expect(press(canvas, Qt::Key_F2, Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier) && s.document()->layers.size() == layers + 1, "Ctrl+Alt+Shift+F2 did not make a layer");
        expect(!press(canvas, Qt::Key_N, Qt::ControlModifier | Qt::ShiftModifier, QStringLiteral("N")) && s.document()->layers.size() == layers + 1, "Ctrl+Shift+N still made a layer");
        s.selectTool(Tool::Move);
        expect(press(canvas, Qt::Key_Y, Qt::NoModifier, QStringLiteral("y")) && s.tool() == Tool::Brush && !s.brushErase, "Y did not choose the Brush");
        QKeyEvent up(QEvent::KeyRelease, Qt::Key_Y, Qt::NoModifier, QStringLiteral("y"));
        QApplication::sendEvent(canvas, &up);
        s.selectTool(Tool::Move);
        press(canvas, Qt::Key_B, Qt::NoModifier, QStringLiteral("b"));
        expect(s.tool() == Tool::Move, "B still chose the Brush");
        // Typing on the canvas still takes the letter the Brush now has.
        s.selectTool(Tool::Text);
        if (canvas->startNewType(QPointF(100, 100), std::nullopt)) {
            expect(!press(canvas, Qt::Key_Y, Qt::NoModifier, QStringLiteral("y")) && s.tool() == Tool::Text, "a shortcut took Y while typing");
            canvas->cancelType();
        } else expect(false, "could not start typing");
        s.selectTool(Tool::Move);
    }
    {
        bool searched = false, tool = false;
        for (const PaletteEntry& e : w.paletteEntries()) {
            if (e.commandId == QLatin1String("layer.new")) searched = e.shortcut == QKeySequence("Ctrl+Alt+Shift+F2").toString(QKeySequence::NativeText);
            if (e.id == QLatin1String("tool:Brush")) tool = e.shortcut == QLatin1String("Y");
        }
        expect(searched, "Edit > Search does not show New Layer's new key");
        expect(tool, "Edit > Search does not show the Brush's new key");
        for (QToolBar* bar : w.findChildren<QToolBar*>())
            for (QAction* a : bar->actions())
                if (a == reg.find(QStringLiteral("tool.brush"))->action) expect(a->toolTip() == QLatin1String("Brush (Y)"), "the Brush's tooltip is " + a->toolTip());
    }

    // Kept: the settings hold the changes (Group Layers' and Color Settings' as no key), read back by another
    // QSettings, and applied again from them after the keys went back to the defaults (as at the next start).
    {
        QSettings settings;
        settings.sync();
        if (settings.status() != QSettings::NoError) std::printf("shortcuts: the settings file could not be written here; checked in memory\n");
        expect(settings.contains(QStringLiteral("shortcuts/layer.group")) && settings.contains(QStringLiteral("shortcuts/edit.colorSettings")), "a removed key is not kept as an empty list");
    }
    {
        QSettings again;
        const shortcuts::KeyMap read = shortcuts::load(again);
        expect(shortcuts::toJson(read) == shortcuts::toJson(wanted), "the settings read back other keys:\n" + QString::fromUtf8(shortcuts::toJson(read)));
    }
    shortcuts::apply(reg, shortcuts::resolve(reg, {}));
    expect(keysOf("layer.new") == QLatin1String("Ctrl+Shift+N") && keysOf("layer.group") == QLatin1String("Ctrl+G"), "the defaults did not come back");
    w.applyShortcuts();
    expect(keysOf("layer.new") == QLatin1String("Ctrl+Alt+Shift+F2") && keysOf("layer.group").isEmpty() && keysOf("tool.brush") == QLatin1String("Y"), "the saved keys were not applied again");

    // Exported, then imported into a dialog reset to the defaults: the same changes.
    QTemporaryDir dir;
    const QString file = dir.filePath(QStringLiteral("mine.nekokeys"));
    {
        KeyboardShortcutsDialog d(reg, &w);
        QString error;
        expect(d.exportFile(file, &error), "export failed: " + error);
        d.resetAll();
        expect(d.overrides().empty(), "Use Default for All left changes");
        expect(d.importFile(file, &error), "import failed: " + error);
        expect(shortcuts::toJson(d.overrides()) == shortcuts::toJson(wanted), "the import gave other keys");
        expect(!d.importFile(QStringLiteral(":/nothing-there.nekokeys"), &error) && !error.isEmpty(), "importing a missing file did not fail");
    }

    // A hand-edited clash (New Layer given Ctrl+J, Layer via Copy's): one action keeps it, the check still passes.
    {
        QSettings settings;
        settings.setValue(QStringLiteral("shortcuts/layer.new"), QStringList{QStringLiteral("Ctrl+J")});
    }
    w.applyShortcuts();
    expect(keysOf("layer.new") == QLatin1String("Ctrl+J") && !keysOf("layer.viaCopy").contains(QStringLiteral("Ctrl+J")), "the clash left Ctrl+J on " + keysOf("layer.viaCopy"));
    expect(registry(w) == 0, "a hand-edited clash fails the registry's check");

    // Use Default for All, then OK: no changes saved, every default back.
    w.showKeyboardShortcuts();
    QApplication::processEvents();
    if (auto* d = w.findChild<KeyboardShortcutsDialog*>()) {
        d->resetAll();
        d->accept();
        QApplication::processEvents();
    } else expect(false, "the dialog did not open again");
    {
        QSettings settings;
        settings.beginGroup(QStringLiteral("shortcuts"));
        expect(settings.childKeys().isEmpty(), "Use Default for All left saved shortcuts");
    }
    expect(keysOf("layer.new") == QLatin1String("Ctrl+Shift+N") && keysOf("layer.group") == QLatin1String("Ctrl+G") && keysOf("layer.viaCopy") == QLatin1String("Ctrl+J")
               && keysOf("tool.brush") == QLatin1String("B") && keysOf("edit.colorSettings") == QLatin1String("Ctrl+Shift+K"), "Use Default for All did not bring the defaults back");
    expect(registry(w) == 0, "the defaults fail the registry's check after a reset");
    std::printf("keyboard shortcuts: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}

} // namespace app
