// Gradient interpolation methods (Photoshop's Method: Perceptual, Linear, Classic; layerstyle.h): the colour each
// gives, and a matrix of gradient fill layers and gradient overlays in every method, at 8 and 16 bits in RGB, CMYK
// and Lab, each rendered, written as PSD, read back with its method, and rendered the same again. With
// AGPSD_FIXTURES pointing at ag-psd's test folder (MIT, https://github.com/Agamnentzar/ag-psd), its Photoshop-saved
// Perceptual and Linear files are rendered against their merged images.
#include "check.h"
#include "compositor/adjustments.h"
#include "compositor/colormgmt.h"
#include "compositor/colormodes.h"
#include "compositor/depth.h"
#include "compositor/document.h"
#include "compositor/layerstyle.h"
#include "compositor/psd.h"
#include "compositor/psd_carry.h"
#include "compositor/psd_writer.h"
#include "compositor/render.h"
#include "compositor/shape.h"
#include "compositor/vectorlayer.h"
#include "compositor/vectormask.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace compositor;

namespace {

using Method = StyleGradient::Interpolation;
const char* methodName(Method m) { return m == Method::Perceptual ? "perceptual" : m == Method::Linear ? "linear" : "classic"; }

/// Three colour stops, a non-default midpoint, and an opacity ramp through a partial stop.
StyleGradient sample(Method method) {
    StyleGradient g;
    g.colors = {{0, {230, 150, 50}, 0.5f}, {0.45f, {40, 145, 220}, 0.3f}, {1, {250, 250, 240}, 0.5f}};
    g.alphas = {{0, 1, 0.5f}, {0.6f, 0.4f, 0.7f}, {1, 0.9f, 0.5f}};
    g.interpolation = method;
    g.angle = 0;
    return g;
}

Document fillDocument(Method method) {
    Document doc(64, 24);
    Layer base("Base", doc.size());
    auto white = std::make_shared<Image>(64, 24);
    std::fill(white->data(), white->data() + white->byteCount(), uint8_t(255));
    base.asset = Asset::make(white, "Base");
    doc.layers.push_back(base);
    // A shape layer filled with the gradient (a 'GdFl' fill) and stroked with it.
    VectorShape shape;
    shape.path = rectanglePath(Rect(2, 2, 60, 20));
    shape.fillPaint.kind = VectorPaint::Kind::Gradient;
    shape.fillPaint.gradient = sample(method);
    shape.fillPaint.gradient.fillLayer = true;
    shape.stroke.enabled = true;
    shape.stroke.width = 3;
    shape.stroke.paint.kind = VectorPaint::Kind::Gradient;
    shape.stroke.paint.gradient = sample(method);
    shape.stroke.paint.gradient.angle = 90;
    Layer fill(Asset::make(std::make_shared<Image>(1, 1), "Gradient"), Point(0, 0));
    setVectorShape(fill, doc, shape);
    doc.layers.push_back(fill);
    // A pixel layer with a gradient overlay in the same method.
    Layer box("Box", doc.size());
    auto pixels = std::make_shared<Image>(64, 24);
    for (int y = 4; y < 20; y++) for (int x = 8; x < 56; x++) { uint8_t* p = pixels->row(y) + x * 4; p[0] = p[1] = p[2] = p[3] = 255; }
    box.asset = Asset::make(pixels, "Box");
    LayerStyle style;
    GradientOverlay overlay;
    overlay.gradient = sample(method);
    style.gradientOverlays.push_back(overlay);
    setLayerStyle(box, style);
    doc.layers.push_back(box);
    return doc;
}

/// How many gradientInterpolationMethodType enums in `bytes` hold `method`'s value as Photoshop spells it.
int methodEnums(const std::vector<uint8_t>& bytes, Method method) {
    const std::string type = "gradientInterpolationMethodType";
    const std::string value = method == Method::Perceptual ? "Perc" : method == Method::Linear ? "Lnr " : "Gcls";
    const std::string all(bytes.begin(), bytes.end());
    int count = 0;
    // The type ID, then the value as a four-character code: a zero length and the code.
    for (size_t at = all.find(type); at != std::string::npos; at = all.find(type, at + 1))
        if (all.compare(at + type.size(), 8, std::string("\0\0\0\0", 4) + value) == 0) count++;
    return count;
}

int maxApart(const Image& a, const Image& b) {
    int worst = 0;
    for (int y = 0; y < a.height(); y++) for (int x = 0; x < a.width() * 4; x++) worst = std::max(worst, std::abs(int(a.row(y)[x]) - int(b.row(y)[x])));
    return worst;
}

} // namespace

TEST_CASE(perceptual_matches_photoshops_sample) {
    // Photoshop's composite of an orange-to-blue Perceptual gradient at 100% smoothness (ag-psd's gradient-mode, as
    // PhotoCraft measured it): at t = 0.491, (0.592, 0.612, 0.596); at t = 0.113, (0.839, 0.608, 0.290).
    StyleGradient g;
    g.colors = {{0, {225, 154, 51}, 0.5f}, {1, {42, 145, 217}, 0.5f}};
    g.interpolation = Method::Perceptual;
    const double want[2][3] = {{0.592, 0.612, 0.596}, {0.839, 0.608, 0.290}};
    const float at[2] = {0.491f, 0.113f};
    for (int i = 0; i < 2; i++) {
        double c[3];
        gradientColorExact(g, at[i], c);
        for (int k = 0; k < 3; k++) CHECK(std::abs(c[k] / 255.0 - want[i][k]) < 0.012);
    }
    // Classic and Linear give other colours half way; all three agree at the stops.
    double classic[3], linear[3], perceptual[3];
    g.interpolation = Method::Classic; gradientColorExact(g, 0.5f, classic);
    g.interpolation = Method::Linear; gradientColorExact(g, 0.5f, linear);
    g.interpolation = Method::Perceptual; gradientColorExact(g, 0.5f, perceptual);
    CHECK(std::abs(classic[0] - linear[0]) > 10);
    CHECK(std::abs(classic[0] - perceptual[0]) > 5);
    for (Method m : {Method::Classic, Method::Linear, Method::Perceptual}) {
        g.interpolation = m;
        const StyleColor end = gradientColor(g, 1.0f), start = gradientColor(g, 0.0f);
        CHECK(end.r == 42 && end.g == 145 && end.b == 217);
        CHECK(start.r == 225 && start.g == 154 && start.b == 51);
    }
}

TEST_CASE(every_method_renders_and_round_trips_in_every_mode) {
    struct Variant { ColorMode mode; SampleType type; const char* name; };
    const Variant variants[] = {{ColorMode::RGB, SampleType::U8, "rgb8"},  {ColorMode::RGB, SampleType::U16, "rgb16"},
                                {ColorMode::CMYK, SampleType::U8, "cmyk8"}, {ColorMode::CMYK, SampleType::U16, "cmyk16"},
                                {ColorMode::Lab, SampleType::U8, "lab8"},   {ColorMode::Lab, SampleType::U16, "lab16"}};
    std::shared_ptr<Image> rgb[3];
    for (const Variant& v : variants) {
        for (Method method : {Method::Classic, Method::Linear, Method::Perceptual}) {
            Document doc = fillDocument(method);
            std::string error;
            if (v.mode != ColorMode::RGB) REQUIRE(convertDocumentMode(doc, v.mode, ColorProfile(), ConvertOptions(), &error));
            if (v.type != SampleType::U8) REQUIRE(convertSampleType(doc, v.type, &error));
            Image before;
            render(doc, RenderOptions(), before);
            if (v.mode == ColorMode::RGB && v.type == SampleType::U8) rgb[int(method)] = std::make_shared<Image>(before);
            // Saved and read back: the method is still there, and the render is the same.
            const auto bytes = encodePsd(doc, PsdExportOptions(), nullptr, &error);
            REQUIRE(!bytes.empty());
            // Written as Photoshop writes it: the gradientInterpolationMethodType enum's Gcls, Perc or Lnr.
            if (v.mode == ColorMode::RGB) CHECK_EQ(methodEnums(bytes, method), 3);
            auto back = importPsdBytes(bytes, &error);
            REQUIRE(back.has_value());
            int found = 0;
            for (const Layer& l : back->document.layers) {
                if (!l.psdCarry) continue;
                if (auto shape = vectorShapeOf(l, back->document)) {
                    CHECK(shape->fillPaint.kind == VectorPaint::Kind::Gradient && shape->fillPaint.gradient.interpolation == method);
                    CHECK(shape->stroke.paint.kind == VectorPaint::Kind::Gradient && shape->stroke.paint.gradient.interpolation == method);
                    found += 2;
                }
                const LayerStyle style = editableLayerStyle(l, back->document);
                for (const GradientOverlay& o : style.gradientOverlays) { CHECK(o.gradient.interpolation == method); found++; }
            }
            // The shape stays a shape through Image > Mode, so in every mode its fill, its stroke and the overlay carry the
            // method through the file.
            CHECK_EQ(found, 3);
            Image after;
            render(back->document, RenderOptions(), after);
            const int apart = maxApart(before, after);
            if (apart > 1) std::fprintf(stderr, "  %s %s: %d levels apart after the round trip\n", v.name, methodName(method), apart);
            CHECK(apart <= 1);
        }
    }
    // The three methods draw differently.
    CHECK(maxApart(*rgb[0], *rgb[1]) > 10);
    CHECK(maxApart(*rgb[0], *rgb[2]) > 5);
    CHECK(maxApart(*rgb[1], *rgb[2]) > 5);
}

TEST_CASE(ag_psd_photoshop_files_match_their_merged_images) {
    const char* dir = std::getenv("AGPSD_FIXTURES");
    if (!dir) { std::printf("  skipped: AGPSD_FIXTURES is not set\n"); return; }
    struct Case { const char* path; double mean; int worst; };
    // Measured on the fixtures (docs/layer-styles.md): Perceptual 0.57 / 4 (Classic drawing gave 8.1 / 20), Linear
    // 0.08 / 1 (2.2 / 12 before).
    const Case cases[] = {{"/read-write/gradient-mode/src.psd", 0.8, 5}, {"/read/gradient-overlay/src.psd", 0.2, 2}};
    for (const Case& c : cases) {
        std::string error;
        auto imported = importPsd(std::string(dir) + c.path, &error);
        if (!imported || !imported->composite) { std::printf("  skipped %s: %s\n", c.path, error.c_str()); continue; }
        Image ours;
        render(imported->document, RenderOptions(), ours);
        const Image& ps = *imported->composite;
        REQUIRE(ours.width() == ps.width() && ours.height() == ps.height());
        // Both over white, as the merged image is.
        double sum = 0;
        int worst = 0;
        for (int y = 0; y < ps.height(); y++)
            for (int x = 0; x < ps.width(); x++) {
                const uint8_t* a = ours.row(y) + x * 4;
                const uint8_t* b = ps.row(y) + x * 4;
                int d = 0;
                for (int k = 0; k < 3; k++) {
                    const int va = a[k] + (255 - a[3]), vb = b[k] + (255 - b[3]);
                    d = std::max(d, std::abs(va - vb));
                }
                sum += d;
                worst = std::max(worst, d);
            }
        const double mean = sum / (double(ps.width()) * ps.height());
        std::printf("  %s: mean %.3f, max %d levels\n", c.path, mean, worst);
        CHECK(mean < c.mean);
        CHECK(worst <= c.worst);
    }
}

namespace {

/// A two-stop gradient between straight sRGB colours, as a layer style holds it (the reference colours).
StyleGradient twoStops(StyleColor a, StyleColor b, Method method) {
    StyleGradient g;
    g.colors = {{0, a, 0.5f}, {1, b, 0.5f}};
    g.interpolation = method;
    return g;
}

} // namespace

TEST_CASE(gradient_tool_draws_each_method) {
    // The Gradient tool's ramp (GradientStops) in each method: the same colours as a layer style's two-stop gradient,
    // and Classic exactly as before (a straight blend of the stored values).
    const StyleColor red{255, 0, 0}, blue{0, 0, 255};
    for (Method method : {Method::Classic, Method::Perceptual, Method::Linear}) {
        GradientStops stops;
        stops.start[0] = 1; stops.start[1] = 0; stops.start[2] = 0; stops.start[3] = 1;
        stops.end[0] = 0; stops.end[1] = 0; stops.end[2] = 1; stops.end[3] = 1;
        stops.method = method;
        Image base(256, 1), out(256, 1);
        fillGradient(base, out, Affine(), GradientShape::Linear, {0, 0}, {256, 0}, stops, 1, nullptr);
        int worst = 0;
        for (int x = 0; x < 256; x++) {
            const float t = (x + 0.5f) / 256;
            double want[3];
            if (method == Method::Classic) { want[0] = 255 * (1 - t); want[1] = 0; want[2] = 255 * t; }
            else gradientColorExact(twoStops(red, blue, method), t, want);
            for (int k = 0; k < 3; k++) worst = std::max(worst, int(std::abs(out.row(0)[x * 4 + k] - want[k]) + 0.5));
        }
        std::printf("  %s tool ramp: within %d levels\n", methodName(method), worst);
        CHECK(worst <= 1);
    }
    // Classic bakes to itself; the others to 257 Classic stops with the opacity carried.
    GradientStops fade;
    fade.end[3] = 0;
    CHECK(fade.baked().colors.empty());
    fade.method = Method::Perceptual;
    const GradientStops baked = fade.baked();
    CHECK_EQ(baked.colors.size(), size_t(257));
    CHECK(baked.method == Method::Classic);
    float mid[4];
    baked.sample(0.5f, mid);
    CHECK(std::abs(mid[3] - 0.5f) < 1e-4f);
}

TEST_CASE(gradient_map_draws_each_method) {
    AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::GradientMap);
    s.gradientMap.shadows = {1, 0, 0};
    s.gradientMap.highlights = {0, 0, 1};
    // Classic: the straight blend it always had, byte for byte.
    const std::vector<uint8_t> classic = s.gradientMap.table();
    for (int i = 0; i < 256; i++) {
        CHECK_EQ(int(classic[size_t(i * 3)]), int(std::round(255 * (1 - i / 255.0))));
        CHECK_EQ(int(classic[size_t(i * 3 + 2)]), int(std::round(255.0 * i / 255.0)));
    }
    for (Method method : {Method::Perceptual, Method::Linear}) {
        s.gradientMap.method = method;
        // 8 bits: a gray ramp maps through the method's colours.
        Image ramp(256, 1);
        for (int x = 0; x < 256; x++) { uint8_t* p = ramp.row(0) + x * 4; p[0] = p[1] = p[2] = uint8_t(x); p[3] = 255; }
        REQUIRE(applyAdjustment(s, ramp, Rect(0, 0, 256, 1), 1));
        // 16 bits: the same at the exact luma.
        Image16 deep(256, 1);
        for (int x = 0; x < 256; x++) { uint16_t* p = deep.row(0) + x * 4; p[0] = p[1] = p[2] = uint16_t(std::lround(x / 255.0 * 32768)); p[3] = 32768; }
        REQUIRE(applyAdjustment(s, deep, Rect(0, 0, 256, 1), 1));
        int worst = 0, worstDeep = 0, apart = 0;
        for (int x = 0; x < 256; x++) {
            double want[3];
            gradientColorExact(twoStops({255, 0, 0}, {0, 0, 255}, method), x / 255.0f, want);
            for (int k = 0; k < 3; k++) {
                worst = std::max(worst, int(std::abs(ramp.row(0)[x * 4 + k] - want[k]) + 0.5));
                worstDeep = std::max(worstDeep, int(std::abs(deep.row(0)[x * 4 + k] / 32768.0 * 255 - want[k]) + 0.5));
                apart = std::max(apart, std::abs(int(ramp.row(0)[x * 4 + k]) - int(classic[size_t(x * 3 + k)])));
            }
        }
        std::printf("  %s gradient map: within %d levels at 8 bits, %d at 16; %d from Classic\n", methodName(method), worst, worstDeep, apart);
        CHECK(worst <= 1);
        CHECK(worstDeep <= 1);
        CHECK(apart > 10);
        // Kept in the settings' JSON (projects and automation).
        AdjustmentSettings back;
        REQUIRE(AdjustmentSettings::parse(s.toJson(), back));
        CHECK(back.gradientMap.method == method);
    }
    // An old project's settings (no method) read as Classic; an unknown method is refused.
    const std::string json = AdjustmentSettings::defaults(AdjustmentKind::GradientMap).toJson();
    const std::string key = "\"interpolation\":\"classic\"";
    const size_t at = json.find(key);
    REQUIRE(at != std::string::npos);
    AdjustmentSettings old;
    std::string without = json;
    without.replace(at, key.size(), "\"unused\":false");
    REQUIRE(AdjustmentSettings::parse(without, old));
    CHECK(old.gradientMap.method == Method::Classic);
    std::string bad = json;
    bad.replace(at, key.size(), "\"interpolation\":\"oklch\"");
    CHECK(!AdjustmentSettings::parse(bad, old));
}

TEST_CASE(photoshop_gradient_map_method_is_read) {
    // ag-psd's gradient-overlay-2: a Photoshop-saved Gradient Map adjustment layer, its 'grdm' at version 3 with 'Perc'.
    const char* dir = std::getenv("AGPSD_FIXTURES");
    if (!dir) { std::printf("  skipped: AGPSD_FIXTURES is not set\n"); return; }
    std::string error;
    auto imported = importPsd(std::string(dir) + "/read/gradient-overlay-2/src.psd", &error);
    if (!imported) { std::printf("  skipped: %s\n", error.c_str()); return; }
    int maps = 0;
    for (const Layer& l : imported->document.layers) {
        if (!l.adjustment || l.adjustment->kind != AdjustmentKind::GradientMap) continue;
        AdjustmentSettings s;
        REQUIRE(AdjustmentSettings::parse(l.adjustment->json, s));
        CHECK(s.gradientMap.method == Method::Perceptual);
        // The first and last stops: black and white.
        CHECK(s.gradientMap.shadows.red < 0.01 && s.gradientMap.highlights.red > 0.99);
        maps++;
    }
    CHECK_EQ(maps, 1);
    // Unedited, its 'grdm' goes back to PSD byte for byte (stops, method and all).
    auto grdm = [](const std::vector<uint8_t>& bytes) {
        const std::string all(bytes.begin(), bytes.end()), tag = "8BIMgrdm";
        const size_t at = all.find(tag);
        if (at == std::string::npos || at + 12 > all.size()) return std::string();
        const size_t size = size_t(uint8_t(all[at + 8])) << 24 | size_t(uint8_t(all[at + 9])) << 16 | size_t(uint8_t(all[at + 10])) << 8 | uint8_t(all[at + 11]);
        return all.substr(at, 12 + size);
    };
    std::ifstream in(std::string(dir) + "/read/gradient-overlay-2/src.psd", std::ios::binary);
    const std::vector<uint8_t> original((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto written = encodePsd(imported->document, PsdExportOptions(), nullptr, &error);
    REQUIRE(!written.empty());
    CHECK(!grdm(original).empty());
    CHECK(grdm(written) == grdm(original));
}

TEST_MAIN()
