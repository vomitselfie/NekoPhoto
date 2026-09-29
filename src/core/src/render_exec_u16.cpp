// The renderer's pixels at 16 bits (render_plan.h): the deep executor (render_exec_deep.inc) at Photoshop's
// 0..32768, with coverage at 15 bits.
#include "render_exec_deep.inc"
#include <mutex>
#include <utility>

namespace compositor {

namespace {

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

} // namespace

Image16Ptr DeepOps<SampleType::U16>::image(const AnyImage& image) {
    if (image.u16()) return image.u16();
    return widenedOnce<Image16>(image.u8());
}

Gray16Ptr DeepOps<SampleType::U16>::gray(const AnyGray& gray) {
    if (gray.u16()) return gray.u16();
    return widenedOnce<Gray16>(gray.u8());
}

std::optional<Image16Ptr> DeepOps<SampleType::U16>::overrideImage(const LayerOverride& o) {
    if (o.image16) return *o.image16;
    if (o.image) return widenedOnce<Image16>(*o.image);
    return std::nullopt;
}

std::optional<Gray16Ptr> DeepOps<SampleType::U16>::overrideMask(const LayerOverride& o) {
    if (o.maskImage16) return *o.maskImage16;
    if (o.maskImage) return widenedOnce<Gray16>(*o.maskImage);
    return std::nullopt;
}

template <>
struct RenderExec<SampleType::U16> : RenderExecDeep<SampleType::U16> {
    using RenderExecDeep::RenderExecDeep;
};

template <>
void executeRender<SampleType::U16>(const RenderPlan& plan, const Rect& region, double scale, Image16& out, RenderCache* cache, uint64_t version) {
    RenderExec<SampleType::U16> exec(plan, region, scale, out.width(), out.height());
    exec.run(out, cache, version);
}

} // namespace compositor
