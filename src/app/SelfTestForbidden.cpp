// forbidden-cursor: the tools that paint pixels refuse a group, an adjustment layer, a hidden layer, a smart object
// and a type or shape layer (EditorSession::toolRefusal), say which in the status bar and show the forbidden cursor
// over the canvas, following the active layer, its visibility and the tool without the pointer moving. A mask takes
// the paint, an adjustment layer's too (it comes with one, targeted: painting it is one step and changes the composite
// only under the stroke); rasterizing a type layer lets the brush on, and undoing it refuses it again. Rasterize Shape
// bakes the outline and drops the vector mask, the composite unchanged (on Patchy's Photoshop shapes too).
#include "SelfTest.h"
#include "CanvasWidget.h"
#include "ColorSwatches.h"
#include "EditorSession.h"
#include "MainWindow.h"
#include "Theme.h"
#include "compositor/psd.h"
#include "compositor/render.h"
#include "compositor/vectorlayer.h"
#include "compositor/vectormask.h"
#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QStatusBar>
#include <QThread>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
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

/// A layer's mask at a document point, 0..255 (an 8-bit document's); -1 without one.
int maskAt(const Layer& layer, double x, double y) {
    const GrayImage* g = layer.mask ? layer.mask->asset.image.u8().get() : nullptr;
    if (!g) return -1;
    const Point p = layer.maskTransform().pixelToDocument(g->width(), g->height()).inverted().apply({x, y});
    return g->at(std::clamp(int(std::floor(p.x)), 0, g->width() - 1), std::clamp(int(std::floor(p.y)), 0, g->height() - 1));
}

/// The largest difference of any channel between two renders (256 when their sizes differ).
int maxDifference(const Image& a, const Image& b) {
    if (a.width() != b.width() || a.height() != b.height()) return 256;
    int most = 0;
    for (int y = 0; y < a.height(); y++)
        for (int x = 0; x < a.width(); x++)
            for (int c = 0; c < 4; c++) most = std::max(most, std::abs(int(a.pixel(x, y)[c]) - int(b.pixel(x, y)[c])));
    return most;
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
        const QColor foreground = s.foregroundColor;
        const bool erase = s.brushErase;
        s.selectTool(Tool::Brush);
        s.brushSettings.diameter = 12; s.brushSettings.hardness = 1; s.brushSettings.opacity = 1;
        s.foregroundColor = Qt::black; s.brushErase = false;   // black: the adjustment is hidden under the stroke
        const double cy = doc.height / 2.0, x0 = doc.width / 4.0, x1 = doc.width / 2.0;
        const bool began = s.beginBrush(QPointF(x0, cy), false);
        expect(began, "the brush did not start on the adjustment layer's mask");
        if (began) { s.continueBrush(QPointF(x1, cy)); s.endBrush(); }
        s.brushSettings = brush; s.foregroundColor = foreground; s.brushErase = erase;
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

    // Add Layer Mask with a selection: Photoshop's button and Layer > Layer Mask > Reveal Selection make it white inside
    // the selection and black outside; Hide Selection the reverse; Reveal All and Hide All ignore it. A new adjustment
    // layer's mask is made the same way. The selection goes into the mask.
    {
        expect(call("layers.add", {{"kind", "pixels"}, {"name", "Masked"}}).has_value(), "could not add a layer to mask");
        const auto maskedId = s.activeLayerId();
        call("pixels.fill", {{"color", "#20a040"}});
        const Document& doc = *s.document();
        const double inX = doc.width * 0.25, outX = doc.width * 0.75, y = doc.height * 0.5;
        auto select = [&] { call("selection.rect", {{"x", 0}, {"y", 0}, {"width", doc.width / 2}, {"height", doc.height}}); };
        auto masked = [&] { return maskedId ? s.document()->find(*maskedId) : nullptr; };
        struct Case { QJsonObject params; int inside, outside; const char* what; };
        const Case cases[] = {
            {{{"action", "addFromSelection"}}, 255, 0, "addFromSelection did not reveal the selection"},
            {{{"action", "add"}, {"mode", "revealSelection"}}, 255, 0, "Reveal Selection did not reveal the selection"},
            {{{"action", "add"}, {"mode", "hideSelection"}}, 0, 255, "Hide Selection did not hide the selection"},
            {{{"action", "addFromSelection"}, {"revealing", false}}, 0, 255, "addFromSelection with revealing false did not hide the selection"},
            {{{"action", "add"}, {"mode", "revealAll"}}, 255, 255, "Reveal All with a selection was not all white"},
            {{{"action", "add"}, {"mode", "hideAll"}}, 0, 0, "Hide All with a selection was not all black"},
        };
        for (const Case& c : cases) {
            select();
            call("layers.mask", c.params);
            const Layer* l = masked();
            expect(l && maskAt(*l, inX, y) == c.inside && maskAt(*l, outX, y) == c.outside, c.what);
            call("history.undo");
            call("selection.none");
        }
        // The Layers panel's mask button and the menu items reach the same requests (MainWindow's menus and the panel).
        select();
        QAction* reveal = nullptr;
        for (QAction* a : w.findChildren<QAction*>()) if (a->text() == QLatin1String("Reveal Selection")) reveal = a;
        expect(reveal && reveal->isEnabled(), "no Layer > Layer Mask > Reveal Selection, or it is greyed with a selection");
        if (reveal) reveal->trigger();
        expect(masked() && maskAt(*masked(), inX, y) == 255 && maskAt(*masked(), outX, y) == 0 && !s.document()->selection,
               "Layer > Layer Mask > Reveal Selection did not reveal the selection and take it");
        call("history.undo");
        call("selection.none");
        select();
        s.addAdjustmentLayer(AdjustmentKind::Invert);
        const Layer* adjustment = s.activeLayer();
        expect(adjustment && adjustment->adjustment && adjustment->mask && maskAt(*adjustment, inX, y) == 255 && maskAt(*adjustment, outX, y) == 0,
               "a new adjustment layer's mask did not reveal the selection");
        call("history.undo");
        call("selection.none");
        if (maskedId) s.selectLayer(*maskedId, false);
    }

    // Painting a mask, as in Photoshop: the colours paint their grey (0.30 R + 0.59 G + 0.11 B), the brush the
    // foreground's, the eraser the background's, the Gradient tool both, the Paint Bucket and Fill the colour asked
    // for; the swatches show the greys while the mask is the target, and D makes them white on black there.
    {
        expect(call("layers.add", {{"kind", "pixels"}, {"name", "Grey mask"}}).has_value(), "could not add a layer for the grey mask");
        const auto greyId = s.activeLayerId();
        call("pixels.fill", {{"color", "#2040c0"}});
        call("layers.mask", {{"action", "add"}, {"mode", "revealAll"}});
        const Document& doc = *s.document();
        const double y = doc.height * 0.5;
        auto mask = [&](double x) { const Layer* l = greyId ? s.document()->find(*greyId) : nullptr; return l ? maskAt(*l, x, y) : -1; };
        auto near = [](int v, int want) { return std::abs(v - want) <= 1; };
        const QColor red(255, 0, 0), green(0, 255, 0), blue(0, 0, 255);
        const int redGrey = 77, greenGrey = 150, blueGrey = 28;   // 0.30, 0.59, 0.11 of 255
        expect(s.paintsMask(), "adding a mask did not target it");
        const QColor foreground = s.foregroundColor, background = s.backgroundColor;
        const Tool tool = s.tool();
        const BrushSettings brush = s.brushSettings;
        const bool erase = s.brushErase;
        s.selectTool(Tool::Brush);
        s.brushSettings.diameter = 9; s.brushSettings.hardness = 1; s.brushSettings.opacity = 1;
        auto stroke = [&](double x) {
            if (s.beginBrush(QPointF(x, y), false)) { s.continueBrush(QPointF(x + 1, y)); s.endBrush(); }
        };
        // The swatches: greys on the mask, the colours themselves elsewhere.
        s.foregroundColor = red; s.backgroundColor = blue;
        emit s.toolChanged();
        auto* swatches = w.findChild<ColorSwatches*>();
        auto grey = [](const QColor& c) { const int v = c.red(); return c.green() == v && c.blue() == v ? v : -1; };
        expect(swatches && near(grey(swatches->foreground()), redGrey) && near(grey(swatches->background()), blueGrey),
               "the swatches did not show the colours' greys on a mask");
        const double x0 = doc.width * 0.2, x1 = doc.width * 0.4, x2 = doc.width * 0.6, x3 = doc.width * 0.8;
        stroke(x0);
        expect(near(mask(x0), redGrey), "the brush did not paint the foreground's grey on the mask");
        s.brushErase = true;
        stroke(x1);
        expect(near(mask(x1), blueGrey), "the eraser did not erase the mask to the background's grey");
        s.brushErase = false;
        s.foregroundColor = green;
        call("selection.rect", {{"x", x2 - 4}, {"y", y - 4}, {"width", 8}, {"height", 8}});
        s.fillSelection(s.foregroundColor);
        expect(near(mask(x2), greenGrey), "Fill did not fill the mask with the colour's grey");
        call("selection.none");
        call("pixels.bucket", {{"x", x3}, {"y", 2}, {"color", "#ff0000"}, {"antialias", false}});
        expect(near(mask(x3), redGrey), "the Paint Bucket did not fill the mask with the colour's grey");
        call("gradient.draw", {{"x0", x1}, {"y0", 0}, {"x1", x2}, {"y1", 0}, {"foreground", "#ff0000"}, {"background", "#00ff00"}, {"style", "foreground-to-background"}});
        expect(near(mask(x0), redGrey) && near(mask(x3), greenGrey), "the Gradient tool did not draw from the foreground's grey to the background's on the mask");
        // D on a mask: white on black, as in Photoshop; elsewhere black on white.
        QAction* defaults = nullptr;
        for (QAction* a : w.findChildren<QAction*>()) if (a->objectName() == QLatin1String("command.colors.default")) defaults = a;
        expect(defaults != nullptr, "no Default Colours command");
        if (defaults) {
            defaults->trigger();
            expect(s.foregroundColor == QColor(Qt::white) && s.backgroundColor == QColor(Qt::black), "D on a mask did not make white the foreground and black the background");
            if (greyId) s.selectLayer(*greyId, false);
            settle(5);
            expect(swatches && swatches->foreground() == QColor(Qt::white), "the swatches did not show the colours again off the mask");
            defaults->trigger();
            expect(s.foregroundColor == QColor(Qt::black) && s.backgroundColor == QColor(Qt::white), "D off a mask did not make black the foreground");
        }
        s.foregroundColor = foreground; s.backgroundColor = background;
        s.brushSettings = brush; s.brushErase = erase;
        s.selectTool(tool);
        emit s.toolChanged();
    }

    // A group's mask takes what a layer's does, as in Photoshop: the brushes and eraser, Blur, the Paint Bucket, Fill
    // and the Gradient tool. Without a mask (or with the group itself the target) a group is refused.
    {
        expect(call("layers.add", {{"kind", "group"}, {"name", "Masked group"}}).has_value(), "could not add a group");
        const auto groupId = s.activeLayerId();
        expect(kind(Tool::Brush, ToolRefusal::Refused), "a group without a mask was not refused");
        call("layers.mask", {{"action", "add"}, {"mode", "revealAll"}});
        expect(s.isMaskSelected() && s.paintsMask(), "adding a group's mask did not target it");
        for (Tool t : {Tool::Brush, Tool::Gradient, Tool::PaintBucket})
            expect(!refusal(t), "a tool was refused on a group's mask");
        const BlurToolMode blur = s.blurMode;
        s.blurMode = BlurToolMode::Blur;
        expect(!refusal(Tool::Smudge), "Blur was refused on a group's mask");
        s.blurMode = blur;
        expect(kind(Tool::CloneStamp, ToolRefusal::Refused), "Clone Stamp on a group's mask was not refused");
        const Document& doc = *s.document();
        const double x = doc.width * 0.5, y = doc.height * 0.5;
        auto groupMask = [&] { const Layer* g = groupId ? s.document()->find(*groupId) : nullptr; return g ? maskAt(*g, x, y) : -1; };
        const Tool tool = s.tool();
        const BrushSettings brush = s.brushSettings;
        const QColor foreground = s.foregroundColor;
        s.selectTool(Tool::Brush);
        s.brushSettings.diameter = 9; s.brushSettings.hardness = 1; s.brushSettings.opacity = 1;
        s.foregroundColor = Qt::black;
        const size_t steps = s.undoNames().size();
        const bool began = s.beginBrush(QPointF(x, y), false);
        expect(began, "the brush did not start on a group's mask");
        if (began) { s.continueBrush(QPointF(x + 1, y)); s.endBrush(); }
        expect(groupMask() == 0 && s.undoNames().size() == steps + 1 && s.undoNames().back() == "Paint Mask", "the brush did not paint the group's mask in one step");
        s.foregroundColor = Qt::white;
        s.fillSelection(s.foregroundColor);
        expect(groupMask() == 255, "Fill did not fill the group's mask");
        expect(call("brush.stroke", {{"points", QJsonArray{QJsonArray{x, y}, QJsonArray{x + 1, y}}}, {"size", 9}, {"hardness", 1}, {"mask", true}, {"color", "#000000"}}).has_value()
                   && groupMask() == 0, "brush.stroke did not paint the group's mask");
        s.brushSettings = brush; s.foregroundColor = foreground;
        s.selectTool(tool);
        if (groupId) s.selectLayer(*groupId, false);
        expect(kind(Tool::Brush, ToolRefusal::Refused), "the group itself, its mask not the target, was not refused");
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

    // Rasterize Shape: the outline and stroke baked into the pixels, past the canvas too, and the vector mask (the
    // shape's path) gone, as in Photoshop; the composite stays as it was. One step, and its undo is the shape again.
    expect(call("shape.draw", {{"kind", "ellipse"}, {"x", -10}, {"y", 30}, {"width", 70}, {"height", 50}, {"color", "#2060c0"},
                               {"stroke", true}, {"strokeWidth", 4}, {"strokeColor", "#f0c020"}}).has_value(), "could not draw a stroked shape");
    if (const Layer* shape = s.activeLayer(); shape && isVectorShapeLayer(*shape) && layerVectorMask(*shape, *s.document())) {
        const auto before = renderFlattened(*s.document());
        const size_t steps = s.undoNames().size();
        expect(s.rasterizeLayer(), "the shape layer did not rasterize");
        expect(s.undoNames().size() == steps + 1 && s.undoNames().back() == "Rasterize Shape", "Rasterize Shape was not one step");
        const Layer* raster = s.activeLayer();
        expect(raster && !isVectorShapeLayer(*raster) && !hasLayerVectorMask(*raster) && !layerVectorMask(*raster, *s.document()) && !layerVectorStroke(*raster),
               "Rasterize Shape kept the shape's vector mask or stroke");
        expect(raster && raster->transform.origin.x < -10, "Rasterize Shape lost the part of the shape past the canvas");
        expect(!refusal(Tool::Brush), "the rasterized shape still refuses the brush");
        expect(maxDifference(*before, *renderFlattened(*s.document())) <= 1, "Rasterize Shape changed the composite");
        call("history.undo");
        const Layer* again = s.activeLayer();
        expect(again && isVectorShapeLayer(*again) && layerVectorMask(*again, *s.document()) && kind(Tool::Brush, ToolRefusal::Rasterize), "undoing Rasterize Shape did not bring the shape back");
    } else expect(false, "the stroked shape is not a vector shape layer");

    s.selectLayer(pixels, false);
    if (call("smartObject.convert").has_value()) expect(kind(Tool::Brush, ToolRefusal::SmartObject) && kind(Tool::Gradient, ToolRefusal::SmartObject), "a smart object did not ask about its contents");
    else expect(false, "could not make a smart object");

    // Photoshop's own shape layers (Patchy's fixtures, when PATCHY_FIXTURES names them): strokes, Feather,
    // combined paths, a live rectangle. Each rasterized shape looks as it did. (A gradient or pattern fill is drawn from
    // its PSD data and refused, not rasterized.)
    const QString fixtures = qEnvironmentVariable("PATCHY_FIXTURES");
    if (!fixtures.isEmpty()) {
        for (const char* name : {"photoshop-shape-strokes.psd", "photoshop-shape-feather.psd", "photoshop-shape-boolean.psd", "photoshop-shape-live-rect.psd"}) {
            const std::string path = (fixtures + "/" + QString::fromLatin1(name)).toStdString();
            std::string why;
            auto imported = importPsd(path, &why);
            if (!imported) { std::fprintf(stderr, "forbidden cursor: %s: %s\n", name, why.c_str()); failures++; continue; }
            s.adoptDocument(imported->document, QString::fromLatin1(name));
            std::vector<Uuid> shapes;
            for (const Layer& l : s.document()->layers)
                if (EditorSession::rasterizable(l) == Rasterizable::Shape && layerVectorMask(l, *s.document())) shapes.push_back(l.id);
            if (shapes.empty()) { std::fprintf(stderr, "forbidden cursor: %s has no shape layer\n", name); failures++; }
            for (const Uuid& id : shapes) {
                s.selectLayer(id, false);
                const auto before = renderFlattened(*s.document());
                const bool rasterized = s.rasterizeLayer();
                const Layer* raster = s.document()->find(id);
                const int difference = maxDifference(*before, *renderFlattened(*s.document()));
                if (!rasterized || !raster || layerVectorMask(*raster, *s.document()) || difference > 2) {
                    std::fprintf(stderr, "forbidden cursor: %s: Rasterize Shape on \"%s\": rasterized %d, difference %d\n", name, raster ? raster->name.c_str() : "?", int(rasterized), difference);
                    failures++;
                }
            }
        }
    }

    std::printf("forbidden cursor: %s\n", failures ? "FAILED" : "ok");
    return failures ? 1 : 0;
}

} // namespace app
