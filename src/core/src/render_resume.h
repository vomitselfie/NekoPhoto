// A frame resumed below an adjustment layer being dragged (RenderCache::resume, render.h).
//
// While an adjustment layer's settings change (its override is `adjusting`), everything drawn before the first layer
// that depends on them stays the same from one frame to the next: the frame as it stood then, and the folders open at
// that point (an isolated folder's buffer, a fading one's backdrop). The executor draws up to there once, keeps that
// state, and every later frame copies it back and draws on from the same index, exactly as an uncached frame goes on
// from it: the result is the same, bit for bit. Where that index is comes from RenderPlan::resumeIndex.
//
// A clipped layer resumes at its stack's base, and inside the stack too: the first frame keeps the stack's state on
// reaching the layer (its buffer and the base's alpha, or a styled base's frame and clip), and the next frames start the
// stack from there, so the base is not drawn again either.
//
// The state is kept against the caller's version, the region, the scale, the output size, the layer and the index;
// any of them changing draws it again. Both executors (8-bit RGB and the deep one, which also draws CMYK and Lab) use
// it through `drawResumed`, with their own image type and a tag of their own.
#pragma once
#include "render_plan.h"
#include <cstring>
#include <memory>
#include <vector>

namespace compositor {

struct RenderResume {
    virtual ~RenderResume() = default;
    const void* kind = nullptr;   // the executor that drew it
    uint64_t version = 0;
    Rect region;
    double scale = 0;
    int width = 0, height = 0;
    Uuid layer;
    size_t index = 0;
};

template <class Img, class Stack>
struct RenderResumeOf : RenderResume {
    std::unique_ptr<Img> out;
    /// A clipped layer: its clipping stack's state just before it (the executor's own record), when it was reached.
    std::unique_ptr<Stack> stack;
    /// The folders open at `index`, outermost first: their buffers and backdrops, and where each one's result goes
    /// (-1 the frame, else the buffer of that earlier entry).
    struct Frame { Uuid group; std::unique_ptr<Img> buffer, before; int parent = -1; };
    std::vector<Frame> frames;
    int cur = -1;   // what is being drawn into: the frame or an open folder's buffer
};

/// Draws `exec`'s plan into `out` (cleared) for the adjustment layer at `adjusting`, resuming at `at` from the state
/// `cache` keeps, or drawing up to `at` and keeping it first. `Exec` has `frames` (group, buffer, before, parent),
/// `byId`, `order`, `stacked`, `region`, `scale`, `outWidth`, `outHeight`, `drawSpan(Img*& cur, from, to)`, and for a
/// clipped layer `StackState` (with `child`) and the `stackKeep` and `stackResume` its clipping stack fills or starts
/// from.
template <class Img, class Exec>
void drawResumed(Exec& exec, Img& out, RenderCache& cache, uint64_t version, size_t adjusting, size_t at, const void* kind) {
    using Stack = typename Exec::StackState;
    using Saved = RenderResumeOf<Img, Stack>;
    const Layer& layer = *exec.order[adjusting];
    auto* saved = dynamic_cast<Saved*>(cache.resume.get());
    bool valid = saved && saved->kind == kind && saved->version == version && saved->layer == layer.id && saved->index == at
        && saved->region == exec.region && saved->scale == exec.scale && saved->width == exec.outWidth && saved->height == exec.outHeight
        && saved->out && saved->out->byteCount() == out.byteCount();
    if (valid)
        for (auto& f : saved->frames) if (!exec.byId.count(f.group)) { valid = false; break; }
    Img* cur = &out;
    exec.frames.clear();
    if (valid) {
        std::memcpy(out.data(), saved->out->data(), out.byteCount());
        for (auto& f : saved->frames) {
            typename Exec::Frame frame{exec.byId.at(f.group), f.buffer ? std::make_unique<Img>(*f.buffer) : nullptr,
                                       f.before ? std::make_unique<Img>(*f.before) : nullptr,
                                       f.parent < 0 ? &out : exec.frames[size_t(f.parent)].buffer.get()};
            exec.frames.push_back(std::move(frame));
        }
        if (saved->cur >= 0) cur = exec.frames[size_t(saved->cur)].buffer.get();
        exec.stackResume = saved->stack.get();
    } else {
        // Whatever the cache held for another edit goes first: one kept frame at a time.
        cache.resume.reset();
        cache.backdrop.reset(); cache.above.reset();
        cache.backdrop16.reset(); cache.above16.reset();
        cache.backdropF.reset(); cache.aboveF.reset();
        cache.backdropC8.reset(); cache.aboveC8.reset();
        cache.version = 0; cache.width = cache.height = 0;
        exec.drawSpan(cur, 0, at);
        auto keep = std::make_shared<Saved>();
        keep->kind = kind;
        keep->version = version;
        keep->region = exec.region;
        keep->scale = exec.scale;
        keep->width = exec.outWidth;
        keep->height = exec.outHeight;
        keep->layer = layer.id;
        keep->index = at;
        keep->out = std::make_unique<Img>(out);
        auto bufferIndex = [&](const Img* image) {
            for (size_t j = 0; j < exec.frames.size(); j++) if (exec.frames[j].buffer.get() == image) return int(j);
            return -1;
        };
        for (auto& f : exec.frames) {
            typename Saved::Frame k;
            k.group = f.group->id;
            if (f.buffer) k.buffer = std::make_unique<Img>(*f.buffer);
            if (f.before) k.before = std::make_unique<Img>(*f.before);
            k.parent = bufferIndex(f.parent);
            keep->frames.push_back(std::move(k));
        }
        keep->cur = bufferIndex(cur);
        Saved* kept = keep.get();
        if (exec.stacked.count(layer.id)) {
            kept->stack = std::make_unique<Stack>();
            kept->stack->child = layer.id;
            exec.stackKeep = kept->stack.get();
        }
        cache.resume = std::move(keep);
        exec.drawSpan(cur, at, exec.order.size());
        // A stack that never reached the layer has nothing to resume from: it is drawn whole each time.
        if (kept->stack && !kept->stack->reached) kept->stack.reset();
        exec.stackKeep = nullptr;
        return;
    }
    exec.drawSpan(cur, at, exec.order.size());
    exec.stackResume = nullptr;
}

} // namespace compositor
