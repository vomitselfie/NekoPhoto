// Mosh: the effect registry, the runtime's maths and the conversion between premultiplied pixels and the runtime's
// straight frames (compositor/mosh.h, mosh_runtime.h). The effects themselves are in mosh_effects.cpp.
//
// The registry is OpenMosh's (MIT, crates/mosh-core/src/registry.rs): ids, labels, ranges and defaults as it has
// them, so presets and parameter values carry over.
#include "compositor/mosh.h"
#include "mosh_effects.h"
#include "mosh_runtime.h"
#include <algorithm>
#include <cmath>

namespace compositor::mosh {

namespace {

constexpr float pi = 3.14159265358979323846f;
constexpr float tau = 6.28318530717958647692f;

ParamSpec floatParam(std::string_view key, std::string_view label, float min, float max, float def) {
    return ParamSpec{key, label, ParamKind::Float, min, max, def, {}};
}
ParamSpec boolParam(std::string_view key, std::string_view label, bool def) {
    return ParamSpec{key, label, ParamKind::Bool, 0, 1, def ? 1.0f : 0.0f, {}};
}
ParamSpec choiceParam(std::string_view key, std::string_view label, std::vector<std::string_view> options, int def) {
    float top = float(options.size()) - 1;
    return ParamSpec{key, label, ParamKind::Choice, 0, top, float(def), std::move(options)};
}

std::vector<EffectSpec> makeEffects() {
    using C = Category;
    auto fx = [](std::string_view id, std::string_view name, C category, std::vector<ParamSpec> params) {
        return EffectSpec{id, name, category, false, std::move(params)};
    };
    auto seeded = [](std::string_view id, std::string_view name, C category, std::vector<ParamSpec> params) {
        return EffectSpec{id, name, category, true, std::move(params)};
    };
    return {
        // ---- Glitch
        fx("soft-glitch", "Soft Glitch", C::Glitch, {floatParam("amount", "Amount", 0, 1, 0.3f), floatParam("angle", "Angle", 0, tau, 0)}),
        seeded("hard-glitch", "Hard Glitch", C::Glitch,
               {floatParam("amount", "Amount", 0, 1, 0.4f), floatParam("blocks", "Blocks", 2, 64, 12), floatParam("shift", "Color Shift", 0, 1, 0.5f)}),
        seeded("decimate", "Decimate", C::Glitch, {floatParam("amount", "Amount", 0, 1, 0.5f), floatParam("size", "Block Size", 2, 128, 24)}),
        seeded("data-mosh", "Data-Mosh", C::Glitch,
               {floatParam("size", "Block Size", 4, 128, 32), floatParam("drift", "Drift", 0, 1, 0.4f), floatParam("stick", "Stuck Blocks", 0, 1, 0.5f)}),
        fx("splitter", "Splitter", C::Glitch,
           {floatParam("strips", "Strips", 1, 64, 8), floatParam("offset", "Offset", 0, 1, 0.2f), boolParam("vertical", "Vertical", false)}),
        seeded("jitter", "Jitter", C::Glitch, {floatParam("amount", "Amount", 0, 1, 0.3f), floatParam("size", "Band Height", 1, 100, 20)}),
        seeded("slices", "Slices", C::Glitch,
               {floatParam("count", "Count", 1, 40, 10), floatParam("offset", "Offset", 0, 1, 0.15f), boolParam("vertical", "Vertical", false)}),
        seeded("shake", "Shake", C::Glitch, {floatParam("amount", "Amount", 0, 0.5f, 0.1f)}),
        seeded("pixel-sort", "Pixel Sort", C::Glitch,
               {floatParam("low", "Threshold Low", 0, 1, 0.25f), floatParam("high", "Threshold High", 0, 1, 0.85f),
                boolParam("reverse", "Reverse", false), boolParam("vertical", "Vertical", false)}),
        fx("strobe", "Strobe", C::Glitch,
           {floatParam("phase", "Phase", 0, 1, 0), floatParam("rate", "Rate", 0, 30, 10), choiceParam("mode", "Mode", {"Blackout", "Whiteout", "Invert"}, 0)}),
        // ---- Distort
        fx("wave", "Wave", C::Distort,
           {floatParam("amplitude", "Amplitude", 0, 0.5f, 0.05f), floatParam("frequency", "Frequency", 0, 50, 8), floatParam("phase", "Phase", 0, tau, 0),
            boolParam("vertical", "Vertical", false)}),
        fx("bulge", "Bulge", C::Distort,
           {floatParam("strength", "Strength", -1, 1, 0.5f), floatParam("radius", "Radius", 0.05f, 1.5f, 0.5f), floatParam("cx", "Center X", 0, 1, 0.5f),
            floatParam("cy", "Center Y", 0, 1, 0.5f)}),
        fx("stretch", "Stretch", C::Distort,
           {floatParam("center", "Center", 0, 1, 0.5f), floatParam("width", "Width", 0.02f, 1, 0.25f), floatParam("amount", "Amount", 0, 4, 1.5f),
            boolParam("vertical", "Vertical", false)}),
        fx("push", "Push", C::Distort, {floatParam("dx", "Push X", -1, 1, 0), floatParam("dy", "Push Y", -1, 1, 0), boolParam("wrap", "Wrap", true)}),
        fx("luma-mesh", "Luma-Mesh", C::Distort, {floatParam("amount", "Amount", 0, 0.3f, 0.08f), floatParam("angle", "Angle", 0, tau, pi / 2)}),
        fx("transform-3d", "3D Transform", C::Distort,
           {floatParam("scale", "Scale", 0.1f, 4, 1), floatParam("rotation", "Rotation", -pi, pi, 0), floatParam("x", "Offset X", -1, 1, 0),
            floatParam("y", "Offset Y", -1, 1, 0), floatParam("tilt_x", "Tilt X", -1, 1, 0), floatParam("tilt_y", "Tilt Y", -1, 1, 0)}),
        fx("tile", "Tile", C::Distort, {floatParam("cols", "Columns", 1, 16, 3), floatParam("rows", "Rows", 1, 16, 3), boolParam("mirror", "Mirror", true)}),
        fx("kaleidoscope", "Kaleidoscope", C::Distort, {floatParam("segments", "Segments", 2, 24, 6), floatParam("rotation", "Rotation", 0, tau, 0)}),
        fx("mirror", "Mirror", C::Distort, {choiceParam("mode", "Mode", {"Left → Right", "Right → Left", "Top → Bottom", "Bottom → Top"}, 0)}),
        fx("wobble", "Wobble", C::Distort,
           {floatParam("amount", "Amount", 0, 0.2f, 0.04f), floatParam("frequency", "Frequency", 0, 40, 10), floatParam("phase", "Phase", 0, tau, 0)}),
        fx("smear", "Smear", C::Distort, {floatParam("distance", "Distance", 0, 0.3f, 0.08f), floatParam("angle", "Angle", 0, tau, 0)}),
        fx("twirl", "Twirl", C::Distort, {floatParam("angle", "Angle", -10, 10, 3), floatParam("radius", "Radius", 0.05f, 1.5f, 0.5f)}),
        fx("optical-flow", "Optical-Flow", C::Distort,
           {floatParam("amount", "Amount", 0, 0.2f, 0.05f), floatParam("scale", "Scale", 1, 20, 4), floatParam("swirl", "Swirl", 0, 1, 0.5f)}),
        // ---- Retro
        fx("pixelate", "Pixelate", C::Retro, {floatParam("size", "Block Size", 1, 128, 12)}),
        fx("scanlines", "Scan Lines", C::Retro, {floatParam("density", "Density", 10, 800, 250), floatParam("opacity", "Opacity", 0, 1, 0.5f)}),
        seeded("vhs", "VHS", C::Retro,
               {floatParam("tracking", "Tracking", 0, 1, 0.4f), floatParam("bleed", "Color Bleed", 0, 1, 0.4f), floatParam("noise", "Noise", 0, 1, 0.3f)}),
        fx("cga-8bit", "8-Bit CGA", C::Retro,
           {floatParam("size", "Pixel Size", 1, 64, 6), choiceParam("palette", "Palette", {"Cyan/Magenta", "Green/Red", "Grayscale"}, 0)}),
        fx("crt", "CRT", C::Retro,
           {floatParam("curvature", "Curvature", 0, 1, 0.3f), floatParam("scan", "Scanlines", 0, 1, 0.5f), floatParam("mask", "Aperture Mask", 0, 1, 0.3f)}),
        fx("dither", "Dither", C::Retro, {floatParam("scale", "Scale", 1, 16, 2), floatParam("levels", "Levels", 2, 8, 2)}),
        fx("dot-screen", "Dot Screen", C::Retro, {floatParam("scale", "Scale", 2, 64, 8), floatParam("angle", "Angle", 0, pi, 0.4f)}),
        fx("halftone", "Halftone", C::Retro, {floatParam("scale", "Scale", 2, 64, 10), floatParam("angle", "Angle", 0, pi, 0.4f)}),
    };
}

} // namespace

const char* categoryName(Category category) {
    switch (category) {
    case Category::Glitch: return "Glitch";
    case Category::Distort: return "Distort";
    case Category::Retro: return "Retro";
    case Category::Stylize: return "Stylize";
    case Category::Color: return "Color";
    case Category::Composite: return "Composite";
    }
    return "";
}

const std::vector<EffectSpec>& effects() {
    static const std::vector<EffectSpec> all = makeEffects();
    return all;
}

const EffectSpec* findEffect(std::string_view id) {
    for (const EffectSpec& e : effects())
        if (e.id == id) return &e;
    return nullptr;
}

Settings Settings::defaults(const EffectSpec& spec) {
    Settings s;
    s.effect = std::string(spec.id);
    for (const ParamSpec& p : spec.params) s.values.push_back(p.defaultValue);
    return s;
}

Settings Settings::normalized() const {
    const EffectSpec* spec = findEffect(effect);
    if (!spec) return *this;
    Settings s = *this;
    s.values.resize(spec->params.size());
    for (size_t i = 0; i < spec->params.size(); i++) {
        const ParamSpec& p = spec->params[i];
        float v = i < values.size() ? values[i] : p.defaultValue;
        if (!std::isfinite(v)) v = p.defaultValue;
        if (p.kind == ParamKind::Bool) v = v > 0.5f ? 1.0f : 0.0f;
        else if (p.kind == ParamKind::Choice) v = std::round(std::clamp(v, p.min, p.max));
        else v = std::clamp(v, p.min, p.max);
        s.values[i] = v;
    }
    if (!std::isfinite(seed)) s.seed = 0;
    else s.seed = seed - 100.0f * std::floor(seed / 100.0f);
    if (!(s.seed < 100.0f)) s.seed = 0;
    return s;
}

float Settings::value(std::string_view key, float fallback) const {
    const EffectSpec* spec = findEffect(effect);
    if (!spec) return fallback;
    for (size_t i = 0; i < spec->params.size(); i++)
        if (spec->params[i].key == key) return i < values.size() ? values[i] : spec->params[i].defaultValue;
    return fallback;
}

bool Settings::set(std::string_view key, float v) {
    const EffectSpec* spec = findEffect(effect);
    if (!spec) return false;
    for (size_t i = 0; i < spec->params.size(); i++)
        if (spec->params[i].key == key) {
            if (values.size() < spec->params.size()) {
                for (size_t j = values.size(); j < spec->params.size(); j++) values.push_back(spec->params[j].defaultValue);
            }
            values[i] = v;
            return true;
        }
    return false;
}

float seedFrom(uint32_t n) {
    // OpenMosh's Reroll: (counter * 0.7131).fract() * 100, from a hashed counter; three decimals keep it readable.
    uint32_t x = n, y = 0x9e3779b9u;
    rt::pcg2d(x, y);
    return float(x % 100000u) / 1000.0f;
}

// ---- runtime maths ------------------------------------------------------------------------------------------------

namespace rt {

namespace {
// pi/2 split so that k * pio2Hi is exact for |k| < 2^20 (Cody and Waite's reduction).
constexpr double pio2Hi = 1.57079632673412561417e+00;
constexpr double pio2Lo = 6.07710050650619224932e-11;
constexpr double twoOverPi = 6.36619772367581382433e-01;
constexpr double piD = 3.14159265358979323846;

// Taylor series on [-pi/4, pi/4]; the first omitted terms are below 1e-14, far under a float's resolution.
double sinKernel(double r) {
    double r2 = r * r;
    return r + r * r2 * (-1.0 / 6 + r2 * (1.0 / 120 + r2 * (-1.0 / 5040 + r2 * (1.0 / 362880 + r2 * (-1.0 / 39916800 + r2 * (1.0 / 6227020800.0))))));
}
double cosKernel(double r) {
    double r2 = r * r;
    return 1 + r2 * (-0.5 + r2 * (1.0 / 24 + r2 * (-1.0 / 720 + r2 * (1.0 / 40320 + r2 * (-1.0 / 3628800 + r2 * (1.0 / 479001600 + r2 * (-1.0 / 87178291200.0)))))));
}
// Quarter turns and the remainder; arguments past 2^20 quarter turns (1.6 million radians) are not met here.
double reduce(double x, int& quadrant) {
    double k = std::floor(x * twoOverPi + 0.5);
    quadrant = int(std::fmod(k, 4.0));
    if (quadrant < 0) quadrant += 4;
    return (x - k * pio2Hi) - k * pio2Lo;
}
double atanSmall(double x) {
    // Two halvings, atan(x) = 2 atan(x / (1 + sqrt(1 + x^2))), bring |x| <= 1 under tan(pi/16); then the series.
    x = x / (1 + std::sqrt(1 + x * x));
    x = x / (1 + std::sqrt(1 + x * x));
    double x2 = x * x, term = x, sum = x;
    for (int n = 1; n <= 14; n++) {
        term *= -x2;
        sum += term / (2 * n + 1);
    }
    return 4 * sum;
}
double atanD(double x) {
    if (x < 0) return -atanD(-x);
    if (x > 1) return piD / 2 - atanSmall(1 / x);
    return atanSmall(x);
}
} // namespace

double sinD(double x) {
    if (!std::isfinite(x)) return std::nan("");
    int q;
    double r = reduce(x, q);
    switch (q) {
    case 0: return sinKernel(r);
    case 1: return cosKernel(r);
    case 2: return -sinKernel(r);
    default: return -cosKernel(r);
    }
}

double cosD(double x) {
    if (!std::isfinite(x)) return std::nan("");
    int q;
    double r = reduce(x, q);
    switch (q) {
    case 0: return cosKernel(r);
    case 1: return -sinKernel(r);
    case 2: return -cosKernel(r);
    default: return sinKernel(r);
    }
}

double expD(double x) {
    if (std::isnan(x)) return x;
    if (x > 709.0) return HUGE_VAL;
    if (x < -745.0) return 0;
    // x = k ln 2 + r, |r| <= ln 2 / 2; the series to r^16 is exact to well past a double's resolution there.
    constexpr double ln2Hi = 6.93147180369123816490e-01, ln2Lo = 1.90821492927058770002e-10;
    const double k = std::floor(x * 1.44269504088896338700 + 0.5);
    const double r = (x - k * ln2Hi) - k * ln2Lo;
    double term = 1, sum = 1;
    for (int n = 1; n <= 16; n++) {
        term *= r / n;
        sum += term;
    }
    return std::ldexp(sum, int(k));
}

double logD(double x) {
    if (std::isnan(x) || x < 0) return std::nan("");
    if (x == 0) return -HUGE_VAL;
    if (std::isinf(x)) return x;
    int e = 0;
    double m = std::frexp(x, &e);   // [0.5, 1)
    if (m < 0.70710678118654752440) { m *= 2; e--; }
    // log m = 2 atanh(s), s = (m - 1) / (m + 1), |s| < 0.172.
    const double s = (m - 1) / (m + 1), s2 = s * s;
    double term = s, sum = s;
    for (int n = 1; n <= 13; n++) {
        term *= s2;
        sum += term / (2 * n + 1);
    }
    return e * 6.93147180559945309417e-01 + 2 * sum;
}

double powD(double x, double y) {
    if (x == 0) return y > 0 ? 0.0 : y == 0 ? 1.0 : HUGE_VAL;
    return expD(y * logD(x));
}

double atan2D(double y, double x) {
    if (std::isnan(x) || std::isnan(y)) return std::nan("");
    if (x == 0) return y > 0 ? piD / 2 : y < 0 ? -piD / 2 : 0;
    if (std::isinf(x) || std::isinf(y)) {
        if (std::isinf(y)) return y > 0 ? piD / 2 : -piD / 2;
        return x > 0 ? 0 : (std::signbit(y) ? -piD : piD);
    }
    double a = atanD(y / x);
    if (x > 0) return a;
    return std::signbit(y) ? a - piD : a + piD;
}

vec3 hsv2rgb(vec3 c) {
    // k = (1, 2/3, 1/3, 3); p = abs(fract(c.xxx + k.xyz) * 6 - k.www); c.z * mix(1, clamp(p - 1, 0, 1), c.y)
    const float k[3] = {1.0f, 2.0f / 3.0f, 1.0f / 3.0f};
    vec3 p{std::fabs(fract(c.x + k[0]) * 6.0f - 3.0f), std::fabs(fract(c.x + k[1]) * 6.0f - 3.0f), std::fabs(fract(c.x + k[2]) * 6.0f - 3.0f)};
    return c.z * mix(v3(1.0f), clamp(p - v3(1.0f), 0.0f, 1.0f), c.y);
}

vec3 rgb2hsv(vec3 c) {
    const float kx = 0.0f, ky = -1.0f / 3.0f, kz = 2.0f / 3.0f, kw = -1.0f;
    float p[4], q[4];
    if (c.y < c.z) { p[0] = c.z; p[1] = c.y; p[2] = kw; p[3] = kz; }
    else { p[0] = c.y; p[1] = c.z; p[2] = kx; p[3] = ky; }
    if (c.x < p[0]) { q[0] = p[0]; q[1] = p[1]; q[2] = p[3]; q[3] = c.x; }
    else { q[0] = c.x; q[1] = p[1]; q[2] = p[2]; q[3] = p[0]; }
    float d = q[0] - std::min(q[3], q[1]);
    const float e = 1.0e-10f;
    return {std::fabs(q[2] + (q[3] - q[1]) / (6.0f * d + e)), d / (q[0] + e), q[0]};
}

// ---- premultiplied pixels and straight frames ----------------------------------------------------------------------

const float* Frame::unitTable() {
    static const std::vector<float> table = [] {
        std::vector<float> t(65536);
        for (int v = 0; v < 65536; v++) t[size_t(v)] = float(v) / 65535.0f;
        return t;
    }();
    return table.data();
}

Frame toFrame(const Image& image) {
    Frame f(image.width(), image.height());
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            const uint8_t* s = image.row(y);
            uint16_t* d = f.row(y);
            for (int x = 0; x < image.width(); x++, s += 4, d += 4) {
                unsigned a = s[3];
                d[3] = uint16_t(a * 257u);
                if (a == 255) { for (int c = 0; c < 3; c++) d[c] = uint16_t(s[c] * 257u); continue; }
                for (int c = 0; c < 3; c++) d[c] = a ? uint16_t(std::min(65535u, (s[c] * 65535u + a / 2) / a)) : 0;
            }
        }
    });
    return f;
}

Frame toFrame(const Image16& image) {
    Frame f(image.width(), image.height());
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            const uint16_t* s = image.row(y);
            uint16_t* d = f.row(y);
            for (int x = 0; x < image.width(); x++, s += 4, d += 4) {
                uint64_t a = std::min<uint64_t>(s[3], 32768u);
                d[3] = uint16_t((a * 65535u + 16384u) >> 15);
                for (int c = 0; c < 3; c++) d[c] = a ? uint16_t(std::min<uint64_t>(65535u, (s[c] * 65535ull + a / 2) / a)) : 0;
            }
        }
    });
    return f;
}

void storePremultiplied(uint8_t* p, vec4 c) {
    auto unit = [](float v) { return v == v ? clamp(v, 0.0f, 1.0f) : 0.0f; };
    float a = std::floor(unit(c.w) * 255.0f + 0.5f);
    p[3] = uint8_t(a);
    p[0] = uint8_t(std::floor(unit(c.x) * a + 0.5f));
    p[1] = uint8_t(std::floor(unit(c.y) * a + 0.5f));
    p[2] = uint8_t(std::floor(unit(c.z) * a + 0.5f));
}

void storePremultiplied(uint16_t* p, vec4 c) {
    auto unit = [](float v) { return v == v ? clamp(v, 0.0f, 1.0f) : 0.0f; };
    float a = std::floor(unit(c.w) * 32768.0f + 0.5f);
    p[3] = uint16_t(a);
    p[0] = uint16_t(std::floor(unit(c.x) * a + 0.5f));
    p[1] = uint16_t(std::floor(unit(c.y) * a + 0.5f));
    p[2] = uint16_t(std::floor(unit(c.z) * a + 0.5f));
}

} // namespace rt

// ---- running an effect -------------------------------------------------------------------------------------------

namespace {

/// A 1x1 transparent texel: OpenMosh's stand-in for a missing aux texture.
const rt::Frame& emptyFrame() {
    static const rt::Frame f(1, 1);
    return f;
}

template <class Img>
bool applyTo(const Settings& settings, Img& image, const Sources& sources) {
    const EffectSpec* spec = findEffect(settings.effect);
    const EffectFn run = spec ? effectFunction(spec->id) : nullptr;
    if (!run) return false;
    if (image.width() <= 0 || image.height() <= 0) return true;
    const Settings s = settings.normalized();
    rt::Uniforms u;
    u.resolution = {float(image.width()), float(image.height())};
    u.seed = s.seed;
    for (size_t i = 0; i < s.values.size() && i < 8; i++) u.p[i] = s.values[i];
    rt::Frame aux, source;
    u.aux = &emptyFrame();
    if (spec->auxImage || spec->text) {
        if (sources.aux16 && sources.aux16->width() > 0 && sources.aux16->height() > 0) aux = rt::toFrame(*sources.aux16);
        else if (sources.aux && sources.aux->width() > 0 && sources.aux->height() > 0) aux = rt::toFrame(*sources.aux);
        if (aux.width() > 0) {
            u.aux = &aux;
            u.auxSize = {sources.auxWidth > 0 ? sources.auxWidth : float(aux.width()), sources.auxHeight > 0 ? sources.auxHeight : float(aux.height())};
        }
    }
    const bool sameSize16 = sources.source16 && sources.source16->width() == image.width() && sources.source16->height() == image.height();
    const bool sameSize8 = sources.source && sources.source->width() == image.width() && sources.source->height() == image.height();
    if (sameSize16) source = rt::toFrame(*sources.source16);
    else if (sameSize8) source = rt::toFrame(*sources.source);
    if (source.width() > 0) u.source = &source;
    const rt::Frame in = rt::toFrame(image);
    run(in, u, [&](int y, const rt::vec4* row) {
        auto* p = image.row(y);
        for (int x = 0; x < image.width(); x++, p += 4) rt::storePremultiplied(p, row[x]);
    });
    return true;
}

template <class Img>
std::shared_ptr<Img> onGridOf(const Img& image, const Affine& imageToDocument, const Affine& gridToDocument, int width, int height) {
    auto out = std::make_shared<Img>(std::max(width, 0), std::max(height, 0));
    const Affine map = gridToDocument.concatenating(imageToDocument.inverted());   // grid pixels to image pixels
    parallelRows(0, out->height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            auto* row = out->row(y);
            for (int x = 0; x < out->width(); x++) {
                const Point p = map.apply({x + 0.5, y + 0.5});
                const double fx = std::floor(p.x), fy = std::floor(p.y);
                if (!(fx >= 0 && fy >= 0 && fx < image.width() && fy < image.height())) continue;
                const auto* s = image.row(int(fy)) + size_t(fx) * 4;
                std::copy(s, s + 4, row + size_t(x) * 4);
            }
        }
    });
    return out;
}

} // namespace

bool apply(const Settings& settings, Image& image, const Sources& sources) { return applyTo(settings, image, sources); }
bool apply(const Settings& settings, Image16& image, const Sources& sources) { return applyTo(settings, image, sources); }

std::shared_ptr<Image> onGrid(const Image& image, const Affine& imageToDocument, const Affine& gridToDocument, int width, int height) {
    return onGridOf(image, imageToDocument, gridToDocument, width, height);
}
std::shared_ptr<Image16> onGrid(const Image16& image, const Affine& imageToDocument, const Affine& gridToDocument, int width, int height) {
    return onGridOf(image, imageToDocument, gridToDocument, width, height);
}

} // namespace compositor::mosh
