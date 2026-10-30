// What the renderer's two executors share (render_plan.h): the 8-bit RGB one (render_exec_u8.cpp) and the deep one
// (render_exec_deep.inc: 16 and 32 bits, CMYK and Lab). `RenderExecCommon` holds the plan's parts under the names the
// drawing code uses, the state of a draw in progress (the folders open, the coverage worked out so far, a clipping
// stack being kept or resumed, render_resume.h) and the bookkeeping that does not touch a sample: channel
// exclusions around a draw, the chain of folder masks, an artboard's rectangle, the run itself. The pixel arithmetic
// (blending, masks, adjustments, effects) stays each executor's own, in its own translation unit.
//
// `Exec` is the executor (CRTP): it supplies keepChannels, folderMask, drawSpan, runCached and resumeIndex, and as
// statics multiplyCoverage (one coverage times another, in place) and coverageSample (a share of a pixel, 0..1, as a
// coverage sample).
#pragma once
#include "render_plan.h"
#include "render_resume.h"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace nekophoto {

template <class Exec, class Img, class Gray, class AlphaSample>
struct RenderExecCommon {
    using Cover = std::shared_ptr<Gray>;

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
    RenderExecCommon(const RenderPlan& p, Rect r, double s, int w, int h)
        : plan(p), document(p.document), overrides(p.overrides), byId(p.byId), order(p.order), stacks(p.stacks), stacked(p.stacked),
          groupsOpen(p.groupsOpen), groupsClose(p.groupsClose), region(r), scale(s), outWidth(w), outHeight(h) {}
    std::map<Uuid, Cover> liveCoverage;
    std::set<Uuid> visiting;
    bool plainOnly = false;   // while taking a clipping base's transparency
    /// While a styled clipping base is drawn: the colours of its pixels with the layers clipped to it over them (straight,
    /// over the output), which its effects then take as its content.
    const Img* clippedColours = nullptr;
    /// And the share of the base's shape that content covers, where the base's Fill is below 100%.
    const std::function<float(int, int)>* clippedFill = nullptr;
    /// A Blend If layer's gate, for its styled draw to run between its pixels and its interior effects.
    std::function<void()>* interiorGate = nullptr;
    bool ungated = false;     // drawing a Blend If layer's own pass (blendif.h)

    /// The folders being drawn: an isolated one draws into its own buffer, a fading one keeps what was under it, one
    /// that excludes channels keeps what was under it before its effects.
    struct Frame { const Layer* group; std::unique_ptr<Img> buffer; std::unique_ptr<Img> before; Img* parent; std::unique_ptr<Img> unexcluded; };
    std::vector<Frame> frames;
    std::map<Uuid, Cover> folderCoverage;                  // per group id, that group's own mask coverage
    std::map<std::optional<Uuid>, Cover> chainCoverage;    // per parent, all enclosing folders combined

    const LayerOverride* over(const Uuid& id) const { return plan.over(id); }
    LayerTransform transformOf(const Layer& l) const { return plan.transformOf(l); }
    BlendMode blendOf(const Layer& l) const { return plan.blendOf(l); }
    static bool isolates(const Layer& g) { return RenderPlan::isolates(g); }
    static bool fades(const Layer& g) { return RenderPlan::fades(g); }

    Exec& self() { return static_cast<Exec&>(*this); }

    /// Runs `draw` over `target` with `layer`'s channel exclusions; a layer that excludes every channel is not drawn.
    template <class Draw>
    void excluding(const Layer& layer, Img& target, Draw&& draw) {
        const uint8_t excluded = plainOnly ? 0 : plan.excluded(layer);
        if (!excluded) { draw(); return; }
        if (excluded == allBlendChannels(document.colorMode)) return;
        const Img before = target;
        draw();
        self().keepChannels(excluded, before, target);
    }

    /// Coverage from every folder enclosing a layer whose parent is `parent`, or null when none has a mask.
    Cover foldersCoverage(const std::optional<Uuid>& parent) {
        auto it = chainCoverage.find(parent);
        if (it != chainCoverage.end()) return it->second;
        Cover result;
        std::optional<Uuid> current = parent;
        int depth = 0;
        while (current && depth < 64) {
            auto lit = byId.find(*current);
            if (lit == byId.end()) break;
            auto mask = self().folderMask(*lit->second);
            if (mask) {
                if (!result) result = std::make_shared<Gray>(*mask);
                else Exec::multiplyCoverage(result, mask);
            }
            current = lit->second->parentId;
            depth++;
        }
        chainCoverage[parent] = result;
        return result;
    }

    /// How much of each output pixel an artboard's rectangle covers.
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
            auto* row = result->row(y);
            for (int x = 0; x < outWidth; x++) row[x] = Exec::coverageSample(cx[size_t(x)] * cy[size_t(y)]);
        }
        return result;
    }

    /// A clipped adjustment layer being dragged (render_resume.h): its stack's state just before it, kept by the first
    /// frame (`stackKeep`) and started from by the next ones (`stackResume`). A plain base's stack keeps its buffer and
    /// the base's alpha; a styled base's, the frame it draws into and the clip.
    struct StackState { Uuid child; bool reached = false; std::unique_ptr<Img> group; std::vector<AlphaSample> alpha; Cover clip; };
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

    /// Draws the layers order[from, to) over `out`.
    void drawRange(Img& out, size_t from, size_t to) {
        Img* cur = &out;
        self().drawSpan(cur, from, to);
    }

    /// The whole plan into `out`: around the layer being edited from the cache's frames, below an adjustment layer
    /// being dragged from the state the cache keeps, or from the start.
    void run(Img& out, RenderCache* cache, uint64_t version) {
        const size_t edited = cache ? plan.editedIndex() : SIZE_MAX;
        if (edited != SIZE_MAX) { self().runCached(out, *cache, version, edited); return; }
        const size_t adjusting = cache ? plan.adjustingIndex() : SIZE_MAX;
        static const char kind = 0;
        if (adjusting != SIZE_MAX) drawResumed(self(), out, *cache, version, adjusting, self().resumeIndex(adjusting), &kind);
        else drawRange(out, 0, order.size());
    }
};

} // namespace nekophoto
