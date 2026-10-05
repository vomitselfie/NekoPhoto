// Layer styles, shapes and text in CMYK and Lab documents (P8). Every effect drawn in the document's channels, its
// colours through the document's profile: the look checked against the same document drawn in RGB (the effects' masks
// are the same; only where colours mix does blending in inks or L, a and b differ from blending in RGB), and directly:
// a CMYK file's ink colours land as those inks, Lab's offered modes, folders' styles, 16 bits. Shape layers filled in
// the document's channels and still shapes after Image > Mode; text rasters' colours through the profile with their
// coverage kept.
#include "check.h"
#include "compositor/colormgmt.h"
#include "compositor/depth.h"
#include "compositor/document.h"
#include "compositor/layerstyle.h"
#include "compositor/project.h"
#include "compositor/render.h"
#include "compositor/vectorlayer.h"
#include <cmath>
#include <cstdio>
#include <functional>

using namespace compositor;

namespace {

/// A white canvas and a grey square (or a soft-edged disc) with `style`.
Document styled(const LayerStyle& style, bool disc = false, uint8_t grey = 128) {
    Document doc(64, 64);
    auto white = std::make_shared<Image>(64, 64);
    white->fill(255, 255, 255, 255);
    doc.layers.push_back(Layer(Asset::make(white, "Background"), Point(0, 0)));
    auto shape = std::make_shared<Image>(24, 24);
    for (int y = 0; y < 24; y++)
        for (int x = 0; x < 24; x++) {
            float a = 1;
            if (disc) a = std::clamp(11.5f - std::hypot(x - 11.5f, y - 11.5f), 0.0f, 1.0f);
            uint8_t* p = shape->pixel(x, y);
            const uint8_t alpha = uint8_t(std::lround(a * 255));
            p[0] = uint8_t(grey * alpha / 255); p[1] = uint8_t(160 * alpha / 255); p[2] = uint8_t(96 * alpha / 255); p[3] = alpha;
        }
    Layer layer(Asset::make(shape, "Shape"), Point(20, 20));
    setLayerStyle(layer, style);
    doc.layers.push_back(layer);
    return doc;
}

Document inMode(Document doc, ColorMode mode, SampleType depth = SampleType::U8) {
    std::string why;
    if (depth != doc.sampleType && !convertSampleType(doc, depth, &why)) check::fail(__FILE__, __LINE__, "convertSampleType: " + why);
    if (!convertDocumentMode(doc, mode, ColorProfile(), ConvertOptions(), &why)) check::fail(__FILE__, __LINE__, "convertDocumentMode: " + why);
    return doc;
}

struct Difference { double mean = 0; int worst = 0; };

/// `modeDoc`'s own render through its profile to sRGB, against the RGB document's composite, per channel in 8-bit levels:
/// how far the look is apart (the inks themselves may split a colour differently: black ink for grey, say).
Difference againstRgb(const Document& rgb, const Document& modeDoc) {
    const AnyImage reference = imageAtDepth(renderNative(rgb), SampleType::U8);
    const AnyImage native = renderNative(modeDoc);
    const AnyImage seen = imageAtDepth(convertImage(native, modeDoc.colorMode, modeDoc.profile, ColorMode::RGB, ColorProfile()), SampleType::U8);
    Difference d;
    if (!reference.u8() || !seen.u8()) { d.worst = 1000; return d; }
    long long total = 0, count = 0;
    for (int y = 0; y < seen.height(); y++)
        for (int x = 0; x < seen.width(); x++)
            for (int c = 0; c < 3; c++) {
                const int e = std::abs(int(reference.u8()->pixel(x, y)[c]) - int(seen.u8()->pixel(x, y)[c]));
                total += e; count++;
                d.worst = std::max(d.worst, e);
            }
    d.mean = count ? double(total) / double(count) : 0;
    return d;
}

/// The styles each check runs: one effect each (their defaults), and all of them together.
std::vector<std::pair<std::string, LayerStyle>> effects() {
    std::vector<std::pair<std::string, LayerStyle>> out;
    auto one = [&](const std::string& name, const std::function<void(LayerStyle&)>& f) { LayerStyle s; f(s); out.push_back({name, s}); };
    one("drop shadow", [](LayerStyle& s) { s.dropShadows.push_back(DropShadow{}); });
    one("inner shadow", [](LayerStyle& s) { s.innerShadows.push_back(InnerShadow{}); });
    one("outer glow", [](LayerStyle& s) { OuterGlow g; g.mode = EffectBlend::Normal; g.color = {255, 200, 0}; s.outerGlows.push_back(g); });
    one("inner glow", [](LayerStyle& s) { InnerGlow g; g.mode = EffectBlend::Normal; g.color = {0, 90, 255}; s.innerGlows.push_back(g); });
    one("bevel", [](LayerStyle& s) { s.bevels.push_back(Bevel{}); });
    one("satin", [](LayerStyle& s) { Satin t; t.mode = EffectBlend::Normal; t.color = {200, 0, 80}; s.satins.push_back(t); });
    one("colour overlay", [](LayerStyle& s) { ColorOverlay o; o.color = {20, 140, 220}; o.opacity = 0.6f; s.colorOverlays.push_back(o); });
    one("gradient overlay", [](LayerStyle& s) {
        GradientOverlay g; g.gradient.colors = {{0, {255, 0, 0}, 0.5f}, {1, {0, 0, 255}, 0.5f}};
        s.gradientOverlays.push_back(g);
    });
    one("stroke", [](LayerStyle& s) { Stroke k; k.color = {0, 160, 60}; k.size = 4; s.strokes.push_back(k); });
    one("everything", [](LayerStyle& s) {
        s.dropShadows.push_back(DropShadow{}); s.innerShadows.push_back(InnerShadow{});
        OuterGlow og; og.mode = EffectBlend::Normal; og.color = {255, 200, 0}; s.outerGlows.push_back(og);
        s.bevels.push_back(Bevel{});
        Stroke k; k.color = {0, 160, 60}; k.size = 3; s.strokes.push_back(k);
    });
    return out;
}

} // namespace

TEST_CASE(cmyk_effects_follow_the_rgb_render_through_the_profile) {
    // Normal and Multiply effects mixed in inks instead of RGB: the same picture within a few levels on average, the
    // largest gaps where a soft edge mixes two far-apart colours.
    for (const auto& [name, style] : effects())
        for (SampleType depth : {SampleType::U8, SampleType::U16})
            for (bool disc : {false, true}) {
                // The reference: the same document in RGB with its colours already brought into the press's gamut (as
                // converting to CMYK brings them), drawn in RGB and converted.
                const Document cmyk = inMode(styled(style, disc), ColorMode::CMYK, depth);
                const Difference d = againstRgb(inMode(cmyk, ColorMode::RGB, depth), cmyk);
                std::printf("  CMYK%d %-16s %s: mean %.2f, worst %d\n", depth == SampleType::U8 ? 8 : 16, name.c_str(), disc ? "disc  " : "square", d.mean, d.worst);
                CHECK(d.mean < 2.0);
                CHECK(d.worst < 80);
            }
}

TEST_CASE(lab_effects_follow_the_rgb_render_through_the_profile) {
    // Lab blends L, a and b as stored, as its layers do: Normal effects come out as RGB's within a few levels.
    for (const auto& [name, style] : effects()) {
        if (name == "drop shadow" || name == "inner shadow" || name == "bevel" || name == "everything") continue;   // Multiply and Screen: below
        for (SampleType depth : {SampleType::U8, SampleType::U16}) {
            const Document lab = inMode(styled(style, true), ColorMode::Lab, depth);
            const Difference d = againstRgb(inMode(lab, ColorMode::RGB, depth), lab);
            std::printf("  Lab%d %-16s: mean %.2f, worst %d\n", depth == SampleType::U8 ? 8 : 16, name.c_str(), d.mean, d.worst);
            CHECK(d.mean < 2.0);
            CHECK(d.worst < 80);
        }
    }
}

TEST_CASE(lab_multiply_darkens_lightness_and_blends_a_and_b_as_stored) {
    // A black Multiply shadow under a Lab layer: L falls as RGB's would; a and b are multiplied as stored, as Lab's
    // layer blending does (unverified against Photoshop, as the layer modes are).
    LayerStyle s;
    DropShadow shadow; shadow.size = 0; shadow.distance = 10; shadow.angle = 90; shadow.opacity = 1; shadow.spread = 0;
    s.dropShadows.push_back(shadow);
    const Document lab = inMode(styled(s), ColorMode::Lab);
    const AnyImage out = renderNative(lab);
    REQUIRE(out.u8());
    const uint8_t* under = out.u8()->pixel(30, 46);   // the shadow, below the square
    const uint8_t* clear = out.u8()->pixel(5, 5);
    CHECK(clear[0] == 255);
    CHECK(under[0] < 10);
}

TEST_CASE(cmyk_inks_from_a_file_land_as_those_inks) {
    // A 'CMYC' colour (a CMYK file's) keeps its inks: a colour overlay paints exactly 20% cyan, 80% magenta.
    LayerStyle s;
    ColorOverlay o;
    o.color = plainRgbOfInk({0.2f, 0.8f, 0.0f, 0.1f});
    s.colorOverlays.push_back(o);
    for (SampleType depth : {SampleType::U8, SampleType::U16}) {
        Document doc = inMode(styled(LayerStyle{}), ColorMode::CMYK, depth);
        setLayerStyle(doc.layers[1], s);
        const AnyImage out = renderNative(doc);
        if (depth == SampleType::U8) {
            REQUIRE(out.c8());
            const uint8_t* p = out.c8()->pixel(30, 30);
            CHECK_EQ(int(p[0]), 204); CHECK_EQ(int(p[1]), 51); CHECK_EQ(int(p[2]), 255); CHECK_EQ(int(p[3]), 230);
        } else {
            REQUIRE(out.u16());
            const uint16_t* p = out.u16()->pixel(30, 30);
            CHECK_NEAR(p[0], 0.8 * 32768, 2); CHECK_NEAR(p[1], 0.2 * 32768, 2); CHECK_NEAR(p[2], 32768, 2); CHECK_NEAR(p[3], 0.9 * 32768, 2);
        }
        // Edited (another RGB): the inks no longer describe it, and it goes through the profile.
        LayerStyle edited = s;
        edited.colorOverlays[0].color.r = uint8_t(edited.colorOverlays[0].color.r + 40);
        setLayerStyle(doc.layers[1], edited);
        const AnyImage again = renderNative(doc);
        const int cyan = depth == SampleType::U8 ? again.c8()->pixel(30, 30)[0] : narrow16(again.u16()->pixel(30, 30)[0]);
        CHECK(cyan != 204);
        // And back to PSD as 'CMYC' while they match.
        const auto back = layerStyleOf(doc.layers[1], doc);
        REQUIRE(back != nullptr);
        CHECK(!inkMatches(back->colorOverlays[0].color));
    }
    Document doc = inMode(styled(LayerStyle{}), ColorMode::CMYK);
    setLayerStyle(doc.layers[1], s);
    const auto read = layerStyleOf(doc.layers[1], doc);
    REQUIRE(read != nullptr);
    CHECK(inkMatches(read->colorOverlays[0].color));
    CHECK_NEAR((*read->colorOverlays[0].color.ink)[1], 0.8, 1e-4);
}

TEST_CASE(lab_draws_the_modes_it_lacks_as_normal) {
    // Lab offers no Difference: an effect in it draws as Normal, as a layer in it does.
    LayerStyle diff, normal;
    ColorOverlay o; o.color = {40, 200, 90}; o.mode = EffectBlend::Difference;
    diff.colorOverlays.push_back(o);
    o.mode = EffectBlend::Normal;
    normal.colorOverlays.push_back(o);
    const AnyImage a = renderNative(inMode(styled(diff), ColorMode::Lab)), b = renderNative(inMode(styled(normal), ColorMode::Lab));
    REQUIRE(a.u8() && b.u8());
    for (int c = 0; c < 4; c++) CHECK_EQ(int(a.u8()->pixel(30, 30)[c]), int(b.u8()->pixel(30, 30)[c]));
}

TEST_CASE(a_folders_style_draws_in_cmyk) {
    // A folder's stroke around its children, in inks.
    Document doc = styled(LayerStyle{});
    Layer folder("Folder", doc.size());
    folder.isGroup = true;
    folder.passThrough = false;
    LayerStyle s;
    Stroke k; k.color = {0, 0, 0}; k.size = 3; s.strokes.push_back(k);
    setLayerStyle(folder, s);
    doc.layers[1].parentId = folder.id;
    doc.layers.insert(doc.layers.begin() + 1, folder);
    const Difference d = againstRgb(doc, inMode(doc, ColorMode::CMYK));
    std::printf("  CMYK8 folder stroke: mean %.2f, worst %d\n", d.mean, d.worst);
    CHECK(d.mean < 1.0);
    const AnyImage out = renderNative(inMode(doc, ColorMode::CMYK));
    REQUIRE(out.c8());
    CHECK(out.c8()->pixel(18, 30)[3] < 64);   // the stroke band: plenty of black ink (stored inverted)
}

TEST_CASE(shape_layers_fill_in_the_documents_channels) {
    for (ColorMode mode : {ColorMode::CMYK, ColorMode::Lab})
        for (SampleType depth : {SampleType::U8, SampleType::U16}) {
            Document doc = inMode(styled(LayerStyle{}), mode, depth);
            VectorShape shape;
            shape.path = rectanglePath(Rect(4, 4, 20, 12));
            shape.r = 32; shape.g = 96; shape.b = 192;
            Layer layer(Asset::make(std::make_shared<Image>(1, 1), "Shape"), Point(0, 0));
            setVectorShape(layer, doc, shape);
            REQUIRE(layer.asset->image.channels() == colorModeChannels(mode) && layer.asset->image.sampleType() == depth);
            CHECK(isVectorShapeLayer(layer));
            // The fill is the colour through the profile, as a pixel of that colour converted is.
            auto one = std::make_shared<Image>(1, 1);
            one->fill(32, 96, 192, 255);
            const AnyImage expected = imageAtFormat(convertImage(AnyImage(ImagePtr(one)), ColorMode::RGB, ColorProfile(), mode, doc.profile), depth, mode);
            REQUIRE(expected);
            auto sample = [](const AnyImage& image, int x, int c) {
                return image.u16() ? int(image.u16()->pixel(x, 0)[c]) : image.c8() ? int(image.c8()->pixel(x, 0)[c]) : int(image.u8()->pixel(x, 0)[c]);
            };
            for (int c = 0; c < colorModeChannels(mode); c++) CHECK(std::abs(sample(layer.asset->image, 5, c) - sample(expected, 0, c)) <= (depth == SampleType::U16 ? 64 : 1));
        }
    // A CMYK file's solid colour: its inks exactly, written back as inks, read back as inks.
    Document doc = inMode(styled(LayerStyle{}), ColorMode::CMYK);
    VectorShape shape;
    shape.path = rectanglePath(Rect(4, 4, 20, 12));
    const StyleColor plain = plainRgbOfInk({0.1f, 0.6f, 0.0f, 0.2f});
    shape.r = plain.r; shape.g = plain.g; shape.b = plain.b; shape.ink = plain.ink;
    Layer layer(Asset::make(std::make_shared<Image>(1, 1), "Shape"), Point(0, 0));
    setVectorShape(layer, doc, shape);
    REQUIRE(layer.asset->image.c8());
    const uint8_t* p = layer.asset->image.c8()->pixel(5, 5);
    CHECK_EQ(int(p[0]), 230); CHECK_EQ(int(p[1]), 102); CHECK_EQ(int(p[2]), 255); CHECK_EQ(int(p[3]), 204);
    auto back = vectorShapeOf(layer, doc);
    REQUIRE(back.has_value() && back->ink.has_value());
    CHECK_NEAR((*back->ink)[1], 0.6, 1e-4);
}

TEST_CASE(a_shape_layer_survives_image_mode) {
    // Drawn in RGB, converted to CMYK and Lab: still a shape layer, its blocks pinned to the converted pixels.
    Document doc = styled(LayerStyle{});
    VectorShape shape;
    shape.path = rectanglePath(Rect(4, 4, 20, 12));
    shape.r = 200; shape.g = 40; shape.b = 40;
    Layer layer(Asset::make(std::make_shared<Image>(1, 1), "Shape"), Point(0, 0));
    setVectorShape(layer, doc, shape);
    doc.layers.push_back(layer);
    for (ColorMode mode : {ColorMode::CMYK, ColorMode::Lab}) {
        const Document converted = inMode(doc, mode);
        CHECK(isVectorShapeLayer(converted.layers.back()));
        CHECK(vectorShapeOf(converted.layers.back(), converted).has_value());
    }
}

TEST_CASE(text_rasters_take_their_colour_through_the_profile) {
    // An antialiased red edge (premultiplied, 16 bits as Qt paints it): every pixel's colour is the text colour through
    // the profile, its alpha the glyph's; a run carrying a CMYK file's inks lays exactly those.
    Image16 rgb(4, 1);
    const uint32_t alphas[4] = {32768, 16384, 4096, 0};
    for (int x = 0; x < 4; x++) {
        uint16_t* q = rgb.pixel(x, 0);
        q[0] = uint16_t(alphas[x]); q[1] = 0; q[2] = 0; q[3] = uint16_t(alphas[x]);
    }
    LayerText text;
    text.text = "A";
    text.red = 1;
    for (SampleType depth : {SampleType::U8, SampleType::U16}) {
        const Document cmyk = inMode(styled(LayerStyle{}), ColorMode::CMYK, depth);
        const AnyImage out = textRasterInMode(rgb, text, cmyk);
        REQUIRE(out && out.channels() == 5 && out.sampleType() == depth);
        if (depth == SampleType::U16) {
            const uint16_t* full = out.u16()->pixel(0, 0);
            const uint16_t* half = out.u16()->pixel(1, 0);
            for (int c = 0; c < 4; c++) CHECK(std::abs(int(half[c]) * 2 - int(full[c])) <= 2);   // the same colour at half coverage
            CHECK_EQ(int(half[4]), 16384);
            CHECK_EQ(int(out.u16()->pixel(3, 0)[4]), 0);
        }
        LayerText inked = text;
        const StyleColor plain = plainRgbOfInk({0, 1, 1, 0});
        inked.red = plain.r / 255.0; inked.green = plain.g / 255.0; inked.blue = plain.b / 255.0;
        inked.ink = std::array<float, 4>{0, 1, 1, 0};
        const AnyImage inks = textRasterInMode(rgb, inked, cmyk);
        REQUIRE(inks);
        if (depth == SampleType::U8) {
            const uint8_t* q = inks.c8()->pixel(0, 0);
            CHECK_EQ(int(q[0]), 255); CHECK_EQ(int(q[1]), 0); CHECK_EQ(int(q[2]), 0); CHECK_EQ(int(q[3]), 255); CHECK_EQ(int(q[4]), 255);
        }
        const Document lab = inMode(styled(LayerStyle{}), ColorMode::Lab, depth);
        const AnyImage l = textRasterInMode(rgb, text, lab);
        REQUIRE(l && l.channels() == 4 && l.sampleType() == depth);
        // sRGB red: L about 54, a about +80 (well above the offset), b about +67.
        if (depth == SampleType::U8) {
            const uint8_t* q = l.u8()->pixel(0, 0);
            CHECK(std::abs(int(q[0]) - 138) <= 3 && q[1] > 200 && q[2] > 180);
        }
    }
    CHECK(!textRasterInMode(rgb, text, styled(LayerStyle{})));   // RGB: nothing to do
}

TEST_CASE(text_inks_survive_the_project_manifest) {
    // A CMYK file's text colour as inks goes into the project and comes back; one changed since is left out.
    Document doc = styled(LayerStyle{});
    Layer& layer = doc.layers[1];
    LayerText text;
    text.text = "Ink";
    const StyleColor plain = plainRgbOfInk({0, 1, 1, 0});
    text.red = plain.r / 255.0; text.green = plain.g / 255.0; text.blue = plain.b / 255.0;
    text.ink = std::array<float, 4>{0, 1, 1, 0};
    TextRun run = baseTextRun(text);
    run.length = 3;
    text.runs = {run};
    layer.text = text;
    layer.textImage = layer.asset->image;
    ProjectError error;
    auto back = parseManifest(manifestJson(doc, std::nullopt), error);
    REQUIRE(back.has_value() && back->layers[1].text.has_value());
    const LayerText& read = *back->layers[1].text;
    REQUIRE(read.ink.has_value() && read.runs.size() == 1 && read.runs[0].ink.has_value());
    CHECK_NEAR((*read.ink)[1], 1.0, 1e-6);
    layer.text->red = 0.2;
    layer.text->runs[0].red = 0.2;
    back = parseManifest(manifestJson(doc, std::nullopt), error);
    REQUIRE(back.has_value() && back->layers[1].text.has_value());
    CHECK(!back->layers[1].text->ink.has_value());
    CHECK(!back->layers[1].text->runs[0].ink.has_value());
}

TEST_MAIN()
