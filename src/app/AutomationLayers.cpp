// Automation methods: layers. Registered from AutomationServer::registerHandlers (Automation.cpp).
#include "Automation.h"
#include "AutomationHandlers.h"
#include "ImageConvert.h"
#include "compositor/render.h"
#include <QJsonDocument>
#include <algorithm>
#include <cmath>

using namespace compositor;
using namespace app::rpc;

namespace app {

void AutomationServer::registerLayersHandlers() {
    MainWindow* w = window_;
    const SessionOf session{w};
    const DocumentOf document{w};
    const LayerOf layer{document};
    const LayerOrActive layerOrActive{w};
    const WithActive withActive{w};

    // ---- layers
    add("layers.list", [document](const QJsonObject& p) {
        // thumbnails: true adds each pixel layer's 96 px thumbnail (and its mask's) as base64 PNG.
        QJsonArray out = layersJson(document());
        if (!flag(p, "thumbnails", false)) return out;
        const Document& doc = document();
        for (int i = 0; i < out.size(); i++) {
            QJsonObject o = out[i].toObject();
            const Layer* l = doc.find(o["id"].toString().toStdString());
            if (l && l->asset && l->asset->thumbnail) o["thumbnail"] = base64Png(*l->asset->thumbnail);
            if (l && l->mask && l->mask->asset.thumbnail) o["maskThumbnail"] = base64Png(*fromQImage(toQImage(*l->mask->asset.thumbnail)));
            out[i] = o;
        }
        return out;
    });
    add("layers.get", [layer](const QJsonObject& p) { return layerJson(layer(p), 0); });
    add("layers.select", [session, layer](const QJsonObject& p) {
        EditorSession* s = session();
        if (has(p, "ids")) {
            std::set<Uuid> ids;
            for (QJsonValue v : p.value("ids").toArray()) ids.insert(layer(QJsonObject{{"id", v}}).id);
            std::optional<Uuid> primary = has(p, "id") ? std::optional<Uuid>(layer(p).id) : (ids.empty() ? std::nullopt : std::optional<Uuid>(*ids.begin()));
            s->selectLayers(ids, primary);
        } else s->selectLayer(layer(p).id, flag(p, "mask", false));
        return QJsonObject{{"activeLayer", s->activeLayerId() ? qs(*s->activeLayerId()) : QString()}, {"maskSelected", s->isMaskSelected()}};
    });
    add("layers.set", [session, layer, withActive](const QJsonObject& p) {
        const Layer& l = layer(p);
        Uuid id = l.id;
        EditorSession* s = session();
        if (has(p, "name")) s->renameLayer(id, str(p, "name"));
        if (has(p, "visible") && flag(p, "visible", true) != l.visible) s->toggleLayerVisibility(id);
        if (has(p, "opacity") || has(p, "blend") || has(p, "sampling")) {
            std::optional<BlendMode> blend;
            std::optional<Sampling> sampling;
            if (has(p, "blend")) {
                blend = blendModeNamed(str(p, "blend"));
                if (!blend) fail("unknown blend mode '" + str(p, "blend") + "'; one of " + blendModeNames().join(", "), invalidParams);
                if (l.isGroup && *blend != BlendMode::Normal) fail("a folder has no blend mode; set it on the layers inside", invalidParams);
            }
            if (has(p, "sampling")) {
                sampling = samplingNamed(str(p, "sampling"));
                if (!sampling) fail("sampling must be Nearest, Smooth or High quality", invalidParams);
            }
            withActive(id, [&] {
                if (has(p, "opacity")) s->setLayerOpacity(std::clamp(num(p, "opacity"), 0.0, 1.0));
                if (blend) s->setLayerBlendMode(*blend);
                if (sampling) s->setLayerSampling(*sampling);
            });
        }
        if (has(p, "clipping") && flag(p, "clipping", false) != l.maskSourceId.has_value()) {
            if (!s->canToggleClippingMask(id)) fail("this layer can't be clipped (it needs a pixel layer beneath it in the same folder)");
            s->toggleClippingMask(id);
        }
        const Layer* now = s->document()->find(id);
        return now ? layerJson(*now, 0) : QJsonObject{};
    });
    add("layers.add", [session, document](const QJsonObject& p) {
        document();
        EditorSession* s = session();
        QString kind = str(p, "kind", QStringLiteral("pixels")).toLower();
        if (kind == "pixels" || kind == "blank") s->addBlankLayer(flag(p, "below", false));
        else if (kind == "group" || kind == "folder") s->addGroup();
        else if (kind == "adjustment") {
            auto ak = adjustmentKindNamed(str(p, "adjustmentKind"));
            if (!ak) fail("adjustmentKind must be Levels, Curves, Hue/Saturation, Exposure, Gradient Map or Grain", invalidParams);
            s->addAdjustmentLayer(*ak);
            if (has(p, "settings")) {
                const Layer* l = s->activeLayer();
                QJsonObject settings = obj(p, "settings");
                settings["kind"] = QString::fromUtf8(adjustmentKindName(*ak));
                AdjustmentSettings parsed;
                if (!l || !AdjustmentSettings::parse(QJsonDocument(settings).toJson(QJsonDocument::Compact).toStdString(), parsed)) fail("couldn't parse settings; adjustments.defaults shows the shape", invalidParams);
                s->setAdjustment(l->id, parsed);
            }
        } else if (kind == "text") {
            LayerText base = s->textStyle;
            base.text = "Text";
            base.red = s->foregroundColor.redF(); base.green = s->foregroundColor.greenF(); base.blue = s->foregroundColor.blueF();
            LayerText text = textFromParams(p, base);
            const Document& doc = document();
            if (!s->addTextLayer(QPointF(num(p, "x", doc.width / 4.0), num(p, "y", doc.height / 4.0)), text, false)) fail("the text could not be added");
        } else fail("kind must be pixels, group, adjustment or text", invalidParams);
        if (has(p, "name") && s->activeLayer()) s->renameLayer(s->activeLayer()->id, str(p, "name"));
        const Layer* l = s->activeLayer();
        return l ? layerJson(*l, 0) : QJsonObject{};
    });
    add("text.set", [session, layer](const QJsonObject& p) {
        // The active (or named) text layer's content and style; the layer must still be text (not painted on).
        EditorSession* s = session();
        const Layer& l = has(p, "id") ? layer(p) : [&]() -> const Layer& { const Layer* a = s->activeLayer(); if (!a) fail("no active layer; pass the text layer's id"); return *a; }();
        std::optional<LayerText> current = s->layerText(l.id);
        if (!current) fail("the layer is not a text layer (or its pixels were edited); add one with layers.add kind text", invalidParams);
        s->setLayerText(l.id, textFromParams(p, *current));
        const Layer* updated = s->document()->find(l.id);
        return updated ? layerJson(*updated, 0) : QJsonObject{};
    });
    add("layers.delete", [session, layer](const QJsonObject& p) {
        std::vector<Uuid> ids;
        if (has(p, "ids")) for (QJsonValue v : p.value("ids").toArray()) ids.push_back(layer(QJsonObject{{"id", v}}).id);
        else ids.push_back(layer(p).id);
        session()->deleteLayersResolvingClipping(ids, flag(p, "bakeClipping", true));
        return QJsonObject{{"deleted", int(ids.size())}};
    });
    add("layers.duplicate", [session, layerOrActive, withActive](const QJsonObject& p) {
        Uuid id = layerOrActive(p).id;
        withActive(id, [&] { session()->duplicateActiveLayer(); });
        // The duplicate is placed above and made active by duplicateActiveLayer; report it.
        const Layer* l = session()->activeLayer();
        return l ? layerJson(*l, 0) : QJsonObject{};
    });
    add("layers.move", [session, layer](const QJsonObject& p) {
        // Into `parent` (or the top level) directly above `above` (or at the bottom / top).
        const Layer& l = layer(p);
        std::optional<Uuid> parent, above;
        if (has(p, "parent")) parent = layer(p, "parent").id;
        if (has(p, "above")) above = layer(p, "above").id;
        if (!session()->placeLayer(l.id, parent, above, flag(p, "atBottom", false))) fail("that placement isn't allowed (a folder can't move into itself)");
        return QJsonObject{{"moved", true}};
    });
    add("layers.reorder", [session, layerOrActive, withActive](const QJsonObject& p) {
        Uuid id = layerOrActive(p).id;
        int offset = integer(p, "offset");
        withActive(id, [&] { if (session()->canMoveActiveLayer(offset)) session()->moveActiveLayer(offset); else fail("can't move the layer that far"); });
        return QJsonObject{{"moved", true}};
    });
    add("layers.setTransform", [session, layerOrActive, withActive](const QJsonObject& p) {
        const Layer& l = layerOrActive(p);
        LayerTransform t = l.transform;
        if (has(p, "x")) t.origin.x = num(p, "x");
        if (has(p, "y")) t.origin.y = num(p, "y");
        if (has(p, "width")) t.size.width = std::max(1.0, num(p, "width"));
        if (has(p, "height")) t.size.height = std::max(1.0, num(p, "height"));
        if (has(p, "rotation")) t.rotation = num(p, "rotation");
        if (has(p, "flipX")) t.flipX = flag(p, "flipX", false);
        if (has(p, "flipY")) t.flipY = flag(p, "flipY", false);
        if (has(p, "scale")) { double k = num(p, "scale"); Point c = t.center(); t.size = {std::max(1.0, t.size.width * k), std::max(1.0, t.size.height * k)}; t.origin = {c.x - t.size.width / 2, c.y - t.size.height / 2}; }
        Uuid id = l.id;
        withActive(id, [&] {
            EditorSession* s = session();
            if (!s->canTransform()) fail("this layer can't be transformed");
            s->beginTransform(true);
            s->previewTransform(t);
            s->commitTransform();
        });
        const Layer* now = session()->document()->find(id);
        return now ? transformJson(now->transform) : QJsonObject{};
    });
    add("layers.flip", [session, layerOrActive, withActive](const QJsonObject& p) {
        Uuid id = layerOrActive(p).id;
        withActive(id, [&] { session()->flipLayer(!flag(p, "vertical", false)); });
        return QJsonObject{{"flipped", true}};
    });
    add("layers.mask", [session, layerOrActive, withActive](const QJsonObject& p) {
        // action: add (revealing all, or hiding), addFromSelection, delete, toggle, invert, apply, link
        Uuid id = layerOrActive(p).id;
        QString action = str(p, "action").toLower();
        withActive(id, [&] {
            EditorSession* s = session();
            if (action == "add") s->addLayerMask(flag(p, "revealing", true));
            else if (action == "addfromselection") s->addMaskFromSelection(flag(p, "revealing", true));
            else if (action == "delete") s->deleteLayerMask();
            else if (action == "toggle") s->toggleLayerMask();
            else if (action == "invert") s->invertMask();
            else if (action == "apply") s->applyMask();
            else if (action == "link") s->toggleMaskLink(id);
            else fail("action must be add, addFromSelection, delete, toggle, invert, apply or link", invalidParams);
        });
        const Layer* now = session()->document()->find(id);
        return now ? layerJson(*now, 0) : QJsonObject{};
    });
    add("layers.merge", [session, document](const QJsonObject& p) {
        document();
        EditorSession* s = session();
        if (flag(p, "down", false)) { s->mergeDown(); return QJsonObject{{"merged", true}}; }
        if (!s->canMergeLayers()) fail("nothing to merge: select two or more layers, a folder, or a layer with one beneath it");
        QString title = s->mergeTitle();
        s->mergeLayers();
        return QJsonObject{{"merged", true}, {"action", title}};
    });
    add("layers.group", [session, document](const QJsonObject&) { document(); session()->groupSelectedLayers(); const Layer* l = session()->activeLayer(); return l ? layerJson(*l, 0) : QJsonObject{}; });
    add("layers.render", [layer](const QJsonObject& p) {
        // A layer's own pixels (not composited), downscaled to maxSize. With a mask, as the layer shows: the
        // mask applied, placed and rotated as on the canvas, over the layer's bounds (masked: false for the
        // raw pixels).
        const Layer& l = layer(p);
        if (!l.asset || !l.asset->image) fail("the layer has no pixels (a folder, adjustment or blank layer); document.overview shows each layer's kind");
        if (l.mask && l.mask->enabled && flag(p, "masked", true)) {
            const Rect bounds = l.transform.bounds().integral();
            if (!Document::validDimension(int(bounds.width)) || !Document::validDimension(int(bounds.height))) fail("the layer is too large to render alone; pass masked: false");
            Document solo(int(bounds.width), int(bounds.height));
            Layer alone = l;
            alone.parentId.reset();
            alone.maskSourceId.reset();
            alone.visible = true;
            alone.opacity = 1;
            alone.blendMode = BlendMode::Normal;
            alone.transform.origin = Point(l.transform.origin.x - bounds.x, l.transform.origin.y - bounds.y);
            solo.layers = {alone};
            const double maxSize = num(p, "maxSize", 1024);
            const double scale = maxSize > 0 ? std::min(1.0, maxSize / std::max(bounds.width, bounds.height)) : 1.0;
            Image out(std::max(1, int(std::lround(bounds.width * scale))), std::max(1, int(std::lround(bounds.height * scale))));
            RenderOptions options;
            options.region = solo.rect();
            options.scale = scale;
            render(solo, options, out, nullptr);
            return deliverPng(out, p, {{"id", qs(l.id)}, {"masked", true}, {"region", rectJson(bounds)}, {"transform", transformJson(l.transform)}});
        }
        auto copy = scaledCopy(*l.asset->image, num(p, "maxSize", 1024));
        return deliverPng(*copy, p, {{"id", qs(l.id)}, {"transform", transformJson(l.transform)}, {"pixelWidth", l.pixelWidth()}, {"pixelHeight", l.pixelHeight()}});
    });

    // ---- adjustment layers
    add("adjustments.get", [layerOrActive](const QJsonObject& p) {
        const Layer& l = layerOrActive(p);
        if (!l.adjustment) fail("not an adjustment layer; document.overview lists them with their kind");
        return QJsonObject{{"id", qs(l.id)}, {"kind", QString::fromUtf8(adjustmentKindName(l.adjustment->kind))},
                           {"settings", QJsonDocument::fromJson(QByteArray::fromStdString(l.adjustment->json)).object()}};
    });
    add("adjustments.set", [session, layerOrActive](const QJsonObject& p) {
        const Layer& l = layerOrActive(p);
        if (!l.adjustment) fail("not an adjustment layer; document.overview lists them with their kind");
        // Merge over the current settings so a partial object works.
        QJsonObject settings = QJsonDocument::fromJson(QByteArray::fromStdString(l.adjustment->json)).object();
        QJsonObject patch = obj(p, "settings");
        for (auto it = patch.begin(); it != patch.end(); ++it) settings[it.key()] = it.value();
        settings["kind"] = QString::fromUtf8(adjustmentKindName(l.adjustment->kind));
        AdjustmentSettings parsed;
        if (!AdjustmentSettings::parse(QJsonDocument(settings).toJson(QJsonDocument::Compact).toStdString(), parsed)) fail("couldn't parse settings; adjustments.get shows the shape", invalidParams);
        session()->setAdjustment(l.id, parsed);
        return QJsonObject{{"settings", QJsonDocument::fromJson(QByteArray::fromStdString(parsed.toJson())).object()}};
    });
    add("adjustments.defaults", [](const QJsonObject& p) {
        auto kind = adjustmentKindNamed(str(p, "kind"));
        if (!kind) fail("kind must be Levels, Curves, Hue/Saturation, Exposure, Gradient Map or Grain", invalidParams);
        return QJsonDocument::fromJson(QByteArray::fromStdString(AdjustmentSettings::defaults(*kind).toJson())).object();
    });

}

} // namespace app
