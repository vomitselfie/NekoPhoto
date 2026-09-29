// The entry points that render a deep document, written once for every depth above 8 bits against the
// `DeepOps<S>` policy (render_deep_ops.h). Each depth's file includes this once and names its entry points
// (render_u16.cpp).
#include "render_deep_ops.h"
#include "compositor/render.h"
#include <algorithm>
#include <cmath>

namespace compositor {

namespace {

/// A deep document's frame for the canvas, which takes 8 bits: the frame at the document's depth, then reduced to
/// 8 bits, or through the display's colour transform in the same pass.
template <SampleType S>
void renderForDisplayDeep(const RenderPlan& plan, const Rect& region, double scale, Image& out, RenderCache* cache, uint64_t version, bool clear, const ColorTransform* display) {
    using O = DeepOps<S>;
    typename O::Image deep(out.width(), out.height());
    if (!clear) deep = *O::widenFrom8(out);
    executeRender<S>(plan, region, scale, deep, cache, version);
    O::toDisplay(deep, out, display);
}

/// Renders a document into a buffer at depth `S`; a document at another depth renders at its own and is converted.
template <SampleType S>
void renderDeep(const Document& document, const RenderOptions& options, ImageOf<S>& out, const Overrides* overrides, RenderCache* cache) {
    using O = DeepOps<S>;
    Rect region = options.region.isEmpty() ? document.rect() : options.region;
    double scale = options.scale > 0 ? options.scale : 1;
    int w = std::max(1, int(std::ceil(region.width * scale - 1e-9))), h = std::max(1, int(std::ceil(region.height * scale - 1e-9)));
    if (out.width() != w || out.height() != h) out = ImageOf<S>(w, h);
    else if (options.clear) out.clear();
    if (document.sampleType != S) {
        Image eight = options.clear ? Image(w, h) : *O::narrowTo8(out);
        RenderOptions eightOptions = options;
        eightOptions.clear = false;
        render(document, eightOptions, eight, overrides, cache);
        out = *O::widenFrom8(eight);
        return;
    }
    RenderPlan plan(document, overrides);
    plan.build();
    executeRender<S>(plan, region, scale, out, options.clear ? cache : nullptr, options.version);
}

} // namespace

} // namespace compositor
