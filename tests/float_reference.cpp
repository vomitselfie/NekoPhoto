#include "float_reference.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>

using namespace compositor;

namespace float_reference {

namespace {

bool photoshop32(BlendMode mode) {
    switch (mode) {
    case BlendMode::Normal: case BlendMode::Dissolve: case BlendMode::Darken: case BlendMode::Multiply: case BlendMode::Lighten:
    case BlendMode::LinearDodge: case BlendMode::Difference: case BlendMode::Subtract: case BlendMode::Divide: case BlendMode::Hue:
    case BlendMode::Saturation: case BlendMode::Color: case BlendMode::Luminosity: case BlendMode::DarkerColor: case BlendMode::LighterColor:
        return true;
    default: return false;
    }
}

double screen(double b, double s) { return b + s - b * s; }
double dodge(double b, double s) { return b <= 0 ? 0 : s >= 1 ? 1 : std::min(1.0, b / (1 - s)); }
double burn(double b, double s) { return b >= 1 ? 1 : s <= 0 ? 0 : 1 - std::min(1.0, (1 - b) / s); }
double hardLight(double b, double s) { return s <= 0.5 ? b * 2 * s : screen(b, 2 * s - 1); }
double softLight(double b, double s) {
    if (s <= 0.5) return b - (1 - 2 * s) * b * (1 - b);
    const double d = b <= 0.25 ? ((16 * b - 12) * b + 4) * b : std::sqrt(b);
    return b + (2 * s - 1) * (d - b);
}

double lum(const double c[3], const std::array<double, 3>& w) { return w[0] * c[0] + w[1] * c[1] + w[2] * c[2]; }

void setLum(double c[3], double l, const std::array<double, 3>& w) {
    const double d = l - lum(c, w);
    for (int k = 0; k < 3; k++) c[k] += d;
    // ClipColor, the lower bound only (colour above 1 is light at 32 bits).
    const double m = lum(c, w), n = std::min({c[0], c[1], c[2]});
    if (n < 0 && m - n > 0) for (int k = 0; k < 3; k++) c[k] = m + (c[k] - m) * m / (m - n);
}

double sat(const double c[3]) { return std::max({c[0], c[1], c[2]}) - std::min({c[0], c[1], c[2]}); }

void setSat(double c[3], double s) {
    int hi = 0, lo = 0;
    for (int k = 1; k < 3; k++) { if (c[k] > c[hi]) hi = k; if (c[k] < c[lo]) lo = k; }
    if (hi == lo) { c[0] = c[1] = c[2] = 0; return; }
    const int mid = 3 - hi - lo;
    c[mid] = (c[mid] - c[lo]) * s / (c[hi] - c[lo]);
    c[hi] = s;
    c[lo] = 0;
}

} // namespace

double blendChannel(BlendMode mode, double b, double s) {
    if (!photoshop32(mode)) { b = std::clamp(b, 0.0, 1.0); s = std::clamp(s, 0.0, 1.0); }
    switch (mode) {
    case BlendMode::Multiply: return b * s;
    case BlendMode::Screen: return screen(b, s);
    case BlendMode::Overlay: return hardLight(s, b);
    case BlendMode::Darken: return std::min(b, s);
    case BlendMode::Lighten: return std::max(b, s);
    case BlendMode::ColorDodge: return dodge(b, s);
    case BlendMode::ColorBurn: return burn(b, s);
    case BlendMode::HardLight: return hardLight(b, s);
    case BlendMode::SoftLight: return softLight(b, s);
    case BlendMode::Difference: return std::fabs(b - s);
    case BlendMode::Exclusion: return b + s - 2 * b * s;
    case BlendMode::LinearBurn: return std::max(0.0, b + s - 1);
    case BlendMode::LinearDodge: return b + s;
    case BlendMode::Subtract: return std::max(0.0, b - s);
    case BlendMode::Divide: return s <= 0 ? (b <= 0 ? 0.0 : 1.0) : b / s;
    case BlendMode::VividLight: return s <= 0.5 ? burn(b, 2 * s) : dodge(b, 2 * s - 1);
    case BlendMode::LinearLight: return std::clamp(b + 2 * s - 1, 0.0, 1.0);
    case BlendMode::PinLight: return s < 0.5 ? std::min(b, 2 * s) : std::max(b, 2 * s - 1);
    case BlendMode::HardMix: return b + s >= 1 ? 1.0 : 0.0;
    default: return s;
    }
}

void composite(BlendMode mode, const double src[4], double k, double dst[4], const std::array<double, 3>& luma) {
    const double as = src[3] * k;
    if (!(as > 0)) return;
    const double da = dst[3];
    const bool plain = mode == BlendMode::Normal || mode == BlendMode::Dissolve || !(da > 0);
    if (plain) {
        for (int c = 0; c < 3; c++) dst[c] = src[c] * k + dst[c] * (1 - as);
        dst[3] = std::min(1.0, as + da * (1 - as));
        return;
    }
    double cs[3], cb[3], r[3];
    for (int c = 0; c < 3; c++) {
        cs[c] = src[c] / src[3];
        cb[c] = dst[c] / da;
    }
    switch (mode) {
    case BlendMode::Hue: for (int c = 0; c < 3; c++) r[c] = cs[c]; setSat(r, sat(cb)); setLum(r, lum(cb, luma), luma); break;
    case BlendMode::Saturation: for (int c = 0; c < 3; c++) r[c] = cb[c]; setSat(r, sat(cs)); setLum(r, lum(cb, luma), luma); break;
    case BlendMode::Color: for (int c = 0; c < 3; c++) r[c] = cs[c]; setLum(r, lum(cb, luma), luma); break;
    case BlendMode::Luminosity: for (int c = 0; c < 3; c++) r[c] = cb[c]; setLum(r, lum(cs, luma), luma); break;
    case BlendMode::DarkerColor: { const bool s = lum(cs, luma) < lum(cb, luma); for (int c = 0; c < 3; c++) r[c] = s ? cs[c] : cb[c]; break; }
    case BlendMode::LighterColor: { const bool s = lum(cs, luma) > lum(cb, luma); for (int c = 0; c < 3; c++) r[c] = s ? cs[c] : cb[c]; break; }
    default: for (int c = 0; c < 3; c++) r[c] = blendChannel(mode, cb[c], cs[c]); break;
    }
    for (int c = 0; c < 3; c++) dst[c] = src[c] * k * (1 - da) + dst[c] * (1 - as) + as * da * r[c];
    dst[3] = std::min(1.0, as + da * (1 - as));
}

namespace {

Encoding& currentEncoding() { static thread_local Encoding encoding; return encoding; }

bool drawChildren(const Document& document, const std::optional<Uuid>& parent, Canvas& out, const std::array<double, 3>& luma);

bool drawLayer(const Document& document, const Layer& layer, Canvas& out, const std::array<double, 3>& luma) {
    if (!layer.visible) return true;
    if (layer.isGroup) {
        if (layer.passThrough && layer.opacity >= 1) return drawChildren(document, layer.id, out, luma);
        Canvas inner{out.width, out.height, std::vector<double>(out.pixels.size(), 0.0)};
        if (!drawChildren(document, layer.id, inner, luma)) return false;
        for (int y = 0; y < out.height; y++)
            for (int x = 0; x < out.width; x++) composite(layer.blendMode, inner.at(x, y), std::clamp(layer.opacity, 0.0, 1.0), out.at(x, y), luma);
        return true;
    }
    if (layer.adjustment) {
        // An adjustment layer: the canvas below adjusted, mixed back in by the opacity (Normal, no mask).
        AdjustmentSettings settings;
        if (!AdjustmentSettings::parse(layer.adjustment->json, settings)) return false;
        if (layer.blendMode != BlendMode::Normal || layer.mask || layer.maskSourceId) return false;
        if (!adjustmentAt32(settings.kind)) return true;   // kept, not drawn
        Canvas adjusted = out;
        if (!adjust(adjusted, settings, currentEncoding())) return false;
        const double mix = std::clamp(layer.opacity, 0.0, 1.0);
        for (size_t i = 0; i < out.pixels.size(); i += 4) {
            if (!(out.pixels[i + 3] > 0) || !(mix > 0)) continue;
            for (int c = 0; c < 3; c++) out.pixels[i + size_t(c)] = std::max(0.0, out.pixels[i + size_t(c)] + (adjusted.pixels[i + size_t(c)] - out.pixels[i + size_t(c)]) * mix);
        }
        return true;
    }
    if (!layer.asset) return true;
    const ImageFPtr image = layer.asset->image.f32();
    if (!image || layer.maskSourceId || layer.transform.rotation != 0) return false;
    const int ox = int(std::lround(layer.transform.origin.x)), oy = int(std::lround(layer.transform.origin.y));
    if (layer.transform.size.width != image->width() || layer.transform.size.height != image->height()) return false;
    GrayFPtr mask;
    if (layer.mask && layer.mask->enabled) {
        mask = layer.mask->asset.image.f32();
        if (!mask || mask->width() != image->width() || mask->height() != image->height() || layer.mask->placement) return false;
    }
    for (int y = 0; y < image->height(); y++) {
        const int ty = oy + y;
        if (ty < 0 || ty >= out.height) continue;
        for (int x = 0; x < image->width(); x++) {
            const int tx = ox + x;
            if (tx < 0 || tx >= out.width) continue;
            double k = std::clamp(layer.opacity, 0.0, 1.0);
            if (mask) k *= mask->at(x, y);
            const float* p = image->pixel(x, y);
            const double src[4] = {p[0], p[1], p[2], p[3]};
            composite(layer.blendMode, src, k, out.at(tx, ty), luma);
        }
    }
    return true;
}

bool drawChildren(const Document& document, const std::optional<Uuid>& parent, Canvas& out, const std::array<double, 3>& luma) {
    for (const Layer& l : document.layers)
        if (l.parentId == parent && !drawLayer(document, l, out, luma)) return false;
    return true;
}

} // namespace

Canvas render(const Document& document, const std::array<double, 3>& luma, const Encoding& encoding) {
    currentEncoding() = encoding;
    Canvas out{document.width, document.height, std::vector<double>(size_t(document.width) * size_t(document.height) * 4, 0.0)};
    if (!drawChildren(document, std::nullopt, out, luma)) return {};
    return out;
}

void brushDab(Canvas& canvas, double cx, double cy, double diameter, double hardness, double opacity, const double colour[3], bool erase,
              const std::vector<double>* selection) {
    const double radius = diameter / 2, inner = radius * hardness;
    auto falloff = [](double u) {
        const double k = 2.5;
        return std::max(0.0, (std::exp(-k * u * u) - std::exp(-k)) / (1 - std::exp(-k)));
    };
    for (int y = 0; y < canvas.height; y++)
        for (int x = 0; x < canvas.width; x++) {
            const double dist = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
            if (dist >= radius + 1) continue;
            double value;
            if (hardness >= 1) value = std::clamp(radius - dist + 0.5, 0.0, 1.0);
            else if (dist <= inner) value = 1;
            else if (dist >= radius) value = 0;
            else value = falloff((dist - inner) / std::max(1e-9, radius - inner));
            const double k = value * opacity * (selection ? (*selection)[size_t(y) * size_t(canvas.width) + size_t(x)] : 1.0);
            if (k <= 0) continue;
            double* p = canvas.at(x, y);
            for (int c = 0; c < 4; c++) {
                if (erase) p[c] = p[c] * (1 - k);
                else p[c] = p[c] + ((c < 3 ? colour[c] : 1.0) - p[c]) * k;
            }
        }
}

double worstError(const ImageF& image, const Canvas& reference) {
    if (image.width() != reference.width || image.height() != reference.height) return 1e30;
    double worst = 0;
    for (int y = 0; y < image.height(); y++)
        for (int x = 0; x < image.width(); x++)
            for (int c = 0; c < 4; c++) {
                const double a = image.pixel(x, y)[c], b = reference.at(x, y)[c];
                worst = std::max(worst, std::fabs(a - b) / (1e-5 + 1e-5 * std::fabs(b)));
            }
    return worst;
}

// ---- the editing kernels (P5b) ----------------------------------------------------------------------------------

double encode(const Encoding& encoding, double v) {
    if (!(v > 0)) return 0;
    if (encoding.srgb) return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
    return std::pow(v, 1 / encoding.gamma);
}

double decode(const Encoding& encoding, double e) {
    if (!(e > 0)) return 0;
    if (encoding.srgb) return e <= 0.04045 ? e / 12.92 : std::pow((e + 0.055) / 1.055, 2.4);
    return std::pow(e, encoding.gamma);
}

Canvas canvasOf(const ImageF& image) {
    Canvas out{image.width(), image.height(), std::vector<double>(size_t(image.width()) * size_t(image.height()) * 4)};
    for (int y = 0; y < image.height(); y++)
        for (int x = 0; x < image.width(); x++)
            for (int c = 0; c < 4; c++) out.at(x, y)[c] = image.pixel(x, y)[c];
    return out;
}

namespace {

double levelsAt(const LevelsRange& range, double x) {
    const LevelsRange n = range.normalized();
    const double input = std::max(0.0, (x * 255 - n.black) / (n.white - n.black));
    return (n.outputBlack + std::pow(input, 1 / n.gamma) * (n.outputWhite - n.outputBlack)) / 255;
}

double curveAt(const CurvesSettings& curves, int channel, double x) {
    if (x <= 255) return curves.value(x, channel);
    const auto& p = curves.channels[size_t(channel)];
    const double slope = p.back().x >= 255 ? (p.back().y - p[p.size() - 2].y) / (p.back().x - p[p.size() - 2].x) : 0.0;
    return std::max(0.0, curves.value(255, channel) + (x - 255) * slope);
}

/// Per channel, encoded in and out.
template <class F>
void perChannel(Canvas& canvas, const Encoding& encoding, F&& f) {
    for (size_t i = 0; i < canvas.pixels.size(); i += 4) {
        const double a = canvas.pixels[i + 3];
        if (!(a > 0)) continue;
        for (int c = 0; c < 3; c++) canvas.pixels[i + size_t(c)] = decode(encoding, f(c, encode(encoding, canvas.pixels[i + size_t(c)] / a))) * a;
    }
}

/// The colour kinds' frame: brighter than white scaled down to it, encoded, f, cut to 0..1, back up.
template <class F>
void perColour(Canvas& canvas, const Encoding& encoding, F&& f) {
    for (size_t i = 0; i < canvas.pixels.size(); i += 4) {
        const double a = canvas.pixels[i + 3];
        if (!(a > 0)) continue;
        double lin[3];
        for (int c = 0; c < 3; c++) lin[c] = canvas.pixels[i + size_t(c)] / a;
        const double bright = std::max({lin[0], lin[1], lin[2], 1.0});
        double e[3];
        for (int c = 0; c < 3; c++) e[c] = encode(encoding, lin[c] / bright);
        f(e[0], e[1], e[2]);
        for (int c = 0; c < 3; c++) canvas.pixels[i + size_t(c)] = decode(encoding, std::clamp(e[c], 0.0, 1.0)) * bright * a;
    }
}

double luma601(double r, double g, double b) { return 0.299 * r + 0.587 * g + 0.114 * b; }

void blackWhite(double& r, double& g, double& b, const BlackWhiteSettings& s) {
    double w[6];
    for (int i = 0; i < 6; i++) w[i] = std::clamp(s.weights[size_t(i)], -200.0, 300.0) / 100;
    const double mx = std::max({r, g, b}), mn = std::min({r, g, b}), md = r + g + b - mx - mn;
    int primary, secondary;
    if (mx == r) { primary = 0; secondary = g >= b ? 1 : 5; }
    else if (mx == g) { primary = 2; secondary = r >= b ? 1 : 3; }
    else { primary = 4; secondary = g >= r ? 3 : 5; }
    const double grey = std::clamp(mn + (md - mn) * w[secondary] + (mx - md) * w[primary], 0.0, 1.0);
    if (!s.tint) { r = g = b = grey; return; }
    const double tr = s.tintColor.red, tg = s.tintColor.green, tb = s.tintColor.blue;
    const double shift = grey - luma601(tr, tg, tb);
    r = tr + shift; g = tg + shift; b = tb + shift;
    const double l = luma601(r, g, b), lo = std::min({r, g, b}), hi = std::max({r, g, b});
    if (lo < 0 && l > lo) { const double k = l / (l - lo); r = l + (r - l) * k; g = l + (g - l) * k; b = l + (b - l) * k; }
    if (hi > 1 && hi > l) { const double k = (1 - l) / (hi - l); r = l + (r - l) * k; g = l + (g - l) * k; b = l + (b - l) * k; }
}

void colorBalance(double& r, double& g, double& b, const ColorBalanceSettings& s) {
    double c[3] = {r, g, b};
    auto lum = [](const double* v) { return 0.3 * v[0] + 0.59 * v[1] + 0.11 * v[2]; };
    const double before = lum(c);
    for (int i = 0; i < 3; i++) {
        const double sh = std::clamp(s.ranges[0][size_t(i)], -100.0, 100.0), mid = std::clamp(s.ranges[1][size_t(i)], -100.0, 100.0);
        const double hi = std::clamp(s.ranges[2][size_t(i)], -100.0, 100.0);
        const double gamma = std::pow(2.0, -mid / 100);
        const double inBlack = sh < 0 ? -sh * 0.0035 : 0, outBlack = sh > 0 ? sh * 0.0035 : 0;
        const double inWhite = hi > 0 ? 1 - hi * 0.003 : 1, outWhite = hi < 0 ? 1 + hi * 0.003 : 1;
        const double x = std::pow(std::clamp((c[i] - inBlack) / std::max(1e-6, inWhite - inBlack), 0.0, 1.0), gamma);
        c[i] = outBlack + (outWhite - outBlack) * x;
    }
    if (s.preserveLuminosity) {
        const double d = before - lum(c);
        for (double& v : c) v += d;
        const double l = lum(c), lo = std::min({c[0], c[1], c[2]}), hi = std::max({c[0], c[1], c[2]});
        if (lo < 0 && l > lo) for (double& v : c) v = l + (v - l) * l / (l - lo);
        if (hi > 1 && hi > l) for (double& v : c) v = l + (v - l) * (1 - l) / (hi - l);
    }
    r = c[0]; g = c[1]; b = c[2];
}

void vibrance(double& r, double& g, double& b, const VibranceSettings& s) {
    const double sat = std::clamp(s.saturation, -100.0, 100.0) / 100, vib = std::clamp(s.vibrance, -100.0, 100.0) / 100;
    const double l = luma601(r, g, b), chroma = std::max({r, g, b}) - std::min({r, g, b});
    const double k = (1 + sat) * (1 + vib * (vib > 0 ? (1 - chroma) : 1));
    r = l + (r - l) * k; g = l + (g - l) * k; b = l + (b - l) * k;
    const double hi = std::max({r, g, b}), lo = std::min({r, g, b});
    double f = 1;
    if (hi > 1 && hi > l) f = std::min(f, (1 - l) / (hi - l));
    if (lo < 0 && lo < l) f = std::min(f, l / (l - lo));
    if (f < 1) { r = l + (r - l) * f; g = l + (g - l) * f; b = l + (b - l) * f; }
}

void photoFilter(double& r, double& g, double& b, const PhotoFilterSettings& s) {
    const double d = std::clamp(s.density, 0.0, 100.0) / 100;
    const AdjustmentColor f = s.color.clamped();
    const double before = luma601(r, g, b);
    r = std::min(1.0, r * (1 - d) + r * f.red * 2 * d);
    g = std::min(1.0, g * (1 - d) + g * f.green * 2 * d);
    b = std::min(1.0, b * (1 - d) + b * f.blue * 2 * d);
    if (s.preserveLuminosity) {
        const double after = luma601(r, g, b);
        if (after > 0.0001) { const double k = before / after; r *= k; g *= k; b *= k; }
    }
}

void channelMixer(double& r, double& g, double& b, const ChannelMixerSettings& s) {
    auto mix = [&](int row) {
        double w[4];
        for (int k = 0; k < 4; k++) w[k] = std::clamp(s.rows[size_t(row)][size_t(k)], -200.0, 200.0) / 100;
        return w[0] * r + w[1] * g + w[2] * b + w[3];
    };
    if (s.monochrome) { r = g = b = mix(3); return; }
    const double nr = mix(0), ng = mix(1), nb = mix(2);
    r = nr; g = ng; b = nb;
}

void hueSaturation(Canvas& canvas, const HueSaturationSettings& settings, const Encoding& encoding) {
    if (settings.isIdentity()) return;
    if (settings.colorize) {
        perColour(canvas, encoding, [&](double& r, double& g, double& b) {
            const double v = (std::max({r, g, b}) + std::min({r, g, b})) / 2;
            r = g = b = v;
            settings.adjust(r, g, b);
        });
        return;
    }
    constexpr int dim = 33;
    std::vector<double> cube(size_t(dim) * dim * dim * 3);
    for (int bi = 0; bi < dim; bi++)
        for (int gi = 0; gi < dim; gi++)
            for (int ri = 0; ri < dim; ri++) {
                double r = ri / double(dim - 1), g = gi / double(dim - 1), b = bi / double(dim - 1);
                settings.adjust(r, g, b);
                const size_t i = (size_t(bi) * dim * dim + size_t(gi) * dim + size_t(ri)) * 3;
                cube[i] = r; cube[i + 1] = g; cube[i + 2] = b;
            }
    auto at = [&](const int* index) { return &cube[(size_t(index[2]) * dim * dim + size_t(index[1]) * dim + size_t(index[0])) * 3]; };
    perColour(canvas, encoding, [&](double& r, double& g, double& b) {
        // Tetrahedral: from the cell's corner along the axes in decreasing order of their fractions.
        const double in[3] = {r, g, b};
        int index[3];
        double f[3];
        for (int c = 0; c < 3; c++) {
            const double s = std::clamp(in[c], 0.0, 1.0) * (dim - 1);
            index[c] = std::min(dim - 2, int(s));
            f[c] = s - index[c];
        }
        int order[3] = {0, 1, 2};
        std::stable_sort(order, order + 3, [&](int a, int b2) { return f[a] > f[b2]; });
        const double* previous = at(index);
        double out[3] = {previous[0], previous[1], previous[2]};
        for (int k = 0; k < 3; k++) {
            index[order[k]]++;
            const double* next = at(index);
            for (int c = 0; c < 3; c++) out[c] += (next[c] - previous[c]) * f[order[k]];
            previous = next;
        }
        r = out[0]; g = out[1]; b = out[2];
    });
}

inline uint32_t hash32(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU;
    x ^= x >> 15; x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

void storePixel(double* p) {
    for (int c = 0; c < 3; c++) p[c] = std::max(0.0, p[c]);
    p[3] = std::clamp(p[3], 0.0, 1.0);
}

} // namespace

bool adjust(Canvas& canvas, const AdjustmentSettings& settings, const Encoding& encoding) {
    switch (settings.kind) {
    case AdjustmentKind::Levels:
        if (settings.levels.isIdentity()) return true;
        perChannel(canvas, encoding, [&](int c, double e) { return levelsAt(settings.levels.ranges[0], levelsAt(settings.levels.ranges[size_t(c + 1)], e)); });
        return true;
    case AdjustmentKind::Curves:
        if (!settings.curves.isValid() || settings.curves.isIdentity()) return true;
        perChannel(canvas, encoding, [&](int c, double e) { return curveAt(settings.curves, 0, curveAt(settings.curves, c + 1, e * 255)) / 255; });
        return true;
    case AdjustmentKind::Exposure: {
        const ExposureSettings s = settings.exposure.normalized();
        for (size_t i = 0; i < canvas.pixels.size(); i += 4) {
            const double a = canvas.pixels[i + 3];
            if (!(a > 0)) continue;
            for (int c = 0; c < 3; c++) canvas.pixels[i + size_t(c)] = std::pow(std::max(0.0, canvas.pixels[i + size_t(c)] / a * std::pow(2.0, s.exposure) + s.offset), 1 / s.gamma) * a;
        }
        return true;
    }
    case AdjustmentKind::Invert: perChannel(canvas, encoding, [](int, double e) { return std::max(0.0, 1 - e); }); return true;
    case AdjustmentKind::GradientMap: {
        const GradientMapSettings& g = settings.gradientMap;
        const AdjustmentColor dark = (g.reversed ? g.highlights : g.shadows).clamped(), light = (g.reversed ? g.shadows : g.highlights).clamped();
        const double from[3] = {dark.red, dark.green, dark.blue}, to[3] = {light.red, light.green, light.blue};
        for (size_t i = 0; i < canvas.pixels.size(); i += 4) {
            const double a = canvas.pixels[i + 3];
            if (!(a > 0)) continue;
            double e[3];
            for (int c = 0; c < 3; c++) e[c] = encode(encoding, canvas.pixels[i + size_t(c)] / a);
            const double t = std::min(1.0, 0.2126 * e[0] + 0.7152 * e[1] + 0.0722 * e[2]);
            for (int c = 0; c < 3; c++) canvas.pixels[i + size_t(c)] = decode(encoding, from[c] + (to[c] - from[c]) * t) * a;
        }
        return true;
    }
    case AdjustmentKind::HueSaturation: hueSaturation(canvas, settings.hsv, encoding); return true;
    case AdjustmentKind::BlackWhite: perColour(canvas, encoding, [&](double& r, double& g, double& b) { blackWhite(r, g, b, settings.blackWhite); }); return true;
    case AdjustmentKind::ColorBalance: perColour(canvas, encoding, [&](double& r, double& g, double& b) { colorBalance(r, g, b, settings.colorBalance); }); return true;
    case AdjustmentKind::Vibrance: perColour(canvas, encoding, [&](double& r, double& g, double& b) { vibrance(r, g, b, settings.vibrance); }); return true;
    case AdjustmentKind::PhotoFilter: perColour(canvas, encoding, [&](double& r, double& g, double& b) { photoFilter(r, g, b, settings.photoFilter); }); return true;
    case AdjustmentKind::ChannelMixer: perColour(canvas, encoding, [&](double& r, double& g, double& b) { channelMixer(r, g, b, settings.channelMixer); }); return true;
    default: return false;
    }
}

void gaussianBlur(Canvas& canvas, double sigma) {
    const int radius = std::max(1, int(std::ceil(sigma * 3)));
    std::vector<double> kernel(size_t(radius) * 2 + 1);
    double total = 0;
    for (int i = -radius; i <= radius; i++) total += kernel[size_t(i + radius)] = std::exp(-(i * i) / (2 * sigma * sigma));
    for (double& k : kernel) k /= total;
    const int w = canvas.width, h = canvas.height;
    Canvas rows{w, h, std::vector<double>(canvas.pixels.size(), 0.0)};
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            for (int i = std::max(-radius, -x); i <= std::min(radius, w - 1 - x); i++)
                for (int c = 0; c < 4; c++) rows.at(x, y)[c] += canvas.at(x + i, y)[c] * kernel[size_t(i + radius)];
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            double* p = canvas.at(x, y);
            for (int c = 0; c < 4; c++) p[c] = 0;
            for (int i = std::max(-radius, -y); i <= std::min(radius, h - 1 - y); i++)
                for (int c = 0; c < 4; c++) p[c] += rows.at(x, y + i)[c] * kernel[size_t(i + radius)];
            storePixel(p);
        }
}

void gaussianBlurRecursive(Canvas& canvas, double sigma) {
    // Deriche's fourth-order fit h(n) = sum_k e^{-l_k n / s} (alpha_k cos(w_k n / s) + beta_k sin(w_k n / s)) (Deriche
    // 1993; Getreuer, IPOL 2013), as a causal and an anticausal recursion normalised to sum to one, zero outside.
    const double alpha[2] = {1.6800, -0.6803}, beta[2] = {3.7350, -0.2598}, omega[2] = {0.6318, 1.9970}, lambda[2] = {1.7830, 1.7230};
    double n[2][2], d[2][3];
    for (int k = 0; k < 2; k++) {
        const double e = std::exp(-lambda[k] / sigma), c = std::cos(omega[k] / sigma), s = std::sin(omega[k] / sigma);
        n[k][0] = alpha[k];
        n[k][1] = -alpha[k] * e * c + beta[k] * e * s;
        d[k][0] = 1; d[k][1] = -2 * e * c; d[k][2] = e * e;
    }
    double num[4] = {0, 0, 0, 0}, den[5] = {0, 0, 0, 0, 0};
    for (int i = 0; i < 2; i++) for (int j = 0; j < 3; j++) num[i + j] += n[0][i] * d[1][j] + n[1][i] * d[0][j];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) den[i + j] += d[0][i] * d[1][j];
    double b[4], a[4], anti[4];
    for (int k = 0; k < 4; k++) { b[k] = num[k]; a[k] = den[k + 1]; }
    for (int k = 0; k < 4; k++) anti[k] = (k < 3 ? b[k + 1] : 0) - a[k] * b[0];
    double sumB = 0, sumA = 1, sumAnti = 0;
    for (int k = 0; k < 4; k++) { sumB += b[k]; sumA += a[k]; sumAnti += anti[k]; }
    const double gain = (sumB + sumAnti) / sumA;
    for (int k = 0; k < 4; k++) { b[k] /= gain; anti[k] /= gain; }
    // One line of samples (x[i] at `at(i)`), both passes summed.
    auto line = [&](int count, const std::function<double&(int)>& at) {
        std::vector<double> x(static_cast<size_t>(count));
        for (int i = 0; i < count; i++) x[size_t(i)] = at(i);
        auto xs = [&](int i) { return i >= 0 && i < count ? x[size_t(i)] : 0.0; };
        std::vector<double> causal(static_cast<size_t>(count)), anticausal(static_cast<size_t>(count));
        for (int i = 0; i < count; i++) {
            double v = b[0] * xs(i) + b[1] * xs(i - 1) + b[2] * xs(i - 2) + b[3] * xs(i - 3);
            for (int k = 0; k < 4; k++) v -= a[k] * (i - 1 - k >= 0 ? causal[size_t(i - 1 - k)] : 0.0);
            causal[size_t(i)] = v;
        }
        for (int i = count - 1; i >= 0; i--) {
            double v = anti[0] * xs(i + 1) + anti[1] * xs(i + 2) + anti[2] * xs(i + 3) + anti[3] * xs(i + 4);
            for (int k = 0; k < 4; k++) v -= a[k] * (i + 1 + k < count ? anticausal[size_t(i + 1 + k)] : 0.0);
            anticausal[size_t(i)] = v;
        }
        for (int i = 0; i < count; i++) at(i) = causal[size_t(i)] + anticausal[size_t(i)];
    };
    for (int c = 0; c < 4; c++) {
        for (int y = 0; y < canvas.height; y++) line(canvas.width, [&](int i) -> double& { return canvas.at(i, y)[c]; });
        for (int x = 0; x < canvas.width; x++) line(canvas.height, [&](int i) -> double& { return canvas.at(x, i)[c]; });
    }
    for (int y = 0; y < canvas.height; y++) for (int x = 0; x < canvas.width; x++) storePixel(canvas.at(x, y));
}

namespace {

/// A centred box of 2 * (length / 2) + 1 samples along a line of `n` RGBA samples, zero beyond the ends.
void boxLine(std::vector<double>& line, int n, int length) {
    const int r = length / 2, taps = 2 * r + 1;
    const std::vector<double> in(line);
    for (int i = 0; i < n; i++)
        for (int c = 0; c < 4; c++) {
            double sum = 0;
            for (int k = std::max(0, i - r); k <= std::min(n - 1, i + r); k++) sum += in[size_t(k) * 4 + size_t(c)];
            line[size_t(i) * 4 + size_t(c)] = sum / taps;
        }
}

/// Linear interpolation between samples `i0` and `i0 + 1` of a line (`count` samples, `stride` doubles apart from
/// `offset`), zero outside.
double lerpLine(const std::vector<double>& data, int count, int i0, double f, size_t stride, size_t offset, int c) {
    const double a = i0 >= 0 && i0 < count ? data[size_t(i0) * stride + offset + size_t(c)] : 0.0;
    const double b = i0 + 1 >= 0 && i0 + 1 < count ? data[size_t(i0 + 1) * stride + offset + size_t(c)] : 0.0;
    return a * (1 - f) + b * f;
}

} // namespace

void motionBlur(Canvas& canvas, double distance, double angleDegrees) {
    if (distance < 1) return;
    const int w = canvas.width, h = canvas.height;
    const double radians = angleDegrees * M_PI / 180, dx = std::cos(radians), dy = -std::sin(radians);
    Canvas out{w, h, std::vector<double>(canvas.pixels.size(), 0.0)};
    if (std::fabs(dx) >= std::fabs(dy)) {
        const double slope = dy / dx;
        const double shiftMin = std::min(0.0, (w - 1) * slope), shiftMax = std::max(0.0, (w - 1) * slope);
        const int vOffset = int(std::floor(-shiftMax)) - 1, vRows = int(std::ceil(h - shiftMin)) + 2 - vOffset;
        std::vector<double> sheared(size_t(w) * size_t(vRows) * 4, 0.0);
        for (int v = 0; v < vRows; v++)
            for (int x = 0; x < w; x++) {
                const double yf = (v + vOffset) + x * slope;
                const int y0 = int(std::floor(yf));
                for (int c = 0; c < 4; c++) sheared[(size_t(v) * size_t(w) + size_t(x)) * 4 + size_t(c)] = lerpLine(canvas.pixels, h, y0, yf - y0, size_t(w) * 4, size_t(x) * 4, c);
            }
        const int length = std::max(1, int(std::round(distance * std::fabs(dx))));
        for (int v = 0; v < vRows; v++) {
            std::vector<double> line(sheared.begin() + std::ptrdiff_t(size_t(v) * size_t(w) * 4), sheared.begin() + std::ptrdiff_t(size_t(v + 1) * size_t(w) * 4));
            boxLine(line, w, length);
            std::copy(line.begin(), line.end(), sheared.begin() + std::ptrdiff_t(size_t(v) * size_t(w) * 4));
        }
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const double vf = (y - x * slope) - vOffset;
                const int v0 = int(std::floor(vf));
                for (int c = 0; c < 4; c++) out.at(x, y)[c] = lerpLine(sheared, vRows, v0, vf - v0, size_t(w) * 4, size_t(x) * 4, c);
                storePixel(out.at(x, y));
            }
    } else {
        const double slope = dx / dy;
        const double shiftMin = std::min(0.0, (h - 1) * slope), shiftMax = std::max(0.0, (h - 1) * slope);
        const int uOffset = int(std::floor(-shiftMax)) - 1, uCols = int(std::ceil(w - shiftMin)) + 2 - uOffset;
        std::vector<double> sheared(size_t(uCols) * size_t(h) * 4, 0.0);
        for (int y = 0; y < h; y++)
            for (int u = 0; u < uCols; u++) {
                const double xf = (u + uOffset) + y * slope;
                const int x0 = int(std::floor(xf));
                for (int c = 0; c < 4; c++) sheared[(size_t(y) * size_t(uCols) + size_t(u)) * 4 + size_t(c)] = lerpLine(canvas.pixels, w, x0, xf - x0, 4, size_t(y) * size_t(w) * 4, c);
            }
        const int length = std::max(1, int(std::round(distance * std::fabs(dy))));
        for (int u = 0; u < uCols; u++) {
            std::vector<double> column(size_t(h) * 4);
            for (int y = 0; y < h; y++) for (int c = 0; c < 4; c++) column[size_t(y) * 4 + size_t(c)] = sheared[(size_t(y) * size_t(uCols) + size_t(u)) * 4 + size_t(c)];
            boxLine(column, h, length);
            for (int y = 0; y < h; y++) for (int c = 0; c < 4; c++) sheared[(size_t(y) * size_t(uCols) + size_t(u)) * 4 + size_t(c)] = column[size_t(y) * 4 + size_t(c)];
        }
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const double uf = (x - y * slope) - uOffset;
                const int u0 = int(std::floor(uf));
                for (int c = 0; c < 4; c++) out.at(x, y)[c] = lerpLine(sheared, uCols, u0, uf - u0, 4, size_t(y) * size_t(uCols) * 4, c);
                storePixel(out.at(x, y));
            }
    }
    canvas = std::move(out);
}

void addNoise(Canvas& canvas, double amount, bool gaussian, bool monochromatic, uint32_t seed, const Encoding& encoding) {
    const double spread = amount / 100 * 127.5 / 255;
    auto unit = [](uint32_t key) { return double(hash32(key) >> 8) / 16777216.0; };
    auto sample = [&](uint32_t key) {
        if (!gaussian) return (unit(key) * 2 - 1) * spread;
        const double u1 = unit(key), u2 = unit(key ^ 0x68e31da4U);
        return std::sqrt(-2 * std::log(1 - u1)) * std::cos(6.2831853 * u2) * spread * (2.0 / 3.0);
    };
    const uint32_t width = uint32_t(canvas.width);
    for (int y = 0; y < canvas.height; y++)
        for (uint32_t x = 0; x < width; x++) {
            double* p = canvas.at(int(x), y);
            if (!(p[3] > 0)) continue;
            const uint32_t base = hash32(seed ^ hash32(uint32_t(y) * width + x));
            const double mono = monochromatic ? sample(base) : 0;
            for (int c = 0; c < 3; c++) {
                const double n = monochromatic ? mono : sample(base + uint32_t(c) * 0x9e3779b9U);
                p[c] = decode(encoding, std::max(0.0, encode(encoding, p[c] / p[3]) + n)) * p[3];
            }
        }
}

void lensCorrection(Canvas& canvas, double distortion, bool bicubic) {
    const double k = std::clamp(distortion, -100.0, 100.0) / 100 * 0.35;
    const Canvas source = canvas;
    const int width = canvas.width, height = canvas.height;
    const double cx = width * 0.5, cy = height * 0.5, halfDiagonal2 = cx * cx + cy * cy;
    auto cubic = [](double t, double w[4]) {
        const double t2 = t * t, t3 = t2 * t;
        w[0] = (-t3 + 2 * t2 - t) / 2; w[1] = (3 * t3 - 5 * t2 + 2) / 2; w[2] = (-3 * t3 + 4 * t2 + t) / 2; w[3] = (t3 - t2) / 2;
    };
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) {
            const double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
            const double scale = 1 - k * (dx * dx + dy * dy) / halfDiagonal2;
            const double sx = cx + dx * scale - 0.5, sy = cy + dy * scale - 0.5;
            const long x0 = long(std::floor(sx)), y0 = long(std::floor(sy));
            const double fx = sx - double(x0), fy = sy - double(y0);
            double wx[4], wy[4];
            const int taps = bicubic ? 4 : 2;
            if (bicubic) { cubic(fx, wx); cubic(fy, wy); } else { wx[0] = 1 - fx; wx[1] = fx; wy[0] = 1 - fy; wy[1] = fy; }
            const long firstX = bicubic ? x0 - 1 : x0, firstY = bicubic ? y0 - 1 : y0;
            double* out = canvas.at(x, y);
            for (int c = 0; c < 4; c++) out[c] = 0;
            for (int j = 0; j < taps; j++)
                for (int i = 0; i < taps; i++) {
                    const long px = firstX + i, py = firstY + j;
                    if (px < 0 || py < 0 || px >= width || py >= height) continue;
                    for (int c = 0; c < 4; c++) out[c] += wx[i] * wy[j] * source.at(int(px), int(py))[c];
                }
            storePixel(out);
        }
}

Canvas resample(const Canvas& canvas, int width, int height, double originX, double stepX, double originY, double stepY, ResampleFilter filter) {
    auto kernel = [&](double x) {
        x = std::fabs(x);
        switch (filter) {
        case ResampleFilter::Triangle: return std::max(0.0, 1 - x);
        case ResampleFilter::CatmullRom: {
            const double x2 = x * x, x3 = x2 * x;
            return x < 1 ? (3 * x3 - 5 * x2 + 2) / 2 : x < 2 ? (-x3 + 5 * x2 - 8 * x + 4) / 2 : 0.0;
        }
        case ResampleFilter::Lanczos3: {
            if (x < 1e-9) return 1.0;
            if (x >= 3) return 0.0;
            const double px = M_PI * x;
            return 3 * std::sin(px) * std::sin(px / 3) / (px * px);
        }
        }
        return 0.0;
    };
    const double reach = filter == ResampleFilter::Triangle ? 1 : filter == ResampleFilter::CatmullRom ? 2 : 3;
    struct Taps { std::vector<std::pair<int, double>> weights; double edge = 1; };
    auto taps = [&](int count, double origin, double step, int sourceCount) {
        std::vector<Taps> out;
        out.resize(size_t(count));
        const double widen = std::max(1.0, std::fabs(step)), radius = reach * widen;
        for (int x = 0; x < count; x++) {
            const double position = origin + x * step, centre = position - 0.5;
            out[size_t(x)].edge = std::clamp(std::min(position, sourceCount - position) / std::fabs(step) + 0.5, 0.0, 1.0);
            int lo = int(std::ceil(centre - radius)), hi = int(std::floor(centre + radius));
            std::vector<double> raw;
            double sum = 0;
            for (int j = lo; j <= hi; j++) { raw.push_back(kernel((j - centre) / widen)); sum += raw.back(); }
            if (raw.empty() || sum <= 0) { raw.assign(1, 1.0); lo = hi = int(std::floor(centre + 0.5)); sum = 1; }
            for (int j = lo; j <= hi; j++) out[size_t(x)].weights.push_back({std::clamp(j, 0, sourceCount - 1), raw[size_t(j - lo)] / sum});
        }
        return out;
    };
    const std::vector<Taps> tx = taps(width, originX, stepX, canvas.width), ty = taps(height, originY, stepY, canvas.height);
    Canvas mid{width, canvas.height, std::vector<double>(size_t(width) * size_t(canvas.height) * 4, 0.0)};
    for (int y = 0; y < canvas.height; y++)
        for (int x = 0; x < width; x++)
            for (auto [j, w] : tx[size_t(x)].weights) for (int c = 0; c < 4; c++) mid.at(x, y)[c] += canvas.at(j, y)[c] * w;
    Canvas out{width, height, std::vector<double>(size_t(width) * size_t(height) * 4, 0.0)};
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) {
            double* p = out.at(x, y);
            for (auto [j, w] : ty[size_t(y)].weights) for (int c = 0; c < 4; c++) p[c] += mid.at(x, j)[c] * w;
            const double edge = tx[size_t(x)].edge * ty[size_t(y)].edge;
            storePixel(p);
            for (int c = 0; c < 4; c++) p[c] *= edge;
        }
    return out;
}

} // namespace float_reference
