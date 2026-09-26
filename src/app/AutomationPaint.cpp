// Automation methods: paint. Registered from AutomationServer::registerHandlers (Automation.cpp).
#include "Automation.h"
#include "AutomationHandlers.h"
#include "BrushImporter.h"
#include "BrushLibrary.h"
#include <QFileInfo>
#include <algorithm>

using namespace compositor;
using namespace app::rpc;

namespace app {

namespace {

/// A path as automation shows it: subpaths {closed, op, knots: [[inX, inY, x, y, outX, outY], ...]}.
QJsonArray pathToJson(const VectorPath& path) {
    QJsonArray subpaths;
    for (const auto& sp : path.subpaths) {
        QJsonArray knots;
        for (const auto& k : sp.knots) knots.append(QJsonArray{k.inX, k.inY, k.x, k.y, k.outX, k.outY});
        static const char* const ops[] = {"exclude", "add", "subtract", "intersect"};
        subpaths.append(QJsonObject{{"closed", sp.closed}, {"op", ops[int(sp.op) & 3]}, {"knots", knots}});
    }
    return subpaths;
}

/// And back; a knot may be [x, y] (a corner). Fails the request on a malformed path.
VectorPath pathFromJson(const QJsonValue& value) {
    VectorPath path;
    for (QJsonValue v : value.toArray()) {
        const QJsonObject o = v.toObject();
        VectorPath::Subpath sp;
        sp.closed = o.value("closed").toBool(true);
        const QString op = o.value("op").toString("add");
        sp.op = op == "subtract" ? VectorPath::Op::Subtract : op == "intersect" ? VectorPath::Op::Intersect : op == "exclude" ? VectorPath::Op::Xor : VectorPath::Op::Add;
        for (QJsonValue k : o.value("knots").toArray()) {
            const QJsonArray a = k.toArray();
            if (a.size() == 2) sp.knots.push_back({a[0].toDouble(), a[1].toDouble(), a[0].toDouble(), a[1].toDouble(), a[0].toDouble(), a[1].toDouble()});
            else if (a.size() == 6) sp.knots.push_back({a[0].toDouble(), a[1].toDouble(), a[2].toDouble(), a[3].toDouble(), a[4].toDouble(), a[5].toDouble()});
            else fail("each knot is [x, y] or [inX, inY, x, y, outX, outY]", invalidParams);
        }
        if (sp.knots.size() < 2) fail("each subpath needs at least two knots", invalidParams);
        path.subpaths.push_back(std::move(sp));
    }
    if (path.subpaths.empty()) fail("path needs at least one subpath", invalidParams);
    return path;
}

} // namespace

void AutomationServer::registerPaintHandlers() {
    MainWindow* w = window_;
    const SessionOf session{w};
    const DocumentOf document{w};
    const LayerOf layer{document};

    // ---- painting by coordinates (document pixels); the person's tool and settings are put back afterwards
    auto pointList = [](const QJsonObject& p, const char* key) {
        std::vector<QPointF> pts;
        for (QJsonValue v : p.value(QLatin1String(key)).toArray()) {
            if (v.isArray()) { QJsonArray a = v.toArray(); if (a.size() == 2) pts.emplace_back(a[0].toDouble(), a[1].toDouble()); }
            else { QJsonObject o = v.toObject(); pts.emplace_back(num(o, "x"), num(o, "y")); }
        }
        return pts;
    };
    add("brush.presets", [](const QJsonObject& p) {
        // The MyPaint presets the Brush tool offers, grouped as in the picker; group filters to one.
        const QString group = str(p, "group", QString());
        QJsonArray out;
        for (const BrushPreset& preset : BrushLibrary::presets()) {
            if (!group.isEmpty() && preset.group.compare(group, Qt::CaseInsensitive) != 0) continue;
            out.append(QJsonObject{{"id", preset.id}, {"name", preset.name}, {"group", preset.group}, {"size", preset.diameter}, {"eraser", preset.eraser}});
        }
        return QJsonObject{{"supported", myPaintSupported()}, {"presets", out}, {"groups", QJsonArray::fromStringList(BrushLibrary::groups())}};
    });
    add("brush.import", [](const QJsonObject& p) {
        // Brush files (Photoshop .abr, Procreate .brushset/.brush, Clip Studio .sut, images) into the library.
        QStringList paths;
        if (has(p, "path")) paths << QFileInfo(str(p, "path")).absoluteFilePath();
        for (QJsonValue v : p.value("paths").toArray()) paths << QFileInfo(v.toString()).absoluteFilePath();
        if (paths.isEmpty()) fail("pass path or paths", invalidParams);
        const BrushImportResult result = importBrushFiles(paths);
        if (result.ids.isEmpty()) fail(result.errors.isEmpty() ? QStringLiteral("nothing was imported") : result.errors.join("; "));
        return QJsonObject{{"presets", QJsonArray::fromStringList(result.ids)}, {"notes", QJsonArray::fromStringList(result.notes)}, {"errors", QJsonArray::fromStringList(result.errors)}};
    });
    add("brush.stroke", [session, document, pointList](const QJsonObject& p) {
        if (session()->smartObjectBlocksPixels() && !flag(p, "mask", false))
            fail("the active layer is a smart object: edit its contents (smartObject.editContents) or rasterize it (smartObject.rasterize) first", invalidParams);
        document();
        EditorSession* s = session();
        std::vector<QPointF> pts = pointList(p, "points");
        if (pts.empty()) fail("points needs at least one [x, y]", invalidParams);
        QString tool = str(p, "tool", QStringLiteral("brush")).toLower();
        Tool previousTool = s->tool();
        BrushSettings previousBrush = s->brushSettings;
        bool previousErase = s->brushErase, previousMask = s->isMaskSelected();
        QColor previousColor = s->foregroundColor;
        BlurToolMode previousBlur = s->blurMode;
        const ToningSettings previousToning = s->toning;
        const int previousHealing = s->spotHealingMode;
        auto previousClone = s->cloneSource;
        auto previousActive = s->activeLayerId();
        const QString previousPreset = s->brushPreset;
        auto restore = [&] {
            s->brushSettings = previousBrush; s->brushErase = previousErase; s->foregroundColor = previousColor; s->blurMode = previousBlur; s->toning = previousToning; s->spotHealingMode = previousHealing; s->cloneSource = previousClone;
            s->brushPreset = previousPreset;
            if (previousActive && s->document() && s->document()->find(*previousActive)) s->selectLayer(previousActive, previousMask);
            s->selectTool(previousTool);
        };
        if (has(p, "size")) s->brushSettings.diameter = std::clamp(num(p, "size"), 1.0, 2000.0);
        if (has(p, "hardness")) s->brushSettings.hardness = std::clamp(num(p, "hardness"), 0.0, 1.0);
        if (has(p, "opacity")) s->brushSettings.opacity = std::clamp(num(p, "opacity"), 0.0, 1.0);
        if (has(p, "color")) { QColor c(str(p, "color")); if (!c.isValid()) { restore(); fail("color must be a CSS colour", invalidParams); } s->foregroundColor = c; }
        if (has(p, "mask") && s->activeLayerId()) s->selectLayer(s->activeLayerId(), flag(p, "mask", false));
        if (has(p, "preset")) {
            // A MyPaint preset from brush.presets, or "round" for the plain tip; the preset's own size unless size is given.
            QString id = str(p, "preset");
            if (id.compare("round", Qt::CaseInsensitive) == 0) id.clear();
            const BrushPreset* preset = id.isEmpty() ? nullptr : BrushLibrary::find(id);
            if (!id.isEmpty() && !preset) { restore(); fail("no brush preset " + id + "; brush.presets lists them", invalidParams); }
            if (!id.isEmpty() && !myPaintSupported()) { restore(); fail("this build has no MyPaint brush engine"); }
            s->brushPreset = id;
            if (preset && !has(p, "size")) s->brushSettings.diameter = preset->diameter;
        }
        // Pen pressure: one value for the whole stroke, or one per point; events come 8 ms apart.
        const QJsonArray pressures = p.value("pressures").toArray();
        const double pressure = std::clamp(num(p, "pressure", 0.5), 0.0, 1.0);
        auto penAt = [&](size_t i) {
            double value = i < size_t(pressures.size()) ? std::clamp(pressures[int(i)].toDouble(pressure), 0.0, 1.0) : pressure;
            // Given pressure acts as a pen's; without it MyPaint sees a mouse (half) and tip brushes full pressure.
            s->pen = {value, 0, 0, qint64(i) * 8, has(p, "pressure") || !pressures.isEmpty()};
        };
        bool warp = false;
        if (tool == "brush" || tool == "eraser") { s->selectTool(Tool::Brush); s->brushErase = tool == "eraser" || flag(p, "erase", false); }
        else if (tool == "healing") { s->selectTool(Tool::SpotHealing); if (s->spotHealingMode > 2) s->spotHealingMode = 0; }
        else if (tool == "healingbrush") {
            s->selectTool(Tool::SpotHealing);
            s->spotHealingMode = 3;
            if (has(p, "source")) { QJsonObject src = obj(p, "source"); s->setCloneSource(QPointF(num(src, "x"), num(src, "y"))); }
        }
        else if (tool == "clone") {
            s->selectTool(Tool::CloneStamp);
            if (has(p, "source")) { QJsonObject src = obj(p, "source"); s->setCloneSource(QPointF(num(src, "x"), num(src, "y"))); }
        } else if (tool == "smudge" || tool == "blur" || tool == "liquify" || tool == "sharpen") {
            s->selectTool(Tool::Smudge);
            s->blurMode = tool == "blur" ? BlurToolMode::Blur : tool == "smudge" ? BlurToolMode::Smudge : tool == "sharpen" ? BlurToolMode::Sharpen : BlurToolMode::Liquify;
            warp = true;
        } else if (tool == "dodge" || tool == "burn" || tool == "sponge") {
            s->selectTool(Tool::Dodge);
            s->toning.kind = tool == "dodge" ? ToningKind::Dodge : tool == "burn" ? ToningKind::Burn : ToningKind::Sponge;
            const QString range = str(p, "range", QStringLiteral("midtones")).toLower();
            if (range != "shadows" && range != "midtones" && range != "highlights") { restore(); fail("range must be shadows, midtones or highlights", invalidParams); }
            s->toning.range = range == "shadows" ? ToneRange::Shadows : range == "highlights" ? ToneRange::Highlights : ToneRange::Midtones;
            s->toning.protectTones = flag(p, "protectTones", true);
            s->toning.saturate = flag(p, "saturate", false);
            warp = true;
        } else { restore(); fail("tool must be brush, eraser, healing, healingbrush, clone, smudge, blur, sharpen, liquify, dodge, burn or sponge", invalidParams); }
        penAt(0);
        bool started = warp ? (tool == "dodge" || tool == "burn" || tool == "sponge" ? s->beginToning(pts[0]) : s->beginWarp(pts[0])) : s->beginBrush(pts[0], false);
        if (!started) { restore(); fail("couldn't start the stroke: the active layer must have pixels (clone needs a source; healing and clone can't paint a mask)"); }
        for (size_t i = 1; i < pts.size(); i++) { penAt(i); if (warp) s->continueWarp(pts[i]); else s->continueBrush(pts[i]); }
        if (warp) s->endWarp(); else s->endBrush();
        const QString usedPreset = s->brushPreset;
        restore();
        QJsonObject answer{{"points", int(pts.size())}, {"tool", tool}};
        if (!usedPreset.isEmpty()) answer["preset"] = usedPreset;
        return answer;
    });
    add("pixels.bucket", [session, document](const QJsonObject& p) {
        document();
        EditorSession* s = session();
        const auto previousBucket = s->bucket;
        const double previousOpacity = s->brushSettings.opacity;
        const QColor previousColor = s->foregroundColor;
        auto restore = [&] { s->bucket = previousBucket; s->brushSettings.opacity = previousOpacity; s->foregroundColor = previousColor; };
        if (has(p, "color")) { QColor c(str(p, "color")); if (!c.isValid()) fail("color must be a CSS colour", invalidParams); s->foregroundColor = c; }
        s->bucket.tolerance = int(std::clamp(num(p, "tolerance", 32), 0.0, 255.0));
        s->bucket.contiguous = flag(p, "contiguous", true);
        s->bucket.antialias = flag(p, "antialias", true);
        s->bucket.allLayers = flag(p, "allLayers", false);
        s->brushSettings.opacity = std::clamp(num(p, "opacity", 1), 0.0, 1.0);
        const bool filled = s->paintBucket(QPointF(num(p, "x"), num(p, "y")));
        restore();
        if (!filled) fail("nothing was filled: the point must be on the canvas and inside the selection, on a pixel layer (or its mask)");
        return QJsonObject{{"filled", true}};
    });
    add("pixels.patch", [session, document](const QJsonObject& p) {
        document();
        if (!session()->patchSelection(int(std::lround(num(p, "dx"))), int(std::lround(num(p, "dy")))))
            fail("nothing was patched: make a selection on a pixel layer and give an offset (dx, dy) to copy from");
        return QJsonObject{{"patched", true}};
    });
    add("gradient.draw", [session, document](const QJsonObject& p) {
        if (session()->smartObjectBlocksPixels() && !flag(p, "mask", false))
            fail("the active layer is a smart object: edit its contents (smartObject.editContents) or rasterize it (smartObject.rasterize) first", invalidParams);
        document();
        EditorSession* s = session();
        GradientSettings previous = s->gradientSettings;
        QColor fg = s->foregroundColor, bg = s->backgroundColor;
        auto restore = [&] { s->gradientSettings = previous; s->foregroundColor = fg; s->backgroundColor = bg; };
        QString shape = str(p, "shape", QStringLiteral("linear")).toLower();
        QString style = str(p, "style", QStringLiteral("foreground-to-transparent")).toLower();
        s->gradientSettings.shape = shape == "radial" ? GradientShape::Radial : GradientShape::Linear;
        s->gradientSettings.style = style.contains("background") ? GradientStyle::ForegroundToBackground : GradientStyle::ForegroundToTransparent;
        s->gradientSettings.reversed = flag(p, "reversed", false);
        s->gradientSettings.opacity = std::clamp(num(p, "opacity", 1), 0.0, 1.0);
        if (has(p, "foreground")) s->foregroundColor = QColor(str(p, "foreground"));
        if (has(p, "background")) s->backgroundColor = QColor(str(p, "background"));
        QPointF a(num(p, "x0"), num(p, "y0")), b(num(p, "x1"), num(p, "y1"));
        s->beginGradient(a);
        if (!s->gradientPending()) { restore(); fail("couldn't start a gradient: select a pixel layer (or its mask)"); }
        s->moveGradient(b);
        s->endGradientDrag();
        s->commitGradient();
        restore();
        return QJsonObject{{"drawn", true}};
    });
    // Vector shapes: JSON both ways. Paths are lists of subpaths, each {closed, op, knots: [[inX, inY, x, y, outX, outY], ...]}.
    auto shapeJson = [](const VectorShape& shape) {
        const QJsonArray subpaths = pathToJson(shape.path);
        auto hex = [](int r, int g, int b) { return QColor(r, g, b).name(); };
        const VectorStroke& t = shape.stroke;
        return QJsonObject{{"path", subpaths}, {"fill", shape.fill}, {"color", hex(shape.r, shape.g, shape.b)},
                           {"stroke", QJsonObject{{"enabled", t.enabled}, {"width", t.width}, {"color", hex(t.r, t.g, t.b)}, {"opacity", t.opacity},
                                                  {"align", t.align == VectorStroke::Align::Inside ? "inside" : t.align == VectorStroke::Align::Outside ? "outside" : "center"},
                                                  {"dashes", QJsonArray::fromVariantList([&] { QVariantList l; for (double d : t.dashes) l << d; return l; }())}}}};
    };
    // Applies `p`'s fill and stroke keys onto `shape` (strokeWidth, strokeColor, ...); false with `why` on a bad value.
    auto applyStyle = [](const QJsonObject& p, VectorShape& shape, QString& why) {
        if (has(p, "color")) { QColor c(str(p, "color")); if (!c.isValid()) { why = "color must be a CSS colour"; return false; } shape.r = uint8_t(c.red()); shape.g = uint8_t(c.green()); shape.b = uint8_t(c.blue()); }
        if (has(p, "fill")) shape.fill = flag(p, "fill", true);
        VectorStroke& t = shape.stroke;
        if (has(p, "stroke")) t.enabled = flag(p, "stroke", false);
        if (has(p, "strokeWidth")) { t.width = std::clamp(num(p, "strokeWidth"), 0.0, 1000.0); t.enabled = true; }
        if (has(p, "strokeColor")) { QColor c(str(p, "strokeColor")); if (!c.isValid()) { why = "strokeColor must be a CSS colour"; return false; } t.r = uint8_t(c.red()); t.g = uint8_t(c.green()); t.b = uint8_t(c.blue()); t.enabled = true; }
        if (has(p, "strokeAlign")) {
            const QString a = str(p, "strokeAlign").toLower();
            if (a != "inside" && a != "center" && a != "outside") { why = "strokeAlign must be inside, center or outside"; return false; }
            t.align = a == "inside" ? VectorStroke::Align::Inside : a == "outside" ? VectorStroke::Align::Outside : VectorStroke::Align::Center;
        }
        if (has(p, "strokeDashes")) { t.dashes.clear(); for (QJsonValue v : p.value("strokeDashes").toArray()) t.dashes.push_back(std::max(0.0, v.toDouble())); }
        if (!shape.fill && !t.enabled) { why = "a shape needs a fill or a stroke"; return false; }
        return true;
    };
    add("shape.draw", [session, document, applyStyle](const QJsonObject& p) {
        // A new vector shape layer; kind rectangle (cornerRadius), ellipse, polygon (sides, star), line (x, y to x2, y2,
        // weight) or custom (name), filled with color (default: the foreground) and optionally stroked.
        const Document& doc = document();
        EditorSession* s = session();
        QString kind = str(p, "kind", QStringLiteral("rectangle")).toLower();
        const double x = num(p, "x"), y = num(p, "y");
        const Rect box(x, y, num(p, "width", 0), num(p, "height", 0));
        std::optional<VectorPath> path;
        if (kind == "line") path = linePath({x, y}, {num(p, "x2", x + box.width), num(p, "y2", y + box.height)}, std::max(0.5, num(p, "weight", 4)));
        else if (box.width < 1 || box.height < 1) fail("width and height must be at least 1", invalidParams);
        else if (kind.startsWith("ell") || kind == "circle") path = ellipsePath(box);
        else if (kind == "polygon" || kind == "star") path = polygonPath(box, int(num(p, "sides", 5)), kind == "star" ? std::clamp(num(p, "star", 0.5), 0.0, 0.99) : std::clamp(num(p, "star", 0), 0.0, 0.99));
        else if (kind == "custom") {
            const std::string name = str(p, "name", QStringLiteral("Heart")).toStdString();
            const auto& names = customShapeNames();
            if (std::find(names.begin(), names.end(), name) == names.end()) fail("name must be one of Heart, Star, Arrow, Speech Bubble, Check Mark, Lightning", invalidParams);
            path = customShapePath(name, box);
        } else if (kind == "rectangle" || kind == "rect") path = rectanglePath(box, std::max(0.0, num(p, "cornerRadius", 0)));
        else fail("kind must be rectangle, ellipse, polygon, star, line or custom", invalidParams);
        VectorShape shape;
        shape.path = *path;
        const QColor fg = s->foregroundColor;
        shape.r = uint8_t(fg.red()); shape.g = uint8_t(fg.green()); shape.b = uint8_t(fg.blue());
        QString why;
        if (!applyStyle(p, shape, why)) fail(why, invalidParams);
        (void)doc;
        static const QMap<QString, QString> names{{"rectangle", "Rectangle"}, {"rect", "Rectangle"}, {"ellipse", "Ellipse"}, {"circle", "Ellipse"}, {"polygon", "Polygon"}, {"star", "Star"}, {"line", "Line"}};
        const QString layerName = has(p, "name") ? str(p, "name") : names.value(kind, QStringLiteral("Shape"));
        if (!s->addVectorShapeLayer(shape, layerName)) fail("couldn't add a shape layer here");
        const Layer* l = s->activeLayer();
        return l ? layerJson(*l, 0) : QJsonObject{};
    });
    // ---- the Paths panel: the Work Path (id 1025) and saved paths (2000 and up)
    auto pathId = [session](const QJsonObject& p) {
        const int id = int(num(p, "id", -1));
        if (id < 0 || !session()->document() || !documentPath(*session()->document(), uint16_t(id))) fail("no path with that id; paths.list gives them", invalidParams);
        return uint16_t(id);
    };
    add("paths.list", [session, document](const QJsonObject&) {
        document();
        QJsonArray out;
        for (const auto& path : session()->paths())
            out.append(QJsonObject{{"id", int(path.id)}, {"name", QString::fromStdString(path.name)}, {"work", path.id == kWorkPathId}, {"path", pathToJson(path.path)}});
        return QJsonObject{{"paths", out}, {"active", session()->activePathId() ? QJsonValue(int(*session()->activePathId())) : QJsonValue()}};
    });
    add("paths.set", [session, document, pathId](const QJsonObject& p) {
        // A path: a new saved one (name), the Work Path (work true), or an existing one (id) replaced.
        document();
        const VectorPath path = pathFromJson(p.value("path"));
        const uint16_t id = has(p, "id") ? pathId(p) : flag(p, "work", false) ? kWorkPathId : 0;
        const uint16_t stored = session()->storePath(id, str(p, "name", QStringLiteral("Path")), path);
        if (!stored) fail("couldn't store the path");
        if (has(p, "id") && has(p, "name")) session()->renamePath(stored, str(p, "name"));
        return QJsonObject{{"id", int(stored)}};
    });
    add("paths.select", [session, document](const QJsonObject& p) {
        document();
        if (!has(p, "id") || p.value("id").isNull()) session()->selectPath(std::nullopt);
        else session()->selectPath(uint16_t(num(p, "id")));
        return QJsonObject{{"active", session()->activePathId() ? QJsonValue(int(*session()->activePathId())) : QJsonValue()}};
    });
    add("paths.delete", [session, pathId](const QJsonObject& p) { session()->deletePath(pathId(p)); return QJsonObject{{"deleted", true}}; });
    add("paths.fill", [session, pathId](const QJsonObject& p) { if (!session()->fillPath(pathId(p))) fail("couldn't fill: the active layer must take pixels"); return QJsonObject{{"filled", true}}; });
    add("paths.stroke", [session, pathId](const QJsonObject& p) { if (!session()->strokePath(pathId(p))) fail("couldn't stroke: the active layer must take pixels"); return QJsonObject{{"stroked", true}}; });
    add("paths.toSelection", [session, pathId](const QJsonObject& p) {
        const QString mode = str(p, "mode", QStringLiteral("replace")).toLower();
        const SelectionMode m = mode == "add" ? SelectionMode::Add : mode == "subtract" ? SelectionMode::Subtract : mode == "intersect" ? SelectionMode::Intersect : SelectionMode::Replace;
        if (!session()->pathToSelection(pathId(p), m)) fail("the path is empty");
        return QJsonObject{{"selected", true}};
    });
    add("paths.toShape", [session, pathId](const QJsonObject& p) {
        if (!session()->pathToShapeLayer(pathId(p))) fail("the path is empty");
        const Layer* l = session()->activeLayer();
        return l ? layerJson(*l, 0) : QJsonObject{};
    });
    add("paths.fromSelection", [session, document](const QJsonObject& p) {
        document();
        if (!session()->selectionToWorkPath(num(p, "tolerance", 1.0))) fail("no selection to make a path from (or its outline is too detailed)");
        return QJsonObject{{"id", int(kWorkPathId)}};
    });
    add("shape.get", [session, layer, shapeJson](const QJsonObject& p) {
        const Layer& l = layer(p);
        auto shape = vectorShapeOf(l, *session()->document());
        if (!shape) fail("not a vector shape layer", invalidParams);
        return shapeJson(*shape);
    });
    add("shape.set", [session, layer, shapeJson, applyStyle](const QJsonObject& p) {
        EditorSession* s = session();
        const Uuid id = layer(p).id;
        s->selectLayer(id);
        auto shape = s->activeVectorShape();
        if (!shape) fail("not a vector shape layer", invalidParams);
        QString why;
        if (!applyStyle(p, *shape, why)) fail(why, invalidParams);
        if (has(p, "path")) {
            shape->path = pathFromJson(p.value("path"));
        }
        if (!s->setActiveVectorShape(*shape, QStringLiteral("Edit Shape"))) fail("couldn't change the shape");
        return shapeJson(*s->activeVectorShape());
    });
}

} // namespace app
