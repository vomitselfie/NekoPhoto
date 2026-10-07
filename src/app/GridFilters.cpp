#include "GridFilters.h"
#include <QJsonValue>
#include <cmath>

using namespace compositor;

namespace app {

namespace {

/// The choice key `kind` has and the FilterSettings field it sets: "mode", "size", "style", "type", "preserve" go to
/// `style`, "quality" to `quality`, "undefinedAreas" to `undefinedAreas`.
struct Choice { const char* key; QStringList names; int FilterSettings::*field; };

std::vector<Choice> choicesOf(FilterKind kind) {
    switch (kind) {
    case FilterKind::RadialBlur: return {{"quality", {"draft", "good", "best"}, &FilterSettings::quality}, {"mode", {"spin", "zoom"}, &FilterSettings::style}};
    case FilterKind::Spherize: return {{"mode", {"normal", "horizontalOnly", "verticalOnly"}, &FilterSettings::style}};
    case FilterKind::PolarCoordinates: return {{"mode", {"rectangularToPolar", "polarToRectangular"}, &FilterSettings::style}};
    case FilterKind::Ripple: return {{"size", {"small", "medium", "large"}, &FilterSettings::style}};
    case FilterKind::ZigZag: return {{"style", {"aroundCenter", "outFromCenter", "pondRipples"}, &FilterSettings::style}};
    case FilterKind::Wave: return {{"type", {"sine", "triangle", "square"}, &FilterSettings::style}, {"undefinedAreas", {"wrap", "repeat"}, &FilterSettings::undefinedAreas}};
    case FilterKind::Shear: return {{"undefinedAreas", {"wrap", "repeat"}, &FilterSettings::undefinedAreas}};
    case FilterKind::Offset: return {{"undefinedAreas", {"wrap", "repeat", "transparent"}, &FilterSettings::undefinedAreas}};
    case FilterKind::Maximum: case FilterKind::Minimum: return {{"preserve", {"squareness", "roundness"}, &FilterSettings::style}};
    default: return {};
    }
}

struct Number { const char* key; double FilterSettings::*real; int FilterSettings::*whole; };

std::vector<Number> numbersOf(FilterKind kind) {
    const Number radius{"radius", &FilterSettings::radius, nullptr}, amount{"amount", &FilterSettings::amount, nullptr},
        angle{"angle", &FilterSettings::angle, nullptr}, threshold{"threshold", nullptr, &FilterSettings::threshold};
    switch (kind) {
    case FilterKind::BoxBlur: case FilterKind::Median: case FilterKind::HighPass: case FilterKind::Maximum: case FilterKind::Minimum: return {radius};
    case FilterKind::SurfaceBlur: case FilterKind::DustAndScratches: return {radius, threshold};
    case FilterKind::UnsharpMask: return {amount, radius, threshold};
    case FilterKind::RadialBlur: case FilterKind::Pinch: case FilterKind::Spherize: case FilterKind::Ripple: case FilterKind::Shear: return {amount};
    case FilterKind::Emboss: return {angle, {"height", nullptr, &FilterSettings::height}, amount};
    case FilterKind::Mosaic: return {{"cellSize", nullptr, &FilterSettings::cellSize}};
    case FilterKind::Twirl: return {angle};
    case FilterKind::ZigZag: return {amount, {"ridges", &FilterSettings::ridges, nullptr}};
    case FilterKind::Wave:
        return {{"generators", nullptr, &FilterSettings::generators}, {"wavelengthMin", &FilterSettings::wavelengthMin, nullptr},
                {"wavelengthMax", &FilterSettings::wavelengthMax, nullptr}, {"amplitudeMin", &FilterSettings::amplitudeMin, nullptr},
                {"amplitudeMax", &FilterSettings::amplitudeMax, nullptr}};
    case FilterKind::Offset: return {{"horizontal", nullptr, &FilterSettings::horizontal}, {"vertical", nullptr, &FilterSettings::vertical}};
    default: return {};
    }
}

} // namespace

bool gridFilterSeeded(FilterKind kind) { return kind == FilterKind::Wave || kind == FilterKind::Clouds || kind == FilterKind::DifferenceClouds; }

QStringList gridFilterChoiceNames(FilterKind kind, const char* key) {
    for (const Choice& c : choicesOf(kind)) if (qstrcmp(c.key, key) == 0) return c.names;
    return {};
}

bool readGridFilterSettings(FilterKind kind, const QJsonObject& params, FilterSettings& settings, QString* error) {
    for (const Number& n : numbersOf(kind)) {
        const QJsonValue v = params.value(QLatin1String(n.key));
        if (v.isUndefined()) continue;
        if (!v.isDouble()) { if (error) *error = QStringLiteral("%1 must be a number").arg(QLatin1String(n.key)); return false; }
        if (n.real) settings.*n.real = v.toDouble();
        else settings.*n.whole = int(std::lround(v.toDouble()));
    }
    for (const Choice& c : choicesOf(kind)) {
        const QJsonValue v = params.value(QLatin1String(c.key));
        if (v.isUndefined()) continue;
        int index = -1;
        if (v.isString())
            for (int i = 0; i < c.names.size(); i++) if (c.names[i].compare(v.toString(), Qt::CaseInsensitive) == 0) index = i;
        if (index < 0) {
            if (error) *error = QStringLiteral("%1 must be one of %2").arg(QLatin1String(c.key), c.names.join(", "));
            return false;
        }
        settings.*c.field = index;
    }
    return true;
}

QJsonObject gridFilterStep(FilterKind kind, const FilterSettings& settings, uint32_t seed) {
    const FilterSettings s = settings.normalizedFor(kind);
    QJsonObject step{{"kind", QString::fromUtf8(filterKindName(kind))}};
    for (const Number& n : numbersOf(kind)) step[QLatin1String(n.key)] = n.real ? s.*n.real : double(s.*n.whole);
    for (const Choice& c : choicesOf(kind)) step[QLatin1String(c.key)] = c.names.value(s.*c.field);
    if (gridFilterSeeded(kind)) step["seed"] = int(seed % 1000000000u);
    return step;
}

GridFilterContext gridFilterContext(const EditorSession* session, const LayerTransform& transform, const AnyGray& coverage, uint32_t seed) {
    GridFilterContext c;
    c.mode = session->colorMode();
    if (coverage) coverageBounds(coverage, c);
    c.originX = int(std::lround(transform.origin.x));
    c.originY = int(std::lround(transform.origin.y));
    if (const auto& d = session->document()) c.documentSide = std::max(d->width, d->height);
    const QColor fg = session->foregroundColor.toRgb(), bg = session->backgroundColor.toRgb();
    c.foreground[0] = fg.redF(); c.foreground[1] = fg.greenF(); c.foreground[2] = fg.blueF();
    c.background[0] = bg.redF(); c.background[1] = bg.greenF(); c.background[2] = bg.blueF();
    c.seed = seed;
    return c;
}

} // namespace app
