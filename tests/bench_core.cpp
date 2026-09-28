// The P0 benches for the high-bit-depth work (docs/high-bit-depth-plan.md): render of a large layered document, a
// brush stroke, a gaussian blur and a few adjustments, each the median of N runs. Not a test (not in ctest):
//   build/tests/bench_core [runs] [filter]
// Results from this machine are kept in docs/benchmarks.md; later phases are held to them.
//
// "worst-case brush" (report only, no threshold; run with the filter "worst"): a 10-second recorded tip-brush stroke with every per-dab feature at
// once on a 4096 x 4096 layer at 8 and 16 bits, with its wall time, samples and dabs per second, and the most memory
// the stroke held beyond what was held before it (sampled every millisecond, on glibc).
#include "compositor/colormgmt.h"
#include "compositor/adjustments.h"
#include "compositor/brush.h"
#include "compositor/depth.h"
#include "compositor/document.h"
#include "compositor/filters.h"
#include "compositor/history.h"
#include "compositor/layerstyle.h"
#include "compositor/vectorlayer.h"
#include "compositor/mosh.h"
#include "compositor/parallel.h"
#include "compositor/render.h"
#include "compositor/tipbrush.h"
#include "render_plan.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace compositor;

// ---- Memory while a bench runs -------------------------------------------------------------------------------------
// A thread samples, every millisecond, the bytes malloc holds (glibc's mallinfo2: the heap and the blocks it maps,
// which the pixel buffers' calloc uses) and the process's resident memory (VmRSS), keeping the most of each. glibc
// only; elsewhere the worst-case bench reports no memory figure.
#if defined(__GLIBC__)
#include <malloc.h>
#include <fstream>
#include <thread>
namespace {
constexpr bool countsMemory = true;
long long mallocHeld() { const struct mallinfo2 m = mallinfo2(); return (long long)(m.uordblks + m.hblkhd); }
long long residentBytes() {
    std::ifstream status("/proc/self/status");
    for (std::string line; std::getline(status, line);)
        if (line.rfind("VmRSS:", 0) == 0) return std::atoll(line.c_str() + 6) * 1024;
    return 0;
}
/// Samples until destroyed; `peakHeld` and `peakResident` are the most seen.
class MemorySampler {
public:
    MemorySampler() : thread_([this] {
        while (!stop_) {
            peakHeld = std::max(peakHeld.load(), mallocHeld());
            peakResident = std::max(peakResident.load(), residentBytes());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }) {}
    ~MemorySampler() { stop_ = true; thread_.join(); }
    std::atomic<long long> peakHeld{0}, peakResident{0};
private:
    std::atomic<bool> stop_{false};
    std::thread thread_;
};
} // namespace
#else
namespace {
constexpr bool countsMemory = false;
long long mallocHeld() { return 0; }
long long residentBytes() { return 0; }
struct MemorySampler { std::atomic<long long> peakHeld{0}, peakResident{0}; };
} // namespace
#endif

namespace {

uint32_t mix(uint32_t v) { v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; v ^= v >> 16; return v; }

std::shared_ptr<Image> busy(int w, int h, uint32_t seed, bool transparent) {
    auto img = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; y++) {
        uint8_t* p = img->row(y);
        for (int x = 0; x < w; x++, p += 4) {
            uint32_t n = mix(seed ^ uint32_t(y * w + x));
            unsigned a = transparent ? unsigned(std::clamp(int(255 - std::hypot(x - w / 2.0, y - h / 2.0) * 255 / (w / 2.0)) + 64, 0, 255)) : 255;
            unsigned r = (x * 3 + y + (n & 15)) & 255, g = (x + y * 5) & 255, b = ((x ^ y) + (n >> 8)) & 255;
            p[0] = uint8_t((r * a + 127) / 255); p[1] = uint8_t((g * a + 127) / 255); p[2] = uint8_t((b * a + 127) / 255); p[3] = uint8_t(a);
        }
    }
    return img;
}

/// 4000 x 3000: a base, eight layers in assorted modes (two masked, one clipped, two in an isolated group) and a
/// Levels and a Curves adjustment layer.
Document largeDocument() {
    const int W = 4000, H = 3000;
    Document doc(W, H);
    doc.layers.push_back(Layer(Asset::make(busy(W, H, 1, false), "base"), Point(0, 0)));
    const BlendMode modes[] = {BlendMode::Normal, BlendMode::Multiply, BlendMode::Screen, BlendMode::Overlay, BlendMode::SoftLight, BlendMode::Color};
    for (int i = 0; i < 6; i++) {
        Layer l(Asset::make(busy(2400, 1800, uint32_t(10 + i), true), "layer"), Point(200 + i * 220, 150 + i * 160));
        l.blendMode = modes[i];
        l.opacity = 0.6 + 0.07 * i;
        if (i % 3 == 1) {
            auto mask = std::make_shared<GrayImage>(2400, 1800, 0);
            for (int y = 0; y < 1800; y++) for (int x = 0; x < 2400; x++) mask->at(x, y) = uint8_t(x * 255 / 2399);
            LayerMask m; m.asset = MaskAsset::make(mask); l.mask = m;
        }
        doc.layers.push_back(l);
    }
    Layer clipped(Asset::make(busy(W, H, 30, false), "clipped"), Point(0, 0));
    clipped.maskSourceId = doc.layers.back().id;
    clipped.opacity = 0.5;
    doc.layers.push_back(clipped);
    Layer group("Group", doc.size());
    group.isGroup = true;
    group.passThrough = false;
    group.blendMode = BlendMode::HardLight;
    doc.layers.push_back(group);
    for (int i = 0; i < 2; i++) {
        Layer l(Asset::make(busy(1600, 1200, uint32_t(40 + i), true), "inner"), Point(1800 + i * 400, 1200 + i * 300));
        l.parentId = group.id;
        l.blendMode = i ? BlendMode::Difference : BlendMode::Normal;
        doc.layers.push_back(l);
    }
    for (AdjustmentKind kind : {AdjustmentKind::Levels, AdjustmentKind::Curves}) {
        AdjustmentSettings s = AdjustmentSettings::defaults(kind);
        s.levels.ranges[0] = {15, 1.2, 240, 0, 255};
        s.curves.channels[0] = {{0, 0}, {64, 50}, {192, 210}, {255, 255}};
        Layer adj("Adjustment", doc.size());
        adj.adjustment = s.toLayerAdjustment();
        doc.layers.push_back(adj);
    }
    return doc;
}

/// `count` layers as a large PSD brings them: small pixel layers sharing one raster, a folder of five every tenth.
Document manyLayers(int count) {
    Document doc(2000, 2000);
    const Asset shared = Asset::make(ImagePtr(busy(32, 32, 3, true)), "tile");
    std::optional<Uuid> folder;
    for (int i = 0; i < count; i++) {
        if (i % 10 == 0) {
            Layer group("Folder", doc.size());
            group.isGroup = true;
            doc.layers.push_back(group);
            folder = group.id;
            continue;
        }
        Layer l(shared, Point((i * 37) % 1968, (i * 91) % 1968));
        l.name = "Layer " + std::to_string(i);
        if (i % 10 <= 5) l.parentId = folder;
        doc.layers.push_back(l);
    }
    // Groups are listed after their children in the stack (bottom to top), as the app keeps them.
    std::stable_partition(doc.layers.begin(), doc.layers.end(), [](const Layer& l) { return !l.isGroup; });
    return doc;
}

struct Bench {
    int runs;
    std::string filter;
    void operator()(const char* name, const std::function<void()>& setup, const std::function<void()>& body) const {
        if (!filter.empty() && std::string(name).find(filter) == std::string::npos) return;
        std::vector<double> ms;
        for (int i = 0; i < runs; i++) {
            setup();
            auto t0 = std::chrono::steady_clock::now();
            body();
            ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        std::sort(ms.begin(), ms.end());
        std::printf("%-36s median %9.2f ms   min %9.2f   max %9.2f   (%d runs)\n", name, ms[ms.size() / 2], ms.front(), ms.back(), runs);
        std::fflush(stdout);
    }
};

/// The worst-case tip brush: a 200-pixel soft tip at 2% spacing, 16 dabs a step scattered half a size both ways, a
/// moving (Stroke) grain, and dynamics on every pen input a Procreate or Photoshop brush drives: the speed on the screen
/// on size and opacity, tilt on roundness, the lean's direction and the barrel's roll on the angle, a size jitter.
BrushTip worstCaseTip() {
    BrushTip tip;
    auto shape = std::make_shared<GrayImage>(256, 256, 0);
    for (int y = 0; y < 256; y++)
        for (int x = 0; x < 256; x++) {
            const double d = std::hypot(x + 0.5 - 128, y + 0.5 - 128) / 128;
            shape->at(x, y) = uint8_t(std::lround(255 * std::clamp(1.2 - 1.2 * d * d, 0.0, 1.0)));
        }
    tip.shape = shape;
    auto grain = std::make_shared<GrayImage>(128, 128, 0);
    for (int y = 0; y < 128; y++) for (int x = 0; x < 128; x++) grain->at(x, y) = uint8_t(mix(uint32_t(y * 128 + x) * 2654435761u) & 255);
    tip.grain = grain;
    tip.grainMode = BrushTip::GrainMode::Stroke;
    tip.grainMovement = 1;
    tip.grainDepth = 0.7;
    tip.spacing = 0.02;
    tip.scatter = 0.5;
    tip.scatterBothAxes = true;
    tip.count = 16;
    tip.flow = 0.2;
    tip.dynamics = {dynamicsMapping(DynamicsInput::ScreenSpeed, DynamicsTarget::Size, 1, -0.5, 1500),
                    dynamicsMapping(DynamicsInput::ScreenSpeed, DynamicsTarget::Opacity, 1, -0.4, 1500),
                    dynamicsMapping(DynamicsInput::Tilt, DynamicsTarget::Roundness, 1, -0.6),
                    dynamicsMapping(DynamicsInput::TiltDirection, DynamicsTarget::Angle, 0, -360),
                    dynamicsMapping(DynamicsInput::Roll, DynamicsTarget::Angle, 0, -360),
                    dynamicsMapping(DynamicsInput::Random, DynamicsTarget::Size, 1, -0.2)};
    return tip;
}

/// Ten seconds of a pen at 120 reports a second: a looping path over the canvas whose speed swings between about 150
/// and 1100 points a second, the pen leaning and turning its barrel as it goes. The same every run.
std::vector<BrushSample> worstCaseStroke() {
    std::vector<BrushSample> out;
    constexpr double pi = 3.14159265358979323846;
    double phase = 0;
    for (int i = 0; i <= 1200; i++) {
        const double t = i / 120.0;
        phase += (0.35 + 0.25 * std::sin(2 * pi * t / 3.3)) / 120;   // turns of the loop per report
        BrushSample s;
        s.time = t;
        s.position = {2048 + 1500 * std::sin(2 * pi * phase * 1.0), 2048 + 1300 * std::sin(2 * pi * phase * 1.5 + 0.7)};
        s.pressure = 0.6 + 0.35 * std::sin(2 * pi * t / 1.7);
        s.tiltX = 40 * std::cos(2 * pi * t / 4.1);
        s.tiltY = 35 * std::sin(2 * pi * t / 2.9);
        s.twist = std::remainder(90 * t, 360.0);
        s.twistReported = true;
        s.stylus = true;
        out.push_back(s);
    }
    return out;
}

/// Paints the worst-case stroke on `layer` `runs` times and prints the median wall time, the rates and the memory.
void worstCaseBrush(Layer& layer, SampleType depth, int runs) {
    const BrushTip tip = worstCaseTip();
    const std::vector<BrushSample> samples = worstCaseStroke();
    BrushSettings settings;
    settings.diameter = 200;
    settings.red = 0.2; settings.green = 0.3; settings.blue = 0.6;
    std::vector<double> ms;
    size_t dabs = 0;
    long long extraHeld = 0, extraResident = 0;
    for (int run = 0; run < runs; run++) {
        const long long heldBefore = mallocHeld(), residentBefore = residentBytes();
        MemorySampler sampler;
        const auto t0 = std::chrono::steady_clock::now();
        {
            auto stroke = depth == SampleType::U16 ? std::make_unique<BrushStroke>(layer, false, settings, Size(4096, 4096), depth, nullptr)
                                                   : std::make_unique<BrushStroke>(layer, false, settings, Size(4096, 4096));
            TipStroke tipStroke(*stroke, tip, settings.diameter, 1234);
            BrushSampleTrack track;
            for (const BrushSample& s : samples) tipStroke.strokeTo(track.add(s));
            tipStroke.finish();
            stroke->flush();
            auto commit = stroke->commit();
            dabs = tipStroke.dabCount();
        }
        ms.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        extraHeld = std::max(extraHeld, sampler.peakHeld - heldBefore);
        extraResident = std::max(extraResident, sampler.peakResident - residentBefore);
    }
    std::sort(ms.begin(), ms.end());
    const double median = ms[ms.size() / 2];
    char memory[96] = "n/a";
    if (countsMemory)
        std::snprintf(memory, sizeof memory, "%.0f MiB allocated, %.0f MiB resident", double(extraHeld) / (1024.0 * 1024.0), double(extraResident) / (1024.0 * 1024.0));
    std::printf("%-36s median %9.2f ms   min %9.2f   max %9.2f   (%d runs)\n", depth == SampleType::U16 ? "worst-case brush u16 4096" : "worst-case brush u8 4096",
                median, ms.front(), ms.back(), runs);
    std::printf("    %zu samples (%.0f/s), %zu dabs (%.0f/s), peak temporary memory %s\n", samples.size(), samples.size() / (median / 1000),
                dabs, dabs / (median / 1000), memory);
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv) {
    Bench bench{argc > 1 ? std::max(1, std::atoi(argv[1])) : 9, argc > 2 ? argv[2] : ""};
    std::printf("bench_core: %d worker threads, median of %d runs\n", workerCount(), bench.runs);
    auto none = [] {};

    // The worst-case brush, on its own layers (report only): about half a minute a run, so only when the filter names
    // it (bench_core 3 worst), and at most three runs.
    if (!bench.filter.empty() && std::string("worst-case brush").find(bench.filter) != std::string::npos) {
        auto paper = busy(4096, 4096, 21, false);
        Layer eight(Asset::make(paper, "worst"), Point(0, 0));
        worstCaseBrush(eight, SampleType::U8, std::min(bench.runs, 3));
        Layer deep(Asset::make(Image16Ptr(widenImage(*paper)), "worst16"), Point(0, 0));
        worstCaseBrush(deep, SampleType::U16, std::min(bench.runs, 3));
    }

    Document doc = largeDocument();
    Image out;
    bench("render 4000x3000, 12 layers", none, [&] { RenderOptions o; render(doc, o, out); });
    bench("render 4000x3000 at 0.25", none, [&] { RenderOptions o; o.scale = 0.25; render(doc, o, out); });
    bench("render 1024x768 region at 1", none, [&] { RenderOptions o; o.region = {1500, 1100, 1024, 768}; render(doc, o, out); });

    Layer canvas(Asset::make(busy(4000, 3000, 5, false), "canvas"), Point(0, 0));
    std::vector<Point> path;
    for (double x = 200; x <= 3800; x += 4) path.push_back({x, 1500 + 900 * std::sin(x / 400.0)});
    for (double hardness : {1.0, 0.3}) {
        BrushSettings s;
        s.diameter = 80;
        s.hardness = hardness;
        s.opacity = 0.8;
        s.red = 0.9;
        std::string name = std::string("brush stroke d80 hardness ") + (hardness == 1 ? "1" : "0.3");
        bench(name.c_str(), none, [&] {
            BrushStroke stroke(canvas, false, s, Size(4000, 3000));
            stroke.appendAll(path);
            stroke.flush();
            auto commit = stroke.commit();
        });
    }

    auto photo = busy(4000, 3000, 7, false);
    Image work;
    auto fresh = [&] { work = *photo; };
    for (double radius : {2.0, 20.0}) {
        std::string name = "gaussian blur r" + std::to_string(int(radius));
        bench(name.c_str(), fresh, [&] { FilterSettings s; s.radius = radius; applyFilter(FilterKind::GaussianBlur, work, s); });
    }
    const Rect all{0, 0, 4000, 3000};
    auto adjust = [&](const char* name, AdjustmentSettings s) { bench(name, fresh, [&] { applyAdjustment(s, work, all, 1); }); };
    AdjustmentSettings levels = AdjustmentSettings::defaults(AdjustmentKind::Levels);
    levels.levels.ranges[0] = {15, 1.2, 240, 0, 255};
    adjust("levels", levels);
    AdjustmentSettings curves = AdjustmentSettings::defaults(AdjustmentKind::Curves);
    curves.curves.channels[0] = {{0, 0}, {64, 50}, {192, 210}, {255, 255}};
    adjust("curves", curves);
    AdjustmentSettings hsv = AdjustmentSettings::defaults(AdjustmentKind::HueSaturation);
    hsv.hsv.adjustments[0] = {20, 25, -5};
    adjust("hue/saturation", hsv);
    // Mosh (docs/mosh.md): the heaviest ports, at their defaults.
    for (const char* id : {"pixel-sort", "vhs", "crt", "hard-glitch", "glow", "light-streak", "feedback", "optical-flow"}) {
        const std::string name = std::string("mosh ") + id;
        const mosh::Settings s = mosh::Settings::defaults(*mosh::findEffect(id));
        bench(name.c_str(), fresh, [&] { mosh::apply(s, work); });
    }

    // The same document at 16 bits (P2): the render at its depth, the canvas's (reduced to 8 bits), and reduced.
    Document deep = doc;
    if (!convertSampleType(deep, SampleType::U16)) { std::printf("16-bit conversion failed\n"); return 1; }
    Image16 out16;
    bench("render16 4000x3000, 12 layers", none, [&] { RenderOptions o; render16(deep, o, out16); });
    bench("render16 4000x3000 to display", none, [&] { RenderOptions o; render(deep, o, out); });
    bench("render16 4000x3000 at 0.25", none, [&] { RenderOptions o; o.scale = 0.25; render16(deep, o, out16); });
    bench("render16 1024x768 region at 1", none, [&] { RenderOptions o; o.region = {1500, 1100, 1024, 768}; render16(deep, o, out16); });

    // A 1200x900 layer with a drop shadow, stroke and bevel, and a gradient shape layer, at 8 and at 16 bits.
    {
        Document styledDoc(1600, 1200);
        styledDoc.layers.push_back(Layer(Asset::make(busy(1600, 1200, 3, false), "base"), Point(0, 0)));
        Layer top(Asset::make(busy(1200, 900, 4, true), "styled"), Point(200, 150));
        LayerStyle style;
        DropShadow shadow; shadow.distance = 10; shadow.size = 12; style.dropShadows.push_back(shadow);
        Stroke stroke; stroke.size = 4; style.strokes.push_back(stroke);
        Bevel bevel; bevel.size = 8; style.bevels.push_back(bevel);
        setLayerStyle(top, style);
        styledDoc.layers.push_back(top);
        VectorShape shape;
        shape.path = ellipsePath(Rect(300, 200, 900, 700));
        shape.fillPaint.kind = VectorPaint::Kind::Gradient;
        shape.fillPaint.gradient.colors = {{0, {255, 0, 0}, 0.5f}, {1, {0, 0, 255}, 0.5f}};
        shape.fillPaint.gradient.alphas = {{0, 1, 0.5f}, {1, 1, 0.5f}};
        shape.stroke.enabled = true;
        Layer shapeLayer(Asset::make(std::make_shared<Image>(1, 1), "shape"), Point(0, 0));
        setVectorShape(shapeLayer, styledDoc, shape);
        styledDoc.layers.push_back(shapeLayer);
        Document styledDeep = styledDoc;
        if (!convertSampleType(styledDeep, SampleType::U16)) { std::printf("16-bit conversion failed\n"); return 1; }
        Image styledOut;
        bench("styles+shape 1600x1200", none, [&] { RenderOptions o; render(styledDoc, o, styledOut); });
        bench("u16 styles+shape 1600x1200", none, [&] { RenderOptions o; render16(styledDeep, o, out16); });
    }

    // Colour management (P4): Convert to Profile's pixel pass at 8 and 16 bits, and the canvas's display transform.
    {
        auto toAdobe8 = transformBetween(srgbProfile(), builtinProfile(WorkingSpace::AdobeRGB), {}, PixelFormat::RGBA8, PixelFormat::RGBA8);
        auto toAdobe16 = transformBetween(srgbProfile(), builtinProfile(WorkingSpace::AdobeRGB), {}, PixelFormat::RGBA16, PixelFormat::RGBA16);
        auto display16 = transformBetween(srgbProfile(), builtinProfile(WorkingSpace::AdobeRGB), {}, PixelFormat::RGBA16, PixelFormat::RGBA8);
        Image16 work16;
        bench("convertImage 8-bit 4000x3000", fresh, [&] { convertImage(work, toAdobe8.get()); });
        bench("convertImage 16-bit 4000x3000", [&] { RenderOptions o; render16(deep, o, work16); }, [&] { convertImage(work16, toAdobe16.get()); });
        bench("render 4000x3000 through display", none, [&] { RenderOptions o; o.display = toAdobe8.get(); render(doc, o, out); });
        bench("render16 4000x3000 through display", none, [&] { RenderOptions o; o.display = display16.get(); render(deep, o, out); });
    }

    // Layer counts: each edit as the app makes it, one history step around the change (EditorSession's
    // beginEdit/endEdit), with a history already full of steps as after a while of work.
    for (int count : {1000, 5000, 10000}) {
        Document layered = manyLayers(count);
        DocumentHistory history;
        const Uuid middle = layered.layers[size_t(count / 2)].id;
        for (int i = 0; i < 100; i++) {
            history.begin("Hide", layered, middle);
            layered.find(middle)->visible = !layered.find(middle)->visible;
            history.end(layered, middle);
        }
        auto step = [&](const char* edit, const std::function<void()>& change) {
            const std::string name = "layers " + std::to_string(count) + ": " + edit;
            bench(name.c_str(), none, [&] { history.begin(edit, layered, middle); change(); history.end(layered, middle); });
        };
        int renamed = 0;
        step("rename", [&] { layered.find(middle)->name = "Renamed " + std::to_string(renamed++); });
        step("visibility", [&] { Layer* l = layered.find(middle); l->visible = !l->visible; });
        step("move", [&] {
            // The bottom pixel layer to the middle of the stack and back on the next run: a drag in the panel.
            const size_t from = 1, to = size_t(count / 2);
            if (renamed++ % 2) std::rotate(layered.layers.begin() + long(from), layered.layers.begin() + long(from) + 1, layered.layers.begin() + long(to) + 1);
            else std::rotate(layered.layers.begin() + long(from), layered.layers.begin() + long(to), layered.layers.begin() + long(to) + 1);
        });
        step("beginEdit/endEdit", [] {});
        std::string name = "layers " + std::to_string(count) + ": undo+redo";
        bench(name.c_str(), none, [&] {
            if (auto back = history.undo()) layered = *back->document;
            if (auto again = history.redo()) layered = *again->document;
        });
        name = "layers " + std::to_string(count) + ": plan build";
        bench(name.c_str(), none, [&] { RenderPlan plan(layered, nullptr); plan.build(); });
        name = "layers " + std::to_string(count) + ": find each by id";
        size_t found = 0;
        bench(name.c_str(), none, [&] { for (const Layer& l : layered.layers) found += layered.indexOf(l.id) >= 0; });
        if (found == 0) std::printf("(no layers found)\n");
    }
    // Editing at 16 bits (P3a): the same photo converted, blurred and adjusted there.
    auto photo16 = widenImage(*photo);
    Image16 work16;
    auto fresh16 = [&] { work16 = *photo16; };
    for (double radius : {2.0, 20.0}) {
        std::string name = "u16 gaussian blur r" + std::to_string(int(radius));
        bench(name.c_str(), fresh16, [&] { FilterSettings s; s.radius = radius; applyFilter(FilterKind::GaussianBlur, work16, s); });
    }
    auto adjust16 = [&](const char* name, AdjustmentSettings s) { bench(name, fresh16, [&] { applyAdjustment(s, work16, all, 1); }); };
    adjust16("u16 levels", levels);
    adjust16("u16 curves", curves);
    adjust16("u16 hue/saturation", hsv);

    // Painting at 16 bits (P3b): the brush strokes above on the same canvas converted.
    Layer canvas16(Asset::make(Image16Ptr(widenImage(*canvas.asset->image.u8())), "canvas"), Point(0, 0));
    for (double hardness : {1.0, 0.3}) {
        BrushSettings s;
        s.diameter = 80;
        s.hardness = hardness;
        s.opacity = 0.8;
        s.red = 0.9;
        std::string name = std::string("u16 brush stroke d80 hardness ") + (hardness == 1 ? "1" : "0.3");
        bench(name.c_str(), none, [&] {
            BrushStroke stroke(canvas16, false, s, Size(4000, 3000), SampleType::U16, nullptr);
            stroke.appendAll(path);
            stroke.flush();
            auto commit = stroke.commit();
        });
    }
    return 0;
}
