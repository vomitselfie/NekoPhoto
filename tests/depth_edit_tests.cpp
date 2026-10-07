// Editing a 16-bit document (docs/high-bit-depth-plan.md, P3a): adjustments, filters, selections and pixel edits at
// 0..32768. Each port is checked against its 8-bit path: on an 8-bit image converted to 16 bits, the 16-bit result
// reduced to 8 bits is within one level of the 8-bit result (or the test says where and why not), and a smooth 16-bit
// input keeps more than 256 levels where the 8-bit path would band.
// COMPOSITOR_REPORT_U16_CALIBRATION=1 prints the worst difference and the share beyond a level for each.
#include "check.h"
#include "compositor/adjustments.h"
#include "compositor/blur.h"
#include "compositor/depth.h"
#include "compositor/contentmove.h"
#include "compositor/filters.h"
#include "compositor/inpaint.h"
#include "compositor/morphology.h"
#include "compositor/seamcarve.h"
#include "compositor/selection.h"
#include "compositor/warp.h"
#include "compositor/warpmesh.h"
#include "compositor/render.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

using namespace compositor;

namespace {

uint32_t mix(uint32_t v) { v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; v ^= v >> 16; return v; }

/// A busy opaque image on the left half and soft, partly transparent paint on the right.
std::shared_ptr<Image> busyImage(int w, int h, uint32_t seed = 1) {
    auto image = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint32_t n = mix(uint32_t(y * w + x) * 2654435761u + seed);
            uint8_t* p = image->pixel(x, y);
            int r = std::clamp(255 * x / (w - 1) + int(n & 63) - 32, 0, 255);
            int g = std::clamp(255 * y / (h - 1) + int((n >> 8) & 63) - 32, 0, 255);
            int b = std::clamp(255 - 255 * (x + y) / (w + h - 2) + int((n >> 16) & 63) - 32, 0, 255);
            int a = 255;
            if (x >= w / 2) {
                const double dx = (x - w * 0.75) / (w * 0.25), dy = (y - h * 0.5) / (h * 0.5);
                a = std::clamp(int(std::lround(255 * (1.2 - std::sqrt(dx * dx + dy * dy)))), 0, 255);
            }
            p[0] = uint8_t((r * a + 127) / 255); p[1] = uint8_t((g * a + 127) / 255); p[2] = uint8_t((b * a + 127) / 255); p[3] = uint8_t(a);
        }
    return image;
}

struct Apart { int worst = 0; double beyondOne = 0; int worstOpaque = 0; };

Apart apart(const Image& eight, const Image16& deep) {
    auto reduced = narrowImage(deep);
    Apart out;
    if (reduced->width() != eight.width() || reduced->height() != eight.height()) return {256, 1, 256};
    size_t beyond = 0, total = 0;
    for (int y = 0; y < eight.height(); y++)
        for (int x = 0; x < eight.width(); x++) {
            const uint8_t* a = eight.pixel(x, y);
            const uint8_t* b = reduced->pixel(x, y);
            for (int c = 0; c < 4; c++, total++) {
                const int d = std::abs(int(a[c]) - int(b[c]));
                out.worst = std::max(out.worst, d);
                if (a[3] == 255) out.worstOpaque = std::max(out.worstOpaque, d);
                beyond += d > 1;
            }
        }
    out.beyondOne = total ? double(beyond) / double(total) : 0;
    return out;
}

void report(const std::string& what, const Apart& a) {
    if (std::getenv("COMPOSITOR_REPORT_U16_CALIBRATION"))
        std::fprintf(stderr, "  %-34s worst %3d (opaque %3d), beyond a level %.4f%%\n", what.c_str(), a.worst, a.worstOpaque, a.beyondOne * 100);
}

/// A .cube LUT that swaps and bends the channels, for Color Lookup.
ColorLookupSettings exampleLut() {
    std::string text = "TITLE \"test\"\nLUT_3D_SIZE 3\n";
    for (int b = 0; b < 3; b++)
        for (int g = 0; g < 3; g++)
            for (int r = 0; r < 3; r++) {
                char line[96];
                std::snprintf(line, sizeof line, "%.6f %.6f %.6f\n", std::pow(g / 2.0, 0.8), 0.1 + 0.8 * b / 2.0, 1 - r / 2.0 * 0.9);
                text += line;
            }
    ColorLookupSettings s;
    s.name = "test.cube"; s.format = "cube"; s.data = text;
    return s;
}

AdjustmentSettings exampleAdjustment(AdjustmentKind kind) {
    AdjustmentSettings s = AdjustmentSettings::defaults(kind);
    switch (kind) {
    case AdjustmentKind::Levels: s.levels.ranges[0] = {20, 1.3, 230, 10, 245}; s.levels.ranges[2].gamma = 0.8; break;
    case AdjustmentKind::Curves: s.curves.channels[0] = {{0, 0}, {64, 40}, {192, 220}, {255, 255}}; s.curves.channels[3] = {{0, 20}, {255, 230}}; break;
    case AdjustmentKind::HueSaturation: s.hsv.adjustments[0] = {25, 30, -10}; s.hsv.adjustments[1] = {-15, -40, 5}; break;
    case AdjustmentKind::Exposure: s.exposure = {0.7, -0.02, 1.2}; break;
    case AdjustmentKind::GradientMap: s.gradientMap.shadows = {0.1, 0, 0.3}; s.gradientMap.highlights = {1, 0.9, 0.5}; break;
    case AdjustmentKind::Grain: s.grain.amount = 40; s.grain.seed = 1234; break;
    case AdjustmentKind::BrightnessContrast: s.brightnessContrast = {30, 25, false}; break;
    case AdjustmentKind::Posterize: s.posterize.levels = 5; break;
    case AdjustmentKind::Threshold: s.threshold.level = 110; break;
    case AdjustmentKind::BlackWhite: s.blackWhite.tint = true; break;
    case AdjustmentKind::ColorBalance: s.colorBalance.ranges = {{{20, -10, 5}, {-15, 25, 10}, {5, 5, -30}}}; break;
    case AdjustmentKind::Vibrance: s.vibrance = {45, -20}; break;
    case AdjustmentKind::PhotoFilter: s.photoFilter.density = 60; break;
    case AdjustmentKind::ChannelMixer: s.channelMixer.rows[0] = {80, 30, -10, 5}; break;
    case AdjustmentKind::SelectiveColor: s.selectiveColor.ranges[0] = {-30, 20, 10, 0}; s.selectiveColor.ranges[7] = {10, 0, -15, 5}; break;
    case AdjustmentKind::ColorLookup: s.colorLookup = exampleLut(); break;
    case AdjustmentKind::Invert: break;
    }
    return s;
}

} // namespace

/// Every adjustment kind: an 8-bit image converted to 16 bits and adjusted there is its 8-bit adjustment within a level.
/// Opaque pixels are held to one level everywhere; partly transparent ones may differ by more on a small share of
/// samples, where the 8-bit kernels unpremultiply to a whole level (a pixel at alpha 20 has 20 colour steps) and the
/// 16-bit ones see the exact colour. Hue/Saturation on opaque pixels is held to three levels: at 8 bits it is
/// Photoshop's byte arithmetic (lightness, half-chroma and hue interpolant each rounded to a level, which a raised
/// saturation multiplies), at 16 bits the same model unrounded. Posterize and Threshold are exact on opaque pixels; at
/// partial alpha the 8-bit kernels decide on a colour already rounded (Posterize also interpolates its table between
/// steps there), so a few such samples land on the neighbouring step.
TEST_CASE(sixteen_bit_adjustments_match_eight_bit_within_a_level) {
    int failures = 0;
    auto source = busyImage(200, 150);
    for (int k = 0; k < adjustmentKindCount; k++) {
        const AdjustmentKind kind = AdjustmentKind(k);
        const AdjustmentSettings s = exampleAdjustment(kind);
        Image eight = *source;
        CHECK(applyAdjustment(s, eight, Rect(0, 0, 200, 150), 1));
        auto deep = widenImage(*source);
        CHECK(applyAdjustment(s, *deep, Rect(0, 0, 200, 150), 1));
        const Apart a = apart(eight, *deep);
        report(std::string("adjust/") + adjustmentKindName(kind), a);
        const int opaqueAllowed = kind == AdjustmentKind::HueSaturation ? 3 : 1;
        if (a.worstOpaque > opaqueAllowed || a.beyondOne > 0.01) {
            std::fprintf(stderr, "  %s: worst %d (opaque %d), %.3f%% beyond a level\n", adjustmentKindName(kind), a.worst, a.worstOpaque, a.beyondOne * 100);
            failures++;
        }
    }
    CHECK_EQ(failures, 0);
}

/// Adjustment layers, now drawn at 16 bits: a masked adjustment layer at 70% over two layers renders within a level of
/// the 8-bit document on all but a small share of samples (the 8-bit engine rounds the layer's coverage to 1/256 and
/// its result to bytes).
TEST_CASE(sixteen_bit_adjustment_layers_render_within_a_level) {
    int failures = 0;
    for (int k = 0; k < adjustmentKindCount; k++) {
        const AdjustmentKind kind = AdjustmentKind(k);
        Document doc(200, 150);
        doc.layers.push_back(Layer(Asset::make(busyImage(200, 150, 3), "base"), Point(0, 0)));
        doc.layers.push_back(Layer(Asset::make(busyImage(120, 100, 9), "paint"), Point(40, 25)));
        Layer adj("Adjustment", doc.size());
        adj.adjustment = exampleAdjustment(kind).toLayerAdjustment();
        adj.opacity = 0.7;
        auto mask = std::make_shared<GrayImage>(200, 150);
        for (int y = 0; y < 150; y++) for (int x = 0; x < 200; x++) mask->at(x, y) = uint8_t(std::clamp(300 - 2 * std::abs(x - 100) - 2 * std::abs(y - 75), 0, 255));
        adj.mask = LayerMask();
        adj.mask->asset = MaskAsset::make(mask);
        doc.layers.push_back(adj);
        Image eight;
        render(doc, RenderOptions(), eight);
        Document deepDoc = doc;
        std::string error;
        CHECK(convertSampleType(deepDoc, SampleType::U16, &error));
        Image16 deep;
        render16(deepDoc, RenderOptions(), deep);
        const Apart a = apart(eight, deep);
        report(std::string("adjust_layer/") + adjustmentKindName(kind), a);
        // Posterize and Threshold jump by a whole step where the 8-bit engine's rounding moves a value across an edge.
        const bool steps = kind == AdjustmentKind::Posterize || kind == AdjustmentKind::Threshold;
        if (a.beyondOne > (steps ? 0.02 : 0.01)) {
            std::fprintf(stderr, "  layer %s: worst %d, %.3f%% beyond a level\n", adjustmentKindName(kind), a.worst, a.beyondOne * 100);
            failures++;
        }
    }
    CHECK_EQ(failures, 0);
}

TEST_CASE(sixteen_bit_levels_keep_a_smooth_gradient_smooth) {
    // A 16-bit ramp over a narrow range stretched by Levels: 16 bits keep hundreds of distinct steps where 8 bits
    // have a few dozen.
    Image16 ramp(4096, 1);
    for (int x = 0; x < 4096; x++) {
        uint16_t* p = ramp.pixel(x, 0);
        p[0] = p[1] = p[2] = uint16_t(12000 + x * 2);   // 12000..20190: about 64 8-bit levels
        p[3] = uint16_t(one16);
    }
    AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::Levels);
    s.levels.ranges[0] = {93, 1.0, 158, 0, 255};
    CHECK(applyAdjustment(s, ramp, Rect(0, 0, 4096, 1), 1));
    std::set<int> levels;
    for (int x = 0; x < 4096; x++) levels.insert(ramp.pixel(x, 0)[0]);
    CHECK(levels.size() > 2000);
    // Monotonic, black to white.
    for (int x = 1; x < 4096; x++) CHECK(ramp.pixel(x, 0)[0] >= ramp.pixel(x - 1, 0)[0]);
}

TEST_CASE(identity_adjustments_leave_sixteen_bit_values_alone) {
    Image16 image(64, 8);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 64; x++) {
            uint16_t* p = image.pixel(x, y);
            p[3] = uint16_t(y == 0 ? one16 : 4096 * y);
            for (int c = 0; c < 3; c++) p[c] = uint16_t(std::min<uint32_t>(p[3], uint32_t(x * 511 + c * 7 + 1)));
        }
    for (AdjustmentKind kind : {AdjustmentKind::Levels, AdjustmentKind::Curves, AdjustmentKind::Exposure, AdjustmentKind::HueSaturation}) {
        Image16 copy = image;
        CHECK(applyAdjustment(AdjustmentSettings::defaults(kind), copy, Rect(0, 0, 64, 8), 1));
        CHECK(copy == image);
    }
    Image16 twice = image;
    applyInvert(twice);
    applyInvert(twice);
    CHECK(twice == image);
}

/// The built-in filters: the same within a level on opaque pixels; the blurs and Lens Correction on partly transparent
/// ones too, bar a small share where 8-bit rounding of premultiplied values between passes accumulates.
TEST_CASE(sixteen_bit_filters_match_eight_bit_within_a_level) {
    int failures = 0;
    auto source = busyImage(220, 160, 11);
    struct Case { std::string name; FilterKind kind; FilterSettings settings; uint32_t seed; };
    std::vector<Case> cases;
    for (double r : {0.6, 2.0, 7.5, 30.0}) { FilterSettings s; s.radius = r; cases.push_back({"gaussian " + std::to_string(r), FilterKind::GaussianBlur, s, 0}); }
    for (double a : {0.0, 30.0, -90.0}) { FilterSettings s; s.angle = a; s.distance = 15; cases.push_back({"motion " + std::to_string(a), FilterKind::MotionBlur, s, 0}); }
    { FilterSettings s; s.amount = 25; cases.push_back({"noise uniform", FilterKind::AddNoise, s, 42}); }
    { FilterSettings s; s.amount = 40; s.gaussian = true; s.monochromatic = true; cases.push_back({"noise gaussian mono", FilterKind::AddNoise, s, 7}); }
    { FilterSettings s; s.distortion = 35; cases.push_back({"lens", FilterKind::LensCorrection, s, 0}); }
    { FilterSettings s; s.distortion = -40; s.bicubic = true; cases.push_back({"lens bicubic", FilterKind::LensCorrection, s, 0}); }
    for (const Case& c : cases) {
        Image eight = *source;
        applyFilter(c.kind, eight, c.settings, 1, c.seed);
        auto deep = widenImage(*source);
        applyFilter(c.kind, *deep, c.settings, 1, c.seed);
        const Apart a = apart(eight, *deep);
        report("filter/" + c.name, a);
        if (a.worstOpaque > 1 || a.beyondOne > 0.01) {
            std::fprintf(stderr, "  %s: worst %d (opaque %d), %.3f%% beyond a level\n", c.name.c_str(), a.worst, a.worstOpaque, a.beyondOne * 100);
            failures++;
        }
    }
    // Invert is exact.
    Image eight = *source;
    applyInvert(eight);
    auto deep = widenImage(*source);
    applyInvert(*deep);
    CHECK_EQ(apart(eight, *deep).worst, 0);
    CHECK_EQ(failures, 0);
}

TEST_CASE(sixteen_bit_blur_of_a_mask_matches_eight_bit) {
    GrayImage mask(120, 90, 0);
    for (int y = 20; y < 70; y++) for (int x = 30; x < 90; x++) mask.at(x, y) = 255;
    auto deep = widenGray(mask);
    gaussianBlur(mask, 4.5);
    gaussianBlur(*deep, 4.5);
    auto reduced = narrowGray(*deep);
    int worst = 0;
    for (int y = 0; y < 90; y++) for (int x = 0; x < 120; x++) worst = std::max(worst, std::abs(int(mask.at(x, y)) - int(reduced->at(x, y))));
    CHECK(worst <= 1);
}

TEST_CASE(sixteen_bit_selection_helpers_blend_through_coverage) {
    auto source = widenImage(*busyImage(40, 30));
    Image16 adjusted = *source;
    applyInvert(adjusted);
    Gray16 coverage(40, 30, 0);
    for (int x = 0; x < 40; x++) for (int y = 0; y < 30; y++) coverage.at(x, y) = uint16_t(x < 20 ? one16 : 0);
    blendThroughCoverage(adjusted, *source, coverage);
    for (int y = 0; y < 30; y++) {
        CHECK(std::memcmp(adjusted.pixel(20, y), source->pixel(20, y), 8) == 0);
        CHECK(std::memcmp(adjusted.pixel(0, y), source->pixel(0, y), 8) != 0 || source->pixel(0, y)[3] == 0);
    }
    LayerTransform t(Point(10, 5), Size(40, 30));
    LayerTransform grown, trimmed;
    auto big = growImage(*source, t, 8, grown);
    CHECK_EQ(big->width(), 56);
    auto back = trimToPixels(*big, grown, trimmed);
    CHECK(*back == *source);
    CHECK(trimmed.samePlacement(t));
}

namespace {

int grayApart(const GrayImage& eight, const Gray16& deep) {
    auto reduced = narrowGray(deep);
    int worst = 0;
    for (int y = 0; y < eight.height(); y++) for (int x = 0; x < eight.width(); x++) worst = std::max(worst, std::abs(int(eight.at(x, y)) - int(reduced->at(x, y))));
    return worst;
}

std::shared_ptr<GrayImage> radialGray(int w, int h) {
    auto out = std::make_shared<GrayImage>(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const double dx = (x - w / 2.0) / (w / 2.0), dy = (y - h / 2.0) / (h / 2.0);
            out->at(x, y) = uint8_t(std::clamp(std::lround(255 * (1.1 - std::sqrt(dx * dx + dy * dy))), 0L, 255L));
        }
    return out;
}

/// A soft-edged selection: an antialiased ellipse and a feathered rectangle.
std::shared_ptr<GrayImage> softSelection() {
    auto shape = rasterizeEllipse(Rect(12, 10, 70, 50), 120, 90, true);
    auto rect = rasterizeRect(Rect(60, 40, 45, 35), 120, 90, true);
    gaussianBlur(*rect, 2.5);
    for (int y = 0; y < 90; y++) for (int x = 0; x < 120; x++) shape->at(x, y) = std::max(shape->at(x, y), rect->at(x, y));
    return shape;
}

} // namespace

/// Select > Modify and the selection operations at 16 bits: Expand, Contract, Border, Smooth and Feather decide on the
/// same pixels as at 8 bits (selected from half coverage up), and their soft rims agree within a level.
TEST_CASE(sixteen_bit_selection_operations_match_eight_bit) {
    auto eight = softSelection();
    auto deep = widenGray(*eight);
    const bool report = std::getenv("COMPOSITOR_REPORT_U16_CALIBRATION") != nullptr;
    auto check = [&](const char* what, const GrayImage& a, const Gray16& b) {
        const int worst = grayApart(a, b);
        if (report) std::fprintf(stderr, "  selection/%-26s worst %d\n", what, worst);
        CHECK(worst <= 1);
    };
    for (int amount : {3, -2, 9}) check(amount > 0 ? "expand" : "contract", *growSelection(*eight, amount), *growSelection(*deep, amount));
    check("border", *borderSelection(*eight, 5), *borderSelection(*deep, 5));
    check("smooth", *smoothSelection(*eight, 4), *smoothSelection(*deep, 4));
    check("feather", *featherSelection(*eight, 6.5), *featherSelection(*deep, 6.5));

    // Combining a shape into a 16-bit selection keeps 16 bits; each mode as at 8 bits.
    Selection current8;
    current8.coverage = eight;
    Selection current16;
    current16.coverage = Gray16Ptr(deep);
    auto shape = rasterizePolygon({{5, 5}, {110, 20}, {50, 85}}, 120, 90, true);
    for (SelectionMode mode : {SelectionMode::Replace, SelectionMode::Add, SelectionMode::Subtract, SelectionMode::Intersect}) {
        auto a = combineSelection(std::optional<Selection>(current8), *shape, mode, true);
        auto b = combineSelection(std::optional<Selection>(current16), AnyGray(shape), mode, true, SampleType::U16);
        CHECK(a && b && b->coverage.u16());
        check("combine", *a->coverage.u8(), *b->coverage.u16());
    }
    // Inverse twice is the selection; a move keeps the depth.
    Selection twice = invertSelection(invertSelection(current16, 120, 90), 120, 90);
    CHECK(*twice.coverage.u16() == *deep);
    Selection moved = offsetSelection(current16, 7, -3);
    CHECK(moved.coverage.u16() && moved.coverage.u16()->at(50, 30) == deep->at(43, 33));
    check("offset", *offsetSelection(current8, 7, -3).coverage.u8(), *moved.coverage.u16());
    // A feathered 16-bit selection has far more than 256 steps.
    auto soft = featherSelection(*rasterizeRect(Rect(20, 20, 80, 50), 120, 90, false), 12);
    std::set<int> steps8;
    for (int y = 0; y < 90; y++) for (int x = 0; x < 120; x++) steps8.insert(soft->at(x, y));
    auto soft16 = featherSelection(*widenGray(*rasterizeRect(Rect(20, 20, 80, 50), 120, 90, false)), 12);
    std::set<int> steps16;
    for (int y = 0; y < 90; y++) for (int x = 0; x < 120; x++) steps16.insert(soft16->at(x, y));
    CHECK(steps16.size() > steps8.size() * 4);
}

TEST_CASE(sixteen_bit_layer_as_selection) {
    Document doc(64, 48);
    auto eight = busyImage(40, 30);
    doc.layers.push_back(Layer(Asset::make(eight, "a"), Point(10, 6)));
    Layer turned(Asset::make(eight, "b"), Point(8, 8));
    turned.transform.rotation = 20;
    doc.layers.push_back(turned);
    for (const Layer& layer : doc.layers) {
        auto a = coverageFromLayer(doc, layer);
        Document deepDoc = doc;
        std::string error;
        CHECK(convertSampleType(deepDoc, SampleType::U16, &error));
        auto b = coverageFromLayer16(deepDoc, *deepDoc.find(layer.id));
        CHECK(grayApart(*a, *b) <= 1);
    }
}

/// Image Size at 16 bits: a document resized in each resampling mode renders within a level of the 8-bit resize,
/// bar a small share of samples at the layers' soft edges (the 8-bit resampler rounds its intermediate to 8.8 fixed
/// point and each pass to bytes).
TEST_CASE(sixteen_bit_image_size_matches_eight_bit) {
    Document doc(160, 120);
    doc.layers.push_back(Layer(Asset::make(busyImage(160, 120, 5), "base"), Point(0, 0)));
    Layer turned(Asset::make(busyImage(70, 50, 8), "turned"), Point(40, 30));
    turned.transform.rotation = 25;
    auto mask = std::make_shared<GrayImage>(70, 50);
    for (int y = 0; y < 50; y++) for (int x = 0; x < 70; x++) mask->at(x, y) = uint8_t(std::min(255, x * 4));
    turned.mask = LayerMask();
    turned.mask->asset = MaskAsset::make(mask);
    doc.layers.push_back(turned);
    for (Sampling mode : {Sampling::High, Sampling::Smooth, Sampling::Nearest}) {
        for (auto [w, h] : {std::pair{237, 171}, std::pair{97, 61}}) {
            Document eight = doc;
            CHECK(resizeDocument(eight, w, h, 72, mode));
            Document deep = doc;
            std::string error;
            CHECK(convertSampleType(deep, SampleType::U16, &error));
            CHECK(resizeDocument(deep, w, h, 72, mode));
            CHECK(deep.layers[1].asset->image.u16() && deep.layers[1].mask->asset.image.u16());
            Image a;
            render(eight, RenderOptions(), a);
            Image16 b;
            render16(deep, RenderOptions(), b);
            const Apart d = apart(a, b);
            report(std::string("image size/") + samplingName(mode) + " " + std::to_string(w), d);
            CHECK(d.beyondOne < 0.01);
        }
    }
}

/// Distort, Warp and Warp Cage bend 16-bit pixels as they bend 8-bit ones.
TEST_CASE(sixteen_bit_distort_and_warp_match_eight_bit) {
    auto eight = busyImage(90, 70, 4);
    Image16Ptr deep = widenImage(*eight);
    LayerTransform t(Point(10, 12), Size(90, 70));
    const Corners corners{Point(14, 8), Point(110, 20), Point(96, 95), Point(6, 80)};
    for (Sampling mode : {Sampling::High, Sampling::Smooth, Sampling::Nearest}) {
        t.sampling = mode;
        auto a = warpImage(ImagePtr(eight), t, corners, 0);
        auto b = warpImage(deep, t, corners, 0);
        CHECK(a && b);
        const Apart d = apart(*a->image, *b->image);
        report(std::string("distort/") + samplingName(mode), d);
        CHECK(d.worst <= 1);
        CHECK(a->transform == b->transform);
    }
    auto maskA = warpMask(*radialGray(90, 70), t, corners, 0, 0);
    auto maskB = warpMask(*widenGray(*radialGray(90, 70)), t, corners, 0, 0);
    CHECK(maskA && maskB && grayApart(*maskA->image, *maskB->image) <= 1);
    // Edit > Warp's bend and a cage.
    auto mesh = styleWarpMesh("warpArc", 35, false, 90, 70);
    CHECK(mesh.has_value());
    auto bentA = renderWarpedOverBox(*eight, *mesh, Rect(0, 0, 90, 70));
    auto bentB = renderWarpedOverBox(*deep, *mesh, Rect(0, 0, 90, 70));
    CHECK(bentA && bentB);
    const Apart bent = apart(*bentA->image, *bentB->image);
    report("warp/arc", bent);
    CHECK(bent.worst <= 1);
}

/// The content-aware tools decide on the pixels rounded to 8 bits and copy 16-bit pixels: on an 8-bit image
/// converted to 16 bits they choose exactly what the 8-bit tools choose, so the results agree within a level.
TEST_CASE(sixteen_bit_content_aware_tools_match_eight_bit) {
    auto source = busyImage(120, 90, 6);
    for (int y = 0; y < 90; y++) for (int x = 60; x < 120; x++) { uint8_t* p = source->pixel(x, y); p[3] = 255; }
    GrayImage hole(120, 90, 0);
    for (int y = 30; y < 55; y++) for (int x = 40; x < 70; x++) hole.at(x, y) = 255;
    Image a = *source;
    CHECK(contentFill(a, hole));
    auto b = widenImage(*source);
    CHECK(contentFill(*b, *widenGray(hole)));
    const Apart fill = apart(a, *b);
    report("content-aware fill", fill);
    CHECK(fill.worst <= 1);

    Image moveA = *source;
    CHECK(contentAwareMove(moveA, hole, 25, -10));
    auto moveB = widenImage(*source);
    CHECK(contentAwareMove(*moveB, *widenGray(hole), 25, -10));
    const Apart move = apart(moveA, *moveB);
    report("content-aware move", move);
    // The tone adaptation is worked out at 8 bits in both; its offset is added to the 16-bit patch, which rounds
    // once instead of twice. That level of difference can steer the seam's resynthesis to other, equally good
    // patches, so a thin band along the patch's rim differs (under 1% of the samples).
    CHECK(move.beyondOne < 0.01);

    for (auto [w, h] : {std::pair{90, 90}, std::pair{150, 70}}) {
        Image carved = seamCarve(*source, w, h);
        Image16 carved16 = seamCarve(*widenImage(*source), w, h);
        const Apart d = apart(carved, carved16);
        report("content-aware scale " + std::to_string(w) + "x" + std::to_string(h), d);
        CHECK(d.worst <= 1);
    }
}

TEST_MAIN()
