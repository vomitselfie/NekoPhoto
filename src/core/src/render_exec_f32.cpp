// The renderer's pixels at 32 bits (render_plan.h): the deep executor (render_exec_deep.inc) in premultiplied linear
// float over the DeepOps<F32> policy (render_deep_ops_f32.h), and the context a 32-bit render reads its document's
// curve, luminance weights and view from.
#include "render_deep_ops_f32.h"
#include "render_exec_deep.inc"
#include <atomic>
#include <cstring>
#include <mutex>
#include <utility>

namespace compositor {

namespace {

const FloatRenderContext& defaultContext() {
    static const FloatRenderContext context = [] {
        FloatRenderContext c;
        c.curve = TransferCurve::srgb();
        for (int i = 0; i < 256; i++) c.linear8[size_t(i)] = c.curve.toLinear(float(i) / 255.0f);
        return c;
    }();
    return context;
}

std::atomic<const FloatRenderContext*> current{nullptr};
std::mutex renderMutex;
thread_local int scopeDepth = 0;

/// A fingerprint of a context's curve, so buffers converted through one curve are not reused under another.
uint64_t curveKey(const FloatRenderContext& c) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (float v : c.linear8) { uint32_t bits; std::memcpy(&bits, &v, 4); h ^= bits; h *= 0x100000001b3ull; }
    return h;
}

/// A buffer held at 8 or 16 bits in a 32-bit document (written before its conversion reached it): converted once per
/// buffer and curve, and kept while the buffer lives.
template <class Wide, class Narrow>
std::shared_ptr<const Wide> convertedOnce(const std::shared_ptr<const Narrow>& narrow) {
    static std::mutex m;
    struct Entry { std::weak_ptr<const Narrow> source; uint64_t curve; std::shared_ptr<const Wide> converted; };
    static std::vector<Entry> cache;
    if (!narrow) return nullptr;
    const FloatRenderContext& context = floatRenderContext();
    const uint64_t key = curveKey(context);
    std::lock_guard<std::mutex> lock(m);
    for (auto it = cache.begin(); it != cache.end();) {
        if (it->source.expired()) { it = cache.erase(it); continue; }
        if (it->source.lock() == narrow && it->curve == key) return it->converted;
        ++it;
    }
    std::shared_ptr<const Wide> wide;
    if constexpr (std::is_same_v<Wide, ImageF>) wide = lineariseImage(*narrow, context.curve);
    else wide = widenGrayF(*narrow);
    if (cache.size() > 64) cache.erase(cache.begin());
    cache.push_back({narrow, key, wide});
    return wide;
}

} // namespace

const FloatRenderContext& floatRenderContext() {
    const FloatRenderContext* c = current.load(std::memory_order_acquire);
    return c ? *c : defaultContext();
}

FloatRenderScope::FloatRenderScope(const Document& document, const View32& view, float peak) {
    context_.curve = encodedTransfer(document);
    for (int i = 0; i < 256; i++) context_.linear8[size_t(i)] = context_.curve.toLinear(float(i) / 255.0f);
    const std::array<float, 3> weights = luminanceWeights(document.sampleType == SampleType::F32 ? document.profile : linearProfile(document.profile));
    for (int k = 0; k < 3; k++) context_.luma[k] = weights[size_t(k)];
    context_.view = view.clamped();
    context_.peak = peak;
    outermost_ = scopeDepth++ == 0;
    if (outermost_) renderMutex.lock();
    previous_ = current.exchange(&context_, std::memory_order_acq_rel);
}

FloatRenderScope::~FloatRenderScope() {
    current.store(previous_, std::memory_order_release);
    scopeDepth--;
    if (outermost_) renderMutex.unlock();
}

ImageFPtr DeepOps<SampleType::F32>::image(const AnyImage& image) {
    if (image.f32()) return image.f32();
    if (image.u16()) return image.u16()->channels() == 4 ? convertedOnce<ImageF>(image.u16()) : nullptr;
    return convertedOnce<ImageF>(image.u8());
}

GrayFPtr DeepOps<SampleType::F32>::gray(const AnyGray& gray) {
    if (gray.f32()) return gray.f32();
    if (gray.u16()) return convertedOnce<GrayF>(gray.u16());
    return convertedOnce<GrayF>(gray.u8());
}

std::optional<ImageFPtr> DeepOps<SampleType::F32>::overrideImage(const LayerOverride& o) {
    if (o.imageF) return *o.imageF;
    if (o.image16) return convertedOnce<ImageF>(*o.image16);
    if (o.image) return convertedOnce<ImageF>(*o.image);
    return std::nullopt;
}

std::optional<GrayFPtr> DeepOps<SampleType::F32>::overrideMask(const LayerOverride& o) {
    if (o.maskImageF) return *o.maskImageF;
    if (o.maskImage16) return convertedOnce<GrayF>(*o.maskImage16);
    if (o.maskImage) return convertedOnce<GrayF>(*o.maskImage);
    return std::nullopt;
}

template <>
struct RenderExec<SampleType::F32> : RenderExecDeep<SampleType::F32> {
    using RenderExecDeep::RenderExecDeep;
};

template <>
void executeRender<SampleType::F32>(const RenderPlan& plan, const Rect& region, double scale, ImageF& out, RenderCache* cache, uint64_t version) {
    FloatRenderScope scope(plan.document);
    RenderExec<SampleType::F32> exec(plan, region, scale, out.width(), out.height());
    exec.run(out, cache, version);
}

} // namespace compositor
