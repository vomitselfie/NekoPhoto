// menu-dump and options-dump: the interface as text, for comparing two builds when code moves without meaning to change
// anything (a menu builder split up, the options bar's pages moved to files of their own). They check nothing and pass
// whenever they run: diff the output of the build before against the one after.
// - menu-dump: every menu bar item in order, its submenus' too (separators shown), with its command id, object name,
//   label, keys, method, feature, whether it is checkable or checked, enabled, and why it is greyed (the registry's
//   reason); then every registry command in the order it was added, and the window's and toolbars' own actions. Twice:
//   on the demo document as it opens, and on a 16-bit copy (the depth greys other items, for other reasons).
// - options-dump: the options bar's widget tree for every tool and its kinds (class, object name, shown or not, enabled,
//   place and size, text, value, tooltip), at a fixed window size.
#include "SelfTestSupport.h"
#include "CanvasWidget.h"
#include "CommandRegistry.h"
#include "EditorSession.h"
#include "MainWindow.h"
#include "ToolOptionsBar.h"
#include <QAbstractButton>
#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QSlider>
#include <QToolBar>
#include <cstdio>
#include <functional>

namespace app {

namespace {

QString keysOf(const QList<QKeySequence>& keys) {
    QStringList out;
    for (const QKeySequence& k : keys) out << k.toString(QKeySequence::PortableText);
    return out.join(QStringLiteral(" | "));
}

QString flags(const QAction* a) {
    QString out;
    if (a->isCheckable()) out += a->isChecked() ? QStringLiteral(" [x]") : QStringLiteral(" [ ]");
    if (!a->isEnabled()) out += QStringLiteral(" disabled");
    if (!a->isVisible()) out += QStringLiteral(" hidden");
    return out;
}

void line(const QString& text) { std::printf("%s\n", text.toUtf8().constData()); }

void dumpAction(const CommandRegistry& reg, const QAction* a, const QString& indent) {
    const Command* c = reg.forAction(a);
    QString text = indent + a->text() + flags(a);
    if (!a->objectName().isEmpty()) text += QStringLiteral(" name=") + a->objectName();
    text += QStringLiteral(" keys=") + keysOf(a->shortcuts());
    if (c) {
        text += QStringLiteral(" id=") + c->id;
        if (!c->method.isEmpty()) text += QStringLiteral(" method=") + c->method;
        if (c->needsDocument) text += QStringLiteral(" document");
        if (!c->feature.isEmpty()) text += QStringLiteral(" feature=") + c->feature;
        const QString why = reg.disabledReason(*c);
        if (!why.isEmpty()) text += QStringLiteral(" greyed=\"") + why + QLatin1Char('"');
    }
    if (!a->toolTip().isEmpty() && a->toolTip() != a->text()) text += QStringLiteral(" tip=\"") + a->toolTip() + QLatin1Char('"');
    line(text);
}

void dumpMenu(const CommandRegistry& reg, QMenu* menu, const QString& indent) {
    emit menu->aboutToShow();
    for (QAction* a : menu->actions()) {
        if (a->isSeparator()) { line(indent + QStringLiteral("----")); continue; }
        if (QMenu* sub = a->menu()) {
            QString text = indent + QStringLiteral("> ") + a->text() + flags(a);
            if (const Command* c = reg.forAction(a)) text += QStringLiteral(" id=") + c->id;
            line(text);
            if (sub->property("commandsDynamic").toBool()) { line(indent + QStringLiteral("    (listed as it fills)")); continue; }
            dumpMenu(reg, sub, indent + QStringLiteral("    "));
            continue;
        }
        dumpAction(reg, a, indent);
    }
}

void dumpMenus(MainWindow& w, const char* state) {
    const CommandRegistry& reg = w.commandRegistry();
    line(QStringLiteral("==== menus: %1").arg(QLatin1String(state)));
    for (QAction* top : w.menuBar()->actions())
        if (QMenu* menu = top->menu()) {
            line(QStringLiteral("# ") + top->text());
            dumpMenu(reg, menu, QStringLiteral("    "));
        }
    line(QStringLiteral("==== registry: %1").arg(QLatin1String(state)));
    for (const Command* c : reg.all()) {
        QString text = c->id + QStringLiteral(" label=") + c->label + QStringLiteral(" defaults=") + keysOf(c->shortcuts) + QStringLiteral(" path=") + c->menuPath.join(QStringLiteral(" > "));
        if (!c->method.isEmpty()) text += QStringLiteral(" method=") + c->method;
        if (c->params) text += QStringLiteral(" request");
        if (!c->errorTitle.isEmpty()) text += QStringLiteral(" errorTitle=") + c->errorTitle;
        if (!c->keyContext.isEmpty()) text += QStringLiteral(" context=") + c->keyContext;
        if (c->action) text += QStringLiteral(" keys=") + keysOf(c->action->shortcuts()) + flags(c->action);
        const QString why = reg.disabledReason(*c);
        if (!why.isEmpty()) text += QStringLiteral(" greyed=\"") + why + QLatin1Char('"');
        line(text);
    }
    line(QStringLiteral("==== window actions: %1").arg(QLatin1String(state)));
    for (QAction* a : w.actions()) dumpAction(reg, a, QStringLiteral("    "));
    for (QToolBar* bar : w.findChildren<QToolBar*>()) {
        line(QStringLiteral("# toolbar ") + bar->objectName());
        for (QAction* a : bar->actions()) if (!a->isSeparator()) dumpAction(reg, a, QStringLiteral("    "));
    }
}

void dumpWidget(QWidget* widget, QWidget* root, const QString& indent) {
    QString text = indent + QString::fromLatin1(widget->metaObject()->className());
    if (!widget->objectName().isEmpty()) text += QStringLiteral(" name=") + widget->objectName();
    if (!widget->isVisibleTo(root)) text += QStringLiteral(" hidden");
    if (!widget->isEnabled()) text += QStringLiteral(" disabled");
    const QRect g = widget->geometry();
    text += QStringLiteral(" at %1,%2 %3x%4").arg(g.x()).arg(g.y()).arg(g.width()).arg(g.height());
    if (auto* b = qobject_cast<QAbstractButton*>(widget)) {
        text += QStringLiteral(" text=\"") + b->text() + QLatin1Char('"');
        if (b->isCheckable()) text += b->isChecked() ? QStringLiteral(" [x]") : QStringLiteral(" [ ]");
    } else if (auto* l = qobject_cast<QLabel*>(widget)) {
        text += QStringLiteral(" text=\"") + l->text() + QLatin1Char('"');
    } else if (auto* e = qobject_cast<QLineEdit*>(widget)) {
        text += QStringLiteral(" text=\"") + e->text() + QLatin1Char('"');
    } else if (auto* c = qobject_cast<QComboBox*>(widget)) {
        QStringList items;
        for (int i = 0; i < c->count(); i++) items << c->itemText(i);
        text += QStringLiteral(" current=%1 items=[%2]").arg(c->currentIndex()).arg(items.join(QStringLiteral(", ")));
    } else if (auto* s = qobject_cast<QAbstractSpinBox*>(widget)) {
        text += QStringLiteral(" text=\"") + s->text() + QLatin1Char('"');
    } else if (auto* s = qobject_cast<QSlider*>(widget)) {
        text += QStringLiteral(" value=%1 range=%2..%3").arg(s->value()).arg(s->minimum()).arg(s->maximum());
    }
    if (!widget->toolTip().isEmpty()) text += QStringLiteral(" tip=\"") + widget->toolTip() + QLatin1Char('"');
    line(text);
    for (QObject* child : widget->children())
        if (auto* w = qobject_cast<QWidget*>(child); w && !w->isWindow()) dumpWidget(w, root, indent + QStringLiteral("  "));
}

/// The demo document in a shown window of a fixed size.
void openDemo(MainWindow& w) {
    buildDemoDocument(*w.session());
    w.resize(1400, 900);
    w.show();
    QApplication::processEvents();
    w.session()->fitView();
    QApplication::processEvents();
}

} // namespace

int menuDump(MainWindow& w) {
    openDemo(w);
    dumpMenus(w, "demo document");
    EditorSession& s = *w.session();
    QString error;
    if (!s.convertMode(nekophoto::SampleType::U16, &error)) { std::fprintf(stderr, "menu-dump: 16 bits: %s\n", qPrintable(error)); return 1; }
    QApplication::processEvents();
    dumpMenus(w, "16-bit document");
    return 0;
}

int optionsDump(MainWindow& w) {
    openDemo(w);
    EditorSession& s = *w.session();
    ToolOptionsBar* bar = nullptr;
    for (ToolOptionsBar* b : w.findChildren<ToolOptionsBar*>()) if (b->isVisibleTo(&w)) bar = b;
    if (!bar) { std::fprintf(stderr, "options-dump: no options bar\n"); return 1; }
    auto dump = [&](const QString& state) {
        emit s.toolChanged();
        QApplication::processEvents();
        line(QStringLiteral("==== ") + state);
        dumpWidget(bar, bar, QString());
    };
    const std::vector<std::pair<Tool, const char*>> tools = {
        {Tool::Move, "move"}, {Tool::Marquee, "marquee"}, {Tool::Lasso, "lasso"}, {Tool::Wand, "wand"}, {Tool::Scribble, "quick select"},
        {Tool::Crop, "crop"}, {Tool::Brush, "brush"}, {Tool::SpotHealing, "spot healing"}, {Tool::CloneStamp, "clone stamp"},
        {Tool::Smudge, "blur"}, {Tool::Gradient, "gradient"}, {Tool::Shape, "shape"}, {Tool::Eyedropper, "eyedropper"},
        {Tool::Hand, "hand"}, {Tool::Zoom, "zoom"}, {Tool::Text, "type"}, {Tool::Dodge, "dodge"}, {Tool::PaintBucket, "paint bucket"},
        {Tool::Pen, "pen"}, {Tool::DirectSelect, "direct selection"}, {Tool::Artboard, "artboard"}, {Tool::Slice, "slice"}};
    for (const auto& [tool, name] : tools) {
        s.selectTool(tool);
        dump(QString::fromLatin1(name));
    }
    s.selectTool(Tool::Brush);
    s.brushErase = true;
    dump(QStringLiteral("eraser"));
    s.brushErase = false;
    s.selectTool(Tool::Marquee);
    s.marqueeKind = MarqueeKind::Ellipse;
    dump(QStringLiteral("elliptical marquee"));
    s.selectTool(Tool::Lasso);
    s.lassoKind = LassoKind::Polygonal;
    dump(QStringLiteral("polygonal lasso"));
    s.selectTool(Tool::SpotHealing);
    for (int mode = 1; mode <= 5; mode++) {
        s.spotHealingMode = mode;
        dump(QStringLiteral("healing mode %1").arg(mode));
    }
    s.selectTool(Tool::Smudge);
    for (BlurToolMode mode : {BlurToolMode::Blur, BlurToolMode::Smudge, BlurToolMode::Sharpen}) {
        s.blurMode = mode;
        dump(QStringLiteral("blur mode %1").arg(int(mode)));
    }
    s.selectTool(Tool::Shape);
    for (VectorShapeKind kind : {VectorShapeKind::Ellipse, VectorShapeKind::Polygon, VectorShapeKind::Line, VectorShapeKind::Custom}) {
        s.shapeTool.kind = kind;
        dump(QStringLiteral("shape kind %1").arg(int(kind)));
    }
    s.selectTool(Tool::Pen);
    s.penMode = EditorSession::PenMode::Path;
    dump(QStringLiteral("pen, path mode"));
    s.selectTool(Tool::Scribble);
    s.setQuickSelectClicks(true);
    dump(QStringLiteral("quick select, clicks"));
    // Every layer of the demo document active with the Move tool (the transform fields follow it).
    s.selectTool(Tool::Move);
    for (const nekophoto::Layer& l : s.document()->layers) {
        s.selectLayer(l.id);
        dump(QStringLiteral("move on ") + QString::fromStdString(l.name));
    }
    // A narrow window: the compact levels.
    for (int width : {1000, 800, 600}) {
        w.resize(width, 900);
        QApplication::processEvents();
        dump(QStringLiteral("move at width %1").arg(width));
    }
    return 0;
}

} // namespace app
