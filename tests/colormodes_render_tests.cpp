// CMYK and Lab documents, steps C and D (docs/high-bit-depth-plan.md, "P7 plan"): which blend modes each mode offers,
// the CMYK and Lab executors at 8 and 16 bits against the RGB kernels they reuse, the display path (never without a
// colour transform), masks, folders and clipping in these modes, mode conversion (pixels, stored colours, dormant
// adjustment layers, per-channel settings), thumbnails and mip levels of 5-channel buffers.
#include "check.h"
#include "compositor/adjustments.h"
#include "compositor/blend.h"
#include "compositor/colormgmt.h"
#include "compositor/depth.h"
#include "compositor/document.h"
#include "compositor/render.h"
#include <cmath>
#include <cstdio>
#include <random>

using namespace compositor;

namespace {

std::shared_ptr<Image> noisy(int w, int h, uint32_t seed, bool translucent) {
    auto out = std::make_shared<Image>(w, h);
    std::mt19937 rng(seed);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t* p = out->pixel(x, y);
            const unsigned a = translucent ? unsigned(rng() % 256) : 255;
            for (int c = 0; c < 3; c++) p[c] = uint8_t((rng() % 256) * a / 255);
            p[3] = uint8_t(a);
        }
    return out;
}

Layer layerOf(const std::string& name, const AnyImage& image, Point origin = {0, 0}) { return Layer(Asset::makeAny(image, name), origin); }

/// An RGB document of `layers` noisy layers (the first opaque), `mode` on the others.
Document rgbDocument(int w, int h, int layers, BlendMode mode) {
    Document doc(w, h);
    for (int i = 0; i < layers; i++) {
        Layer l = layerOf("layer " + std::to_string(i), ImagePtr(noisy(w, h, uint32_t(11 + i), i > 0)));
        if (i > 0) l.blendMode = mode;
        doc.layers.push_back(l);
    }
    return doc;
}

Document inMode(Document doc, ColorMode mode) {
    std::string why;
    if (!convertDocumentMode(doc, mode, ColorProfile(), ConvertOptions(), &why)) check::fail(__FILE__, __LINE__, "convertDocumentMode: " + why);
    return doc;
}

int maxDifference(const Image& a, const Image& b) {
    if (a.width() != b.width() || a.height() != b.height()) return 256;
    int worst = 0;
    for (int y = 0; y < a.height(); y++)
        for (int i = 0; i < a.width() * 4; i++) worst = std::max(worst, std::abs(int(a.row(y)[i]) - int(b.row(y)[i])));
    return worst;
}

} // namespace

TEST_CASE(blend_modes_offered_per_mode) {
    const BlendMode notInLab[] = {BlendMode::ColorDodge, BlendMode::ColorBurn, BlendMode::Darken, BlendMode::Lighten,
                                  BlendMode::Difference, BlendMode::Exclusion, BlendMode::Subtract, BlendMode::Divide};
    int offeredInLab = 0;
    for (int m = 0; m < blendModeCount; m++) {
        const BlendMode mode = BlendMode(m);
        CHECK(blendModeAvailable(mode, ColorMode::RGB));
        CHECK(blendModeAvailable(mode, ColorMode::CMYK));
        offeredInLab += blendModeAvailable(mode, ColorMode::Lab);
        CHECK(!blendModeApproximated(mode, ColorMode::RGB));
        CHECK(!blendModeApproximated(mode, ColorMode::Lab));
    }
    CHECK_EQ(offeredInLab, blendModeCount - 8);
    for (BlendMode m : notInLab) { CHECK(!blendModeAvailable(m, ColorMode::Lab)); CHECK(blendModeFor(m, ColorMode::Lab) == BlendMode::Normal); }
    for (BlendMode m : {BlendMode::Hue, BlendMode::Saturation, BlendMode::Color, BlendMode::Luminosity, BlendMode::DarkerColor, BlendMode::LighterColor}) {
        CHECK(blendModeApproximated(m, ColorMode::CMYK));
        CHECK(blendModeFor(m, ColorMode::CMYK) == BlendMode::Normal);
        CHECK(blendModeFor(m, ColorMode::Lab) == m);
    }
    CHECK(blendModeFor(BlendMode::Multiply, ColorMode::CMYK) == BlendMode::Multiply);
}

TEST_CASE(cmyk_separable_modes_are_the_rgb_kernels_per_ink) {
    std::mt19937 rng(5);
    for (int m = 0; m < blendModeCount; m++) {
        const BlendMode mode = BlendMode(m);
        if (mode == BlendMode::Dissolve || blendModeApproximated(mode, ColorMode::CMYK)) continue;
        for (int trial = 0; trial < 200; trial++) {
            uint8_t src[5], dst[5];
            const uint8_t sa = uint8_t(rng() % 256), da = uint8_t(rng() % 256);
            for (int c = 0; c < 4; c++) { src[c] = uint8_t((rng() % 256) * sa / 255); dst[c] = uint8_t((rng() % 256) * da / 255); }
            src[4] = sa; dst[4] = da;
            const uint16_t k = uint16_t(rng() % 257);
            uint8_t out[5];
            std::memcpy(out, dst, 5);
            compositeSpanMode8(mode, ColorMode::CMYK, src, &k, out, 1);
            // The same inks as two RGBA pixels through the RGB kernel.
            uint8_t s1[4] = {src[0], src[1], src[2], sa}, d1[4] = {dst[0], dst[1], dst[2], da};
            uint8_t s2[4] = {src[3], src[3], src[3], sa}, d2[4] = {dst[3], dst[3], dst[3], da};
            if (k) { compositePixelSteps(mode, s1, k, d1); compositePixelSteps(mode, s2, k, d2); }
            CHECK_EQ(int(out[0]), int(d1[0])); CHECK_EQ(int(out[1]), int(d1[1])); CHECK_EQ(int(out[2]), int(d1[2]));
            CHECK_EQ(int(out[3]), int(d2[0])); CHECK_EQ(int(out[4]), int(d1[3]));
        }
    }
}

TEST_CASE(cmyk_display_is_the_documents_pixels_through_its_profile) {
    const Document doc = inMode(rgbDocument(48, 32, 1, BlendMode::Normal), ColorMode::CMYK);
    REQUIRE(doc.colorMode == ColorMode::CMYK);
    REQUIRE(doc.layers[0].asset->image.c8());
    CHECK(!doc.profile.icc.empty());   // the Working CMYK the document was converted to
    Image shown;
    render(doc, RenderOptions(), shown);
    Image expected(48, 32);
    auto t = transformBetween(doc.profile, srgbProfile(), ConvertOptions(), PixelFormat::CMYKA8, PixelFormat::RGBA8);
    REQUIRE(t);
    REQUIRE(convertImageTo8(doc.layers[0].asset->image, expected, *t));
    CHECK_EQ(maxDifference(shown, expected), 0);
    // A display transform of another layout is not used: the frame still goes through the document's profile.
    auto rgbDisplay = transformBetween(srgbProfile(), builtinProfile(WorkingSpace::AdobeRGB), ConvertOptions(), PixelFormat::RGBA8, PixelFormat::RGBA8);
    RenderOptions options;
    options.display = rgbDisplay.get();
    Image again;
    render(doc, options, again);
    CHECK_EQ(maxDifference(again, expected), 0);
    // The native render is the layer's own ink.
    const AnyImage native = renderNative(doc);
    REQUIRE(native.c8());
    CHECK(*native.c8() == *doc.layers[0].asset->image.c8());
}

TEST_CASE(cmyk_non_separable_modes_draw_as_normal_for_now) {
    const Document hue = inMode(rgbDocument(40, 30, 2, BlendMode::Hue), ColorMode::CMYK);
    Document normal = hue;
    normal.layers[1].blendMode = BlendMode::Normal;
    CHECK(*renderNative(hue).c8() == *renderNative(normal).c8());
}

TEST_CASE(cmyk_multiply_darkens_every_ink) {
    const Document doc = inMode(rgbDocument(40, 30, 2, BlendMode::Multiply), ColorMode::CMYK);
    Document normal = doc;
    normal.layers[1].blendMode = BlendMode::Normal;
    const AnyImage rendered = renderNative(doc);
    const ImageC8& a = *rendered.c8();
    const ImageC8& base = *doc.layers[0].asset->image.c8();
    // Inverted ink: Multiply only ever lowers the stored value (adds ink), never below the product.
    for (int y = 0; y < a.height(); y++)
        for (int x = 0; x < a.width(); x++)
            for (int c = 0; c < 4; c++) CHECK(a.pixel(x, y)[c] <= base.pixel(x, y)[c]);
    CHECK(!(a == *renderNative(normal).c8()));
}

TEST_CASE(lab_normal_is_the_rgb_normal_on_the_same_values) {
    // The same bytes rendered as an RGB document and as a Lab one: Normal's arithmetic is the RGB kernel's.
    Document rgb = rgbDocument(36, 28, 3, BlendMode::Normal);
    rgb.layers[1].opacity = 0.6;
    Document lab = rgb;
    lab.colorMode = ColorMode::Lab;
    RenderOptions plain;
    Image rgbOut;
    render(rgb, plain, rgbOut);
    const AnyImage labOut = renderNative(lab);
    REQUIRE(labOut.u8());
    CHECK_EQ(maxDifference(rgbOut, *labOut.u8()), 0);
}

TEST_CASE(lab_luminosity_and_color_split_lightness_from_colour) {
    for (SampleType type : {SampleType::U8, SampleType::U16}) {
        Document doc(4, 4);
        doc.colorMode = ColorMode::Lab;
        auto solid = [&](double l, double a, double b) {
            auto img = std::make_shared<Image>(4, 4);
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++) {
                    uint8_t* p = img->pixel(x, y);
                    p[0] = storedLabL<SampleType::U8>(l, 255); p[1] = storedLabAB<SampleType::U8>(a, 255); p[2] = storedLabAB<SampleType::U8>(b, 255); p[3] = 255;
                }
            return img;
        };
        doc.layers.push_back(layerOf("back", ImagePtr(solid(30, 20, -10))));
        Layer top = layerOf("top", ImagePtr(solid(80, -40, 25)));
        top.blendMode = BlendMode::Luminosity;
        doc.layers.push_back(top);
        if (type == SampleType::U16) { std::string e; REQUIRE(convertSampleType(doc, SampleType::U16, &e)); }
        auto read = [&](const Document& d, double out[3]) {
            const AnyImage n = renderNative(d);
            if (n.u8()) { const uint8_t* p = n.u8()->pixel(1, 1); out[0] = labL<SampleType::U8>(p[0], p[3]); out[1] = labA<SampleType::U8>(p[1], p[3]); out[2] = labB<SampleType::U8>(p[2], p[3]); }
            else { const uint16_t* p = n.u16()->pixel(1, 1); out[0] = labL<SampleType::U16>(p[0], p[3]); out[1] = labA<SampleType::U16>(p[1], p[3]); out[2] = labB<SampleType::U16>(p[2], p[3]); }
        };
        double v[3];
        read(doc, v);
        CHECK_NEAR(v[0], 80, 0.6); CHECK_NEAR(v[1], 20, 1.01); CHECK_NEAR(v[2], -10, 1.01);
        doc.layers[1].blendMode = BlendMode::Color;
        read(doc, v);
        CHECK_NEAR(v[0], 30, 0.6); CHECK_NEAR(v[1], -40, 1.01); CHECK_NEAR(v[2], 25, 1.01);
    }
}

TEST_CASE(sixteen_bit_cmyk_renders_as_eight_bit_within_a_level) {
    Document eight = inMode(rgbDocument(40, 30, 3, BlendMode::Screen), ColorMode::CMYK);
    eight.layers[2].blendMode = BlendMode::Overlay;
    eight.layers[2].opacity = 0.7;
    Document sixteen = eight;
    std::string e;
    REQUIRE(convertSampleType(sixteen, SampleType::U16, &e));
    REQUIRE(sixteen.layers[0].asset->image.u16() && sixteen.layers[0].asset->image.u16()->channels() == 5);
    const AnyImage r8 = renderNative(eight), r16 = renderNative(sixteen);
    const ImageC8& a = *r8.c8();
    const Image16& b = *r16.u16();
    REQUIRE(b.channels() == 5);
    int worst = 0;
    for (int y = 0; y < a.height(); y++)
        for (int i = 0; i < a.width() * 5; i++) worst = std::max(worst, std::abs(int(a.row(y)[i]) - int(narrow16(b.row(y)[i]))));
    CHECK(worst <= 1);
    // The display of each is its native render through the profile (Little CMS's 8-bit and float pipelines differ by
    // several levels on random inks past the profile's total ink limit, which is printed).
    Image d8, d16;
    render(eight, RenderOptions(), d8);
    render(sixteen, RenderOptions(), d16);
    std::printf("  CMYK display, 8 against 16 bits: at most %d levels\n", maxDifference(d8, d16));
    Image expected16(d16.width(), d16.height());
    auto t16 = transformBetween(sixteen.profile, srgbProfile(), ConvertOptions(), PixelFormat::CMYKA16, PixelFormat::RGBA8);
    REQUIRE(t16 && convertImageTo8(r16, expected16, *t16));
    CHECK_EQ(maxDifference(d16, expected16), 0);
}

TEST_CASE(masks_folders_and_clipping_in_cmyk) {
    Document doc = inMode(rgbDocument(32, 24, 2, BlendMode::Normal), ColorMode::CMYK);
    const auto withoutTop = [&] { Document d = doc; d.layers.pop_back(); return *renderNative(d).c8(); }();
    // A mask of zeros hides the layer.
    Document masked = doc;
    LayerMask m;
    m.asset = MaskAsset::make(std::make_shared<GrayImage>(32, 24, 0));
    masked.layers[1].mask = m;
    CHECK(*renderNative(masked).c8() == withoutTop);
    // A hidden folder hides its children; a visible isolated one at full opacity draws them as they are.
    Document grouped = doc;
    Layer group("Folder", grouped.size());
    group.isGroup = true;
    group.passThrough = false;
    grouped.layers[1].parentId = group.id;
    grouped.layers.insert(grouped.layers.begin() + 1, group);
    CHECK(*renderNative(grouped).c8() == *renderNative(doc).c8());
    // A layer clipped to a transparent base draws nothing.
    Document clipped = doc;
    Layer empty = layerOf("empty", ImageC8Ptr(std::make_shared<ImageC8>(32, 24, 5)));
    clipped.layers.insert(clipped.layers.begin() + 1, empty);
    clipped.layers[2].maskSourceId = clipped.layers[1].id;
    CHECK(*renderNative(clipped).c8() == withoutTop);
}

TEST_CASE(cmyk_adjustment_layers_drawn_and_not_yet_drawn) {
    Document doc = inMode(rgbDocument(20, 16, 1, BlendMode::Normal), ColorMode::CMYK);
    const ImageC8 before = *renderNative(doc).c8();
    Layer invert("Invert", doc.size());
    invert.adjustment = AdjustmentSettings::defaults(AdjustmentKind::Invert).toLayerAdjustment();
    invert.asset.reset();
    doc.layers.push_back(invert);
    const ImageC8 inverted = *renderNative(doc).c8();
    for (int i = 0; i < 4; i++) CHECK_EQ(int(inverted.pixel(3, 3)[i]), 255 - int(before.pixel(3, 3)[i]));
    // A kind the CMYK renderer does not draw yet leaves the pixels as they are.
    doc.layers.back().adjustment = AdjustmentSettings::defaults(AdjustmentKind::Vibrance).toLayerAdjustment();
    CHECK(*renderNative(doc).c8() == before);
}

TEST_CASE(mode_conversion_round_trips_and_keeps_dormant_adjustments) {
    Document doc = rgbDocument(24, 18, 2, BlendMode::Multiply);
    Layer vibrance("Vibrance", doc.size());
    vibrance.asset.reset();
    vibrance.adjustment = AdjustmentSettings::defaults(AdjustmentKind::Vibrance).toLayerAdjustment();
    doc.layers.push_back(vibrance);
    AdjustmentSettings curves = AdjustmentSettings::defaults(AdjustmentKind::Curves);
    curves.curves.channels[2] = {{0, 0}, {128, 200}, {255, 255}};
    Layer curvesLayer("Curves", doc.size());
    curvesLayer.asset.reset();
    curvesLayer.adjustment = curves.toLayerAdjustment();
    doc.layers.push_back(curvesLayer);
    doc.layers[1].text = LayerText();
    doc.layers[1].text->red = 0; doc.layers[1].text->green = 1; doc.layers[1].text->blue = 0;   // pure green: outside CMYK

    Document cmyk = inMode(doc, ColorMode::CMYK);
    CHECK(!cmyk.layers[2].visible);
    CHECK(isDormantAdjustment(cmyk.layers[2]));
    CHECK(cmyk.layers[3].visible);
    AdjustmentSettings reset;
    REQUIRE(AdjustmentSettings::parse(cmyk.layers[3].adjustment->json, reset));
    CHECK(reset.curves.channels[2] == (std::vector<CurvePoint>{{0, 0}, {255, 255}}));
    CHECK(cmyk.layers[1].text->green < 1.0 && cmyk.layers[1].text->red > 0.0);   // brought into the press gamut
    CHECK(cmyk.layers[0].asset->thumbnail && cmyk.layers[0].asset->thumbnail->width() == 24);

    Document back = inMode(cmyk, ColorMode::RGB);
    CHECK(back.layers[2].visible);
    CHECK(!isDormantAdjustment(back.layers[2]));
    CHECK(back.layers[0].asset->image.u8());

    // RGB to Lab and back (the loss is measured in rgb_lab_round_trip_loss_by_depth).
    Document lab = inMode(rgbDocument(24, 18, 1, BlendMode::Normal), ColorMode::Lab);
    CHECK(lab.profile.icc.empty());
    Document rgbAgain = inMode(lab, ColorMode::RGB);
    CHECK(rgbAgain.layers[0].asset->image.u8());
}

TEST_CASE(mode_conversion_refuses_32_bit_and_over_budget) {
    Document doc(8, 8);
    doc.sampleType = SampleType::F32;
    std::string why;
    CHECK(!convertDocumentMode(doc, ColorMode::CMYK, ColorProfile(), ConvertOptions(), &why));
    CHECK(!why.empty());
    CHECK(doc.colorMode == ColorMode::RGB);
}

TEST_CASE(stored_colours_follow_the_mode) {
    double green[3] = {0, 1, 0};
    convertModeColor(ColorMode::RGB, ColorProfile(), ColorMode::CMYK, ColorProfile(), ConvertOptions(), green);
    CHECK(green[1] < 1.0);
    double grey[3] = {0.5, 0.5, 0.5};
    convertModeColor(ColorMode::RGB, ColorProfile(), ColorMode::Lab, ColorProfile(), ConvertOptions(), grey);
    CHECK_NEAR(grey[0], 0.5, 0.01);
}

TEST_CASE(five_channel_mips_and_thumbnails) {
    auto c8 = std::make_shared<ImageC8>(9, 7, 5);
    for (int y = 0; y < 7; y++) for (int x = 0; x < 9; x++) for (int c = 0; c < 5; c++) c8->pixel(x, y)[c] = uint8_t(c == 4 ? 255 : (x * 20 + c * 30) % 256);
    const auto half = halveImage(*c8);
    CHECK_EQ(half->channels(), 5);
    CHECK_EQ(half->width(), 5);
    CHECK_EQ(int(half->pixel(0, 0)[4]), 255);
    const ImageC8Ptr shared = c8;
    const auto level = MipCache::shared().level(shared, 1);
    REQUIRE(level);
    CHECK(*level == *half);
    auto w16 = std::make_shared<Image16>(9, 7, 5);
    const auto half16 = halveImage(*w16);
    CHECK_EQ(half16->channels(), 5);
    const Asset asset = Asset::make(ImageC8Ptr(c8), "cmyk");
    REQUIRE(asset.thumbnail);
    CHECK_EQ(asset.thumbnail->width(), 9);
}

TEST_CASE(rgb_lab_round_trip_loss_by_depth) {
    // 16 bits: exact for 8-bit sources. 8 bits: 8-bit Lab's quantisation (one step of a or b) moves a saturated colour
    // near R = 0 by tens of encoded levels while its colour difference stays near one; the mean stays under a level.
    const auto src = noisy(24, 18, 11, false);
    for (SampleType type : {SampleType::U8, SampleType::U16}) {
        const AnyImage in = imageAtDepth(ImagePtr(src), type);
        const AnyImage lab = convertImage(in, ColorMode::RGB, ColorProfile(), ColorMode::Lab, ColorProfile());
        const AnyImage back = imageAtDepth(convertImage(lab, ColorMode::Lab, ColorProfile(), ColorMode::RGB, ColorProfile()), SampleType::U8);
        REQUIRE(back.u8());
        int worst = 0;
        double sum = 0;
        for (int y = 0; y < 18; y++) for (int x = 0; x < 24; x++) for (int c = 0; c < 3; c++) {
            const int d = std::abs(int(back.u8()->pixel(x, y)[c]) - int(src->pixel(x, y)[c]));
            worst = std::max(worst, d);
            sum += d;
        }
        const double mean = sum / (24 * 18 * 3);
        std::printf("  RGB > Lab > RGB at %s bits: worst %d, mean %.2f levels\n", sampleTypeName(type), worst, mean);
        if (type == SampleType::U16) CHECK(worst <= 1);
        else CHECK(mean < 1.5 && worst <= 32);
    }
}

TEST_MAIN()
