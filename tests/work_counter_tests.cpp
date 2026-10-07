// Work regressions without a clock (docs/work-counters.md): each case runs a fixed, scripted workload and bounds the
// work the counters in workcounters.h saw (pixels composited, layers drawn, reductions built, filters run) or the bytes
// the history kept, instead of timing it. A failure names the counter, the bound and the measured value. Every bound
// is the value measured when the case was written with some headroom, and the comment beside it says how it was
// chosen; the counts are sums of areas, so they do not depend on the worker count or how the rows were split.
//
// Two consistency checks ride along: a region rendered alone equals the same region cut from the full render, and a
// render at a quarter (drawn from the cached reductions) stays within a small tolerance of the full render reduced.
#include "check.h"
#include "compositor/brush.h"
#include "compositor/history.h"
#include "compositor/png.h"
#include "compositor/render.h"
#include "compositor/smartfilter.h"
#include "compositor/smartobject_edit.h"
#include "compositor/workcounters.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace compositor;
using work::Counter;

namespace {

/// Fails unless the counter's value in `delta` is at most `limit`; prints the measured value either way, so a
/// bound can be updated from a passing run's log.
void atMost(const char* file, int line, const char* what, const work::Snapshot& delta, Counter counter, double limit) {
    const uint64_t actual = delta[counter], bound = uint64_t(std::floor(limit));
    if (actual > bound)
        check::fail(file, line, std::string(what) + ": " + work::name(counter) + " = " + std::to_string(actual) + ", expected at most " +
                                    std::to_string(bound) + " (all: " + delta.describe() + ")");
    else std::fprintf(stderr, "  %s: %s = %llu (at most %llu)\n", what, work::name(counter), (unsigned long long)actual, (unsigned long long)bound);
}
#define WORK_AT_MOST(what, delta, counter, limit) atMost(__FILE__, __LINE__, what, delta, counter, double(limit))

/// The same for a value that is not a counter (the history's bytes).
void valueAtMost(const char* file, int line, const char* what, uint64_t actual, double limit) {
    const uint64_t bound = uint64_t(std::floor(limit));
    if (actual > bound) check::fail(file, line, std::string(what) + " = " + std::to_string(actual) + ", expected at most " + std::to_string(bound));
    else std::fprintf(stderr, "  %s = %llu (at most %llu)\n", what, (unsigned long long)actual, (unsigned long long)bound);
}
#define VALUE_AT_MOST(what, actual, limit) valueAtMost(__FILE__, __LINE__, what, uint64_t(actual), double(limit))

std::shared_ptr<Image> solid(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    auto img = std::make_shared<Image>(w, h);
    img->fill(uint8_t(r * a / 255), uint8_t(g * a / 255), uint8_t(b * a / 255), a);
    return img;
}

/// An opaque, photo-like gradient with detail (as the benches' base layer).
std::shared_ptr<Image> photo(int w, int h) {
    auto img = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; y++) {
        uint8_t* row = img->row(y);
        for (int x = 0; x < w; x++, row += 4) {
            row[0] = uint8_t(x * 255 / std::max(1, w - 1));
            row[1] = uint8_t(y * 255 / std::max(1, h - 1));
            row[2] = uint8_t((x ^ y) & 0xFF);
            row[3] = 255;
        }
    }
    return img;
}

/// A soft, partly transparent disc of paint over most of the canvas (the benches' paint layers), premultiplied.
std::shared_ptr<Image> softPaint(int w, int h, int l) {
    auto img = std::make_shared<Image>(w, h);
    const double cx = w * (0.3 + 0.1 * l), cy = h * (0.6 - 0.08 * l), r = std::min(w, h) * 0.45;
    for (int y = 0; y < h; y++) {
        uint8_t* row = img->row(y);
        for (int x = 0; x < w; x++, row += 4) {
            const double d = std::hypot(x - cx, y - cy) / r;
            const unsigned a = d >= 1 ? 0 : unsigned(200 * (1 - d));
            row[0] = uint8_t((a * unsigned(40 * l)) / 255); row[1] = uint8_t((a * unsigned(255 - 40 * l)) / 255); row[2] = uint8_t(a / 2); row[3] = uint8_t(a);
        }
    }
    return img;
}

Layer imageLayer(const std::string& name, std::shared_ptr<Image> image, Point origin) { return Layer(Asset::make(image, name), origin); }

uint64_t area(const Rect& r) { return r.isEmpty() ? 0 : uint64_t(std::ceil(r.width)) * uint64_t(std::ceil(r.height)); }

} // namespace

// ---- Brush: dirty-rect discipline -----------------------------------------------------------------------------------

TEST_CASE(a_brush_dab_and_a_stroke_segment_recomposite_only_their_area) {
    // A 4000 x 3000 photo with a blank layer above it, painted with a 40 px round brush, then each step rendered as the
    // canvas renders it: only the stroke's dirty rectangle, with the stroke's pixels as an override and a RenderCache.
    const int W = 4000, H = 3000;
    Document doc(W, H);
    doc.layers.push_back(imageLayer("photo", photo(W, H), {0, 0}));
    doc.layers.push_back(Layer("paint", doc.size()));
    const Uuid paint = doc.layers[1].id;
    BrushSettings s;
    s.diameter = 40; s.hardness = 0.8; s.red = 0.8; s.green = 0.1; s.blue = 0.2;
    BrushStroke stroke(doc.layers[1], false, s, doc.size());
    REQUIRE(stroke.isValid());
    RenderCache cache;
    auto frame = [&](const Rect& dirty) {
        Overrides overrides;
        overrides[paint].image = stroke.previewImage();
        overrides[paint].transform = stroke.paintTransform();
        RenderOptions o;
        o.region = dirty;
        o.version = 1;
        Image out;
        render(doc, o, out, &overrides, &cache);
    };
    const double canvasPixels = double(W) * H;

    // The press: one dab. The bounds are the dab's box with a margin, 44 x 44 = 1936 (40 px, a pixel of rounding and
    // takeDirtyRect's 1 px margin each side).
    work::Snapshot before = work::snapshot();
    stroke.append({1000, 1000});
    const Rect press = stroke.takeDirtyRect();
    frame(press);
    work::Snapshot d = work::snapshot() - before;
    WORK_AT_MOST("press", d, Counter::BrushDabs, 1);
    // Measured 1600: the dab's 40 x 40 pixels recomposed.
    WORK_AT_MOST("press", d, Counter::BrushRecomposePixels, 44 * 44);
    // Measured 1764: the dirty rectangle (42 x 42) rendered once.
    WORK_AT_MOST("press", d, Counter::RenderPixels, 44 * 44);
    // The photo and the stroke over the same rectangle: two draws, 3528 pixels measured.
    WORK_AT_MOST("press", d, Counter::LayerDraws, 2);
    WORK_AT_MOST("press", d, Counter::LayerDrawPixels, 2 * 44 * 44);

    // Twenty moves 12 px apart along a gentle curve: each one's work must follow the segment, not the canvas.
    work::Snapshot strokeStart = work::snapshot();
    uint64_t worstRender = 0, worstRecompose = 0, worstDraw = 0;
    for (int i = 1; i <= 20; i++) {
        before = work::snapshot();
        stroke.append({1000.0 + 12 * i, 1000.0 + 20 * std::sin(i * 0.3)});
        const Rect dirty = stroke.takeDirtyRect();
        frame(dirty);
        d = work::snapshot() - before;
        worstRender = std::max(worstRender, d[Counter::RenderPixels]);
        worstRecompose = std::max(worstRecompose, d[Counter::BrushRecomposePixels]);
        worstDraw = std::max(worstDraw, d[Counter::LayerDrawPixels]);
        CHECK(d[Counter::RenderPixels] <= area(dirty) + 1);
        CHECK(d[Counter::LayerDrawPixels] <= 2 * d[Counter::RenderPixels]);   // the two layers, each over the frame at most
    }
    d = work::snapshot() - strokeStart;
    // A segment of 12 px plus the provisional tail to the pointer, 40 px wide: the curve piece, the tail it takes back
    // and the new tail cover about (12 + 12 + 44) x 44 and the curve's bend. Measured worst 3876 rendered, 3630
    // recomposed and 7752 drawn; bounds about 1.3x those (a whole-canvas recomposite would be 12 million).
    VALUE_AT_MOST("worst segment: render.pixels", worstRender, 5000);
    VALUE_AT_MOST("worst segment: brush.recomposePixels", worstRecompose, 5000);
    VALUE_AT_MOST("worst segment: layer.drawPixels", worstDraw, 10000);
    // Whole stroke: 20 segments at 1 px spacing (40 px x 0.025) give about 250 dabs, and the provisional tails, drawn and
    // taken back each time, about as many again. Measured 493; bound 600.
    WORK_AT_MOST("stroke", d, Counter::BrushDabs, 600);
    // Measured 70116 rendered, 140232 drawn, 65422 recomposed: about 0.6% of the canvas. Bounds about 1.3x, all under
    // 1.5% of it.
    WORK_AT_MOST("stroke", d, Counter::RenderPixels, 90000);
    WORK_AT_MOST("stroke", d, Counter::LayerDrawPixels, 180000);
    WORK_AT_MOST("stroke", d, Counter::BrushRecomposePixels, 85000);
    CHECK(90000 < canvasPixels * 0.01);
    // At 1:1 nothing is drawn from a reduction.
    WORK_AT_MOST("stroke", d, Counter::MipMisses, 0);
}

TEST_CASE(a_stroke_seen_zoomed_out_refreshes_only_its_area_of_the_reductions) {
    // The same stroke with the canvas at 25%: the layer under the stroke is drawn from its level-2 reduction, which the
    // stroke keeps current over what it painted (MipCache::refresh) instead of rebuilding it.
    const int W = 4000, H = 3000;
    Document doc(W, H);
    doc.layers.push_back(imageLayer("photo", photo(W, H), {0, 0}));
    const Uuid id = doc.layers[0].id;
    BrushSettings s;
    s.diameter = 40; s.hardness = 0.8; s.red = 0.1; s.green = 0.2; s.blue = 0.8;
    BrushStroke stroke(doc.layers[0], false, s, doc.size());
    REQUIRE(stroke.isValid());
    RenderCache cache;
    auto frame = [&](const Rect& dirty) {
        Overrides overrides;
        overrides[id].image = stroke.previewImage();
        overrides[id].transform = stroke.paintTransform();
        RenderOptions o;
        o.region = dirty;
        o.scale = 0.25;
        o.version = 1;
        Image out;
        render(doc, o, out, &overrides, &cache);
    };
    stroke.append({1000, 1000});
    frame(stroke.takeDirtyRect());   // the reductions of the working pixels are built here, once
    frame(doc.rect());
    const work::Snapshot start = work::snapshot();
    for (int i = 1; i <= 20; i++) {
        stroke.append({1000.0 + 12 * i, 1000.0});
        frame(stroke.takeDirtyRect());
    }
    const work::Snapshot d = work::snapshot() - start;
    // No reduction is rebuilt while painting: the 48 MB layer's levels would be 4 million pixels a rebuild.
    WORK_AT_MOST("zoomed-out stroke", d, Counter::MipMisses, 0);
    WORK_AT_MOST("zoomed-out stroke", d, Counter::MipBuiltPixels, 0);
    // Levels 1 and 2 over each step's dirty box (about 60 x 44, so 30 x 22 + 15 x 11 a step), 20 steps. Measured 18320;
    // bound 24000, about 1.3x (the whole of level 1 alone would be 3 million).
    WORK_AT_MOST("zoomed-out stroke", d, Counter::MipRefreshPixels, 24000);
    // Every frame asks for the reduction it draws from and finds it: hits only.
    CHECK(d[Counter::MipHits] >= 20);
}

// ---- Adjustment layers: a slider drag -------------------------------------------------------------------------------

TEST_CASE(an_adjustment_slider_tick_redraws_each_layer_once_from_cached_reductions) {
    // The adjust bench's document (a photo and four soft paint layers) under a Levels adjustment layer, shown at 25%
    // as a fitted view would; then thirty ticks of the white input, each one a full render of the view.
    const int W = 2400, H = 1600, layers = 5;
    Document doc(W, H);
    doc.layers.push_back(imageLayer("photo", photo(W, H), {0, 0}));
    for (int l = 1; l < layers; l++) doc.layers.push_back(imageLayer("paint " + std::to_string(l), softPaint(W, H, l), {0, 0}));
    AdjustmentSettings levels = AdjustmentSettings::defaults(AdjustmentKind::Levels);
    Layer adjustment("Levels", doc.size());
    adjustment.adjustment = levels.toLayerAdjustment();
    doc.layers.push_back(adjustment);
    RenderOptions o;
    o.scale = 0.25;
    RenderCache cache;
    Image out;
    const uint64_t frame = uint64_t(W / 4) * uint64_t(H / 4);
    render(doc, o, out, nullptr, &cache);   // the first frame builds the reductions
    const work::Snapshot start = work::snapshot();
    const int ticks = 30;
    for (int i = 0; i < ticks; i++) {
        levels.levels.ranges[0].white = 255 - i * 3;
        doc.layers.back().adjustment = levels.toLayerAdjustment();
        o.version = uint64_t(i + 2);
        render(doc, o, out, nullptr, &cache);
    }
    const work::Snapshot d = work::snapshot() - start;
    // Each tick draws the five pixel layers once over the frame (600 x 400): measured 5 draws and 1.2 million pixels a
    // tick, exactly. Nothing is cached around an adjustment layer, so this is the whole cost; a cache of the layers below
    // would bring it to one pass (docs/work-counters.md, "Found"). The counts follow from the geometry alone, so the
    // bounds are the measured counts: any extra draw fails.
    WORK_AT_MOST("slider ticks", d, Counter::LayerDraws, layers * ticks);
    WORK_AT_MOST("slider ticks", d, Counter::LayerDrawPixels, frame * layers * ticks);
    // The Levels layer itself: one pass over the frame a tick.
    WORK_AT_MOST("slider ticks", d, Counter::Adjustments, ticks);
    WORK_AT_MOST("slider ticks", d, Counter::AdjustmentPixels, frame * ticks);
    WORK_AT_MOST("slider ticks", d, Counter::RenderPixels, frame * ticks);
    // Every layer drawn from its cached reduction: no level built during the drag (the budget bug in brush-latency.md's
    // "Adjustment drags" rebuilt them from the full-size pixels every frame).
    WORK_AT_MOST("slider ticks", d, Counter::MipMisses, 0);
    WORK_AT_MOST("slider ticks", d, Counter::MipBuiltPixels, 0);
    CHECK_EQ(d[Counter::MipHits], uint64_t(layers * ticks));
}

// ---- Mip cache: the first frame at a zoom builds, the next ones only hit --------------------------------------------

TEST_CASE(a_second_frame_at_the_same_zoom_builds_no_reduction) {
    const int W = 2400, H = 1600;
    Document doc(W, H);
    doc.layers.push_back(imageLayer("photo", photo(W, H), {0, 0}));
    doc.layers.push_back(imageLayer("paint", softPaint(W, H, 1), {0, 0}));
    RenderOptions o;
    o.scale = 0.25;
    Image out;
    work::Snapshot before = work::snapshot();
    render(doc, o, out);
    work::Snapshot d = work::snapshot() - before;
    // Two layers at 25%: each builds levels 1 and 2 once (1200 x 800 + 600 x 400 = 1.2 million pixels a layer).
    WORK_AT_MOST("first frame", d, Counter::MipMisses, 2);
    WORK_AT_MOST("first frame", d, Counter::MipBuiltPixels, 2 * (1200 * 800 + 600 * 400));
    before = work::snapshot();
    render(doc, o, out);
    o.scale = 0.3;   // level 1 serves 0.3 (a final resample of at most 2x): already held from the first frame
    render(doc, o, out);
    d = work::snapshot() - before;
    WORK_AT_MOST("same levels again", d, Counter::MipMisses, 0);
    WORK_AT_MOST("same levels again", d, Counter::MipBuiltPixels, 0);
    CHECK_EQ(d[Counter::MipHits], uint64_t(4));
}

// ---- History: a small edit on a big layer ---------------------------------------------------------------------------

TEST_CASE(a_small_edit_on_a_big_layer_keeps_history_bytes_for_its_area) {
    // A 4000 x 3000 opaque layer (48 MB): a short stroke committed as the app commits one (a new raster for the layer
    // inside one history step). The history must keep the changed region's before and after, not the layer.
    const int W = 4000, H = 3000;
    std::optional<Document> doc(Document(W, H));
    doc->layers.push_back(imageLayer("photo", photo(W, H), {0, 0}));
    DocumentHistory history;
    BrushSettings s;
    s.diameter = 30; s.hardness = 1; s.red = 1;
    Rect painted;
    {
        BrushStroke stroke(doc->layers[0], false, s, doc->size());
        REQUIRE(stroke.isValid());
        stroke.append({2000, 1500});
        stroke.append({2030, 1510});
        stroke.append({2060, 1500});
        stroke.flush();
        painted = stroke.takeDirtyRect();
        BrushStroke::Commit commit = stroke.commit();
        REQUIRE(commit.asset);
        history.begin("Brush Stroke", doc, std::nullopt);
        history.noteRegion(painted);
        doc->layers[0].asset = commit.asset;
        doc->layers[0].transform = commit.transform;
        history.end(doc, std::nullopt);
    }
    REQUIRE(history.canUndo());
    // Before and after crops of the changed pixels at 4 bytes a pixel (3600 pixels: 28.8 KB) and the replaced
    // asset's 96 px thumbnail (27.6 KB); a whole layer would be 48 MB. Measured 56448 bytes; bound 75000 (1.3x).
    VALUE_AT_MOST("history after one stroke: retained bytes", history.retainedBytes(doc), 75000);
    std::fprintf(stderr, "  the stroke's dirty box: %gx%g\n", painted.width, painted.height);
    CHECK_EQ(history.patchCount(), 1);
    // A second, separate edit (a 16 x 16 block written directly): proportional to that block alone.
    const size_t afterStroke = history.retainedBytes(doc);
    history.begin("Fill", doc, std::nullopt);
    {
        auto copy = std::make_shared<Image>(*doc->layers[0].asset->image.u8());
        for (int y = 100; y < 116; y++) for (int x = 100; x < 116; x++) { uint8_t* p = copy->pixel(x, y); p[0] = 0; p[1] = 0; p[2] = 255; p[3] = 255; }
        doc->layers[0].asset = Asset::make(copy, "photo");
    }
    history.end(doc, std::nullopt);
    // 16 x 16 x 8 = 2 KB of crops plus the thumbnail (96 x 72 x 4): measured 29696 more bytes; bound 40000 (1.35x).
    VALUE_AT_MOST("history after a 16 x 16 edit: retained bytes added", history.retainedBytes(doc) - afterStroke, 40000);
}

// ---- Smart Filters: a render that changes nothing runs no filter --------------------------------------------------

TEST_CASE(a_smart_filter_runs_once_when_added_and_never_on_a_render) {
    Document doc(600, 400);
    doc.layers.push_back(imageLayer("photo", photo(600, 400), {0, 0}));
    SmartObjectContents c;
    auto image = photo(200, 100);
    c.image = image;
    encodePngImage(*image, c.bytes);
    c.fileName = "chip.png";
    auto source = makeSmartObjectSource(std::move(c));
    doc.smartObjects[source->id] = source;
    doc.layers.push_back(smartObjectLayer(source, {100, 100, 300, 100, 300, 200, 100, 200}, "Chip"));
    SmartFilterEntry blur, noise;
    blur.parameters = smartfilter::GaussianBlur{3};
    noise.parameters = smartfilter::AddNoise{10, false, false, 1};
    std::string error;
    work::Snapshot before = work::snapshot();
    REQUIRE(addSmartFilter(doc, doc.layers[1], blur, &error));
    REQUIRE(addSmartFilter(doc, doc.layers[1], noise, &error));
    work::Snapshot d = work::snapshot() - before;
    // Adding each filter draws the instance through its stack once: one stack with one entry, then one with two.
    WORK_AT_MOST("adding two filters", d, Counter::SmartFilterStacks, 2);
    WORK_AT_MOST("adding two filters", d, Counter::SmartFilterPasses, 3);
    CHECK(d[Counter::SmartFilterStacks] >= 2);   // and the counter does see them
    // Rendering, at 1:1 and zoomed out, twice, and after an unrelated change (the photo's opacity): the filtered pixels
    // are the layer's own, so no filter runs again.
    before = work::snapshot();
    Image out;
    RenderOptions o;
    render(doc, o, out);
    render(doc, o, out);
    o.scale = 0.5;
    render(doc, o, out);
    doc.layers[0].opacity = 0.5;
    render(doc, o, out);
    d = work::snapshot() - before;
    WORK_AT_MOST("renders", d, Counter::SmartFilterStacks, 0);
    WORK_AT_MOST("renders", d, Counter::SmartFilterPasses, 0);
}

// ---- Consistency: a region equals the full render cut -------------------------------------------------------------

namespace {

/// A document that exercises the renderer's paths: a photo, a scaled and rotated layer between pixels, a masked
/// Multiply layer, a soft Normal layer and a Curves adjustment above them.
Document consistencyDocument() {
    const int W = 640, H = 480;
    Document doc(W, H);
    doc.layers.push_back(imageLayer("photo", photo(W, H), {0, 0}));
    Layer turned = imageLayer("turned", softPaint(300, 200, 2), {0, 0});
    turned.transform = LayerTransform(Point(120.25, 80.5), Size(330.7, 210.3));
    turned.transform.rotation = 12;   // degrees
    doc.layers.push_back(turned);
    Layer multiplied = imageLayer("multiplied", solid(400, 300, 200, 120, 60, 230), {150, 100});
    multiplied.blendMode = BlendMode::Multiply;
    LayerMask mask;
    auto m = std::make_shared<GrayImage>(400, 300, 0);
    for (int y = 0; y < 300; y++) for (int x = 0; x < 400; x++) m->at(x, y) = uint8_t((x * 255 / 399 + y) & 0xFF);
    mask.asset = MaskAsset::make(m);
    multiplied.mask = mask;
    doc.layers.push_back(multiplied);
    doc.layers.push_back(imageLayer("soft", softPaint(W, H, 3), {0, 0}));
    AdjustmentSettings curves = AdjustmentSettings::defaults(AdjustmentKind::Curves);
    Layer adjustment("Curves", doc.size());
    adjustment.adjustment = curves.toLayerAdjustment();
    adjustment.opacity = 0.7;
    doc.layers.push_back(adjustment);
    return doc;
}

template <class Img>
int worstAgainstCrop(const Img& full, const Img& part, int x0, int y0) {
    int worst = 0;
    for (int y = 0; y < part.height(); y++)
        for (int x = 0; x < part.width() * 4; x++)
            worst = std::max(worst, std::abs(int(part.row(y)[x]) - int(full.row(y0 + y)[size_t(x0) * 4 + size_t(x)])));
    return worst;
}

} // namespace

TEST_CASE(a_region_render_equals_the_full_render_cropped) {
    Document doc = consistencyDocument();
    const Rect regions[] = {Rect(0, 0, 64, 64), Rect(117, 93, 211, 157), Rect(301, 0, 339, 480), Rect(600, 440, 40, 40), Rect(149, 99, 3, 3)};
    // 8 bits.
    auto full = renderFlattened(doc);
    for (const Rect& r : regions) {
        RenderOptions o;
        o.region = r;
        Image part;
        render(doc, o, part);
        const int worst = worstAgainstCrop(*full, part, int(r.x), int(r.y));
        if (worst != 0) std::fprintf(stderr, "  8-bit region %g,%g %gx%g differs from the full render by %d\n", r.x, r.y, r.width, r.height, worst);
        CHECK_EQ(worst, 0);
    }
    // 16 bits: the same document converted.
    Document deep = doc;
    REQUIRE(convertSampleType(deep, SampleType::U16));
    auto full16 = renderFlattened16(deep);
    for (const Rect& r : regions) {
        RenderOptions o;
        o.region = r;
        Image16 part;
        render16(deep, o, part);
        const int worst = worstAgainstCrop(*full16, part, int(r.x), int(r.y));
        if (worst != 0) std::fprintf(stderr, "  16-bit region %g,%g %gx%g differs from the full render by %d\n", r.x, r.y, r.width, r.height, worst);
        CHECK_EQ(worst, 0);
    }
}

TEST_CASE(a_zoomed_out_render_matches_the_full_render_reduced) {
    // At 50% and 25% the layers are drawn from their reductions and composited small; the reference composites at full
    // size and halves the result. Blending is not linear, so they differ a little at soft and Multiply edges.
    Document doc = consistencyDocument();
    auto full = renderFlattened(doc);
    std::shared_ptr<Image> reduced = full;
    for (int level = 1; level <= 2; level++) {
        reduced = halveImage(*reduced);
        RenderOptions o;
        o.scale = std::ldexp(1.0, -level);
        Image small;
        render(doc, o, small);
        REQUIRE(small.width() == reduced->width() && small.height() == reduced->height());
        int worst = 0;
        double total = 0;
        for (int y = 0; y < small.height(); y++)
            for (int x = 0; x < small.width() * 4; x++) {
                const int diff = std::abs(int(small.row(y)[x]) - int(reduced->row(y)[x]));
                worst = std::max(worst, diff);
                total += diff;
            }
        const double mean = total / (double(small.width()) * small.height() * 4);
        std::fprintf(stderr, "  level %d: worst %d, mean %.3f\n", level, worst, mean);
        // Measured worst 2 / mean 0.086 at level 1 and worst 43 / mean 0.186 at level 2 (where the mask's ramp wraps
        // from 255 to 0, so masking before or after the reduction differs most); bounds: worst 64, mean 0.5.
        CHECK(worst <= 64);
        CHECK(mean <= 0.5);
    }
}

TEST_MAIN()
