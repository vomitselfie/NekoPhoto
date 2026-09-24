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

void AutomationServer::registerPaintHandlers() {
    MainWindow* w = window_;
    const SessionOf session{w};
    const DocumentOf document{w};

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
        auto previousClone = s->cloneSource;
        auto previousActive = s->activeLayerId();
        const QString previousPreset = s->brushPreset;
        auto restore = [&] {
            s->brushSettings = previousBrush; s->brushErase = previousErase; s->foregroundColor = previousColor; s->blurMode = previousBlur; s->cloneSource = previousClone;
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
        else if (tool == "healing") s->selectTool(Tool::SpotHealing);
        else if (tool == "clone") {
            s->selectTool(Tool::CloneStamp);
            if (has(p, "source")) { QJsonObject src = obj(p, "source"); s->setCloneSource(QPointF(num(src, "x"), num(src, "y"))); }
        } else if (tool == "smudge" || tool == "blur" || tool == "liquify") {
            s->selectTool(Tool::Smudge);
            s->blurMode = tool == "blur" ? BlurToolMode::Blur : tool == "smudge" ? BlurToolMode::Smudge : BlurToolMode::Liquify;
            warp = true;
        } else { restore(); fail("tool must be brush, eraser, healing, clone, smudge, blur or liquify", invalidParams); }
        penAt(0);
        bool started = warp ? s->beginWarp(pts[0]) : s->beginBrush(pts[0], false);
        if (!started) { restore(); fail("couldn't start the stroke: the active layer must have pixels (clone needs a source; healing and clone can't paint a mask)"); }
        for (size_t i = 1; i < pts.size(); i++) { penAt(i); if (warp) s->continueWarp(pts[i]); else s->continueBrush(pts[i]); }
        if (warp) s->endWarp(); else s->endBrush();
        const QString usedPreset = s->brushPreset;
        restore();
        QJsonObject answer{{"points", int(pts.size())}, {"tool", tool}};
        if (!usedPreset.isEmpty()) answer["preset"] = usedPreset;
        return answer;
    });
    add("gradient.draw", [session, document](const QJsonObject& p) {
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
    add("shape.draw", [session, document](const QJsonObject& p) {
        // A new shape layer; kind rectangle (with cornerRadius) or ellipse, in color (default: the foreground).
        document();
        EditorSession* s = session();
        ShapeKind previousKind = s->shapeKind;
        double previousRadius = s->shapeCornerRadius;
        QColor fg = s->foregroundColor;
        auto restore = [&] { s->shapeKind = previousKind; s->shapeCornerRadius = previousRadius; s->foregroundColor = fg; };
        QString kind = str(p, "kind", QStringLiteral("rectangle")).toLower();
        s->shapeKind = kind.startsWith("ell") || kind == "circle" ? ShapeKind::Ellipse : ShapeKind::Rectangle;
        s->shapeCornerRadius = std::max(0.0, num(p, "cornerRadius", 0));
        if (has(p, "color")) { QColor c(str(p, "color")); if (!c.isValid()) { restore(); fail("color must be a CSS colour", invalidParams); } s->foregroundColor = c; }
        double x = num(p, "x"), y = num(p, "y"), w = num(p, "width"), h = num(p, "height");
        s->beginShape(QPointF(x, y));
        if (!s->shapeDraft()) { restore(); fail("couldn't start a shape: the document has no editable layer"); }
        s->dragShape(QPointF(x + w, y + h), false, false);
        s->finishShape();
        restore();
        const Layer* l = s->activeLayer();
        return l ? layerJson(*l, 0) : QJsonObject{};
    });

}

} // namespace app
