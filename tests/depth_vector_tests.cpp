// Shapes, vector masks and layer styles in a 16-bit document (docs/bit-depth.md): each is checked against its 8-bit
// path. An 8-bit document converted to 16 bits (or the same shape made in a 16-bit one) renders, reduced to 8 bits,
// within a level of the 8-bit render where the result is continuous; where it is not, the test says by how much and
// why. And a smooth ramp keeps more than 256 levels where 8 bits would band.
// COMPOSITOR_REPORT_U16_CALIBRATION=1 prints the worst difference and the share beyond a level for each scene.
#include "check.h"
#include "compositor/depth.h"
#include "compositor/layerstyle.h"
#include "compositor/presets.h"
#include "compositor/psd_carry.h"
#include "compositor/render.h"
#include "compositor/vectorlayer.h"
#include "compositor/vectormask.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <set>
#include <string>

using namespace compositor;

namespace {

uint32_t mix(uint32_t v) { v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; v ^= v >> 16; return v; }

/// An opaque gradient with a little noise: the backdrop.
std::shared_ptr<Image> base(int w, int h) {
    auto image = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint32_t n = mix(uint32_t(y * w + x));
            uint8_t* p = image->pixel(x, y);
            p[0] = uint8_t(std::clamp(255 * x / (w - 1) + int(n & 15) - 8, 0, 255));
            p[1] = uint8_t(std::clamp(255 * y / (h - 1) + int((n >> 8) & 15) - 8, 0, 255));
            p[2] = uint8_t(std::clamp(200 - 150 * (x + y) / (w + h - 2), 0, 255));
            p[3] = 255;
        }
    return image;
}

/// Soft paint: a coloured disc whose alpha falls off toward its rim, with a hard-edged square beside it.
std::shared_ptr<Image> soft(int w, int h) {
    auto image = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const double dx = (x - w * 0.35) / (w * 0.3), dy = (y - h * 0.5) / (h * 0.35);
            int a = std::clamp(int(std::lround(255 * (1.3 - std::sqrt(dx * dx + dy * dy)))), 0, 255);
            if (x > w * 0.7 && x < w * 0.9 && y > h * 0.3 && y < h * 0.7) a = 255;
            const int r = 40 + 180 * x / w, g = 200 - 120 * y / h, b = 90;
            uint8_t* p = image->pixel(x, y);
            p[0] = uint8_t((r * a + 127) / 255); p[1] = uint8_t((g * a + 127) / 255); p[2] = uint8_t((b * a + 127) / 255); p[3] = uint8_t(a);
        }
    return image;
}

constexpr int W = 160, H = 120;

Document backdrop() {
    Document doc(W, H);
    doc.layers.push_back(Layer(Asset::make(base(W, H), "Background"), Point(0, 0)));
    return doc;
}

Document sixteen(const Document& doc) {
    Document out = doc;
    CHECK(convertSampleType(out, SampleType::U16));
    return out;
}

struct Apart { int worst = 0; double beyondOne = 0; };

Apart apart(const Image& eight, const Image16& deep) {
    auto reduced = narrowImage(deep);
    Apart out;
    if (reduced->width() != eight.width() || reduced->height() != eight.height()) return {256, 1};
    size_t beyond = 0, total = 0;
    for (int y = 0; y < eight.height(); y++)
        for (int i = 0; i < eight.width() * 4; i++, total++) {
            const int d = std::abs(int(eight.row(y)[i]) - int(reduced->row(y)[i]));
            out.worst = std::max(out.worst, d);
            beyond += d > 1;
        }
    out.beyondOne = total ? double(beyond) / double(total) : 0;
    return out;
}

void report(const std::string& what, const Apart& a) {
    if (std::getenv("COMPOSITOR_REPORT_U16_CALIBRATION"))
        std::fprintf(stderr, "  %-40s worst %3d, beyond a level %.4f%%\n", what.c_str(), a.worst, a.beyondOne * 100);
}

/// The document rendered at 8 bits against its 16-bit conversion rendered at 16, at 1:1 and reduced.
Apart renderApart(const Document& doc, const RenderOptions& options = {}) {
    Image eight;
    render(doc, options, eight);
    Image16 deep;
    render16(sixteen(doc), options, deep);
    return apart(eight, deep);
}

RenderOptions reduced() {
    RenderOptions options;
    options.region = {10, 6, 140, 100};
    options.scale = 0.5;
    return options;
}

/// The same shape layer made in an 8-bit document and in its 16-bit conversion (at that depth), rendered apart.
Apart shapeApart(const VectorShape& shape) {
    Document eight = backdrop();
    Document deep = sixteen(eight);
    for (Document* doc : {&eight, &deep}) {
        Layer layer(Asset::make(std::make_shared<Image>(1, 1), "Shape"), Point(0, 0));
        setVectorShape(layer, *doc, shape);
        doc->layers.push_back(layer);
        CHECK(isVectorShapeLayer(doc->layers.back()));
    }
    CHECK(deep.layers.back().asset->image.u16() != nullptr);   // made at the document's depth
    Image a;
    render(eight, {}, a);
    Image16 b;
    render16(deep, {}, b);
    return apart(a, b);
}

StyleGradient ramp(StyleColor from, StyleColor to) {
    StyleGradient g;
    g.colors = {{0, from, 0.5f}, {1, to, 0.5f}};
    g.alphas = {{0, 1, 0.5f}, {1, 1, 0.5f}};
    return g;
}

/// A document whose second layer is soft paint carrying `style`, and the pattern "tile" among its patterns.
Document styled(const LayerStyle& style, BlendMode mode = BlendMode::Normal, double opacity = 1) {
    Document doc = backdrop();
    std::vector<uint8_t> rgba;
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) { rgba.push_back(uint8_t(30 * x)); rgba.push_back(uint8_t(30 * y)); rgba.push_back(uint8_t((x ^ y) * 30)); rgba.push_back(255); }
    addDocumentPatterns(doc, {makePattern("tile", "Tile", 8, 8, rgba)});
    Layer layer(Asset::make(soft(100, 70), "Paint"), Point(30, 25));
    layer.blendMode = mode;
    layer.opacity = opacity;
    setLayerStyle(layer, style);
    doc.layers.push_back(layer);
    return doc;
}

} // namespace

// ---- Vector coverage -----------------------------------------------------------------------------------------------

TEST_CASE(sixteen_bit_vector_coverage_is_the_eight_bit_coverage_at_fifteen_bits) {
    VectorPath path = ellipsePath(Rect(10.3, 7.7, 60.2, 41.5));
    VectorPath star = polygonPath(Rect(40, 20, 50, 50), 5, 0.5);
    addShapeComponent(path, star, VectorPath::Op::Xor);
    for (bool inverted : {false, true}) {
        path.inverted = inverted;
        auto eight = rasterizeVectorMask(path, Rect(0, 0, 100, 80), 1, 100, 80);
        auto deep = rasterizeVectorMask16(path, Rect(0, 0, 100, 80), 1, 100, 80);
        auto back = narrowGray(*deep);
        int worst = 0;
        std::set<int> levels;
        for (int y = 0; y < 80; y++)
            for (int x = 0; x < 100; x++) { worst = std::max(worst, std::abs(int(eight->at(x, y)) - int(back->at(x, y)))); levels.insert(deep->at(x, y)); }
        CHECK(worst <= 1);   // the same coverage rounded at 8 or 15 bits
        CHECK(levels.size() > 256);   // an edge's antialiasing keeps its fine steps
    }
    VectorStroke stroke;
    stroke.enabled = true;
    stroke.width = 3.5;
    for (auto align : {VectorStroke::Align::Center, VectorStroke::Align::Inside, VectorStroke::Align::Outside}) {
        stroke.align = align;
        auto eight = rasterizeVectorStroke(path, stroke, Rect(0, 0, 100, 80), 1, 100, 80);
        auto back = narrowGray(*rasterizeVectorStroke16(path, stroke, Rect(0, 0, 100, 80), 1, 100, 80));
        int worst = 0;
        for (int y = 0; y < 80; y++) for (int x = 0; x < 100; x++) worst = std::max(worst, std::abs(int(eight->at(x, y)) - int(back->at(x, y))));
        CHECK(worst <= 1);
    }
}

// ---- Shape layers ----------------------------------------------------------------------------------------------------

TEST_CASE(shape_layers_made_at_sixteen_bits_render_as_their_eight_bit_twins) {
    VectorShape solid;
    solid.path = ellipsePath(Rect(20.5, 15.25, 90, 70));
    solid.r = 200; solid.g = 60; solid.b = 30;
    solid.stroke.enabled = false;
    Apart a = shapeApart(solid);
    report("shape: solid ellipse", a);
    CHECK(a.worst <= 1);

    VectorShape stroked = solid;
    stroked.path = rectanglePath(Rect(30, 20, 80, 60), 12);
    stroked.stroke.enabled = true;
    stroked.stroke.width = 5;
    stroked.stroke.r = 10; stroked.stroke.g = 20; stroked.stroke.b = 200;
    stroked.stroke.opacity = 0.8f;
    stroked.stroke.dashes = {2, 1};
    a = shapeApart(stroked);
    report("shape: dashed stroke", a);
    CHECK(a.worst <= 1);

    VectorShape gradient = stroked;
    gradient.fillPaint.kind = VectorPaint::Kind::Gradient;
    gradient.fillPaint.gradient = ramp({255, 0, 0}, {0, 0, 255});
    gradient.fillPaint.gradient.angle = 30;
    gradient.stroke.dashes.clear();
    gradient.stroke.align = VectorStroke::Align::Outside;
    gradient.stroke.paint.kind = VectorPaint::Kind::Gradient;
    gradient.stroke.paint.gradient = ramp({255, 255, 0}, {0, 128, 0});
    a = shapeApart(gradient);
    report("shape: gradient fill and stroke", a);
    // The 16-bit ramp is drawn from the gradient's exact colours; the 8-bit one rounds each colour to a byte, rounds
    // again as it premultiplies, and again as it composites (the stroke at 80%). Three half-level roundings reach two
    // levels on a few samples: 0.005% here.
    CHECK(a.worst <= 2 && a.beyondOne < 0.0005);

    VectorShape hidden = solid;
    hidden.fill = false;
    hidden.stroke.enabled = true;
    hidden.stroke.width = 4;
    hidden.path = polygonPath(Rect(25, 15, 100, 90), 6, 0.4);
    a = shapeApart(hidden);
    report("shape: stroke only (star)", a);
    CHECK(a.worst <= 1);
}

TEST_CASE(a_sixteen_bit_gradient_shape_has_more_than_256_levels) {
    Document doc = sixteen(backdrop());
    VectorShape shape;
    shape.path = rectanglePath(Rect(0, 0, 160, 20));
    shape.fillPaint.kind = VectorPaint::Kind::Gradient;
    shape.fillPaint.gradient = ramp({0, 0, 0}, {40, 40, 40});   // 41 levels at 8 bits over 160 pixels
    shape.fillPaint.gradient.angle = 0;
    Layer layer(Asset::make(std::make_shared<Image>(1, 1), "Ramp"), Point(0, 0));
    setVectorShape(layer, doc, shape);
    REQUIRE(layer.asset->image.u16() != nullptr);
    const Image16& image = *layer.asset->image.u16();
    std::set<int> levels;
    for (int x = 0; x < image.width(); x++) levels.insert(image.pixel(x, image.height() / 2)[0]);
    CHECK(levels.size() > 100);   // one per pixel, where 8 bits have 41
}

TEST_CASE(vector_masks_and_fill_layers_at_sixteen_bits) {
    // A vector mask on a pixel layer, moved with the layer, and its twin reduced and at 1:1.
    Document doc = backdrop();
    Layer paint(Asset::make(soft(100, 70), "Paint"), Point(30, 25));
    VectorPath cut = ellipsePath(Rect(35.5, 30.25, 70, 50));
    addShapeComponent(cut, rectanglePath(Rect(60, 40, 30, 20)), VectorPath::Op::Subtract);
    setLayerVectorMask(paint, doc, cut);
    doc.layers.push_back(paint);
    Apart a = renderApart(doc);
    report("vector mask", a);
    CHECK(a.worst <= 1);
    a = renderApart(doc, reduced());
    report("vector mask @0.5", a);
    // Reduced, the layer's own pixels are resampled at each depth (P2's resampling: two levels on a few samples).
    CHECK(a.worst <= 2 && a.beyondOne < 0.001);
    // A gradient fill layer (no pixels of its own: its 'GdFl' drawn over the canvas).
    Document fill = backdrop();
    Layer layer("Gradient Fill", fill.size());
    auto carry = std::make_shared<PsdLayerCarry>();
    StyleGradient g = ramp({250, 200, 10}, {10, 40, 220});
    g.angle = 60;
    carry->blocks.push_back({"GdFl", authorGradientFill(g)});
    layer.psdCarry = carry;
    layer.opacity = 0.7;
    fill.layers.push_back(layer);
    a = renderApart(fill);
    report("gradient fill layer", a);
    CHECK(a.worst <= 2 && a.beyondOne < 0.0005);   // the ramp's three 8-bit roundings, as for shapes (0.02%)
}

// ---- Layer styles ----------------------------------------------------------------------------------------------------

namespace {

struct StyleScene { std::string name; LayerStyle style; BlendMode mode = BlendMode::Normal; double opacity = 1; int allowed = 1; double share = 0; };

std::vector<StyleScene> styleScenes() {
    std::vector<StyleScene> out;
    auto add = [&](const std::string& name, const std::function<void(LayerStyle&)>& make, int allowed = 1, double share = 0) {
        StyleScene s; s.name = name; make(s.style); s.allowed = allowed; s.share = share; out.push_back(s);
    };
    add("drop shadow", [](LayerStyle& s) { DropShadow d; d.distance = 6; d.size = 8; d.spread = 10; s.dropShadows.push_back(d); });
    add("inner shadow", [](LayerStyle& s) { InnerShadow d; d.distance = 4; d.size = 6; d.choke = 20; s.innerShadows.push_back(d); });
    add("outer glow", [](LayerStyle& s) { OuterGlow g; g.size = 9; g.spread = 15; s.outerGlows.push_back(g); });
    add("outer glow precise", [](LayerStyle& s) { OuterGlow g; g.size = 7; g.precise = true; s.outerGlows.push_back(g); });
    add("inner glow", [](LayerStyle& s) { InnerGlow g; g.size = 8; g.choke = 10; s.innerGlows.push_back(g); });
    add("inner glow center", [](LayerStyle& s) { InnerGlow g; g.size = 6; g.center = true; g.precise = true; s.innerGlows.push_back(g); });
    add("satin", [](LayerStyle& s) { Satin t; t.size = 10; t.distance = 8; s.satins.push_back(t); });
    add("color overlay", [](LayerStyle& s) { ColorOverlay c; c.color = {20, 180, 90}; c.opacity = 0.6f; c.mode = EffectBlend::Multiply; s.colorOverlays.push_back(c); });
    add("gradient overlay", [](LayerStyle& s) { GradientOverlay g; g.gradient = ramp({255, 0, 0}, {0, 0, 255}); g.opacity = 0.8f; s.gradientOverlays.push_back(g); });
    add("pattern overlay", [](LayerStyle& s) { PatternOverlay p; p.patternId = "tile"; p.opacity = 0.7f; s.patternOverlays.push_back(p); });
    add("pattern overlay scaled", [](LayerStyle& s) { PatternOverlay p; p.patternId = "tile"; p.scale = 1.7f; p.angle = 20; s.patternOverlays.push_back(p); });
    add("stroke outside", [](LayerStyle& s) { Stroke k; k.size = 3; k.color = {0, 0, 0}; s.strokes.push_back(k); });
    add("stroke inside gradient", [](LayerStyle& s) { Stroke k; k.size = 4; k.position = Stroke::Position::Inside; k.gradientFill = true; k.gradient = ramp({255, 255, 0}, {0, 128, 255}); s.strokes.push_back(k); });
    add("stroke center shape burst", [](LayerStyle& s) { Stroke k; k.size = 5; k.position = Stroke::Position::Center; k.gradientFill = true; k.gradient = ramp({255, 0, 255}, {0, 255, 0}); k.gradient.type = StyleGradient::Type::ShapeBurst; s.strokes.push_back(k); });
    add("bevel inner smooth", [](LayerStyle& s) { Bevel b; b.size = 7; s.bevels.push_back(b); });
    add("bevel outer chisel", [](LayerStyle& s) { Bevel b; b.kind = Bevel::Kind::Outer; b.technique = Bevel::Technique::ChiselHard; b.size = 6; s.bevels.push_back(b); });
    add("emboss soft", [](LayerStyle& s) { Bevel b; b.kind = Bevel::Kind::Emboss; b.technique = Bevel::Technique::ChiselSoft; b.size = 6; b.soften = 2; s.bevels.push_back(b); });
    add("pillow emboss", [](LayerStyle& s) { Bevel b; b.kind = Bevel::Kind::Pillow; b.size = 8; s.bevels.push_back(b); });
    add("stroke emboss", [](LayerStyle& s) { Stroke k; k.size = 4; s.strokes.push_back(k); Bevel b; b.kind = Bevel::Kind::StrokeEmboss; b.size = 4; s.bevels.push_back(b); });
    add("bevel texture", [](LayerStyle& s) { Bevel b; b.size = 5; b.useTexture = true; b.texturePattern = "tile"; s.bevels.push_back(b); });
    // Every effect at once: the 8-bit engine rounds to a byte after each effect it composites, ten times here.
    add("everything", [](LayerStyle& s) {
        DropShadow d; d.distance = 5; d.size = 5; s.dropShadows.push_back(d);
        OuterGlow o; o.size = 6; s.outerGlows.push_back(o);
        InnerShadow i; i.size = 4; s.innerShadows.push_back(i);
        InnerGlow g; g.size = 5; s.innerGlows.push_back(g);
        Satin t; s.satins.push_back(t);
        ColorOverlay c; c.color = {200, 200, 40}; c.opacity = 0.3f; s.colorOverlays.push_back(c);
        GradientOverlay go; go.gradient = ramp({0, 0, 0}, {255, 255, 255}); go.opacity = 0.3f; go.mode = EffectBlend::Overlay; s.gradientOverlays.push_back(go);
        PatternOverlay p; p.patternId = "tile"; p.opacity = 0.2f; s.patternOverlays.push_back(p);
        Stroke k; k.size = 2; s.strokes.push_back(k);
        Bevel b; b.size = 4; s.bevels.push_back(b);
    }, 2, 0.0001);
    // The layer in a mode over its shadow, and at a Fill below 100% (the overlays then paint their own pass).
    StyleScene blended; blended.name = "multiply layer over its shadow"; blended.mode = BlendMode::Multiply;
    { DropShadow d; d.distance = 6; d.size = 6; blended.style.dropShadows.push_back(d); ColorOverlay c; c.opacity = 0.5f; blended.style.colorOverlays.push_back(c); }
    out.push_back(blended);
    StyleScene faded; faded.name = "opacity 0.6"; faded.opacity = 0.6;
    { Stroke k; k.size = 3; faded.style.strokes.push_back(k); InnerGlow g; g.size = 5; faded.style.innerGlows.push_back(g); }
    out.push_back(faded);
    return out;
}

} // namespace

/// At 1:1 every effect is within a level of its 8-bit render (the document's pixels, what is saved, merged and
/// exported). A reduced view is drawn from the layer resampled at each depth, and the 16-bit resampling is not the
/// 8-bit one rounded (P2): the matte differs by up to two levels, so where an effect decides on it (the stroke's
/// half-coverage contour, the precise glow's and chisel bevel's distance fields) an edge pixel can land on the
/// other side. Those are held to a share of the samples: at most 1.5%.
TEST_CASE(layer_styles_at_sixteen_bits_are_within_a_level_of_eight_bits) {
    int failures = 0;
    for (const StyleScene& scene : styleScenes()) {
        const Document doc = styled(scene.style, scene.mode, scene.opacity);
        REQUIRE(layerStyleOf(doc.layers.back(), doc) != nullptr);
        for (auto [options, suffix] : {std::pair{RenderOptions(), ""}, std::pair{reduced(), " @0.5"}}) {
            const Apart a = renderApart(doc, options);
            report("style: " + scene.name + suffix, a);
            const bool whole = options.scale == 1;
            if (whole ? a.worst > scene.allowed || a.beyondOne > scene.share : a.beyondOne > 0.015) {
                std::fprintf(stderr, "  style %s%s: %d levels apart, %.3f%% beyond a level\n", scene.name.c_str(), suffix, a.worst, a.beyondOne * 100);
                failures++;
            }
        }
    }
    CHECK_EQ(failures, 0);
}

TEST_CASE(folder_styles_at_sixteen_bits) {
    // A folder with a style: its exterior effects under the children, the rest over what they made.
    Document doc = backdrop();
    Layer folder("Folder", doc.size());
    folder.isGroup = true;
    LayerStyle style;
    OuterGlow glow; glow.size = 8; style.outerGlows.push_back(glow);
    Stroke stroke; stroke.size = 2; stroke.color = {255, 255, 255}; style.strokes.push_back(stroke);
    ColorOverlay tint; tint.color = {0, 90, 200}; tint.opacity = 0.4f; style.colorOverlays.push_back(tint);
    setLayerStyle(folder, style);
    Layer a(Asset::make(soft(80, 60), "A"), Point(20, 20));
    a.parentId = folder.id;
    Layer b(Asset::make(soft(60, 50), "B"), Point(70, 40));
    b.parentId = folder.id;
    b.blendMode = BlendMode::Screen;
    doc.layers.push_back(folder);
    doc.layers.push_back(a);
    doc.layers.push_back(b);
    for (bool passThrough : {true, false}) {
        doc.layers[1].passThrough = passThrough;
        const Apart at1 = renderApart(doc);
        const Apart half = renderApart(doc, reduced());
        report(std::string("folder style") + (passThrough ? " (pass through)" : " (isolated)"), at1);
        report(std::string("folder style @0.5") + (passThrough ? " (pass through)" : " (isolated)"), half);
        CHECK(at1.worst <= 1);
        CHECK(half.beyondOne <= 0.01);   // reduced: the resampled matte's contour, as for layers above
    }
}

TEST_CASE(a_sixteen_bit_style_keeps_a_smooth_ramp_smooth) {
    // A gradient overlay across a wide layer: 8 bits give at most 256 steps; 16 bits one per pixel along the ramp.
    Document doc(1024, 16);
    auto white = std::make_shared<Image>(1024, 16);
    white->fill(255, 255, 255, 255);
    doc.layers.push_back(Layer(Asset::make(white, "Paint"), Point(0, 0)));
    LayerStyle style;
    GradientOverlay g; g.gradient = ramp({0, 0, 0}, {64, 64, 64}); g.gradient.angle = 0; style.gradientOverlays.push_back(g);
    setLayerStyle(doc.layers[0], style);
    Image16 deep;
    render16(sixteen(doc), {}, deep);
    std::set<int> levels;
    for (int x = 0; x < 1024; x++) levels.insert(deep.pixel(x, 8)[0]);
    CHECK(levels.size() > 500);   // 65 levels at 8 bits
}

// The view at a quarter size draws from the layers' level-2 mips. With a mip budget too small for the level-1
// steps as well (a large 16-bit document at fit zoom), those steps are released and the view is unchanged; and it
// stays within a fraction of an 8-bit level of a box downscale of the 1:1 render.
TEST_CASE(deep_reduced_view_keeps_its_mips_under_a_tight_budget) {
    Document doc = backdrop();
    for (int i = 0; i < 3; i++) {
        Layer layer(Asset::make(base(W, H), "Copy"), Point(0, 0));
        layer.opacity = 0.4 + 0.1 * i;
        doc.layers.push_back(layer);
    }
    const Document deep = sixteen(doc);
    RenderOptions options;
    options.region = {0, 0, double(W), double(H)};
    options.scale = 0.25;
    MipCache& cache = MipCache::shared();
    const size_t previous = cache.budget();
    cache.clear();
    Image16 roomy;
    render16(deep, options, roomy);
    // Each layer's level 2 fits, level 1 beside it does not.
    const size_t level2 = size_t((W / 4) * (H / 4)) * 4 * sizeof(uint16_t), level1 = level2 * 4;
    cache.clear();
    cache.setBudget(deep.layers.size() * level2 + level1);
    Image16 tight;
    render16(deep, options, tight);
    CHECK(tight == roomy);
    CHECK(cache.bytesUsed() <= cache.budget());
    Image16 again;
    render16(deep, options, again);
    CHECK(again == roomy);
    cache.setBudget(previous);
    cache.clear();
    Image16 whole;
    render16(deep, {}, whole);
    const auto box = boxResizeImage(whole, roomy.width(), roomy.height());
    int worst = 0;
    for (int y = 0; y < roomy.height(); y++)
        for (int x = 0; x < roomy.width(); x++)
            for (int c = 0; c < 4; c++) worst = std::max(worst, std::abs(int(roomy.pixel(x, y)[c]) - int(box->pixel(x, y)[c])));
    CHECK(worst <= 64);   // under half an 8-bit level (128 in 0..32768)
}

TEST_MAIN()
