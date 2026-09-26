#include "AutomationHandlers.h"
#include "compositor/png.h"
#include <QFileInfo>
#include <QJsonDocument>
#include <algorithm>
#include <cmath>

using namespace compositor;

namespace app::rpc {

void fail(const QString& message, int code) { throw RpcError(code, message); }

// ---- Parameter access ---------------------------------------------------------------------------

bool has(const QJsonObject& p, const char* key) { return p.contains(QLatin1String(key)) && !p.value(QLatin1String(key)).isNull(); }

double num(const QJsonObject& p, const char* key, std::optional<double> fallback) {
    if (!has(p, key)) { if (fallback) return *fallback; fail(QStringLiteral("missing number parameter '%1'").arg(key), invalidParams); }
    QJsonValue v = p.value(QLatin1String(key));
    if (!v.isDouble()) fail(QStringLiteral("parameter '%1' must be a number").arg(key), invalidParams);
    return v.toDouble();
}

int integer(const QJsonObject& p, const char* key, std::optional<int> fallback) {
    return int(std::lround(num(p, key, fallback ? std::optional<double>(*fallback) : std::nullopt)));
}

QString str(const QJsonObject& p, const char* key, std::optional<QString> fallback) {
    if (!has(p, key)) { if (fallback) return *fallback; fail(QStringLiteral("missing string parameter '%1'").arg(key), invalidParams); }
    QJsonValue v = p.value(QLatin1String(key));
    if (!v.isString()) fail(QStringLiteral("parameter '%1' must be a string").arg(key), invalidParams);
    return v.toString();
}

bool flag(const QJsonObject& p, const char* key, bool fallback) {
    if (!has(p, key)) return fallback;
    QJsonValue v = p.value(QLatin1String(key));
    if (!v.isBool()) fail(QStringLiteral("parameter '%1' must be true or false").arg(key), invalidParams);
    return v.toBool();
}

QJsonObject obj(const QJsonObject& p, const char* key) {
    if (!has(p, key)) return {};
    QJsonValue v = p.value(QLatin1String(key));
    if (!v.isObject()) fail(QStringLiteral("parameter '%1' must be an object").arg(key), invalidParams);
    return v.toObject();
}

QString qs(const std::string& s) { return QString::fromStdString(s); }

SelectionMode selectionMode(const QJsonObject& p) {
    QString m = str(p, "mode", QStringLiteral("replace")).toLower();
    if (m == "replace") return SelectionMode::Replace;
    if (m == "add") return SelectionMode::Add;
    if (m == "subtract") return SelectionMode::Subtract;
    if (m == "intersect") return SelectionMode::Intersect;
    fail("mode must be replace, add, subtract or intersect", invalidParams);
}

QString adjustmentKindList() {
    QStringList names;
    for (int i = 0; i < adjustmentKindCount; i++) names << QString::fromUtf8(adjustmentKindName(AdjustmentKind(i)));
    return names.join(", ");
}

std::optional<AdjustmentKind> adjustmentKindNamed(QString name) {
    name = name.toLower().remove('/').remove(' ').remove('-');
    for (int i = 0; i < adjustmentKindCount; i++) {
        QString n = QString::fromUtf8(adjustmentKindName(AdjustmentKind(i))).toLower().remove('/').remove(' ').remove('-').remove('&');
        if (n == name) return AdjustmentKind(i);
    }
    return std::nullopt;
}

namespace {
QString bare(QString name) { return name.toLower().remove(' ').remove('-').remove('_'); }
} // namespace

std::optional<BlendMode> blendModeNamed(const QString& name) {
    for (int i = 0; i < blendModeCount; i++)
        if (bare(QString::fromUtf8(blendModeName(BlendMode(i)))) == bare(name)) return BlendMode(i);
    return std::nullopt;
}

QStringList blendModeNames() {
    QStringList out;
    for (int i = 0; i < blendModeCount; i++) out << QString::fromUtf8(blendModeName(BlendMode(i)));
    return out;
}

std::optional<Sampling> samplingNamed(const QString& name) {
    const QString n = bare(name);
    if (n.isEmpty()) return std::nullopt;
    for (int i = 0; i < 3; i++)
        if (bare(QString::fromUtf8(samplingName(Sampling(i)))).startsWith(n)) return Sampling(i);
    return std::nullopt;
}

std::optional<FilterKind> filterKindNamed(QString name) {
    name = name.toLower().remove(' ').remove('-');
    for (int i = 0; i < 4; i++) {
        QString n = QString::fromUtf8(filterKindName(FilterKind(i))).toLower().remove(' ').remove('-');
        if (n == name) return FilterKind(i);
    }
    if (name == "gaussian" || name == "blur") return FilterKind::GaussianBlur;
    if (name == "motion") return FilterKind::MotionBlur;
    if (name == "noise") return FilterKind::AddNoise;
    if (name == "lens") return FilterKind::LensCorrection;
    return std::nullopt;
}

// ---- JSON views of the model --------------------------------------------------------------------

QJsonObject rectJson(const Rect& r) { return {{"x", r.x}, {"y", r.y}, {"width", r.width}, {"height", r.height}}; }

QJsonObject transformJson(const LayerTransform& t) {
    return {{"x", t.origin.x}, {"y", t.origin.y}, {"width", t.size.width}, {"height", t.size.height},
            {"rotation", t.rotation}, {"flipX", t.flipX}, {"flipY", t.flipY}, {"sampling", QString::fromUtf8(samplingName(t.sampling))}};
}

QJsonObject layerJson(const Layer& layer, int depth) {
    QJsonObject o{
        {"id", qs(layer.id)}, {"name", qs(layer.name)}, {"depth", depth},
        {"kind", layer.isGroup ? "group" : layer.adjustment ? "adjustment" : layer.isLiveShape() ? "shape" : layer.isLiveText() ? "text"
                 : layer.isLiveSmartObject() ? "smartObject" : "pixels"},
        {"visible", layer.visible}, {"opacity", layer.opacity}, {"blend", layer.isGroup && layer.passThrough ? QStringLiteral("Pass Through") : QString::fromUtf8(blendModeName(layer.blendMode))},
        {"clipping", layer.maskSourceId.has_value()}, {"transform", transformJson(layer.transform)},
    };
    if (layer.parentId) o["parent"] = qs(*layer.parentId);
    if (layer.isLiveSmartObject())
        o["smartObject"] = QJsonObject{{"source", qs(layer.smartObject->sourceId)}, {"locked", layer.smartObject->locked()},
                                       {"state", QString::fromUtf8(smartObjectLockDescription(layer.smartObject->lock))}};
    if (!layer.isGroup && !layer.adjustment) o["pixelSize"] = QJsonObject{{"width", layer.pixelWidth()}, {"height", layer.pixelHeight()}, {"blank", !layer.asset || !layer.asset->image}};
    if (layer.mask) o["mask"] = QJsonObject{{"enabled", layer.mask->enabled}, {"linked", layer.mask->linked}, {"placed", layer.mask->placement.has_value()}};
    if (layer.adjustment) {
        o["adjustmentKind"] = QString::fromUtf8(adjustmentKindName(layer.adjustment->kind));
        o["adjustment"] = QJsonDocument::fromJson(QByteArray::fromStdString(layer.adjustment->json)).object();
    }
    if (layer.isLiveText()) {
        const LayerText& t = *layer.text;
        o["text"] = QJsonObject{{"text", qs(t.text)}, {"font", qs(t.fontFamily)}, {"size", t.fontSize}, {"bold", t.bold}, {"italic", t.italic},
                                {"color", QColor::fromRgbF(float(t.red), float(t.green), float(t.blue)).name()}, {"align", t.alignment == 1 ? "center" : t.alignment == 2 ? "right" : "left"},
                                {"lineSpacing", t.lineSpacing}, {"letterSpacing", t.letterSpacing}};
        if (!t.runs.empty()) {
            // Text in several styles: each run, in UTF-16 units of the text (read-only here; text.set carries
            // changes into them).
            QJsonArray runs;
            for (const TextRun& r : t.runs) {
                QJsonObject rj{{"length", r.length}, {"font", qs(r.fontFamily)}, {"size", r.fontSize}, {"bold", r.bold}, {"italic", r.italic},
                               {"color", QColor::fromRgbF(float(r.red), float(r.green), float(r.blue)).name()}, {"letterSpacing", r.letterSpacing}};
                if (r.baselineShift != 0) rj["baselineShift"] = r.baselineShift;
                if (r.weight != 0) rj["weight"] = r.weight;
                if (r.caps != TextRun::Caps::Normal) rj["caps"] = r.caps == TextRun::Caps::Small ? "small" : "all";
                if (r.underline) rj["underline"] = true;
                if (r.strikethrough) rj["strikethrough"] = true;
                runs.append(rj);
            }
            QJsonObject text = o["text"].toObject();
            text["runs"] = runs;
            o["text"] = text;
        }
    }
    return o;
}

LayerText textFromParams(const QJsonObject& p, LayerText base) {
    if (has(p, "text")) base.text = str(p, "text").toStdString();
    if (has(p, "font")) base.fontFamily = str(p, "font").toStdString();
    if (has(p, "size")) { double size = num(p, "size", base.fontSize); if (!(size >= 1 && size <= 2000)) fail("size must be 1..2000 pixels", invalidParams); base.fontSize = size; }
    if (has(p, "bold")) base.bold = flag(p, "bold", base.bold);
    if (has(p, "italic")) base.italic = flag(p, "italic", base.italic);
    if (has(p, "color")) { QColor c(str(p, "color")); if (!c.isValid()) fail("color must be a CSS colour", invalidParams); base.red = c.redF(); base.green = c.greenF(); base.blue = c.blueF(); }
    if (has(p, "align")) {
        QString a = str(p, "align").toLower();
        if (a == "left") base.alignment = 0; else if (a == "center" || a == "centre") base.alignment = 1; else if (a == "right") base.alignment = 2;
        else fail("align must be left, center or right", invalidParams);
    }
    if (has(p, "lineSpacing")) base.lineSpacing = std::clamp(num(p, "lineSpacing", base.lineSpacing), 0.5, 5.0);
    if (has(p, "letterSpacing")) base.letterSpacing = std::clamp(num(p, "letterSpacing", base.letterSpacing), -20.0, 100.0);
    return base;
}

QJsonArray layersJson(const Document& doc) {
    QJsonArray out;
    for (const HierarchyEntry& e : hierarchyEntries(doc.layers, true)) out.append(layerJson(*e.layer, e.depth));
    return out;
}

QString base64Png(const Image& image) {
    std::vector<uint8_t> bytes;
    std::string error;
    if (!encodePngImage(image, bytes, 0, &error)) fail("PNG encoding failed: " + qs(error));
    return QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(bytes.data()), int(bytes.size())).toBase64());
}

QJsonObject deliverPng(const Image& image, const QJsonObject& p, QJsonObject result) {
    result["width"] = image.width();
    result["height"] = image.height();
    if (has(p, "path")) {
        QString path = str(p, "path");
        std::string error;
        if (!writePngImage(path.toStdString(), image, 0, &error)) fail("couldn't write " + path + ": " + qs(error));
        result["path"] = QFileInfo(path).absoluteFilePath();
    } else result["png"] = base64Png(image);
    return result;
}

std::shared_ptr<Image> scaledCopy(const Image& image, double maxSize) {
    int longest = std::max(image.width(), image.height());
    if (maxSize <= 0 || longest <= maxSize) return std::make_shared<Image>(image);
    double scale = maxSize / longest;
    int w = std::max(1, int(std::lround(image.width() * scale))), h = std::max(1, int(std::lround(image.height() * scale)));
    LayerTransform full(Point(0, 0), Size(image.width(), image.height()));
    return resampleLayer(image, full, full, w, h);
}

const Document& DocumentOf::operator()() const {
    EditorSession* s = w->session();
    if (!s->hasDocument()) fail("no document is open in the current tab; use document.new or document.open");
    return *s->document();
}

const Layer& LayerOf::operator()(const QJsonObject& p, const char* key) const {
    const Document& doc = document();
    QString id = str(p, key);
    const Layer* l = doc.find(id.toStdString());
    if (!l) fail("no layer with id " + id + "; document.overview or layers.list give the ids");
    return *l;
}

const Layer& LayerOrActive::operator()(const QJsonObject& p) const {
    if (has(p, "id")) return LayerOf{DocumentOf{w}}(p, "id");
    const Layer* l = w->session()->activeLayer();
    if (!l) { DocumentOf{w}(); fail("no active layer; pass an id or layers.select first"); }
    return *l;
}

} // namespace app::rpc
