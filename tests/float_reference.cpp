#include "float_reference.h"
#include <algorithm>
#include <cmath>

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
    if (!layer.asset) return true;
    const ImageFPtr image = layer.asset->image.f32();
    if (!image || layer.adjustment || layer.maskSourceId || layer.transform.rotation != 0) return false;
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

Canvas render(const Document& document, const std::array<double, 3>& luma) {
    Canvas out{document.width, document.height, std::vector<double>(size_t(document.width) * size_t(document.height) * 4, 0.0)};
    if (!drawChildren(document, std::nullopt, out, luma)) return {};
    return out;
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

} // namespace float_reference
