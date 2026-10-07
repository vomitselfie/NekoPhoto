// The renderer's pixels at a deep sample type (render_plan.h), written once for every depth above 8 bits:
// `RenderExecDeep<S>` follows the 8-bit executor's structure, with coverage (masks, folder masks, clipping) at the
// document's depth. What differs between depths (sample arithmetic, blend spans, the primitives that draw a layer
// or sample a mask) comes from the `DeepOps<S>` policy (render_deep_ops.h). Each depth's file includes this once
// and instantiates it (render_exec_u16.cpp).
//
// Everything is drawn at the document's depth: pixel layers in every blend mode with opacity, pixel and vector
// masks, clipping stacks, folders (Pass Through, faded and isolated), artboards, shape fills and strokes, gradient
// and pattern fill layers, adjustment layers, and layer styles on layers and folders.
#include "render_deep_ops.h"
#include "compositor/fill_cache.h"
#include "compositor/layerstyle.h"
#include "compositor/parallel.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <type_traits>
#include <utility>
#include <vector>

namespace compositor {

namespace {

template <class O>
std::shared_ptr<typename O::Gray> multipliedDeep(const std::shared_ptr<typename O::Gray>& a, const std::shared_ptr<typename O::Gray>& b) {
    if (!a) return b;
    if (!b) return a;
    auto r = std::make_shared<typename O::Gray>(*a);
    const size_t n = size_t(r->width()) * size_t(r->height());
    for (size_t i = 0; i < n; i++) r->data()[i] = O::mul(r->data()[i], b->data()[i]);
    return r;
}

} // namespace

template <SampleType S, class Ops = DeepOps<S>>
struct RenderExecDeep {
    using O = Ops;
    /// Samples per pixel and colour samples (alpha is the last sample): 4 and 3 for RGB and Lab, 5 and 4 for CMYK.
    static constexpr int N = O::channels, C = N - 1;
    using Sample = typename O::Sample;
    using Img = typename O::Image;
    using Gray = typename O::Gray;
    using ImagePtr = typename O::ImagePtr;
    using GrayPtr = typename O::GrayPtr;
    using Cover = std::shared_ptr<Gray>;
    using Step = typename O::Step;
    using Params = typename O::Params;

    const RenderPlan& plan;
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
    RenderExecDeep(const RenderPlan& p, Rect r, double s, int w, int h)
        : plan(p), document(p.document), overrides(p.overrides), byId(p.byId), order(p.order), stacks(p.stacks), stacked(p.stacked),
          groupsOpen(p.groupsOpen), groupsClose(p.groupsClose), region(r), scale(s), outWidth(w), outHeight(h) {}
    std::map<Uuid, Cover> liveCoverage;
    std::set<Uuid> visiting;
    bool plainOnly = false;
    /// A Blend If layer's gate, for its styled draw to run between its pixels and its interior effects.
    std::function<void()>* interiorGate = nullptr;
    bool ungated = false;   // drawing a Blend If layer's own pass (blendif.h)

    /// A straight colour sample as Blend If reads it: 0..255, in the document's encoding.
    static int level(Sample c, Sample a) {
        if constexpr (requires { O::blendIfLevel(c, a); }) return O::blendIfLevel(c, a);
        else return std::clamp(int(std::lround(O::unit(c) / O::unit(a) * 255.0f)), 0, 255);
    }

    /// Blend If (the 8-bit executor's applyGate): `target` holds the layer drawn over `before`; each pixel keeps the
    /// share the gates allow. The gates read 8-bit levels at every depth.
    void applyGate(const BlendIfGate& gate, const Img& source, const Img& before, Img& target) {
        parallelRows(0, outHeight, [&](int ya, int yb) {
            for (int y = ya; y < yb; y++) {
                const Sample* s = source.row(y);
                const Sample* b = before.row(y);
                Sample* d = target.row(y);
                for (int x = 0; x < outWidth; x++, s += N, b += N, d += N) {
                    if (!(s[C] > 0) || std::memcmp(b, d, sizeof(Sample) * N) == 0) continue;
                    int c[N];
                    for (int k = 0; k < C; k++) c[k] = level(s[k], s[C]);
                    float g = gate.sourceFactor(c);
                    if (g > 0 && gate.anyUnder && b[C] > 0) {
                        for (int k = 0; k < C; k++) c[k] = level(b[k], b[C]);
                        g *= gate.underFactor(c, std::min(1.0f, O::unit(b[C])));
                    }
                    if (g >= 1) continue;
                    for (int k = 0; k < N; k++) d[k] = O::fade(b[k], d[k], g);
                }
            }
        });
    }

    /// The layer's own colours, for This Layer's gate.
    Img ownColours(const Layer& layer) {
        Img source = blank(outWidth, outHeight);
        const bool wasPlain = plainOnly, wasUngated = ungated;
        plainOnly = true; ungated = true;
        drawOwn(layer, source, nullptr);
        plainOnly = wasPlain; ungated = wasUngated;
        return source;
    }

    static bool isolates(const Layer& g) { return RenderPlan::isolates(g); }
    static bool fades(const Layer& g) { return RenderPlan::fades(g); }

    struct Frame { const Layer* group; std::unique_ptr<Img> buffer; std::unique_ptr<Img> before; Img* parent; };
    std::vector<Frame> frames;
    std::map<Uuid, Cover> folderCoverage;
    std::map<std::optional<Uuid>, Cover> chainCoverage;

    const LayerOverride* over(const Uuid& id) const { return plan.over(id); }
    LayerTransform transformOf(const Layer& l) const { return plan.transformOf(l); }
    BlendMode blendOf(const Layer& l) const { return plan.blendOf(l); }
    ImagePtr imageOf(const Layer& l) const {
        auto* o = over(l.id);
        if (o) if (auto image = O::overrideImage(*o)) return *image;
        return l.asset ? imageFrom(l.asset->image) : nullptr;
    }
    /// A layer's buffer at this layout: a policy that converts between colour modes takes the document.
    ImagePtr imageFrom(const AnyImage& any) const {
        if constexpr (requires { O::image(document, any); }) return O::image(document, any);
        else return O::image(any);
    }
    /// An RGB colour (straight bytes, alpha last) as a premultiplied pixel of this layout.
    void solid(const uint8_t rgba[4], Sample* out) const {
        if constexpr (requires { O::solid(document, rgba, out); }) O::solid(document, rgba, out);
        else for (int k = 0; k < 4; k++) out[k] = O::from8(rgba[k]);
    }
    /// A blank (transparent) buffer of this layout.
    static Img blank(int w, int h) {
        if constexpr (requires { Img(w, h, N); }) return Img(w, h, N);
        else return Img(w, h);
    }
    static GrayPtr maskOf(const Layer& l) { return l.mask ? O::gray(l.mask->asset.image) : nullptr; }

    void compositeRows(const Img& src, BlendMode mode, float opacity, const Gray* coverage, Img& dst) {
        parallelRows(0, outHeight, [&](int ya, int yb) {
            std::vector<Step> steps(static_cast<size_t>(outWidth));
            for (int y = ya; y < yb; y++) {
                const Sample* s = src.row(y);
                Sample* d = dst.row(y);
                const Sample* c = coverage ? coverage->row(y) : nullptr;
                if (mode == BlendMode::Dissolve) {
                    for (int x = 0; x < outWidth; x++)
                        if (s[x * N + C]) O::pixelAt(mode, s + x * N, opacity * (c ? O::unit(c[x]) : 1.0f), d + x * N, docX(region, scale, x), docY(region, scale, y));
                    continue;
                }
                for (int x = 0; x < outWidth; x++) steps[size_t(x)] = s[x * N + C] ? O::steps(opacity * (c ? O::unit(c[x]) : 1.0f)) : 0;
                O::span(mode, s, steps.data(), d, outWidth);
            }
        });
    }

    void openGroup(const Layer& g, Img*& cur) {
        if (O::styles && layerStyleOf(g, document)) drawGroupStyle(g, *cur, StyledDraw::Phase::Exterior);
        Frame f{&g, nullptr, nullptr, cur};
        if (isolates(g)) { f.buffer = std::make_unique<Img>(blank(outWidth, outHeight)); cur = f.buffer.get(); }
        else if (fades(g) || folderContentFill(g) < 1) f.before = std::make_unique<Img>(*cur);
        frames.push_back(std::move(f));
    }

    void closeGroup(const Layer& g, Img*& cur) {
        if (frames.empty() || frames.back().group != &g) return;
        Frame f = std::move(frames.back());
        frames.pop_back();
        // The contents at the folder's opacity and, when it has a style, its Fill (the effects are drawn after).
        const float opacity = float(clamp(g.opacity, 0.0, 1.0)) * folderContentFill(g);
        // Blend Interior Effects as Group: the overlays and satins join the folder's result before its mode blends it.
        bool foldsInteriors = false;
        if constexpr (O::styles) {
            auto style = f.buffer ? layerStyleOf(g, document) : nullptr;
            foldsInteriors = style && style->blendInteriorAsGroup;
        }
        if (foldsInteriors) drawGroupStyle(g, *f.buffer, StyledDraw::Phase::InteriorOverlays);
        if (f.buffer) {
            cur = f.parent;
            // A folder's Blend If gates its result against what is under the folder.
            const std::optional<BlendIf> blendIf = layerBlendIf(g, document.colorMode);
            std::optional<Img> before;
            if (blendIf) before.emplace(*cur);
            compositeRows(*f.buffer, blendOf(g), opacity, nullptr, *cur);
            if (blendIf) applyGate(BlendIfGate(*blendIf, document.colorMode), *f.buffer, *before, *cur);
        } else if (f.before) {
            // Pass Through below full opacity: the result fades back toward what was under the folder.
            parallelRows(0, outHeight, [&](int ya, int yb) {
                for (int y = ya; y < yb; y++) {
                    const Sample* b = f.before->row(y);
                    Sample* d = cur->row(y);
                    for (int i = 0; i < outWidth * N; i++) d[i] = O::fade(b[i], d[i], opacity);
                }
            });
        }
        if (O::styles && layerStyleOf(g, document)) drawGroupStyle(g, *cur, foldsInteriors ? StyledDraw::Phase::InteriorRest : StyledDraw::Phase::Interior);
    }

    void drawGroupStyle(const Layer& group, Img& out, StyledDraw::Phase phase) {
        if constexpr (O::styles) drawGroupStyleWith(group, out, phase);
    }

    void drawGroupStyleWith(const Layer& group, Img& out, StyledDraw::Phase phase) requires O::styles {
        auto style = layerStyleOf(group, document);
        if (!style) return;
        StyledDraw draw;
        draw.style = style;
        draw.region = region;
        draw.scale = scale;
        draw.phase = phase;
        layerOpacities(group, draw.master, draw.fill);
        draw.fill = 1;
        if (isolates(group) || fades(group)) draw.master = 1;
        auto folders = foldersCoverage(group.parentId);
        O::setStyleCoverage(draw, folders.get());
        draw.patterns = documentPatterns(document);
        draw.documentWidth = document.width;
        draw.documentHeight = document.height;
        if constexpr (requires { O::setStyleMode(draw, document); }) O::setStyleMode(draw, document);
        O::setStyleSource(draw, [&](Img& into, const Rect& area) {
            RenderPlan subPlan(document, overrides);
            subPlan.buildWithin(plan, group.id);
            RenderExecDeep sub(subPlan, area, scale, into.width(), into.height());
            sub.drawRange(into, 0, sub.order.size());
        });
        O::drawStyled(draw, out);
    }

    Cover folderMask(const Layer& group) {
        auto it = folderCoverage.find(group.id);
        if (it != folderCoverage.end()) return it->second;
        Cover result;
        if (group.mask && group.mask->enabled && group.mask->asset.image) {
            result = std::make_shared<Gray>(outWidth, outHeight, 0);
            O::sampleMask(maskOf(group), transformOf(group), region, scale, *result);
        }
        if (group.artboard) result = multipliedDeep<O>(result, artboardCoverage(*group.artboard));
        folderCoverage[group.id] = result;
        return result;
    }

    Cover foldersCoverage(const std::optional<Uuid>& parent) {
        auto it = chainCoverage.find(parent);
        if (it != chainCoverage.end()) return it->second;
        Cover result;
        std::optional<Uuid> current = parent;
        int depth = 0;
        while (current && depth < 64) {
            auto lit = byId.find(*current);
            if (lit == byId.end()) break;
            auto mask = folderMask(*lit->second);
            if (mask) result = result ? multipliedDeep<O>(result, mask) : std::make_shared<Gray>(*mask);
            current = lit->second->parentId;
            depth++;
        }
        chainCoverage[parent] = result;
        return result;
    }

    Cover artboardCoverage(const Artboard& a) const {
        auto result = std::make_shared<Gray>(outWidth, outHeight, 0);
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
            Sample* row = result->row(y);
            for (int x = 0; x < outWidth; x++) row[x] = O::coverage(cx[size_t(x)] * cy[size_t(y)]);
        }
        return result;
    }

    void drawArtboardBackground(const Layer& group, Img& out) {
        uint8_t colour8[4];
        group.artboard->fill(colour8);
        if (!colour8[3]) return;
        Sample colour[N];
        solid(colour8, colour);
        auto clip = multipliedDeep<O>(foldersCoverage(group.parentId), artboardCoverage(*group.artboard));
        parallelRows(0, outHeight, [&](int ya, int yb) {
            for (int y = ya; y < yb; y++) {
                const Sample* c = clip->row(y);
                Sample* d = out.row(y);
                for (int x = 0; x < outWidth; x++) if (c[x]) O::pixelCovered(BlendMode::Normal, colour, c[x], d + x * N);
            }
        });
    }

    Params paramsFor(const Layer& layer, const ImagePtr& image) const {
        Params params;
        params.image = image;
        params.transform = transformOf(layer);
        params.opacity = layer.opacity;
        params.mode = blendOf(layer);
        params.layerTransformForMask = params.transform;
        if (layer.mask) {
            params.mask = &*layer.mask;
            auto* o = over(layer.id);
            if (o && o->maskPlacement) params.maskPlacement = *o->maskPlacement;
            std::optional<GrayPtr> overridden = o ? O::overrideMask(*o) : std::nullopt;
            params.maskImage = overridden ? *overridden : maskOf(layer);
        }
        return params;
    }

    /// A gradient or pattern fill layer's contents at the document's depth (vectormask.h), kept while its carry lives.
    ImagePtr fillImage(const Layer& layer) {
        static FillLayerCache<ImagePtr> cache;
        if (!layer.psdCarry) return nullptr;
        if (auto cached = cache.find(layer.psdCarry, document.width, document.height)) return *cached;
        ImagePtr image = O::fillLayer(layer, document);
        cache.insert(layer.psdCarry, document.width, document.height, image);
        return image;
    }

    /// A vector mask's coverage over `area`, with the mask's density and feather when it has them.
    Cover vectorCut(const VectorPath& path, const std::optional<MaskParameters>& parameters, const Rect& area, int w, int h) const {
        auto cut = O::vectorMask(path, area, scale, w, h);
        if (parameters) O::maskParameters(*cut, parameters->vectorDensity, parameters->vectorFeather, scale);
        return cut;
    }

    void drawOwn(const Layer& layer, Img& target, const Gray* coverage) {
        if (!plainOnly && !ungated) if (auto blendIf = layerBlendIf(layer, document.colorMode)) {
            const Img before = target;
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
        [[maybe_unused]] std::function<void()>* const gateInside = std::exchange(interiorGate, nullptr);
        ImagePtr image = imageOf(layer);
        if (!image) image = fillImage(layer);
        if (!image) return;
        Params params = paramsFor(layer, image);
        if (!image->isEmpty() && !layer.asset) params.transform = LayerTransform(Point(0, 0), Size(document.width, document.height));
        const Gray* coverageWithoutVector = coverage;
        const std::optional<MaskParameters> maskParameters = layer.psdCarry ? parseMaskParameters(layer.psdCarry->maskData) : std::nullopt;
        std::optional<VectorPath> vector = layerVectorMask(layer, document);
        Cover cut;
        if (vector) {
            cut = vectorCut(*vector, maskParameters, region, outWidth, outHeight);
            if (coverage) {
                const size_t n = size_t(outWidth) * size_t(outHeight);
                for (size_t i = 0; i < n; i++) cut->data()[i] = O::mul(cut->data()[i], coverage->data()[i]);
            }
            coverage = cut.get();
        }
        // A pixel mask with Photoshop's density or feather.
        Cover userCut;
        if (maskParameters && (maskParameters->userDensity || maskParameters->userFeather) && layer.mask && layer.mask->enabled && layer.mask->asset.image) {
            userCut = std::make_shared<Gray>(outWidth, outHeight, 0);
            O::sampleMask(maskOf(layer), layer.maskTransform(), region, scale, *userCut);
            O::maskParameters(*userCut, maskParameters->userDensity, maskParameters->userFeather, scale, true);
            if (coverage) {
                const size_t n = size_t(outWidth) * size_t(outHeight);
                for (size_t i = 0; i < n; i++) userCut->data()[i] = O::mul(userCut->data()[i], coverage->data()[i]);
            }
            coverage = userCut.get();
            params.mask = nullptr;
            params.maskImage = nullptr;
        }
        const std::optional<VectorStroke> stroke = vector ? layerVectorStroke(layer) : std::nullopt;
        auto drawStroke = [&] {
            if (!stroke || !stroke->enabled || stroke->opacity <= 0) return;
            VectorPath path = *vector;
            path.inverted = false;
            auto band = O::vectorStroke(path, *stroke, region, scale, outWidth, outHeight);
            if (maskParameters && maskParameters->vectorFeather) O::maskParameters(*band, std::nullopt, maskParameters->vectorFeather, scale);
            const float opacity = float(clamp(layer.opacity, 0.0, 1.0)) * stroke->opacity;
            const BlendMode mode = blendOf(layer);
            ImagePtr paint;
            if (stroke->paint.kind != VectorPaint::Kind::Solid) {
                double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
                for (auto& s : path.subpaths) for (auto& k : s.knots) for (auto [px, py] : {std::pair{k.x, k.y}, {k.inX, k.inY}, {k.outX, k.outY}}) {
                    x0 = std::min(x0, px); x1 = std::max(x1, px); y0 = std::min(y0, py); y1 = std::max(y1, py);
                }
                paint = O::vectorPaint(stroke->paint, document, x1 > x0 ? Rect(x0, y0, x1 - x0, y1 - y0) : Rect(), region, scale, outWidth, outHeight);
            }
            const uint8_t stroke8[4] = {stroke->r, stroke->g, stroke->b, 255};
            Sample strokeColour[N];
            solid(stroke8, strokeColour);
            parallelRows(0, outHeight, [&](int ya, int yb) {
                for (int y = ya; y < yb; y++) {
                    const Sample* b = band->row(y);
                    const Sample* c = coverageWithoutVector ? coverageWithoutVector->row(y) : nullptr;
                    const Sample* p = paint ? paint->row(y) : nullptr;
                    Sample* d = target.row(y);
                    for (int x = 0; x < outWidth; x++) {
                        if (!b[x]) continue;
                        Sample colour[N];
                        std::memcpy(colour, strokeColour, sizeof colour);
                        float alpha = 1;
                        if (p) {
                            const Sample* q = p + x * N;
                            if (!q[C]) continue;
                            for (int k = 0; k < C; k++) colour[k] = O::unpremultiply(q[k], q[C]);
                            alpha = O::unit(q[C]);
                        }
                        O::pixelAt(mode, colour, O::unit(b[x]) * opacity * alpha * (c ? O::unit(c[x]) : 1.0f), d + x * N, docX(region, scale, x), docY(region, scale, y));
                    }
                }
            });
        };
        if (stroke && stroke->enabled && !stroke->fillEnabled) { drawStroke(); return; }
        if constexpr (O::styles) if (!plainOnly) if (auto style = layerStyleOf(layer, document)) {
            // Layer styles at the document's depth (layerstyle_render.h).
            StyledDraw draw;
            draw.style = style;
            draw.region = region;
            draw.scale = scale;
            draw.mode = params.mode;
            layerOpacities(layer, draw.master, draw.fill);
            O::setStyleCoverage(draw, vector ? coverageWithoutVector : coverage);
            draw.patterns = documentPatterns(document);
            draw.documentWidth = document.width;
            draw.documentHeight = document.height;
            draw.bounds = params.transform.bounds();
            if constexpr (requires { O::setStyleMode(draw, document); }) O::setStyleMode(draw, document);
            // Blend If gates the pixels before the interior effects land. With Blend Interior Effects as Group the
            // interiors join the pixels and are gated with them; a shape's stroke, drawn after the effects, keeps
            // the gate over the whole draw.
            if (gateInside && !style->blendInteriorAsGroup && !(stroke && stroke->enabled)) draw.afterContent = *gateInside;
            O::setStyleSource(draw, [&](Img& into, const Rect& area) {
                Params plain = params;
                plain.opacity = 1;
                plain.mode = BlendMode::Normal;
                // The vector mask shapes what the effects are drawn around, as the pixel mask does.
                Cover shape = vector ? Cover(O::vectorMask(*vector, area, scale, into.width(), into.height())) : nullptr;
                O::drawLayer(plain, area, scale, shape.get(), into);
            });
            O::drawStyled(draw, target);
            drawStroke();
            return;
        }
        if constexpr (std::is_same_v<Img, Image16>)
            if (!plainOnly && N == 4 && params.mode == BlendMode::Normal && document.colorMode == ColorMode::RGB && photoshopType(layer)) {
                // Type in Normal mode blends its colours with Photoshop's text gamma (render_plan.h).
                Img own = blank(outWidth, outHeight);
                O::drawLayer(params, region, scale, coverage, own);
                blendTextGamma(own, target);
                drawStroke();
                return;
            }
        O::drawLayer(params, region, scale, coverage, target);
        drawStroke();
    }

    Cover coverageOf(const Uuid& id) {
        auto it = liveCoverage.find(id);
        if (it != liveCoverage.end()) return it->second;
        if (visiting.count(id) || visiting.size() >= 256) return nullptr;
        auto lit = byId.find(id);
        if (lit == byId.end()) return nullptr;
        visiting.insert(id);
        Img pixels = blank(outWidth, outHeight);
        const bool wasPlain = plainOnly;
        plainOnly = true;
        drawClipped(*lit->second, pixels, nullptr);
        plainOnly = wasPlain;
        visiting.erase(id);
        auto gray = std::make_shared<Gray>(outWidth, outHeight);
        const size_t n = size_t(outWidth) * size_t(outHeight);
        for (size_t i = 0; i < n; i++) gray->data()[i] = pixels.data()[i * N + C];
        liveCoverage[id] = gray;
        return gray;
    }

    void drawClipped(const Layer& layer, Img& target, Cover coverage) {
        if (layer.maskSourceId) {
            auto source = coverageOf(*layer.maskSourceId);
            if (!source) return;
            coverage = multipliedDeep<O>(coverage, source);
        }
        drawOwn(layer, target, coverage.get());
    }

    void adjust(const Layer& layer, Img& target, Cover coverage) {
        if (!layer.adjustment) return;
        // The adjustment at the document's depth (adjustments_u16.cpp at 16 bits).
        Img adjusted = target;
        if constexpr (requires { O::adjust(document, *layer.adjustment, adjusted, region, scale); }) {
            if (!O::adjust(document, *layer.adjustment, adjusted, region, scale)) return;
        } else if (!O::adjust(*layer.adjustment, adjusted, region, scale)) return;
        const BlendMode mode = blendOf(layer);
        const float opacity = float(clamp(layer.opacity, 0.0, 1.0));
        Cover clip = coverage;
        if (layer.mask && layer.mask->enabled && layer.mask->asset.image) {
            auto own = std::make_shared<Gray>(outWidth, outHeight, 0);
            O::sampleMask(maskOf(layer), transformOf(layer), region, scale, *own);
            clip = multipliedDeep<O>(clip, own);
        }
        // Blend If: This Layer reads the adjusted colours, Underlying Layer the colours before the adjustment.
        const std::optional<BlendIf> blendIf = layerBlendIf(layer, document.colorMode);
        std::optional<BlendIfGate> gate;
        if (blendIf) gate.emplace(*blendIf, document.colorMode);
        // The loop twice: with the gates and, for every other adjustment layer, without them.
        auto rows = [&](auto gated) {
            parallelRows(0, outHeight, [&](int ya, int yb) {
            for (int y = ya; y < yb; y++) {
                Sample* d = target.row(y);
                const Sample* a = adjusted.row(y);
                const Sample* c = clip ? clip->row(y) : nullptr;
                for (int x = 0; x < outWidth; x++, d += N, a += N) {
                    float mix = opacity * (c ? O::unit(c[x]) : 1.0f);
                    if (mix <= 0 || d[C] == 0) continue;
                    if constexpr (decltype(gated)::value) {
                        int sc[N], bc[N];
                        for (int k = 0; k < C; k++) { sc[k] = level(a[k], d[C]); bc[k] = level(d[k], d[C]); }
                        mix *= gate->sourceFactor(sc) * gate->underFactor(bc, std::min(1.0f, O::unit(d[C])));
                        if (mix <= 0) continue;
                    }
                    const float ab = O::unit(d[C]);
                    float cb[C], cs[C];
                    for (int k = 0; k < C; k++) { cb[k] = O::unit(d[k]) / ab; cs[k] = O::unit(a[k]) / ab; }
                    O::blendStraight(mode, cb, cs);
                    for (int k = 0; k < C; k++) {
                        const float v = (cb[k] + (cs[k] - cb[k]) * mix) * ab;
                        d[k] = O::store(v, d[C]);
                    }
                }
            }
            });
        };
        if (gate) rows(std::true_type{});
        else rows(std::false_type{});
    }

    void drawComposite(const Layer& layer, Img& out) {
        if (layer.isGroup) { if (layer.artboard) drawArtboardBackground(layer, out); return; }
        if (stacked.count(layer.id)) return;
        Cover folders = foldersCoverage(layer.parentId);
        if (layer.adjustment) {
            if (!layer.maskSourceId) adjust(layer, out, folders);
            return;
        }
        auto stack = stacks.find(layer.id);
        if (stack == stacks.end()) { drawClipped(layer, out, folders); return; }
        if (O::styles && !plainOnly && layerStyleOf(layer, document)) {
            drawOwn(layer, out, folders.get());
            auto clip = multipliedDeep<O>(folders, coverageOf(layer.id));
            for (auto& childId : stack->second) {
                auto it = byId.find(childId);
                if (it == byId.end()) continue;
                if (it->second->adjustment) adjust(*it->second, out, clip);
                else drawOwn(*it->second, out, clip.get());
            }
            return;
        }
        // A clipping stack: the base's alpha is shared by the layers clipped to it.
        Img group = blank(outWidth, outHeight);
        // A base with Blend If: its gate applies to the clipped result against the backdrop, so it does not shrink
        // the shape the clipped layers take.
        const std::optional<BlendIf> blendIf = plainOnly ? std::nullopt : layerBlendIf(layer, document.colorMode);
        const bool wasUngated = ungated;
        if (blendIf) ungated = true;
        drawOwn(layer, group, nullptr);
        ungated = wasUngated;
        const size_t n = size_t(outWidth) * size_t(outHeight);
        std::vector<Sample> alpha(n);
        for (size_t i = 0; i < n; i++) {
            Sample* p = group.data() + i * N;
            alpha[i] = p[C];
            const Sample a = p[C];
            for (int c = 0; c < C; c++) p[c] = a ? O::unpremultiply(p[c], a) : Sample(0);
            p[C] = O::one;
        }
        for (auto& childId : stack->second) {
            auto it = byId.find(childId);
            if (it == byId.end()) continue;
            if (it->second->adjustment) adjust(*it->second, group, nullptr);
            else drawOwn(*it->second, group, nullptr);
        }
        for (size_t i = 0; i < n; i++) {
            Sample* p = group.data() + i * N;
            const Sample a = alpha[i];
            for (int c = 0; c < C; c++) p[c] = O::mul(p[c], a);
            p[C] = a;
        }
        std::optional<Img> before;
        if (blendIf) before.emplace(out);
        compositeRows(group, blendOf(layer), 1.0f, folders.get(), out);
        if (blendIf) applyGate(BlendIfGate(*blendIf, document.colorMode), ownColours(layer), *before, out);
    }

    void drawRange(Img& out, size_t from, size_t to) {
        Img* cur = &out;
        for (size_t index = from; index < to; index++) {
            if (auto open = groupsOpen.find(index); open != groupsOpen.end())
                for (const Layer* g : open->second) openGroup(*g, cur);
            drawComposite(*order[index], *cur);
            if (auto close = groupsClose.find(index); close != groupsClose.end())
                for (const Layer* g : close->second) closeGroup(*g, cur);
        }
    }

    void runCached(Img& out, RenderCache& cache, uint64_t version, size_t edited) {
        const Layer& layer = *order[edited];
        std::shared_ptr<Img>& backdrop = O::backdrop(cache);
        std::shared_ptr<Img>& above = O::above(cache);
        const bool valid = backdrop && cache.version == version && cache.layer == layer.id && cache.width == outWidth && cache.height == outHeight
            && cache.region == region && cache.scale == scale;
        if (!valid) {
            cache.version = version; cache.layer = layer.id; cache.width = outWidth; cache.height = outHeight; cache.region = region; cache.scale = scale;
            O::resetOtherDepths(cache);
            backdrop = std::make_shared<Img>(blank(outWidth, outHeight));
            drawRange(*backdrop, 0, edited);
            above.reset();
            cache.aboveFlat = true;
            for (size_t i = edited + 1; i < order.size(); i++) if (!plan.plainAbove(*order[i])) { cache.aboveFlat = false; break; }
            if (cache.aboveFlat && edited + 1 < order.size()) {
                above = std::make_shared<Img>(blank(outWidth, outHeight));
                drawRange(*above, edited + 1, order.size());
            }
        }
        std::memcpy(out.data(), backdrop->data(), out.byteCount());
        drawComposite(layer, out);
        if (!cache.aboveFlat) { drawRange(out, edited + 1, order.size()); return; }
        if (above) compositeRows(*above, BlendMode::Normal, 1.0f, nullptr, out);
    }

    void run(Img& out, RenderCache* cache, uint64_t version) {
        const size_t edited = cache ? plan.editedIndex() : SIZE_MAX;
        if (edited != SIZE_MAX) runCached(out, *cache, version, edited);
        else drawRange(out, 0, order.size());
    }
};

} // namespace compositor
