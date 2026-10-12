// guides, canvas-menus, search and preferences: ruler guides through the pointer, the canvas's context menu,
// Edit > Search and Edit > Preferences.
#include "SelfTestSupport.h"
#include "Autosave.h"
#include "CanvasWidget.h"
#include "CommandPalette.h"
#include "EditorSession.h"
#include "MainWindow.h"
#include "PreferencesDialog.h"
#include "Ruler.h"
#include <QApplication>
#include <QDir>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QSpinBox>
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

using namespace compositor;

namespace app {

using namespace selftest;

/// Ruler guides through the pointer: one pulled out of the top ruler, moved with the Move tool, dragged off the
/// canvas; each an undo step named as Photoshop names it, and undone again.
int guides(MainWindow& w) {
    EditorSession& s = *w.session();
    buildDemoDocument(s);
    CanvasWidget* canvas = w.canvasAt(w.currentTabIndex());
    Ruler* top = nullptr;
    for (Ruler* r : w.findChildren<Ruler*>()) if (r->height() == Ruler::thickness && r->isVisible()) top = r;
    if (!canvas || !top) { std::fprintf(stderr, "no canvas or ruler\n"); return 1; }
    int failures = 0;
    auto expect = [&](bool ok, const char* what) { if (!ok) { std::fprintf(stderr, "%s\n", what); failures++; } };
    auto mouse = [](QWidget* target, QEvent::Type type, QPointF local, Qt::KeyboardModifiers modifiers = Qt::ControlModifier) {
        // Ctrl keeps snapping out of it, so the positions are exact.
        QMouseEvent e(type, local, target->mapToGlobal(local), Qt::LeftButton, type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton, modifiers);
        QApplication::sendEvent(target, &e);
    };
    const QPointF at = canvas->viewPointForTest(QPointF(100, 150));
    // Out of the top ruler and down onto the canvas.
    const QPointF onRuler(at.x(), Ruler::thickness / 2.0);
    mouse(top, QEvent::MouseButtonPress, onRuler);
    mouse(top, QEvent::MouseMove, top->mapFromGlobal(canvas->mapToGlobal(at)));
    mouse(top, QEvent::MouseButtonRelease, top->mapFromGlobal(canvas->mapToGlobal(at)));
    // Within a pixel: the ruler hands the canvas whole screen points.
    expect(s.guides().size() == 1 && !s.guides()[0].vertical() && std::abs(s.guides()[0].position - 150) <= 1, "the ruler did not make a horizontal guide at 150");
    if (!s.guides().empty()) std::printf("ruler guide at %g\n", s.guides()[0].position);
    expect(!s.undoNames().empty() && s.undoNames().back() == "New Guide", "no New Guide step");
    // The Move tool picks it up and moves it.
    s.selectTool(Tool::Move);
    const QPointF below = canvas->viewPointForTest(QPointF(100, 200));
    mouse(canvas, QEvent::MouseButtonPress, at);
    mouse(canvas, QEvent::MouseMove, below);
    mouse(canvas, QEvent::MouseButtonRelease, below);
    expect(s.guides().size() == 1 && s.guides()[0].position == 200, "the Move tool did not move the guide to 200");
    expect(s.undoNames().back() == "Move Guide", "no Move Guide step");
    // Dragged off the canvas (onto the ruler), it goes.
    mouse(canvas, QEvent::MouseButtonPress, below);
    mouse(canvas, QEvent::MouseMove, QPointF(below.x(), -5));
    mouse(canvas, QEvent::MouseButtonRelease, QPointF(below.x(), -5));
    expect(s.guides().empty(), "the guide dragged off the canvas is still there");
    expect(s.undoNames().back() == "Delete Guide", "no Delete Guide step");
    for (int i = 0; i < 3; i++) s.undo();
    expect(s.guides().empty() && s.canRedo(), "undo did not take the three steps back");
    s.redo();
    expect(s.guides().size() == 1, "redo did not bring the guide back");
    std::printf("guides: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}

/// The canvas's context menu offers what the tool and the point allow, and leaves the rest out.
int canvasMenus(MainWindow& w) {
    EditorSession& s = *w.session();
    buildDemoDocument(s);
    CanvasWidget* canvas = w.canvasAt(w.currentTabIndex());
    int failures = 0;
    auto texts = [&](QPointF doc) {
        QMenu* menu = w.buildCanvasMenu(canvas->viewPointForTest(doc), &w);
        QStringList out;
        for (QAction* a : menu->actions()) if (!a->isSeparator()) out << a->text().remove('&').section('\t', 0, 0);
        delete menu;
        return out;
    };
    auto expect = [&](const QStringList& menu, const QStringList& present, const QStringList& absent, const char* where) {
        for (const QString& t : present) if (!menu.contains(t)) { std::fprintf(stderr, "%s: no \"%s\" in [%s]\n", where, qPrintable(t), qPrintable(menu.join(", "))); failures++; }
        for (const QString& t : absent) if (menu.contains(t)) { std::fprintf(stderr, "%s: \"%s\" should not be there\n", where, qPrintable(t)); failures++; }
    };
    select(s, "Background");
    s.selectTool(Tool::Move);
    expect(texts(QPointF(20, 20)), {"Background", "Free Transform", "Duplicate Layer", "Add Layer Mask"}, {"Deselect"}, "Move tool");
    s.selectTool(Tool::Marquee);
    s.selectAll();
    expect(texts(QPointF(20, 20)), {"Deselect", "Select Inverse", "Feather…", "Layer via Copy"}, {"Select All", "Background"}, "Marquee with a selection");
    s.deselect();
    expect(texts(QPointF(20, 20)), {"Select All", "Reselect"}, {"Deselect", "Select Inverse"}, "Marquee without a selection");
    // An open path: anchors, a segment, and the path's own commands.
    s.selectTool(Tool::Pen);
    s.penMode = EditorSession::PenMode::Path;
    s.penPress(QPointF(40, 40));
    s.penPress(QPointF(200, 40));
    s.penPress(QPointF(200, 200));
    expect(texts(QPointF(100, 100)), {"Close Path", "End Path"}, {}, "Pen drawing");
    s.penFinish(false);
    s.selectTool(Tool::DirectSelect);
    expect(texts(QPointF(200, 40)), {"Delete Anchor Point", "Convert Point", "Close Path", "Make Selection", "Fill Path", "Stroke Path", "Delete Path"}, {"Add Anchor Point"}, "Direct Selection on an anchor");
    expect(texts(QPointF(120, 40)), {"Add Anchor Point"}, {"Delete Anchor Point"}, "Direct Selection on a segment");
    // A transform in progress.
    s.selectTool(Tool::Move);
    select(s, "Paint");
    s.transformCommand();
    expect(texts(QPointF(20, 20)), {"Distort", "Rotate 180°", "Flip Horizontal", "Apply Transform", "Cancel Transform"}, {"Duplicate Layer"}, "Free Transform");
    s.cancelTransform();
    std::printf("canvas-menus: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}

/// Edit > Search…: Ctrl+F opens the palette, typed queries put the expected command first, Enter runs it and it
/// joins Recently Used.
int search(MainWindow& w) {
    int failures = 0;
    w.show();
    QApplication::processEvents();
    QAction* searchAction = action(w, "edit.search");
    if (!searchAction || searchAction->shortcut() != QKeySequence("Ctrl+F")) { std::fprintf(stderr, "Edit > Search has no Ctrl+F\n"); return 1; }
    auto open = [&]() -> CommandPalette* {
        searchAction->trigger();
        QApplication::processEvents();
        for (CommandPalette* p : w.findChildren<CommandPalette*>()) if (p->isVisible()) return p;   // a closed one waits for deletion
        return static_cast<CommandPalette*>(nullptr);
    };
    auto type = [](CommandPalette* p, const QString& text) {
        auto* field = p->findChild<QLineEdit*>(QStringLiteral("commandPaletteField"));
        field->clear();
        for (QChar c : text) {
            QKeyEvent press(QEvent::KeyPress, 0, Qt::NoModifier, QString(c));
            QApplication::sendEvent(field, &press);
        }
        QApplication::processEvents();
    };
    {
        // Undo is greyed with nothing to undo, and says so; its entry carries the registry's id.
        CommandPalette* p = open();
        if (!p) { std::fprintf(stderr, "Ctrl+F opened no palette\n"); return 1; }
        type(p, QStringLiteral("undo"));
        const PaletteEntry* first = p->resultEntry(0);
        if (!first || first->commandId != QLatin1String("edit.undo")) { std::fprintf(stderr, "search \"undo\": the first row is not edit.undo\n"); failures++; }
        else if (!w.session()->canUndo() && !p->rowText(0, 1).contains(QLatin1String("Nothing to undo"))) {
            std::fprintf(stderr, "search \"undo\": the greyed row says \"%s\", not why\n", qPrintable(p->rowText(0, 1)));
            failures++;
        }
        p->close();
        QApplication::processEvents();
        // A command that needs a document, with none open, says so.
        if (!w.session()->hasDocument()) {
            p = open();
            type(p, QStringLiteral("gauss"));
            if (!p->rowText(0, 1).contains(QLatin1String("No document is open"))) { std::fprintf(stderr, "search \"gauss\": \"%s\"\n", qPrintable(p->rowText(0, 1))); failures++; }
            p->close();
            QApplication::processEvents();
        }
    }
    const std::vector<std::pair<QString, QString>> cases = {
        {"gauss", "Gaussian Blur…"}, {"merge vis", "Merge Visible"}, {"sel inv", "Inverse"}, {"prefer", "Preferences…"},
        {"lasso", "Lasso"}};
    for (const auto& [query, expected] : cases) {
        CommandPalette* p = open();
        if (!p) { std::fprintf(stderr, "Ctrl+F opened no palette\n"); return 1; }
        type(p, query);
        const QStringList rows = p->resultLabels();
        if (rows.isEmpty() || rows.first() != expected) {
            std::fprintf(stderr, "search \"%s\": expected \"%s\" first, got \"%s\"\n", qPrintable(query), qPrintable(expected), qPrintable(rows.value(0)));
            failures++;
        }
        p->close();
        QApplication::processEvents();
    }
    // Enter runs the top row: "lasso" picks the Lasso tool, which then leads Recently Used.
    CommandPalette* p = open();
    type(p, QStringLiteral("lasso"));
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(p->findChild<QLineEdit*>(QStringLiteral("commandPaletteField")), &enter);
    QApplication::processEvents();
    QApplication::processEvents();
    if (w.session()->tool() != Tool::Lasso) { std::fprintf(stderr, "Enter on \"lasso\" did not pick the Lasso tool\n"); failures++; }
    if (CommandPalette::recent().value(0) != QLatin1String("tool:Lasso")) { std::fprintf(stderr, "recent: %s\n", qPrintable(CommandPalette::recent().join(", "))); failures++; }
    p = open();
    if (!p || p->resultLabels().value(0) != QLatin1String("Lasso")) { std::fprintf(stderr, "Recently Used does not lead the empty query\n"); failures++; }
    if (p) p->close();
    std::printf("search: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}

/// Edit > Preferences: the autosave interval applies as it is changed (Off stops it, a number starts it again) rather
/// than when the dialog closes, and the settings that apply only at the next launch say so beside their controls.
int preferences(MainWindow& w) {
    int failures = 0;
    auto fail = [&](const char* what) { std::fprintf(stderr, "preferences: %s\n", what); failures++; };
    // This run's own recovery folder only: nothing left by another run is offered back (which would wait on a dialog).
    QDir(Autosave::root()).removeRecursively();
    w.enableAutosave();
    Autosave* autosave = w.autosave();
    if (!autosave) { fail("no autosaver"); return failures; }
    QAction* item = menuItem(w, {"Edit", "Preferences…"});
    if (!item) return 1;
    item->trigger();
    QApplication::processEvents();
    PreferencesDialog* dialog = w.findChild<PreferencesDialog*>();
    if (!dialog) { fail("the dialog did not open"); return failures; }
    auto* minutes = dialog->findChild<QSpinBox*>("autosaveMinutes");
    if (!minutes) { fail("no autosave interval"); dialog->close(); return failures; }
    minutes->setValue(0);
    if (autosave->running()) fail("Off left autosave running while the dialog is open");
    minutes->setValue(7);
    if (!autosave->running()) fail("turning autosave on did not start it while the dialog is open");
    else if (autosave->intervalMs() != 7 * 60 * 1000) fail("the new interval did not apply while the dialog is open");
    minutes->setValue(2);
    if (autosave->intervalMs() != 2 * 60 * 1000) fail("a changed interval did not apply while the dialog is open");
    // Language, CPU power and Automation apply at the next launch, and each says so where it is set.
    const QString nextLaunch = PreferencesDialog::tr("Takes effect the next time NekoPhoto starts.");
    int saying = 0;
    for (QLabel* label : dialog->findChildren<QLabel*>()) if (label->text().contains(nextLaunch)) saying++;
    if (saying < 3) { std::fprintf(stderr, "preferences: %d hints say the setting applies at the next launch, expected 3\n", saying); failures++; }
    dialog->close();
    QApplication::processEvents();
    autosave->finish();
    std::printf("preferences: %s\n", failures ? "FAILED" : "ok");
    return failures;
}

} // namespace app
