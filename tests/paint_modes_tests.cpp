// Painting in 32-bit float documents (P5c) and in CMYK and Lab documents (P7 E): the brush on StrokeOps<F32> against
// the double reference in float_reference, the colour put into the document's model before it is painted (linearised
// at 32 bits; through the profile to inks with the profile's black generation in CMYK; to L, a, b in Lab), light above
// white kept, gradients running in the document's model, moving selected pixels, healing at 32 bits, and MyPaint's
// 15-bit round trip keeping every float a dab did not change.
#include "check.h"
#include "float_reference.h"
#include "compositor/brush.h"
#include "compositor/colormgmt.h"
#include "compositor/colormodes.h"
#include "compositor/depth.h"
#include "compositor/mypaint.h"
#include <cmath>
#include <fstream>
#include <sstream>

using namespace compositor;
namespace fr = float_reference;

namespace {

/// A 32-bit document holding one layer: premultiplied linear float at `value` (colour) over alpha 1, with a bright
/// patch (colour 4) in the top-left corner so light above white is on the canvas.
Document floatDocument(int w, int h, float value) {
    Document doc(w, h);
    doc.sampleType = SampleType::F32;
    auto image = std::make_shared<ImageF>(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float* p = image->pixel(x, y);
            const float v = x < 6 && y < 6 ? 4.0f : value;
            p[0] = v; p[1] = v * 0.5f; p[2] = v * 0.25f; p[3] = 1;
        }
    doc.layers = {Layer(Asset::make(ImageFPtr(image), "float"), Point(0, 0))};
    return doc;
}

/// A white CMYK or Lab document at 8 or 16 bits (one opaque layer).
Document whiteDocument(int w, int h, ColorMode mode, SampleType type) {
    Document doc(w, h);
    doc.sampleType = type;
    doc.colorMode = mode;
    doc.profile = mode == ColorMode::CMYK ? defaultCmykProfile() : labProfile();
    const int n = colorModeChannels(mode);
    AnyImage image;
    if (type == SampleType::U16) {
        auto buffer = std::make_shared<Image16>(w, h, n);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint16_t* p = buffer->pixel(x, y);
                for (int c = 0; c < n; c++) p[c] = uint16_t(one16);   // no ink, or L 100
                if (mode == ColorMode::Lab) p[1] = p[2] = uint16_t(labOffset<SampleType::U16>());
            }
        image = Image16Ptr(buffer);
    } else if (mode == ColorMode::CMYK) {
        auto buffer = std::make_shared<ImageC8>(w, h, 5);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) std::fill(buffer->pixel(x, y), buffer->pixel(x, y) + 5, uint8_t(255));
        image = ImageC8Ptr(buffer);
    } else {
        auto buffer = std::make_shared<Image>(w, h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) { uint8_t* p = buffer->pixel(x, y); p[0] = 255; p[1] = p[2] = 128; p[3] = 255; }
        image = ImagePtr(buffer);
    }
    doc.layers = {Layer(Asset::makeAny(image, "white"), Point(0, 0))};
    return doc;
}

BrushSettings brush(double diameter, double hardness, double opacity, float r, float g, float b) {
    BrushSettings s;
    s.diameter = diameter; s.hardness = hardness; s.opacity = opacity;
    s.red = r; s.green = g; s.blue = b;
    return s;
}

/// Little CMS's own answer for an sRGB colour in `profile`'s model, as float (inks 0..100, or L a b).
std::array<float, 4> expected(const float rgb[3], const ColorProfile& profile, PixelFormat to) {
    const ColorTransformPtr t = transformBetween(ColorProfile(), profile, ConvertOptions(), PixelFormat::RGBFloat, to);
    std::array<float, 4> out{};
    if (t) t->apply(rgb, out.data(), 1);
    return out;
}

/// A straight sample (0..1 of `one`) of a pixel at `x`, `y`, channel `c`, of any buffer.
double straightAt(const AnyImage& image, int x, int y, int c) {
    const int n = image.channels();
    double v = 0, a = 1;
    if (image.u16()) { v = image.u16()->pixel(x, y)[c] / double(one16); a = image.u16()->pixel(x, y)[n - 1] / double(one16); }
    else if (image.c8()) { v = image.c8()->pixel(x, y)[c] / 255.0; a = image.c8()->pixel(x, y)[n - 1] / 255.0; }
    else if (image.u8()) { v = image.u8()->pixel(x, y)[c] / 255.0; a = image.u8()->pixel(x, y)[n - 1] / 255.0; }
    return a > 0 ? v / a : 0;
}

} // namespace

TEST_CASE(f32_dab_matches_the_double_reference) {
    // One dab (a click) over light up to 4, hard and soft, stamped and computed per pixel, with a selection and an
    // opacity; the colour is linearised through sRGB's curve first.
    const int w = 48, h = 40;
    for (double hardness : {1.0, 0.0, 0.6})
        for (bool stamped : {true, false})
            for (bool erase : {false, true}) {
                Document doc = floatDocument(w, h, 0.3f);
                auto selection = std::make_shared<GrayF>(w, h);
                std::vector<double> sel(size_t(w) * h);
                for (int y = 0; y < h; y++)
                    for (int x = 0; x < w; x++) sel[size_t(y) * w + x] = selection->at(x, y) = float(x) / w;
                Selection s;
                s.coverage = GrayFPtr(selection);
                doc.selection = s;
                BrushSettings settings = brush(21, hardness, 0.7, 0.8f, 0.4f, 0.1f);
                settings.stampedDabs = stamped;
                settings.erasing = erase;
                BrushStroke stroke(doc.layers[0], false, settings, doc);
                CHECK(stroke.isValid());
                stroke.append(Point{20.3, 17.6});
                stroke.flush();
                fr::Canvas reference = fr::canvasOf(*doc.layers[0].asset->image.f32());
                const TransferCurve curve = TransferCurve::srgb();
                const double colour[3] = {curve.toLinear(0.8f), curve.toLinear(0.4f), curve.toLinear(0.1f)};
                // Stamped dabs land at the nearest quarter pixel (stroke_raster.h's dab cache).
                const double cx = stamped ? std::floor(20.3 * 4 + 0.5) / 4 : 20.3, cy = stamped ? std::floor(17.6 * 4 + 0.5) / 4 : 17.6;
                fr::brushDab(reference, cx, cy, 21, hardness, 0.7, colour, erase, &sel);
                const AnyImage painted = stroke.preview();
                CHECK(painted.f32() != nullptr);
                // Within 1e-5 absolute plus relative, as the renderer is held (worst 0.08 of it when written; the
                // kernel reads its profile from an 8192-entry table the reference computes exactly).
                const double worst = fr::worstError(*painted.f32(), reference);
                if (worst > 1) std::printf("hardness %.1f stamped %d erase %d: worst %.2f\n", hardness, int(stamped), int(erase), worst);
                CHECK(worst <= 1);
            }
}

TEST_CASE(f32_stroke_keeps_light_above_white_and_commits_linear_colour) {
    Document doc = floatDocument(40, 30, 2.5f);   // colour 2.5, 1.25, 0.625: brighter than white
    BrushStroke stroke(doc.layers[0], false, brush(10, 1, 1, 0.5f, 0.5f, 0.5f), doc);
    stroke.append(Point{20.3, 15.4});
    stroke.append(Point{30.3, 15.4});
    BrushStroke::Commit commit = stroke.commit();
    CHECK(commit.asset && commit.asset->image.f32());
    const ImageF& out = *commit.asset->image.f32();
    const float grey = TransferCurve::srgb().toLinear(0.5f);
    CHECK_NEAR(out.pixel(25, 15)[0], grey, 1e-6);   // a fully covered pixel: the colour, linear
    CHECK_NEAR(out.pixel(25, 15)[3], 1.0, 1e-6);
    CHECK_EQ(out.pixel(25, 2)[0], 2.5f);            // untouched: the exact float, above 1
    CHECK_EQ(out.pixel(2, 2)[0], 4.0f);
    // The rim blends in linear light between the colour and the light under it, above white on one side.
    bool between = false;
    for (int y = 8; y < 22; y++) {
        const float v = out.pixel(20, y)[0];
        if (v > grey + 0.01f && v < 2.49f) between = true;
    }
    CHECK(between);
}

TEST_CASE(f32_mask_stroke_and_mask_gradient_stay_coverage) {
    Document doc = floatDocument(32, 24, 0.5f);
    auto mask = std::make_shared<GrayF>(32, 24, 1.0f);
    LayerMask m;
    m.asset = MaskAsset::make(GrayFPtr(mask));
    doc.layers[0].mask = m;
    BrushSettings settings = brush(8, 1, 1, 0, 0, 0);
    settings.maskValue = 0;
    BrushStroke stroke(doc.layers[0], true, settings, doc);
    stroke.append(Point{16, 12});
    const AnyGray painted = stroke.previewMaskAny();
    CHECK(painted.f32() != nullptr);
    CHECK_NEAR(painted.f32()->at(16, 12), 0.0, 1e-7);
    CHECK_NEAR(painted.f32()->at(2, 2), 1.0, 1e-7);
    BrushStroke gradient(doc.layers[0], true, BrushSettings(), doc);
    const float black[4] = {0, 0, 0, 1}, white[4] = {1, 1, 1, 1};
    gradient.fillGradientOver(0, Point{0, 0}, Point{32, 0}, black, white, 1);
    const GrayF& g = *gradient.previewMaskAny().f32();
    CHECK_NEAR(g.at(16, 5), 16.5 / 32, 1e-5);   // coverage, not linearised
}

TEST_CASE(f32_gradient_runs_in_linear_light) {
    Document doc = floatDocument(64, 8, 0.0f);
    BrushStroke stroke(doc.layers[0], false, BrushSettings(), doc);
    const float black[4] = {0, 0, 0, 1}, white[4] = {1, 1, 1, 1};
    stroke.fillGradientOver(0, Point{0, 0}, Point{64, 0}, black, white, 1);
    const ImageF& out = *stroke.preview().f32();
    CHECK_NEAR(out.pixel(31, 4)[0], 31.5 / 64, 1e-5);   // halfway in linear light, as Photoshop's 32-bit gradients run
    CHECK_NEAR(out.pixel(0, 4)[0], 0.5 / 64, 1e-5);
}

TEST_CASE(f32_moving_selected_pixels_keeps_their_floats) {
    Document doc = floatDocument(32, 24, 0.75f);
    auto selection = std::make_shared<GrayF>(32, 24);
    for (int y = 0; y < 6; y++) for (int x = 0; x < 6; x++) selection->at(x, y) = 1;
    Selection s;
    s.coverage = GrayFPtr(selection);
    doc.selection = s;
    BrushStroke move(doc.layers[0], false, BrushSettings(), doc);
    CHECK(move.liftSelection());
    move.moveLifted(Point{10, 8}, false);
    const ImageF& out = *move.preview().f32();
    CHECK_EQ(out.pixel(12, 10)[0], 4.0f);        // the bright patch, moved whole
    CHECK_EQ(out.pixel(2, 2)[3], 0.0f);          // a hole where it was
    CHECK_EQ(out.pixel(20, 20)[0], 0.75f);
}

TEST_CASE(f32_spot_healing_heals_light_above_white) {
    // A flat field of light at 2.5 with a dark dot: healed back to the field, not clipped at white.
    Document doc(48, 48);
    doc.sampleType = SampleType::F32;
    auto image = std::make_shared<ImageF>(48, 48);
    for (int y = 0; y < 48; y++)
        for (int x = 0; x < 48; x++) {
            float* p = image->pixel(x, y);
            const bool dot = (x - 24) * (x - 24) + (y - 24) * (y - 24) < 9;
            p[0] = p[1] = p[2] = dot ? 0.05f : 2.5f;
            p[3] = 1;
        }
    doc.layers = {Layer(Asset::make(ImageFPtr(image), "hdr"), Point(0, 0))};
    for (int mode : {0, 1, 2}) {
        BrushSettings settings = brush(12, 1, 1, 0, 0, 0);
        settings.healing = true;
        settings.healingMode = mode;
        settings.healingSeed = 7;
        BrushStroke stroke(doc.layers[0], false, settings, doc);
        stroke.append(Point{24.5, 24.5});
        BrushStroke::Commit commit = stroke.commit();
        const ImageF& out = *commit.asset->image.f32();
        CHECK_NEAR(out.pixel(24, 24)[0], 2.5, 0.05);
        CHECK_EQ(out.pixel(2, 2)[0], 2.5f);   // outside the stroke: the exact float
    }
}

TEST_CASE(cmyk_black_lays_ink_by_the_profiles_black_generation) {
    for (SampleType type : {SampleType::U8, SampleType::U16}) {
        Document doc = whiteDocument(32, 24, ColorMode::CMYK, type);
        BrushStroke stroke(doc.layers[0], false, brush(10, 1, 1, 0, 0, 0), doc);
        CHECK(stroke.isValid());
        stroke.append(Point{8, 12});
        stroke.append(Point{24, 12});
        BrushStroke::Commit commit = stroke.commit();
        const AnyImage out = commit.asset->image;
        CHECK_EQ(out.channels(), 5);
        const float black[3] = {0, 0, 0};
        const std::array<float, 4> ink = expected(black, doc.profile, PixelFormat::CMYKFloat);
        CHECK(ink[3] > 50);   // the profile's rich black: K and C, M, Y
        const double step = type == SampleType::U16 ? 1.5 / 327.68 : 0.5 / 2.55;   // half a level, in percent
        for (int c = 0; c < 4; c++) CHECK_NEAR((1 - straightAt(out, 16, 12, c)) * 100, ink[c], step + 1e-3);
        CHECK(straightAt(out, 16, 2, 3) == 1.0);   // untouched: no ink
    }
}

TEST_CASE(cmyk_gradient_ends_are_the_stop_inks) {
    Document doc = whiteDocument(64, 8, ColorMode::CMYK, SampleType::U8);
    BrushStroke stroke(doc.layers[0], false, BrushSettings(), doc);
    const float a[4] = {0.1f, 0.6f, 0.9f, 1}, b[4] = {0.9f, 0.2f, 0.3f, 1};
    stroke.fillGradientOver(0, Point{0, 0}, Point{64, 0}, a, b, 1);
    const AnyImage out = stroke.preview();
    const std::array<float, 4> inkA = expected(a, doc.profile, PixelFormat::CMYKFloat), inkB = expected(b, doc.profile, PixelFormat::CMYKFloat);
    for (int c = 0; c < 4; c++) {
        CHECK_NEAR((1 - straightAt(out, 0, 4, c)) * 100, inkA[c] + (inkB[c] - inkA[c]) * 0.5 / 64, 0.6);
        CHECK_NEAR((1 - straightAt(out, 63, 4, c)) * 100, inkA[c] + (inkB[c] - inkA[c]) * 63.5 / 64, 0.6);
        // Halfway, the inks halfway: the gradient runs in CMYK, not RGB.
        CHECK_NEAR((1 - straightAt(out, 31, 4, c)) * 100, inkA[c] + (inkB[c] - inkA[c]) * 31.5 / 64, 0.6);
    }
}

TEST_CASE(lab_stroke_lightness_matches_the_colour) {
    for (SampleType type : {SampleType::U8, SampleType::U16}) {
        Document doc = whiteDocument(32, 24, ColorMode::Lab, type);
        BrushStroke stroke(doc.layers[0], false, brush(10, 1, 1, 0.2f, 0.6f, 0.3f), doc);
        stroke.append(Point{8, 12});
        stroke.append(Point{24, 12});
        const AnyImage out = stroke.commit().asset->image;
        const float rgb[3] = {0.2f, 0.6f, 0.3f};
        const std::array<float, 4> lab = expected(rgb, doc.profile, PixelFormat::LabFloat);
        const double one = type == SampleType::U16 ? double(one16) : 255.0;
        const double scale = type == SampleType::U16 ? 128 : 1, offset = type == SampleType::U16 ? 16384 : 128;
        CHECK_NEAR(straightAt(out, 16, 12, 0) * 100, lab[0], 100 / one);
        CHECK_NEAR((straightAt(out, 16, 12, 1) * one - offset) / scale, lab[1], 1 / scale);
        CHECK_NEAR((straightAt(out, 16, 12, 2) * one - offset) / scale, lab[2], 1 / scale);
        CHECK_NEAR(straightAt(out, 16, 2, 0), 1.0, 1e-9);   // untouched white
    }
}

TEST_CASE(cmyk_move_and_clone_carry_all_five_samples) {
    Document doc = whiteDocument(32, 24, ColorMode::CMYK, SampleType::U8);
    {
        BrushStroke paint(doc.layers[0], false, brush(6, 1, 1, 0, 0, 0), doc);
        paint.append(Point{4, 4});
        BrushStroke::Commit c = paint.commit();
        doc.layers[0].asset = c.asset;
        doc.layers[0].transform = c.transform;
    }
    const uint8_t* before = doc.layers[0].asset->image.c8()->pixel(4, 4);
    const std::vector<uint8_t> ink(before, before + 5);
    auto selection = std::make_shared<GrayImage>(32, 24);
    for (int y = 0; y < 10; y++) for (int x = 0; x < 10; x++) selection->at(x, y) = 255;
    Selection s;
    s.coverage = GrayPtr(selection);
    doc.selection = s;
    BrushStroke move(doc.layers[0], false, BrushSettings(), doc);
    CHECK(move.liftSelection());
    move.moveLifted(Point{12, 6}, true);
    const ImageC8& out = *move.preview().c8();
    for (int c = 0; c < 5; c++) CHECK_EQ(int(out.pixel(16, 10)[c]), int(ink[size_t(c)]));
    for (int c = 0; c < 5; c++) CHECK_EQ(int(out.pixel(4, 4)[c]), int(ink[size_t(c)]));   // duplicated: the original stays
    // Clone Stamp from a CMYK source copies inks.
    doc.selection.reset();
    BrushStroke clone(doc.layers[0], false, brush(6, 1, 1, 0, 0, 0), doc);
    CloneSource source;
    source.imageC8 = doc.layers[0].asset->image.c8();
    source.offset = {-20, -8};
    clone.setClone(source);
    clone.append(Point{24, 12});
    const ImageC8& cloned = *clone.preview().c8();
    for (int c = 0; c < 5; c++) CHECK_NEAR(double(cloned.pixel(24, 12)[c]), double(ink[size_t(c)]), 1);
}

#ifdef BRUSHES_DIR
TEST_CASE(mypaint_at_32_bits_rewrites_only_what_a_dab_changed) {
    std::ifstream in(std::string(BRUSHES_DIR) + "/classic/pencil.myb");
    std::stringstream text;
    text << in.rdbuf();
    if (!myPaintSupported() || text.str().empty()) return;
    Document doc = floatDocument(64, 40, 3.7f);   // light above white everywhere
    BrushSettings settings = brush(6, 1, 1, 0, 0, 0);
    BrushStroke grid(doc.layers[0], false, settings, doc);
    MyPaintStroke stroke(grid, text.str(), settings);
    CHECK(stroke.isValid());
    for (int i = 0; i <= 10; i++) {
        BrushSample s = mouseSample(Point{10.0 + 4 * i, 20}, i * 0.01);
        s.pressure = 1;
        s.dt = 0.01;
        stroke.strokeTo(s);
    }
    stroke.finish();
    const ImageF& out = *grid.preview().f32();
    const ImageF& base = *doc.layers[0].asset->image.f32();
    int changed = 0, kept = 0;
    for (int y = 0; y < 40; y++)
        for (int x = 0; x < 64; x++) {
            const bool same = std::equal(out.pixel(x, y), out.pixel(x, y) + 4, base.pixel(x, y));
            if (same) kept++; else changed++;
        }
    CHECK(changed > 0);
    CHECK_EQ(out.pixel(60, 2)[0], 3.7f);   // far from the stroke: the exact float, above 1
    CHECK_EQ(out.pixel(2, 2)[0], 4.0f);
    CHECK(kept > changed);
    // Under the pencil the colour is black, decoded from 15 bits.
    CHECK(out.pixel(30, 20)[0] < 0.5f);
}
#endif

TEST_CASE(mypaint_refuses_cmyk_and_lab) {
    Document doc = whiteDocument(16, 16, ColorMode::CMYK, SampleType::U8);
    BrushSettings settings = brush(6, 1, 1, 0, 0, 0);
    BrushStroke grid(doc.layers[0], false, settings, doc);
    MyPaintStroke stroke(grid, "{\"settings\": {}}", settings);
    CHECK(!stroke.isValid());
}

TEST_MAIN()
