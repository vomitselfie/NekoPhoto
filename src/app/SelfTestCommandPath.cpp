// command-path: the converted commands (New Layer, Duplicate Layer, Merge Down, Gaussian Blur's OK, Levels' OK and
// Free Transform's commit) give the same document and the same history whether they come from the interface
// (menu actions, the dialogs, the canvas's Enter) or from automation requests, and an action recording the
// interface path holds the requests automation would send. Then the same for the menu items, dialogs and panel
// buttons converted after them (menuCommands): each through the menu bar (answering its dialog), through the
// requests the recording holds, and through the session calls the interface made before, compared as documents and
// history names, with each recorded once as the method it names. The file commands (New, Open, Import File, Save As,
// Edit Contents) through their dialogs (fileCommands), and the command registry: every menu item a command with an id
// of its own, every key one action's (registry).
//
// The menu items, dialogs and panel buttons converted after them are SelfTestMenuCommands.cpp, the file commands
// SelfTestFiles.cpp and the registry's checks SelfTestKeys.cpp.
#include "SelfTestSupport.h"
#include "ActionLibrary.h"
#include "AdjustmentEditor.h"
#include "Automation.h"
#include "CanvasWidget.h"
#include "EditorSession.h"
#include "FilterDialog.h"
#include "MainWindow.h"
#include <QApplication>
#include <QDoubleSpinBox>
#include <QJsonDocument>
#include <QKeyEvent>
#include <cstdio>

using namespace nekophoto;

namespace app {

using namespace selftest;

namespace {

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

} // namespace

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

} // namespace app
