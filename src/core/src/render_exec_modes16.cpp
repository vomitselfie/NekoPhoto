// The renderer's pixels for 16-bit CMYK and Lab documents (P7 step C): the deep executor (render_exec_deep.inc) over
// `ModeOps<U16, CMYK>` (5 samples a pixel) and `ModeOps<U16, Lab>` (render_modes.h), coverage at 15 bits. The RGB
// instance (render_exec_u16.cpp) keeps its own policy, so its code is unchanged.
#include "render_exec_deep.inc"
#include "render_modes.h"

namespace compositor {

void executeRenderMode16(const RenderPlan& plan, const Rect& region, double scale, Image16& out, RenderCache* cache, uint64_t version) {
    if (plan.document.colorMode == ColorMode::CMYK) {
        RenderExecDeep<SampleType::U16, ModeOps<SampleType::U16, ColorMode::CMYK>> exec(plan, region, scale, out.width(), out.height());
        exec.run(out, cache, version);
    } else {
        RenderExecDeep<SampleType::U16, ModeOps<SampleType::U16, ColorMode::Lab>> exec(plan, region, scale, out.width(), out.height());
        exec.run(out, cache, version);
    }
}

} // namespace compositor
