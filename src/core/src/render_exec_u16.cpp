// The renderer's pixels at 16 bits (render_plan.h): the 8-bit executor's structure at Photoshop's 0..32768, with
// coverage (masks, folder masks, clipping) at the same depth.
//
// What is drawn natively: pixel layers in every blend mode with opacity, pixel and vector masks, clipping stacks,
// folders (Pass Through, faded and isolated), artboards, shape strokes and adjustment layers. What has no 16-bit
// path yet is drawn at 8 bits and applied as a difference, so the pixels it does not touch keep their full
// precision: layer styles (P8 ports them; docs/bit-depth.md).
#include "render_plan.h"
#include "compositor/layerstyle.h"
#include "layerstyle_render.h"
#include "compositor/vectormask.h"
#include "compositor/adjustments.h"
#include "compositor/blend.h"
#include "compositor/depth.h"
#include "compositor/parallel.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <mutex>
#include <tuple>
#include <map>
#include <optional>
#include <set>

namespace compositor {

namespace {

using Cover = std::shared_ptr<Gray16>;

std::shared_ptr<Gray16> multiplied(const std::shared_ptr<Gray16>& a, const std::shared_ptr<Gray16>& b) {
    if (!a) return b;
    if (!b) return a;
    auto r = std::make_shared<Gray16>(*a);
    const size_t n = size_t(r->width()) * size_t(r->height());
    for (size_t i = 0; i < n; i++) r->data()[i] = uint16_t(mul15(r->data()[i], b->data()[i]));
    return r;
}

/// A buffer held at 8 bits in a 16-bit document (something written before its conversion reached it): widened once
/// per buffer and kept while the buffer lives.
template <class Wide, class Narrow>
std::shared_ptr<const Wide> widenedOnce(const std::shared_ptr<const Narrow>& narrow) {
    static std::mutex m;
    static std::vector<std::pair<std::weak_ptr<const Narrow>, std::shared_ptr<const Wide>>> cache;
    if (!narrow) return nullptr;
    std::lock_guard<std::mutex> lock(m);
    for (auto it = cache.begin(); it != cache.end();) {
        if (it->first.expired()) { it = cache.erase(it); continue; }
        if (it->first.lock() == narrow) return it->second;
        ++it;
    }
    std::shared_ptr<const Wide> wide;
    if constexpr (std::is_same_v<Wide, Image16>) wide = widenImage(*narrow); else wide = widenGray(*narrow);
    if (cache.size() > 64) cache.erase(cache.begin());
    cache.emplace_back(narrow, wide);
    return wide;
}

Image16Ptr wideImage(const AnyImage& image) {
    if (image.u16()) return image.u16();
    return widenedOnce<Image16>(image.u8());
}

Gray16Ptr wideGray(const AnyGray& image) {
    if (image.u16()) return image.u16();
    return widenedOnce<Gray16>(image.u8());
}

/// Draws with 8-bit code onto a 16-bit target: `draw` gets the target reduced to 8 bits, and what it changed is
/// added back at 16 bits. Pixels it leaves alone keep every bit.
void via8(Image16& target, const std::function<void(Image&)>& draw) {
    Image before(target.width(), target.height());
    narrowInto(target, before);
    Image after = before;
    draw(after);
    parallelRows(0, target.height(), [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            const uint8_t* b = before.row(y);
            const uint8_t* a = after.row(y);
            uint16_t* d = target.row(y);
            for (int x = 0; x < target.width(); x++, a += 4, b += 4, d += 4) {
                if (std::memcmp(a, b, 4) == 0) continue;
                int32_t v[4];
                for (int c = 0; c < 4; c++) v[c] = std::clamp(int32_t(d[c]) + int32_t(widen8(a[c])) - int32_t(widen8(b[c])), 0, int32_t(one16));
                for (int c = 0; c < 3; c++) d[c] = uint16_t(std::min(v[c], v[3]));
                d[3] = uint16_t(v[3]);
            }
        }
    });
}

} // namespace

template <>
struct RenderExec<SampleType::U16> {
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
    RenderExec(const RenderPlan& p, Rect r, double s, int w, int h)
        : plan(p), document(p.document), overrides(p.overrides), byId(p.byId), order(p.order), stacks(p.stacks), stacked(p.stacked),
          groupsOpen(p.groupsOpen), groupsClose(p.groupsClose), region(r), scale(s), outWidth(w), outHeight(h) {}
    std::map<Uuid, Cover> liveCoverage;
    std::set<Uuid> visiting;
    bool plainOnly = false;

    static bool isolates(const Layer& g) { return RenderPlan::isolates(g); }
    static bool fades(const Layer& g) { return RenderPlan::fades(g); }

    struct Frame { const Layer* group; std::unique_ptr<Image16> buffer; std::unique_ptr<Image16> before; Image16* parent; };
    std::vector<Frame> frames;
    std::map<Uuid, Cover> folderCoverage;
    std::map<std::optional<Uuid>, Cover> chainCoverage;

    const LayerOverride* over(const Uuid& id) const { return plan.over(id); }
    LayerTransform transformOf(const Layer& l) const { return plan.transformOf(l); }
    BlendMode blendOf(const Layer& l) const { return plan.blendOf(l); }
    Image16Ptr imageOf(const Layer& l) const {
        auto* o = over(l.id);
        if (o && o->image16) return *o->image16;
        if (o && o->image) return widenedOnce<Image16>(*o->image);
        return l.asset ? wideImage(l.asset->image) : nullptr;
    }
    static Gray16Ptr maskOf(const Layer& l) { return l.mask ? wideGray(l.mask->asset.image) : nullptr; }

    void compositeRows(const Image16& src, BlendMode mode, float opacity, const Gray16* coverage, Image16& dst) {
        parallelRows(0, outHeight, [&](int ya, int yb) {
            std::vector<uint32_t> steps(static_cast<size_t>(outWidth));
            for (int y = ya; y < yb; y++) {
                const uint16_t* s = src.row(y);
                uint16_t* d = dst.row(y);
                const uint16_t* c = coverage ? coverage->row(y) : nullptr;
                if (mode == BlendMode::Dissolve) {
                    for (int x = 0; x < outWidth; x++)
                        if (s[x * 4 + 3]) compositePixelAt16(mode, s + x * 4, opacity * (c ? c[x] / 32768.0f : 1.0f), d + x * 4, docX(region, scale, x), docY(region, scale, y));
                    continue;
                }
                for (int x = 0; x < outWidth; x++) steps[size_t(x)] = s[x * 4 + 3] ? coverageSteps16(opacity * (c ? c[x] / 32768.0f : 1.0f)) : 0;
                compositeSpan16(mode, s, steps.data(), d, outWidth);
            }
        });
    }

    void openGroup(const Layer& g, Image16*& cur) {
        if (layerStyleOf(g, document)) drawGroupStyle(g, *cur, StyledDraw::Phase::Exterior);
        Frame f{&g, nullptr, nullptr, cur};
        if (isolates(g)) { f.buffer = std::make_unique<Image16>(outWidth, outHeight); cur = f.buffer.get(); }
        else if (fades(g)) f.before = std::make_unique<Image16>(*cur);
        frames.push_back(std::move(f));
    }

    void closeGroup(const Layer& g, Image16*& cur) {
        if (frames.empty() || frames.back().group != &g) return;
        Frame f = std::move(frames.back());
        frames.pop_back();
        const float opacity = float(clamp(g.opacity, 0.0, 1.0));
        if (f.buffer) {
            cur = f.parent;
            compositeRows(*f.buffer, blendOf(g), opacity, nullptr, *cur);
        } else if (f.before) {
            // Pass Through below full opacity: the result fades back toward what was under the folder.
            parallelRows(0, outHeight, [&](int ya, int yb) {
                for (int y = ya; y < yb; y++) {
                    const uint16_t* b = f.before->row(y);
                    uint16_t* d = cur->row(y);
                    for (int i = 0; i < outWidth * 4; i++) d[i] = uint16_t(std::lround(b[i] + (float(d[i]) - float(b[i])) * opacity));
                }
            });
        }
        if (layerStyleOf(g, document)) drawGroupStyle(g, *cur, StyledDraw::Phase::Interior);
    }

    void drawGroupStyle(const Layer& group, Image16& out, StyledDraw::Phase phase) {
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
        std::shared_ptr<GrayImage> folders8 = folders ? narrowGray(*folders) : nullptr;
        draw.coverage = folders8.get();
        draw.patterns = documentPatterns(document);
        draw.documentWidth = document.width;
        draw.documentHeight = document.height;
        draw.drawSource = [&](Image& into, const Rect& area) {
            RenderPlan subPlan(document, overrides);
            subPlan.buildWithin(plan, group.id);
            RenderExec sub(subPlan, area, scale, into.width(), into.height());
            Image16 deep(into.width(), into.height());
            sub.drawRange(deep, 0, sub.order.size());
            narrowInto(deep, into);
        };
        via8(out, [&](Image& eight) { drawStyledLayer(draw, eight); });
    }

    Cover folderMask(const Layer& group) {
        auto it = folderCoverage.find(group.id);
        if (it != folderCoverage.end()) return it->second;
        Cover result;
        if (group.mask && group.mask->enabled && group.mask->asset.image) {
            result = std::make_shared<Gray16>(outWidth, outHeight, 0);
            sampleMaskCoverage(maskOf(group), transformOf(group), region, scale, 0, *result, false);
        }
        if (group.artboard) result = multiplied(result, artboardCoverage(*group.artboard));
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
            if (mask) result = result ? multiplied(result, mask) : std::make_shared<Gray16>(*mask);
            current = lit->second->parentId;
            depth++;
        }
        chainCoverage[parent] = result;
        return result;
    }

    Cover artboardCoverage(const Artboard& a) const {
        auto result = std::make_shared<Gray16>(outWidth, outHeight, 0);
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
            uint16_t* row = result->row(y);
            for (int x = 0; x < outWidth; x++) row[x] = uint16_t(std::lround(cx[size_t(x)] * cy[size_t(y)] * 32768));
        }
        return result;
    }

    void drawArtboardBackground(const Layer& group, Image16& out) {
        uint8_t colour8[4];
        group.artboard->fill(colour8);
        if (!colour8[3]) return;
        const uint16_t colour[4] = {widen8(colour8[0]), widen8(colour8[1]), widen8(colour8[2]), widen8(colour8[3])};
        auto clip = multiplied(foldersCoverage(group.parentId), artboardCoverage(*group.artboard));
        parallelRows(0, outHeight, [&](int ya, int yb) {
            for (int y = ya; y < yb; y++) {
                const uint16_t* c = clip->row(y);
                uint16_t* d = out.row(y);
                for (int x = 0; x < outWidth; x++) if (c[x]) compositePixelSteps16(BlendMode::Normal, colour, c[x], d + x * 4);
            }
        });
    }

    DrawParams16 paramsFor(const Layer& layer, const Image16Ptr& image) const {
        DrawParams16 params;
        params.image = image;
        params.transform = transformOf(layer);
        params.opacity = layer.opacity;
        params.mode = blendOf(layer);
        params.layerTransformForMask = params.transform;
        if (layer.mask) {
            params.mask = &*layer.mask;
            auto* o = over(layer.id);
            if (o && o->maskPlacement) params.maskPlacement = *o->maskPlacement;
            if (o && o->maskImage16) params.maskImage = *o->maskImage16;
            else if (o && o->maskImage) params.maskImage = widenedOnce<Gray16>(*o->maskImage);
            else params.maskImage = maskOf(layer);
        }
        return params;
    }

    /// A gradient or pattern fill layer's contents, drawn at 8 bits (vectormask.h) and widened, kept while its carry lives.
    Image16Ptr fillImage(const Layer& layer) {
        static std::mutex m;
        static std::vector<std::tuple<std::weak_ptr<const PsdLayerCarry>, int, int, Image16Ptr>> cache;
        if (!layer.psdCarry) return nullptr;
        std::lock_guard<std::mutex> lock(m);
        for (auto& [carry, w, h, image] : cache)
            if (carry.lock() == layer.psdCarry && w == document.width && h == document.height) return image;
        ImagePtr eight = renderFillLayer(layer, document);
        Image16Ptr image = eight ? Image16Ptr(widenImage(*eight)) : nullptr;
        if (cache.size() > 32) cache.erase(cache.begin());
        cache.emplace_back(layer.psdCarry, document.width, document.height, image);
        return image;
    }

    void drawOwn(const Layer& layer, Image16& target, const Gray16* coverage) {
        Image16Ptr image = imageOf(layer);
        if (!image) image = fillImage(layer);
        if (!image) return;
        DrawParams16 params = paramsFor(layer, image);
        if (!image->isEmpty() && !layer.asset) params.transform = LayerTransform(Point(0, 0), Size(document.width, document.height));
        const Gray16* coverageWithoutVector = coverage;
        const std::optional<MaskParameters> maskParameters = layer.psdCarry ? parseMaskParameters(layer.psdCarry->maskData) : std::nullopt;
        std::optional<VectorPath> vector = layerVectorMask(layer, document);
        Cover cut;
        if (vector) {
            auto cut8 = rasterizeVectorMask(*vector, region, scale, outWidth, outHeight);
            if (maskParameters) applyMaskParameters(*cut8, maskParameters->vectorDensity, maskParameters->vectorFeather, scale);
            cut = widenGray(*cut8);
            if (coverage) {
                const size_t n = size_t(outWidth) * size_t(outHeight);
                for (size_t i = 0; i < n; i++) cut->data()[i] = uint16_t(mul15(cut->data()[i], coverage->data()[i]));
            }
            coverage = cut.get();
        }
        // A pixel mask with Photoshop's density or feather: those are worked out at 8 bits.
        Cover userCut;
        if (maskParameters && (maskParameters->userDensity || maskParameters->userFeather) && layer.mask && layer.mask->enabled && layer.mask->asset.image) {
            Gray16 sampled(outWidth, outHeight, 0);
            sampleMaskCoverage(maskOf(layer), layer.maskTransform(), region, scale, 0, sampled, false);
            auto eight = narrowGray(sampled);
            applyMaskParameters(*eight, maskParameters->userDensity, maskParameters->userFeather, scale, true);
            userCut = widenGray(*eight);
            if (coverage) {
                const size_t n = size_t(outWidth) * size_t(outHeight);
                for (size_t i = 0; i < n; i++) userCut->data()[i] = uint16_t(mul15(userCut->data()[i], coverage->data()[i]));
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
            auto band = rasterizeVectorStroke(path, *stroke, region, scale, outWidth, outHeight);
            if (maskParameters && maskParameters->vectorFeather) applyMaskParameters(*band, std::nullopt, maskParameters->vectorFeather, scale);
            const float opacity = float(clamp(layer.opacity, 0.0, 1.0)) * stroke->opacity;
            const BlendMode mode = blendOf(layer);
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
                    const uint16_t* c = coverageWithoutVector ? coverageWithoutVector->row(y) : nullptr;
                    const uint8_t* p = paint ? paint->row(y) : nullptr;
                    uint16_t* d = target.row(y);
                    for (int x = 0; x < outWidth; x++) {
                        if (!b[x]) continue;
                        uint16_t colour[4] = {widen8(stroke->r), widen8(stroke->g), widen8(stroke->b), uint16_t(one16)};
                        float alpha = 1;
                        if (p) {
                            const uint8_t* q = p + x * 4;
                            if (!q[3]) continue;
                            for (int k = 0; k < 3; k++) colour[k] = uint16_t(std::min<uint32_t>(one16, (uint32_t(q[k]) * one16 + q[3] / 2) / q[3]));
                            alpha = q[3] / 255.0f;
                        }
                        compositePixelAt16(mode, colour, b[x] / 255.0f * opacity * alpha * (c ? c[x] / 32768.0f : 1.0f), d + x * 4, docX(region, scale, x), docY(region, scale, y));
                    }
                }
            });
        };
        if (stroke && stroke->enabled && !stroke->fillEnabled) { drawStroke(); return; }
        if (!plainOnly) if (auto style = layerStyleOf(layer, document)) {
            // Layer styles are drawn at 8 bits (layerstyle_render.h) and applied as a difference.
            StyledDraw draw;
            draw.style = style;
            draw.region = region;
            draw.scale = scale;
            draw.mode = params.mode;
            layerOpacities(layer, draw.master, draw.fill);
            const Gray16* styleCoverage = vector ? coverageWithoutVector : coverage;
            std::shared_ptr<GrayImage> coverage8 = styleCoverage ? narrowGray(*styleCoverage) : nullptr;
            draw.coverage = coverage8.get();
            draw.patterns = documentPatterns(document);
            draw.documentWidth = document.width;
            draw.documentHeight = document.height;
            draw.bounds = params.transform.bounds();
            draw.drawSource = [&](Image& into, const Rect& area) {
                DrawParams16 plain = params;
                plain.opacity = 1;
                plain.mode = BlendMode::Normal;
                std::shared_ptr<GrayImage> shape = vector ? rasterizeVectorMask(*vector, area, scale, into.width(), into.height()) : nullptr;
                Cover shape16 = shape ? widenGray(*shape) : nullptr;
                Image16 deep(into.width(), into.height());
                drawLayer(plain, area, scale, shape16.get(), deep);
                narrowInto(deep, into);
            };
            via8(target, [&](Image& eight) { drawStyledLayer(draw, eight); });
            drawStroke();
            return;
        }
        drawLayer(params, region, scale, coverage, target);
        drawStroke();
    }

    Cover coverageOf(const Uuid& id) {
        auto it = liveCoverage.find(id);
        if (it != liveCoverage.end()) return it->second;
        if (visiting.count(id) || visiting.size() >= 256) return nullptr;
        auto lit = byId.find(id);
        if (lit == byId.end()) return nullptr;
        visiting.insert(id);
        Image16 pixels(outWidth, outHeight);
        const bool wasPlain = plainOnly;
        plainOnly = true;
        drawClipped(*lit->second, pixels, nullptr);
        plainOnly = wasPlain;
        visiting.erase(id);
        auto gray = std::make_shared<Gray16>(outWidth, outHeight);
        const size_t n = size_t(outWidth) * size_t(outHeight);
        for (size_t i = 0; i < n; i++) gray->data()[i] = pixels.data()[i * 4 + 3];
        liveCoverage[id] = gray;
        return gray;
    }

    void drawClipped(const Layer& layer, Image16& target, Cover coverage) {
        if (layer.maskSourceId) {
            auto source = coverageOf(*layer.maskSourceId);
            if (!source) return;
            coverage = multiplied(coverage, source);
        }
        drawOwn(layer, target, coverage.get());
    }

    void adjust(const Layer& layer, Image16& target, Cover coverage) {
        if (!layer.adjustment) return;
        // The adjustment at 16 bits (adjustments_u16.cpp).
        Image16 adjusted = target;
        if (!applyAdjustment(*layer.adjustment, adjusted, region, scale)) return;
        const BlendMode mode = blendOf(layer);
        const float opacity = float(clamp(layer.opacity, 0.0, 1.0));
        Cover clip = coverage;
        if (layer.mask && layer.mask->enabled && layer.mask->asset.image) {
            auto own = std::make_shared<Gray16>(outWidth, outHeight, 0);
            sampleMaskCoverage(maskOf(layer), transformOf(layer), region, scale, 0, *own, false);
            clip = multiplied(clip, own);
        }
        parallelRows(0, outHeight, [&](int ya, int yb) {
            for (int y = ya; y < yb; y++) {
                uint16_t* d = target.row(y);
                const uint16_t* a = adjusted.row(y);
                const uint16_t* c = clip ? clip->row(y) : nullptr;
                for (int x = 0; x < outWidth; x++, d += 4, a += 4) {
                    const float mix = opacity * (c ? c[x] / 32768.0f : 1.0f);
                    if (mix <= 0 || d[3] == 0) continue;
                    const float ab = d[3] / 32768.0f;
                    float cb[3], cs[3];
                    for (int k = 0; k < 3; k++) { cb[k] = d[k] / 32768.0f / ab; cs[k] = a[k] / 32768.0f / ab; }
                    if (mode != BlendMode::Normal) {
                        const Rgb m = blendColor(mode, {cb[0], cb[1], cb[2]}, {cs[0], cs[1], cs[2]});
                        cs[0] = m.r; cs[1] = m.g; cs[2] = m.b;
                    }
                    for (int k = 0; k < 3; k++) {
                        const float v = (cb[k] + (cs[k] - cb[k]) * mix) * ab;
                        d[k] = uint16_t(clamp(v * 32768.0f + 0.5f, 0.0f, float(d[3])));
                    }
                }
            }
        });
    }

    void drawComposite(const Layer& layer, Image16& out) {
        if (layer.isGroup) { if (layer.artboard) drawArtboardBackground(layer, out); return; }
        if (stacked.count(layer.id)) return;
        Cover folders = foldersCoverage(layer.parentId);
        if (layer.adjustment) {
            if (!layer.maskSourceId) adjust(layer, out, folders);
            return;
        }
        auto stack = stacks.find(layer.id);
        if (stack == stacks.end()) { drawClipped(layer, out, folders); return; }
        if (!plainOnly && layerStyleOf(layer, document)) {
            drawOwn(layer, out, folders.get());
            auto clip = multiplied(folders, coverageOf(layer.id));
            for (auto& childId : stack->second) {
                auto it = byId.find(childId);
                if (it == byId.end()) continue;
                if (it->second->adjustment) adjust(*it->second, out, clip);
                else drawOwn(*it->second, out, clip.get());
            }
            return;
        }
        // A clipping stack: the base's alpha is shared by the layers clipped to it.
        Image16 group(outWidth, outHeight);
        drawOwn(layer, group, nullptr);
        const size_t n = size_t(outWidth) * size_t(outHeight);
        std::vector<uint16_t> alpha(n);
        for (size_t i = 0; i < n; i++) {
            uint16_t* p = group.data() + i * 4;
            alpha[i] = p[3];
            const uint32_t a = p[3];
            for (int c = 0; c < 3; c++) p[c] = a ? uint16_t(std::min<uint32_t>(one16, (p[c] * one16 + a / 2) / a)) : 0;
            p[3] = uint16_t(one16);
        }
        for (auto& childId : stack->second) {
            auto it = byId.find(childId);
            if (it == byId.end()) continue;
            if (it->second->adjustment) adjust(*it->second, group, nullptr);
            else drawOwn(*it->second, group, nullptr);
        }
        for (size_t i = 0; i < n; i++) {
            uint16_t* p = group.data() + i * 4;
            const uint32_t a = alpha[i];
            for (int c = 0; c < 3; c++) p[c] = uint16_t(mul15(p[c], a));
            p[3] = uint16_t(a);
        }
        compositeRows(group, blendOf(layer), 1.0f, folders.get(), out);
    }

    void drawRange(Image16& out, size_t from, size_t to) {
        Image16* cur = &out;
        for (size_t index = from; index < to; index++) {
            if (auto open = groupsOpen.find(index); open != groupsOpen.end())
                for (const Layer* g : open->second) openGroup(*g, cur);
            drawComposite(*order[index], *cur);
            if (auto close = groupsClose.find(index); close != groupsClose.end())
                for (const Layer* g : close->second) closeGroup(*g, cur);
        }
    }

    void runCached(Image16& out, RenderCache& cache, uint64_t version, size_t edited) {
        const Layer& layer = *order[edited];
        const bool valid = cache.backdrop16 && cache.version == version && cache.layer == layer.id && cache.width == outWidth && cache.height == outHeight
            && cache.region == region && cache.scale == scale;
        if (!valid) {
            cache.version = version; cache.layer = layer.id; cache.width = outWidth; cache.height = outHeight; cache.region = region; cache.scale = scale;
            cache.backdrop.reset(); cache.above.reset();
            cache.backdrop16 = std::make_shared<Image16>(outWidth, outHeight);
            drawRange(*cache.backdrop16, 0, edited);
            cache.above16.reset();
            cache.aboveFlat = true;
            for (size_t i = edited + 1; i < order.size(); i++) if (!plan.plainAbove(*order[i])) { cache.aboveFlat = false; break; }
            if (cache.aboveFlat && edited + 1 < order.size()) {
                cache.above16 = std::make_shared<Image16>(outWidth, outHeight);
                drawRange(*cache.above16, edited + 1, order.size());
            }
        }
        std::memcpy(out.data(), cache.backdrop16->data(), out.byteCount());
        drawComposite(layer, out);
        if (!cache.aboveFlat) { drawRange(out, edited + 1, order.size()); return; }
        if (cache.above16) compositeRows(*cache.above16, BlendMode::Normal, 1.0f, nullptr, out);
    }

    void run(Image16& out, RenderCache* cache, uint64_t version) {
        const size_t edited = cache ? plan.editedIndex() : SIZE_MAX;
        if (edited != SIZE_MAX) runCached(out, *cache, version, edited);
        else drawRange(out, 0, order.size());
    }
};

template <>
void executeRender<SampleType::U16>(const RenderPlan& plan, const Rect& region, double scale, Image16& out, RenderCache* cache, uint64_t version) {
    RenderExec<SampleType::U16> exec(plan, region, scale, out.width(), out.height());
    exec.run(out, cache, version);
}

} // namespace compositor
