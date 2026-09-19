// Timings for the pixel pipeline on a 4000 x 3000 document, for checking the effect of kernel work.
// Not a test: `build/tests/compositor_bench [filter]` prints milliseconds per operation.
#include "compositor/adjustments.h"
#include "compositor/blend.h"
#include "compositor/document.h"
#include "compositor/filters.h"
#include "compositor/kernels.h"
#include "compositor/morphology.h"
#include "compositor/brush.h"
#include "compositor/warp.h"
#include "compositor/wand.h"
#include "compositor/resample.h"
#include "compositor/heal.h"
#include "compositor/inpaint.h"
#include "compositor/warpstroke.h"
#include "compositor/subject.h"
#include "compositor/render.h"
#include "compositor/selection.h"
extern "C" {
#include "HealPixels.h"
#include "ContentFill.h"
}
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <string>

using namespace compositor;

namespace {

Image busyImage(int w, int h, uint32_t seed = 1) {
    Image img(w, h);
    std::mt19937 rng(seed);
    for (int y = 0; y < h; y++) {
        uint8_t* p = img.row(y);
        for (int x = 0; x < w; x++, p += 4) {
            unsigned a = (x + y) % 97 < 90 ? 255 : uint8_t(rng() % 256);
            unsigned r = uint8_t((x * 3 + y) % 256), g = uint8_t((x + y * 5) % 256), b = uint8_t((x ^ y) % 256);
            p[0] = uint8_t((r * a + 127) / 255); p[1] = uint8_t((g * a + 127) / 255); p[2] = uint8_t((b * a + 127) / 255); p[3] = uint8_t(a);
        }
    }
    return img;
}

double timeMs(const std::function<void()>& body, int repeats = 3) {
    double best = 1e300;
    for (int i = 0; i < repeats; i++) {
        auto t0 = std::chrono::steady_clock::now();
        body();
        auto t1 = std::chrono::steady_clock::now();
        best = std::min(best, std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    return best;
}

} // namespace

int main(int argc, char** argv) {
    std::string only = argc > 1 ? argv[1] : "";
    const int W = 4000, H = 3000;
    Image base = busyImage(W, H);
    auto want = [&](const char* name) { return only.empty() || std::strstr(name, only.c_str()) != nullptr; };
    auto report = [&](const char* name, double ms) { std::printf("%-34s %8.1f ms\n", name, ms); };

    if (want("levels")) {
        AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::Levels);
        s.levels.ranges[0].black = 20; s.levels.ranges[0].gamma = 1.3;
        Image img = base;
        report("levels (LUT kernel)", timeMs([&] { img = base; applyAdjustment(s, img, Rect(0, 0, W, H), 1); }));
    }
    if (want("hue")) {
        AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::HueSaturation);
        s.hsv.current().hue = 40; s.hsv.current().saturation = 20;
        Image img = base;
        report("hue/saturation (cube)", timeMs([&] { img = base; applyAdjustment(s, img, Rect(0, 0, W, H), 1); }));
    }
    if (want("gradient")) {
        AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::GradientMap);
        Image img = base;
        report("gradient map", timeMs([&] { img = base; applyAdjustment(s, img, Rect(0, 0, W, H), 1); }));
    }
    if (want("invert")) {
        Image img = base;
        report("invert", timeMs([&] { img = base; applyInvert(img); }));
    }
    if (want("noise")) {
        FilterSettings s; s.amount = 30; s.gaussian = true;
        Image img = base;
        report("add noise (gaussian)", timeMs([&] { img = base; applyFilter(FilterKind::AddNoise, img, s, 1, 7); }));
    }
    if (want("lens")) {
        FilterSettings s; s.distortion = -35;
        Image img = base;
        report("lens correction", timeMs([&] { img = base; applyFilter(FilterKind::LensCorrection, img, s, 1, 0); }));
    }
    if (want("blur")) {
        for (double sigma : {2.0, 20.0, 200.0}) {
            FilterSettings s; s.radius = sigma;
            Image img = base;
            char name[64]; std::snprintf(name, sizeof name, "gaussian blur sigma %.0f", sigma);
            report(name, timeMs([&] { img = base; applyFilter(FilterKind::GaussianBlur, img, s, 1, 0); }, 2));
        }
    }
    if (want("motion")) {
        for (double d : {10.0, 100.0}) {
            FilterSettings s; s.distance = d; s.angle = 30;
            Image img = base;
            char name[64]; std::snprintf(name, sizeof name, "motion blur distance %.0f", d);
            report(name, timeMs([&] { img = base; applyFilter(FilterKind::MotionBlur, img, s, 1, 0); }, 1));
        }
    }
    if (want("composite")) {
        // Five layers over a 1080p view of the document, the interactive redraw case.
        Document doc(W, H);
        for (int i = 0; i < 5; i++) {
            Layer layer(Asset::make(std::make_shared<Image>(busyImage(2000, 1500, uint32_t(i + 2))), "L"), Point(300.0 * i, 200.0 * i));
            layer.transform.rotation = i == 2 ? 12 : 0;
            layer.opacity = i == 3 ? 0.6 : 1;
            layer.blendMode = i == 4 ? BlendMode::Multiply : BlendMode::Normal;
            doc.layers.push_back(layer);
        }
        Image out(1920, 1440);
        RenderOptions o; o.region = doc.rect(); o.scale = 1920.0 / W;
        report("composite 5 layers -> 1920 px", timeMs([&] { render(doc, o, out); }, 5));
        Image flat(W, H);
        RenderOptions full; full.region = doc.rect(); full.scale = 1;
        report("flatten 5 layers at 4000x3000", timeMs([&] { render(doc, full, flat); }, 2));
    }
    if (want("warp")) {
        auto layer = std::make_shared<Image>(busyImage(2000, 1500, 3));
        LayerTransform t(Point(100, 100), Size(2000, 1500));
        Corners trap = {Point{100, 100}, Point{2300, 300}, Point{2100, 1900}, Point{150, 1700}};
        report("warp 2000x1500 perspective", timeMs([&] { (void)warpImage(layer, t, trap, 0); }));
        Corners same = cornersOf(t);
        report("warp 2000x1500 identity corners", timeMs([&] { (void)warpImage(layer, t, same, 0); }));
        Document doc(W, H);
        doc.layers.push_back(Layer(Asset::make(std::make_shared<Image>(base), "L"), Point(0, 0)));
        report("image size 4000x3000 -> 2000x1500", timeMs([&] { Document d = doc; resizeDocument(d, 2000, 1500, 72, Sampling::High); }, 2));
    }
    if (want("resample")) {
        report("halve 4000x3000 (mip level)", timeMs([&] { (void)halveImage(base); }));
        report("resample -> 2000x1500 lanczos3", timeMs([&] { (void)resampleAxisAligned(base, 2000, 1500, 1, 2, 1, 2, ResampleFilter::Lanczos3); }));
        report("resample -> 2000x1500 triangle", timeMs([&] { (void)resampleAxisAligned(base, 2000, 1500, 1, 2, 1, 2, ResampleFilter::Triangle); }));
        report("resample -> 6000x4500 lanczos3", timeMs([&] { (void)resampleAxisAligned(base, 6000, 4500, 1.0 / 3, 2.0 / 3, 1.0 / 3, 2.0 / 3, ResampleFilter::Lanczos3); }, 2));
    }
    if (want("heal")) {
        for (int r : {20, 60, 150}) {
            GrayImage coverage(W, H, 0);
            for (int y = H / 2 - r; y < H / 2 + r; y++) for (int x = W / 2 - r; x < W / 2 + r; x++) if (std::hypot(x - W / 2, y - H / 2) < r) coverage.at(x, y) = 255;
            Image img = base;
            char name[64];
            std::snprintf(name, sizeof name, "spot heal %d px content-aware", r * 2);
            report(name, timeMs([&] { img = base; spotHeal(img, coverage, 1.0f, 0, 1); }));
            std::snprintf(name, sizeof name, "spot heal %d px create texture", r * 2);
            report(name, timeMs([&] { img = base; spotHeal(img, coverage, 1.0f, 1, 1); }));
            std::snprintf(name, sizeof name, "spot heal %d px (C reference)", r * 2);
            report(name, timeMs([&] { img = base; spot_heal(img.data(), coverage.data(), size_t(W), size_t(H), size_t(img.stride()), 1.0f, 0, 1); }, r > 100 ? 1 : 2));
        }
    }
    if (want("fill")) {
        for (int side : {200, 600}) {
            GrayImage hole(W, H, 0);
            for (int y = H / 2 - side / 2; y < H / 2 + side / 2; y++) for (int x = W / 2 - side / 2; x < W / 2 + side / 2; x++) hole.at(x, y) = 255;
            Image img = base;
            char name[64];
            std::snprintf(name, sizeof name, "content fill %dx%d", side, side);
            report(name, timeMs([&] { img = base; contentFill(img, hole); }, side > 300 ? 1 : 2));
            if (side <= 200) {
                std::snprintf(name, sizeof name, "content fill %dx%d (C reference)", side, side);
                report(name, timeMs([&] { img = base; content_fill(img.data(), size_t(img.stride()), hole.data(), size_t(hole.stride()), W, H); }, 1));
            }
        }
    }
    if (want("liquify")) {
        for (double diameter : {100.0, 300.0}) {
            char name[64]; std::snprintf(name, sizeof name, "liquify %.0f px brush, 1000 px stroke", diameter);
            report(name, timeMs([&] {
                auto img = std::make_shared<Image>(base);
                WarpStroke w(img, WarpMode::Liquify, diameter, 0.5, 0.6);
                for (int i = 0; i <= 100; i++) w.append({500.0 + i * 10, 1500});
            }, 2));
        }
    }
    if (want("brush")) {
        Layer layer(Asset::make(std::make_shared<Image>(base), "L"), Point(0, 0));
        for (double diameter : {60.0, 500.0}) {
            for (double hardness : {1.0, 0.5}) {
                BrushSettings s; s.diameter = diameter; s.hardness = hardness; s.opacity = 0.8; s.red = 0.2; s.green = 0.9; s.blue = 0.3;
                char name[64]; std::snprintf(name, sizeof name, "brush %.0f px hardness %.1f, 1000 px stroke", diameter, hardness);
                report(name, timeMs([&] {
                    BrushStroke stroke(layer, false, s, Size(W, H), nullptr);
                    // As the canvas does: the dirty rect is taken after every pointer event.
                    for (int i = 0; i <= 50; i++) { stroke.append({500.0 + i * 20, 1500.0 + 200 * std::sin(i * 0.3)}); (void)stroke.takeDirtyRect(); }
                    stroke.flush();
                    (void)stroke.commit();
                }, 2));
            }
        }
    }
    if (want("wand")) {
        GrayImage mask(W, H);
        report("magic wand contiguous tol 40", timeMs([&] { wandMask(base, W / 2, H / 2, 1, 40, true, mask); }));
        report("magic wand global tol 40", timeMs([&] { wandMask(base, W / 2, H / 2, 1, 40, false, mask); }));
    }
    if (want("matte")) {
        GrayImage mask(W, H, 0);
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) if ((x - 2000) * (x - 2000) + (y - 1500) * (y - 1500) < 1000 * 1000) mask.at(x, y) = 255;
        report("matte refine r=12 (full)", timeMs([&] { (void)guidedRefine(mask, base, 12, 0); }, 2));
        report("matte refine r=12 (limit 1400)", timeMs([&] { (void)guidedRefine(mask, base, 12, 1400); }, 2));
        MatteSettings s; s.refineEdges = 12; s.shiftEdge = 3;
        report("matte refine + shift edge", timeMs([&] { (void)refineMatte(mask, base, s, 0); }, 2));
    }
    if (want("selection")) {
        auto shape = rasterizeEllipse(Rect(200, 200, 3000, 2000), W, H, true);
        report("ellipse rasterize 3000x2000", timeMs([&] { shape = rasterizeEllipse(Rect(200, 200, 3000, 2000), W, H, true); }, 1));
        Selection sel; sel.coverage = shape;
        report("selection bounds", timeMs([&] { Selection s2 = sel; (void)s2.bounds(); }));
        report("selection expand 10 px", timeMs([&] { (void)resizeSelection(sel, 10); }, 1));
        report("selection contract 100 px", timeMs([&] { (void)resizeSelection(sel, -100); }, 1));
        report("selection smooth r=5", timeMs([&] { (void)smoothSelection(*shape, 5); }, 1));
        report("selection border 6", timeMs([&] { (void)borderSelection(*shape, 6); }, 1));
        report("selection feather 10", timeMs([&] { (void)featherSelection(*shape, 10); }, 1));
        auto other = rasterizeRect(Rect(1000, 1000, 2000, 1500), W, H, true);
        report("selection add", timeMs([&] { (void)combineSelection(sel, *other, SelectionMode::Add, true); }));
    }
    return 0;
}
