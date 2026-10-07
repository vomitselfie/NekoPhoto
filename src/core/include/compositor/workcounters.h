// Work counters: tallies of the work the renderer, the mip cache, the brush engine and the Smart Filters do, so tests
// can bound work done instead of timing it (docs/work-counters.md). Each is a relaxed atomic bumped once per call or
// per pass (never per pixel), with the pixel count of what that call covered, so they stay compiled in: a few
// uncontended increments per layer drawn. Counts are sums of areas fixed by the call, not by how a parallel loop split
// the rows, so they come out the same at any thread count.
#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace compositor::work {

enum class Counter : int {
    Renders,                // render(), render16(), renderF(), renderNative(): one per call
    RenderPixels,           //   their output pixels
    LayerDraws,             // drawLayer at any depth: one layer resampled and composited into a frame
    LayerDrawPixels,        //   the output pixels its footprint covered (clipped to the frame)
    Adjustments,            // adjustment layers applied while rendering (each layer of a fused run of table adjustments)
    AdjustmentPixels,       //   the frame pixels their passes went over (a fused run is one pass)
    MipHits,                // MipCache::level asked for a reduction it held
    MipMisses,              // ... or had to build (from the nearest level it held)
    MipBuiltPixels,         //   the pixels of the levels built
    MipRefreshPixels,       // MipCache::refresh: level pixels brought up to date after a stroke wrote in place
    BrushDabs,              // round-brush dabs stamped (BrushStroke::dab)
    BrushRecomposePixels,   // working pixels a stroke recomposed from its base and coverage
    SmartFilterStacks,      // renderSmartFilterStack runs (one per stack drawn, per depth pass)
    SmartFilterPasses,      //   the enabled entries those runs drew
    Count
};

constexpr size_t counterCount = size_t(Counter::Count);

/// The counters themselves (workcounters.cpp); use add() and snapshot().
extern std::array<std::atomic<uint64_t>, counterCount> counters;

inline void add(Counter counter, uint64_t amount = 1) {
    counters[size_t(counter)].fetch_add(amount, std::memory_order_relaxed);
}
inline uint64_t get(Counter counter) { return counters[size_t(counter)].load(std::memory_order_relaxed); }

/// The counter's name as tests and `app.counters` print it ("render.pixels").
const char* name(Counter counter);

/// Every counter at one moment; the difference of two is the work done between them.
struct Snapshot {
    std::array<uint64_t, counterCount> values{};
    uint64_t operator[](Counter counter) const { return values[size_t(counter)]; }
    Snapshot operator-(const Snapshot& earlier) const {
        Snapshot d;
        for (size_t i = 0; i < counterCount; i++) d.values[i] = values[i] - earlier.values[i];
        return d;
    }
    /// "name=value" for every nonzero counter, space separated: for failure messages.
    std::string describe() const;
};
Snapshot snapshot();
/// Sets every counter to zero (tests; other threads still rendering would race with it).
void reset();

} // namespace compositor::work
