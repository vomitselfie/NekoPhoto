// 32-bit documents (docs/bit-depth.md, "32 bits"): the float primitives, linear profiles, the blend modes and the
// renderer against a double-precision reference (float_reference.h), the view's tone mapping, and the mode
// conversion's round trips: 8 and 16 bits to 32 and back give every sample exactly, and opaque Normal stacks render
// to the same 8-bit pixels.
#include "check.h"
#include "float_reference.h"
#include "compositor/blend.h"
#include "compositor/colormgmt.h"
#include "compositor/depth.h"
#include "compositor/layerstyle.h"
#include "compositor/render.h"
#include "compositor/view32.h"
#include <cmath>
#include <limits>
#include <cstring>
#include <random>

using namespace compositor;

namespace {

std::shared_ptr<Image> randomImage(std::mt19937& rng, int w, int h, bool opaque) {
    auto image = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t* p = image->pixel(x, y);
            const uint8_t a = opaque ? 255 : uint8_t(rng() % 256);
            for (int c = 0; c < 3; c++) p[c] = a ? uint8_t(rng() % (a + 1u)) : 0;
            p[3] = a;
        }
    return image;
}

std::shared_ptr<GrayImage> randomGray(std::mt19937& rng, int w, int h) {
    auto g = std::make_shared<GrayImage>(w, h);
    for (int i = 0; i < w * h; i++) g->data()[i] = uint8_t(rng() % 256);
    return g;
}

/// Linear float with colour up to `range` (above 1 for HDR), premultiplied.
std::shared_ptr<ImageF> randomFloat(std::mt19937& rng, int w, int h, float range, bool opaque = false) {
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    auto image = std::make_shared<ImageF>(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float* p = image->pixel(x, y);
            const float a = opaque ? 1.0f : unit(rng) < 0.1f ? 0.0f : unit(rng);
            for (int c = 0; c < 3; c++) p[c] = unit(rng) * range * a;
            p[3] = a;
        }
    return image;
}

Document eightBitDocument(std::mt19937& rng, bool opaque) {
    Document doc(40, 30);
    doc.layers.push_back(Layer(Asset::make(randomImage(rng, 40, 30, true), "base"), Point(0, 0)));
    Layer top(Asset::make(randomImage(rng, 24, 18, opaque), "top"), Point(9, 5));
    LayerMask mask;
    mask.asset = MaskAsset::make(randomGray(rng, 24, 18));
    top.mask = mask;
    top.opacity = 0.7;
    top.blendMode = BlendMode::Multiply;
    doc.layers.push_back(top);
    return doc;
}

std::array<double, 3> weightsOf(const Document& doc) {
    const auto w = luminanceWeights(doc.profile);
    return {w[0], w[1], w[2]};
}

} // namespace

TEST_CASE(linear_profiles_keep_primaries_and_come_back) {
    for (WorkingSpace s : {WorkingSpace::SRGB, WorkingSpace::AdobeRGB, WorkingSpace::DisplayP3, WorkingSpace::ProPhoto}) {
        const ColorProfile& gamma = builtinProfile(s);
        const ColorProfile linear = linearProfile(gamma);
        REQUIRE(!linear.empty());
        CHECK(isLinearProfile(linear));
        CHECK(!isLinearProfile(gamma));
        CHECK(linear.description == std::string(workingSpaceName(s)) + " (Linear)");
        // The same bytes every time, and the way back is the working space itself.
        CHECK(linearProfile(gamma).icc == linear.icc);
        CHECK(gammaCounterpart(linear).icc == gamma.icc);
        CHECK(linearProfile(linear).icc == linear.icc);
        // The curve is the identity.
        const TransferCurve curve = TransferCurve::ofProfile(linear);
        for (float v : {0.0f, 0.01f, 0.2f, 0.5f, 0.9f, 1.0f}) CHECK_NEAR(curve.toLinear(v), v, 2e-4);
    }
    // Untagged is sRGB's.
    CHECK(linearProfile(ColorProfile{}).icc == linearProfile(srgbProfile()).icc);
    // Luminance weights: Y of the primaries, summing to 1.
    const auto w = luminanceWeights(linearProfile(srgbProfile()));
    CHECK_NEAR(w[0] + w[1] + w[2], 1.0, 1e-6);
    CHECK_NEAR(w[1], 0.7169, 2e-3);   // sRGB's green Y, D50-adapted
}

TEST_CASE(transfer_curves_invert_exactly) {
    for (WorkingSpace s : {WorkingSpace::SRGB, WorkingSpace::AdobeRGB, WorkingSpace::ProPhoto}) {
        const TransferCurve curve = TransferCurve::ofProfile(builtinProfile(s));
        for (int i = 0; i <= 32768; i += 7) {
            const float v = float(i) / 32768.0f;
            CHECK_NEAR(curve.fromLinearExact(curve.toLinear(v)), v, 1e-5);
        }
    }
}

TEST_CASE(eight_and_sixteen_bit_documents_round_trip_through_32_bits_exactly) {
    std::mt19937 rng(32);
    for (WorkingSpace s : {WorkingSpace::SRGB, WorkingSpace::AdobeRGB, WorkingSpace::DisplayP3, WorkingSpace::ProPhoto}) {
        for (bool tagged : {false, true}) {
            Document doc = eightBitDocument(rng, false);
            if (tagged) doc.profile = builtinProfile(s);
            const Document original = doc;
            std::string error;
            REQUIRE(convertSampleType(doc, SampleType::F32, &error));
            CHECK(doc.sampleType == SampleType::F32);
            REQUIRE(doc.layers[1].asset->image.f32() != nullptr);
            REQUIRE(doc.layers[1].mask->asset.image.f32() != nullptr);
            CHECK(isLinearProfile(doc.profile));
            REQUIRE(doc.encodedProfile.has_value());
            CHECK(doc.encodedProfile->icc == original.profile.icc);
            REQUIRE(convertSampleType(doc, SampleType::U8, &error));
            CHECK(doc.profile.icc == original.profile.icc);
            CHECK(!doc.encodedProfile.has_value());
            for (size_t i = 0; i < doc.layers.size(); i++) CHECK(*doc.layers[i].asset->image.u8() == *original.layers[i].asset->image.u8());
            CHECK(*doc.layers[1].mask->asset.image.u8() == *original.layers[1].mask->asset.image.u8());
            // 16 bits the same way: every 15-bit level comes back.
            Document deep = original;
            REQUIRE(convertSampleType(deep, SampleType::U16, &error));
            const Document deepOriginal = deep;
            REQUIRE(convertSampleType(deep, SampleType::F32, &error));
            REQUIRE(convertSampleType(deep, SampleType::U16, &error));
            for (size_t i = 0; i < deep.layers.size(); i++) CHECK(*deep.layers[i].asset->image.u16() == *deepOriginal.layers[i].asset->image.u16());
            CHECK(*deep.layers[1].mask->asset.image.u16() == *deepOriginal.layers[1].mask->asset.image.u16());
        }
    }
}

TEST_CASE(every_sixteen_bit_level_round_trips) {
    // Opaque and at a few alphas, every 15-bit colour level.
    for (uint16_t alpha : {uint16_t(32768), uint16_t(20000), uint16_t(517), uint16_t(3)}) {
        auto image = std::make_shared<Image16>(16385, 2);   // a side is at most 30,000 pixels
        for (int x = 0; x <= 32768; x++) {
            uint16_t* p = image->pixel(x % 16385, x / 16385);
            const uint16_t v = uint16_t(std::min<int>(x, alpha));
            p[0] = v; p[1] = uint16_t(alpha - v); p[2] = uint16_t(v / 2); p[3] = alpha;
        }
        for (WorkingSpace s : {WorkingSpace::SRGB, WorkingSpace::AdobeRGB, WorkingSpace::ProPhoto}) {
            const TransferCurve curve = TransferCurve::ofProfile(builtinProfile(s));
            auto back = encodeImage16(*lineariseImage(*image, curve), curve);
            CHECK(*back == *image);
        }
    }
}

TEST_CASE(opaque_normal_stacks_render_the_same_eight_bit_pixels) {
    std::mt19937 rng(7);
    Document doc(48, 32);
    doc.layers.push_back(Layer(Asset::make(randomImage(rng, 48, 32, true), "a"), Point(0, 0)));
    doc.layers.push_back(Layer(Asset::make(randomImage(rng, 20, 12, true), "b"), Point(3, 4)));
    doc.layers.push_back(Layer(Asset::make(randomImage(rng, 30, 10, true), "c"), Point(15, 20)));
    const auto eight = renderFlattened(doc);
    Document f = doc;
    REQUIRE(convertSampleType(f, SampleType::F32));
    // The canvas's frame (exposure 0) and the float render encoded exactly: the 8-bit pixels.
    const auto canvas = renderFlattened(f);
    CHECK(*canvas == *eight);
    const auto encoded = encodeImage8(*renderFlattenedF(f), encodedTransfer(f));
    CHECK(*encoded == *eight);
}

TEST_CASE(blend_modes_match_the_double_reference) {
    std::mt19937 rng(2718);
    for (int m = 0; m < blendModeCount; m++) {
        const BlendMode mode = BlendMode(m);
        if (mode == BlendMode::Dissolve) continue;
        Document doc(32, 24);
        doc.sampleType = SampleType::F32;
        doc.profile = linearProfile(srgbProfile());
        doc.layers.push_back(Layer(Asset::make(ImageFPtr(randomFloat(rng, 32, 24, 4.0f, true)), "base"), Point(0, 0)));
        Layer mid(Asset::make(ImageFPtr(randomFloat(rng, 20, 16, 1.0f)), "mid"), Point(2, 3));
        mid.blendMode = BlendMode::Screen;
        mid.opacity = 0.5;
        doc.layers.push_back(mid);
        Layer top(Asset::make(ImageFPtr(randomFloat(rng, 26, 18, 3.0f)), "top"), Point(5, 2));
        top.blendMode = mode;
        top.opacity = 0.8;
        auto mask = std::make_shared<GrayF>(26, 18);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        for (int i = 0; i < 26 * 18; i++) mask->data()[i] = unit(rng);
        LayerMask lm;
        lm.asset = MaskAsset::make(GrayFPtr(mask));
        top.mask = lm;
        doc.layers.push_back(top);
        // An isolated folder in the same mode, over the rest.
        Layer folder("folder", doc.size());
        folder.isGroup = true;
        folder.passThrough = false;
        folder.blendMode = mode;
        folder.opacity = 0.6;
        Layer inside(Asset::make(ImageFPtr(randomFloat(rng, 14, 10, 2.0f)), "inside"), Point(12, 9));
        inside.parentId = folder.id;
        doc.layers.push_back(inside);
        doc.layers.push_back(folder);
        const auto reference = float_reference::render(doc, weightsOf(doc));
        REQUIRE(!reference.pixels.empty());
        const double error = float_reference::worstError(*renderFlattenedF(doc), reference);
        if (!(error <= 1)) std::fprintf(stderr, "  %s: %g\n", blendModeName(mode), error);
        CHECK(error <= 1);
    }
}

TEST_CASE(photoshops_32_bit_modes_keep_values_above_one) {
    const float luma[3] = {0.2126f, 0.7152f, 0.0722f};
    float dst[4] = {3.0f, 0.5f, 2.0f, 1.0f};
    const float src[4] = {2.0f, 2.0f, 0.25f, 1.0f};
    compositePixelF(BlendMode::LinearDodge, src, 1.0f, dst, luma);
    CHECK_NEAR(dst[0], 5.0, 1e-6);
    CHECK_NEAR(dst[1], 2.5, 1e-6);
    float dst2[4] = {3.0f, 0.5f, 2.0f, 1.0f};
    compositePixelF(BlendMode::Screen, src, 1.0f, dst2, luma);   // not in the 32-bit set: clamped inside B
    CHECK_NEAR(dst2[0], 1.0, 1e-6);
    CHECK(blendModeAt32(BlendMode::Multiply));
    CHECK(!blendModeAt32(BlendMode::Overlay));
}

TEST_CASE(view_tone_mapping) {
    const float luma[3] = {0.2126f, 0.7152f, 0.0722f};
    View32 view;
    CHECK(ToneMap::of(view, luma).identity);
    view.exposure = 1;
    float c[3] = {0.25f, 0.5f, 1.0f};
    ToneMap::of(view, luma).apply(c);
    CHECK_NEAR(c[0], 0.5, 1e-6);
    CHECK_NEAR(c[2], 2.0, 1e-6);
    view = View32();
    view.gamma = 2;
    float g[3] = {0.25f, 0.25f, 0.25f};
    ToneMap::of(view, luma).apply(g);
    CHECK_NEAR(g[0], 0.5, 1e-6);
    // Highlight Compression: the peak lands on 1, an image within 1 is left alone.
    view = View32();
    view.method = ToneMethod::HighlightCompression;
    CHECK(ToneMap::of(view, luma, 0.9f).identity);
    float h[3] = {8.0f, 8.0f, 8.0f};
    ToneMap::of(view, luma, 8.0f).apply(h);
    CHECK_NEAR(h[1], 1.0, 1e-5);
}

TEST_CASE(display_applies_exposure) {
    Document doc(4, 1);
    doc.sampleType = SampleType::F32;
    doc.profile = linearProfile(srgbProfile());
    auto image = std::make_shared<ImageF>(4, 1);
    for (int x = 0; x < 4; x++) { float* p = image->pixel(x, 0); p[0] = p[1] = p[2] = 0.25f * float(x + 1); p[3] = 1; }
    doc.layers.push_back(Layer(Asset::make(ImageFPtr(image), "l"), Point(0, 0)));
    Image out;
    RenderOptions o;
    render(doc, o, out);
    const TransferCurve srgb = TransferCurve::srgb();
    CHECK_EQ(int(out.pixel(1, 0)[0]), int(std::lround(srgb.fromLinear(0.5f) * 255)));
    o.view32.exposure = 1;
    render(doc, o, out);
    CHECK_EQ(int(out.pixel(1, 0)[0]), 255);
    CHECK_EQ(int(out.pixel(0, 0)[0]), int(std::lround(srgb.fromLinear(0.5f) * 255)));
    o.view32.exposure = -1;
    render(doc, o, out);
    CHECK_EQ(int(out.pixel(3, 0)[0]), int(std::lround(srgb.fromLinear(0.5f) * 255)));
}

TEST_CASE(hdr_toning_on_the_way_down) {
    Document doc(2, 1);
    doc.sampleType = SampleType::F32;
    doc.profile = linearProfile(srgbProfile());
    auto image = std::make_shared<ImageF>(2, 1);
    float* p = image->pixel(0, 0); p[0] = p[1] = p[2] = 0.5f; p[3] = 1;
    p = image->pixel(1, 0); p[0] = p[1] = p[2] = 4.0f; p[3] = 1;
    doc.layers.push_back(Layer(Asset::make(ImageFPtr(image), "l"), Point(0, 0)));
    const TransferCurve srgb = TransferCurve::srgb();
    Document plain = doc;
    REQUIRE(convertSampleType(plain, SampleType::U8));
    CHECK_EQ(int(plain.layers[0].asset->image.u8()->pixel(1, 0)[0]), 255);   // clipped
    Document darker = doc;
    View32 view;
    view.exposure = -3;
    REQUIRE(convertSampleType(darker, SampleType::U8, nullptr, &view));
    CHECK_EQ(int(darker.layers[0].asset->image.u8()->pixel(1, 0)[0]), int(std::lround(srgb.fromLinear(0.5f) * 255)));
    Document compressed = doc;
    view = View32();
    view.method = ToneMethod::HighlightCompression;
    REQUIRE(convertSampleType(compressed, SampleType::U16, nullptr, &view));
    CHECK_EQ(int(compressed.layers[0].asset->image.u16()->pixel(1, 0)[0]), 32768);   // the peak lands on white
    CHECK(compressed.layers[0].asset->image.u16()->pixel(0, 0)[0] < 32768 * 3 / 4);
}

TEST_CASE(float_values_are_cleaned) {
    ImageF image(3, 1);
    float* p = image.data();
    p[0] = std::numeric_limits<float>::quiet_NaN(); p[1] = std::numeric_limits<float>::infinity(); p[2] = -2; p[3] = 1.5f;
    p[4] = 0.5f; p[5] = 0.5f; p[6] = 0.5f; p[7] = std::numeric_limits<float>::quiet_NaN();
    CHECK_EQ(int(cleanFloat(image)), 5);
    CHECK(p[0] == 0.0f);
    CHECK(p[1] == 65504.0f);
    CHECK(p[2] == 0.0f);
    CHECK(p[3] == 1.0f);
    CHECK(p[7] == 0.0f);
}

TEST_CASE(float_predictor_round_trips) {
    std::mt19937 rng(9);
    std::uniform_real_distribution<float> d(-100.0f, 100.0f);
    std::vector<float> row(37), back(37);
    for (auto& v : row) v = d(rng);
    row[3] = std::numeric_limits<float>::infinity();
    std::vector<uint8_t> bytes(37 * 4);
    predictFloatRow(row.data(), 37, bytes.data());
    unpredictFloatRow(bytes.data(), 37, back.data());
    CHECK(std::memcmp(row.data(), back.data(), row.size() * 4) == 0);
}

TEST_CASE(primitives_in_float) {
    std::mt19937 rng(3);
    auto image = randomFloat(rng, 9, 7, 2.0f);
    auto halved = halveImage(*image);
    CHECK_EQ(halved->width(), 5);
    const float* a = image->pixel(0, 0);
    const float* b = image->pixel(1, 0);
    const float* c = image->pixel(0, 1);
    const float* d = image->pixel(1, 1);
    CHECK_NEAR(halved->pixel(0, 0)[0], (a[0] + b[0] + c[0] + d[0]) / 4, 1e-6);
    float s[4];
    sampleBilinear(*image, 1.5, 0.5, s);
    CHECK_NEAR(s[0], image->pixel(1, 0)[0], 1e-6);
    auto crop = cropImage(*image, 2, 1, 3, 3);
    CHECK(std::memcmp(crop->pixel(0, 0), image->pixel(2, 1), 16) == 0);
    CHECK(contentHash(image.get()) != 0);
    CHECK(contentHash(image.get()) == contentHash(image.get()));
    auto thumb = makeThumbnail(*image, 4);
    CHECK(thumb->width() <= 4);
    GrayF gray(4, 4, 0.0f);
    gray.at(2, 1) = 0.5f;
    const PixelBounds bounds = nonzeroBounds(gray);
    CHECK_EQ(bounds.x0, 2);
    CHECK_EQ(bounds.y1, 2);
    // MipCache keeps float levels.
    ImageFPtr shared = image;
    CHECK_EQ(MipCache::shared().level(shared, 1)->width(), 5);
}

TEST_CASE(layer_styles_draw_at_32_bits) {
    std::mt19937 rng(11);
    Document doc = eightBitDocument(rng, true);
    doc.layers[1].blendMode = BlendMode::Normal;
    doc.layers[1].opacity = 1;
    doc.layers[1].mask.reset();
    LayerStyle styleValue;
    LayerStyle* style = &styleValue;
    DropShadow shadow;
    shadow.opacity = 1;
    shadow.distance = 3;
    shadow.size = 2;
    style->dropShadows.push_back(shadow);
    ColorOverlay overlay;
    overlay.color = {255, 128, 0};
    overlay.opacity = 1;
    style->colorOverlays.push_back(overlay);
    setLayerStyle(doc.layers[1], styleValue);
    const auto eight = renderFlattened(doc);
    Document f = doc;
    REQUIRE(convertSampleType(f, SampleType::F32));
    const auto canvas = renderFlattened(f);
    // The overlay's colour comes out as it went in (opaque, a Normal overlay).
    const uint8_t* p = canvas->pixel(15, 10);
    CHECK_EQ(int(p[0]), 255);
    CHECK(std::abs(int(p[1]) - 128) <= 1);
    CHECK_EQ(int(p[2]), 0);
    const uint8_t* q = eight->pixel(15, 10);
    CHECK_EQ(int(q[0]), 255);
}

TEST_MAIN()
