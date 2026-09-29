// The renderer's pixels for 8-bit Lab documents (P7 step C): the deep executor over `ModeOps<U8, Lab>`
// (render_modes.h). Normal and Dissolve run blend_u8.cpp's arithmetic on L, a and b; the other modes are blend_c8.cpp's.
#include "render_exec_deep.inc"
#include "render_modes.h"

namespace compositor {

void executeRenderLab8(const RenderPlan& plan, const Rect& region, double scale, Image& out, RenderCache* cache, uint64_t version) {
    RenderExecDeep<SampleType::U8, ModeOps<SampleType::U8, ColorMode::Lab>> exec(plan, region, scale, out.width(), out.height());
    exec.run(out, cache, version);
}

} // namespace compositor
