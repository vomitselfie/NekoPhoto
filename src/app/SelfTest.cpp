// In-app checks that need the whole window (run by ctest through `nekophoto --self-test <name>`).
//
// command-path: the converted commands (New Layer, Duplicate Layer, Merge Down, Gaussian Blur's OK, Levels' OK and
// Free Transform's commit) give the same document and the same history whether they come from the interface
// (menu actions, the dialogs, the canvas's Enter) or from automation requests, and an action recording the
// interface path holds the requests automation would send.
#include "SelfTest.h"
#include "ActionLibrary.h"
#include "AdjustmentEditor.h"
#include "Automation.h"
#include "CanvasWidget.h"
#include "FilterDialog.h"
#include "MainWindow.h"
#include "Ruler.h"
#include <QApplication>
#include <QDoubleSpinBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QMenu>
#include <QMenuBar>
#include <cmath>
#include <cstdio>

using namespace compositor;

namespace app {

void buildDemoDocument(EditorSession& session);   // main.cpp

namespace {

uint64_t hashBytes(const uint8_t* data, size_t size, uint64_t h = 1469598103934665603ull) {
    for (size_t i = 0; i < size; i++) { h ^= data[i]; h *= 1099511628211ull; }
    return h;
}

/// The document as text, without its random ids: every layer's name, kind, placement, look and pixels, the
/// selection, the guides, the composite, and the history's step names.
QString describe(EditorSession& s) {
    QStringList out;
    const Document& d = *s.document();
    for (const Layer& l : d.layers) {
        uint64_t pixels = 0, mask = 0;
        if (l.asset && l.asset->image && l.asset->image.u8()) pixels = hashBytes(l.asset->image.u8()->data(), l.asset->image.u8()->byteCount());
        if (l.mask && l.mask->asset.image.u8()) mask = hashBytes(l.mask->asset.image.u8()->data(), l.mask->asset.image.u8()->byteCount());
        const Layer* parent = l.parentId ? d.find(*l.parentId) : nullptr;
        out << QString("layer %1 in %2 group %3 at %4,%5 %6x%7 rot %8 flip %9%10 opacity %11 blend %12 visible %13 pixels %14 %15x%16 mask %17")
                   .arg(QString::fromStdString(l.name), parent ? QString::fromStdString(parent->name) : QStringLiteral("-")).arg(l.isGroup)
                   .arg(l.transform.origin.x, 0, 'g', 17).arg(l.transform.origin.y, 0, 'g', 17).arg(l.transform.size.width, 0, 'g', 17).arg(l.transform.size.height, 0, 'g', 17)
                   .arg(l.transform.rotation, 0, 'g', 17).arg(l.transform.flipX).arg(l.transform.flipY).arg(l.opacity).arg(int(l.blendMode)).arg(l.visible)
                   .arg(pixels).arg(l.asset && l.asset->image ? l.asset->image.width() : 0).arg(l.asset && l.asset->image ? l.asset->image.height() : 0).arg(mask);
    }
    out << QString("selection %1").arg(d.selection.has_value());
    out << QString("guides %1").arg(d.guides.size());
    if (auto flat = s.flattened()) out << QString("composite %1").arg(hashBytes(flat->data(), flat->byteCount()));
    QStringList history;
    for (const std::string& n : s.undoNames()) history << QString::fromStdString(n);
    out << "history " + history.join(" | ");
    return out.join('\n');
}

bool select(EditorSession& s, const char* name) {
    for (const Layer& l : s.document()->layers) if (l.name == name) { s.selectLayer(l.id); return true; }
    std::fprintf(stderr, "no layer named %s\n", name);
    return false;
}

QAction* action(MainWindow& w, const char* name) {
    QAction* a = w.findChild<QAction*>(QString::fromLatin1(name));
    if (!a) std::fprintf(stderr, "no action %s\n", name);
    return a;
}

AdjustmentSettings levelsSettings() {
    AdjustmentSettings levels = AdjustmentSettings::defaults(AdjustmentKind::Levels);
    levels.levels.ranges[0].black = 18;
    levels.levels.ranges[0].gamma = 1.3;
    levels.levels.ranges[0].white = 230;
    return levels;
}

constexpr double blurRadius = 3.5;
const LayerTransform moved(const LayerTransform& t) {
    LayerTransform out = t;
    out.origin.x += 13;
    out.origin.y -= 7;
    out.rotation = 15;
    return out;
}

int commandPath(MainWindow& w) {
    AutomationServer* engine = w.automationEngine();
    auto call = [engine](const QString& method, const QJsonObject& params = {}) {
        const QJsonObject reply = engine->handle(QJsonObject{{"jsonrpc", "2.0"}, {"id", 1}, {"method", method}, {"params", params}});
        if (reply.contains("error")) std::fprintf(stderr, "%s: %s\n", qPrintable(method), qPrintable(reply.value("error").toObject().value("message").toString()));
        return reply.value("result");
    };

    // The interface, recorded as an action.
    EditorSession& a = *w.session();
    buildDemoDocument(a);
    // Free Transform with a selection moves the selected pixels; these steps are about the layer.
    a.deselect();
    ActionLibrary::instance().startRecording(QStringLiteral("command-path self-test"));
    if (!select(a, "Paint")) return 1;
    QAction* add = action(w, "command.layers.add");
    QAction* duplicate = action(w, "command.layers.duplicate");
    QAction* merge = action(w, "command.layers.merge");
    QAction* transform = action(w, "command.transform");
    if (!add || !duplicate || !merge || !transform) return 1;
    add->trigger();
    if (!select(a, "Red")) return 1;
    duplicate->trigger();
    merge->trigger();
    if (!select(a, "Background")) return 1;
    {
        auto* blur = new FilterDialog(&a, FilterKind::GaussianBlur, &w);
        blur->findChild<QDoubleSpinBox*>()->setValue(blurRadius);
        blur->accept();
    }
    {
        auto* levels = new PixelAdjustmentDialog(&a, AdjustmentKind::Levels, &w);
        levels->findChild<AdjustmentEditor*>()->setSettings(levelsSettings());
        levels->accept();
    }
    if (!select(a, "Paint")) return 1;
    transform->trigger();
    if (!a.transformEdit()) { std::fprintf(stderr, "Free Transform did not start\n"); return 1; }
    a.previewTransform(moved(a.transformEdit()->draft));
    {
        CanvasWidget* canvas = w.canvasAt(w.currentTabIndex());
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(canvas, &enter);
    }
    QApplication::processEvents();
    ActionLibrary::instance().stopRecording();
    QStringList recorded;
    if (const RecordedAction* r = ActionLibrary::instance().find(QStringLiteral("command-path self-test")))
        for (const ActionStep& step : r->steps) recorded << step.method;
    ActionLibrary::instance().remove(QStringLiteral("command-path self-test"));
    const QString viaInterface = describe(a);

    // The same through automation, in a tab of its own.
    w.newTab();
    EditorSession& b = *w.session();
    buildDemoDocument(b);
    b.deselect();
    select(b, "Paint");
    call("layers.add");
    select(b, "Red");
    call("layers.duplicate");
    call("layers.merge");
    select(b, "Background");
    call("pixels.filter", {{"kind", "Gaussian Blur"}, {"radius", blurRadius}});
    call("pixels.adjust", {{"kind", "Levels"}, {"settings", QJsonDocument::fromJson(QByteArray::fromStdString(levelsSettings().toJson())).object()}});
    select(b, "Paint");
    const LayerTransform target = moved(b.activeLayer()->transform);
    call("layers.setTransform", {{"x", target.origin.x}, {"y", target.origin.y}, {"width", target.size.width}, {"height", target.size.height}, {"rotation", target.rotation}});
    const QString viaAutomation = describe(b);

    // And as the interface did it before the command path: the session's own calls, the dialogs committing their
    // own result, the transform committed directly. Behaviour must not have changed.
    w.newTab();
    EditorSession& c = *w.session();
    c.commandReady = [] { return false; };
    buildDemoDocument(c);
    c.deselect();
    select(c, "Paint");
    c.addBlankLayer();
    select(c, "Red");
    c.duplicateActiveLayer();
    c.mergeLayers();
    select(c, "Background");
    {
        auto* blur = new FilterDialog(&c, FilterKind::GaussianBlur, &w);
        blur->findChild<QDoubleSpinBox*>()->setValue(blurRadius);
        blur->accept();
    }
    {
        auto* levels = new PixelAdjustmentDialog(&c, AdjustmentKind::Levels, &w);
        levels->findChild<AdjustmentEditor*>()->setSettings(levelsSettings());
        levels->accept();
    }
    select(c, "Paint");
    c.transformCommand();
    if (c.transformEdit()) c.previewTransform(moved(c.transformEdit()->draft));
    c.commitTransformCommand();
    const QString direct = describe(c);

    int failures = 0;
    if (viaInterface != viaAutomation) {
        std::fprintf(stderr, "the interface and automation differ:\n--- interface\n%s\n--- automation\n%s\n", qPrintable(viaInterface), qPrintable(viaAutomation));
        failures++;
    }
    if (viaInterface != direct) {
        std::fprintf(stderr, "the command path changed what the interface does:\n--- now\n%s\n--- before\n%s\n", qPrintable(viaInterface), qPrintable(direct));
        failures++;
    }
    const QStringList expected{"layers.add", "layers.duplicate", "layers.merge", "pixels.filter", "pixels.adjust", "layers.setTransform"};
    if (recorded != expected) {
        std::fprintf(stderr, "recorded %s, expected %s\n", qPrintable(recorded.join(", ")), qPrintable(expected.join(", ")));
        failures++;
    }
    // Every step happened: six named steps after the demo's.
    for (const char* name : {"New Blank Layer", "Duplicate Layer", "Merge Down", "Gaussian Blur", "Levels", "Transform Layer"})
        if (!viaInterface.contains(QString::fromLatin1(name))) { std::fprintf(stderr, "no \"%s\" step in the history\n", name); failures++; }
    std::printf("command-path: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}

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

} // namespace

int runSelfTest(MainWindow& window, const QString& name) {
    if (name == QLatin1String("command-path")) return commandPath(window);
    if (name == QLatin1String("guides")) return guides(window);
    if (name == QLatin1String("canvas-menus")) return canvasMenus(window);
    std::fprintf(stderr, "unknown self-test %s (command-path, guides, canvas-menus)\n", qPrintable(name));
    return 2;
}

} // namespace app
