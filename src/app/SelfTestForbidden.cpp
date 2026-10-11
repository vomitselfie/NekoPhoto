// forbidden-cursor: the tools that paint pixels refuse a group, an adjustment layer, a hidden layer, a smart object
// and a type or shape layer (EditorSession::toolRefusal), say which in the status bar and show the forbidden cursor
// over the canvas, following the active layer, its visibility and the tool without the pointer moving. A mask takes
// the paint, an adjustment layer's too (it comes with one, targeted: painting it is one step and changes the composite
// only under the stroke); rasterizing a type layer lets the brush on, and undoing it refuses it again.
#include "SelfTest.h"
#include "CanvasWidget.h"
#include "EditorSession.h"
#include "MainWindow.h"
#include "Theme.h"
#include "compositor/render.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QStatusBar>
#include <QThread>
#include <cstdio>
#include <cstring>

using namespace compositor;

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

} // namespace

int forbiddenCursor(MainWindow& w) {
    EditorSession& s = *w.session();
    buildDemoDocument(s);
    w.resize(1200, 800);
    w.show();
    w.activateWindow();
    settle(100);
    s.fitView();
    CanvasWidget* canvas = w.canvasAt(w.currentTabIndex());
    if (!canvas) { std::fprintf(stderr, "forbidden cursor: no canvas\n"); return 1; }
    QString errors;
    int failures = 0;
    auto expect = [&](bool ok, const char* what) { if (!ok) { std::fprintf(stderr, "forbidden cursor: %s\n", what); failures++; } };
    auto call = [&](const char* method, const QJsonObject& params = {}) {
        w.setErrorSink(&errors);   // a refused request is reported here, not in a message box (each request clears it)
        return w.runCommand(QString::fromLatin1(method), params);
    };
    auto refusal = [&](Tool tool) { return s.toolRefusal(tool); };
    auto kind = [&](Tool tool, ToolRefusal::Kind k) { auto r = refusal(tool); return r && r->kind == k && !r->reason.isEmpty(); };

    // A pixel layer, shown: every tool works.
    expect(call("layers.add", {{"kind", "pixels"}, {"name", "Paint"}}).has_value(), "could not add a pixel layer");
    const auto pixels = s.activeLayerId();
    expect(call("pixels.fill", {{"color", "#c03060"}}).has_value(), "could not fill the pixel layer");
    for (Tool t : {Tool::Brush, Tool::SpotHealing, Tool::CloneStamp, Tool::Smudge, Tool::Dodge, Tool::PaintBucket, Tool::Gradient, Tool::Move, Tool::Marquee})
        expect(!refusal(t), "a tool was refused on a plain pixel layer");
    // Hidden: refused, as the brush's own check says.
    call("layers.set", {{"visible", false}});
    expect(kind(Tool::Brush, ToolRefusal::Refused) && kind(Tool::Gradient, ToolRefusal::Refused) && kind(Tool::PaintBucket, ToolRefusal::Refused), "a hidden layer was not refused");
    expect(!refusal(Tool::Marquee) && !refusal(Tool::Move), "a selection or the Move tool was refused on a hidden layer");
    call("layers.set", {{"visible", true}});
    expect(!refusal(Tool::Brush), "the brush stayed refused once the layer was shown");
    // Its mask takes the paint; Clone Stamp and Spot Healing have nothing to do there.
    s.addLayerMask(true);
    s.selectLayer(pixels, true);
    if (s.isMaskSelected()) {
        expect(!refusal(Tool::Brush) && !refusal(Tool::Gradient), "painting a mask was refused");
        expect(kind(Tool::CloneStamp, ToolRefusal::Refused) && kind(Tool::SpotHealing, ToolRefusal::Refused), "Clone Stamp on a mask was not refused");
    } else expect(false, "could not target the mask");
    s.selectLayer(pixels, false);

    // A group and an adjustment layer.
    call("layers.add", {{"kind", "group"}});
    expect(kind(Tool::Brush, ToolRefusal::Refused) && kind(Tool::Smudge, ToolRefusal::Refused), "a group was not refused");
    // An adjustment layer comes with its mask targeted, as in Photoshop: it takes what a pixel layer's mask does, and
    // stays the target when the layer is chosen again. Without a mask there is nothing to paint.
    expect(call("layers.add", {{"kind", "adjustment"}, {"adjustmentKind", "levels"}}).has_value(), "could not add an adjustment layer");
    const Layer* levels = s.activeLayer();
    expect(levels && levels->adjustment && levels->mask && s.isMaskSelected(), "a new adjustment layer did not come with its mask targeted");
    expect(!refusal(Tool::Brush) && !refusal(Tool::Gradient) && !refusal(Tool::PaintBucket), "painting an adjustment layer's mask was refused");
    expect(kind(Tool::CloneStamp, ToolRefusal::Refused) && kind(Tool::Dodge, ToolRefusal::Refused), "Clone Stamp or Dodge on an adjustment layer's mask was not refused");
    if (levels) {
        const auto levelsId = levels->id;
        s.selectLayer(pixels, false);
        s.selectLayer(levelsId, false);
        expect(s.isMaskSelected(), "choosing the adjustment layer again did not target its mask");
        s.deleteLayerMask();
        expect(kind(Tool::Brush, ToolRefusal::Refused) && kind(Tool::Gradient, ToolRefusal::Refused), "an adjustment layer without a mask was not refused");
        call("history.undo");
        expect(s.activeLayer() && s.activeLayer()->mask && s.isMaskSelected() && !refusal(Tool::Brush), "undoing Delete Layer Mask did not target the adjustment layer's mask again");
    }

    // Painting an adjustment layer's mask: one undo step, and the composite changes only where the brush went.
    s.selectLayer(pixels, false);
    s.addAdjustmentLayer(AdjustmentKind::Invert);
    const Layer* invert = s.activeLayer();
    expect(invert && invert->adjustment && invert->mask && s.isMaskSelected(), "the Invert adjustment layer did not come with its mask targeted");
    if (invert && invert->mask && s.isMaskSelected()) {
        const Document& doc = *s.document();
        const auto before = renderFlattened(doc);
        const size_t steps = s.undoNames().size();
        const Tool tool = s.tool();
        const BrushSettings brush = s.brushSettings;
        const bool white = s.maskPaintWhite, erase = s.brushErase;
        s.selectTool(Tool::Brush);
        s.brushSettings.diameter = 12; s.brushSettings.hardness = 1; s.brushSettings.opacity = 1;
        s.maskPaintWhite = false; s.brushErase = false;   // black: the adjustment is hidden under the stroke
        const double cy = doc.height / 2.0, x0 = doc.width / 4.0, x1 = doc.width / 2.0;
        const bool began = s.beginBrush(QPointF(x0, cy), false);
        expect(began, "the brush did not start on the adjustment layer's mask");
        if (began) { s.continueBrush(QPointF(x1, cy)); s.endBrush(); }
        s.brushSettings = brush; s.maskPaintWhite = white; s.brushErase = erase;
        s.selectTool(tool);
        expect(s.undoNames().size() == steps + 1 && s.undoNames().back() == "Paint Mask", "painting the adjustment layer's mask was not one Paint Mask step");
        const auto after = renderFlattened(*s.document());
        int inside = 0, outside = 0;
        for (int y = 0; y < after->height(); y++)
            for (int x = 0; x < after->width(); x++) {
                if (std::memcmp(before->pixel(x, y), after->pixel(x, y), 4) == 0) continue;
                // The stroke's reach: its line, widened by the radius and a pixel of antialiasing.
                const bool near = x >= x0 - 8 && x <= x1 + 8 && y >= cy - 8 && y <= cy + 8;
                (near ? inside : outside)++;
            }
        expect(inside > 0, "painting the adjustment layer's mask changed nothing");
        expect(outside == 0, "painting the adjustment layer's mask changed the composite away from the stroke");
        s.undo();
        const auto undone = renderFlattened(*s.document());
        bool restored = undone->width() == before->width() && undone->height() == before->height();
        for (int y = 0; restored && y < before->height(); y++)
            for (int x = 0; restored && x < before->width(); x++) restored = std::memcmp(before->pixel(x, y), undone->pixel(x, y), 4) == 0;
        expect(restored, "undoing the mask stroke did not restore the composite");
    }

    // A type layer: rasterize first. Rasterized, the brush works; undone, it is type again.
    expect(call("layers.add", {{"kind", "text"}, {"text", "Neko"}, {"x", 20}, {"y", 20}, {"size", 40}}).has_value(), "could not add a type layer");
    const auto typeId = s.activeLayerId();
    expect(kind(Tool::Brush, ToolRefusal::Rasterize) && kind(Tool::PaintBucket, ToolRefusal::Rasterize), "a type layer did not ask to be rasterized");
    expect(!refusal(Tool::Move) && !refusal(Tool::Marquee), "the Move tool or a selection was refused on a type layer");

    // The cursor over the canvas: forbidden on the type layer, the reason in the status bar, without the pointer moving
    // when the tool or the layer changes.
    s.selectTool(Tool::Brush);
    const QPoint middle = canvas->rect().center();
    QMouseEvent hover(QEvent::MouseMove, QPointF(middle), canvas->mapToGlobal(QPointF(middle)), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(canvas, &hover);
    settle(20);
    const QCursor forbidden = themedCursor(Qt::ForbiddenCursor);
    auto showsForbidden = [&] { return canvas->cursor().shape() == forbidden.shape() && canvas->cursor().pixmap().size() == forbidden.pixmap().size(); };
    auto hint = [&] {
        for (QLabel* l : w.statusBar()->findChildren<QLabel*>()) if (l->isVisible() && l->text() == canvas->refusal()) return true;
        return false;
    };
    expect(showsForbidden() && !canvas->refusal().isEmpty(), "the brush over a type layer did not show the forbidden cursor");
    expect(hint(), "the status bar did not say why");
    s.selectTool(Tool::Marquee);
    settle(20);
    expect(!showsForbidden() && canvas->refusal().isEmpty(), "the forbidden cursor stayed with the Marquee");
    s.selectTool(Tool::Brush);
    settle(20);
    expect(showsForbidden(), "the forbidden cursor did not come back with the brush");
    s.selectLayer(pixels, false);
    settle(20);
    expect(!showsForbidden() && canvas->refusal().isEmpty(), "the forbidden cursor stayed on a pixel layer");
    call("layers.set", {{"visible", false}});
    settle(20);
    expect(showsForbidden(), "hiding the layer did not show the forbidden cursor");
    call("layers.set", {{"visible", true}});
    settle(20);
    expect(!showsForbidden(), "showing the layer again kept the forbidden cursor");

    // Rasterize Type, and its undo.
    s.selectLayer(typeId, false);
    const Layer* type = s.activeLayer();
    expect(type && type->isLiveText(), "could not choose the type layer again");
    if (type && type->isLiveText()) {
        expect(s.rasterizeLayer(), "the type layer did not rasterize");
        expect(!refusal(Tool::Brush) && s.activeLayer() && !s.activeLayer()->isLiveText() && s.activeLayer()->asset, "the rasterized type layer still refuses the brush");
        call("history.undo");
        expect(kind(Tool::Brush, ToolRefusal::Rasterize), "undoing the rasterize did not make it type again");
    }

    // A shape layer and a smart object.
    expect(call("shape.draw", {{"x", 10}, {"y", 10}, {"width", 60}, {"height", 40}}).has_value(), "could not draw a shape");
    expect(kind(Tool::Brush, ToolRefusal::Rasterize), "a shape layer did not ask to be rasterized");
    s.selectLayer(pixels, false);
    if (call("smartObject.convert").has_value()) expect(kind(Tool::Brush, ToolRefusal::SmartObject) && kind(Tool::Gradient, ToolRefusal::SmartObject), "a smart object did not ask about its contents");
    else expect(false, "could not make a smart object");

    std::printf("forbidden cursor: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}

} // namespace app
