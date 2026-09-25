// Drawing layer styles around a layer's pixels. The float-mask machinery (the tent blur, spread and choke as
// grayscale dilation, exact Euclidean distance fields, the stroke band, the bevel height field) and the
// compositing rules (burn/dodge folding, exterior knockout, interior order, Blend Interior Effects as Group)
// are ported from Patchy (MIT; src/render/layer_style_mask_ops.cpp and render/layer_compositor.hpp there),
// which calibrated them against Photoshop 2026 COM renders; the comments there carry the probe details.
#include "layerstyle_render.h"
#include "compositor/blend.h"
#include "compositor/parallel.h"
#include <algorithm>
#include <cmath>
#include <cstring>

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
    if (bevel.soften > 0) {
        const int support = int(std::ceil(bevel.soften)), passes = std::min(3, support);
        for (int p = 0; p < passes; p++) boxBlur(height, w, h, support / passes + (p < support % passes ? 1 : 0), 1);
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

/// An effect colour onto a premultiplied pixel. The burn modes fold the effect's alpha into the colour toward
/// white, Color Dodge toward black, then blend fully (Photoshop's effect compositing); the rest lerp.
void compositeEffect(uint8_t* d, const float color[3], float alpha, EffectBlend mode) {
    if (alpha <= 0) return;
    alpha = unit(alpha);
    if (mode == EffectBlend::Dissolve) mode = EffectBlend::Normal;
    const float da = d[3] / 255.0f;
    float b[3] = {0, 0, 0};
    if (da > 0) for (int k = 0; k < 3; k++) b[k] = d[k] / 255.0f / da;
    float c[3] = {color[0], color[1], color[2]};
    float a = alpha;
    if (da >= 0.999f && (mode == EffectBlend::LinearBurn || mode == EffectBlend::ColorBurn)) { for (auto& v : c) v = 1 - (1 - v) * alpha; a = 1; }
    else if (da >= 0.999f && mode == EffectBlend::ColorDodge) { for (auto& v : c) v *= alpha; a = 1; }
    float blended[3];
    effectBlend(mode, b, c, blended);
    const float outA = a + da * (1 - a);
    for (int k = 0; k < 3; k++) {
        const float premul = a * (1 - da) * c[k] + a * da * blended[k] + (1 - a) * da * b[k];
        d[k] = uint8_t(std::clamp(premul * 255 + 0.5f, 0.0f, 255.0f));
    }
    d[3] = uint8_t(std::clamp(outA * 255 + 0.5f, 0.0f, 255.0f));
    for (int k = 0; k < 3; k++) d[k] = std::min(d[k], d[3]);
}

/// The same on a straight colour over an opaque backdrop (the interior folds).
void foldEffect(float rgb[3], const float color[3], float alpha, EffectBlend mode) {
    if (alpha <= 0) return;
    alpha = unit(alpha);
    if (mode == EffectBlend::Dissolve) mode = EffectBlend::Normal;
    float c[3] = {color[0], color[1], color[2]};
    float a = alpha;
    if (mode == EffectBlend::LinearBurn || mode == EffectBlend::ColorBurn) { for (auto& v : c) v = 1 - (1 - v) * alpha; a = 1; }
    else if (mode == EffectBlend::ColorDodge) { for (auto& v : c) v *= alpha; a = 1; }
    float blended[3];
    effectBlend(mode, rgb, c, blended);
    for (int k = 0; k < 3; k++) rgb[k] = rgb[k] + (blended[k] - rgb[k]) * a;
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
    default: return EffectBlend::Normal;
    }
}

struct Pattern {
    const PatternTile* tile = nullptr;
    double anchorX = 0, anchorY = 0, inverseScale = 1, cosine = 1, sine = 0;
    bool nearest = true, box = false;
    void sample(double x, double y, float rgb[3], float& alpha) const {
        auto texel = [&](long tx, long ty) {
            tx %= tile->width; if (tx < 0) tx += tile->width;
            ty %= tile->height; if (ty < 0) ty += tile->height;
            return tile->rgba.data() + (size_t(ty) * size_t(tile->width) + size_t(tx)) * 4;
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
                    const uint8_t* p = texel(tx, ty);
                    for (int k = 0; k < 4; k++) sum[k] += cx * cy * p[k];
                    total += cx * cy;
                }
            }
            for (int k = 0; k < 3; k++) rgb[k] = total > 0 ? float(sum[k] / total / 255) : 0;
            alpha = total > 0 ? float(sum[3] / total / 255) : 0;
            return;
        }
        if (nearest) {
            const uint8_t* p = texel(long(std::floor(x + 0.5 - anchorX)), long(std::floor(y + 0.5 - anchorY)));
            for (int k = 0; k < 3; k++) rgb[k] = p[k] / 255.0f;
            alpha = p[3] / 255.0f;
            return;
        }
        double u = x - anchorX, v = y - anchorY;
        const double ru = u * cosine - v * sine, rv = u * sine + v * cosine;
        u = ru * inverseScale; v = rv * inverseScale;
        const double fu = std::floor(u), fv = std::floor(v);
        const float tx = float(u - fu), ty = float(v - fv);
        const uint8_t *a = texel(long(fu), long(fv)), *b = texel(long(fu) + 1, long(fv)), *c = texel(long(fu), long(fv) + 1), *d = texel(long(fu) + 1, long(fv) + 1);
        auto mix = [&](int k) { return ((a[k] * (1 - tx) + b[k] * tx) * (1 - ty) + (c[k] * (1 - tx) + d[k] * tx) * ty) / 255.0f; };
        for (int k = 0; k < 3; k++) rgb[k] = mix(k);
        alpha = mix(3);
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

} // namespace

void drawStyledLayer(const StyledDraw& in, Image& target) {
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
    Image source(w, h);
    in.drawSource(source, padded);
    Mask alpha(size_t(w) * h);
    bool any = false;
    for (int y = 0; y < h; y++) { const uint8_t* p = source.row(y); for (int x = 0; x < w; x++) { alpha[size_t(y) * w + x] = p[x * 4 + 3] / 255.0f; any |= p[x * 4 + 3] != 0; } }
    if (!any) return;
    const float master = in.master, fill = in.fill;
    const GrayImage* cover = in.coverage;
    auto coverAt = [&](int ox, int oy) { return cover ? cover->row(oy)[ox] / 255.0f : 1.0f; };
    auto rgb = [](StyleColor c, float out[3]) { out[0] = c.r / 255.0f; out[1] = c.g / 255.0f; out[2] = c.b / 255.0f; };
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
    auto paintOut = [&](const std::function<void(int ox, int oy, uint8_t* d, size_t i)>& f) {
        parallelRows(oy0, oy1, [&](int a, int b) {
            for (int oy = a; oy < b; oy++) {
                uint8_t* row = target.row(oy);
                for (int ox = ox0; ox < ox1; ox++) f(ox, oy, row + ox * 4, size_t(oy - wy0) * w + size_t(ox - wx0));
            }
        }, 32);
    };

    // What the exterior effects paint under: the layer's own blend reads the backdrop from before them.
    std::optional<Image> backdrop;
    const bool exterior = !style.dropShadows.empty() || !style.outerGlows.empty();
    if (exterior && in.mode != BlendMode::Normal) backdrop = target;

    const bool drawExterior = in.phase != StyledDraw::Phase::Interior, drawInterior = in.phase != StyledDraw::Phase::Exterior;
    const bool ownContent = in.phase == StyledDraw::Phase::Whole;

    // Exterior: drop shadows, then outer glows.
    if (drawExterior) for (const DropShadow& shadow : style.dropShadows) {
        if (shadow.opacity <= 0) continue;
        int dx, dy;
        offset(shadow.angle, shadow.distance, dx, dy);
        Mask m = shifted(dx, dy);
        softExterior(m, w, h, shadow.size * float(s), shadow.spread);
        float color[3]; rgb(shadow.color, color);
        paintOut([&](int ox, int oy, uint8_t* d, size_t i) {
            float a = m[i] * shadow.opacity * master * coverAt(ox, oy);
            if (shadow.layerConceals) a *= exteriorKnockout(alpha[i], alpha[i] * fill * master);
            compositeEffect(d, color, a, shadow.mode);
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
        float color[3]; rgb(glow.color, color);
        paintOut([&](int ox, int oy, uint8_t* d, size_t i) {
            compositeEffect(d, color, m[i] * exteriorKnockout(alpha[i], alpha[i] * fill * master) * glow.opacity * master * coverAt(ox, oy), glow.mode);
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
    const bool foldInteriors = fill >= 0.999f && ownContent;
    auto foldInto = [&](float c[3], int ox, int oy, size_t i) {
        const double x = docX(ox), y = docY(oy);
        for (const PatternOverlay& p : style.patternOverlays) {
            const PatternTile* tile = findPattern(p.patternId);
            if (!tile || p.opacity <= 0) continue;
            float pc[3], pa;
            patternFor(tile, style, p.scale, p.angle, p.linkWithLayer, p.phaseX, p.phaseY).sample(x, y, pc, pa);
            foldEffect(c, pc, pa * p.opacity, p.mode);
        }
        for (const GradientOverlay& g : style.gradientOverlays) {
            if (g.opacity <= 0) continue;
            double gx, gy, gw, gh;
            gradientBounds(g.gradient, gx, gy, gw, gh);
            const float t = gradientPosition(g.gradient, gx, gy, gw, gh, x, y);
            float gc[3]; rgb(gradientColor(g.gradient, t), gc);
            foldEffect(c, gc, g.opacity * gradientOpacity(g.gradient, t), g.mode);
        }
        for (const ColorOverlay& o : style.colorOverlays) { float oc[3]; rgb(o.color, oc); foldEffect(c, oc, o.opacity, o.mode); }
        for (auto& sm : satins) {
            float sc[3]; rgb(sm.satin->color, sc);
            // Satin keeps the plain model (Patchy's pinned fold), even for the burn and dodge modes.
            const float a = sm.mask[i] * sm.satin->opacity;
            if (a <= 0) continue;
            float blended[3];
            effectBlend(sm.satin->mode == EffectBlend::Dissolve ? EffectBlend::Normal : sm.satin->mode, c, sc, blended);
            for (int k = 0; k < 3; k++) c[k] += (blended[k] - c[k]) * a;
        }
    };

    // The layer itself: with Blend Interior Effects as Group the interiors fold into its colour and its mode
    // carries them; without (Photoshop's default) its mode blends its own pixels and the interiors land on that.
    const bool interiorsFirst = style.blendInteriorAsGroup;
    const EffectBlend layerMode = asEffect(in.mode);
    if (ownContent) paintOut([&](int ox, int oy, uint8_t* d, size_t i) {
        const uint8_t* p = source.row(oy - wy0) + (ox - wx0) * 4;
        if (p[3] == 0) return;
        const float a = p[3] / 255.0f;
        float c[3];
        for (int k = 0; k < 3; k++) c[k] = std::min(1.0f, p[k] / 255.0f / a);
        const float paint = a * master * fill * knock(i) * coverAt(ox, oy);
        if (paint <= 0) return;
        if (foldInteriors && interiorsFirst) foldInto(c, ox, oy, i);
        if (in.mode != BlendMode::Normal && foldInteriors && !interiorsFirst) {
            // Blend against the backdrop first, then the interiors over that, then a plain source-over.
            const uint8_t* bd = backdrop ? backdrop->row(oy) + ox * 4 : d;
            const float ba = bd[3] / 255.0f;
            if (ba > 0) {
                float b[3], blended[3];
                for (int k = 0; k < 3; k++) b[k] = bd[k] / 255.0f / ba;
                effectBlend(layerMode, b, c, blended);
                for (int k = 0; k < 3; k++) c[k] = c[k] + (blended[k] - c[k]) * ba;
            }
            foldInto(c, ox, oy, i);
            compositeEffect(d, c, paint, EffectBlend::Normal);
            return;
        }
        if (foldInteriors && !interiorsFirst) foldInto(c, ox, oy, i);
        if (backdrop && in.mode != BlendMode::Normal) {
            // Exterior effects under a blended layer: blend against the backdrop from before them.
            const uint8_t* bd = backdrop->row(oy) + ox * 4;
            const float ba = bd[3] / 255.0f;
            if (ba > 0) {
                float b[3], blended[3];
                for (int k = 0; k < 3; k++) b[k] = bd[k] / 255.0f / ba;
                effectBlend(layerMode, b, c, blended);
                for (int k = 0; k < 3; k++) c[k] = c[k] + (blended[k] - c[k]) * ba;
            }
            compositeEffect(d, c, paint, EffectBlend::Normal);
            return;
        }
        compositeEffect(d, c, paint, layerMode);
    });

    // With Fill below 100% the overlays and satins are their own passes (Fill fades the pixels, not the effects).
    if (!foldInteriors) {
        paintOut([&](int ox, int oy, uint8_t* d, size_t i) {
            const float shape = alpha[i] * master * knock(i) * coverAt(ox, oy);
            if (shape <= 0) return;
            const double x = docX(ox), y = docY(oy);
            for (const PatternOverlay& p : style.patternOverlays) {
                const PatternTile* tile = findPattern(p.patternId);
                if (!tile || p.opacity <= 0) continue;
                float pc[3], pa;
                patternFor(tile, style, p.scale, p.angle, p.linkWithLayer, p.phaseX, p.phaseY).sample(x, y, pc, pa);
                compositeEffect(d, pc, shape * pa * p.opacity, p.mode);
            }
            for (const GradientOverlay& g : style.gradientOverlays) {
                double gx, gy, gw, gh;
                gradientBounds(g.gradient, gx, gy, gw, gh);
                const float t = gradientPosition(g.gradient, gx, gy, gw, gh, x, y);
                float gc[3]; rgb(gradientColor(g.gradient, t), gc);
                compositeEffect(d, gc, shape * g.opacity * gradientOpacity(g.gradient, t), g.mode);
            }
            for (const ColorOverlay& o : style.colorOverlays) { float oc[3]; rgb(o.color, oc); compositeEffect(d, oc, shape * o.opacity, o.mode); }
            for (auto& sm : satins) { float sc[3]; rgb(sm.satin->color, sc); compositeEffect(d, sc, shape * sm.mask[i] * sm.satin->opacity, sm.satin->mode); }
        });
    }

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
        float color[3]; rgb(glow.color, color);
        paintOut([&](int ox, int oy, uint8_t* d, size_t i) {
            if (alpha[i] <= 0) return;
            compositeEffect(d, color, alpha[i] * m[i] * glow.opacity * master * knock(i) * coverAt(ox, oy), glow.mode);
        });
    }
    for (const InnerShadow& shadow : style.innerShadows) {
        if (shadow.opacity <= 0 || shadow.size <= 0) continue;
        int dx, dy;
        offset(shadow.angle, shadow.distance, dx, dy);
        Mask m = shifted(dx, dy);
        softInterior(m, w, h, shadow.size * float(s), shadow.choke);
        float color[3]; rgb(shadow.color, color);
        paintOut([&](int ox, int oy, uint8_t* d, size_t i) {
            if (alpha[i] <= 0) return;
            compositeEffect(d, color, alpha[i] * m[i] * shadow.opacity * master * knock(i) * coverAt(ox, oy), shadow.mode);
        });
    }

    // Strokes.
    for (size_t k = 0; k < style.strokes.size(); k++) {
        const Stroke& stroke = style.strokes[k];
        if (stroke.opacity <= 0) continue;
        const Mask& band = bands[k];
        float color[3]; rgb(stroke.color, color);
        paintOut([&](int ox, int oy, uint8_t* d, size_t i) {
            float a = band[i] * stroke.opacity * master * coverAt(ox, oy);
            if (a <= 0) return;
            float c[3] = {color[0], color[1], color[2]};
            if (stroke.gradientFill && stroke.gradient.type == StyleGradient::Type::ShapeBurst) {
                // Photoshop supersamples the ramp over the pixel: a 1-2-1 tent of the colours around it.
                const float step = 1.0f / std::max(1.0f, stroke.size * float(s) + 2);
                float t = burstPositions[k][i];
                if (stroke.gradient.reverse) t = 1 - t;
                float acc[3] = {0, 0, 0};
                for (int j = -1; j <= 1; j++) {
                    float cc[3]; rgb(gradientColor(stroke.gradient, unit(t + j * step)), cc);
                    const float wgt = j == 0 ? 0.5f : 0.25f;
                    for (int q = 0; q < 3; q++) acc[q] += cc[q] * wgt;
                }
                for (int q = 0; q < 3; q++) c[q] = acc[q];
                a *= gradientOpacity(stroke.gradient, t);
            } else if (stroke.gradientFill) {
                double gx, gy, gw, gh;
                gradientBounds(stroke.gradient, gx, gy, gw, gh);
                const float t = gradientPosition(stroke.gradient, gx, gy, gw, gh, docX(ox), docY(oy));
                rgb(gradientColor(stroke.gradient, t), c);
                a *= gradientOpacity(stroke.gradient, t);
            }
            compositeEffect(d, c, a, stroke.mode);
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
        Mask height = bevelHeight(matte, w, h, bevel, pillowFamily ? size * 0.5f : size);
        float slopeGain = 1;
        if (bevel.useContour && bevel.contour.linear()) slopeGain = 1 / std::clamp(bevel.contourRange, 0.01f, 1.0f);
        else if (bevel.useContour) {
            const auto lut = bevel.contour.lut();
            const float range = std::clamp(bevel.contourRange, 0.01f, 1.0f);
            for (auto& v : height) v = sampleContour(lut, unit(v / range), bevel.contourAntialiased);
        }
        if (bevel.useTexture) {
            if (const PatternTile* tile = findPattern(bevel.texturePattern)) {
                const Pattern p = patternFor(tile, style, bevel.textureScale, 0, bevel.textureLinkWithLayer, bevel.texturePhaseX, bevel.texturePhaseY);
                Mask bump(height.size(), 0);
                for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                    float c[3], a;
                    p.sample(std::floor(padded.x + (x + 0.5) / s), std::floor(padded.y + (y + 0.5) / s), c, a);
                    const float lum = (299 * c[0] + 590 * c[1] + 111 * c[2]) / 1000;
                    bump[size_t(y) * w + x] = bevel.textureInvert ? lum - 0.5f : 0.5f - lum;
                }
                boxBlur(bump, w, h, 1, 1);
                for (size_t i = 0; i < height.size(); i++) {
                    const float coverage = bevel.kind == Bevel::Kind::Inner || bevel.kind == Bevel::Kind::StrokeEmboss ? matte[i] : 1 - std::abs(unit(height[i]) * 2 - 1);
                    height[i] += bump[i] * bevel.textureDepth * unit(coverage);
                }
            }
        }
        const float angle = (180 - bevel.angle) * kPi / 180, altitude = std::clamp(bevel.altitude, 0.0f, 90.0f) * kPi / 180;
        const float lx = -std::cos(angle) * std::cos(altitude), ly = -std::sin(angle) * std::cos(altitude), lz = std::sin(altitude);
        const float normalScale = pillowFamily ? 0.5f * std::clamp(bevel.depth, 0.25f, 10.0f) * float(std::max(2L, std::lround(size * 0.5f))) * slopeGain
                                               : 0.5f * std::clamp(bevel.depth, 0.01f, 10.0f) * std::max(1.0f, size) * slopeGain;
        const bool glossLinear = bevel.gloss.linear();
        const auto glossLut = glossLinear ? std::array<uint8_t, 256>{} : bevel.gloss.lut();
        float hi[3], sh[3]; rgb(bevel.highlight, hi); rgb(bevel.shadow, sh);
        auto sample = [&](int x, int y) { return x < 0 || y < 0 || x >= w || y >= h ? 0.0f : height[size_t(y) * w + x]; };
        paintOut([&](int ox, int oy, uint8_t* d, size_t i) {
            const float m = unit(matte[i]);
            float effect = 0;
            switch (bevel.kind) {
            case Bevel::Kind::Inner: case Bevel::Kind::StrokeEmboss: effect = m; break;
            case Bevel::Kind::Outer: effect = 1 - m; break;
            default: effect = 1; break;
            }
            effect *= coverAt(ox, oy);
            if (effect <= 0) return;
            const int x = ox - wx0, y = oy - wy0;
            const float left = sample(x - 1, y), right = sample(x + 1, y), top = sample(x, y - 1), bottom = sample(x, y + 1);
            const float gx0 = (left - right) * normalScale, gy0 = (top - bottom) * normalScale;
            const bool flat = left == right && top == bottom;
            auto shade = [&](float sign, float weight) {
                if (weight <= 0) return;
                const float gx = gx0 * sign, gy = gy0 * sign;
                const float length = std::sqrt(gx * gx + gy * gy + 1);
                const float raw = gx * lx + gy * ly + lz, surface = raw / std::max(0.0001f, length);
                float lighting = surface >= lz ? (surface - lz) / std::max(0.01f, 1 - lz) : -((lz - surface) / std::max(0.01f, lz));
                if (pillowFamily && raw < lz) lighting = -((lz - raw) / std::max(0.01f, lz));
                if (!glossLinear) {
                    if (flat) { weight *= m; if (weight <= 0) return; }
                    const float remapped = sampleContour(glossLut, unit(surface), bevel.glossAntialiased);
                    lighting = remapped >= lz ? (remapped - lz) / std::max(0.01f, 1 - lz) : -((lz - remapped) / std::max(0.01f, lz));
                    if (pillowFamily && remapped < lz) {
                        const float rr = sampleContour(glossLut, unit(raw), bevel.glossAntialiased);
                        if (rr < lz) lighting = -((lz - rr) / std::max(0.01f, lz));
                    }
                }
                if (lighting > 0) compositeEffect(d, hi, unit(lighting) * weight * bevel.highlightOpacity * master, bevel.highlightMode);
                else if (lighting < 0) compositeEffect(d, sh, unit(-lighting) * weight * bevel.shadowOpacity * master, bevel.shadowMode);
            };
            if (bevel.kind == Bevel::Kind::Pillow) {
                const float base = bevel.up ? -1.0f : 1.0f;
                shade(base, effect * (1 - m));
                shade(-base, effect * m);
            } else shade(bevel.up ? 1.0f : -1.0f, effect);
        });
    }
}

} // namespace compositor
