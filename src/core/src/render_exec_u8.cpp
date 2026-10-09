// The renderer's pixels at 8 bits (render_plan.h): the former body of render.cpp's Renderer, moved as it was.
// It draws a RenderPlan into premultiplied RGBA8.
#include <cstdlib>
#include <cctype>
#include "compositor/png.h"
#include "render_plan.h"
#include "render_resume.h"
#include "compositor/fill_cache.h"
#include "compositor/layerstyle.h"
#include "layerstyle_render.h"
#include "compositor/vectorlayer.h"
#include "compositor/vectormask.h"
#include "compositor/adjustments.h"
#include "compositor/blend.h"
#include "compositor/parallel.h"
#include "compositor/workcounters.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <mutex>
#include <tuple>
#include <map>
#include <optional>
#include <set>
#include <utility>

extern "C" {
#include "BrushPixels.h"
}

namespace compositor {

template <>
struct RenderExec<SampleType::U8> {
    const RenderPlan& plan;
    // The plan's parts, under the names the drawing code has always used.
    const Document& document;
    const Overrides* overrides;
    const std::map<Uuid, const Layer*>& byId;
    const std::vector<const Layer*>& order;
    const std::map<Uuid, std::vector<Uuid>>& stacks;
    const std::set<Uuid>& stacked;
    const std::map<size_t, std::vector<const Layer*>>& groupsOpen;
    const std::map<size_t, std::vector<const Layer*>>& groupsClose;
    Rect region;
    double scale;
    int outWidth, outHeight;
    RenderExec(const RenderPlan& p, Rect r, double s, int w, int h)
        : plan(p), document(p.document), overrides(p.overrides), byId(p.byId), order(p.order), stacks(p.stacks), stacked(p.stacked),
          groupsOpen(p.groupsOpen), groupsClose(p.groupsClose), region(r), scale(s), outWidth(w), outHeight(h) {}
    std::map<Uuid, std::shared_ptr<GrayImage>> liveCoverage;
    std::set<Uuid> visiting;
    bool plainOnly = false;   // while taking a clipping base's transparency
    /// A Blend If layer's gate, for its styled draw to run between its pixels and its interior effects.
    std::function<void()>* interiorGate = nullptr;
    bool ungated = false;     // drawing a Blend If layer's own pass (blendif.h)

    /// Blend If: `target` holds the layer drawn over `before`; each pixel keeps the share the gates allow, This
    /// Layer read from `source` (the layer's own colours) and Underlying Layer from `before`. Where the layer has no
    /// pixels (its exterior effects) nothing is gated, as in Photoshop.
    void applyGate(const BlendIfGate& gate, const Image& source, const Image& before, Image& target) {
        parallelRows(0, outHeight, [&](int ya, int yb) {
            for (int y = ya; y < yb; y++) {
                const uint8_t* s = source.row(y);
                const uint8_t* b = before.row(y);
                uint8_t* d = target.row(y);
                for (int x = 0; x < outWidth; x++, s += 4, b += 4, d += 4) {
                    if (!s[3] || std::memcmp(b, d, 4) == 0) continue;
                    int c[3];
                    for (int k = 0; k < 3; k++) c[k] = std::min(255, (s[k] * 255 + s[3] / 2) / s[3]);
                    float g = gate.sourceFactor(c);
                    if (g > 0 && gate.anyUnder && b[3]) {
                        for (int k = 0; k < 3; k++) c[k] = std::min(255, (b[k] * 255 + b[3] / 2) / b[3]);
                        g *= gate.underFactor(c, b[3] / 255.0f);
                    }
                    if (g >= 1) continue;
                    for (int k = 0; k < 4; k++) d[k] = uint8_t(std::lround(b[k] + (d[k] - b[k]) * g));
                }
            }
        });
    }

    /// The layer's own colours, for This Layer's gate: drawn alone, without effects or folder masks.
    Image ownColours(const Layer& layer) {
        Image source(outWidth, outHeight);
        const bool wasPlain = plainOnly, wasUngated = ungated;
        plainOnly = true; ungated = true;
        drawOwn(layer, source, nullptr);
        plainOnly = wasPlain; ungated = wasUngated;
        return source;
    }

    /// Advanced Blending's Channels (blendif.h): `target` holds a layer drawn over `before`; the channels it excludes
    /// keep their premultiplied values from `before`, and with every channel excluded the whole pixel does.
    void keepChannels(uint8_t excluded, const Image& before, Image& target) {
        const bool all = excluded == allBlendChannels(document.colorMode);
        parallelRows(0, outHeight, [&](int ya, int yb) {
            for (int y = ya; y < yb; y++) {
                const uint8_t* b = before.row(y);
                uint8_t* d = target.row(y);
                if (all) { std::memcpy(d, b, size_t(outWidth) * 4); continue; }
                for (int x = 0; x < outWidth; x++, b += 4, d += 4)
                    for (int k = 0; k < 3; k++) if (excluded & (1u << k)) d[k] = std::min(b[k], d[3]);
            }
        });
    }

    /// Runs `draw` over `target` with `layer`'s channel exclusions; a layer that excludes every channel is not drawn.
    template <class Draw>
    void excluding(const Layer& layer, Image& target, Draw&& draw) {
        const uint8_t excluded = plainOnly ? 0 : plan.excluded(layer);
        if (!excluded) { draw(); return; }
        if (excluded == allBlendChannels(document.colorMode)) return;
        const Image before = target;
        draw();
        keepChannels(excluded, before, target);
    }

    static bool isolates(const Layer& g) { return RenderPlan::isolates(g); }
    static bool fades(const Layer& g) { return RenderPlan::fades(g); }

    /// The folders being drawn: an isolated one draws into its own buffer, a fading one keeps what was under it, one
    /// that excludes channels keeps what was under it before its effects.
    struct Frame { const Layer* group; std::unique_ptr<Image> buffer; std::unique_ptr<Image> before; Image* parent; std::unique_ptr<Image> unexcluded; };
    std::vector<Frame> frames;

    /// NEKOPHOTO_DUMP_FOLDERS=<dir>: every styled folder's stages as PNGs (a developer's aid for comparing with
    /// Photoshop): the backdrop, after its exterior effects, its children drawn, after its opacity, after its interior
    /// effects, and the shape its effects are made from.
    void dumpStage(const Layer& g, const char* stage, const Image& image) const {
        static const char* dir = std::getenv("NEKOPHOTO_DUMP_FOLDERS");
        if (!dir || !layerStyleOf(g, document)) return;
        std::string name = g.name;
        for (char& c : name) if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
        writePngImage(std::string(dir) + "/" + name + "_" + g.id.substr(0, 4) + "_" + stage + ".png", image);
    }

    void openGroup(const Layer& g, Image*& cur) {
        dumpStage(g, "1-backdrop", *cur);
        // Excluded channels hold over the folder's effects and every child, Pass Through or not.
        std::unique_ptr<Image> unexcluded = plan.excluded(g) ? std::make_unique<Image>(*cur) : nullptr;
        if (layerStyleOf(g, document)) drawGroupStyle(g, *cur, StyledDraw::Phase::Exterior);
        dumpStage(g, "2-exterior", *cur);
        Frame f{&g, nullptr, nullptr, cur, std::move(unexcluded)};
        if (isolates(g)) { f.buffer = std::make_unique<Image>(outWidth, outHeight); cur = f.buffer.get(); }
        else if (fades(g) || folderContentFill(g) < 1) f.before = std::make_unique<Image>(*cur);
        frames.push_back(std::move(f));
    }

    void closeGroup(const Layer& g, Image*& cur) {
        if (frames.empty() || frames.back().group != &g) return;
        Frame f = std::move(frames.back());
        frames.pop_back();
        dumpStage(g, "3-children", *cur);
        // The contents at the folder's opacity and, when it has a style, its Fill (the effects are drawn after).
        const float opacity = float(clamp(g.opacity, 0.0, 1.0)) * folderContentFill(g);
        const bool foldsInteriors = f.buffer && interiorsAsGroup(g);
        if (foldsInteriors) drawGroupStyle(g, *f.buffer, StyledDraw::Phase::InteriorOverlays);
        if (f.buffer) {
            // The folder's result, in its own mode and opacity, over what is below it.
            cur = f.parent;
            const BlendMode mode = blendOf(g);
            const std::optional<BlendIf> blendIf = layerBlendIf(g, document.colorMode);
            std::optional<Image> before;
            if (blendIf) before.emplace(*cur);
            parallelRows(0, outHeight, [&](int ya, int yb) {
                for (int y = ya; y < yb; y++) {
                    const uint8_t* src = f.buffer->row(y);
                    uint8_t* dst = cur->row(y);
                    for (int x = 0; x < outWidth; x++) if (src[x * 4 + 3]) compositePixelAt(mode, src + x * 4, opacity, dst + x * 4, docX(region, scale, x), docY(region, scale, y));
                }
            });
            // A folder's Blend If gates its result against what is under the folder.
            if (blendIf) applyGate(BlendIfGate(*blendIf, document.colorMode), *f.buffer, *before, *cur);
        } else if (f.before) {
            // Pass Through at reduced opacity: the children met the backdrop at full strength; the result fades
            // back toward it (Photoshop's non-isolated group opacity).
            parallelRows(0, outHeight, [&](int ya, int yb) {
                for (int y = ya; y < yb; y++) {
                    const uint8_t* b = f.before->row(y);
                    uint8_t* d = cur->row(y);
                    for (int i = 0; i < outWidth * 4; i++) d[i] = uint8_t(std::lround(b[i] + (d[i] - b[i]) * opacity));
                }
            });
        }
        dumpStage(g, "4-opacity", *cur);
        if (layerStyleOf(g, document)) drawGroupStyle(g, *cur, foldsInteriors ? StyledDraw::Phase::InteriorRest : StyledDraw::Phase::Interior);
        dumpStage(g, "5-interior", *cur);
        if (f.unexcluded) keepChannels(plan.excluded(g), *f.unexcluded, *cur);
    }

    /// Blend Interior Effects as Group on a folder with a style: its overlays and satins join its result before its
    /// mode blends it (Patchy's photoshop-group-fx-interior probe: a Multiply folder's Color Overlay multiplies).
    bool interiorsAsGroup(const Layer& g) const {
        auto style = layerStyleOf(g, document);
        return style && style->blendInteriorAsGroup;
    }

    void drawGroupStyle(const Layer& group, Image& out, StyledDraw::Phase phase) {
        auto style = layerStyleOf(group, document);
        if (!style) return;
        StyledDraw draw;
        draw.style = style;
        draw.region = region;
        draw.scale = scale;
        draw.phase = phase;
        layerOpacities(group, draw.master, draw.fill);
        draw.fill = 1;   // Photoshop ignores a folder's Fill once it has effects
        if (isolates(group) || fades(group)) draw.master = 1;   // the folder's opacity already faded its result
        auto folders = foldersCoverage(group.parentId);
        draw.coverage = folders.get();
        draw.patterns = documentPatterns(document);
        draw.documentWidth = document.width;
        draw.documentHeight = document.height;
        // The effects' shape: the folder's children drawn alone, their effects included, through its mask.
        draw.drawSource = [&](Image& into, const Rect& area) {
            RenderPlan subPlan(document, overrides);
            subPlan.buildWithin(plan, group.id);
            RenderExec sub(subPlan, area, scale, into.width(), into.height());
            sub.drawRange(into, 0, sub.order.size());
            dumpStage(group, phase == StyledDraw::Phase::Exterior ? "0-shape-exterior" : "0-shape-interior", into);
        };
        drawStyledLayer(draw, out);
    }
    std::map<Uuid, std::shared_ptr<GrayImage>> folderCoverage; // per group id, that group's own mask coverage
    std::map<std::optional<Uuid>, std::shared_ptr<GrayImage>> chainCoverage; // per parent, all enclosing folders combined

    const LayerOverride* over(const Uuid& id) const { return plan.over(id); }
    LayerTransform transformOf(const Layer& l) const { return plan.transformOf(l); }
    ImagePtr imageOf(const Layer& l) const {
        auto* o = over(l.id);
        if (o && o->image) return *o->image;
        return l.asset ? l.asset->image.u8() : nullptr;
    }
    BlendMode blendOf(const Layer& l) const { return plan.blendOf(l); }

    std::shared_ptr<GrayImage> folderMask(const Layer& group) {
        auto it = folderCoverage.find(group.id);
        if (it != folderCoverage.end()) return it->second;
        std::shared_ptr<GrayImage> result;
        if (group.mask && group.mask->enabled && group.mask->asset.image.u8()) {
            result = std::make_shared<GrayImage>(outWidth, outHeight, 0);
            sampleMaskCoverage(group.mask->asset.image.u8(), transformOf(group), region, scale, 0, *result, false);
        }
        // An artboard clips its children to its rectangle.
        if (group.artboard) result = multiply(result, artboardCoverage(*group.artboard));
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

    /// How much of each output pixel an artboard's rectangle covers.
    std::shared_ptr<GrayImage> artboardCoverage(const Artboard& a) const {
        auto result = std::make_shared<GrayImage>(outWidth, outHeight, 0);
        auto span = [&](double lo, double hi, double origin, int n, std::vector<float>& out) {
            out.assign(size_t(n), 0.0f);
            for (int i = 0; i < n; i++) {
                const double p0 = origin + i / scale, p1 = origin + (i + 1) / scale;
                out[size_t(i)] = float(std::clamp((std::min(p1, hi) - std::max(p0, lo)) * scale, 0.0, 1.0));
            }
        };
        std::vector<float> cx, cy;
        span(a.x, a.x + a.width, region.x, outWidth, cx);
        span(a.y, a.y + a.height, region.y, outHeight, cy);
        for (int y = 0; y < outHeight; y++) {
            if (cy[size_t(y)] <= 0) continue;
            uint8_t* row = result->row(y);
            for (int x = 0; x < outWidth; x++) row[x] = uint8_t(std::lround(cx[size_t(x)] * cy[size_t(y)] * 255));
        }
        return result;
    }

    /// An artboard's background, under its children (the artboard's entry in `order`).
    void drawArtboardBackground(const Layer& group, Image& out) {
        uint8_t colour[4];
        group.artboard->fill(colour);
        if (!colour[3]) return;
        auto clip = multiply(foldersCoverage(group.parentId), artboardCoverage(*group.artboard));
        parallelRows(0, outHeight, [&](int ya, int yb) {
            for (int y = ya; y < yb; y++) {
                const uint8_t* c = clip->row(y);
                uint8_t* d = out.row(y);
                for (int x = 0; x < outWidth; x++) if (c[x]) compositePixelAt(BlendMode::Normal, colour, c[x] / 255.0f, d + x * 4, docX(region, scale, x), docY(region, scale, y));
            }
        });
    }

    static std::shared_ptr<GrayImage> multiply(const std::shared_ptr<GrayImage>& a, const std::shared_ptr<GrayImage>& b) {
        if (!a) return b;
        if (!b) return a;
        auto r = std::make_shared<GrayImage>(*a);
        for (size_t i = 0; i < r->byteCount(); i++) r->data()[i] = uint8_t((r->data()[i] * b->data()[i] + 127) / 255);
        return r;
    }

    DrawParams paramsFor(const Layer& layer, const ImagePtr& image) const {
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
        return params;
    }

    /// A gradient or pattern fill layer's contents (they have no pixels of their own), kept while its carry lives
    /// (fill_cache.h: least recently used first out, within 256 MB).
    ImagePtr fillImage(const Layer& layer) {
        static FillLayerCache<ImagePtr> cache;
        if (!layer.psdCarry) return nullptr;
        if (auto cached = cache.find(layer.psdCarry, document.width, document.height)) return *cached;
        ImagePtr image = renderFillLayer(layer, document);
        cache.insert(layer.psdCarry, document.width, document.height, image);
        return image;
    }

    void drawOwn(const Layer& layer, Image& target, const GrayImage* coverage) {
        if (!plainOnly && !ungated) if (auto blendIf = layerBlendIf(layer, document.colorMode)) {
            const Image before = target;
            const BlendIfGate gate(*blendIf, document.colorMode);
            // The gate takes the layer's pixels; a styled draw runs it before the interior effects (Photoshop does
            // not gate them), otherwise it runs over the whole draw (exterior effects outside the pixels pass).
            bool gated = false;
            std::function<void()> gateNow = [&] { applyGate(gate, ownColours(layer), before, target); gated = true; };
            ungated = true;
            interiorGate = &gateNow;
            drawOwn(layer, target, coverage);
            interiorGate = nullptr;
            ungated = false;
            if (!gated) gateNow();
            return;
        }
        std::function<void()>* const gateInside = std::exchange(interiorGate, nullptr);
        ImagePtr image = imageOf(layer);
        if (!image) image = fillImage(layer);
        if (!image) return;
        DrawParams params = paramsFor(layer, image);
        if (!image->isEmpty() && !layer.asset) params.transform = LayerTransform(Point(0, 0), Size(document.width, document.height));
        const GrayImage* coverageWithoutVector = coverage;
        const std::optional<MaskParameters> maskParameters = layer.psdCarry ? parseMaskParameters(layer.psdCarry->maskData) : std::nullopt;
        // A vector mask (a shape layer's shape, or a mask drawn with paths) cuts the layer like its pixel mask.
        std::optional<VectorPath> vector = layerVectorMask(layer, document);
        std::shared_ptr<GrayImage> cut;
        if (vector) {
            cut = rasterizeVectorMask(*vector, region, scale, outWidth, outHeight);
            if (maskParameters) applyMaskParameters(*cut, maskParameters->vectorDensity, maskParameters->vectorFeather, scale);
            if (coverage) for (size_t i = 0; i < cut->byteCount(); i++) cut->data()[i] = uint8_t((cut->data()[i] * coverage->data()[i] + 127) / 255);
            coverage = cut.get();
        }
        // A pixel mask with Photoshop's density or feather: drawn here with them, instead of by drawLayer.
        std::shared_ptr<GrayImage> userCut;
        if (maskParameters && (maskParameters->userDensity || maskParameters->userFeather) && layer.mask && layer.mask->enabled && layer.mask->asset.image.u8()) {
            userCut = std::make_shared<GrayImage>(outWidth, outHeight, 0);
            sampleMaskCoverage(layer.mask->asset.image.u8(), layer.maskTransform(), region, scale, 0, *userCut, false);
            applyMaskParameters(*userCut, maskParameters->userDensity, maskParameters->userFeather, scale, true);
            if (coverage) for (size_t i = 0; i < userCut->byteCount(); i++) userCut->data()[i] = uint8_t((userCut->data()[i] * coverage->data()[i] + 127) / 255);
            coverage = userCut.get();
            params.mask = nullptr;
        }
        // A shape's stroke goes over its fill, in the layer's mode; with its fill off, the stroke alone.
        const std::optional<VectorStroke> stroke = vector ? layerVectorStroke(layer) : std::nullopt;
        // The stroke into `into`, in `mode` at `opacity` through `cover`; `feathered`: softened by the shape's Feather.
        auto strokeInto = [&](Image& into, BlendMode mode, float opacity, const GrayImage* cover, bool feathered) {
            if (!stroke || !stroke->enabled || stroke->opacity <= 0) return;
            VectorPath path = *vector;   // parsed once per draw
            path.inverted = false;
            auto band = rasterizeVectorStroke(path, *stroke, region, scale, outWidth, outHeight);
            if (feathered && maskParameters && maskParameters->vectorFeather) applyMaskParameters(*band, std::nullopt, maskParameters->vectorFeather, scale);
            // A gradient or pattern stroke: its colours over the region (a gradient aligned with the shape's bounds).
            ImagePtr paint;
            if (stroke->paint.kind != VectorPaint::Kind::Solid) {
                double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
                for (auto& s : path.subpaths) for (auto& k : s.knots) for (auto [px, py] : {std::pair{k.x, k.y}, {k.inX, k.inY}, {k.outX, k.outY}}) {
                    x0 = std::min(x0, px); x1 = std::max(x1, px); y0 = std::min(y0, py); y1 = std::max(y1, py);
                }
                paint = renderVectorPaint(stroke->paint, document, x1 > x0 ? Rect(x0, y0, x1 - x0, y1 - y0) : Rect(), region, scale, outWidth, outHeight);
            }
            parallelRows(0, outHeight, [&](int ya, int yb) {
                for (int y = ya; y < yb; y++) {
                    const uint8_t* b = band->row(y);
                    const uint8_t* c = cover ? cover->row(y) : nullptr;
                    const uint8_t* p = paint ? paint->row(y) : nullptr;
                    uint8_t* d = into.row(y);
                    for (int x = 0; x < outWidth; x++) {
                        if (!b[x]) continue;
                        uint8_t colour[4] = {stroke->r, stroke->g, stroke->b, 255};
                        float alpha = 1;
                        if (p) {
                            const uint8_t* q = p + x * 4;
                            if (!q[3]) continue;
                            for (int k = 0; k < 3; k++) colour[k] = uint8_t(std::min(255, (q[k] * 255 + q[3] / 2) / q[3]));
                            alpha = q[3] / 255.0f;
                        }
                        compositePixelAt(mode, colour, b[x] / 255.0f * opacity * alpha * (c ? c[x] / 255.0f : 1.0f), d + x * 4, docX(region, scale, x), docY(region, scale, y));
                    }
                }
            });
        };
        auto drawStroke = [&] { strokeInto(target, blendOf(layer), float(clamp(layer.opacity, 0.0, 1.0)) * (stroke ? stroke->opacity : 1.0f), coverageWithoutVector, true); };
        // A shape layer with a Feather: Photoshop draws the shape, fill and stroke, then feathers the drawing as a whole
        // (photoshop-shape-feather), so where the stroke covers the fill the edge fades once, not twice, and the fill
        // fades past its own pixels.
        const bool stroked = stroke && stroke->enabled;
        if (vector && maskParameters && maskParameters->vectorFeather && *maskParameters->vectorFeather > 0 && (stroked || isVectorShapeLayer(layer))
            && !(layer.mask && layer.mask->enabled) && (plainOnly || !layerStyleOf(layer, document))) {
            Image shape(outWidth, outHeight);
            if (!stroked || stroke->fillEnabled) {
                auto sharp = rasterizeVectorMask(*vector, region, scale, outWidth, outHeight);
                if (maskParameters->vectorDensity) applyMaskParameters(*sharp, maskParameters->vectorDensity, std::nullopt, scale);
                DrawParams plain = params;
                plain.opacity = 1;
                plain.mode = BlendMode::Normal;
                drawLayer(plain, region, scale, sharp.get(), shape);
            }
            if (stroked) strokeInto(shape, BlendMode::Normal, stroke->opacity, nullptr, false);
            featherDrawnShape(shape, 4, vectorFeatherSigma(*maskParameters->vectorFeather) * scale);
            const float opacity = float(clamp(layer.opacity, 0.0, 1.0));
            const BlendMode mode = blendOf(layer);
            parallelRows(0, outHeight, [&](int ya, int yb) {
                for (int y = ya; y < yb; y++) {
                    const uint8_t* s = shape.row(y);
                    const uint8_t* c = coverageWithoutVector ? coverageWithoutVector->row(y) : nullptr;
                    uint8_t* d = target.row(y);
                    for (int x = 0; x < outWidth; x++)
                        if (s[x * 4 + 3]) compositePixelAt(mode, s + x * 4, opacity * (c ? c[x] / 255.0f : 1.0f), d + x * 4, docX(region, scale, x), docY(region, scale, y));
                }
            });
            return;
        }
        if (stroke && stroke->enabled && !stroke->fillEnabled) { drawStroke(); return; }
        // A Photoshop layer style draws the layer with its effects (never while taking a clipping base's
        // transparency, which effects do not shape).
        if (!plainOnly) if (auto style = layerStyleOf(layer, document)) {
            StyledDraw draw;
            draw.style = style;
            draw.region = region;
            draw.scale = scale;
            draw.mode = params.mode;
            layerOpacities(layer, draw.master, draw.fill);
            draw.coverage = coverage;
            draw.patterns = documentPatterns(document);
            draw.documentWidth = document.width;
            draw.documentHeight = document.height;
            draw.bounds = params.transform.bounds();
            // Blend If gates the pixels before the interior effects land. With Blend Interior Effects as Group the
            // interiors join the pixels and are gated with them; a shape's stroke, drawn after the effects, keeps
            // the gate over the whole draw.
            if (gateInside && !style->blendInteriorAsGroup && !(stroke && stroke->enabled)) draw.afterContent = *gateInside;
            draw.drawSource = [&](Image& into, const Rect& area) {
                DrawParams plain = params;
                plain.opacity = 1;
                plain.mode = BlendMode::Normal;
                // The vector mask shapes what the effects are drawn around, as the pixel mask does.
                std::shared_ptr<GrayImage> shape = vector ? rasterizeVectorMask(*vector, area, scale, into.width(), into.height()) : nullptr;
                drawLayer(plain, area, scale, shape.get(), into);
            };
            // The folders' coverage alone: the vector mask is already in the source.
            if (vector) draw.coverage = coverageWithoutVector;
            drawStyledLayer(draw, target);
            drawStroke();
            return;
        }
        if (!plainOnly && params.mode == BlendMode::Normal && document.colorMode == ColorMode::RGB && photoshopType(layer)) {
            // Type in Normal mode blends its colours with Photoshop's text gamma.
            Image own(outWidth, outHeight);
            drawLayer(params, region, scale, coverage, own);
            blendTextGamma(own, target);
            drawStroke();
            return;
        }
        drawLayer(params, region, scale, coverage, target);
        drawStroke();
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
        const bool wasPlain = plainOnly;
        plainOnly = true;
        drawClipped(*lit->second, pixels, nullptr);
        plainOnly = wasPlain;
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
        work::add(work::Counter::Adjustments);
        work::add(work::Counter::AdjustmentPixels, uint64_t(target.width()) * uint64_t(target.height()));
        Image adjusted = target;
        if (!applyAdjustment(*layer.adjustment, adjusted, region, scale)) return;
        BlendMode mode = blendOf(layer);
        float opacity = float(clamp(layer.opacity, 0.0, 1.0));
        // Clip: the layer's own mask over its transform (and folder masks in `coverage`).
        std::shared_ptr<GrayImage> clip = coverage;
        if (layer.mask && layer.mask->enabled && layer.mask->asset.image.u8()) {
            auto own = std::make_shared<GrayImage>(outWidth, outHeight, 0);
            sampleMaskCoverage(layer.mask->asset.image.u8(), transformOf(layer), region, scale, 0, *own, false);
            clip = multiply(clip, own);
        }
        // Blend If: This Layer reads the adjusted colours, Underlying Layer the colours before the adjustment.
        const std::optional<BlendIf> blendIf = layerBlendIf(layer, document.colorMode);
        std::optional<BlendIfGate> gate;
        if (blendIf) gate.emplace(*blendIf, document.colorMode);
        if (mode == BlendMode::Normal && !gate) {
            // Same alpha on both sides, so the straight-colour lerp is a premultiplied lerp: no divides.
            parallelRows(0, outHeight, [&](int ya, int yb) {
            for (int y = ya; y < yb; y++) {
                uint8_t* d = target.row(y);
                const uint8_t* a = adjusted.row(y);
                const uint8_t* c = clip ? clip->row(y) : nullptr;
                for (int x = 0; x < outWidth; x++, d += 4, a += 4) {
                    if (d[3] == 0) continue;
                    int mix = int(opacity * (c ? c[x] : 255) + 0.5f);   // 0..255
                    if (mix <= 0) continue;
                    for (int k = 0; k < 3; k++) {
                        int delta = (int(a[k]) - int(d[k])) * mix;   // rounded symmetrically: a full mix is exact either way
                        d[k] = uint8_t(std::min(int(d[3]), int(d[k]) + (delta + (delta < 0 ? -127 : 127)) / 255));
                    }
                }
            }
            });
            return;
        }
        parallelRows(0, outHeight, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            uint8_t* d = target.row(y);
            const uint8_t* a = adjusted.row(y);
            const uint8_t* c = clip ? clip->row(y) : nullptr;
            for (int x = 0; x < outWidth; x++, d += 4, a += 4) {
                float mix = opacity * (c ? c[x] / 255.0f : 1.0f);
                if (mix <= 0 || d[3] == 0) continue;
                if (gate) {
                    int sc[3], bc[3];
                    for (int k = 0; k < 3; k++) { sc[k] = std::min(255, (a[k] * 255 + d[3] / 2) / d[3]); bc[k] = std::min(255, (d[k] * 255 + d[3] / 2) / d[3]); }
                    mix *= gate->sourceFactor(sc) * gate->underFactor(bc, d[3] / 255.0f);
                    if (mix <= 0) continue;
                }
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

    /// The layer's transfer when it can be applied in place: a table-driven adjustment, unclipped, at full
    /// opacity in Normal mode, without a layer mask or a masked folder above it.
    std::optional<Transfer> fusibleTransfer(const Layer& layer) {
        if (!layer.adjustment || layer.maskSourceId || stacked.count(layer.id)) return std::nullopt;
        if (blendOf(layer) != BlendMode::Normal || clamp(layer.opacity, 0.0, 1.0) < 1) return std::nullopt;
        if (layer.mask && layer.mask->enabled && layer.mask->asset.image.u8()) return std::nullopt;
        if (foldersCoverage(layer.parentId)) return std::nullopt;
        if (plan.hasBlendIf(layer) || plan.excluded(layer)) return std::nullopt;
        AdjustmentSettings settings;
        if (!AdjustmentSettings::parse(layer.adjustment->json, settings)) return std::nullopt;
        return adjustmentTransfer(settings);
    }

    /// A clipped adjustment layer being dragged (render_resume.h): its stack's state just before it, kept by the first
    /// frame (`stackKeep`) and started from by the next ones (`stackResume`). A plain base's stack keeps its buffer and
    /// the base's alpha; a styled base's, the frame it draws into and the clip.
    struct StackState { Uuid child; bool reached = false; std::unique_ptr<Image> group; std::vector<uint8_t> alpha; std::shared_ptr<GrayImage> clip; };
    StackState* stackKeep = nullptr;
    const StackState* stackResume = nullptr;

    /// Where the stack's dragged layer is among its children: the stack resumes there (`resume`) or keeps its state
    /// there (`keep`); children.size() for neither.
    size_t stackChild(const std::vector<Uuid>& children, const StackState*& resume, StackState*& keep) {
        resume = nullptr; keep = nullptr;
        const Uuid* child = stackResume ? &stackResume->child : stackKeep ? &stackKeep->child : nullptr;
        if (!child) return children.size();
        const size_t at = size_t(std::find(children.begin(), children.end(), *child) - children.begin());
        if (at == children.size()) return at;
        if (stackResume) { resume = stackResume; stackResume = nullptr; }
        else { keep = stackKeep; stackKeep = nullptr; }
        return at;
    }

    /// One entry of the drawing order: a layer (with the layers clipped to it), an adjustment, or an artboard's
    /// background; through the layer's channel exclusions (a clipping base's hold over its whole clipped result).
    void drawComposite(const Layer& layer, Image& out) {
        if (layer.isGroup) { if (layer.artboard) drawArtboardBackground(layer, out); return; }
        if (stacked.count(layer.id)) return;
        excluding(layer, out, [&] { drawUnexcluded(layer, out); });
    }

    void drawUnexcluded(const Layer& layer, Image& out) {
        std::shared_ptr<GrayImage> folders = foldersCoverage(layer.parentId);
        if (layer.adjustment) {
            if (!layer.maskSourceId) adjust(layer, out, folders);
            return;
        }
        auto stack = stacks.find(layer.id);
        if (stack == stacks.end()) { drawClipped(layer, out, folders); return; }
        const StackState* resume = nullptr;
        StackState* keep = nullptr;
        const size_t dragged = plainOnly ? stack->second.size() : stackChild(stack->second, resume, keep);
        if (!plainOnly && layerStyleOf(layer, document)) {
            // A styled base: the base with its effects, then the clipped layers over it, masked by the base's own
            // transparency (never by its effects, as in Photoshop).
            std::shared_ptr<GrayImage> clip;
            size_t first = 0;
            if (resume) {
                std::memcpy(out.data(), resume->group->data(), out.byteCount());
                clip = resume->clip;
                first = dragged;
            } else {
                drawOwn(layer, out, folders.get());
                clip = multiply(folders, coverageOf(layer.id));
            }
            for (size_t c = first; c < stack->second.size(); c++) {
                const Uuid& childId = stack->second[c];
                if (keep && c == dragged) { keep->group = std::make_unique<Image>(out); keep->clip = clip; keep->reached = true; }
                auto it = byId.find(childId);
                if (it == byId.end()) continue;
                excluding(*it->second, out, [&] {
                    if (it->second->adjustment) adjust(*it->second, out, clip);
                    else drawOwn(*it->second, out, clip.get());
                });
            }
            return;
        }
        // A clipping stack: the base's alpha is shared by the layers clipped to it.
        Image group(outWidth, outHeight);
        // A base with Blend If: its gate applies to the clipped result against the backdrop, so it does not shrink
        // the shape the clipped layers take.
        const std::optional<BlendIf> blendIf = plainOnly ? std::nullopt : layerBlendIf(layer, document.colorMode);
        std::vector<uint8_t> alpha;
        size_t first = 0;
        if (resume) {
            std::memcpy(group.data(), resume->group->data(), group.byteCount());
            alpha = resume->alpha;
            first = dragged;
        } else {
            const bool wasUngated = ungated;
            if (blendIf) ungated = true;
            drawOwn(layer, group, nullptr);
            ungated = wasUngated;
            alpha.resize(size_t(outWidth) * outHeight);
            layer_extract_alpha(group.data(), size_t(group.stride()), alpha.data(), size_t(outWidth), size_t(outWidth), size_t(outHeight));
            layer_unpremultiply_opaque(group.data(), size_t(group.stride()), size_t(outWidth), size_t(outHeight));
        }
        for (size_t c = first; c < stack->second.size(); c++) {
            const Uuid& childId = stack->second[c];
            if (keep && c == dragged) { keep->group = std::make_unique<Image>(group); keep->alpha = alpha; keep->reached = true; }
            auto it = byId.find(childId);
            if (it == byId.end()) continue;
            // A clipped layer's excluded channels keep the base's colours.
            excluding(*it->second, group, [&] {
                if (it->second->adjustment) adjust(*it->second, group, nullptr);
                else drawOwn(*it->second, group, nullptr);
            });
        }
        layer_restore_alpha(group.data(), size_t(group.stride()), alpha.data(), size_t(outWidth), size_t(outWidth), size_t(outHeight));
        BlendMode mode = blendOf(layer);
        std::optional<Image> before;
        if (blendIf) before.emplace(out);
        parallelRows(0, outHeight, [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            const uint8_t* s = group.row(y);
            uint8_t* d = out.row(y);
            const uint8_t* c = folders ? folders->row(y) : nullptr;
            for (int x = 0; x < outWidth; x++, s += 4, d += 4) {
                float cov = c ? c[x] / 255.0f : 1.0f;
                if (cov > 0) compositePixelAt(mode, s, cov, d, docX(region, scale, x), docY(region, scale, y));
            }
        }
        });
        if (blendIf) applyGate(BlendIfGate(*blendIf, document.colorMode), ownColours(layer), *before, out);
    }

    /// Draws the layers order[from, to) over `out`.
    void drawRange(Image& out, size_t from, size_t to) {
        Image* cur = &out;
        drawSpan(cur, from, to);
    }

    /// The fused run of adjustments that starts at `index`: its transfer, and its end (`index` when there is none).
    std::optional<Transfer> fusedRun(size_t index, size_t to, size_t& end) {
        std::optional<Transfer> fused;
        for (end = index; end < to; end++) {
            if (end > index && (groupsOpen.count(end) || groupsClose.count(end - 1))) break;
            std::optional<Transfer> next = fusibleTransfer(*order[end]);
            if (!next) break;
            fused = fused ? composeTransfer(*fused, *next) : std::move(next);
        }
        return fused;
    }

    /// Draws order[from, to) into `cur`, going on from the folders open in `frames`.
    void drawSpan(Image*& cur, size_t from, size_t to) {
        for (size_t index = from; index < to;) {
            // A run of table-driven adjustment layers (Levels, Curves, Exposure) at full opacity in Normal mode
            // with no masks composes into one transfer: one pass over the canvas, quantised once.
            if (auto open = groupsOpen.find(index); open != groupsOpen.end())
                for (const Layer* g : open->second) openGroup(*g, cur);
            size_t end = index;
            std::optional<Transfer> fused = fusedRun(index, to, end);
            if (fused) {
                work::add(work::Counter::Adjustments, uint64_t(end - index));
                work::add(work::Counter::AdjustmentPixels, uint64_t(cur->width()) * uint64_t(cur->height()));   // one pass for the run
                applyTransfer(*cur, *fused);
                for (size_t i = index; i < end; i++)
                    if (auto close = groupsClose.find(i); close != groupsClose.end()) for (const Layer* g : close->second) closeGroup(*g, cur);
                index = end;
                continue;
            }
            drawComposite(*order[index], *cur);
            if (auto close = groupsClose.find(index); close != groupsClose.end())
                for (const Layer* g : close->second) closeGroup(*g, cur);
            index++;
        }
    }

    bool plainAbove(const Layer& l) const { return plan.plainAbove(l); }

    void runCached(Image& out, RenderCache& cache, uint64_t version, size_t edited) {
        const Layer& layer = *order[edited];
        const bool valid = cache.backdrop && cache.version == version && cache.layer == layer.id && cache.width == outWidth && cache.height == outHeight
            && cache.region == region && cache.scale == scale;
        if (!valid) {
            cache.version = version; cache.layer = layer.id; cache.width = outWidth; cache.height = outHeight; cache.region = region; cache.scale = scale;
            cache.resume.reset();
            cache.backdrop = std::make_shared<Image>(outWidth, outHeight);
            drawRange(*cache.backdrop, 0, edited);
            cache.above.reset();
            cache.aboveFlat = true;
            for (size_t i = edited + 1; i < order.size(); i++) if (!plainAbove(*order[i])) { cache.aboveFlat = false; break; }
            if (cache.aboveFlat && edited + 1 < order.size()) {
                cache.above = std::make_shared<Image>(outWidth, outHeight);
                drawRange(*cache.above, edited + 1, order.size());
            }
        }
        std::memcpy(out.data(), cache.backdrop->data(), out.byteCount());
        drawComposite(layer, out);
        if (!cache.aboveFlat) { drawRange(out, edited + 1, order.size()); return; }
        if (!cache.above) return;
        // Source-over is associative: the flattened layers above go on in one pass.
        const std::vector<uint16_t> steps(size_t(outWidth), coverageSteps(1.0f));
        parallelRows(0, outHeight, [&](int y0, int y1) {
            for (int y = y0; y < y1; y++) compositeSpanNormal(cache.above->row(y), steps.data(), out.row(y), outWidth);
        });
    }

    /// Where a frame for the adjustment layer at `adjusting` resumes (RenderPlan::resumeIndex), moved down to the start
    /// of the fused run it is in, or that it would join with other settings: a run is one pass, quantised once, so
    /// it is drawn whole as an uncached frame draws it.
    size_t resumeIndex(size_t adjusting) {
        const size_t at = plan.resumeIndex(adjusting);
        for (size_t index = 0; index < at;) {
            size_t end = index;
            const bool run = bool(fusedRun(index, order.size(), end));
            if (!run) { index++; continue; }
            if (at < end) return index;
            if (end == at && at == adjusting && !groupsOpen.count(at) && !groupsClose.count(at - 1)) return index;
            index = end;
        }
        return at;
    }

    void run(Image& out, RenderCache* cache, uint64_t version) {
        const size_t edited = cache ? plan.editedIndex() : SIZE_MAX;
        if (edited != SIZE_MAX) { runCached(out, *cache, version, edited); return; }
        const size_t adjusting = cache ? plan.adjustingIndex() : SIZE_MAX;
        static const char kind = 0;
        if (adjusting != SIZE_MAX) drawResumed(*this, out, *cache, version, adjusting, resumeIndex(adjusting), &kind);
        else drawRange(out, 0, order.size());
    }
};

template <>
void executeRender<SampleType::U8>(const RenderPlan& plan, const Rect& region, double scale, Image& out, RenderCache* cache, uint64_t version) {
    RenderExec<SampleType::U8> exec(plan, region, scale, out.width(), out.height());
    exec.run(out, cache, version);
}

} // namespace compositor
