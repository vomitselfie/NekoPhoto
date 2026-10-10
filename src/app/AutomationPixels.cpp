// Automation methods: pixels. Registered from AutomationServer::registerHandlers (Automation.cpp).
#include "Automation.h"
#include "AutomationHandlers.h"
#include "Gmic.h"
#include "GridFilters.h"
#include "ModelStore.h"
#include "MoshDialog.h"
#include "compositor/cameraraw.h"
#include "compositor/depth.h"
#include "compositor/matte.h"
#include "compositor/modeedit.h"
#include "compositor/subject.h"
#include <QJsonDocument>
#include <cmath>

using namespace compositor;
using namespace app::rpc;

namespace app {

namespace {
/// Pixel methods refuse a smart object (Photoshop asks; an agent is told what to do instead).
void refuseSmartObject(EditorSession* s) {
    if (s->smartObjectBlocksPixels())
        fail("the active layer is a smart object: edit its contents (smartObject.editContents) or rasterize it (smartObject.rasterize) first", invalidParams);
}
} // namespace

void AutomationServer::registerPixelsHandlers() {
    MainWindow* w = window_;
    const SessionOf session{w};
    const DocumentOf document{w};

    // ---- destructive pixel edits on the active layer, inside the selection
    add("pixels.adjust", [session, document](const QJsonObject& p) {
        refuseSmartObject(session());
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
        // Photoshop's 32-bit set only (Brightness/Contrast, Posterize, Threshold, Selective Color and Grain are not in it).
        const std::string feature = std::string("adjustment.") + adjustmentKindName(*kind);
        if (!s->supportsFeature(feature)) fail(QString::fromUtf8(adjustmentKindName(*kind)) + ": " + s->unavailableTip(feature), invalidParams);
        if (s->colorMode() != ColorMode::RGB) {
            // CMYK and Lab: the layer's own samples (modeedit.h).
            const AnyImage source = s->adjustmentSourceAny(0, transform);
            if (!source) fail("the active layer has no pixels; select a pixel layer with layers.select (document.overview shows each layer's kind)");
            AnyImage out = adjustedInMode(parsed, source, s->colorMode(), s->document()->profile);
            if (!out) fail(QString::fromUtf8(adjustmentKindName(*kind)) + ": " + s->unavailableTip(feature), invalidParams);
            if (AnyGray coverage = s->selectionOnGridAny(transform, out.width(), out.height())) out = blendThroughCoverageAny(out, source, coverage);
            s->commitPixels(out, transform, QString::fromUtf8(adjustmentKindName(*kind)));
            return QJsonObject{{"applied", QString::fromUtf8(adjustmentKindName(*kind))}};
        }
        if (auto deep = s->adjustmentSourceF(0, transform)) {
            auto out = std::make_shared<ImageF>(*deep);
            applyAdjustment(parsed, *out, Rect(0, 0, deep->width(), deep->height()), 1, s->documentCurve());
            if (auto coverage = s->selectionOnGridF(transform, deep->width(), deep->height())) blendThroughCoverage(*out, *deep, *coverage);
            s->commitPixels(ImageFPtr(out), transform, QString::fromUtf8(adjustmentKindName(*kind)));
            return QJsonObject{{"applied", QString::fromUtf8(adjustmentKindName(*kind))}};
        }
        if (auto deep = s->adjustmentSource16(0, transform)) {
            auto out = std::make_shared<Image16>(*deep);
            applyAdjustment(parsed, *out, Rect(0, 0, deep->width(), deep->height()), 1);
            if (auto coverage = s->selectionOnGrid16(transform, deep->width(), deep->height())) blendThroughCoverage(*out, *deep, *coverage);
            s->commitPixels(Image16Ptr(out), transform, QString::fromUtf8(adjustmentKindName(*kind)));
            return QJsonObject{{"applied", QString::fromUtf8(adjustmentKindName(*kind))}};
        }
        auto source = s->adjustmentSource(0, transform);
        if (!source) fail("the active layer has no pixels; select a pixel layer with layers.select (document.overview shows each layer's kind)");
        auto out = std::make_shared<Image>(*source);
        applyAdjustment(parsed, *out, Rect(0, 0, source->width(), source->height()), 1);
        if (auto coverage = s->selectionOnGrid(transform, source->width(), source->height())) blendThroughCoverage(*out, *source, *coverage);
        s->commitPixels(out, transform, QString::fromUtf8(adjustmentKindName(*kind)));
        return QJsonObject{{"applied", QString::fromUtf8(adjustmentKindName(*kind))}};
    });
    add("pixels.filter", [session, document](const QJsonObject& p) {
        refuseSmartObject(session());
        document();
        EditorSession* s = session();
        if (!s->canAdjustPixels()) fail("the active layer has no pixels to filter; select a pixel layer");
        auto kind = filterKindNamed(str(p, "kind"));
        if (!kind) {
            QStringList names;
            for (int i = 0; i < filterKindCount; i++) names << QString::fromUtf8(filterKindName(FilterKind(i)));
            fail("kind must be one of " + names.join(", "), invalidParams);
        }
        if (isGridFilter(*kind)) {
            // The grid filters (filters.h): any depth and layout their feature supports, through one path.
            const QString name = QString::fromUtf8(filterKindName(*kind));
            const std::string feature = std::string("filter.") + filterKindName(*kind);
            if (!s->supportsFeature(feature)) fail(name + ": " + s->unavailableTip(feature), invalidParams);
            FilterSettings grid = FilterSettings::defaults(*kind);
            QString error;
            if (!readGridFilterSettings(*kind, p, grid, &error)) fail(name + ": " + error, invalidParams);
            LayerTransform transform;
            const AnyImage source = s->adjustmentSourceAny(gridFilterMargin(*kind, grid), transform);
            if (!source) fail("the active layer has no pixels, or it is too large to grow for this filter");
            const AnyGray coverage = s->selectionOnGridAny(transform, source.width(), source.height());
            const GridFilterContext context = gridFilterContext(s, transform, coverage, uint32_t(integer(p, "seed", 1)));
            AnyImage out = applyGridFilter(*kind, source, grid, context);
            if (!out) fail(name + ": " + s->unavailableTip(feature), invalidParams);
            if (coverage) out = blendThroughCoverageAny(out, source, coverage);
            LayerTransform placed = transform;
            if (gridFilterTrims(*kind)) {
                bool empty = false;
                if (AnyImage trimmed = trimToPixelsAny(out, transform, placed, &empty)) out = trimmed;
            }
            s->commitPixels(out, placed, name);
            return QJsonObject{{"applied", name}};
        }
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
        const bool trims = *kind == FilterKind::GaussianBlur || *kind == FilterKind::MotionBlur;
        if (s->colorMode() != ColorMode::RGB) {
            // CMYK and Lab: every ink, or L, a and b (modeedit.h).
            const AnyImage source = s->adjustmentSourceAny(margin, transform);
            if (!source) fail("the active layer has no pixels, or it is too large to grow for this filter");
            AnyImage out = filteredInMode(*kind, source, s->colorMode(), settings, 1, uint32_t(integer(p, "seed", 1)));
            if (!out) fail(QString::fromUtf8(filterKindName(*kind)) + ": the layer's pixels are not at the document's layout", invalidParams);
            if (AnyGray coverage = s->selectionOnGridAny(transform, out.width(), out.height())) out = blendThroughCoverageAny(out, source, coverage);
            LayerTransform placed = transform;
            if (trims) {
                bool empty = false;
                if (AnyImage trimmed = trimToPixelsAny(out, transform, placed, &empty)) out = trimmed;
            }
            s->commitPixels(out, placed, QString::fromUtf8(filterKindName(*kind)));
            return QJsonObject{{"applied", QString::fromUtf8(filterKindName(*kind))}};
        }
        if (auto deep = s->adjustmentSourceF(margin, transform)) {
            auto out = std::make_shared<ImageF>(*deep);
            applyFilter(*kind, *out, settings, s->documentCurve(), 1, uint32_t(integer(p, "seed", 1)));
            if (auto coverage = s->selectionOnGridF(transform, deep->width(), deep->height())) blendThroughCoverage(*out, *deep, *coverage);
            LayerTransform placed = transform;
            ImageFPtr image = out;
            if (trims) image = trimToPixels(*out, transform, placed);
            s->commitPixels(image, placed, QString::fromUtf8(filterKindName(*kind)));
            return QJsonObject{{"applied", QString::fromUtf8(filterKindName(*kind))}};
        }
        if (auto deep = s->adjustmentSource16(margin, transform)) {
            auto out = std::make_shared<Image16>(*deep);
            applyFilter(*kind, *out, settings, 1, uint32_t(integer(p, "seed", 1)));
            if (auto coverage = s->selectionOnGrid16(transform, deep->width(), deep->height())) blendThroughCoverage(*out, *deep, *coverage);
            LayerTransform placed = transform;
            Image16Ptr image = out;
            if (trims) image = trimToPixels(*out, transform, placed);
            s->commitPixels(image, placed, QString::fromUtf8(filterKindName(*kind)));
            return QJsonObject{{"applied", QString::fromUtf8(filterKindName(*kind))}};
        }
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
    add("pixels.mosh", [session, document](const QJsonObject& p) {
        // Filter > Mosh: one of OpenMosh's effects by id, its parameters by OpenMosh's keys (docs/mosh.md).
        refuseSmartObject(session());
        document();
        EditorSession* s = session();
        if (!s->canAdjustPixels()) fail("the active layer has no pixels to filter; select a pixel layer");
        const QString id = str(p, "effect");
        const mosh::EffectSpec* spec = mosh::findEffect(id.toStdString());
        if (!spec) {
            QStringList ids;
            for (const auto& e : mosh::effects()) ids << QString::fromUtf8(e.id.data(), qsizetype(e.id.size()));
            fail("effect must be one of " + ids.join(", "), invalidParams);
        }
        mosh::Settings settings = mosh::Settings::defaults(*spec);
        settings.seed = float(num(p, "seed", 0));
        const QJsonObject given = obj(p, "params");
        for (auto it = given.begin(); it != given.end(); ++it) {
            const std::string key = it.key().toStdString();
            const mosh::ParamSpec* param = nullptr;
            for (const auto& q : spec->params) if (q.key == key) param = &q;
            if (!param) {
                QStringList keys;
                for (const auto& q : spec->params) keys << QString::fromUtf8(q.key.data(), qsizetype(q.key.size()));
                fail("params." + it.key() + ": " + id + " takes " + keys.join(", "), invalidParams);
            }
            float value = 0;
            if (it->isBool()) value = it->toBool() ? 1.0f : 0.0f;
            else if (it->isDouble()) value = float(it->toDouble());
            else if (it->isString() && param->kind == mosh::ParamKind::Choice) {
                int index = -1;
                for (size_t i = 0; i < param->options.size(); i++)
                    if (it->toString().compare(QString::fromUtf8(param->options[i].data(), qsizetype(param->options[i].size())), Qt::CaseInsensitive) == 0) index = int(i);
                if (index < 0) fail("params." + it.key() + ": not one of the options", invalidParams);
                value = float(index);
            } else fail("params." + it.key() + " must be a number" + QString(param->kind == mosh::ParamKind::Bool ? " or a boolean" : param->kind == mosh::ParamKind::Choice ? " or an option's name" : ""), invalidParams);
            settings.set(key, value);
        }
        settings = settings.normalized();
        // Overlay and Mask read another layer (by id, where it lies over this one); Caption stamps text.
        std::optional<Uuid> auxLayer;
        if (has(p, "layer")) {
            if (!spec->auxImage) fail("layer: " + id + " reads no other layer (overlay and mask do)", invalidParams);
            const LayerOf layerOf{document};
            const Layer& layer = layerOf(p, "layer");
            if (layer.isGroup || layer.adjustment || !layer.asset || !layer.asset->image) fail("layer: that layer has no pixels", invalidParams);
            auxLayer = layer.id;
        } else if (spec->auxImage) fail(id + " reads another layer: give its id in layer (document.overview lists them)", invalidParams);
        const QString text = str(p, "text", QString());
        if (has(p, "text") && !spec->text) fail("text: only caption takes text", invalidParams);
        if (spec->text && text.trimmed().isEmpty()) fail("caption needs text", invalidParams);
        const QString name = QString::fromUtf8(spec->name.data(), qsizetype(spec->name.size()));
        LayerTransform transform;
        if (auto deep = s->adjustmentSource16(0, transform)) {
            const MoshExtras more = moshExtras(*spec, settings, document(), auxLayer, text, transform, deep->width(), deep->height());
            auto out = std::make_shared<Image16>(*deep);
            mosh::apply(settings, *out, more.sources());
            if (auto coverage = s->selectionOnGrid16(transform, deep->width(), deep->height())) blendThroughCoverage(*out, *deep, *coverage);
            s->commitPixels(Image16Ptr(out), transform, name);
        } else {
            auto source = s->adjustmentSource(0, transform);
            if (!source) fail("the active layer has no pixels; select a pixel layer with layers.select (document.overview shows each layer's kind)");
            const MoshExtras more = moshExtras(*spec, settings, document(), auxLayer, text, transform, source->width(), source->height());
            auto out = std::make_shared<Image>(*source);
            mosh::apply(settings, *out, more.sources());
            if (auto coverage = s->selectionOnGrid(transform, source->width(), source->height())) blendThroughCoverage(*out, *source, *coverage);
            s->commitPixels(std::shared_ptr<const Image>(out), transform, name);
        }
        QJsonObject applied = moshRequest(settings, auxLayer, text);
        applied["applied"] = applied.take("effect");
        return applied;
    });
    add("pixels.cameraRaw", [session, document](const QJsonObject& p) {
        // Filter > Camera Raw Filter: `settings` has the model's keys (compositor/cameraraw.h); omitted keys keep their defaults.
        refuseSmartObject(session());
        document();
        EditorSession* s = session();
        if (!s->canAdjustPixels()) fail("the active layer has no pixels to filter; select a pixel layer");
        CameraRawSettings settings;
        std::string error;
        const QByteArray text = QJsonDocument(obj(p, "settings")).toJson(QJsonDocument::Compact);
        if (!CameraRawSettings::parse(text.toStdString(), settings, &error)) fail("settings." + qs(error), invalidParams);
        if (settings.rawTemperature != 0 || settings.rawTint != 0
            || (settings.whiteBalance != CameraRawWhiteBalance::Custom && settings.whiteBalance != CameraRawWhiteBalance::Auto))
            fail("settings: rawTemperature, rawTint and the As Shot and preset white balances apply to camera RAW files; the filter's "
                 "whiteBalance is Custom or Auto with relative temperature and tint", invalidParams);
        if (settings.whiteBalance == CameraRawWhiteBalance::Auto && !obj(p, "settings").contains("temperature") && !obj(p, "settings").contains("tint")) {
            // Auto without explicit numbers: the gray-world balance of the layer, as the dialog's White Balance > Auto.
            LayerTransform probe;
            std::optional<std::array<double, 2>> solved;
            if (auto deep = s->adjustmentSource16(0, probe)) solved = CameraRawSettings::autoBalance(*deep);
            else if (auto layer = s->adjustmentSource(0, probe)) solved = CameraRawSettings::autoBalance(*layer);
            if (solved) {
                settings.temperature = std::clamp((*solved)[0], -100.0, 100.0);
                settings.tint = std::clamp((*solved)[1], -100.0, 100.0);
            }
        }
        const CameraRawSettings normalized = settings.normalized();
        QJsonObject applied = QJsonDocument::fromJson(QByteArray::fromStdString(normalized.toJson())).object();
        if (normalized.isIdentity()) return QJsonObject{{"applied", false}, {"settings", applied}};
        LayerTransform transform;
        if (auto deep = s->adjustmentSource16(0, transform)) {
            auto out = std::make_shared<Image16>(*deep);
            if (!applyCameraRaw(*out, normalized, 1, uint32_t(integer(p, "seed", 1)))) fail("settings are out of range", invalidParams);
            if (auto coverage = s->selectionOnGrid16(transform, deep->width(), deep->height())) blendThroughCoverage(*out, *deep, *coverage);
            s->commitPixels(Image16Ptr(out), transform, "Camera Raw Filter");
            return QJsonObject{{"applied", true}, {"settings", applied}};
        }
        auto source = s->adjustmentSource(0, transform);
        if (!source) fail("the active layer has no pixels; select a pixel layer with layers.select (document.overview shows each layer's kind)");
        auto out = std::make_shared<Image>(*source);
        if (!applyCameraRaw(*out, normalized, 1, uint32_t(integer(p, "seed", 1)))) fail("settings are out of range", invalidParams);
        if (auto coverage = s->selectionOnGrid(transform, source->width(), source->height())) blendThroughCoverage(*out, *source, *coverage);
        s->commitPixels(out, transform, "Camera Raw Filter");
        return QJsonObject{{"applied", true}, {"settings", applied}};
    });
    add("pixels.invert", [session, document](const QJsonObject&) {
        refuseSmartObject(session()); document(); session()->invertActive(); return QJsonObject{}; });
    add("pixels.fill", [session, document](const QJsonObject& p) {
        refuseSmartObject(session());
        document();
        QColor color(str(p, "color", QStringLiteral("#000000")));
        if (!color.isValid()) fail("color must be a CSS colour such as #ff8800", invalidParams);
        session()->fillSelection(color);
        return QJsonObject{};
    });
    add("pixels.clear", [session, document](const QJsonObject&) {
        refuseSmartObject(session()); document(); session()->clearSelectionPixels(); return QJsonObject{}; });
    // The pixel clipboard: Edit > Copy, Copy Merged, Cut and Paste. Other apps get the pixels at 8 bits, sRGB; a
    // paste converts them to the document's mode, profile and depth.
    add("pixels.copy", [session, document](const QJsonObject&) {
        document();
        if (!session()->copySelection()) fail("nothing to copy: select a layer with pixels (or its mask), and pixels of it with the selection");
        return QJsonObject{{"copied", true}};
    });
    add("pixels.copyMerged", [session, document](const QJsonObject&) {
        document();
        if (!session()->copyMerged()) fail("nothing to copy: the selection is empty");
        return QJsonObject{{"copied", true}};
    });
    add("pixels.cut", [session, document](const QJsonObject&) {
        refuseSmartObject(session());
        document();
        if (!session()->cutSelection()) fail("nothing to cut: make a selection on a layer with pixels");
        return QJsonObject{{"copied", true}};
    });
    add("pixels.paste", [session, document](const QJsonObject&) {
        document();
        EditorSession* s = session();
        if (!s->hasPixelsToPaste()) fail("no pixels are on the clipboard (pixels.copy puts them there; layers.paste pastes copied layers)");
        QString why;
        if (!s->pastePixels(&why)) fail(why.isEmpty() ? QStringLiteral("the pixels could not be pasted here") : why);
        const Layer* l = s->activeLayer();
        return l ? layerJson(*l, 0) : QJsonObject{};
    });
    add("pixels.contentAwareFill", [session, document](const QJsonObject& p) {
        refuseSmartObject(session());
        document();
        ContentFillRequest request;
        const QString sampling = str(p, "sampling", QStringLiteral("auto")).toLower();
        // No user-drawn sampling region (docs/legal-boundaries.md, "Content-Aware Fill").
        if (sampling == "custom") fail("custom sampling areas are not available: use auto or all", invalidParams);
        if (sampling == "all") request.sampling = ContentFillRequest::Sampling::All;
        else if (sampling != "auto") fail("sampling must be auto or all", invalidParams);
        const QString output = str(p, "output", QStringLiteral("current")).toLower();
        if (output != "current" && output != "new") fail("output must be current or new", invalidParams);
        request.newLayer = output == "new";
        QString error;
        if (!session()->contentAwareFill(&error, request)) fail(error.isEmpty() ? "content-aware fill needs a selection on a pixel layer" : error);
        QJsonObject answer{{"filled", true}};
        if (request.newLayer && session()->activeLayerId()) answer["layer"] = qs(*session()->activeLayerId());
        return answer;
    });
    add("pixels.contentAwareMove", [session, document](const QJsonObject& p) {
        refuseSmartObject(session());
        document();
        EditorSession* s = session();
        const QString mode = str(p, "mode", QStringLiteral("move")).toLower();
        if (mode != "move" && mode != "extend") fail("mode must be move or extend", invalidParams);
        const int adaptation = int(std::lround(num(p, "adaptation", 2)));
        if (adaptation < 0 || adaptation > 4) fail("adaptation must be 0 (very strict) to 4 (very loose)", invalidParams);
        const bool extendBefore = s->contentMoveExtend;
        const int adaptationBefore = s->contentMoveAdaptation;
        s->contentMoveExtend = mode == "extend";
        s->contentMoveAdaptation = adaptation;
        QString error;
        const bool moved = s->contentAwareMove(int(std::lround(num(p, "dx"))), int(std::lround(num(p, "dy"))), &error);
        s->contentMoveExtend = extendBefore;
        s->contentMoveAdaptation = adaptationBefore;
        if (!moved) fail(error.isEmpty() ? "nothing was moved: make a selection on a pixel layer and give an offset (dx, dy)" : error);
        return QJsonObject{{"moved", true}, {"mode", mode}};
    });
    add("pixels.contentAwareScale", [session, document](const QJsonObject& p) {
        refuseSmartObject(session());
        document();
        const Layer* layer = session()->activeLayer();
        if (!layer || layer->isGroup || !layer->asset || !layer->asset->image) fail("select an image layer");
        const int w0 = layer->asset->image.width(), h0 = layer->asset->image.height();
        int w = p.contains("width") ? p.value("width").toInt() : int(std::lround(w0 * p.value("widthPercent").toDouble(100) / 100));
        int h = p.contains("height") ? p.value("height").toInt() : int(std::lround(h0 * p.value("heightPercent").toDouble(100) / 100));
        QString error;
        if (!session()->contentAwareScale(w, h, p.value("protectSelection").toBool(false), &error)) fail(error, invalidParams);
        return QJsonObject{{"width", w}, {"height", h}};
    });
    add("pixels.gmic", [session, document](const QJsonObject& p) {
        refuseSmartObject(session());
        // A G'MIC command on the active layer's pixels inside the selection, e.g. "unsharp 2,1.5" or "fx_dreamsmooth 3,0,1,0.8,0,0.8,0,24,0".
        document();
        EditorSession* s = session();
        if (!s->canAdjustPixels()) fail("the active layer has no pixels; select a pixel layer");
        QString command = str(p, "command").trimmed();
        if (command.isEmpty()) fail("command is empty", invalidParams);
        if (!GmicRunner::available()) fail("G'MIC is not installed (no gmic executable was found; Filter > G'MIC can download it on Windows)");
        if (QString why; !GmicRunner::allowedForAutomation(command, &why)) fail(why, invalidParams);
        // The undo step's name after "G'MIC: ": the filter's name the dialog shows, else the command's first word.
        const QString stepName = has(p, "name") && !str(p, "name").trimmed().isEmpty() ? str(p, "name").trimmed() : command.section(' ', 0, 0);
        LayerTransform transform;
        if (auto deep = s->adjustmentSource16(0, transform)) {
            QString error;
            auto result = GmicRunner::runSync(*deep, command, &error, std::clamp(integer(p, "timeoutMs", 300000), 1, 600000));
            if (!result) fail(error);
            if (auto coverage = s->selectionOnGrid16(transform, deep->width(), deep->height())) blendThroughCoverage(*result, *deep, *coverage);
            s->commitPixels(Image16Ptr(result), transform, "G'MIC: " + stepName);
            return QJsonObject{{"applied", command}, {"gmic", GmicRunner::version()}};
        }
        auto source = s->adjustmentSource(0, transform);
        if (!source) fail("the active layer has no pixels; select a pixel layer with layers.select (document.overview shows each layer's kind)");
        QString error;
        auto result = GmicRunner::runSync(*source, command, &error, std::clamp(integer(p, "timeoutMs", 300000), 1, 600000));
        if (!result) fail(error);
        if (auto coverage = s->selectionOnGrid(transform, source->width(), source->height())) blendThroughCoverage(*result, *source, *coverage);
        s->commitPixels(result, transform, "G'MIC: " + stepName);
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
        refuseSmartObject(session());
        document();
        if (!ModelStore::supported()) fail("this build has no OpenCV, so the segmentation model can't run");
        if (!ModelStore::ready()) fail("Remove Background is off or its model isn't downloaded: enable it in Edit > Preferences (or run nekophoto --download-model isnet)");
        EditorSession* s = session();
        LayerTransform transform;
        // A 16-bit layer: the model and the refinement's guide see it reduced to 8 bits (the model takes 8-bit input),
        // the matte stays float to the end and becomes a 16-bit mask, and the edge colours are estimated at 16 bits.
        auto deep = s->adjustmentSource16(0, transform);
        auto source = deep ? std::shared_ptr<const Image>(narrowImage(*deep)) : s->adjustmentSource(0, transform);
        if (!source) fail("the active layer has no pixels; select a pixel layer with layers.select (document.overview shows each layer's kind)");
        std::string error;
        const bool detail = flag(p, "detail", false), mirror = flag(p, "flip", ModelStore::mirrorAverage());
        // The mask the dialog showed, when it was made from these pixels (Remove Background's OK runs this method).
        auto mask = ModelStore::subjectMask(*source, ModelStore::pathFor(ModelStore::selected()), mirror, detail ? std::max(1, int(num(p, "detailWindows", 12))) : 0, &error);
        if (!mask) fail("the model failed: " + qs(error));
        MatteSettings settings;
        settings.refineEdges = num(p, "refineEdges", settings.refineEdges);
        settings.contrast = num(p, "contrast", settings.contrast);
        settings.matting = num(p, "matting", settings.matting);
        settings.shiftEdge = num(p, "shiftEdge", settings.shiftEdge);
        settings.cleanup = flag(p, "cleanup", settings.cleanup);
        settings.decontaminate = flag(p, "decontaminate", settings.decontaminate);
        const bool refine = flag(p, "refine", true);
        if (deep) {
            AlphaPlane plane(*mask);
            if (refine) plane = refineMatte(plane, *source, settings, 0);
            std::shared_ptr<const Image16> pixels;
            if (refine && settings.decontaminate) pixels = estimateForeground(*deep, plane);
            s->applySubjectMask(plane.toGray16(), pixels);
            return QJsonObject{{"applied", true}, {"decontaminated", bool(pixels)}, {"detail", detail}};
        }
        std::shared_ptr<const Image> pixels;
        if (refine) {
            mask = refineMatte(*mask, *source, settings, 0);
            if (settings.decontaminate) pixels = estimateForeground(*source, *mask);
        }
        s->applySubjectMask(mask, pixels);
        return QJsonObject{{"applied", true}, {"decontaminated", bool(pixels)}, {"detail", detail}};
    });

}

} // namespace app
