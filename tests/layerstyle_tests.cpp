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
    CHECK(!layerStyleFromJson(R"({"strokes": [{"sise": 3}]})", fromJson, &error));
    CHECK(error.find("sise") != std::string::npos);
    CHECK(!layerStyleFromJson(R"({"colorOverlays": [{"mode": "sparkle"}]})", fromJson, &error));
    // Clearing takes the block away.
    setLayerStyle(doc.layers[1], LayerStyle{});
    CHECK(layerStyleOf(doc.layers[1], doc) == nullptr);
    for (auto& b : doc.layers[1].psdCarry->blocks) CHECK(b.key != "lfx2");
}

TEST_MAIN()
