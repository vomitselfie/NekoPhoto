// Editing 32-bit documents (docs/bit-depth.md, "32 bits"; P5b): each kernel against its double-precision version in
// float_reference.h within 1e-5 absolute plus relative, on linear colour up to 4 with partial alpha; the extension of
// Levels and Curves above 1; adjustment layers in the renderer; and the cross-depth check, an adjustment applied at 32
// bits to an 8-bit-sourced document and converted back at exposure 0 against the 8-bit result.
#include "check.h"
#include "float_reference.h"
#include "compositor/adjustments.h"
#include "compositor/blur.h"
#include "compositor/channels.h"
#include "compositor/colormgmt.h"
#include "compositor/depth.h"
#include "compositor/filters.h"
#include "compositor/morphology.h"
#include "compositor/render.h"
#include "compositor/resample.h"
#include "compositor/selection.h"
#include "compositor/warp.h"
#include <cmath>
#include <cstdio>
#include <random>

using namespace compositor;
namespace ref = float_reference;

namespace {

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

ref::Encoding encodingOf(const TransferCurve& curve) {
    if (curve.kind() == TransferCurve::Kind::SRGB) return {};
    return {false, curve.gamma()};
}

/// The 32-bit set with settings that move every part of each function.
std::vector<AdjustmentSettings> samples() {
    std::vector<AdjustmentSettings> out;
    AdjustmentSettings levels = AdjustmentSettings::defaults(AdjustmentKind::Levels);
    levels.levels.ranges[0] = {15, 1.3, 230, 10, 245};
    levels.levels.ranges[2] = {0, 0.8, 200, 0, 255};
    out.push_back(levels);
    AdjustmentSettings curves = AdjustmentSettings::defaults(AdjustmentKind::Curves);
    curves.curves.channels[0] = {{0, 0}, {64, 50}, {192, 210}, {255, 255}};
    curves.curves.channels[3] = {{0, 20}, {128, 150}, {255, 240}};
    out.push_back(curves);
    AdjustmentSettings exposure = AdjustmentSettings::defaults(AdjustmentKind::Exposure);
    exposure.exposure = {1.3, 0.02, 1.2};
    out.push_back(exposure);
    AdjustmentSettings hsv = AdjustmentSettings::defaults(AdjustmentKind::HueSaturation);
    hsv.hsv.adjustments[0] = {20, 25, -5};
    hsv.hsv.adjustments[1] = {-15, 40, 10};
    out.push_back(hsv);
    AdjustmentSettings colorize = AdjustmentSettings::defaults(AdjustmentKind::HueSaturation);
    colorize.hsv.colorize = true;
    colorize.hsv.adjustments[0] = {200, 45, 10};
    out.push_back(colorize);
    AdjustmentSettings balance = AdjustmentSettings::defaults(AdjustmentKind::ColorBalance);
    balance.colorBalance.ranges = {{{20, -10, 5}, {-30, 15, 40}, {10, 0, -25}}};
    out.push_back(balance);
    AdjustmentSettings bw = AdjustmentSettings::defaults(AdjustmentKind::BlackWhite);
    bw.blackWhite.tint = true;
    out.push_back(bw);
    AdjustmentSettings photo = AdjustmentSettings::defaults(AdjustmentKind::PhotoFilter);
    photo.photoFilter.density = 60;
    out.push_back(photo);
    AdjustmentSettings mixer = AdjustmentSettings::defaults(AdjustmentKind::ChannelMixer);
    mixer.channelMixer.rows[0] = {80, 30, -10, 5};
    mixer.channelMixer.rows[2] = {10, 20, 90, -5};
    out.push_back(mixer);
    AdjustmentSettings vibrance = AdjustmentSettings::defaults(AdjustmentKind::Vibrance);
    vibrance.vibrance = {60, -20};
    out.push_back(vibrance);
    AdjustmentSettings map = AdjustmentSettings::defaults(AdjustmentKind::GradientMap);
    map.gradientMap.shadows = {0.1, 0.0, 0.3};
    map.gradientMap.highlights = {1.0, 0.9, 0.4};
    out.push_back(map);
    out.push_back(AdjustmentSettings::defaults(AdjustmentKind::Invert));
    return out;
}

/// Worst difference in 8-bit levels between two 8-bit images, and the share of samples beyond one level.
std::pair<int, double> levelsApart(const Image& a, const Image& b) {
    int worst = 0;
    size_t over = 0, count = 0;
    for (int y = 0; y < a.height(); y++)
        for (int x = 0; x < a.width(); x++)
            for (int c = 0; c < 4; c++) {
                const int d = std::abs(int(a.pixel(x, y)[c]) - int(b.pixel(x, y)[c]));
                worst = std::max(worst, d);
                over += d > 1;
                count++;
            }
    return {worst, double(over) / double(count)};
}

} // namespace

TEST_CASE(adjustments_match_the_double_reference) {
    std::mt19937 rng(31);
    const auto source = randomFloat(rng, 40, 30, 4.0f);
    for (const TransferCurve& curve : {TransferCurve::srgb(), TransferCurve::ofProfile(builtinProfile(WorkingSpace::AdobeRGB))}) {
        for (const AdjustmentSettings& settings : samples()) {
            ImageF image = *source;
            REQUIRE(applyAdjustment(settings, image, Rect(0, 0, 40, 30), 1, curve));
            ref::Canvas expected = ref::canvasOf(*source);
            REQUIRE(ref::adjust(expected, settings, encodingOf(curve)));
            const double error = ref::worstError(image, expected);
            if (!(error <= 1)) std::fprintf(stderr, "  %s (%s): %g\n", adjustmentKindName(settings.kind), curve.kind() == TransferCurve::Kind::SRGB ? "sRGB" : "gamma", error);
            CHECK(error <= 1);
        }
    }
}

TEST_CASE(kinds_photoshop_lacks_at_32_bits_are_refused) {
    for (AdjustmentKind kind : {AdjustmentKind::BrightnessContrast, AdjustmentKind::Posterize, AdjustmentKind::Threshold, AdjustmentKind::SelectiveColor, AdjustmentKind::Grain}) {
        CHECK(!adjustmentAt32(kind));
        ImageF image(4, 4);
        const float px[4] = {0.2f, 0.4f, 0.6f, 1.0f};
        image.fill(px);
        const ImageF before = image;
        CHECK(!applyAdjustment(AdjustmentSettings::defaults(kind), image, Rect(0, 0, 4, 4), 1, TransferCurve::srgb()));
        CHECK(image == before);
    }
    for (const AdjustmentSettings& s : samples()) CHECK(adjustmentAt32(s.kind));
}

TEST_CASE(levels_and_curves_go_on_above_one) {
    const TransferCurve srgb = TransferCurve::srgb();
    // Encoding continued above 1 is continuous and undoes itself.
    CHECK_NEAR(encodeExtended(srgb, 1.0f), 1.0f, 1e-6);
    CHECK(encodeExtended(srgb, 1.0001f) > 1.0f);
    for (float v : {0.001f, 0.2f, 0.9f, 1.5f, 4.0f, 100.0f}) CHECK_NEAR(decodeExtended(srgb, encodeExtended(srgb, v)), v, 1e-5 * v);
    auto one = [](float value) { auto i = std::make_shared<ImageF>(1, 1); float p[4] = {value, value, value, 1}; i->fill(p); return i; };
    // Levels: input white at 200, so the level at 200/255 encoded is white and above it the output goes on rising.
    AdjustmentSettings levels = AdjustmentSettings::defaults(AdjustmentKind::Levels);
    levels.levels.ranges[0] = {0, 1, 200, 0, 255};
    float previous = 0;
    for (float v : {0.3f, 0.6f, 1.0f, 2.0f, 8.0f}) {
        auto image = one(v);
        applyAdjustment(levels, *image, Rect(0, 0, 1, 1), 1, srgb);
        CHECK(image->pixel(0, 0)[0] > previous);
        previous = image->pixel(0, 0)[0];
    }
    CHECK(previous > 8.0f);
    // Curves: the last segment's tangent carries on above 255; a curve ending short of 255 stays flat.
    AdjustmentSettings curves = AdjustmentSettings::defaults(AdjustmentKind::Curves);
    curves.curves.channels[0] = {{0, 0}, {128, 100}, {255, 255}};
    auto below = one(0.999f), above = one(1.001f), far = one(4.0f);
    for (auto* i : {&below, &above, &far}) applyAdjustment(curves, **i, Rect(0, 0, 1, 1), 1, srgb);
    CHECK(above->pixel(0, 0)[0] > below->pixel(0, 0)[0]);
    CHECK(above->pixel(0, 0)[0] - below->pixel(0, 0)[0] < 0.01f);
    CHECK(far->pixel(0, 0)[0] > 4.0f);
    // Exposure is an exact multiply in linear light.
    AdjustmentSettings exposure = AdjustmentSettings::defaults(AdjustmentKind::Exposure);
    exposure.exposure.exposure = 2;
    auto bright = one(3.0f);
    applyAdjustment(exposure, *bright, Rect(0, 0, 1, 1), 1, srgb);
    CHECK_NEAR(bright->pixel(0, 0)[0], 12.0f, 1e-5);
}

TEST_CASE(colour_kinds_keep_light_above_one) {
    // A colour brighter than white keeps its level through Hue/Saturation (adjusted at white and scaled back).
    AdjustmentSettings hsv = AdjustmentSettings::defaults(AdjustmentKind::HueSaturation);
    hsv.hsv.adjustments[0] = {120, 0, 0};
    ImageF image(1, 1);
    const float px[4] = {6.0f, 1.0f, 1.0f, 1.0f};   // a bright red
    image.fill(px);
    applyAdjustment(hsv, image, Rect(0, 0, 1, 1), 1, TransferCurve::srgb());
    const float* p = image.pixel(0, 0);
    CHECK(p[1] > p[0]);                       // now green
    CHECK(std::max({p[0], p[1], p[2]}) > 5.0f);   // and as bright
}

TEST_CASE(adjustments_at_32_bits_match_8_bits_on_8_bit_pixels) {
    // An 8-bit-sourced layer adjusted at 32 bits and encoded back at exposure 0, against the 8-bit adjustment. Exposure
    // and Levels (inside 0..1) are within one level; the others are reported with their bounds (docs/bit-depth.md).
    std::mt19937 rng(5);
    const TransferCurve srgb = TransferCurve::srgb();
    AdjustmentSettings exposure = AdjustmentSettings::defaults(AdjustmentKind::Exposure);
    exposure.exposure = {0.7, 0.01, 1.1};
    std::vector<AdjustmentSettings> kinds = samples();
    kinds.push_back(exposure);
    // Levels within 0..1: no range clips at its white point with its output white below 255 (there the 8-bit Levels
    // cuts and 32 bits carries on, by design: levels_and_curves_go_on_above_one).
    kinds[0].levels.ranges[0] = {15, 1.3, 230, 10, 255};
    kinds[0].levels.ranges[2] = {0, 0.8, 255, 20, 240};
    for (bool opaque : {true, false})
    for (const AdjustmentSettings& settings : kinds) {
        const auto eight = randomImage(rng, 64, 48, opaque);
        Image at8 = *eight;
        applyAdjustment(settings, at8, Rect(0, 0, 64, 48), 1);
        auto deep = lineariseImage(*eight, srgb);
        applyAdjustment(settings, *deep, Rect(0, 0, 64, 48), 1, srgb);
        const auto back = encodeImage8(*deep, srgb);
        const auto [worst, over] = levelsApart(*back, at8);
        std::fprintf(stderr, "  %s%s: %d levels, %.3f%% beyond one\n", adjustmentKindName(settings.kind), opaque ? " (opaque)" : "", worst, over * 100);
        switch (opaque ? AdjustmentKind::Levels : settings.kind) {
        case AdjustmentKind::Exposure: case AdjustmentKind::Levels: case AdjustmentKind::Curves: case AdjustmentKind::Invert:
        case AdjustmentKind::BlackWhite: case AdjustmentKind::ChannelMixer: case AdjustmentKind::GradientMap:
            CHECK(worst <= 1);
            break;
        default:
            // Opaque, every kind is within a level. On half-transparent pixels the 8-bit colour kinds take the straight
            // colour as a whole level (p * 255 / a, cut) before their function: at a low alpha that is off by up to a
            // level divided by the alpha, which a saturating function (Color Balance, Photo Filter, Vibrance,
            // Hue/Saturation) moves by several levels on a few samples.
            CHECK(worst <= 6);
            CHECK(over < 0.002);
            break;
        }
    }
}

TEST_CASE(adjustment_layers_render_as_the_reference) {
    std::mt19937 rng(88);
    Document doc(36, 28);
    doc.sampleType = SampleType::F32;
    doc.profile = linearProfile(srgbProfile());
    doc.layers.push_back(Layer(Asset::make(ImageFPtr(randomFloat(rng, 36, 28, 3.0f, true)), "base"), Point(0, 0)));
    doc.layers.push_back(Layer(Asset::make(ImageFPtr(randomFloat(rng, 20, 14, 2.0f)), "mid"), Point(4, 6)));
    const std::vector<AdjustmentSettings> all = samples();
    int n = 0;
    for (const AdjustmentSettings& settings : all) {
        if (n++ % 3) continue;   // Levels, Colorize, Black & White, Vibrance: four layers
        Layer adjustment(adjustmentKindName(settings.kind), doc.size());
        adjustment.adjustment = settings.toLayerAdjustment();
        adjustment.opacity = 0.75;
        doc.layers.push_back(adjustment);
    }
    // One kind Photoshop lacks at 32 bits: kept, not drawn.
    Layer posterize("Posterize", doc.size());
    posterize.adjustment = AdjustmentSettings::defaults(AdjustmentKind::Posterize).toLayerAdjustment();
    doc.layers.push_back(posterize);
    const auto luma = luminanceWeights(doc.profile);
    const auto reference = ref::render(doc, {luma[0], luma[1], luma[2]});
    REQUIRE(!reference.pixels.empty());
    const double error = ref::worstError(*renderFlattenedF(doc), reference);
    if (!(error <= 1)) std::fprintf(stderr, "  adjustment layers: %g\n", error);
    CHECK(error <= 1);
}

TEST_CASE(filters_match_the_double_reference) {
    std::mt19937 rng(64);
    const auto source = randomFloat(rng, 45, 33, 4.0f);
    auto check = [&](const char* name, ImageF& image, const ref::Canvas& expected, double limit = 1) {
        const double error = ref::worstError(image, expected);
        if (!(error <= limit)) std::fprintf(stderr, "  %s: %g\n", name, error);
        CHECK(error <= limit);
    };
    for (double sigma : {0.6, 1.5, 4.0}) {
        ImageF image = *source;
        gaussianBlur(image, sigma);
        ref::Canvas expected = ref::canvasOf(*source);
        ref::gaussianBlur(expected, sigma);
        check("gaussian", image, expected);
    }
    {
        // Above sigma 6 the blur is Deriche's recursive fit, within 0.05% of the true kernel: against the exact FIR
        // within 2e-3 of the range (a scaled bound, 200 here in units of 1e-5).
        ImageF image = *source;
        gaussianBlur(image, 9.0);
        ref::Canvas expected = ref::canvasOf(*source);
        ref::gaussianBlur(expected, 9.0);
        check("gaussian deriche", image, expected, 200);
    }
    for (double angle : {0.0, 30.0, 75.0, -60.0, 90.0}) {
        ImageF image = *source;
        motionBlur(image, 9, angle);
        ref::Canvas expected = ref::canvasOf(*source);
        ref::motionBlur(expected, 9, angle);
        check("motion", image, expected);
    }
    for (const TransferCurve& curve : {TransferCurve::srgb(), TransferCurve::ofProfile(builtinProfile(WorkingSpace::ProPhoto))}) {
        for (int variant = 0; variant < 3; variant++) {
            FilterSettings s;
            s.amount = 25;
            s.gaussian = variant == 1;
            s.monochromatic = variant == 2;
            ImageF image = *source;
            applyFilter(FilterKind::AddNoise, image, s, curve, 1, 1234);
            ref::Canvas expected = ref::canvasOf(*source);
            ref::addNoise(expected, 25, s.gaussian, s.monochromatic, 1234, encodingOf(curve));
            check("noise", image, expected);
        }
    }
    for (double distortion : {-60.0, 45.0}) {
        for (bool bicubic : {false, true}) {
            FilterSettings s;
            s.distortion = distortion;
            s.bicubic = bicubic;
            ImageF image = *source;
            applyFilter(FilterKind::LensCorrection, image, s, TransferCurve::srgb());
            ref::Canvas expected = ref::canvasOf(*source);
            ref::lensCorrection(expected, distortion, bicubic);
            check("lens", image, expected);
        }
    }
}

TEST_CASE(resampling_matches_the_double_reference) {
    std::mt19937 rng(12);
    const auto source = randomFloat(rng, 40, 30, 4.0f);
    for (ResampleFilter filter : {ResampleFilter::Triangle, ResampleFilter::CatmullRom, ResampleFilter::Lanczos3}) {
        struct Case { int w, h; double ox, sx, oy, sy; };
        for (const Case& c : {Case{73, 51, 40.0 / 73 / 2, 40.0 / 73, 30.0 / 51 / 2, 30.0 / 51}, Case{17, 11, 40.0 / 17 / 2, 40.0 / 17, 30.0 / 11 / 2, 30.0 / 11},
                              Case{40, 30, 39.5, -1, 0.5, 1}}) {
            auto out = resampleAxisAligned(*source, c.w, c.h, c.ox, c.sx, c.oy, c.sy, filter);
            const auto expected = ref::resample(ref::canvasOf(*source), c.w, c.h, c.ox, c.sx, c.oy, c.sy, filter);
            const double error = ref::worstError(*out, expected);
            if (!(error <= 1)) std::fprintf(stderr, "  resample %d (%dx%d): %g\n", int(filter), c.w, c.h, error);
            CHECK(error <= 1);
        }
    }
}

TEST_CASE(warps_at_32_bits_encode_back_to_the_8_bit_warp) {
    // A perspective warp of an 8-bit-sourced layer at 32 bits, encoded back, against the 8-bit warp: the 8-bit
    // samplers blend encoded bytes with 8.8 weights, the float ones blend light, so a level or two at edges between
    // unlike colours, none on flat colour.
    std::mt19937 rng(3);
    auto eight = std::make_shared<Image>(32, 24);
    const uint8_t flat[4] = {180, 90, 40, 255};
    eight->fill(flat[0], flat[1], flat[2], flat[3]);
    const LayerTransform t(Point(10, 10), Size(32, 24));
    const Corners corners{Point(12, 8), Point(50, 14), Point(44, 40), Point(8, 36)};
    auto warped8 = warpImage(ImagePtr(eight), t, corners);
    const TransferCurve srgb = TransferCurve::srgb();
    auto warpedF = warpImage(ImageFPtr(lineariseImage(*eight, srgb)), t, corners);
    REQUIRE(warped8 && warpedF);
    const auto back = encodeImage8(*warpedF->image, srgb);
    // Interior pixels (full coverage) are the flat colour exactly.
    int interior = 0;
    for (int y = 0; y < back->height(); y++)
        for (int x = 0; x < back->width(); x++)
            if (warped8->image->pixel(x, y)[3] == 255 && back->pixel(x, y)[3] == 255) {
                interior++;
                for (int c = 0; c < 3; c++) CHECK(std::abs(int(back->pixel(x, y)[c]) - int(flat[c])) <= 1);
            }
    CHECK(interior > 400);
    (void)rng;
}


TEST_CASE(selections_are_float_coverage) {
    // Shapes combine in float at 32 bits; an 8-bit shape widens by scale.
    auto rect = rasterizeRect(Rect(4, 4, 20, 10), 40, 30, true);
    auto ellipse = rasterizeEllipse(Rect(10, 2, 24, 20), 40, 30, true);
    std::optional<Selection> s = combineSelection(std::nullopt, AnyGray(GrayPtr(rect)), SelectionMode::Replace, true, SampleType::F32);
    REQUIRE(s && s->coverage.f32());
    s = combineSelection(s, AnyGray(GrayPtr(ellipse)), SelectionMode::Add, true, SampleType::F32);
    REQUIRE(s && s->coverage.f32());
    for (int y = 0; y < 30; y++)
        for (int x = 0; x < 40; x++) CHECK_NEAR(s->coverage.f32()->at(x, y), std::max(rect->at(x, y), ellipse->at(x, y)) / 255.0f, 1e-6);
    const Selection inverse = invertSelection(*s, 40, 30);
    REQUIRE(inverse.coverage.f32());
    CHECK_NEAR(inverse.coverage.f32()->at(0, 0), 1.0f, 1e-6);
    const Selection moved = offsetSelection(*s, 3, -2);
    REQUIRE(moved.coverage.f32());
    CHECK_NEAR(moved.coverage.f32()->at(8, 3), s->coverage.f32()->at(5, 5), 1e-6);
    // Select > Modify on float coverage matches the 8-bit operations on the same shape within a level.
    auto eight = std::make_shared<GrayImage>(*rect);
    for (int amount : {3, -2}) {
        auto f = growSelection(*widenGrayF(*eight), amount);
        auto e = growSelection(*eight, amount);
        for (int i = 0; i < 40 * 30; i++) CHECK(std::abs(f->data()[i] * 255.0f - e->data()[i]) <= 1.0f);
    }
    auto borderF = borderSelection(*widenGrayF(*eight), 4);
    auto border8 = borderSelection(*eight, 4);
    for (int i = 0; i < 40 * 30; i++) CHECK(std::abs(borderF->data()[i] * 255.0f - border8->data()[i]) <= 1.0f);
    // Feather is the float Gaussian: against the reference blur.
    auto coverage = widenGrayF(*ellipse);
    auto feathered = featherSelection(*coverage, 2.5);
    ImageF asImage(40, 30);
    for (int y = 0; y < 30; y++) for (int x = 0; x < 40; x++) { float* p = asImage.pixel(x, y); p[0] = p[1] = p[2] = p[3] = coverage->at(x, y); }
    ref::Canvas expected = ref::canvasOf(asImage);
    ref::gaussianBlur(expected, 2.5);
    double worst = 0;
    for (int y = 0; y < 30; y++) for (int x = 0; x < 40; x++) worst = std::max(worst, std::fabs(feathered->at(x, y) - expected.at(x, y)[3]) / (1e-5 + 1e-5 * expected.at(x, y)[3]));
    CHECK(worst <= 1);
}

TEST_CASE(channels_and_layer_transparency_at_32_bits) {
    std::mt19937 rng(9);
    Document doc(40, 30);
    doc.sampleType = SampleType::F32;
    doc.profile = linearProfile(srgbProfile());
    auto pixels = randomFloat(rng, 20, 16, 2.0f);
    Layer layer(Asset::make(ImageFPtr(pixels), "layer"), Point(5, 6));
    doc.layers.push_back(layer);
    // A layer's transparency as a selection: its alpha, exactly, where it lies.
    auto cover = coverageFromLayerF(doc, doc.layers[0]);
    CHECK_NEAR(cover->at(5 + 3, 6 + 4), pixels->pixel(3, 4)[3], 1e-7);
    CHECK_NEAR(cover->at(0, 0), 0.0f, 1e-7);
    // Save Selection into an alpha channel and load it back, in float.
    Selection s;
    s.coverage = GrayFPtr(cover);
    doc.selection = s;
    Channel channel = makeAlphaChannel(doc, "Alpha 1");
    REQUIRE(channel.image.f32());
    saveSelectionInto(channel, doc.selection, SelectionMode::Replace, doc.sampleType, doc.width, doc.height);
    REQUIRE(channel.image.f32());
    doc.channels.push_back(channel);
    doc.selection.reset();
    SelectionSource source;
    source.kind = SelectionSource::AlphaChannel;
    source.id = channel.id;
    auto loaded = loadSelectionFrom(doc, source, false, SelectionMode::Replace, true);
    REQUIRE(loaded && loaded->coverage.f32());
    CHECK(*loaded->coverage.f32() == *cover);
    // Color Indicates Selected Areas inverts the stored gray and keeps the selection.
    setSelectedAreas(doc.channels[0], true);
    auto again = loadSelectionFrom(doc, source, false, SelectionMode::Replace, true);
    REQUIRE(again && again->coverage.f32());
    for (int i = 0; i < 40 * 30; i++) CHECK_NEAR(again->coverage.f32()->data()[i], cover->data()[i], 1e-6);
    // The composite as a selection decides on the encoded colour.
    source.kind = SelectionSource::Composite;
    auto composite = selectionSourceCoverage(doc, source);
    CHECK(bool(composite.f32()));
}

TEST_CASE(decision_image_is_the_exposure_0_encoding) {
    // The wand, Quick Select and Trim decide on decisionImage(): an 8-bit document converted to 32 bits gives its own
    // 8-bit composite back, whatever view the canvas has.
    std::mt19937 rng(21);
    Document doc(48, 32);
    doc.layers.push_back(Layer(Asset::make(randomImage(rng, 48, 32, true), "a"), Point(0, 0)));
    doc.layers.push_back(Layer(Asset::make(randomImage(rng, 20, 12, true), "b"), Point(3, 4)));
    const auto eight = renderFlattened(doc);
    Document f = doc;
    REQUIRE(convertSampleType(f, SampleType::F32));
    CHECK(*decisionImage(f) == *eight);
    CHECK(*decisionImage(doc) == *eight);
}

TEST_CASE(image_size_resamples_in_float) {
    // Image Size of an axis-aligned layer covering the canvas: the float separable resampler, against the reference.
    std::mt19937 rng(17);
    auto pixels = randomFloat(rng, 40, 30, 3.0f);
    for (auto [filter, sampling] : {std::pair{ResampleFilter::Lanczos3, Sampling::High}, std::pair{ResampleFilter::Triangle, Sampling::Smooth}}) {
        Document doc(40, 30);
        doc.sampleType = SampleType::F32;
        doc.profile = linearProfile(srgbProfile());
        doc.layers.push_back(Layer(Asset::make(ImageFPtr(pixels), "layer"), Point(0, 0)));
        REQUIRE(resizeDocument(doc, 73, 51, 144, sampling));
        const ImageFPtr out = doc.layers[0].asset->image.f32();
        REQUIRE(out && out->width() == 73 && out->height() == 51);
        const auto expected = ref::resample(ref::canvasOf(*pixels), 73, 51, 40.0 / 73 / 2, 40.0 / 73, 30.0 / 51 / 2, 30.0 / 51, filter);
        const double error = ref::worstError(*out, expected);
        if (!(error <= 1)) std::fprintf(stderr, "  image size %d: %g\n", int(filter), error);
        CHECK(error <= 1);
    }
}

TEST_MAIN()
