// Automation methods: selection. Registered from AutomationServer::registerHandlers (Automation.cpp).
#include "Automation.h"
#include "AutomationHandlers.h"
#include "ImageConvert.h"
#include "ModelStore.h"
#include "compositor/scribble.h"
#include <algorithm>

using namespace compositor;
using namespace app::rpc;

namespace app {

void AutomationServer::registerSelectionHandlers() {
    MainWindow* w = window_;
    const SessionOf session{w};
    const DocumentOf document{w};
    const LayerOrActive layerOrActive{w};

    // ---- selection (document pixels)
    add("selection.info", [document](const QJsonObject&) {
        const Document& doc = document();
        if (!doc.selection || doc.selection->isEmpty()) return QJsonObject{{"active", false}};
        return QJsonObject{{"active", true}, {"bounds", rectJson(doc.selection->bounds())}, {"antialiased", doc.selection->antialiased}};
    });
    add("selection.render", [document](const QJsonObject& p) {
        // The selection as a mask image: white selected, black not, downscaled to maxSize.
        const Document& doc = document();
        if (!doc.selection || !doc.selection->coverage) fail("there is no selection");
        QImage mask = toQImage(*doc.selection->coverage);
        double maxSize = num(p, "maxSize", 1024);
        if (maxSize > 0 && std::max(mask.width(), mask.height()) > maxSize) mask = mask.scaled(int(maxSize), int(maxSize), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        return deliverPng(*fromQImage(mask), p, {{"bounds", rectJson(doc.selection->bounds())}});
    });
    add("selection.all", [session, document](const QJsonObject&) { document(); session()->selectAll(); return QJsonObject{}; });
    add("selection.none", [session, document](const QJsonObject&) { document(); session()->deselect(); return QJsonObject{}; });
    add("selection.invert", [session, document](const QJsonObject&) { document(); session()->invertSelection(); return QJsonObject{}; });
    add("selection.rect", [session, document](const QJsonObject& p) {
        const Document& doc = document();
        Rect r(num(p, "x"), num(p, "y"), num(p, "width"), num(p, "height"));
        auto shape = flag(p, "ellipse", false) ? rasterizeEllipse(r, doc.width, doc.height, session()->selectionAntialiased) : rasterizeRect(r, doc.width, doc.height, session()->selectionAntialiased);
        session()->applySelectionShape(*shape, selectionMode(p), flag(p, "ellipse", false) ? "Ellipse" : "Marquee");
        return QJsonObject{{"bounds", rectJson(doc.selection ? doc.selection->bounds() : Rect())}};
    });
    add("selection.polygon", [session, document](const QJsonObject& p) {
        const Document& doc = document();
        std::vector<Point> points;
        for (QJsonValue v : p.value("points").toArray()) {
            QJsonArray pt = v.toArray();
            if (pt.size() == 2) points.push_back({pt[0].toDouble(), pt[1].toDouble()});
            else { QJsonObject o = v.toObject(); points.push_back({num(o, "x"), num(o, "y")}); }
        }
        if (points.size() < 3) fail("points needs at least three [x, y] pairs", invalidParams);
        auto shape = rasterizePolygon(points, doc.width, doc.height, session()->selectionAntialiased);
        session()->applySelectionShape(*shape, selectionMode(p), "Lasso");
        return QJsonObject{{"bounds", rectJson(doc.selection ? doc.selection->bounds() : Rect())}};
    });
    add("selection.wand", [session, document](const QJsonObject& p) {
        const Document& doc = document();
        EditorSession* s = session();
        s->magicWand(QPointF(num(p, "x"), num(p, "y")), integer(p, "tolerance", s->wandTolerance), flag(p, "contiguous", s->wandContiguous), flag(p, "sampleAll", s->wandSampleAll), selectionMode(p), integer(p, "sampleRadius", s->wandSampleRadius));
        return QJsonObject{{"bounds", rectJson(doc.selection ? doc.selection->bounds() : Rect())}};
    });
    add("selection.scribble", [session, document](const QJsonObject& p) {
        // Quick Select by scribble: strokes as lists of [x, y] points, `size` pixels wide.
        const Document& doc = document();
        EditorSession* s = session();
        if (!scribbleSelectionSupported()) fail("this build has no OpenCV, which runs the scribble selection");
        if (flag(p, "clear", false)) s->clearScribbles();
        if (has(p, "size")) s->scribbleSize = std::max(1, integer(p, "size", s->scribbleSize));
        if (has(p, "refine")) s->scribbleRefine = std::clamp(integer(p, "refine", s->scribbleRefine), 0, 40);
        auto strokes = [&](const char* key, bool background) {
            for (QJsonValue stroke : p.value(key).toArray()) {
                std::vector<QPointF> points;
                for (QJsonValue pt : stroke.toArray()) { QJsonArray a = pt.toArray(); if (a.size() >= 2) points.emplace_back(a[0].toDouble(), a[1].toDouble()); }
                if (!points.empty()) s->addScribble(points, background, false);
            }
        };
        strokes("foreground", false);
        strokes("background", true);
        QString error;
        if (!s->runScribbleSelection(selectionMode(p), &error)) fail(error);
        return QJsonObject{{"strokes", int(s->scribbles().size())}, {"bounds", rectJson(doc.selection ? doc.selection->bounds() : Rect())}};
    });
    add("selection.subject", [session, document](const QJsonObject& p) {
        // Click to select: `foreground` and `background` points as [x, y] lists, an optional `box` [x0, y0, x1, y1];
        // EfficientSAM finds the object, refined onto the image's edges. Up to six prompts count (a box is two).
        const Document& doc = document();
        EditorSession* s = session();
        if (!ModelStore::promptReady()) fail("the click-to-select model is not downloaded (Quick Select > Click > Download model, or nekophoto --download-model efficientsam_ti)");
        if (flag(p, "clear", true)) s->clearClickPrompts();
        s->setQuickSelectClicks(true);   // the prompts show on the canvas as the person's own would
        if (has(p, "refine")) s->scribbleRefine = std::clamp(integer(p, "refine", s->scribbleRefine), 0, 40);
        for (QJsonValue pt : p.value("foreground").toArray()) { QJsonArray a = pt.toArray(); if (a.size() >= 2) s->addClickPrompt(QPointF(a[0].toDouble(), a[1].toDouble()), false, false); }
        for (QJsonValue pt : p.value("background").toArray()) { QJsonArray a = pt.toArray(); if (a.size() >= 2) s->addClickPrompt(QPointF(a[0].toDouble(), a[1].toDouble()), true, false); }
        if (has(p, "box")) { QJsonArray b = p.value("box").toArray(); if (b.size() >= 4) s->setClickBox(QPointF(b[0].toDouble(), b[1].toDouble()), QPointF(b[2].toDouble(), b[3].toDouble()), false); }
        QString error;
        if (!s->runClickSelection(selectionMode(p), &error)) fail(error);
        return QJsonObject{{"prompts", int(s->clickPrompts().size())}, {"bounds", rectJson(doc.selection ? doc.selection->bounds() : Rect())}};
    });
    add("selection.fromLayer", [session, layerOrActive, document](const QJsonObject& p) {
        const Document& doc = document();
        session()->loadLayerAsSelection(layerOrActive(p).id, flag(p, "mask", false), selectionMode(p));
        return QJsonObject{{"bounds", rectJson(doc.selection ? doc.selection->bounds() : Rect())}};
    });
    add("selection.feather", [session, document](const QJsonObject& p) {
        const Document& doc = document();
        session()->selectionFeather(num(p, "radius"));
        return QJsonObject{{"bounds", rectJson(doc.selection ? doc.selection->bounds() : Rect())}};
    });
    add("selection.smooth", [session, document](const QJsonObject& p) {
        const Document& doc = document();
        session()->selectionSmooth(integer(p, "radius"));
        return QJsonObject{{"bounds", rectJson(doc.selection ? doc.selection->bounds() : Rect())}};
    });
    add("selection.border", [session, document](const QJsonObject& p) {
        const Document& doc = document();
        session()->selectionBorder(integer(p, "width"));
        return QJsonObject{{"bounds", rectJson(doc.selection ? doc.selection->bounds() : Rect())}};
    });
    add("selection.grow", [session, document](const QJsonObject& p) {
        const Document& doc = document();
        int amount = integer(p, "amount");
        if (amount >= 0) session()->selectionExpand(amount); else session()->selectionContract(-amount);
        return QJsonObject{{"bounds", rectJson(doc.selection ? doc.selection->bounds() : Rect())}};
    });

}

} // namespace app
