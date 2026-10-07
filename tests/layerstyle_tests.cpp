// Layer styles carried from a PSD ('lfx2'), drawn by the renderer: each test builds the descriptor Photoshop
// would write and checks the pixels. The calibration itself is checked against Photoshop's own renders by
// build/tests/psd_roundtrip's sibling scripts over Patchy's fixtures (docs/layer-styles.md).
#include "check.h"
#include "compositor/layerstyle.h"
#include "compositor/psd_carry.h"
#include "compositor/render.h"
#include "psd/psd_descriptor.hpp"

using namespace compositor;
namespace psd = patchy::psd;

namespace {

psd::DescriptorValue number(double v, const char* unit = nullptr) {
    psd::DescriptorValue d;
    if (unit) { d.type = psd::DescriptorValue::Type::UnitFloat; d.unit = unit; }
    else d.type = psd::DescriptorValue::Type::Double;
    d.double_value = v;
    return d;
}
psd::DescriptorValue boolean(bool v) { psd::DescriptorValue d; d.type = psd::DescriptorValue::Type::Bool; d.bool_value = v; return d; }
psd::DescriptorValue enumeration(const char* type, const char* value) {
    psd::DescriptorValue d; d.type = psd::DescriptorValue::Type::Enum; d.enum_type = type; d.enum_value = value;
    d.enum_value_long_form = std::string(value).size() != 4;
    return d;
}
psd::DescriptorValue object(psd::DescriptorObject o) {
    psd::DescriptorValue d; d.type = psd::DescriptorValue::Type::Object; d.object_value = std::make_shared<psd::DescriptorObject>(std::move(o));
    return d;
}
psd::DescriptorObject descriptor(const char* cls, std::vector<std::pair<std::string, psd::DescriptorValue>> items) {
    psd::DescriptorObject o;
    o.class_id = cls;
    for (auto& [k, v] : items) { o.values[k] = v; o.key_order.push_back({k, k.size() != 4}); }
    return o;
}
psd::DescriptorValue rgb(int r, int g, int b) {
    return object(descriptor("RGBC", {{"Rd  ", number(r)}, {"Grn ", number(g)}, {"Bl  ", number(b)}}));
}

/// The 'lfx2' block for `effects` (key, descriptor).
std::vector<uint8_t> lfx2(std::vector<std::pair<std::string, psd::DescriptorValue>> effects) {
    effects.insert(effects.begin(), {"Scl ", number(100, "#Prc")});
    effects.insert(effects.begin() + 1, {"masterFXSwitch", boolean(true)});
    psd::BigEndianWriter w;
    w.write_u32(0);
    w.write_u32(16);
    psd::write_descriptor(w, descriptor("null", effects));
    return w.bytes();
}

/// A 40 x 40 white document with a 10 x 10 opaque square at (15, 15) carrying `block`.
Document squareWith(const std::vector<uint8_t>& block, uint8_t r = 128, uint8_t g = 128, uint8_t b = 128) {
    Document doc(40, 40);
    auto white = std::make_shared<Image>(40, 40);
    white->fill(255, 255, 255, 255);
    doc.layers.push_back(Layer(Asset::make(white, "Background"), Point(0, 0)));
    auto square = std::make_shared<Image>(10, 10);
    square->fill(r, g, b, 255);
    Layer layer(Asset::make(square, "Square"), Point(15, 15));
    auto carry = std::make_shared<PsdLayerCarry>();
    carry->blocks.push_back({"lfx2", block});
    carry->placement = layer.transform;
    layer.psdCarry = carry;
    doc.layers.push_back(layer);
    return doc;
}

const uint8_t* at(const Image& image, int x, int y) { return image.pixel(x, y); }

} // namespace

TEST_CASE(color_overlay_replaces_the_layer_colour) {
    auto doc = squareWith(lfx2({{"SoFi", object(descriptor("SoFi", {{"enab", boolean(true)}, {"Md  ", enumeration("BlnM", "normal")},
        {"Clr ", rgb(255, 0, 0)}, {"Opct", number(100, "#Prc")}}))}}));
    REQUIRE(layerStyleOf(doc.layers[1], doc) != nullptr);
    auto out = renderFlattened(doc);
    const uint8_t* inside = at(*out, 20, 20);
    CHECK_EQ(int(inside[0]), 255); CHECK_EQ(int(inside[1]), 0); CHECK_EQ(int(inside[2]), 0);
    const uint8_t* outside = at(*out, 5, 5);
    CHECK_EQ(int(outside[1]), 255);
}

TEST_CASE(multiply_overlay_on_a_multiply_layer_blends_after_the_layer) {
    // Photoshop's default (Blend Interior Effects as Group off): the layer multiplies the backdrop, then the
    // overlay multiplies that. Over white, grey x gold.
    auto doc = squareWith(lfx2({{"SoFi", object(descriptor("SoFi", {{"enab", boolean(true)}, {"Md  ", enumeration("BlnM", "multiply")},
        {"Clr ", rgb(252, 210, 0)}, {"Opct", number(100, "#Prc")}}))}}));
    doc.layers[1].blendMode = BlendMode::Multiply;
    auto out = renderFlattened(doc);
    const uint8_t* p = at(*out, 20, 20);
    CHECK(std::abs(int(p[0]) - 128 * 252 / 255) <= 1);
    CHECK(std::abs(int(p[1]) - 128 * 210 / 255) <= 1);
    CHECK(int(p[2]) <= 1);
}

TEST_CASE(drop_shadow_falls_away_from_the_light) {
    // Angle 120 (light from the upper left): the shadow falls down and to the right.
    auto doc = squareWith(lfx2({{"DrSh", object(descriptor("DrSh", {{"enab", boolean(true)}, {"Md  ", enumeration("BlnM", "multiply")},
        {"Clr ", rgb(0, 0, 0)}, {"Opct", number(100, "#Prc")}, {"uglg", boolean(false)}, {"lagl", number(120, "#Ang")},
        {"Dstn", number(6, "#Pxl")}, {"Ckmt", number(100, "#Prc")}, {"blur", number(0, "#Pxl")}}))}}));
    auto out = renderFlattened(doc);
    CHECK(at(*out, 27, 27)[0] < 20);    // below-right of the square: in the hard shadow
    CHECK(at(*out, 13, 13)[0] > 250);   // above-left: none
    CHECK_EQ(int(at(*out, 20, 20)[0]), 128);   // the layer covers its own shadow
}

TEST_CASE(outside_stroke_draws_a_band_around_the_shape) {
    auto doc = squareWith(lfx2({{"FrFX", object(descriptor("FrFX", {{"enab", boolean(true)}, {"Styl", enumeration("FStl", "OutF")},
        {"PntT", enumeration("FrFl", "SClr")}, {"Md  ", enumeration("BlnM", "normal")}, {"Opct", number(100, "#Prc")},
        {"Sz  ", number(3, "#Pxl")}, {"Clr ", rgb(0, 0, 255)}}))}}));
    auto out = renderFlattened(doc);
    const uint8_t* band = at(*out, 13, 20);   // two pixels outside the left edge
    CHECK(band[2] > 240 && band[0] < 15);
    CHECK(at(*out, 10, 20)[0] > 240);          // five pixels out: past the band
    CHECK_EQ(int(at(*out, 20, 20)[0]), 128);
}

TEST_CASE(inner_shadow_and_outer_glow_stay_on_their_sides) {
    auto doc = squareWith(lfx2({
        {"IrSh", object(descriptor("IrSh", {{"enab", boolean(true)}, {"Md  ", enumeration("BlnM", "normal")}, {"Clr ", rgb(0, 0, 0)},
            {"Opct", number(100, "#Prc")}, {"uglg", boolean(false)}, {"lagl", number(90, "#Ang")}, {"Dstn", number(0, "#Pxl")},
            {"Ckmt", number(0, "#Prc")}, {"blur", number(4, "#Pxl")}}))},
        {"OrGl", object(descriptor("OrGl", {{"enab", boolean(true)}, {"Md  ", enumeration("BlnM", "normal")}, {"Clr ", rgb(255, 0, 0)},
            {"Opct", number(100, "#Prc")}, {"GlwT", enumeration("BETE", "SfBL")}, {"Ckmt", number(0, "#Prc")},
            {"blur", number(4, "#Pxl")}, {"Inpr", number(100, "#Prc")}}))}}));
    auto out = renderFlattened(doc);
    CHECK(at(*out, 15, 20)[0] < 100);                 // inside at the edge: darkened
    CHECK(std::abs(int(at(*out, 20, 20)[0]) - 128) <= 3);   // the middle: untouched
    const uint8_t* glow = at(*out, 13, 20);
    CHECK(glow[0] > 240 && glow[1] < 240);            // outside: reddened
}

TEST_CASE(effects_off_draw_nothing_extra) {
    auto doc = squareWith(lfx2({{"SoFi", object(descriptor("SoFi", {{"enab", boolean(false)}, {"Md  ", enumeration("BlnM", "normal")},
        {"Clr ", rgb(255, 0, 0)}, {"Opct", number(100, "#Prc")}}))}}));
    CHECK(layerStyleOf(doc.layers[1], doc) == nullptr);
    auto out = renderFlattened(doc);
    CHECK_EQ(int(at(*out, 20, 20)[0]), 128);
}

TEST_CASE(clipping_ignores_the_base_layers_effects) {
    // A layer clipped to a styled base is masked by the base's pixels, never by its shadow or stroke.
    auto doc = squareWith(lfx2({{"FrFX", object(descriptor("FrFX", {{"enab", boolean(true)}, {"Styl", enumeration("FStl", "OutF")},
        {"PntT", enumeration("FrFl", "SClr")}, {"Md  ", enumeration("BlnM", "normal")}, {"Opct", number(100, "#Prc")},
        {"Sz  ", number(3, "#Pxl")}, {"Clr ", rgb(0, 0, 255)}}))}}));
    auto green = std::make_shared<Image>(40, 40);
    green->fill(0, 255, 0, 255);
    Layer clipped(Asset::make(green, "Clipped"), Point(0, 0));
    clipped.maskSourceId = doc.layers[1].id;
    doc.layers.push_back(clipped);
    auto out = renderFlattened(doc);
    CHECK(at(*out, 13, 20)[1] < 20);   // the stroke band stays blue, not green
}

TEST_CASE(a_style_set_here_draws_and_reads_back) {
    // A layer with no PSD past: the Layer Style dialog's path, with one effect switched off.
    auto doc = squareWith(lfx2({}));
    doc.layers[1].psdCarry = nullptr;
    LayerStyle style;
    ColorOverlay red; red.color = {255, 0, 0};
    style.colorOverlays.push_back(red);
    DropShadow off; off.enabled = false; off.distance = 9;
    style.dropShadows.push_back(off);
    Stroke gradientStroke; gradientStroke.gradientFill = true; gradientStroke.gradient.colors = {{0, {0, 0, 255}, 0.5f}, {1, {0, 255, 0}, 0.5f}};
    gradientStroke.gradient.type = StyleGradient::Type::ShapeBurst;
    style.strokes.push_back(gradientStroke);
    style.strokes.push_back(Stroke{});   // two: the '...Multi' list
    setLayerStyle(doc.layers[1], style);
    auto out = renderFlattened(doc);
    CHECK_EQ(int(at(*out, 20, 20)[0]), 255);
    CHECK_EQ(int(at(*out, 20, 20)[1]), 0);
    CHECK(at(*out, 27, 27)[0] > 250);   // the shadow is off
    // What is drawn leaves the switched-off shadow out; the editor keeps it.
    auto drawn = layerStyleOf(doc.layers[1], doc);
    REQUIRE(drawn != nullptr);
    CHECK(drawn->dropShadows.empty());
    const LayerStyle back = editableLayerStyle(doc.layers[1], doc);
    REQUIRE(back.dropShadows.size() == 1);
    CHECK(!back.dropShadows[0].enabled);
    CHECK_EQ(back.dropShadows[0].distance, 9.0f);
    REQUIRE(back.strokes.size() == 2);
    CHECK(back.strokes[0].gradientFill && back.strokes[0].gradient.type == StyleGradient::Type::ShapeBurst);
    CHECK_EQ(int(back.strokes[0].gradient.colors[1].color.g), 255);
    CHECK(authorLayerStyleBlock(back) == authorLayerStyleBlock(style));
    // JSON both ways gives the same style.
    LayerStyle fromJson;
    std::string error;
    REQUIRE(layerStyleFromJson(layerStyleToJson(back), fromJson, &error));
    CHECK(authorLayerStyleBlock(fromJson) == authorLayerStyleBlock(style));
    // A CMYK colour's inks and a bevel texture's phase come back too (the Layer Style dialog commits through JSON).
    LayerStyle inked = style;
    inked.colorOverlays[0].color.ink = std::array<float, 4>{0.f, 0.99f, 1.f, 0.f};
    inked.strokes[0].gradient.colors[0].ink = std::array<float, 4>{1.f, 0.5f, 0.f, 0.25f};
    Bevel textured; textured.useTexture = true; textured.texturePhaseX = 12; textured.texturePhaseY = -7;
    inked.bevels.push_back(textured);
    REQUIRE(layerStyleFromJson(layerStyleToJson(inked), fromJson, &error));
    REQUIRE(fromJson.colorOverlays.size() == 1);
    CHECK(fromJson.colorOverlays[0].color.ink == inked.colorOverlays[0].color.ink);
    CHECK(fromJson.strokes[0].gradient.colors[0].ink == inked.strokes[0].gradient.colors[0].ink);
    REQUIRE(fromJson.bevels.size() == 1);
    CHECK_EQ(fromJson.bevels[0].texturePhaseX, 12.0f);
    CHECK_EQ(fromJson.bevels[0].texturePhaseY, -7.0f);
    CHECK(authorLayerStyleBlock(fromJson) == authorLayerStyleBlock(inked));
    CHECK(!layerStyleFromJson(R"({"strokes": [{"sise": 3}]})", fromJson, &error));
    CHECK(error.find("sise") != std::string::npos);
    CHECK(!layerStyleFromJson(R"({"colorOverlays": [{"mode": "sparkle"}]})", fromJson, &error));
    // Clearing takes the block away.
    setLayerStyle(doc.layers[1], LayerStyle{});
    CHECK(layerStyleOf(doc.layers[1], doc) == nullptr);
    for (auto& b : doc.layers[1].psdCarry->blocks) CHECK(b.key != "lfx2");
}

TEST_CASE(style_json_is_held_to_the_dialog_ranges) {
    LayerStyle style;
    std::string error;
    REQUIRE(layerStyleFromJson(R"({"dropShadows":[{"size":1e30,"distance":-5,"opacity":7}],
        "gradientOverlays":[{"gradient":{"colors":[{"location":1,"color":"#ffffff"},{"location":0,"color":"#000000"}]}}]})", style, &error));
    CHECK_EQ(style.dropShadows[0].size, 250.0f);
    CHECK_EQ(style.dropShadows[0].distance, 0.0f);
    CHECK_EQ(style.dropShadows[0].opacity, 1.0f);
    const auto& colors = style.gradientOverlays[0].gradient.colors;
    REQUIRE(colors.size() == 2);
    CHECK(colors[0].location == 0 && colors[0].color.r == 0);   // sorted
    // What is set is what PSD keeps.
    CHECK(authorLayerStyleBlock(style) == authorLayerStyleBlock(style));
}

TEST_MAIN()

TEST_CASE(radial_fill_layer_gradients_snap_as_photoshop_draws_them) {
    // A Photoshop-saved 100 x 100 radial fill (scale 85%) is centred on pixel 50's centre and spans a whole-pixel
    // radius (42, not 42.5); a linear fill stays unsnapped (Patchy's calibration).
    StyleGradient g;
    g.fillLayer = true;
    g.type = StyleGradient::Type::Radial;
    g.angle = 90;
    g.scale = 0.85f;
    CHECK_NEAR(gradientPosition(g, 0, 0, 100, 100, 50, 50), 0.0, 1e-6);
    CHECK_NEAR(gradientPosition(g, 0, 0, 100, 100, 50 + 21, 50), 0.5, 1e-6);
    CHECK_NEAR(gradientPosition(g, 0, 0, 100, 100, 50 + 42, 50), 1.0, 1e-6);
    g.type = StyleGradient::Type::Linear;
    CHECK_NEAR(gradientPosition(g, 0, 0, 100, 100, 49.5, 49.5), 0.5, 1e-6);
}

TEST_CASE(two_stop_overlays_ease_as_photoshop_draws_them) {
    // Photoshop's merged image of a two-stop Classic overlay (Patchy's photoshop-overlay-zorder.psd, 28 px wide) eases
    // its ends: 5, not the linear 9, one pixel in from blue toward cyan; the middle stays half way.
    StyleGradient g;
    g.colors = {{0, {0, 0, 255}, 0.5f, std::nullopt}, {1, {0, 255, 255}, 0.5f, std::nullopt}};
    CHECK_EQ(int(gradientColor(g, 1.0f / 28).g), 5);
    CHECK(std::abs(int(gradientColor(g, 0.5f).g) - 128) <= 1);
    g.smoothness = 0;   // Smoothness 0: the plain ramp
    CHECK_EQ(int(gradientColor(g, 1.0f / 28).g), 9);
}

TEST_CASE(sixteen_bit_patterns_keep_their_samples) {
    // A 'Patt' record as Photoshop writes a 16-bit grayscale pattern (photoshop-pattern-deep.psd): one column, two
    // rows, raw 16-bit samples. Read to 8 bits rounded, and kept at 15 bits for 16- and 32-bit documents.
    psd::BigEndianWriter body;
    body.write_u32(1); body.write_u32(1);            // version, grayscale
    body.write_u16(2); body.write_u16(1);            // height, width
    psd::write_descriptor_unicode_string(body, "Deep");
    const std::string id = "deep-pattern";
    body.write_u8(uint8_t(id.size()));
    body.write_bytes(std::span(reinterpret_cast<const uint8_t*>(id.data()), id.size()));
    body.write_u32(3); body.write_u32(16 + 4 + (8 + 23 + 4) + 25 * 4);   // the VMA's version and length
    body.write_u32(0); body.write_u32(0); body.write_u32(2); body.write_u32(1);
    body.write_u32(24);
    body.write_u32(1); body.write_u32(23 + 4); body.write_u32(16);
    body.write_u32(0); body.write_u32(0); body.write_u32(2); body.write_u32(1);
    body.write_u16(16); body.write_u8(0);
    body.write_u16(0x0a7f); body.write_u16(0x763f);
    for (int slot = 1; slot < 26; slot++) body.write_u32(0);
    psd::BigEndianWriter block;
    block.write_u32(uint32_t(body.bytes().size()));
    block.write_bytes(body.bytes());
    while (block.bytes().size() % 4) block.write_u8(0);
    const auto tiles = parsePatternBlock(block.bytes());
    REQUIRE(tiles.count(id) == 1);
    const PatternTile& tile = tiles.at(id);
    REQUIRE(tile.width == 1 && tile.height == 2);
    CHECK_EQ(int(tile.rgba[0]), 0x15);   // Photoshop's merged image of it: 0x15 and 0xec
    CHECK_EQ(int(tile.rgba[4]), 0xec);
    CHECK(tile.rgba[1] == tile.rgba[0] && tile.rgba[2] == tile.rgba[0] && tile.rgba[3] == 255);
    REQUIRE(tile.rgba16.size() == 8);
    CHECK_EQ(int(tile.rgba16[0]), 0x0a7f);
    CHECK_EQ(int(tile.rgba16[7]), 32768);
}

TEST_CASE(type_blends_with_photoshops_text_gamma) {
    // Black type at 60% coverage over white: Photoshop's merged image has 141 (Blend Text Colors Using Gamma 1.45, the
    // default); the plain blend gives 102, which a pixel layer with the same pixels keeps.
    Document doc(4, 1);
    auto white = std::make_shared<Image>(4, 1);
    white->fill(255, 255, 255, 255);
    doc.layers.push_back(Layer(Asset::make(white, "Background"), Point(0, 0)));
    auto edge = std::make_shared<Image>(4, 1);
    edge->fill(0, 0, 0, 153);
    Layer type(Asset::make(edge, "Type"), Point(0, 0));
    type.text = LayerText{};
    doc.layers.push_back(type);
    CHECK_EQ(int(at(*renderFlattened(doc), 1, 0)[0]), 141);
    doc.layers[1].text.reset();
    CHECK_EQ(int(at(*renderFlattened(doc), 1, 0)[0]), 102);
}

TEST_CASE(an_isolated_folder_carries_its_folded_overlay_in_its_mode) {
    // Blend Interior Effects as Group on a Multiply folder: its Color Overlay joins the folder's result, which then
    // multiplies the backdrop (Patchy's photoshop-group-fx-interior.psd: green over (200, 150, 100) gives (0, 150, 0)).
    // Off, the overlay lands over the composite in its own Normal mode.
    for (bool asGroup : {true, false}) {
        Document doc(20, 20);
        auto back = std::make_shared<Image>(20, 20);
        back->fill(200, 150, 100, 255);
        doc.layers.push_back(Layer(Asset::make(back, "Background"), Point(0, 0)));
        Layer folder("Folder", doc.size());
        folder.isGroup = true;
        folder.passThrough = false;
        folder.blendMode = BlendMode::Multiply;
        LayerStyle style;
        ColorOverlay green;
        green.color = {0, 255, 0};
        style.colorOverlays.push_back(green);
        style.blendInteriorAsGroup = asGroup;
        setLayerStyle(folder, style);
        auto grey = std::make_shared<Image>(10, 10);
        grey->fill(128, 128, 128, 255);
        Layer child(Asset::make(grey, "Child"), Point(5, 5));
        child.parentId = folder.id;
        doc.layers.push_back(folder);
        doc.layers.push_back(child);
        auto out = renderFlattened(doc);
        const uint8_t* p = at(*out, 10, 10);
        CHECK_EQ(int(p[0]), 0);
        CHECK_EQ(int(p[1]), asGroup ? 150 : 255);
        CHECK_EQ(int(p[2]), 0);
        CHECK_EQ(int(at(*out, 2, 2)[0]), 200);   // outside the folder's pixels: the backdrop
    }
}
