// The P0 benches for the high-bit-depth work (docs/high-bit-depth-plan.md): render of a large layered document, a
// brush stroke, a gaussian blur and a few adjustments, each the median of N runs. Not a test (not in ctest):
//   build/tests/bench_core [runs] [filter]
// Results from this machine are kept in docs/benchmarks.md; later phases are held to them.
#include "compositor/adjustments.h"
#include "compositor/brush.h"
#include "compositor/document.h"
#include "compositor/filters.h"
#include "compositor/parallel.h"
#include "compositor/render.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using namespace compositor;

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

} // namespace

int main(int argc, char** argv) {
    Bench bench{argc > 1 ? std::max(1, std::atoi(argv[1])) : 9, argc > 2 ? argv[2] : ""};
    std::printf("bench_core: %d worker threads, median of %d runs\n", workerCount(), bench.runs);
    auto none = [] {};

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
    return 0;
}
