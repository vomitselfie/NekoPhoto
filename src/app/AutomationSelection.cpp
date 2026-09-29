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
        if (!doc.selection || !doc.selection->coverage) fail("there is no selection; make one with selection.rect, selection.wand or selection.fromLayer");
        auto coverage = EditorSession::coverage8(*doc.selection);   // a 16-bit selection reduced for the PNG
        if (!coverage) fail("there is no selection; make one with selection.rect, selection.wand or selection.fromLayer");
        QImage mask = toQImage(*coverage);
        double maxSize = num(p, "maxSize", 1024);
        if (maxSize > 0 && std::max(mask.width(), mask.height()) > maxSize) mask = mask.scaled(int(maxSize), int(maxSize), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        return deliverPng(*fromQImage(mask), p, {{"bounds", rectJson(doc.selection->bounds())}});
    });
    add("selection.all", [session, document](const QJsonObject&) { document(); session()->selectAll(); return QJsonObject{}; });
    add("selection.none", [session, document](const QJsonObject&) { document(); session()->deselect(); return QJsonObject{}; });
    add("selection.invert", [session, document](const QJsonObject&) { document(); session()->invertSelection(); return QJsonObject{}; });
    add("selection.quickMask", [session, document](const QJsonObject& p) {
        document();
        EditorSession* s = session();
        const bool on = has(p, "on") ? flag(p, "on", true) : !s->quickMaskActive();
        if (on && !s->quickMaskActive() && !s->beginQuickMask()) fail("couldn't enter Quick Mask");
        if (!on) s->endQuickMask();
        return QJsonObject{{"quickMask", s->quickMaskActive()}, {"layer", s->quickMaskActive() && s->activeLayerId() ? qs(*s->activeLayerId()) : QString()}};
    });
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
        s->magicWand(QPointF(num(p, "x"), num(p, "y")), integer(p, "tolerance", s->wandTolerance), flag(p, "contiguous", s->wandContiguous), flag(p, "sampleAll", s->wandSampleAll), selectionMode(p), integer(p, "sampleRadius", s->wandSampleRadius),
                     flag(p, "edgeAware", s->wandEdgeAware), has(p, "refineEdge") ? std::optional<bool>(flag(p, "refineEdge", true)) : std::nullopt);
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

    // ---- channels (the Channels panel; docs/channels.md). A channel is "rgb", "red", "green", "blue" or an alpha or
    // spot channel's id.
    auto colorBits = [](const QString& name) -> unsigned {
        const QString n = name.toLower();
        return n == "rgb" ? colorChannelsAll : n == "red" ? 1u : n == "green" ? 2u : n == "blue" ? 4u : 0u;
    };
    auto colorNames = [](unsigned bits) {
        QJsonArray names;
        for (int c = 0; c < 3; c++) if (bits >> c & 1) names.append(QStringList{"red", "green", "blue"}[c]);
        return names;
    };
    auto channelOf = [document](const QString& id) -> const Channel& {
        const Channel* c = findChannel(document(), id.toStdString());
        if (!c) fail("no channel " + id + "; channels.list gives the ids", invalidParams);
        return *c;
    };
    auto channelJson = [session](const Channel& c, int index) {
        const QColor color = QColor::fromRgbF(float(c.color[0]), float(c.color[1]), float(c.color[2]));
        const auto target = session()->targetChannel();
        return QJsonObject{{"id", qs(c.id)}, {"name", qs(c.name)}, {"kind", c.kind == ChannelKind::Spot ? "spot" : "alpha"}, {"index", index},
                           {"color", color.name()}, {"opacity", c.opacity}, {"colorIndicates", c.selectedAreas ? "selected" : "masked"},
                           {"visible", session()->visibleAlphaChannels().count(c.id) > 0}, {"target", target && *target == c.id}};
    };
    add("channels.list", [session, document, colorNames, channelJson](const QJsonObject&) {
        const Document& doc = document();
        EditorSession* s = session();
        QJsonArray channels;
        for (size_t i = 0; i < doc.channels.size(); i++) channels.append(channelJson(doc.channels[i], int(i)));
        return QJsonObject{{"activeColors", colorNames(s->targetChannel() ? 0u : s->activeColorChannels())}, {"visibleColors", colorNames(s->visibleColorChannels())},
                           {"target", s->targetChannel() ? qs(*s->targetChannel()) : QString()}, {"quickMask", s->quickMaskActive()}, {"channels", channels}};
    });
    add("channels.new", [session, document, channelJson](const QJsonObject& p) {
        document();
        QString error;
        EditorSession* s = session();
        const auto id = flag(p, "fromSelection", false) ? s->saveSelectionToChannel(std::nullopt, str(p, "name", QString()), SelectionMode::Replace, &error)
                                                        : s->newChannel(str(p, "name", QString()), &error);
        if (!id) fail(error.isEmpty() ? QStringLiteral("couldn't add a channel") : error);
        const Document& doc = document();
        return channelJson(*findChannel(doc, *id), channelIndex(doc, *id));
    });
    add("channels.duplicate", [session, document, channelOf, channelJson](const QJsonObject& p) {
        const Channel& c = channelOf(str(p, "id"));
        QString error;
        const auto id = session()->duplicateChannel(c.id, str(p, "name", QString()), &error);
        if (!id) fail(error.isEmpty() ? QStringLiteral("couldn't duplicate the channel") : error);
        const Document& doc = document();
        return channelJson(*findChannel(doc, *id), channelIndex(doc, *id));
    });
    add("channels.delete", [session, channelOf](const QJsonObject& p) {
        const Channel& c = channelOf(str(p, "id"));
        session()->deleteChannel(c.id);
        return QJsonObject{};
    });
    add("channels.select", [session, document, colorBits, channelOf](const QJsonObject& p) {
        document();
        const QString channel = str(p, "channel");
        const bool extend = flag(p, "extend", false);
        if (const unsigned bits = colorBits(channel)) session()->selectColorChannels(bits, extend);
        else if (!session()->selectAlphaChannel(channelOf(channel).id, extend)) fail("couldn't make the channel the target");
        return QJsonObject{};
    });
    add("channels.set", [session, document, colorBits, channelOf, channelJson](const QJsonObject& p) {
        document();
        EditorSession* s = session();
        const QString channel = str(p, "channel");
        if (const unsigned bits = colorBits(channel)) {
            for (const char* key : {"name", "color", "opacity", "colorIndicates", "index"})
                if (has(p, key)) fail(QStringLiteral("%1 is for alpha and spot channels").arg(key), invalidParams);
            if (has(p, "visible")) s->setColorChannelVisible(bits, flag(p, "visible", true));
            return QJsonObject{};
        }
        const Channel& c = channelOf(channel);
        const Uuid id = c.id;
        if (has(p, "name") || has(p, "color") || has(p, "opacity") || has(p, "colorIndicates")) {
            const QColor color = has(p, "color") ? QColor(str(p, "color")) : QColor::fromRgbF(float(c.color[0]), float(c.color[1]), float(c.color[2]));
            if (!color.isValid()) fail("color must be #rrggbb", invalidParams);
            const QString indicates = str(p, "colorIndicates", c.selectedAreas ? QStringLiteral("selected") : QStringLiteral("masked"));
            if (indicates != "selected" && indicates != "masked") fail("colorIndicates must be masked or selected", invalidParams);
            const double opacity = has(p, "opacity") ? num(p, "opacity") : c.opacity;
            if (opacity < 0 || opacity > 1) fail("opacity must be 0..1", invalidParams);
            s->setChannelOptions(id, str(p, "name", qs(c.name)), color, opacity, indicates == "selected");
        }
        if (has(p, "index")) s->moveChannel(id, integer(p, "index"));
        if (has(p, "visible")) s->setAlphaChannelVisible(id, flag(p, "visible", true));
        const Document& doc = document();
        return channelJson(*findChannel(doc, id), channelIndex(doc, id));
    });
    add("channels.saveSelection", [session, document, channelOf, channelJson](const QJsonObject& p) {
        document();
        std::optional<Uuid> into;
        if (has(p, "id")) into = channelOf(str(p, "id")).id;
        QString error;
        const auto id = session()->saveSelectionToChannel(into, str(p, "name", QString()), selectionMode(p), &error);
        if (!id) fail(error.isEmpty() ? QStringLiteral("couldn't save the selection") : error);
        const Document& doc = document();
        return channelJson(*findChannel(doc, *id), channelIndex(doc, *id));
    });
    add("channels.loadSelection", [session, document, colorBits, channelOf](const QJsonObject& p) {
        const Document& doc = document();
        SelectionSource source;
        if (has(p, "layer")) {
            const Layer* layer = doc.find(str(p, "layer").toStdString());
            if (!layer) fail("no layer " + str(p, "layer"), invalidParams);
            source.kind = flag(p, "mask", false) ? SelectionSource::LayerMask : SelectionSource::Transparency;
            source.id = layer->id;
        } else {
            const QString channel = str(p, "channel");
            const unsigned bits = colorBits(channel);
            if (bits == colorChannelsAll) source.kind = SelectionSource::Composite;
            else if (bits) source.kind = bits == 1 ? SelectionSource::Red : bits == 2 ? SelectionSource::Green : SelectionSource::Blue;
            else { source.kind = SelectionSource::AlphaChannel; source.id = channelOf(channel).id; }
        }
        // A thumbnail's Ctrl-click: shift and alt pick the mode as Photoshop does.
        const SelectionMode mode = has(p, "shift") || has(p, "alt") ? thumbnailClickMode(flag(p, "shift", false), flag(p, "alt", false)) : selectionMode(p);
        QString error;
        if (!session()->loadSelectionFromSource(source, flag(p, "invert", false), mode, &error)) fail(error.isEmpty() ? QStringLiteral("couldn't load the selection") : error);
        const Document& after = document();
        return QJsonObject{{"active", bool(after.selection)}, {"bounds", rectJson(after.selection ? after.selection->bounds() : Rect())}};
    });
}

} // namespace app
