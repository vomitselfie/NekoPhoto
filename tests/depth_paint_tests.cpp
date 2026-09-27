// Painting and retouching a 16-bit document (docs/high-bit-depth-plan.md, P3b). Each tool is held to its 8-bit path:
// an 8-bit image converted to 16 bits, painted with the same stroke, then reduced to 8 bits, lands within one level of
// the 8-bit result, or the test says where and by how much it does not, and why. The brush engines' own presets are
// compared the same way, fixture by fixture, in brush_parity's 16-bit section.
// COMPOSITOR_REPORT_U16_CALIBRATION=1 prints the worst difference and the share beyond a level for each.
#include "check.h"
#include "compositor/blur.h"
#include "compositor/brush.h"
#include "compositor/depth.h"
#include "compositor/heal.h"
#include "compositor/render.h"
#include "compositor/tipbrush.h"
#include "compositor/toning.h"
#include "compositor/warpstroke.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <set>
#include <string>

using namespace compositor;

namespace {

uint32_t mix(uint32_t v) { v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; v ^= v >> 16; return v; }

/// A busy opaque image on the left half and soft, partly transparent paint on the right (as depth_edit_tests).
std::shared_ptr<Image> busyImage(int w, int h, uint32_t seed = 1) {
    auto image = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint32_t n = mix(uint32_t(y * w + x) * 2654435761u + seed);
            uint8_t* p = image->pixel(x, y);
            const int r = std::clamp(255 * x / (w - 1) + int(n & 63) - 32, 0, 255);
            const int g = std::clamp(255 * y / (h - 1) + int((n >> 8) & 63) - 32, 0, 255);
            const int b = std::clamp(255 - 255 * (x + y) / (w + h - 2) + int((n >> 16) & 63) - 32, 0, 255);
            int a = 255;
            if (x >= w / 2) {
                const double dx = (x - w * 0.75) / (w * 0.25), dy = (y - h * 0.5) / (h * 0.5);
                a = std::clamp(int(std::lround(255 * (1.2 - std::sqrt(dx * dx + dy * dy)))), 0, 255);
            }
            p[0] = uint8_t((r * a + 127) / 255); p[1] = uint8_t((g * a + 127) / 255); p[2] = uint8_t((b * a + 127) / 255); p[3] = uint8_t(a);
        }
    return image;
}

/// A smooth opaque image (for the healers, which copy texture from around a spot).
std::shared_ptr<Image> smoothImage(int w, int h) {
    auto image = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t* p = image->pixel(x, y);
            p[0] = uint8_t(40 + 150 * x / w + 10 * std::sin(y * 0.3));
            p[1] = uint8_t(60 + 120 * y / h);
            p[2] = uint8_t(90 + 60 * std::sin(x * 0.05 + y * 0.07));
            p[3] = 255;
        }
    return image;
}

struct Apart { int worst = 0; double beyondOne = 0; long samples = 0; };

Apart apart(const Image& eight, const Image16& deep) {
    auto reduced = narrowImage(deep);
    Apart out;
    if (reduced->width() != eight.width() || reduced->height() != eight.height()) return {256, 1, 0};
    long beyond = 0;
    for (int y = 0; y < eight.height(); y++)
        for (int x = 0; x < eight.width(); x++) {
            const uint8_t* a = eight.pixel(x, y);
            const uint8_t* b = reduced->pixel(x, y);
            for (int c = 0; c < 4; c++, out.samples++) {
                const int d = std::abs(int(a[c]) - int(b[c]));
                out.worst = std::max(out.worst, d);
                beyond += d > 1;
            }
        }
    out.beyondOne = out.samples ? double(beyond) / double(out.samples) : 0;
    return out;
}

Apart apart(const GrayImage& eight, const Gray16& deep) {
    auto reduced = narrowGray(deep);
    Apart out;
    if (reduced->width() != eight.width() || reduced->height() != eight.height()) return {256, 1, 0};
    long beyond = 0;
    for (int y = 0; y < eight.height(); y++)
        for (int x = 0; x < eight.width(); x++, out.samples++) {
            const int d = std::abs(int(eight.at(x, y)) - int(reduced->at(x, y)));
            out.worst = std::max(out.worst, d);
            beyond += d > 1;
        }
    out.beyondOne = out.samples ? double(beyond) / double(out.samples) : 0;
    return out;
}

void report(const std::string& what, const Apart& a) {
    if (std::getenv("COMPOSITOR_REPORT_U16_CALIBRATION"))
        std::fprintf(stderr, "  %-40s worst %3d, beyond a level %.4f%%\n", what.c_str(), a.worst, a.beyondOne * 100);
}

/// Within a level, or for a soft tip within the 8-bit stroke's own coverage rounding: a soft tip builds its coverage
/// up by screen, dab over dab, and at 8 bits every step rounds to 1/255, so a pixel under many dabs of the soft rim
/// collects up to a few levels of error that the 16-bit coverage does not (soft_stroke_differences_are_the_eight_bit_
/// coverage_rounding measures both against the exact coverage: the 8-bit stroke is up to 3 levels off in about 0.9% of
/// its pixels, the 16-bit one within a level). Measured between the depths: at most 4 levels, in at most 0.72% of the
/// samples (a soft tip at full opacity on a transparent layer, where every sample is the coverage itself).
bool withinALevel(const Apart& d, bool soft) {
    if (!soft) return d.worst <= 1;
    return d.worst <= 4 && d.beyondOne < 0.01;
}

Layer layer8(const std::shared_ptr<Image>& image) { return Layer(Asset::make(ImagePtr(image), "Layer"), Point(0, 0)); }
Layer layer16(const std::shared_ptr<Image>& image) { return Layer(Asset::make(Image16Ptr(widenImage(*image)), "Layer"), Point(0, 0)); }

/// A wavy stroke across a w x h canvas.
std::vector<Point> wave(int w, int h, double step = 3) {
    std::vector<Point> points;
    for (double x = 8; x < w - 8; x += step) points.push_back({x, h / 2.0 + h * 0.3 * std::sin(x / 17.0)});
    return points;
}

/// Paints `points` with `settings` on both layers and compares the layers as the strokes leave them.
Apart paintBoth(const Layer& eight, const Layer& deep, bool mask, BrushSettings settings, Size canvas, const std::vector<Point>& points,
                const GrayImage* selection = nullptr, const std::function<void(BrushStroke&, BrushStroke&)>& setup = {}) {
    BrushStroke a(eight, mask, settings, canvas, selection);
    std::shared_ptr<Gray16> selection16 = selection ? widenGray(*selection) : nullptr;
    BrushStroke b(deep, mask, settings, canvas, SampleType::U16, selection16.get());
    if (!a.isValid() || !b.isValid()) return {256, 1, 0};
    if (setup) setup(a, b);
    for (const Point& p : points) { a.append(p); b.append(p); }
    a.flush(); b.flush();
    auto ca = a.commit();
    auto cb = b.commit();
    if (mask) {
        if (!ca.mask || !cb.mask || !ca.mask->image.u8() || !cb.mask->image.u16()) return {256, 1, 0};
        return apart(*ca.mask->image.u8(), *cb.mask->image.u16());
    }
    if (!ca.asset || !cb.asset || !ca.asset->image.u8() || !cb.asset->image.u16()) return {256, 1, 0};
    if (!(ca.transform.origin == cb.transform.origin) || !(ca.transform.size == cb.transform.size)) return {256, 1, 0};
    return apart(*ca.asset->image.u8(), *cb.asset->image.u16());
}

BrushSettings ink(double diameter, double hardness, double opacity) {
    BrushSettings s;
    s.diameter = diameter; s.hardness = hardness; s.opacity = opacity;
    s.red = 0.8; s.green = 0.25; s.blue = 0.1;
    return s;
}

/// A soft selection: a ramp across the canvas, clear on the left.
GrayImage rampSelection(int w, int h) {
    GrayImage selection(w, h, 0);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) selection.at(x, y) = uint8_t(std::clamp((x - w / 4) * 255 / (w / 2), 0, 255));
    return selection;
}

} // namespace

/// The round brush: hard and soft tips at several opacities, on an image and on a transparent layer, through a soft
/// selection, and on a layer turned on the canvas (the per-pixel dab rather than the stamps).
TEST_CASE(sixteen_bit_round_brush_matches_eight_bit) {
    const int W = 200, H = 120;
    auto image = busyImage(W, H, 3);
    const Layer eight = layer8(image), deep = layer16(image);
    auto blank = std::make_shared<Image>(W, H);
    const Layer blank8 = layer8(blank), blank16 = layer16(blank);
    const GrayImage selection = rampSelection(W, H);
    for (double hardness : {1.0, 0.6, 0.0})
        for (double opacity : {1.0, 0.55}) {
            const std::string name = "round h" + std::to_string(hardness).substr(0, 3) + " o" + std::to_string(opacity).substr(0, 4);
            for (int variant = 0; variant < 3; variant++) {
                const Apart d = variant == 0 ? paintBoth(eight, deep, false, ink(24, hardness, opacity), Size(W, H), wave(W, H))
                              : variant == 1 ? paintBoth(blank8, blank16, false, ink(24, hardness, opacity), Size(W, H), wave(W, H))
                                             : paintBoth(eight, deep, false, ink(24, hardness, opacity), Size(W, H), wave(W, H), &selection);
                report(name + (variant == 0 ? " on image" : variant == 1 ? " on blank" : " through selection"), d);
                CHECK(withinALevel(d, hardness < 1));
            }
        }
    // A turned layer: the grid does not line up with the document, so every dab is worked out per pixel.
    Layer turned8 = eight, turned16 = deep;
    turned8.transform.rotation = turned16.transform.rotation = 20;
    for (double hardness : {1.0, 0.3}) {
        const Apart d = paintBoth(turned8, turned16, false, ink(18, hardness, 0.8), Size(W, H), wave(W, H));
        report("round on a turned layer h" + std::to_string(hardness).substr(0, 3), d);
        CHECK(withinALevel(d, hardness < 1));
    }
}

/// The eraser, and painting a layer mask (and so the Quick Mask, which is a mask being painted).
TEST_CASE(sixteen_bit_eraser_and_mask_painting_match_eight_bit) {
    const int W = 180, H = 110;
    auto image = busyImage(W, H, 4);
    const Layer eight = layer8(image), deep = layer16(image);
    for (double hardness : {1.0, 0.2}) {
        BrushSettings s = ink(30, hardness, 0.7);
        s.erasing = true;
        const Apart d = paintBoth(eight, deep, false, s, Size(W, H), wave(W, H));
        report("eraser h" + std::to_string(hardness).substr(0, 3), d);
        CHECK(withinALevel(d, hardness < 1));
    }
    auto mask = std::make_shared<GrayImage>(W, H, 0);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) mask->at(x, y) = uint8_t((x * 3 + y) % 256);
    Layer masked8 = eight, masked16 = deep;
    LayerMask m8; m8.asset = MaskAsset::make(GrayPtr(mask));
    LayerMask m16; m16.asset = MaskAsset::make(Gray16Ptr(widenGray(*mask)));
    masked8.mask = m8; masked16.mask = m16;
    const GrayImage selection = rampSelection(W, H);
    for (double value : {0.0, 1.0})
        for (double hardness : {1.0, 0.4}) {
            BrushSettings s = ink(26, hardness, 0.8);
            s.maskValue = value;
            const Apart d = paintBoth(masked8, masked16, true, s, Size(W, H), wave(W, H));
            report("mask " + std::string(value > 0 ? "reveal" : "hide") + " h" + std::to_string(hardness).substr(0, 3), d);
            CHECK(withinALevel(d, hardness < 1));
            const Apart through = paintBoth(masked8, masked16, true, s, Size(W, H), wave(W, H), &selection);
            report("mask through a selection", through);
            CHECK(withinALevel(through, hardness < 1));
        }
}

/// A tip brush (the stamp engine for imported brushes) with a textured tip, a grain, scatter, jitter and density by
/// spacing, seeded alike: the dynamics are depth-free, only the coverage write differs.
TEST_CASE(sixteen_bit_tip_brush_matches_eight_bit) {
    const int W = 220, H = 120;
    auto image = busyImage(W, H, 5);
    const Layer eight = layer8(image), deep = layer16(image);
    BrushTip tip;
    auto shape = std::make_shared<GrayImage>(40, 40, 0);
    for (int y = 0; y < 40; y++)
        for (int x = 0; x < 40; x++) {
            const double r = std::hypot(x + 0.5 - 20, y + 0.5 - 20) / 20;
            shape->at(x, y) = uint8_t(std::clamp(255 * (1 - r * r) * (0.6 + 0.4 * ((x * 7 + y * 3) % 5) / 4.0), 0.0, 255.0));
        }
    tip.shape = shape;
    auto grain = std::make_shared<GrayImage>(16, 16, 0);
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) grain->at(x, y) = uint8_t(mix(uint32_t(y * 16 + x)) & 255);
    tip.grain = grain;
    tip.grainDepth = 0.5;
    tip.spacing = 0.1;
    tip.flow = 0.45;
    tip.scatter = 0.3;
    tip.dynamics = {dynamicsMapping(DynamicsInput::Random, DynamicsTarget::Size, 1, -0.3), dynamicsMapping(DynamicsInput::Pressure, DynamicsTarget::Opacity, 0.3, 0.7)};
    for (bool density : {false, true}) {
        tip.densityBySpacing = density;
        BrushSettings s = ink(28, 1, 0.9);
        BrushStroke a(eight, false, s, Size(W, H));
        BrushStroke b(deep, false, s, Size(W, H), SampleType::U16, nullptr);
        TipStroke ta(a, tip, 28, 77), tb(b, tip, 28, 77);
        REQUIRE(ta.isValid() && tb.isValid());
        BrushSampleTrack trackA, trackB;
        int i = 0;
        for (const Point& p : wave(W, H, 2)) {
            BrushSample sample;
            sample.position = p;
            sample.time = i++ / 120.0;
            sample.pressure = 0.3 + 0.7 * std::fabs(std::sin(i * 0.05));
            sample.stylus = true;
            ta.strokeTo(trackA.add(sample));
            tb.strokeTo(trackB.add(sample));
        }
        a.flush(); b.flush();
        const Apart d = apart(*a.previewImage(), *b.previewImage16());
        report(std::string("tip brush") + (density ? " with density by spacing" : ""), d);
        // Each dab builds the coverage up towards its opacity with a rounding step, as the round tip's screen does:
        // the same 8-bit accumulation, and (with density by spacing) its 256-entry table against one per 15-bit level.
        CHECK(withinALevel(d, true));
    }
}

/// Clone Stamp from a document-sized sample (a replacing clone, as Smudge and Liquify paint back, too), the Healing
/// Brush from a sample, and the processed sources the Blur, Sharpen, Dodge, Burn and Sponge tools paint through.
TEST_CASE(sixteen_bit_clone_and_processed_strokes_match_eight_bit) {
    const int W = 200, H = 120;
    auto image = busyImage(W, H, 6);
    const Layer eight = layer8(image), deep = layer16(image);
    auto sample = busyImage(W, H, 9);
    auto sample16 = widenImage(*sample);
    for (bool replaces : {false, true}) {
        const Apart d = paintBoth(eight, deep, false, ink(22, 0.5, 0.9), Size(W, H), wave(W, H), nullptr, [&](BrushStroke& a, BrushStroke& b) {
            a.setClone(CloneSource{sample, {17, -9}, nullptr}, replaces);
            CloneSource c16;
            c16.image16 = sample16;
            c16.offset = {17, -9};
            b.setClone(c16, replaces);
        });
        report(replaces ? "clone (replacing)" : "clone stamp", d);
        CHECK(withinALevel(d, true));
    }
    // The Healing Brush: the sample's texture, its tone matched to the edge.
    auto smooth = smoothImage(W, H);
    const Layer smooth8 = layer8(smooth), smooth16 = layer16(smooth);
    auto sampleSmooth = smoothImage(W, H);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) sampleSmooth->pixel(x, y)[1] = uint8_t(std::clamp(sampleSmooth->pixel(x, y)[1] + int(mix(uint32_t(x * 31 + y)) % 17) - 8, 0, 255));
    BrushSettings heal = ink(20, 0.8, 1);
    heal.healing = true;
    const Apart healed = paintBoth(smooth8, smooth16, false, heal, Size(W, H), wave(W, H), nullptr, [&](BrushStroke& a, BrushStroke& b) {
        a.setClone(CloneSource{sampleSmooth, {5, 20}, nullptr});
        CloneSource c16;
        c16.image16 = widenImage(*sampleSmooth);
        c16.offset = {5, 20};
        b.setClone(c16);
    });
    report("healing brush", healed);
    CHECK(withinALevel(healed, true));

    // Processed sources, made a tile at a time.
    Document doc8(W, H), doc16(W, H);
    doc8.layers = {eight};
    doc16.layers = {deep};
    doc16.sampleType = SampleType::U16;
    struct Tool { const char* name; std::function<void(Image&)> f8; std::function<void(Image16&)> f16; int margin; };
    ToningSettings dodge; dodge.kind = ToningKind::Dodge;
    ToningSettings burn; burn.kind = ToningKind::Burn; burn.range = ToneRange::Shadows;
    ToningSettings sponge; sponge.kind = ToningKind::Sponge; sponge.saturate = true;
    ToningSettings unprotected; unprotected.kind = ToningKind::Dodge; unprotected.protectTones = false; unprotected.range = ToneRange::Highlights;
    const Tool tools[] = {
        {"dodge", [&](Image& i) { toneImage(i, dodge); }, [&](Image16& i) { toneImage(i, dodge); }, 0},
        {"burn shadows", [&](Image& i) { toneImage(i, burn); }, [&](Image16& i) { toneImage(i, burn); }, 0},
        {"sponge saturate", [&](Image& i) { toneImage(i, sponge); }, [&](Image16& i) { toneImage(i, sponge); }, 0},
        {"dodge highlights, tones unprotected", [&](Image& i) { toneImage(i, unprotected); }, [&](Image16& i) { toneImage(i, unprotected); }, 0},
        {"sharpen", [](Image& i) { sharpenImage(i); }, [](Image16& i) { sharpenImage(i); }, 3},
        {"blur", [](Image& i) { gaussianBlur(i, 2.4); }, [](Image16& i) { gaussianBlur(i, 2.4); }, 8},
    };
    for (const Tool& tool : tools) {
        const Apart d = paintBoth(eight, deep, false, ink(30, 0.3, 0.5), Size(W, H), wave(W, H), nullptr, [&](BrushStroke& a, BrushStroke& b) {
            a.setClone(CloneSource{nullptr, {0, 0}, tiledProcessedDocument(doc8, tool.f8, tool.margin)});
            CloneSource c16;
            c16.tiled16 = tiledProcessedDocument16(doc16, tool.f16, tool.margin);
            b.setClone(c16);
        });
        report(std::string(tool.name) + " tool", d);
        CHECK(withinALevel(d, true));
    }
}

/// Spot Healing in each of its modes, and the Patch tool's heal from a shifted copy.
TEST_CASE(sixteen_bit_spot_healing_and_patch_match_eight_bit) {
    const int W = 160, H = 120;
    auto image = smoothImage(W, H);
    for (int y = 50; y < 64; y++) for (int x = 60; x < 90; x++) { uint8_t* p = image->pixel(x, y); p[0] = 250; p[1] = 20; p[2] = 30; }
    const Layer eight = layer8(image), deep = layer16(image);
    std::vector<Point> spot;
    for (double x = 62; x <= 88; x += 2) spot.push_back({x, 57});
    for (int mode : {0, 1, 2}) {
        BrushSettings s = ink(24, 0.9, 1);
        s.healing = true;
        s.healingMode = mode;
        s.healingSeed = 42;
        const Apart d = paintBoth(eight, deep, false, s, Size(W, H), spot);
        report("spot healing mode " + std::to_string(mode), d);
        CHECK(d.worst <= 1);
    }
    // Patch: the selection healed from the pixels 40 to the right.
    GrayImage selection(W, H, 0);
    for (int y = 46; y < 68; y++) for (int x = 56; x < 94; x++) selection.at(x, y) = 255;
    Image source(W, H);
    for (int y = 0; y < H; y++) for (int x = 0; x + 40 < W; x++) std::memcpy(source.pixel(x, y), image->pixel(x + 40, y), 4);
    Image patched = *image;
    healFrom(patched, source, selection, 1.0f);
    auto patched16 = widenImage(*image);
    healFrom(*patched16, *widenImage(source), *widenGray(selection), 1.0f);
    const Apart d = apart(patched, *patched16);
    report("patch", d);
    CHECK(d.worst <= 1);
}

/// Smudge and Liquify on the layer at document size.
TEST_CASE(sixteen_bit_smudge_and_liquify_match_eight_bit) {
    const int W = 180, H = 110;
    auto image = busyImage(W, H, 8);
    for (WarpMode mode : {WarpMode::Smudge, WarpMode::Liquify}) {
        auto a = std::make_shared<Image>(*image);
        WarpStroke eight(a, mode, 30, 0.5, 0.7);
        WarpStroke deep(widenImage(*image), mode, 30, 0.5, 0.7);
        for (const Point& p : wave(W, H, 4)) { eight.append(p); deep.append(p); }
        const Apart d = apart(*eight.image(), *deep.image16());
        report(mode == WarpMode::Smudge ? "smudge" : "liquify", d);
        // Smudge reads back what it has just painted: the 8-bit stroke rounds the pixels at every dab and picks the
        // rounded colour up again, the 16-bit one at 15 bits, so the carried colour drifts apart by up to two levels in a
        // few samples along the path (measured: 2 levels, 0.04%). Liquify resamples the untouched original: within a level.
        CHECK(mode == WarpMode::Liquify ? d.worst <= 1 : (d.worst <= 2 && d.beyondOne < 0.001));
    }
}

/// The Gradient tool on pixels and on a mask, linear and radial, two stops and a multi-stop gradient with opacity
/// stops, through a soft selection; and a smooth 16-bit gradient keeps far more than 256 levels.
TEST_CASE(sixteen_bit_gradients_match_eight_bit) {
    const int W = 240, H = 90;
    auto image = busyImage(W, H, 10);
    const Layer eight = layer8(image), deep = layer16(image);
    const GrayImage selection = rampSelection(W, H);
    GradientStops two;
    two.start[0] = 0.1f; two.start[1] = 0.4f; two.start[2] = 0.9f; two.start[3] = 1;
    two.end[0] = 1; two.end[1] = 0.8f; two.end[2] = 0.2f; two.end[3] = 0.2f;
    GradientStops multi;
    multi.colors = {{0, {1, 0, 0}, 0.5f}, {0.4f, {0, 1, 0.2f}, 0.3f}, {1, {0.1f, 0.1f, 1}, 0.7f}};
    multi.alphas = {{0, 1, 0.5f}, {0.6f, 0.3f, 0.5f}, {1, 1, 0.5f}};
    for (int shape : {0, 1})
        for (const GradientStops* stops : {&two, &multi})
            for (bool mask : {false, true})
                for (bool selected : {false, true}) {
                    Layer a8 = eight, a16 = deep;
                    if (mask) {
                        auto m = std::make_shared<GrayImage>(W, H, 200);
                        LayerMask m8; m8.asset = MaskAsset::make(GrayPtr(m));
                        LayerMask m16; m16.asset = MaskAsset::make(Gray16Ptr(widenGray(*m)));
                        a8.mask = m8; a16.mask = m16;
                    }
                    BrushStroke a(a8, mask, BrushSettings(), Size(W, H), selected ? &selection : nullptr);
                    auto sel16 = widenGray(selection);
                    BrushStroke b(a16, mask, BrushSettings(), Size(W, H), SampleType::U16, selected ? sel16.get() : nullptr);
                    a.fillGradientOver(shape, {20, 10}, {220, 70}, *stops, 0.85);
                    b.fillGradientOver(shape, {20, 10}, {220, 70}, *stops, 0.85);
                    const Apart d = mask ? apart(*a.previewMask(), *b.previewMask16()) : apart(*a.previewImage(), *b.previewImage16());
                    report(std::string("gradient ") + (shape ? "radial" : "linear") + (stops == &multi ? " multi-stop" : "") + (mask ? " on a mask" : "") + (selected ? " selected" : ""), d);
                    CHECK(d.worst <= 1);
                }
    // A long, gentle ramp: 256 levels at 8 bits, thousands at 16.
    auto flat = std::make_shared<Image>(4000, 2);
    BrushStroke ramp(layer16(flat), false, BrushSettings(), Size(4000, 2), SampleType::U16, nullptr);
    GradientStops grey;
    grey.start[0] = grey.start[1] = grey.start[2] = 0.2f; grey.start[3] = 1;
    grey.end[0] = grey.end[1] = grey.end[2] = 0.4f; grey.end[3] = 1;
    ramp.fillGradientOver(0, {0, 0}, {4000, 0}, grey, 1);
    std::set<int> levels;
    for (int x = 0; x < 4000; x++) levels.insert(ramp.previewImage16()->pixel(x, 0)[0]);
    if (std::getenv("COMPOSITOR_REPORT_U16_CALIBRATION")) std::fprintf(stderr, "  a 20%% to 40%% ramp over 4000 pixels: %zu levels at 16 bits (51 at 8)\n", levels.size());
    CHECK(levels.size() > 3000);
}

/// Moving selected pixels: lifted through a soft selection, moved by whole pixels and, on a scaled layer, between them.
TEST_CASE(sixteen_bit_pixel_move_matches_eight_bit) {
    const int W = 160, H = 100;
    auto image = busyImage(W, H, 11);
    GrayImage selection(W, H, 0);
    for (int y = 20; y < 70; y++) for (int x = 30; x < 110; x++) selection.at(x, y) = uint8_t(std::min({255, (x - 30) * 20, (y - 20) * 20}));
    auto selection16 = widenGray(selection);
    for (bool scaled : {false, true})
        for (bool duplicate : {false, true}) {
            Layer a8 = layer8(image), a16 = layer16(image);
            if (scaled) {
                a8.transform.size = a16.transform.size = Size(W * 0.75, H * 0.75);
            }
            BrushStroke a(a8, false, BrushSettings(), Size(W, H), &selection);
            BrushStroke b(a16, false, BrushSettings(), Size(W, H), SampleType::U16, selection16.get());
            REQUIRE(a.liftSelection() == b.liftSelection());
            a.moveLifted({13, -7}, duplicate);
            b.moveLifted({13, -7}, duplicate);
            auto ca = a.commit();
            auto cb = b.commit();
            REQUIRE(ca.asset && cb.asset);
            const Apart d = apart(*ca.asset->image.u8(), *cb.asset->image.u16());
            report(std::string("move pixels") + (scaled ? " on a scaled layer" : "") + (duplicate ? ", duplicating" : ""), d);
            CHECK(d.worst <= 1);
        }
}

/// Where a soft stroke differs between the depths, it is the 8-bit stroke that is off: a soft tip builds its coverage
/// up by screen, dab over dab, and the 8-bit coverage rounds to 1/255 at every step, so a pixel under the forty-odd dabs
/// of a soft stroke's rim collects those roundings. Held to the exact coverage (every dab's falloff in double
/// precision), the 16-bit stroke is within a level everywhere and the 8-bit one is not.
TEST_CASE(soft_stroke_differences_are_the_eight_bit_coverage_rounding) {
    const int W = 200, H = 60;
    auto blank = std::make_shared<Image>(W, H);
    for (bool stamped : {false, true}) {
        BrushSettings s = ink(24, 0.3, 1);
        s.stampedDabs = stamped;   // unstamped, every dab is worked out where the path puts it
        BrushStroke a(layer8(blank), false, s, Size(W, H));
        BrushStroke b(layer16(blank), false, s, Size(W, H), SampleType::U16, nullptr);
        for (Point p : {Point{20, 30.1}, Point{180, 30.1}}) { a.append(p); b.append(p); }
        a.flush(); b.flush();
        // The same dabs in double precision: every `spacing` from the first point (a stamp lands on the nearest quarter
        // pixel), the tip's falloff between its hard core and rim.
        const double radius = 12, inner = radius * 0.3, spacing = std::max(0.25, 24 * 0.025);
        const double cy = stamped ? std::floor(30.1 * 4 + 0.5) / 4 : 30.1;
        std::vector<double> centres;
        for (double x = 20; x <= 180 + 1e-9; x += spacing) centres.push_back(stamped ? std::floor(x * 4 + 0.5) / 4 : x);
        int worst8 = 0, worst16 = 0;
        long off8 = 0;
        auto reduced = narrowImage(*b.previewImage16());
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                double clear = 1;
                for (double cx : centres) {
                    const double dist = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
                    const double v = dist <= inner ? 1 : dist >= radius ? 0 : brushFalloff((dist - inner) / (radius - inner));
                    clear *= 1 - v;
                }
                const int exact = int(std::lround((1 - clear) * 255));
                const int d8 = std::abs(int(a.previewImage()->pixel(x, y)[3]) - exact);
                worst8 = std::max(worst8, d8);
                off8 += d8 > 1;
                worst16 = std::max(worst16, std::abs(int(reduced->pixel(x, y)[3]) - exact));
            }
        if (std::getenv("COMPOSITOR_REPORT_U16_CALIBRATION"))
            std::fprintf(stderr, "  soft stroke (%s) against its exact coverage: 8-bit worst %d (%.2f%% beyond a level), 16-bit worst %d\n",
                         stamped ? "stamped" : "per pixel", worst8, 100.0 * double(off8) / (W * H), worst16);
        CHECK(worst16 <= 1);
        CHECK(worst8 > worst16);
    }
}

/// A 16-bit stroke keeps its layer's 16 bits: pixels the brush does not reach are the layer's own, sample for sample.
TEST_CASE(sixteen_bit_stroke_leaves_untouched_pixels_alone) {
    auto deep = std::make_shared<Image16>(64, 64);
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            uint16_t* p = deep->pixel(x, y);
            p[0] = uint16_t(x * 500 + y); p[1] = uint16_t(y * 511); p[2] = 12345; p[3] = one16;
        }
    Layer layer(Asset::make(Image16Ptr(deep), "Deep"), Point(0, 0));
    BrushStroke stroke(layer, false, ink(10, 1, 1), Size(64, 64), SampleType::U16, nullptr);
    stroke.append({10, 10});
    stroke.append({20, 12});
    auto commit = stroke.commit();
    REQUIRE(commit.asset && commit.asset->image.u16());
    const Image16& out = *commit.asset->image.u16();
    int kept = 0, total = 0;
    for (int y = 40; y < 64; y++) for (int x = 40; x < 64; x++, total++) kept += std::memcmp(out.pixel(x, y), deep->pixel(x, y), 8) == 0;
    CHECK_EQ(kept, total);
    CHECK_EQ(int(out.pixel(15, 11)[0]), int(widen8(204)));   // red 0.8 at full coverage
}

TEST_MAIN()
