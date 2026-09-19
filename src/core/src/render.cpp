#include "compositor/render.h"
#include "compositor/adjustments.h"
#include "compositor/blend.h"
#include "compositor/parallel.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <set>

extern "C" {
#include "BrushPixels.h"
}

namespace compositor {

namespace {

// ---- Sampling -------------------------------------------------------------

// Bilinear sample of a premultiplied RGBA image at continuous pixel coordinates, clamped to the edge.
inline void sampleBilinear(const Image& image, double x, double y, float out[4]) {
    int w = image.width(), h = image.height();
    x -= 0.5; y -= 0.5;
    if (x < 0) x = 0; else if (x > w - 1) x = w - 1;
    if (y < 0) y = 0; else if (y > h - 1) y = h - 1;
    int x0 = int(x), y0 = int(y);
    int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    float fx = float(x - x0), fy = float(y - y0);
    const uint8_t* p00 = image.pixel(x0, y0);
    const uint8_t* p10 = image.pixel(x1, y0);
    const uint8_t* p01 = image.pixel(x0, y1);
    const uint8_t* p11 = image.pixel(x1, y1);
    float w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy), w01 = (1 - fx) * fy, w11 = fx * fy;
    for (int c = 0; c < 4; c++) out[c] = p00[c] * w00 + p10[c] * w10 + p01[c] * w01 + p11[c] * w11;
}

inline void sampleNearest(const Image& image, double x, double y, float out[4]) {
    int ix = clamp(int(std::floor(x)), 0, image.width() - 1), iy = clamp(int(std::floor(y)), 0, image.height() - 1);
    const uint8_t* p = image.pixel(ix, iy);
    for (int c = 0; c < 4; c++) out[c] = p[c];
}

inline float sampleGrayBilinear(const GrayImage& image, double x, double y) {
    int w = image.width(), h = image.height();
    if (w == 1 && h == 1) return image.at(0, 0);
    x -= 0.5; y -= 0.5;
    if (x < 0) x = 0; else if (x > w - 1) x = w - 1;
    if (y < 0) y = 0; else if (y > h - 1) y = h - 1;
    int x0 = int(x), y0 = int(y);
    int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    float fx = float(x - x0), fy = float(y - y0);
    return image.at(x0, y0) * (1 - fx) * (1 - fy) + image.at(x1, y0) * fx * (1 - fy) + image.at(x0, y1) * (1 - fx) * fy + image.at(x1, y1) * fx * fy;
}

inline float sampleGrayNearest(const GrayImage& image, double x, double y) {
    int ix = clamp(int(std::floor(x)), 0, image.width() - 1), iy = clamp(int(std::floor(y)), 0, image.height() - 1);
    return image.at(ix, iy);
}

/// The mip level and per-axis scale for drawing `pixels` source pixels across `size` output pixels.
struct MipChoice { int level; double factor; };
MipChoice mipFor(Sampling sampling, double outputWidth, double outputHeight, int pixelWidth, int pixelHeight) {
    if (sampling == Sampling::Nearest) return {0, 1};
    double fx = outputWidth / std::max(1, pixelWidth), fy = outputHeight / std::max(1, pixelHeight);
    int level = MipCache::levelFor(std::min(fx, fy));
    return {level, std::ldexp(1.0, level)};
}

// Per-output-pixel iteration over the region a transform covers.
struct Mapping {
    Affine documentToOutput; // document -> output pixels
    Affine outputToPixel;    // output pixel centers -> layer pixel grid
    Rect outputRect;         // the output pixels to visit (integral, clipped)
};

Mapping mappingFor(const LayerTransform& transform, int pixelWidth, int pixelHeight, const Rect& region, double scale, int outWidth, int outHeight, double margin) {
    Mapping m;
    m.documentToOutput = Affine::translation(-region.x, -region.y).concatenating(Affine::scaling(scale, scale));
    Affine pixelToDocument = transform.pixelToDocument(pixelWidth, pixelHeight);
    Affine pixelToOutput = pixelToDocument.concatenating(m.documentToOutput);
    m.outputToPixel = pixelToOutput.inverted();
    Rect bounds = pixelToOutput.mapBounds(Rect(0, 0, pixelWidth, pixelHeight)).insetBy(-margin, -margin).integral();
    m.outputRect = bounds.intersection(Rect(0, 0, outWidth, outHeight));
    return m;
}

} // namespace

// ---- Mask coverage --------------------------------------------------------

void sampleMaskCoverage(const GrayImage& mask, const LayerTransform& transform, const Rect& region, double scale, uint8_t outside, GrayImage& out, bool multiply) {
    int mw = mask.width(), mh = mask.height();
    if (!multiply) out.fill(outside);
    if (mw <= 0 || mh <= 0) return;
    Mapping m = mappingFor(transform, mw, mh, region, scale, out.width(), out.height(), 1);
    double sx = std::hypot(m.outputToPixel.a, m.outputToPixel.b); // layer px per output px along x
    double sy = std::hypot(m.outputToPixel.c, m.outputToPixel.d);
    MipChoice mip = mipFor(transform.sampling, transform.size.width * scale, transform.size.height * scale, mw, mh);
    std::shared_ptr<const GrayImage> reduced;
    if (mip.level > 0) {
        // Build the reduction directly (masks are small and this path is uncommon).
        std::shared_ptr<GrayImage> r = std::make_shared<GrayImage>(mask);
        for (int i = 0; i < mip.level; i++) r = halveGray(*r);
        reduced = r;
    }
    const GrayImage& src = reduced ? *reduced : mask;
    double factor = mip.factor;
    bool nearest = transform.sampling == Sampling::Nearest;
    // Multiply outside the rect too when multiplying (outside coverage applies there).
    if (multiply && outside != 255) {
        for (int y = 0; y < out.height(); y++) {
            uint8_t* row = out.row(y);
            for (int x = 0; x < out.width(); x++) {
                bool inside = x >= m.outputRect.minX() && x < m.outputRect.maxX() && y >= m.outputRect.minY() && y < m.outputRect.maxY();
                if (!inside) row[x] = uint8_t(row[x] * outside / 255);
            }
        }
    }
    parallelRows(int(m.outputRect.minY()), int(m.outputRect.maxY()), [&](int ya, int yb) {
    for (int y = ya; y < yb; y++) {
        uint8_t* row = out.row(y);
        Point p = m.outputToPixel.apply({m.outputRect.minX() + 0.5, y + 0.5});
        Point dp = m.outputToPixel.applyVector({1, 0});
        for (int x = int(m.outputRect.minX()); x < int(m.outputRect.maxX()); x++, p = p + dp) {
            float value;
            bool inside = p.x >= 0 && p.x < mw && p.y >= 0 && p.y < mh;
            if (nearest) {
                value = inside ? sampleGrayNearest(src, p.x, p.y) : outside;
            } else {
                // Antialiased rectangle edge, then the mask's value, `outside` beyond it.
                double ex = std::min(p.x, mw - p.x) / std::max(1e-9, sx), ey = std::min(p.y, mh - p.y) / std::max(1e-9, sy);
                float edge = float(clamp(std::min(ex, ey) + 0.5, 0.0, 1.0));
                float in = sampleGrayBilinear(src, p.x / factor, p.y / factor);
                value = in * edge + outside * (1 - edge);
            }
            uint8_t v = uint8_t(clamp(value + 0.5f, 0.0f, 255.0f));
            row[x] = multiply ? uint8_t((row[x] * v + 127) / 255) : v;
        }
    }
    });
}

// ---- Drawing a layer -------------------------------------------------------

void drawLayer(const DrawParams& params, const Rect& region, double scale, const GrayImage* coverage, Image& out) {
    if (!params.image || params.image->isEmpty() || out.isEmpty()) return;
    const Image& full = *params.image;
    int pw = full.width(), ph = full.height();
    Mapping m = mappingFor(params.transform, pw, ph, region, scale, out.width(), out.height(), 1);
    if (m.outputRect.isEmpty()) return;
    bool nearest = params.transform.sampling == Sampling::Nearest;
    MipChoice mip = mipFor(params.transform.sampling, params.transform.size.width * scale, params.transform.size.height * scale, pw, ph);
    ImagePtr source = MipCache::shared().level(params.image, mip.level);
    double factor = mip.factor;
    // Layer pixels per output pixel along each output axis, for edge antialiasing.
    double sx = std::hypot(m.outputToPixel.a, m.outputToPixel.b);
    double sy = std::hypot(m.outputToPixel.c, m.outputToPixel.d);

    // The layer's own mask.
    const GrayImage* mask = nullptr;
    GrayPtr maskHold;
    std::optional<LayerTransform> maskPlacement;
    if (params.mask && params.mask->enabled) {
        maskHold = params.maskImage ? params.maskImage : params.mask->asset.image;
        mask = maskHold.get();
        maskPlacement = params.maskPlacement ? params.maskPlacement : params.mask->placement;
        if (maskPlacement && maskPlacement->samePlacement(params.layerTransformForMask)) maskPlacement.reset();
    }
    std::shared_ptr<GrayImage> placedMask;
    if (mask && maskPlacement) {
        // A mask placed apart from its layer: resample it into output space over this layer's area.
        uint8_t background = params.mask->asset.thumbnail ? LayerMask::background(*params.mask->asset.thumbnail) : 255;
        placedMask = std::make_shared<GrayImage>(out.width(), out.height(), background);
        sampleMaskCoverage(*mask, *maskPlacement, region, scale, background, *placedMask, false);
    }
    double maskScaleX = mask ? double(mask->width()) / pw : 1, maskScaleY = mask ? double(mask->height()) / ph : 1;
    float opacity = float(clamp(params.opacity, 0.0, 1.0));

    parallelRows(int(m.outputRect.minY()), int(m.outputRect.maxY()), [&](int ya, int yb) {
    for (int y = ya; y < yb; y++) {
        uint8_t* row = out.row(y);
        const uint8_t* covRow = coverage ? coverage->row(y) : nullptr;
        const uint8_t* placedRow = placedMask ? placedMask->row(y) : nullptr;
        Point p = m.outputToPixel.apply({m.outputRect.minX() + 0.5, y + 0.5});
        Point dp = m.outputToPixel.applyVector({1, 0});
        for (int x = int(m.outputRect.minX()); x < int(m.outputRect.maxX()); x++, p = p + dp) {
            float edge;
            if (nearest) {
                if (p.x < 0 || p.x >= pw || p.y < 0 || p.y >= ph) continue;
                edge = 1;
            } else {
                double ex = std::min(p.x, pw - p.x) / std::max(1e-9, sx), ey = std::min(p.y, ph - p.y) / std::max(1e-9, sy);
                edge = float(clamp(std::min(ex, ey) + 0.5, 0.0, 1.0));
                if (edge <= 0) continue;
            }
            float cov = edge * opacity;
            if (covRow) cov *= covRow[x] / 255.0f;
            if (mask) {
                if (placedRow) cov *= placedRow[x] / 255.0f;
                else cov *= (nearest ? sampleGrayNearest(*mask, p.x * maskScaleX, p.y * maskScaleY) : sampleGrayBilinear(*mask, p.x * maskScaleX, p.y * maskScaleY)) / 255.0f;
            }
            if (cov <= 0.0005f) continue;
            float s[4];
            if (nearest) sampleNearest(*source, p.x, p.y, s);
            else sampleBilinear(*source, p.x / factor, p.y / factor, s);
            uint8_t src[4] = {uint8_t(s[0] + 0.5f), uint8_t(s[1] + 0.5f), uint8_t(s[2] + 0.5f), uint8_t(s[3] + 0.5f)};
            if (!src[3]) continue;
            compositePixel(params.mode, src, cov, row + x * 4);
        }
    }
    });
}

// ---- Resampling into a grid --------------------------------------------------

std::shared_ptr<Image> resampleLayer(const Image& image, const LayerTransform& transform, const LayerTransform& target, int width, int height) {
    auto out = std::make_shared<Image>(width, height);
    if (image.isEmpty() || width <= 0 || height <= 0) return out;
    Affine targetToDoc = target.pixelToDocument(width, height);
    Affine docToPixel = transform.pixelToDocument(image.width(), image.height()).inverted();
    Affine map = targetToDoc.concatenating(docToPixel);
    double sx = std::hypot(map.a, map.b), sy = std::hypot(map.c, map.d);
    int level = transform.sampling == Sampling::Nearest ? 0 : MipCache::levelFor(1.0 / std::max(sx, sy));
    ImagePtr source = MipCache::shared().level(std::make_shared<Image>(image), level);
    double factor = std::ldexp(1.0, level);
    int pw = image.width(), ph = image.height();
    parallelRows(0, height, [&](int ya, int yb) {
    for (int y = ya; y < yb; y++) {
        uint8_t* row = out->row(y);
        Point p = map.apply({0.5, y + 0.5});
        Point dp = map.applyVector({1, 0});
        for (int x = 0; x < width; x++, p = p + dp, row += 4) {
            float s[4];
            if (transform.sampling == Sampling::Nearest) {
                if (p.x < 0 || p.x >= pw || p.y < 0 || p.y >= ph) continue;
                sampleNearest(*source, p.x, p.y, s);
                for (int c = 0; c < 4; c++) row[c] = uint8_t(s[c] + 0.5f);
            } else {
                double ex = std::min(p.x, pw - p.x) / std::max(1e-9, sx), ey = std::min(p.y, ph - p.y) / std::max(1e-9, sy);
                float edge = float(clamp(std::min(ex, ey) + 0.5, 0.0, 1.0));
                if (edge <= 0) continue;
                sampleBilinear(*source, p.x / factor, p.y / factor, s);
                for (int c = 0; c < 4; c++) row[c] = uint8_t(clamp(s[c] * edge + 0.5f, 0.0f, 255.0f));
            }
        }
    }
    });
    return out;
}

std::shared_ptr<GrayImage> resampleMask(const GrayImage& mask, const LayerTransform& transform, const LayerTransform& target, int width, int height, uint8_t outside) {
    auto out = std::make_shared<GrayImage>(width, height, outside);
    if (mask.isEmpty() || width <= 0 || height <= 0) return out;
    Affine map = target.pixelToDocument(width, height).concatenating(transform.pixelToDocument(mask.width(), mask.height()).inverted());
    double sx = std::hypot(map.a, map.b), sy = std::hypot(map.c, map.d);
    int level = MipCache::levelFor(1.0 / std::max(sx, sy));
    std::shared_ptr<GrayImage> reduced = std::make_shared<GrayImage>(mask);
    for (int i = 0; i < level; i++) reduced = halveGray(*reduced);
    double factor = std::ldexp(1.0, level);
    int mw = mask.width(), mh = mask.height();
    parallelRows(0, height, [&](int ya, int yb) {
    for (int y = ya; y < yb; y++) {
        uint8_t* row = out->row(y);
        Point p = map.apply({0.5, y + 0.5});
        Point dp = map.applyVector({1, 0});
        for (int x = 0; x < width; x++, p = p + dp) {
            double ex = std::min(p.x, mw - p.x) / std::max(1e-9, sx), ey = std::min(p.y, mh - p.y) / std::max(1e-9, sy);
            float edge = float(clamp(std::min(ex, ey) + 0.5, 0.0, 1.0));
            float in = sampleGrayBilinear(*reduced, p.x / factor, p.y / factor);
            row[x] = uint8_t(clamp(in * edge + outside * (1 - edge) + 0.5f, 0.0f, 255.0f));
        }
    }
    });
    return out;
}

// ---- Document rendering ------------------------------------------------------

namespace {

struct Renderer {
    const Document& document;
    const Overrides* overrides;
    Rect region;
    double scale;
    int outWidth, outHeight;
    Renderer(const Document& d, const Overrides* o, Rect r, double s, int w, int h)
        : document(d), overrides(o), region(r), scale(s), outWidth(w), outHeight(h) {}
    std::map<Uuid, const Layer*> byId;
    std::vector<const Layer*> order;         // visible non-group layers, bottom to top
    std::map<Uuid, std::vector<Uuid>> stacks; // clipping base -> children
    std::set<Uuid> stacked;
    std::map<Uuid, std::shared_ptr<GrayImage>> liveCoverage;
    std::set<Uuid> visiting;
    std::map<Uuid, std::shared_ptr<GrayImage>> folderCoverage; // per group id, that group's own mask coverage
    std::map<std::optional<Uuid>, std::shared_ptr<GrayImage>> chainCoverage; // per parent, all enclosing folders combined

    const LayerOverride* over(const Uuid& id) const {
        if (!overrides) return nullptr;
        auto it = overrides->find(id);
        return it == overrides->end() ? nullptr : &it->second;
    }
    LayerTransform transformOf(const Layer& l) const {
        auto* o = over(l.id);
        return o && o->transform ? *o->transform : l.transform;
    }
    ImagePtr imageOf(const Layer& l) const {
        auto* o = over(l.id);
        if (o && o->image) return *o->image;
        return l.asset ? l.asset->image : nullptr;
    }
    BlendMode blendOf(const Layer& l) const {
        auto* o = over(l.id);
        return o && o->blendMode ? *o->blendMode : l.blendMode;
    }
    std::optional<Uuid> sourceOf(const Uuid& id) const {
        auto it = byId.find(id);
        return it == byId.end() ? std::nullopt : it->second->maskSourceId;
    }

    std::shared_ptr<GrayImage> folderMask(const Layer& group) {
        auto it = folderCoverage.find(group.id);
        if (it != folderCoverage.end()) return it->second;
        std::shared_ptr<GrayImage> result;
        if (group.mask && group.mask->enabled && group.mask->asset.image) {
            result = std::make_shared<GrayImage>(outWidth, outHeight, 0);
            sampleMaskCoverage(*group.mask->asset.image, transformOf(group), region, scale, 0, *result, false);
        }
        folderCoverage[group.id] = result;
        return result;
    }

    /// Coverage from every folder enclosing a layer whose parent is `parent`, or null when none has a mask.
    std::shared_ptr<GrayImage> foldersCoverage(const std::optional<Uuid>& parent) {
        auto it = chainCoverage.find(parent);
        if (it != chainCoverage.end()) return it->second;
        std::shared_ptr<GrayImage> result;
        std::optional<Uuid> current = parent;
        int depth = 0;
        while (current && depth < 64) {
            auto lit = byId.find(*current);
            if (lit == byId.end()) break;
            auto mask = folderMask(*lit->second);
            if (mask) {
                if (!result) result = std::make_shared<GrayImage>(*mask);
                else for (size_t i = 0; i < result->byteCount(); i++) result->data()[i] = uint8_t((result->data()[i] * mask->data()[i] + 127) / 255);
            }
            current = lit->second->parentId;
            depth++;
        }
        chainCoverage[parent] = result;
        return result;
    }

    static std::shared_ptr<GrayImage> multiply(const std::shared_ptr<GrayImage>& a, const std::shared_ptr<GrayImage>& b) {
        if (!a) return b;
        if (!b) return a;
        auto r = std::make_shared<GrayImage>(*a);
        for (size_t i = 0; i < r->byteCount(); i++) r->data()[i] = uint8_t((r->data()[i] * b->data()[i] + 127) / 255);
        return r;
    }

    void drawOwn(const Layer& layer, Image& target, const GrayImage* coverage) {
        ImagePtr image = imageOf(layer);
        if (!image) return;
        DrawParams params;
        params.image = image;
        params.transform = transformOf(layer);
        params.opacity = layer.opacity;
        params.mode = blendOf(layer);
        params.layerTransformForMask = params.transform;
        if (layer.mask) {
            params.mask = &*layer.mask;
            auto* o = over(layer.id);
            if (o && o->maskPlacement) params.maskPlacement = *o->maskPlacement;
            if (o && o->maskImage) params.maskImage = *o->maskImage;
        }
        drawLayer(params, region, scale, coverage, target);
    }

    /// Alpha of `id` drawn with its own mask and upstream clipping, ignoring visibility and folder masks.
    std::shared_ptr<GrayImage> coverageOf(const Uuid& id) {
        auto it = liveCoverage.find(id);
        if (it != liveCoverage.end()) return it->second;
        if (visiting.count(id) || visiting.size() >= 256) return nullptr;
        auto lit = byId.find(id);
        if (lit == byId.end()) return nullptr;
        visiting.insert(id);
        Image pixels(outWidth, outHeight);
        drawClipped(*lit->second, pixels, nullptr);
        visiting.erase(id);
        auto gray = std::make_shared<GrayImage>(outWidth, outHeight);
        layer_extract_alpha(pixels.data(), size_t(pixels.stride()), gray->data(), size_t(gray->stride()), size_t(outWidth), size_t(outHeight));
        liveCoverage[id] = gray;
        return gray;
    }

    /// LiveMaskRenderer.draw: the layer through its clipping source's coverage.
    void drawClipped(const Layer& layer, Image& target, std::shared_ptr<GrayImage> coverage) {
        if (layer.maskSourceId) {
            auto source = coverageOf(*layer.maskSourceId);
            if (!source) return;
            coverage = multiply(coverage, source);
        }
        drawOwn(layer, target, coverage.get());
    }

    void adjust(const Layer& layer, Image& target, std::shared_ptr<GrayImage> coverage) {
        if (!layer.adjustment) return;
        Image adjusted = target;
        if (!applyAdjustment(*layer.adjustment, adjusted, region, scale)) return;
        BlendMode mode = blendOf(layer);
        float opacity = float(clamp(layer.opacity, 0.0, 1.0));
        // Clip: the layer's own mask over its transform (and folder masks in `coverage`).
        std::shared_ptr<GrayImage> clip = coverage;
        if (layer.mask && layer.mask->enabled && layer.mask->asset.image) {
            auto own = std::make_shared<GrayImage>(outWidth, outHeight, 0);
            sampleMaskCoverage(*layer.mask->asset.image, transformOf(layer), region, scale, 0, *own, false);
            clip = multiply(clip, own);
        }
        parallelRows(0, outHeight, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            uint8_t* d = target.row(y);
            const uint8_t* a = adjusted.row(y);
            const uint8_t* c = clip ? clip->row(y) : nullptr;
            for (int x = 0; x < outWidth; x++, d += 4, a += 4) {
                float mix = opacity * (c ? c[x] / 255.0f : 1.0f);
                if (mix <= 0 || d[3] == 0) continue;
                float ab = d[3] / 255.0f;
                float cb[3], cs[3];
                for (int k = 0; k < 3; k++) { cb[k] = d[k] / 255.0f / ab; cs[k] = a[k] / 255.0f / ab; }
                if (mode != BlendMode::Normal) {
                    Rgb m = blendColor(mode, {cb[0], cb[1], cb[2]}, {cs[0], cs[1], cs[2]});
                    cs[0] = m.r; cs[1] = m.g; cs[2] = m.b;
                }
                for (int k = 0; k < 3; k++) {
                    float v = (cb[k] + (cs[k] - cb[k]) * mix) * ab;
                    d[k] = uint8_t(clamp(v * 255.0f + 0.5f, 0.0f, float(d[3])));
                }
            }
        }
        });
    }

    void prepareStacks() {
        for (size_t index = 0; index < order.size(); index++) {
            const Layer* base = order[index];
            if (base->maskSourceId || base->adjustment) continue;
            std::vector<Uuid> children;
            for (size_t j = index + 1; j < order.size(); j++) {
                const Layer* child = order[j];
                if (!(child->maskSourceId && *child->maskSourceId == base->id && child->parentId == base->parentId)) break;
                children.push_back(child->id);
            }
            if (children.empty()) continue;
            stacks[base->id] = children;
            for (auto& c : children) stacked.insert(c);
        }
    }

    void drawComposite(const Layer& layer, Image& out) {
        if (stacked.count(layer.id)) return;
        std::shared_ptr<GrayImage> folders = foldersCoverage(layer.parentId);
        if (layer.adjustment) {
            if (!layer.maskSourceId) adjust(layer, out, folders);
            return;
        }
        auto stack = stacks.find(layer.id);
        if (stack == stacks.end()) { drawClipped(layer, out, folders); return; }
        // A clipping stack: the base's alpha is shared by the layers clipped to it.
        Image group(outWidth, outHeight);
        drawOwn(layer, group, nullptr);
        std::vector<uint8_t> alpha(size_t(outWidth) * outHeight);
        layer_extract_alpha(group.data(), size_t(group.stride()), alpha.data(), size_t(outWidth), size_t(outWidth), size_t(outHeight));
        layer_unpremultiply_opaque(group.data(), size_t(group.stride()), size_t(outWidth), size_t(outHeight));
        for (auto& childId : stack->second) {
            auto it = byId.find(childId);
            if (it == byId.end()) continue;
            if (it->second->adjustment) adjust(*it->second, group, nullptr);
            else drawOwn(*it->second, group, nullptr);
        }
        layer_restore_alpha(group.data(), size_t(group.stride()), alpha.data(), size_t(outWidth), size_t(outWidth), size_t(outHeight));
        BlendMode mode = blendOf(layer);
        parallelRows(0, outHeight, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            const uint8_t* s = group.row(y);
            uint8_t* d = out.row(y);
            const uint8_t* c = folders ? folders->row(y) : nullptr;
            for (int x = 0; x < outWidth; x++, s += 4, d += 4) {
                float cov = c ? c[x] / 255.0f : 1.0f;
                if (cov > 0) compositePixel(mode, s, cov, d);
            }
        }
        });
    }

    void run(Image& out) {
        for (auto& l : document.layers) byId[l.id] = &l;
        order = renderLayers(document.layers);
        prepareStacks();
        for (const Layer* layer : order) drawComposite(*layer, out);
    }
};

} // namespace

void render(const Document& document, const RenderOptions& options, Image& out, const Overrides* overrides) {
    Rect region = options.region.isEmpty() ? document.rect() : options.region;
    double scale = options.scale > 0 ? options.scale : 1;
    int w = std::max(1, int(std::ceil(region.width * scale - 1e-9))), h = std::max(1, int(std::ceil(region.height * scale - 1e-9)));
    if (out.width() != w || out.height() != h) out = Image(w, h);
    else if (options.clear) out.clear();
    Renderer renderer(document, overrides, region, scale, w, h);
    renderer.run(out);
}

std::shared_ptr<Image> renderFlattened(const Document& document) {
    auto out = std::make_shared<Image>(document.width, document.height);
    RenderOptions options;
    render(document, options, *out);
    return out;
}

} // namespace compositor
