// The renderer's pixels for 8-bit CMYK documents (docs/high-bit-depth-plan.md, P7 step C): the deep executor
// (render_exec_deep.inc) over `ModeOps<U8, CMYK>` (render_modes.h), 5 samples a pixel and 8-bit coverage. The RGB
// executor (render_exec_u8.cpp) is separate and unchanged.
#include "render_exec_deep.inc"
#include "render_modes.h"

namespace compositor {

void executeRenderMode(const RenderPlan& plan, const Rect& region, double scale, ImageC8& out, RenderCache* cache, uint64_t version) {
    RenderExecDeep<SampleType::U8, ModeOps<SampleType::U8, ColorMode::CMYK>> exec(plan, region, scale, out.width(), out.height());
    exec.run(out, cache, version);
}

} // namespace compositor
