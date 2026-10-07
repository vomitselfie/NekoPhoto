// Drawing layer styles around a layer's pixels. The float-mask machinery (the tent blur, spread and choke as
// grayscale dilation, exact Euclidean distance fields, the stroke band, the bevel height field) and the
// compositing rules (burn/dodge folding, exterior knockout, interior order, Blend Interior Effects as Group)
// are ported from Patchy (MIT; src/render/layer_style_mask_ops.cpp and render/layer_compositor.hpp there),
// which calibrated them against Photoshop 2026 COM renders; the comments there carry the probe details.
#include "layerstyle_render.h"
#include "compositor/blend.h"
#include "compositor/imaget.h"
#include "compositor/depth.h"
#include "compositor/parallel.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <type_traits>

namespace compositor {
namespace {

using Mask = std::vector<float>;
constexpr float kPi = 3.14159265358979323846f;
constexpr float kUnreached = 1.0e20f;

inline float unit(float v) { return std::clamp(v, 0.0f, 1.0f); }

// ---- Blurs ----------------------------------------------------------------------------------------------------

/// Photoshop's tent blur [1..N..1]/N^2, N = max(2, size), separable, through prefix sums.
void tentBlur(Mask& mask, int w, int h, float size) {
    if (size <= 0 || w <= 0 || h <= 0) return;
    const int peak = std::max(2, int(std::lround(size)));
    auto lines = [&](const Mask& in, Mask& out, int count, int length, size_t lineStep, size_t sampleStep) {
        parallelRows(0, count, [&](int a, int b) {
            std::vector<double> prefix(size_t(length) + 1), weighted(size_t(length) + 1);
            const double divisor = double(peak) * peak;
            for (int line = a; line < b; line++) {
                const size_t base = size_t(line) * lineStep;
                for (int p = 0; p < length; p++) {
                    const double v = in[base + size_t(p) * sampleStep];
                    prefix[size_t(p) + 1] = prefix[size_t(p)] + v;
                    weighted[size_t(p) + 1] = weighted[size_t(p)] + p * v;
                }
                for (int p = 0; p < length; p++) {
                    const int left = std::max(0, p - peak + 1), right = std::min(length, p + peak);
                    const double ls = prefix[size_t(p) + 1] - prefix[size_t(left)], lw = weighted[size_t(p) + 1] - weighted[size_t(left)];
                    const double rs = prefix[size_t(right)] - prefix[size_t(p) + 1], rw = weighted[size_t(right)] - weighted[size_t(p) + 1];
                    out[base + size_t(p) * sampleStep] = float(((peak - p) * ls + lw + (peak + p) * rs - rw) / divisor);
                }
            }
        }, 16);
    };
    Mask horizontal(mask.size()), output(mask.size());
    lines(mask, horizontal, h, w, size_t(w), 1);
    lines(horizontal, output, w, h, 1, size_t(w));
    mask.swap(output);
}

void boxBlur(Mask& mask, int w, int h, int radius, int passes) {
    if (radius <= 0 || passes <= 0) return;
    Mask tmp(mask.size()), out(mask.size());
    for (int pass = 0; pass < passes; pass++) {
        for (int y = 0; y < h; y++) {
            float sum = 0; int count = 0;
            for (int x = -radius; x <= radius; x++) if (x >= 0 && x < w) { sum += mask[size_t(y) * w + x]; count++; }
            for (int x = 0; x < w; x++) {
                tmp[size_t(y) * w + x] = sum / float(std::max(1, count));
                const int rm = x - radius, ad = x + radius + 1;
                if (rm >= 0 && rm < w) { sum -= mask[size_t(y) * w + rm]; count--; }
                if (ad >= 0 && ad < w) { sum += mask[size_t(y) * w + ad]; count++; }
            }
        }
        for (int x = 0; x < w; x++) {
            float sum = 0; int count = 0;
            for (int y = -radius; y <= radius; y++) if (y >= 0 && y < h) { sum += tmp[size_t(y) * w + x]; count++; }
            for (int y = 0; y < h; y++) {
                out[size_t(y) * w + x] = sum / float(std::max(1, count));
                const int rm = y - radius, ad = y + radius + 1;
                if (rm >= 0 && rm < h) { sum -= tmp[size_t(rm) * w + x]; count--; }
                if (ad >= 0 && ad < h) { sum += tmp[size_t(ad) * w + x]; count++; }
            }
        }
        mask.swap(out);
    }
}

// ---- Distances --------------------------------------------------------------------------------------------------

void edt1d(const float* f, float* d, int* v, double* z, int n) {
    int k = 0;
    v[0] = 0; z[0] = -1e30; z[1] = 1e30;
    for (int q = 1; q < n; q++) {
        const double fq = double(f[q]) + double(q) * q;
        double s = (fq - (double(f[v[k]]) + double(v[k]) * v[k])) / (2.0 * q - 2.0 * v[k]);
        while (s <= z[k]) { k--; s = (fq - (double(f[v[k]]) + double(v[k]) * v[k])) / (2.0 * q - 2.0 * v[k]); }
        k++; v[k] = q; z[k] = s; z[k + 1] = 1e30;
    }
    k = 0;
    for (int q = 0; q < n; q++) { while (z[k + 1] < q) k++; const float dx = float(q - v[k]); d[q] = dx * dx + f[v[k]]; }
}

/// Exact squared Euclidean distance transform (Felzenszwalb-Huttenlocher): 0 at sources, huge elsewhere on entry.
void edt(Mask& field, int w, int h) {
    parallelRows(0, w, [&](int a, int b) {
        const int n = std::max(w, h);
        std::vector<float> f(static_cast<size_t>(n)), d(static_cast<size_t>(n)); std::vector<int> v(static_cast<size_t>(n)); std::vector<double> z(static_cast<size_t>(n) + 1);
        for (int x = a; x < b; x++) {
            for (int y = 0; y < h; y++) f[size_t(y)] = field[size_t(y) * w + x];
            edt1d(f.data(), d.data(), v.data(), z.data(), h);
            for (int y = 0; y < h; y++) field[size_t(y) * w + x] = d[size_t(y)];
        }
    }, 16);
    parallelRows(0, h, [&](int a, int b) {
        const int n = std::max(w, h);
        std::vector<float> f(static_cast<size_t>(n)), d(static_cast<size_t>(n)); std::vector<int> v(static_cast<size_t>(n)); std::vector<double> z(static_cast<size_t>(n) + 1);
        for (int y = a; y < b; y++) {
            float* row = field.data() + size_t(y) * w;
            std::copy(row, row + w, f.data());
            edt1d(f.data(), d.data(), v.data(), z.data(), w);
            std::copy(d.data(), d.data() + w, row);
        }
    }, 16);
}

/// Distance to the nearest painted (value > 0) pixel, or to the nearest clear one.
Mask distanceField(const Mask& base, int w, int h, bool toPainted) {
    Mask field(base.size(), kUnreached);
    for (size_t i = 0; i < base.size(); i++) if ((base[i] > 0) == toPainted) field[i] = 0;
    edt(field, w, h);
    for (auto& v : field) v = std::sqrt(v);
    return field;
}

/// Chamfer distance to painted pixels, carrying each connected component's strongest value.
void chamfer(const Mask& input, int w, int h, Mask& dist, Mask& strength) {
    Mask source(input.size(), 0);
    {
        std::vector<uint8_t> seen(input.size(), 0);
        std::vector<size_t> stack, component;
        for (size_t start = 0; start < input.size(); start++) {
            if (seen[start] || input[start] <= 0) continue;
            stack.assign(1, start); component.clear(); seen[start] = 1;
            float best = unit(input[start]);
            while (!stack.empty()) {
                const size_t i = stack.back(); stack.pop_back(); component.push_back(i);
                best = std::max(best, unit(input[i]));
                const int x = int(i % size_t(w)), y = int(i / size_t(w));
                for (int ny = std::max(0, y - 1); ny <= std::min(h - 1, y + 1); ny++)
                    for (int nx = std::max(0, x - 1); nx <= std::min(w - 1, x + 1); nx++) {
                        const size_t j = size_t(ny) * w + nx;
                        if (!seen[j] && input[j] > 0) { seen[j] = 1; stack.push_back(j); }
                    }
            }
            for (size_t i : component) source[i] = best;
        }
    }
    dist.assign(input.size(), kUnreached); strength.assign(input.size(), 0);
    for (size_t i = 0; i < input.size(); i++) if (input[i] > 0) { dist[i] = 0; strength[i] = source[i]; }
    auto relax = [&](size_t i, size_t j, float step) {
        if (strength[j] <= 0) return;
        const float c = dist[j] + step;
        if (c + 0.001f < dist[i] || (std::abs(c - dist[i]) <= 0.001f && strength[j] > strength[i])) { dist[i] = c; strength[i] = strength[j]; }
    };
    const float diag = 1.41421356f;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        const size_t i = size_t(y) * w + x;
        if (x > 0) relax(i, i - 1, 1);
        if (y > 0) relax(i, i - size_t(w), 1);
        if (x > 0 && y > 0) relax(i, i - size_t(w) - 1, diag);
        if (x + 1 < w && y > 0) relax(i, i - size_t(w) + 1, diag);
    }
    for (int y = h - 1; y >= 0; y--) for (int x = w - 1; x >= 0; x--) {
        const size_t i = size_t(y) * w + x;
        if (x + 1 < w) relax(i, i + 1, 1);
        if (y + 1 < h) relax(i, i + size_t(w), 1);
        if (x + 1 < w && y + 1 < h) relax(i, i + size_t(w) + 1, diag);
        if (x > 0 && y + 1 < h) relax(i, i + size_t(w) - 1, diag);
    }
}

void expand(Mask& mask, int w, int h, float radius) {
    if (radius <= 0) return;
    Mask d, s;
    chamfer(mask, w, h, d, s);
    for (size_t i = 0; i < mask.size(); i++) mask[i] = std::max(mask[i], s[i] * unit(radius + 1 - d[i]));
}

/// Grayscale dilation by an integer radius: max of value x area-sampled disc coverage (exact up to 8 pixels).
void dilate(Mask& mask, int w, int h, int radius) {
    if (radius <= 0) return;
    if (radius > 8) { expand(mask, w, h, float(radius)); return; }
    const int reach = radius + 1;
    struct Tap { int dx, dy; float weight; };
    std::vector<Tap> taps;
    for (int dy = -reach; dy <= reach; dy++) for (int dx = -reach; dx <= reach; dx++) {
        if (!dx && !dy) continue;
        const float c = float(std::clamp(radius + 1.0 - std::sqrt(double(dx) * dx + double(dy) * dy), 0.0, 1.0));
        if (c > 0) taps.push_back({dx, dy, c});
    }
    Mask out(mask);
    parallelRows(0, h, [&](int a, int b) {
        for (int y = a; y < b; y++) for (int x = 0; x < w; x++) {
            float v = out[size_t(y) * w + x];
            for (auto& t : taps) {
                const int sx = x + t.dx, sy = y + t.dy;
                if (sx < 0 || sy < 0 || sx >= w || sy >= h) continue;
                v = std::max(v, mask[size_t(sy) * w + sx] * t.weight);
            }
            out[size_t(y) * w + x] = v;
        }
    }, 16);
    mask.swap(out);
}

// ---- Effect falloffs --------------------------------------------------------------------------------------------

/// Drop shadow and outer glow ("Softer"): spread dilates by lround(spread% x size), the rest blurs with the tent.
void softExterior(Mask& mask, int w, int h, float size, float spread) {
    const long rounded = std::lround(std::max(0.0f, size));
    const int spreadRadius = int(std::lround(std::max(0.0f, size) * unit(spread / 100)));
    dilate(mask, w, h, spreadRadius);
    if (rounded > 0) { const long peak = std::max(2L, rounded) - spreadRadius; if (peak >= 2) tentBlur(mask, w, h, float(peak)); }
}

/// Inner shadow and inner glow: the inverse matte dilated by the choke, then the tent; 1 at the contour.
void softInterior(Mask& mask, int w, int h, float size, float choke) {
    const long rounded = std::lround(std::max(0.0f, size));
    const int chokeRadius = int(std::lround(std::max(0.0f, size) * unit(choke / 100)));
    for (auto& v : mask) v = 1 - unit(v);
    dilate(mask, w, h, chokeRadius);
    if (rounded > 0) { const long peak = std::max(2L, rounded) - chokeRadius; if (peak >= 2) tentBlur(mask, w, h, float(peak)); }
}

/// Precise glows: a smooth falloff along the distance from the shape.
Mask distanceFalloff(const Mask& input, int w, int h, float size, float spread) {
    Mask d, s;
    chamfer(input, w, h, d, s);
    const float solid = size * unit(spread / 100);
    for (size_t i = 0; i < d.size(); i++) {
        float a = 0;
        if (size <= 0) a = d[i] <= 0 ? 1 : 0;
        else if (d[i] <= solid || spread >= 99.9f) a = d[i] <= size ? 1 : 0;
        else if (d[i] <= size) { const float t = unit((d[i] - solid) / std::max(0.001f, size - solid)); a = 1 - t * t * (3 - 2 * t); }
        d[i] = s[i] * a;
    }
    return d;
}

/// Distance fields anchored at the matte's subpixel half-coverage contour: supersampled 3x (bilinear),
/// thresholded at 0.5, exact EDT, read back at the pixel centres with a +1/3 px compensation that keeps binary
/// mattes on the pixel-centre convention.
void subpixelFields(const Mask& matte, int w, int h, bool needOut, bool needIn, Mask& outside, Mask& inside) {
    constexpr int k = 3;
    if (matte.size() > 1024u * 1024u) {
        Mask contour(matte.size());
        for (size_t i = 0; i < matte.size(); i++) contour[i] = matte[i] >= 0.5f ? 1 : 0;
        if (needOut) outside = distanceField(contour, w, h, true);
        if (needIn) inside = distanceField(contour, w, h, false);
        return;
    }
    const int fw = w * k, fh = h * k;
    Mask fine(size_t(fw) * fh, 0);
    for (int fy = 0; fy < fh; fy++) {
        const float cy = (fy + 0.5f) / k - 0.5f;
        const int y0 = std::clamp(int(std::floor(cy)), 0, h - 1), y1 = std::min(y0 + 1, h - 1);
        const float ty = unit(cy - y0);
        for (int fx = 0; fx < fw; fx++) {
            const float cx = (fx + 0.5f) / k - 0.5f;
            const int x0 = std::clamp(int(std::floor(cx)), 0, w - 1), x1 = std::min(x0 + 1, w - 1);
            const float tx = unit(cx - x0);
            const float top = matte[size_t(y0) * w + x0] + (matte[size_t(y0) * w + x1] - matte[size_t(y0) * w + x0]) * tx;
            const float bottom = matte[size_t(y1) * w + x0] + (matte[size_t(y1) * w + x1] - matte[size_t(y1) * w + x0]) * tx;
            fine[size_t(fy) * fw + fx] = top + (bottom - top) * ty >= 0.5f ? 1 : 0;
        }
    }
    constexpr float compensation = 0.5f - 0.5f / k;
    auto back = [&](const Mask& fd, Mask& out) {
        out.assign(size_t(w) * h, 0);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            const float d = fd[size_t(y * k + 1) * fw + size_t(x * k + 1)];
            out[size_t(y) * w + x] = d <= 0 ? 0 : d / k + compensation;
        }
    };
    if (needOut) back(distanceField(fine, fw, fh, true), outside);
    if (needIn) back(distanceField(fine, fw, fh, false), inside);
}

/// Whether the bilinear matte reaches half coverage at a 1/3 px sample within `reach` of pixel (x, y).
bool reachesHalf(const Mask& matte, int w, int h, int x, int y, float reach) {
    constexpr int k = 3;
    const int steps = int(std::floor(double(reach) * k));
    for (int j = -steps; j <= steps; j++) for (int i = -steps; i <= steps; i++) {
        if (i * i + j * j > steps * steps) continue;
        const float cy = (y * k + 1 + j + 0.5f) / k - 0.5f, cx = (x * k + 1 + i + 0.5f) / k - 0.5f;
        const int y0 = std::clamp(int(std::floor(cy)), 0, h - 1), y1 = std::min(y0 + 1, h - 1);
        const int x0 = std::clamp(int(std::floor(cx)), 0, w - 1), x1 = std::min(x0 + 1, w - 1);
        const float ty = unit(cy - y0), tx = unit(cx - x0);
        const float top = matte[size_t(y0) * w + x0] + (matte[size_t(y0) * w + x1] - matte[size_t(y0) * w + x0]) * tx;
        const float bottom = matte[size_t(y1) * w + x0] + (matte[size_t(y1) * w + x1] - matte[size_t(y1) * w + x0]) * tx;
        if (top + (bottom - top) * ty >= 0.5f) return true;
    }
    return false;
}

/// The stroke band: Outside `size` out, Inside `size` in, Center half each way, measured from the matte's
/// half-coverage contour, 1 px antialiasing ramp.
Mask strokeBand(const Mask& base, int w, int h, float size, Stroke::Position position, Mask* burst = nullptr) {
    const float bandOut = position == Stroke::Position::Inside ? 0 : position == Stroke::Position::Center ? size / 2 : size;
    const float bandIn = position == Stroke::Position::Outside ? 0 : position == Stroke::Position::Center ? size / 2 : size;
    // The contour: half-covered pixels, plus faint ones that are a flat wash rather than an antialiased fringe
    // (farther than 2 px from a solid pixel and 3 px from any half-coverage crossing).
    Mask contour(base.size(), 0);
    bool solid = false, faint = false;
    for (size_t i = 0; i < base.size(); i++) { if (base[i] >= 0.5f) { contour[i] = 1; solid = true; } else if (base[i] > 0) faint = true; }
    if (!solid) for (size_t i = 0; i < base.size(); i++) contour[i] = base[i] > 0 ? 1 : 0;
    else if (faint) {
        const Mask solidDistance = distanceField(contour, w, h, true);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            const size_t i = size_t(y) * w + x;
            if (base[i] <= 0 || contour[i] != 0 || solidDistance[i] <= 2) continue;
            if (solidDistance[i] > 3 + 1.4142137f || !reachesHalf(base, w, h, x, y, 3)) contour[i] = 1;
        }
    }
    Mask anchor(base.size(), 0);
    bool fringe = false;
    for (size_t i = 0; i < base.size(); i++) {
        anchor[i] = contour[i] > 0 ? (base[i] >= 0.5f ? base[i] : 1) : base[i];
        if (anchor[i] > 0 && anchor[i] < 1) fringe = true;
    }
    Mask outside, inside;
    if (fringe) subpixelFields(anchor, w, h, bandOut > 0, bandIn > 0, outside, inside);
    else {
        if (bandOut > 0) outside = distanceField(contour, w, h, true);
        if (bandIn > 0) inside = distanceField(contour, w, h, false);
    }
    auto coverage = [](float d, float band) { return band <= 0 ? 0.0f : unit(band + 1 - d); };
    Mask band(base.size(), 0);
    for (size_t i = 0; i < base.size(); i++) {
        const float a = base[i];
        const float o = outside.empty() ? 0 : coverage(outside[i], bandOut), in = inside.empty() ? 0 : coverage(inside[i], bandIn);
        band[i] = unit(a * in + (1 - a) * o);
    }
    if (burst) {
        // Shape Burst: linear in the band's distance, 0 at its outer limit, 1 at its inner one; each side that is a
        // band limit reaches 1 px further, like the coverage.
        const float outerReach = bandOut > 0 ? bandOut + 1 : 0, innerReach = bandIn > 0 ? bandIn + 1 : 0;
        const float span = std::max(1.0f, outerReach + innerReach);
        burst->assign(base.size(), 0);
        for (size_t i = 0; i < base.size(); i++) {
            const float along = contour[i] > 0 ? (inside.empty() ? span : bandOut + inside[i]) : (outside.empty() ? 0 : outerReach - outside[i]);
            (*burst)[i] = unit(along / span);
        }
    }
    return band;
}

Mask bevelHeight(const Mask& alpha, int w, int h, const Bevel& bevel, float size) {
    Mask height;
    size = std::max(0.01f, size);
    if (bevel.technique == Bevel::Technique::Smooth) { height = alpha; tentBlur(height, w, h, size); }
    else {
        const Mask toPainted = distanceField(alpha, w, h, true), toClear = distanceField(alpha, w, h, false);
        height.resize(alpha.size());
        for (size_t i = 0; i < alpha.size(); i++) {
            const float a = unit(alpha[i]);
            height[i] = (0.5f - 0.5f * unit(toPainted[i] / size)) * (1 - a) + (0.5f + 0.5f * unit(toClear[i] / size)) * a;
        }
        if (bevel.technique == Bevel::Technique::ChiselSoft) boxBlur(height, w, h, 1, 1);
    }
    for (auto& v : height) v = unit(v);
    return height;
}

float sampleContour(const std::array<uint8_t, 256>& lut, float t, bool antialiased) {
    t = unit(t);
    if (!antialiased) return lut[size_t(std::lround(t * 255))] / 255.0f;
    const float s = t * 255;
    const size_t lo = size_t(s), hi = std::min<size_t>(lo + 1, 255);
    const float f = s - float(lo);
    return (lut[lo] * (1 - f) + lut[hi] * f) / 255.0f;
}

// ---- Compositing ---------------------------------------------------------------------------------------------

/// The colour channels an effect works on in a document of mode `M`: R, G, B; the complements of C, M, Y and K (stored
/// inverted, so 1 is no ink); L, a and b as stored (a and b offset to the middle).
template <ColorMode M>
constexpr int colourChannels = M == ColorMode::CMYK ? 4 : 3;

/// `b` blended with `s` in `mode`, on the document's channels. RGB: effectBlend. CMYK and Lab: the separable modes per
/// stored channel with effectBlend's arithmetic (as their layer blending applies the RGB kernels per channel), the
/// others as the mode's own layer blending computes them (blendStraightMode); a mode Lab does not offer is Normal.
template <ColorMode M>
inline void blendEffect(EffectBlend mode, const float* b, const float* s, float* out) {
    if constexpr (M == ColorMode::RGB) effectBlend(mode, b, s, out);
    else {
        constexpr int nc = colourChannels<M>;
        const BlendMode as = blendModeFor(effectBlendMode(mode), M);
        switch (as) {
        case BlendMode::Hue: case BlendMode::Saturation: case BlendMode::Color: case BlendMode::Luminosity:
        case BlendMode::DarkerColor: case BlendMode::LighterColor:
            for (int k = 0; k < nc; k++) out[k] = s[k];
            blendStraightMode(as, M, b, out);
            return;
        case BlendMode::Normal: case BlendMode::Dissolve:
            for (int k = 0; k < nc; k++) out[k] = s[k];
            return;
        default:
            for (int k = 0; k < nc; k++) out[k] = effectBlendChannel(mode, b[k], s[k]);
            return;
        }
    }
}

/// An effect colour onto a premultiplied pixel. The burn modes fold the effect's alpha into the colour toward
/// white, Color Dodge toward black, then blend fully (Photoshop's effect compositing); the rest lerp.
/// At either depth: `T` is the sample (uint8_t 0..255 or uint16_t 0..32768); `M` the document's mode (its colour
/// channels, then alpha).
template <class T, ColorMode M = ColorMode::RGB>
void compositeEffect(T* d, const float* color, float alpha, EffectBlend mode) {
    constexpr int nc = colourChannels<M>;
    constexpr float one = std::is_same_v<T, uint8_t> ? 255.0f : std::is_same_v<T, float> ? 1.0f : 32768.0f;
    if (alpha <= 0) return;
    alpha = unit(alpha);
    if (mode == EffectBlend::Dissolve) mode = EffectBlend::Normal;
    const float da = d[nc] / one;
    float b[nc] = {};
    if (da > 0) for (int k = 0; k < nc; k++) b[k] = d[k] / one / da;
    float c[nc];
    for (int k = 0; k < nc; k++) c[k] = color[k];
    float a = alpha;
    if (da >= 0.999f && (mode == EffectBlend::LinearBurn || mode == EffectBlend::ColorBurn)) { for (auto& v : c) v = 1 - (1 - v) * alpha; a = 1; }
    else if (da >= 0.999f && mode == EffectBlend::ColorDodge) { for (auto& v : c) v *= alpha; a = 1; }
    float blended[nc];
    blendEffect<M>(mode, b, c, blended);
    const float outA = a + da * (1 - a);
    if constexpr (std::is_same_v<T, float>) {
        // Linear float: nothing rounded, colour kept above 1 (a bright backdrop stays bright), alpha within 0..1.
        for (int k = 0; k < nc; k++) d[k] = std::max(0.0f, a * (1 - da) * c[k] + a * da * blended[k] + (1 - a) * da * b[k]);
        d[nc] = std::clamp(outA, 0.0f, 1.0f);
        return;
    } else {
        for (int k = 0; k < nc; k++) {
            const float premul = a * (1 - da) * c[k] + a * da * blended[k] + (1 - a) * da * b[k];
            d[k] = T(std::clamp(premul * one + 0.5f, 0.0f, one));
        }
        d[nc] = T(std::clamp(outA * one + 0.5f, 0.0f, one));
        for (int k = 0; k < nc; k++) d[k] = std::min(d[k], d[nc]);
    }
}

/// The same on a straight colour over an opaque backdrop (the interior folds).
template <ColorMode M = ColorMode::RGB>
void foldEffect(float* rgb, const float* color, float alpha, EffectBlend mode) {
    constexpr int nc = colourChannels<M>;
    if (alpha <= 0) return;
    alpha = unit(alpha);
    if (mode == EffectBlend::Dissolve) mode = EffectBlend::Normal;
    float c[nc];
    for (int k = 0; k < nc; k++) c[k] = color[k];
    float a = alpha;
    if (mode == EffectBlend::LinearBurn || mode == EffectBlend::ColorBurn) { for (auto& v : c) v = 1 - (1 - v) * alpha; a = 1; }
    else if (mode == EffectBlend::ColorDodge) { for (auto& v : c) v *= alpha; a = 1; }
    float blended[nc];
    blendEffect<M>(mode, rgb, c, blended);
    for (int k = 0; k < nc; k++) rgb[k] = rgb[k] + (blended[k] - rgb[k]) * a;
}

/// Photoshop's exterior knockout: the layer and its shadow or glow contribute additively against the backdrop.
float exteriorKnockout(float shape, float paint) {
    const float remaining = 1 - unit(paint);
    return remaining <= 0 ? 0 : std::min(1.0f, (1 - unit(shape)) / remaining);
}

EffectBlend asEffect(BlendMode m) {
    switch (m) {
    case BlendMode::Multiply: return EffectBlend::Multiply;
    case BlendMode::Screen: return EffectBlend::Screen;
    case BlendMode::Overlay: return EffectBlend::Overlay;
    case BlendMode::Darken: return EffectBlend::Darken;
    case BlendMode::Lighten: return EffectBlend::Lighten;
    case BlendMode::Difference: return EffectBlend::Difference;
    case BlendMode::ColorDodge: return EffectBlend::ColorDodge;
    case BlendMode::ColorBurn: return EffectBlend::ColorBurn;
    case BlendMode::Hue: return EffectBlend::Hue;
    case BlendMode::Saturation: return EffectBlend::Saturation;
    case BlendMode::Color: return EffectBlend::Color;
    case BlendMode::Luminosity: return EffectBlend::Luminosity;
    case BlendMode::Dissolve: return EffectBlend::Dissolve;
    case BlendMode::LinearBurn: return EffectBlend::LinearBurn;
    case BlendMode::DarkerColor: return EffectBlend::DarkerColor;
    case BlendMode::LinearDodge: return EffectBlend::LinearDodge;
    case BlendMode::LighterColor: return EffectBlend::LighterColor;
    case BlendMode::SoftLight: return EffectBlend::SoftLight;
    case BlendMode::HardLight: return EffectBlend::HardLight;
    case BlendMode::VividLight: return EffectBlend::VividLight;
    case BlendMode::LinearLight: return EffectBlend::LinearLight;
    case BlendMode::PinLight: return EffectBlend::PinLight;
    case BlendMode::HardMix: return EffectBlend::HardMix;
    case BlendMode::Exclusion: return EffectBlend::Exclusion;
    case BlendMode::Subtract: return EffectBlend::Subtract;
    case BlendMode::Divide: return EffectBlend::Divide;
    default: return EffectBlend::Normal;
    }
}

struct Pattern {
    const PatternTile* tile = nullptr;
    /// A CMYK or Lab document's copy of the tile: straight colour in its channels, then alpha (0..1), `channels` a texel.
    const float* native = nullptr;
    int channels = 4;
    double anchorX = 0, anchorY = 0, inverseScale = 1, cosine = 1, sine = 0;
    bool nearest = true, box = false;
    /// A 16- or 32-bit document's: a pattern stored at 16 bits is sampled at 15 (its `rgba16`), not its 8-bit copy.
    bool wide = false;
    void sample(double x, double y, float rgb[3], float& alpha) const {
        if (native) { sampleNative(x, y, rgb, alpha); return; }
        if (wide && !tile->rgba16.empty()) sampleTexels(tile->rgba16.data(), float(one16), x, y, rgb, alpha);
        else sampleTexels(tile->rgba.data(), 255.0f, x, y, rgb, alpha);
    }
    /// The sampling on straight RGBA texels whose full value is `unit`.
    template <class Texel>
    void sampleTexels(const Texel* texels, float unit, double x, double y, float rgb[3], float& alpha) const {
        auto texel = [&](long tx, long ty) {
            tx %= tile->width; if (tx < 0) tx += tile->width;
            ty %= tile->height; if (ty < 0) ty += tile->height;
            return texels + (size_t(ty) * size_t(tile->width) + size_t(tx)) * 4;
        };
        if (box) {
            // Minified: average the texels under the pixel's footprint, weighted by how much of each it covers.
            const double u0 = (x - anchorX) * inverseScale, u1 = (x + 1 - anchorX) * inverseScale;
            const double v0 = (y - anchorY) * inverseScale, v1 = (y + 1 - anchorY) * inverseScale;
            double sum[4] = {0, 0, 0, 0}, total = 0;
            for (long ty = long(std::floor(v0)); ty <= long(std::ceil(v1)) - 1; ty++) {
                const double cy = std::min(v1, ty + 1.0) - std::max(v0, double(ty));
                if (cy <= 0) continue;
                for (long tx = long(std::floor(u0)); tx <= long(std::ceil(u1)) - 1; tx++) {
                    const double cx = std::min(u1, tx + 1.0) - std::max(u0, double(tx));
                    if (cx <= 0) continue;
                    const Texel* p = texel(tx, ty);
                    for (int k = 0; k < 4; k++) sum[k] += cx * cy * p[k];
                    total += cx * cy;
                }
            }
            for (int k = 0; k < 3; k++) rgb[k] = total > 0 ? float(sum[k] / total / unit) : 0;
            alpha = total > 0 ? float(sum[3] / total / unit) : 0;
            return;
        }
        if (nearest) {
            const Texel* p = texel(long(std::floor(x + 0.5 - anchorX)), long(std::floor(y + 0.5 - anchorY)));
            for (int k = 0; k < 3; k++) rgb[k] = p[k] / unit;
            alpha = p[3] / unit;
            return;
        }
        double u = x - anchorX, v = y - anchorY;
        const double ru = u * cosine - v * sine, rv = u * sine + v * cosine;
        u = ru * inverseScale; v = rv * inverseScale;
        const double fu = std::floor(u), fv = std::floor(v);
        const float tx = float(u - fu), ty = float(v - fv);
        const Texel *a = texel(long(fu), long(fv)), *b = texel(long(fu) + 1, long(fv)), *c = texel(long(fu), long(fv) + 1), *d = texel(long(fu) + 1, long(fv) + 1);
        auto mix = [&](int k) { return ((a[k] * (1 - tx) + b[k] * tx) * (1 - ty) + (c[k] * (1 - tx) + d[k] * tx) * ty) / unit; };
        for (int k = 0; k < 3; k++) rgb[k] = mix(k);
        alpha = mix(3);
    }
    /// The same sampling on the native tile (`channels` - 1 colour values into `c`).
    void sampleNative(double x, double y, float* c, float& alpha) const {
        const int n = channels, nc = n - 1;
        auto texel = [&](long tx, long ty) {
            tx %= tile->width; if (tx < 0) tx += tile->width;
            ty %= tile->height; if (ty < 0) ty += tile->height;
            return native + (size_t(ty) * size_t(tile->width) + size_t(tx)) * size_t(n);
        };
        if (box) {
            const double u0 = (x - anchorX) * inverseScale, u1 = (x + 1 - anchorX) * inverseScale;
            const double v0 = (y - anchorY) * inverseScale, v1 = (y + 1 - anchorY) * inverseScale;
            double sum[5] = {0, 0, 0, 0, 0}, total = 0;
            for (long ty = long(std::floor(v0)); ty <= long(std::ceil(v1)) - 1; ty++) {
                const double cy = std::min(v1, ty + 1.0) - std::max(v0, double(ty));
                if (cy <= 0) continue;
                for (long tx = long(std::floor(u0)); tx <= long(std::ceil(u1)) - 1; tx++) {
                    const double cx = std::min(u1, tx + 1.0) - std::max(u0, double(tx));
                    if (cx <= 0) continue;
                    const float* p = texel(tx, ty);
                    for (int k = 0; k < n; k++) sum[k] += cx * cy * p[k];
                    total += cx * cy;
                }
            }
            for (int k = 0; k < nc; k++) c[k] = total > 0 ? float(sum[k] / total) : 0;
            alpha = total > 0 ? float(sum[nc] / total) : 0;
            return;
        }
        if (nearest) {
            const float* p = texel(long(std::floor(x + 0.5 - anchorX)), long(std::floor(y + 0.5 - anchorY)));
            for (int k = 0; k < nc; k++) c[k] = p[k];
            alpha = p[nc];
            return;
        }
        double u = x - anchorX, v = y - anchorY;
        const double ru = u * cosine - v * sine, rv = u * sine + v * cosine;
        u = ru * inverseScale; v = rv * inverseScale;
        const double fu = std::floor(u), fv = std::floor(v);
        const float tx = float(u - fu), ty = float(v - fv);
        const float *a = texel(long(fu), long(fv)), *b = texel(long(fu) + 1, long(fv)), *cc = texel(long(fu), long(fv) + 1), *d = texel(long(fu) + 1, long(fv) + 1);
        auto mix = [&](int k) { return (a[k] * (1 - tx) + b[k] * tx) * (1 - ty) + (cc[k] * (1 - tx) + d[k] * tx) * ty; };
        for (int k = 0; k < nc; k++) c[k] = mix(k);
        alpha = mix(nc);
    }
};

Pattern patternFor(const PatternTile* tile, const LayerStyle& style, float scale, float angle, bool link, float phaseX, float phaseY) {
    Pattern p;
    p.tile = tile;
    p.anchorX = phaseX + (link ? style.referenceX : 0);
    p.anchorY = phaseY + (link ? style.referenceY : 0);
    p.inverseScale = 1.0 / std::max(0.01, double(scale));
    const double r = angle * M_PI / 180;
    p.cosine = std::cos(r); p.sine = std::sin(r);
    p.nearest = std::abs(r) < 1e-9 && std::abs(scale - 1) < 1e-6;
    p.box = !p.nearest && std::abs(r) < 1e-9 && scale < 1;
    return p;
}

/// An image of `Img`'s type with `channels` samples a pixel (the 8-bit RGB and Lab `Image` has four).
template <class Img>
Img blankImage(int w, int h, int channels) {
    if constexpr (std::is_constructible_v<Img, int, int, int>) return Img(w, h, channels);
    else { (void)channels; return Img(w, h); }
}

/// What a CMYK or Lab document's effects need besides their masks: their colours through the document's profile,
/// gradients as ramps of converted colours, and pattern tiles converted once.
template <SampleType S, ColorMode M>
struct NativeColours {
    static constexpr int nc = colourChannels<M>;
    static constexpr int lutSteps = 1024;
    ColorTransformPtr transform;
    explicit NativeColours(const StyledDraw& in) {
        transform = transformBetween(ColorProfile(), in.profile ? *in.profile : ColorProfile(), ConvertOptions(), PixelFormat::RGBFloat,
                                     M == ColorMode::CMYK ? PixelFormat::CMYKFloat : PixelFormat::LabFloat);
    }
    /// `count` sRGB colours (0..1, three floats each) as the document's stored channels (0..1, `nc` each): CMYK's
    /// complements (1 is no ink), Lab's L, a and b with a and b offset as the document stores them.
    void convert(const float* rgb, float* out, size_t count) const {
        std::vector<float> lc(count * 4, 0.0f);
        if (transform) transform->apply(rgb, lc.data(), count);
        for (size_t i = 0; i < count; i++) {
            const float* v = lc.data() + i * (M == ColorMode::CMYK ? 4 : 3);
            float* o = out + i * nc;
            if (!transform) {
                // A profile that cannot be used: the plain conversion (grey from the RGB's mean in Lab).
                const float* c = rgb + i * 3;
                if constexpr (M == ColorMode::CMYK) {
                    const float k = 1 - std::max({c[0], c[1], c[2]});
                    for (int j = 0; j < 3; j++) o[j] = k >= 1 ? 1 : 1 - (1 - c[j] - k) / (1 - k);
                    o[3] = 1 - k;
                } else { o[0] = (c[0] + c[1] + c[2]) / 3; o[1] = o[2] = float(labOffset<S>()) / float(SampleTraits<S>::one); }
                continue;
            }
            if constexpr (M == ColorMode::CMYK) for (int j = 0; j < 4; j++) o[j] = 1 - std::clamp(v[j] / 100.0f, 0.0f, 1.0f);
            else {
                const float one = float(SampleTraits<S>::one);
                o[0] = std::clamp(v[0] / 100.0f, 0.0f, 1.0f);
                for (int j = 1; j < 3; j++) o[j] = std::clamp((v[j] * float(labScale<S>()) + float(labOffset<S>())) / one, 0.0f, 1.0f);
            }
        }
    }
    void colour(const StyleColor& c, float* out) const {
        if constexpr (M == ColorMode::CMYK) if (inkMatches(c)) { for (int j = 0; j < 4; j++) out[j] = 1 - (*c.ink)[size_t(j)]; return; }
        const float rgb[3] = {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f};
        convert(rgb, out, 1);
    }
    /// A gradient's colours along its length, `lutSteps` + 1 of them. In CMYK a ramp whose stops are all inks runs from
    /// ink to ink (as a gradient fill layer's does, vectormask.cpp), not through RGB.
    std::vector<float> ramp(const StyleGradient& g) const {
        std::vector<float> out(size_t(lutSteps + 1) * nc);
        bool inks = M == ColorMode::CMYK && !g.colors.empty();
        for (auto& stop : g.colors) inks = inks && stop.ink.has_value();
        if (inks) {
            StyleGradient cmy = g, k = g;
            cmy.interpolation = k.interpolation = StyleGradient::Interpolation::Classic;
            auto level = [](float ink) { return uint8_t(std::lround((1 - ink) * 255)); };
            for (size_t i = 0; i < g.colors.size(); i++) {
                const auto& ink = *g.colors[i].ink;
                cmy.colors[i].color = {level(ink[0]), level(ink[1]), level(ink[2])};
                const uint8_t kl = level(ink[3]);
                k.colors[i].color = {kl, kl, kl};
            }
            for (int i = 0; i <= lutSteps; i++) {
                double a[3], b[3];
                gradientColorExact(cmy, float(i) / lutSteps, a);
                gradientColorExact(k, float(i) / lutSteps, b);
                float* o = out.data() + size_t(i) * nc;
                for (int j = 0; j < 3; j++) o[j] = float(std::clamp(a[j], 0.0, 255.0) / 255.0);
                o[3] = float(std::clamp(b[0], 0.0, 255.0) / 255.0);
            }
            return out;
        }
        std::vector<float> rgb(size_t(lutSteps + 1) * 3);
        for (int i = 0; i <= lutSteps; i++) {
            double c[3];
            gradientColorExact(g, float(i) / lutSteps, c);
            for (int j = 0; j < 3; j++) rgb[size_t(i) * 3 + size_t(j)] = float(std::clamp(c[j], 0.0, 255.0) / 255.0);
        }
        convert(rgb.data(), out.data(), size_t(lutSteps + 1));
        return out;
    }
    static void atRamp(const std::vector<float>& lut, float t, float* out) {
        const float x = std::clamp(t, 0.0f, 1.0f) * lutSteps;
        const int i = std::min(lutSteps - 1, int(x));
        const float f = x - float(i);
        const float* a = lut.data() + size_t(i) * nc;
        const float* b = a + nc;
        for (int j = 0; j < nc; j++) out[j] = a[j] + (b[j] - a[j]) * f;
    }
    /// A pattern tile in the document's channels, straight, alpha last (a 16-bit pattern's 15-bit samples in a 16-bit
    /// document).
    std::vector<float> tile(const PatternTile& t) const {
        const size_t count = size_t(t.width) * size_t(t.height);
        const bool wide = S != SampleType::U8 && !t.rgba16.empty();
        auto texel = [&](size_t i) { return wide ? t.rgba16[i] / float(one16) : t.rgba[i] / 255.0f; };
        std::vector<float> rgb(count * 3), colours(count * nc), out(count * (nc + 1));
        for (size_t i = 0; i < count; i++) for (int j = 0; j < 3; j++) rgb[i * 3 + size_t(j)] = texel(i * 4 + size_t(j));
        convert(rgb.data(), colours.data(), count);
        for (size_t i = 0; i < count; i++) {
            for (int j = 0; j < nc; j++) out[i * (nc + 1) + size_t(j)] = colours[i * nc + size_t(j)];
            out[i * (nc + 1) + nc] = texel(i * 4 + 3);
        }
        return out;
    }
};

/// The layer and its effects at the target's depth: the effects are worked out on float masks either way, and only
/// reading the source and backdrop and writing the result know the sample type. In a CMYK or Lab document (`M`) the
/// masks are the same; the effects' colours come through the document's profile and every blend is in its channels.
template <SampleType S, ColorMode M, class Img>
void drawStyled(const StyledDraw& in, Img& target) {
    using T = SampleOf<S>;
    constexpr int nc = colourChannels<M>, n = nc + 1;
    constexpr bool native = M != ColorMode::RGB;
    constexpr float one = float(SampleTraits<S>::one);
    const LayerStyle& style = *in.style;
    const int outW = target.width(), outH = target.height();
    const double s = in.scale;
    const int pad = int(std::ceil(style.reach() * s)) + 2;
    // The working area, in output pixels: the layer's bounds and the effects' reach, within the output and that reach.
    int wx0 = -pad, wy0 = -pad, wx1 = outW + pad, wy1 = outH + pad;
    if (in.bounds) {
        wx0 = std::max(wx0, int(std::floor((in.bounds->x - in.region.x) * s)) - pad);
        wy0 = std::max(wy0, int(std::floor((in.bounds->y - in.region.y) * s)) - pad);
        wx1 = std::min(wx1, int(std::ceil((in.bounds->x + in.bounds->width - in.region.x) * s)) + pad);
        wy1 = std::min(wy1, int(std::ceil((in.bounds->y + in.bounds->height - in.region.y) * s)) + pad);
    }
    if (wx1 <= wx0 || wy1 <= wy0 || wx1 <= 0 || wy1 <= 0 || wx0 >= outW || wy0 >= outH) return;
    const int w = wx1 - wx0, h = wy1 - wy0;
    if ((long long)w * h > 400LL * 1000 * 1000) return;
    const Rect padded(in.region.x + wx0 / s, in.region.y + wy0 / s, w / s, h / s);
    Img source = blankImage<Img>(w, h, n);
    if constexpr (std::is_same_v<Img, ImageC8>) in.drawSourceC8(source, padded);
    else if constexpr (S == SampleType::U8) in.drawSource(source, padded);
    else if constexpr (S == SampleType::U16) in.drawSource16(source, padded);
    else in.drawSourceF(source, padded);
    Mask alpha(size_t(w) * h);
    bool any = false;
    for (int y = 0; y < h; y++) {
        const T* p = source.row(y);
        for (int x = 0; x < w; x++) {
            float a = p[x * n + nc] / one;
            // Coverage under half an 8-bit level is clear to the effects (as the healers treat it): the mattes' painted and
            // contour tests then see the pixels an 8-bit layer would.
            if constexpr (S == SampleType::U16) if (narrow16(p[x * n + nc]) == 0) a = 0;
            if constexpr (S == SampleType::F32) if (a < 0.5f / 255) a = 0;
            alpha[size_t(y) * w + x] = a;
            any |= p[x * n + nc] != 0;
        }
    }
    if (!any) return;
    const float master = in.master, fill = in.fill;
    const GrayOf<S>* cover = nullptr;
    if constexpr (S == SampleType::U8) cover = in.coverage;
    else if constexpr (S == SampleType::U16) cover = in.coverage16;
    else cover = in.coverageF;
    // At 32 bits the effects' colours are 8-bit values in the document's encoding: linearised for the float target.
    auto linearised = [&](float c[3]) {
        if constexpr (S == SampleType::F32) {
            static const TransferCurve srgb = TransferCurve::srgb();
            const TransferCurve& curve = in.linear ? *in.linear : srgb;
            for (int k = 0; k < 3; k++) c[k] = curve.toLinear(c[k]);
        } else (void)c;
    };
    auto coverAt = [&](int ox, int oy) { return cover ? cover->row(oy)[ox] / one : 1.0f; };
    // CMYK and Lab: the colours, ramps and tiles in the document's channels, worked out once before drawing.
    std::optional<NativeColours<S, M>> colours;
    std::map<const StyleGradient*, std::vector<float>> ramps;
    std::map<const PatternTile*, std::vector<float>> nativeTiles;
    if constexpr (native) {
        colours.emplace(in);
        auto addRamp = [&](const StyleGradient& g) { ramps[&g] = colours->ramp(g); };
        for (const GradientOverlay& g : style.gradientOverlays) addRamp(g.gradient);
        for (const Stroke& stroke : style.strokes) if (stroke.gradientFill) addRamp(stroke.gradient);
        if (in.patterns) for (const PatternOverlay& p : style.patternOverlays) {
            auto it = in.patterns->find(p.patternId);
            if (it != in.patterns->end() && it->second.width > 0 && !nativeTiles.count(&it->second)) nativeTiles[&it->second] = colours->tile(it->second);
        }
    }
    auto rgb = [&](StyleColor c, float* out) {
        if constexpr (native) { colours->colour(c, out); return; }
        out[0] = c.r / 255.0f; out[1] = c.g / 255.0f; out[2] = c.b / 255.0f; linearised(out);
    };
    // A gradient's colour: at 8 bits rounded to a byte as before; at 16 the ramp's exact colour, without 8-bit steps.
    auto gradRgb = [&](const StyleGradient& g, float t, float* out) {
        if constexpr (native) NativeColours<S, M>::atRamp(ramps.at(&g), t, out);
        else if constexpr (S == SampleType::U8) rgb(gradientColor(g, t), out);
        else {
            double c[3];
            gradientColorExact(g, t, c);
            for (int k = 0; k < 3; k++) out[k] = float(std::clamp(c[k], 0.0, 255.0) / 255.0);
            linearised(out);
        }
    };
    // Document pixel under output pixel (ox, oy), for gradients and patterns.
    auto docX = [&](int ox) { return std::floor(in.region.x + (ox + 0.5) / s); };
    auto docY = [&](int oy) { return std::floor(in.region.y + (oy + 0.5) / s); };
    // The layer's painted bounds in document pixels (gradients aligned with the layer).
    double bx0 = 1e18, by0 = 1e18, bx1 = -1e18, by1 = -1e18;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) if (alpha[size_t(y) * w + x] > 0) {
        bx0 = std::min(bx0, double(x)); bx1 = std::max(bx1, double(x + 1)); by0 = std::min(by0, double(y)); by1 = std::max(by1, double(y + 1));
    }
    const double boundsX = std::floor(padded.x + bx0 / s), boundsY = std::floor(padded.y + by0 / s);
    const double boundsW = std::max(1.0, std::round((bx1 - bx0) / s)), boundsH = std::max(1.0, std::round((by1 - by0) / s));
    auto gradientBounds = [&](const StyleGradient& g, double& x, double& y, double& gw, double& gh) {
        if (g.alignWithLayer) { x = boundsX; y = boundsY; gw = boundsW; gh = boundsH; }
        else { x = 0; y = 0; gw = in.documentWidth; gh = in.documentHeight; }
    };
    auto offset = [&](float angle, float distance, int& dx, int& dy) {
        const float r = (180 - angle) * kPi / 180;
        dx = int(std::lround(std::cos(r) * distance * s)); dy = int(std::lround(std::sin(r) * distance * s));
    };
    auto shifted = [&](int dx, int dy) {
        Mask m(alpha.size(), 0);
        for (int y = 0; y < h; y++) {
            const int sy = y - dy;
            if (sy < 0 || sy >= h) continue;
            for (int x = 0; x < w; x++) { const int sx = x - dx; if (sx >= 0 && sx < w) m[size_t(y) * w + x] = alpha[size_t(sy) * w + sx]; }
        }
        return m;
    };
    const int ox0 = std::max(0, wx0), ox1 = std::min(outW, wx1), oy0 = std::max(0, wy0), oy1 = std::min(outH, wy1);
    auto paintOut = [&](const std::function<void(int ox, int oy, T* d, size_t i)>& f) {
        parallelRows(oy0, oy1, [&](int a, int b) {
            for (int oy = a; oy < b; oy++) {
                T* row = target.row(oy);
                for (int ox = ox0; ox < ox1; ox++) f(ox, oy, row + ox * n, size_t(oy - wy0) * w + size_t(ox - wx0));
            }
        }, 32);
    };

    // What the exterior effects paint under: the layer's own blend reads the backdrop from before them.
    std::optional<Img> backdrop;
    const bool exterior = !style.dropShadows.empty() || !style.outerGlows.empty();
    if (exterior && in.mode != BlendMode::Normal) backdrop = target;

    using Phase = StyledDraw::Phase;
    const bool drawExterior = in.phase == Phase::Whole || in.phase == Phase::Exterior, drawInterior = in.phase != Phase::Exterior;
    const bool drawOverlays = in.phase != Phase::InteriorRest, drawRest = in.phase != Phase::InteriorOverlays;
    const bool ownContent = in.phase == StyledDraw::Phase::Whole;

    // Exterior: drop shadows, then outer glows.
    if (drawExterior) for (const DropShadow& shadow : style.dropShadows) {
        if (shadow.opacity <= 0) continue;
        int dx, dy;
        offset(shadow.angle, shadow.distance, dx, dy);
        Mask m = shifted(dx, dy);
        softExterior(m, w, h, shadow.size * float(s), shadow.spread);
        float color[4] = {}; rgb(shadow.color, color);
        paintOut([&](int ox, int oy, T* d, size_t i) {
            float a = m[i] * shadow.opacity * master * coverAt(ox, oy);
            if (shadow.layerConceals) a *= exteriorKnockout(alpha[i], alpha[i] * fill * master);
            compositeEffect<T, M>(d, color, a, shadow.mode);
        });
    }
    if (drawExterior) for (const OuterGlow& glow : style.outerGlows) {
        if (glow.opacity <= 0 || glow.size <= 0) continue;
        Mask m = alpha;
        if (glow.precise) m = distanceFalloff(alpha, w, h, glow.size * float(s), glow.spread);
        else {
            softExterior(m, w, h, glow.size * float(s), glow.spread);
            const float gain = 100 / std::clamp(glow.range, 1.0f, 100.0f);
            if (gain > 1) for (auto& v : m) v = std::min(1.0f, v * gain);
        }
        float color[4] = {}; rgb(glow.color, color);
        paintOut([&](int ox, int oy, T* d, size_t i) {
            compositeEffect<T, M>(d, color, m[i] * exteriorKnockout(alpha[i], alpha[i] * fill * master) * glow.opacity * master * coverAt(ox, oy), glow.mode);
        });
    }

    if (!drawInterior) return;

    // Strokes: their bands, and (Overprint off, the default) the knockout they make in the layer's own content.
    std::vector<Mask> bands, burstPositions;
    Mask knockout;
    for (const Stroke& stroke : style.strokes) {
        const bool burst = stroke.gradientFill && stroke.gradient.type == StyleGradient::Type::ShapeBurst;
        burstPositions.emplace_back();
        bands.push_back(strokeBand(alpha, w, h, stroke.size * float(s), stroke.position, burst ? &burstPositions.back() : nullptr));
        // Overprint off (the default): the band knocks the layer's content out, even at 0% opacity.
        if (!stroke.overprint && ownContent) {
            if (knockout.empty()) knockout.assign(alpha.size(), 1);
            for (size_t i = 0; i < knockout.size(); i++) knockout[i] *= 1 - bands.back()[i];
        }
    }
    auto knock = [&](size_t i) { return knockout.empty() ? 1.0f : knockout[i]; };

    // Interior overlays (pattern under gradient under colour) and satins, folded into the layer's colour.
    auto patterns = in.patterns;
    auto findPattern = [&](const std::string& id) -> const PatternTile* {
        if (!patterns) return nullptr;
        auto it = patterns->find(id);
        return it == patterns->end() || it->second.width <= 0 ? nullptr : &it->second;
    };
    // An overlay's pattern: in CMYK and Lab sampled from the tile converted to the document's channels.
    auto overlayPattern = [&](const PatternTile* tile, const PatternOverlay& p) {
        Pattern pattern = patternFor(tile, style, p.scale, p.angle, p.linkWithLayer, p.phaseX, p.phaseY);
        pattern.wide = S != SampleType::U8;
        if constexpr (native) { pattern.native = nativeTiles.at(tile).data(); pattern.channels = n; }
        return pattern;
    };
    struct SatinMask { const Satin* satin; Mask mask; };
    std::vector<SatinMask> satins;
    for (const Satin& satin : style.satins) {
        if (satin.opacity <= 0) continue;
        const float r = (180 - satin.angle) * kPi / 180, dist = std::max(1.0f, satin.distance) * float(s);
        const int dx = int(std::lround(std::cos(r) * dist)), dy = int(std::lround(std::sin(r) * dist));
        Mask m = shifted(dx, dy);
        const Mask opposite = shifted(-dx, -dy);
        for (size_t i = 0; i < m.size(); i++) m[i] -= opposite[i];
        tentBlur(m, w, h, satin.size * float(s));
        for (auto& v : m) { v = unit(std::abs(v)); if (satin.invert) v = 1 - v; }
        satins.push_back({&satin, std::move(m)});
    }
    const bool foldables = !style.patternOverlays.empty() || !style.gradientOverlays.empty() || !style.colorOverlays.empty() || !satins.empty();
    const bool foldInteriors = fill >= 0.999f && ownContent && !(in.afterContent && foldables);
    auto foldInto = [&](float* c, int ox, int oy, size_t i) {
        const double x = docX(ox), y = docY(oy);
        for (const PatternOverlay& p : style.patternOverlays) {
            const PatternTile* tile = findPattern(p.patternId);
            if (!tile || p.opacity <= 0) continue;
            float pc[4] = {}, pa = 0;
            overlayPattern(tile, p).sample(x, y, pc, pa);
            linearised(pc);
            foldEffect<M>(c, pc, pa * p.opacity, p.mode);
        }
        for (const GradientOverlay& g : style.gradientOverlays) {
            if (g.opacity <= 0) continue;
            double gx, gy, gw, gh;
            gradientBounds(g.gradient, gx, gy, gw, gh);
            const float t = gradientPosition(g.gradient, gx, gy, gw, gh, x, y);
            float gc[4] = {}; gradRgb(g.gradient, t, gc);
            foldEffect<M>(c, gc, g.opacity * gradientOpacity(g.gradient, t), g.mode);
        }
        for (const ColorOverlay& o : style.colorOverlays) { float oc[4] = {}; rgb(o.color, oc); foldEffect<M>(c, oc, o.opacity, o.mode); }
        for (auto& sm : satins) {
            float sc[4] = {}; rgb(sm.satin->color, sc);
            // Satin keeps the plain model (Patchy's pinned fold), even for the burn and dodge modes.
            const float a = sm.mask[i] * sm.satin->opacity;
            if (a <= 0) continue;
            float blended[4];
            blendEffect<M>(sm.satin->mode == EffectBlend::Dissolve ? EffectBlend::Normal : sm.satin->mode, c, sc, blended);
            for (int k = 0; k < nc; k++) c[k] += (blended[k] - c[k]) * a;
        }
    };

    // The layer itself: with Blend Interior Effects as Group the interiors fold into its colour and its mode
    // carries them; without (Photoshop's default) its mode blends its own pixels and the interiors land on that.
    const bool interiorsFirst = style.blendInteriorAsGroup;
    const EffectBlend layerMode = asEffect(in.mode);
    if (ownContent) paintOut([&](int ox, int oy, T* d, size_t i) {
        const T* p = source.row(oy - wy0) + (ox - wx0) * n;
        if (p[nc] == 0) return;
        const float a = p[nc] / one;
        float c[4] = {};
        for (int k = 0; k < nc; k++) {
            if constexpr (S == SampleType::F32) c[k] = std::max(0.0f, p[k] / a);   // linear, above 1 kept
            else c[k] = std::min(1.0f, p[k] / one / a);
        }
        const float paint = a * master * fill * knock(i) * coverAt(ox, oy);
        if (paint <= 0) return;
        if (foldInteriors && interiorsFirst) foldInto(c, ox, oy, i);
        if (in.mode != BlendMode::Normal && foldInteriors && !interiorsFirst) {
            // Blend against the backdrop first, then the interiors over that, then a plain source-over.
            const T* bd = backdrop ? backdrop->row(oy) + ox * n : d;
            const float ba = bd[nc] / one;
            if (ba > 0) {
                float b[4], blended[4];
                for (int k = 0; k < nc; k++) b[k] = bd[k] / one / ba;
                blendEffect<M>(layerMode, b, c, blended);
                for (int k = 0; k < nc; k++) c[k] = c[k] + (blended[k] - c[k]) * ba;
            }
            foldInto(c, ox, oy, i);
            compositeEffect<T, M>(d, c, paint, EffectBlend::Normal);
            return;
        }
        if (foldInteriors && !interiorsFirst) foldInto(c, ox, oy, i);
        if (backdrop && in.mode != BlendMode::Normal) {
            // Exterior effects under a blended layer: blend against the backdrop from before them.
            const T* bd = backdrop->row(oy) + ox * n;
            const float ba = bd[nc] / one;
            if (ba > 0) {
                float b[4], blended[4];
                for (int k = 0; k < nc; k++) b[k] = bd[k] / one / ba;
                blendEffect<M>(layerMode, b, c, blended);
                for (int k = 0; k < nc; k++) c[k] = c[k] + (blended[k] - c[k]) * ba;
            }
            compositeEffect<T, M>(d, c, paint, EffectBlend::Normal);
            return;
        }
        compositeEffect<T, M>(d, c, paint, layerMode);
    });
    if (ownContent && in.afterContent) in.afterContent();

    // With Fill below 100% the overlays and satins are their own passes (Fill fades the pixels, not the effects).
    if (!foldInteriors && drawOverlays) {
        paintOut([&](int ox, int oy, T* d, size_t i) {
            const float shape = alpha[i] * master * knock(i) * coverAt(ox, oy);
            if (shape <= 0) return;
            const double x = docX(ox), y = docY(oy);
            for (const PatternOverlay& p : style.patternOverlays) {
                const PatternTile* tile = findPattern(p.patternId);
                if (!tile || p.opacity <= 0) continue;
                float pc[4] = {}, pa = 0;
                overlayPattern(tile, p).sample(x, y, pc, pa);
                linearised(pc);
                compositeEffect<T, M>(d, pc, shape * pa * p.opacity, p.mode);
            }
            for (const GradientOverlay& g : style.gradientOverlays) {
                double gx, gy, gw, gh;
                gradientBounds(g.gradient, gx, gy, gw, gh);
                const float t = gradientPosition(g.gradient, gx, gy, gw, gh, x, y);
                float gc[4] = {}; gradRgb(g.gradient, t, gc);
                compositeEffect<T, M>(d, gc, shape * g.opacity * gradientOpacity(g.gradient, t), g.mode);
            }
            for (const ColorOverlay& o : style.colorOverlays) { float oc[4] = {}; rgb(o.color, oc); compositeEffect<T, M>(d, oc, shape * o.opacity, o.mode); }
            for (auto& sm : satins) { float sc[4] = {}; rgb(sm.satin->color, sc); compositeEffect<T, M>(d, sc, shape * sm.mask[i] * sm.satin->opacity, sm.satin->mode); }
        });
    }
    if (!drawRest) return;

    // Inner glows under inner shadows.
    for (const InnerGlow& glow : style.innerGlows) {
        if (glow.opacity <= 0 || glow.size <= 0) continue;
        Mask m = alpha;
        const float size = glow.size * float(s);
        if (glow.precise) {
            for (auto& v : m) v = 1 - unit(v);
            expand(m, w, h, size * unit(glow.choke / 100));
            boxBlur(m, w, h, std::max(0, int(std::lround(size * (1 - unit(glow.choke / 100)) * 0.5f))), 3);
            for (auto& v : m) v = glow.center ? unit(1 - unit(v)) : unit(v);
        } else {
            softInterior(m, w, h, size, glow.choke);
            const float gain = 100 / std::clamp(glow.range, 1.0f, 100.0f);
            for (auto& v : m) v = glow.center ? 1 - std::min(1.0f, v * gain) : std::min(1.0f, v * gain);
        }
        float color[4] = {}; rgb(glow.color, color);
        paintOut([&](int ox, int oy, T* d, size_t i) {
            if (alpha[i] <= 0) return;
            compositeEffect<T, M>(d, color, alpha[i] * m[i] * glow.opacity * master * knock(i) * coverAt(ox, oy), glow.mode);
        });
    }
    for (const InnerShadow& shadow : style.innerShadows) {
        if (shadow.opacity <= 0 || shadow.size <= 0) continue;
        int dx, dy;
        offset(shadow.angle, shadow.distance, dx, dy);
        Mask m = shifted(dx, dy);
        softInterior(m, w, h, shadow.size * float(s), shadow.choke);
        float color[4] = {}; rgb(shadow.color, color);
        paintOut([&](int ox, int oy, T* d, size_t i) {
            if (alpha[i] <= 0) return;
            compositeEffect<T, M>(d, color, alpha[i] * m[i] * shadow.opacity * master * knock(i) * coverAt(ox, oy), shadow.mode);
        });
    }

    // Strokes.
    for (size_t k = 0; k < style.strokes.size(); k++) {
        const Stroke& stroke = style.strokes[k];
        if (stroke.opacity <= 0) continue;
        const Mask& band = bands[k];
        float color[4] = {}; rgb(stroke.color, color);
        paintOut([&](int ox, int oy, T* d, size_t i) {
            float a = band[i] * stroke.opacity * master * coverAt(ox, oy);
            if (a <= 0) return;
            float c[4] = {color[0], color[1], color[2], color[3]};
            if (stroke.gradientFill && stroke.gradient.type == StyleGradient::Type::ShapeBurst) {
                // Photoshop supersamples the ramp over the pixel: a 1-2-1 tent of the colours around it.
                const float step = 1.0f / std::max(1.0f, stroke.size * float(s) + 2);
                float t = burstPositions[k][i];
                if (stroke.gradient.reverse) t = 1 - t;
                float acc[4] = {0, 0, 0, 0};
                for (int j = -1; j <= 1; j++) {
                    float cc[4] = {}; gradRgb(stroke.gradient, unit(t + j * step), cc);
                    const float wgt = j == 0 ? 0.5f : 0.25f;
                    for (int q = 0; q < nc; q++) acc[q] += cc[q] * wgt;
                }
                for (int q = 0; q < nc; q++) c[q] = acc[q];
                a *= gradientOpacity(stroke.gradient, t);
            } else if (stroke.gradientFill) {
                double gx, gy, gw, gh;
                gradientBounds(stroke.gradient, gx, gy, gw, gh);
                const float t = gradientPosition(stroke.gradient, gx, gy, gw, gh, docX(ox), docY(oy));
                gradRgb(stroke.gradient, t, c);
                a *= gradientOpacity(stroke.gradient, t);
            }
            compositeEffect<T, M>(d, c, a, stroke.mode);
        });
    }

    // Bevels and embosses: a height field from the matte (or the stroke), lit with Photoshop's calibrated Lambert split.
    for (const Bevel& bevel : style.bevels) {
        if (bevel.highlightOpacity <= 0 && bevel.shadowOpacity <= 0) continue;
        Mask matte;
        if (bevel.kind == Bevel::Kind::StrokeEmboss) {
            if (bands.empty()) continue;
            matte.assign(alpha.size(), 0);
            for (size_t k = 0; k < bands.size(); k++) for (size_t i = 0; i < matte.size(); i++) {
                const float a = bands[k][i] * unit(style.strokes[k].opacity);
                matte[i] = a + matte[i] * (1 - a);
            }
        } else matte = alpha;
        const float size = bevel.size * float(s);
        const bool pillowFamily = bevel.kind == Bevel::Kind::Pillow || bevel.kind == Bevel::Kind::Emboss;
        const Mask base = bevelHeight(matte, w, h, bevel, pillowFamily ? size * 0.5f : size);
        float slopeGain = 1;
        if (bevel.useContour && bevel.contour.linear()) slopeGain = 1 / std::clamp(bevel.contourRange, 0.01f, 1.0f);
        const bool contourCurve = bevel.useContour && !bevel.contour.linear();
        const auto contourLut = contourCurve ? bevel.contour.lut() : std::array<uint8_t, 256>{};
        const float contourRange = std::clamp(bevel.contourRange, 0.01f, 1.0f);
        // The texture's bump before its coverage: the pattern's luminance, Photoshop raising the dark texels (Invert
        // raises the light ones, a negative depth flips it again), box-smoothed so shading covers whole texel plateaus.
        // An Outer Bevel's texture comes out the other way up (photoshop-bevel-subs: Invert with a negative depth
        // raises the light texels there).
        Mask bump;
        if (bevel.useTexture) {
            if (const PatternTile* tile = findPattern(bevel.texturePattern)) {
                const Pattern p = patternFor(tile, style, bevel.textureScale, 0, bevel.textureLinkWithLayer, bevel.texturePhaseX, bevel.texturePhaseY);
                bump.assign(base.size(), 0);
                const bool raiseLight = bevel.textureInvert != (bevel.kind == Bevel::Kind::Outer);
                for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                    float c[3], a;
                    p.sample(std::floor(padded.x + (x + 0.5) / s), std::floor(padded.y + (y + 0.5) / s), c, a);
                    const float lum = (299 * c[0] + 590 * c[1] + 111 * c[2]) / 1000;
                    bump[size_t(y) * w + x] = raiseLight ? lum - 0.5f : 0.5f - lum;
                }
                boxBlur(bump, w, h, 1, 1);
            }
        }
        // The surface at an offset of (dx, dy) pixels: the base field (bilinear between pixels, nothing beyond the
        // window), through the Contour, plus the texture over the face it covers.
        auto surfaceAt = [&](float dx, float dy, Mask& out) {
            out.resize(base.size());
            const int ix = int(std::floor(dx)), iy = int(std::floor(dy));
            const float fx = dx - float(ix), fy = dy - float(iy);
            auto at = [&](const Mask& m, int x, int y) { return x < 0 || y < 0 || x >= w || y >= h ? 0.0f : m[size_t(y) * w + x]; };
            auto lerp2 = [&](const Mask& m, int x, int y) {
                x += ix; y += iy;
                if (fx == 0 && fy == 0) return at(m, x, y);
                return (at(m, x, y) * (1 - fx) + at(m, x + 1, y) * fx) * (1 - fy) + (at(m, x, y + 1) * (1 - fx) + at(m, x + 1, y + 1) * fx) * fy;
            };
            parallelRows(0, h, [&](int a, int b) {
                for (int y = a; y < b; y++) for (int x = 0; x < w; x++) {
                    float v = lerp2(base, x, y);
                    if (contourCurve) v = sampleContour(contourLut, unit(v / contourRange), bevel.contourAntialiased);
                    if (!bump.empty()) {
                        const float coverage = bevel.kind == Bevel::Kind::Inner || bevel.kind == Bevel::Kind::StrokeEmboss ? lerp2(matte, x, y) : 1 - std::abs(unit(v) * 2 - 1);
                        v += lerp2(bump, x, y) * bevel.textureDepth * unit(coverage);
                    }
                    out[size_t(y) * w + x] = v;
                }
            }, 32);
        };
        const float angle = (180 - bevel.angle) * kPi / 180, altitude = std::clamp(bevel.altitude, 0.0f, 90.0f) * kPi / 180;
        const float lx = -std::cos(angle) * std::cos(altitude), ly = -std::sin(angle) * std::cos(altitude), lz = std::sin(altitude);
        const float normalScale = pillowFamily ? 0.5f * std::clamp(bevel.depth, 0.25f, 10.0f) * float(std::max(2L, std::lround(size * 0.5f))) * slopeGain
                                               : 0.5f * std::clamp(bevel.depth, 0.01f, 10.0f) * std::max(1.0f, size) * slopeGain;
        const bool glossLinear = bevel.gloss.linear();
        const auto glossLut = glossLinear ? std::array<uint8_t, 256>{} : bevel.gloss.lut();
        // The signed light on a slope (gx, gy) turned by `sign`: the highlight's share above zero, the shadow's below.
        auto lighting = [&](float gx, float gy, float sign) {
            gx *= sign; gy *= sign;
            const float length = std::sqrt(gx * gx + gy * gy + 1);
            const float raw = gx * lx + gy * ly + lz, surface = raw / std::max(0.0001f, length);
            float l = surface >= lz ? (surface - lz) / std::max(0.01f, 1 - lz) : -((lz - surface) / std::max(0.01f, lz));
            if (pillowFamily && raw < lz) l = -((lz - raw) / std::max(0.01f, lz));
            if (!glossLinear) {
                // Gloss Contour remaps the Lambert light value; the split then runs against the flat face's sin(altitude).
                const float remapped = sampleContour(glossLut, unit(surface), bevel.glossAntialiased);
                l = remapped >= lz ? (remapped - lz) / std::max(0.01f, 1 - lz) : -((lz - remapped) / std::max(0.01f, lz));
                if (pillowFamily && remapped < lz) {
                    const float rr = sampleContour(glossLut, unit(raw), bevel.glossAntialiased);
                    if (rr < lz) l = -((lz - rr) / std::max(0.01f, lz));
                }
            }
            return l;
        };
        // The shading as signed light per pixel. Pillow Emboss lights the outside of the contour one way up and the
        // inside the other (both at an anti-aliased edge, weighted by the matte); the other styles one way.
        const bool pillow = bevel.kind == Bevel::Kind::Pillow;
        const float sign = pillow ? (bevel.up ? -1.0f : 1.0f) : (bevel.up ? 1.0f : -1.0f);
        Mask outer(base.size(), 0), inner(pillow ? base.size() : 0, 0);
        std::vector<uint8_t> flat(base.size(), 0);
        // An anti-aliased Contour or Gloss Contour is shaded at nine points of each pixel, a third of a pixel apart from
        // its corner, and averaged: no centred sampling matched (photoshop-bevel-subs, photoshop-bevel-gloss).
        const bool supersample = (contourCurve && bevel.contourAntialiased) || (!glossLinear && bevel.glossAntialiased);
        const int steps = supersample ? 3 : 1;
        Mask surface;
        for (int sy = 0; sy < steps; sy++) for (int sx = 0; sx < steps; sx++) {
            surfaceAt(float(sx) * float(s) / 3, float(sy) * float(s) / 3, surface);
            const bool first = sx == 0 && sy == 0;
            parallelRows(0, h, [&](int a, int b) {
                auto sample = [&](int x, int y) { return x < 0 || y < 0 || x >= w || y >= h ? 0.0f : surface[size_t(y) * w + x]; };
                for (int y = a; y < b; y++) for (int x = 0; x < w; x++) {
                    const size_t i = size_t(y) * w + x;
                    const float left = sample(x - 1, y), right = sample(x + 1, y), top = sample(x, y - 1), bottom = sample(x, y + 1);
                    const float gx = (left - right) * normalScale, gy = (top - bottom) * normalScale;
                    if (first) flat[i] = left == right && top == bottom;
                    outer[i] += std::clamp(lighting(gx, gy, sign), -1.0f, 1.0f);
                    if (pillow) inner[i] += std::clamp(lighting(gx, gy, -sign), -1.0f, 1.0f);
                }
            }, 32);
        }
        if (steps > 1) {
            const float k = 1.0f / float(steps * steps);
            for (auto& v : outer) v *= k;
            for (auto& v : inner) v *= k;
        }
        // Soften blurs the shading, not the surface ("blurs the results of shading"): a tent of the soften's width, then
        // a [1 2 1] (photoshop-bevel-gloss). A flat face's Gloss Contour wash goes into the blur, and is drawn outside
        // the shape only within the softened reach of the slopes.
        const float soften = bevel.soften * float(s);
        if (soften > 0) {
            auto softenField = [&](Mask& field) {
                if (std::lround(soften) >= 2) tentBlur(field, w, h, soften);
                tentBlur(field, w, h, 2 * float(s));
            };
            softenField(outer);
            if (pillow) softenField(inner);
            if (!glossLinear) {
                Mask reach(base.size());
                for (size_t i = 0; i < reach.size(); i++) reach[i] = flat[i] ? 0.0f : 1.0f;
                softenField(reach);
                for (size_t i = 0; i < reach.size(); i++) if (reach[i] > 1e-6f) flat[i] = 0;
            }
        }
        float hi[4] = {}, sh[4] = {}; rgb(bevel.highlight, hi); rgb(bevel.shadow, sh);
        paintOut([&](int ox, int oy, T* d, size_t i) {
            const float m = unit(matte[i]);
            float effect = 0;
            switch (bevel.kind) {
            case Bevel::Kind::Inner: case Bevel::Kind::StrokeEmboss: effect = m; break;
            case Bevel::Kind::Outer: effect = 1 - m; break;
            default: effect = 1; break;
            }
            effect *= coverAt(ox, oy);
            // A flat face carries the Gloss Contour's constant wash under the shape only: flat ground outside stays clean.
            if (!glossLinear && flat[i]) effect *= m;
            if (effect <= 0) return;
            auto shade = [&](float l, float weight) {
                if (weight <= 0) return;
                if (l > 0) compositeEffect<T, M>(d, hi, unit(l) * weight * bevel.highlightOpacity * master, bevel.highlightMode);
                else if (l < 0) compositeEffect<T, M>(d, sh, unit(-l) * weight * bevel.shadowOpacity * master, bevel.shadowMode);
            };
            if (pillow) {
                shade(outer[i], effect * (1 - m));
                shade(inner[i], effect * m);
            } else shade(outer[i], effect);
        });
    }
}


} // namespace

void drawStyledLayer(const StyledDraw& in, Image& target) {
    if (in.colorMode == ColorMode::Lab) drawStyled<SampleType::U8, ColorMode::Lab>(in, target);
    else drawStyled<SampleType::U8, ColorMode::RGB>(in, target);
}
void drawStyledLayer(const StyledDraw& in, ImageC8& target) {
    if (in.colorMode == ColorMode::CMYK && target.channels() == 5) drawStyled<SampleType::U8, ColorMode::CMYK>(in, target);
}
void drawStyledLayer(const StyledDraw& in, Image16& target) {
    if (in.colorMode == ColorMode::CMYK && target.channels() == 5) drawStyled<SampleType::U16, ColorMode::CMYK>(in, target);
    else if (in.colorMode == ColorMode::Lab) drawStyled<SampleType::U16, ColorMode::Lab>(in, target);
    else drawStyled<SampleType::U16, ColorMode::RGB>(in, target);
}
void drawStyledLayer(const StyledDraw& in, ImageF& target) { drawStyled<SampleType::F32, ColorMode::RGB>(in, target); }

} // namespace compositor
