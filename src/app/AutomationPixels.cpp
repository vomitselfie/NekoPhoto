// Automation methods: pixels. Registered from AutomationServer::registerHandlers (Automation.cpp).
#include "Automation.h"
#include "AutomationHandlers.h"
#include "Gmic.h"
#include "ModelStore.h"
#include "compositor/matte.h"
#include "compositor/subject.h"
#include <QJsonDocument>

using namespace compositor;
using namespace app::rpc;

namespace app {

void AutomationServer::registerPixelsHandlers() {
    MainWindow* w = window_;
    const SessionOf session{w};
    const DocumentOf document{w};

    // ---- destructive pixel edits on the active layer, inside the selection
    add("pixels.adjust", [session, document](const QJsonObject& p) {
        document();
        EditorSession* s = session();
        if (!s->canAdjustPixels()) fail("the active layer has no pixels to adjust; select a pixel layer");
        auto kind = adjustmentKindNamed(str(p, "kind"));
        if (!kind) fail("kind must be Levels, Curves, Hue/Saturation, Exposure, Gradient Map or Grain", invalidParams);
        QJsonObject settings = QJsonDocument::fromJson(QByteArray::fromStdString(AdjustmentSettings::defaults(*kind).toJson())).object();
        QJsonObject patch = obj(p, "settings");
        for (auto it = patch.begin(); it != patch.end(); ++it) settings[it.key()] = it.value();
        settings["kind"] = QString::fromUtf8(adjustmentKindName(*kind));
        AdjustmentSettings parsed;
        if (!AdjustmentSettings::parse(QJsonDocument(settings).toJson(QJsonDocument::Compact).toStdString(), parsed)) fail("couldn't parse settings; adjustments.defaults shows the shape", invalidParams);
        LayerTransform transform;
        auto source = s->adjustmentSource(0, transform);
        if (!source) fail("the active layer has no pixels; select a pixel layer with layers.select (document.overview shows each layer's kind)");
        auto out = std::make_shared<Image>(*source);
        applyAdjustment(parsed, *out, Rect(0, 0, source->width(), source->height()), 1);
        if (auto coverage = s->selectionOnGrid(transform, source->width(), source->height())) blendThroughCoverage(*out, *source, *coverage);
        s->commitPixels(out, transform, QString::fromUtf8(adjustmentKindName(*kind)));
        return QJsonObject{{"applied", QString::fromUtf8(adjustmentKindName(*kind))}};
    });
    add("pixels.filter", [session, document](const QJsonObject& p) {
        document();
        EditorSession* s = session();
        if (!s->canAdjustPixels()) fail("the active layer has no pixels to filter; select a pixel layer");
        auto kind = filterKindNamed(str(p, "kind"));
        if (!kind) fail("kind must be Gaussian Blur, Motion Blur, Add Noise or Lens Correction", invalidParams);
        FilterSettings settings;
        settings.radius = num(p, "radius", settings.radius);
        settings.angle = num(p, "angle", settings.angle);
        settings.distance = num(p, "distance", settings.distance);
        settings.amount = num(p, "amount", settings.amount);
        settings.gaussian = flag(p, "gaussian", settings.gaussian);
        settings.monochromatic = flag(p, "monochromatic", settings.monochromatic);
        settings.distortion = num(p, "distortion", settings.distortion);
        settings.bicubic = flag(p, "bicubic", settings.bicubic);
        int margin = int(std::ceil(blurMargin(*kind, settings)));
        LayerTransform transform;
        auto source = s->adjustmentSource(margin, transform);
        if (!source) fail("the active layer has no pixels; select a pixel layer with layers.select (document.overview shows each layer's kind)");
        auto out = std::make_shared<Image>(*source);
        applyFilter(*kind, *out, settings, 1, uint32_t(integer(p, "seed", 1)));
        if (auto coverage = s->selectionOnGrid(transform, source->width(), source->height())) blendThroughCoverage(*out, *source, *coverage);
        LayerTransform placed = transform;
        std::shared_ptr<const Image> image = out;
        if (*kind == FilterKind::GaussianBlur || *kind == FilterKind::MotionBlur) image = trimToPixels(*out, transform, placed);
        s->commitPixels(image, placed, QString::fromUtf8(filterKindName(*kind)));
        return QJsonObject{{"applied", QString::fromUtf8(filterKindName(*kind))}};
    });
    add("pixels.invert", [session, document](const QJsonObject&) { document(); session()->invertActive(); return QJsonObject{}; });
    add("pixels.fill", [session, document](const QJsonObject& p) {
        document();
        QColor color(str(p, "color", QStringLiteral("#000000")));
        if (!color.isValid()) fail("color must be a CSS colour such as #ff8800", invalidParams);
        session()->fillSelection(color);
        return QJsonObject{};
    });
    add("pixels.clear", [session, document](const QJsonObject&) { document(); session()->clearSelectionPixels(); return QJsonObject{}; });
    add("pixels.contentAwareFill", [session, document](const QJsonObject&) {
        document();
        QString error;
        if (!session()->contentAwareFill(&error)) fail(error.isEmpty() ? "content-aware fill needs a selection on a pixel layer" : error);
        return QJsonObject{};
    });
    add("pixels.gmic", [session, document](const QJsonObject& p) {
        // A G'MIC command on the active layer's pixels inside the selection, e.g. "unsharp 2,1.5" or "fx_dreamsmooth 3,0,1,0.8,0,0.8,0,24,0".
        document();
        EditorSession* s = session();
        if (!s->canAdjustPixels()) fail("the active layer has no pixels; select a pixel layer");
        QString command = str(p, "command").trimmed();
        if (command.isEmpty()) fail("command is empty", invalidParams);
        if (GmicRunner::executable().isEmpty()) fail("G'MIC is not installed (no gmic executable on PATH)");
        if (QString why; !GmicRunner::allowedForAutomation(command, &why)) fail(why, invalidParams);
        LayerTransform transform;
        auto source = s->adjustmentSource(0, transform);
        if (!source) fail("the active layer has no pixels; select a pixel layer with layers.select (document.overview shows each layer's kind)");
        QString error;
        auto result = GmicRunner::runSync(*source, command, &error, integer(p, "timeoutMs", 300000));
        if (!result) fail(error);
        if (auto coverage = s->selectionOnGrid(transform, source->width(), source->height())) blendThroughCoverage(*result, *source, *coverage);
        s->commitPixels(result, transform, "G'MIC: " + command.section(' ', 0, 0));
        return QJsonObject{{"applied", command}, {"gmic", GmicRunner::version()}};
    });
    add("gmic.filters", [](const QJsonObject& p) {
        // The catalogue: name, folder, command and parameters, optionally filtered by a search string; the
        // filters that do not work here are left out unless all: true (then they carry "unsupported").
        GmicCatalogue catalogue;
        QString path = GmicCatalogue::preferredFile();
        QJsonArray out;
        if (!path.isEmpty() && catalogue.load(path)) {
            QString needle = str(p, "search", QString()).trimmed();
            for (const GmicFilter& f : catalogue.filters()) {
                if (!needle.isEmpty() && !f.name.contains(needle, Qt::CaseInsensitive) && !f.folder.contains(needle, Qt::CaseInsensitive)) continue;
                const QString problem = GmicCatalogue::unsupported().value(f.command);
                if (!problem.isEmpty() && !flag(p, "all", false)) continue;
                QJsonArray params;
                for (const GmicParam& gp : f.params) {
                    if (!gp.contributes()) continue;
                    QJsonObject o{{"label", gp.label}};
                    switch (gp.kind) {
                    case GmicParam::Float: case GmicParam::Int: o["type"] = gp.kind == GmicParam::Int ? "int" : "float"; o["default"] = gp.value; o["min"] = gp.min; o["max"] = gp.max; break;
                    case GmicParam::Bool: o["type"] = "bool"; o["default"] = gp.value != 0; break;
                    case GmicParam::Choice: o["type"] = "choice"; o["default"] = int(gp.value); o["choices"] = QJsonArray::fromStringList(gp.choices); break;
                    case GmicParam::Color: o["type"] = "color"; o["default"] = gp.argument(); break;
                    default: o["type"] = "text"; o["default"] = gp.argument(); break;
                    }
                    params.append(o);
                }
                QJsonObject entry{{"name", f.name}, {"folder", f.folder}, {"command", f.command}, {"defaultCommand", f.commandLine(false)}, {"params", params}};
                if (!problem.isEmpty()) entry["unsupported"] = problem;
                out.append(entry);
            }
        }
        return QJsonObject{{"installed", !GmicRunner::executable().isEmpty()}, {"version", GmicRunner::version()}, {"catalogue", path}, {"filters", out}};
    });
    add("pixels.removeBackground", [session, document](const QJsonObject& p) {
        document();
        if (!ModelStore::supported()) fail("this build has no OpenCV, so the segmentation model can't run");
        if (!ModelStore::ready()) fail("Remove Background is off or its model isn't downloaded: enable it in Edit > Preferences (or run nekophoto --download-model isnet)");
        EditorSession* s = session();
        LayerTransform transform;
        auto source = s->adjustmentSource(0, transform);
        if (!source) fail("the active layer has no pixels; select a pixel layer with layers.select (document.overview shows each layer's kind)");
        std::string error;
        const std::string modelPath = ModelStore::pathFor(ModelStore::selected()).toStdString();
        const bool detail = flag(p, "detail", false), mirror = flag(p, "flip", ModelStore::mirrorAverage());
        auto mask = detail ? subjectMaskDetailed(*source, modelPath, nullptr, int(num(p, "detailWindows", 12)), &error, mirror) : subjectMask(*source, modelPath, &error, mirror);
        if (!mask) fail("the model failed: " + qs(error));
        std::shared_ptr<const Image> pixels;
        if (flag(p, "refine", true)) {
            MatteSettings settings;
            settings.refineEdges = num(p, "refineEdges", settings.refineEdges);
            settings.contrast = num(p, "contrast", settings.contrast);
            settings.matting = num(p, "matting", settings.matting);
            settings.shiftEdge = num(p, "shiftEdge", settings.shiftEdge);
            settings.cleanup = flag(p, "cleanup", settings.cleanup);
            settings.decontaminate = flag(p, "decontaminate", settings.decontaminate);
            mask = refineMatte(*mask, *source, settings, 0);
            if (settings.decontaminate) pixels = estimateForeground(*source, *mask);
        }
        s->applySubjectMask(mask, pixels);
        return QJsonObject{{"applied", true}, {"decontaminated", bool(pixels)}, {"detail", detail}};
    });

}

} // namespace app
