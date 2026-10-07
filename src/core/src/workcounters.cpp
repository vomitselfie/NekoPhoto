// Work counters (workcounters.h, docs/work-counters.md).
#include "compositor/workcounters.h"

namespace compositor::work {

std::array<std::atomic<uint64_t>, counterCount> counters{};

const char* name(Counter counter) {
    switch (counter) {
    case Counter::Renders: return "render.calls";
    case Counter::RenderPixels: return "render.pixels";
    case Counter::LayerDraws: return "layer.draws";
    case Counter::LayerDrawPixels: return "layer.drawPixels";
    case Counter::Adjustments: return "adjustment.passes";
    case Counter::AdjustmentPixels: return "adjustment.pixels";
    case Counter::MipHits: return "mip.hits";
    case Counter::MipMisses: return "mip.misses";
    case Counter::MipBuiltPixels: return "mip.builtPixels";
    case Counter::MipRefreshPixels: return "mip.refreshPixels";
    case Counter::BrushDabs: return "brush.dabs";
    case Counter::BrushRecomposePixels: return "brush.recomposePixels";
    case Counter::SmartFilterStacks: return "smartFilter.stacks";
    case Counter::SmartFilterPasses: return "smartFilter.passes";
    case Counter::Count: break;
    }
    return "?";
}

std::string Snapshot::describe() const {
    std::string out;
    for (size_t i = 0; i < counterCount; i++) {
        if (!values[i]) continue;
        if (!out.empty()) out += ' ';
        out += name(Counter(i));
        out += '=';
        out += std::to_string(values[i]);
    }
    return out.empty() ? "(no work)" : out;
}

Snapshot snapshot() {
    Snapshot s;
    for (size_t i = 0; i < counterCount; i++) s.values[i] = counters[i].load(std::memory_order_relaxed);
    return s;
}

void reset() {
    for (auto& c : counters) c.store(0, std::memory_order_relaxed);
}

} // namespace compositor::work
