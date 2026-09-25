// PSD export against our own importer: each document is written, read back, and compared, structure first
// (layers, names, order, bounds, opacity, visibility, blend modes, clipping, folders, masks) and then as
// rendered, which must agree to within one level. The fixtures follow docs/psd-export.md.
#include "check.h"
#include "compositor/adjustments.h"
#include "compositor/psd.h"
#include "compositor/psd_writer.h"
#include "compositor/render.h"
#include <filesystem>

using namespace compositor;

namespace {

std::shared_ptr<Image> solid(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    auto image = std::make_shared<Image>(w, h);
    // Premultiplied, as images are held.
    image->fill(uint8_t((r * a + 127) / 255), uint8_t((g * a + 127) / 255), uint8_t((b * a + 127) / 255), a);
    return image;
}

/// A soft disc: alpha falling off to the edge, colour premultiplied (for the transparent-edge fixture).
std::shared_ptr<Image> softDisc(int side, uint8_t r, uint8_t g, uint8_t b) {
    auto image = std::make_shared<Image>(side, side);
    const double c = (side - 1) / 2.0;
    for (int y = 0; y < side; y++)
        for (int x = 0; x < side; x++) {
            const double d = std::hypot(x - c, y - c) / c;
            const unsigned a = d >= 1 ? 0 : unsigned(std::lround(255 * (1 - d)));
            uint8_t* p = image->pixel(x, y);
            p[0] = uint8_t((r * a + 127) / 255); p[1] = uint8_t((g * a + 127) / 255); p[2] = uint8_t((b * a + 127) / 255); p[3] = uint8_t(a);
        }
    return image;
}

Layer pixels(const std::string& name, std::shared_ptr<Image> image, Point at) { return Layer(Asset::make(image, name), at); }

Layer folder(const std::string& name, const Document& doc) {
    Layer f(name, doc.size());
    f.isGroup = true;
    return f;
}

struct RoundTrip {
    PsdExportSummary summary;
    std::optional<PsdImport> imported;
};

RoundTrip roundTrip(const Document& doc, const char* name) {
    RoundTrip out;
    std::string error;
    const auto dir = std::filesystem::temp_directory_path() / "nekophoto-psd-writer-tests";
    std::filesystem::create_directories(dir);
    const std::string path = (dir / name).string();
    bool ok = exportPsd(doc, path, {}, &out.summary, &error);
    if (!ok) std::fprintf(stderr, "export failed: %s\n", error.c_str());
    CHECK(ok);
    out.imported = importPsd(path, &error);
    if (!out.imported) std::fprintf(stderr, "import failed: %s\n", error.c_str());
    return out;
}

int worstDifference(const Image& a, const Image& b) {
    if (a.width() != b.width() || a.height() != b.height()) return 256;
    int worst = 0;
    for (int y = 0; y < a.height(); y++) {
        const uint8_t* p = a.row(y); const uint8_t* q = b.row(y);
        for (int x = 0; x < a.width() * 4; x++) worst = std::max(worst, std::abs(int(p[x]) - int(q[x])));
    }
    return worst;
}

/// The two documents render alike, and the file's merged image is the original's render.
void checkLooksTheSame(const Document& original, const PsdImport& imported, int tolerance = 1) {
    auto before = renderFlattened(original);
    auto after = renderFlattened(imported.document);
    const int layers = worstDifference(*before, *after);
    if (layers > tolerance) std::fprintf(stderr, "  rendered layers differ by %d\n", layers);
    CHECK(layers <= tolerance);
    REQUIRE(imported.composite != nullptr);
    const int merged = worstDifference(*before, *imported.composite);
    if (merged > 1) std::fprintf(stderr, "  merged image differs by %d\n", merged);
    CHECK(merged <= 1);
}

} // namespace

TEST_CASE(psd_export_single_layer) {
    Document doc(40, 30);
    doc.resolution = 300;
    doc.layers.push_back(pixels("Paint", solid(40, 30, 200, 60, 20), {0, 0}));
    auto rt = roundTrip(doc, "01_single.psd");
    REQUIRE(rt.imported.has_value());
    const Document& back = rt.imported->document;
    CHECK_EQ(back.width, 40); CHECK_EQ(back.height, 30);
    CHECK_NEAR(back.resolution, 300, 0.01);
    REQUIRE(back.layers.size() == 1u);
    CHECK_EQ(back.layers[0].name, std::string("Paint"));
    CHECK(rt.summary.warnings.empty());
    checkLooksTheSame(doc, *rt.imported);
}

TEST_CASE(psd_export_layers_offsets_names_opacity_visibility_blends) {
    Document doc(64, 48);
    doc.layers.push_back(pixels("Background", solid(64, 48, 240, 240, 230), {0, 0}));
    Layer offset = pixels("Offset", solid(20, 10, 10, 120, 200), {30, 25});
    offset.opacity = 0.5;
    doc.layers.push_back(offset);
    Layer beyond = pixels("Past the edge", solid(30, 20, 250, 200, 0), {-10, 35});   // partly off the canvas
    beyond.blendMode = BlendMode::Multiply;
    doc.layers.push_back(beyond);
    Layer hidden = pixels("Hidden", solid(10, 10, 255, 0, 0), {5, 5});
    hidden.visible = false;
    doc.layers.push_back(hidden);
    Layer unicode = pixels("線画 ✏️ Lineart", solid(8, 8, 0, 0, 0), {50, 5});
    unicode.blendMode = BlendMode::Screen;
    doc.layers.push_back(unicode);
    for (BlendMode mode : {BlendMode::Overlay, BlendMode::Darken, BlendMode::Lighten, BlendMode::Difference, BlendMode::ColorDodge,
                           BlendMode::ColorBurn, BlendMode::Hue, BlendMode::Saturation, BlendMode::Color, BlendMode::Luminosity}) {
        Layer l = pixels(blendModeName(mode), solid(6, 6, 90, 160, 220), {double(2 + 6 * int(doc.layers.size() % 9)), 40});
        l.blendMode = mode;
        doc.layers.push_back(l);
    }
    auto rt = roundTrip(doc, "02_layers.psd");
    REQUIRE(rt.imported.has_value());
    const Document& back = rt.imported->document;
    REQUIRE(back.layers.size() == doc.layers.size());
    for (size_t i = 0; i < doc.layers.size(); i++) {
        const Layer& a = doc.layers[i];
        const Layer& b = back.layers[i];
        CHECK_EQ(b.name, a.name);
        CHECK(b.blendMode == a.blendMode);
        CHECK_EQ(b.visible, a.visible);
        CHECK(std::abs(b.opacity - a.opacity) <= 0.5 / 255);
        CHECK_NEAR(b.transform.origin.x, a.transform.origin.x, 1e-9);
        CHECK_NEAR(b.transform.origin.y, a.transform.origin.y, 1e-9);
        CHECK_EQ(b.pixelWidth(), a.pixelWidth());
    }
    checkLooksTheSame(doc, *rt.imported);
}

TEST_CASE(psd_export_masks_and_clipping) {
    Document doc(50, 50);
    doc.layers.push_back(pixels("Paper", solid(50, 50, 255, 255, 255), {0, 0}));
    Layer base = pixels("Base", softDisc(30, 30, 90, 200), {10, 10});
    doc.layers.push_back(base);
    Layer shade = pixels("Shade", solid(50, 50, 20, 20, 60), {0, 0});
    shade.maskSourceId = base.id;
    shade.blendMode = BlendMode::Multiply;
    shade.opacity = 0.6;
    doc.layers.push_back(shade);
    Layer light = pixels("Light", solid(50, 20, 255, 250, 200), {0, 0});
    light.maskSourceId = base.id;
    doc.layers.push_back(light);
    // A masked layer: the left half hidden, a soft ramp across the middle; and a disabled mask.
    Layer masked = pixels("Masked", solid(40, 20, 0, 160, 60), {5, 28});
    auto mask = std::make_shared<GrayImage>(40, 20, 0);
    for (int y = 0; y < 20; y++) for (int x = 0; x < 40; x++) mask->at(x, y) = uint8_t(std::clamp((x - 15) * 25, 0, 255));
    LayerMask lm; lm.asset = MaskAsset::make(mask);
    masked.mask = lm;
    doc.layers.push_back(masked);
    Layer off = pixels("Mask off", solid(10, 10, 200, 0, 200), {38, 2});
    LayerMask disabled; disabled.asset = MaskAsset::make(std::make_shared<GrayImage>(10, 10, 0)); disabled.enabled = false;
    off.mask = disabled;
    doc.layers.push_back(off);
    auto rt = roundTrip(doc, "09_masks_clipping.psd");
    REQUIRE(rt.imported.has_value());
    const Document& back = rt.imported->document;
    REQUIRE(back.layers.size() == 6u);
    CHECK(back.layers[2].maskSourceId == std::optional<Uuid>(back.layers[1].id));
    CHECK(back.layers[3].maskSourceId == std::optional<Uuid>(back.layers[1].id));
    REQUIRE(back.layers[4].mask.has_value());
    CHECK_EQ(int(back.layers[4].mask->asset.image->at(30, 10)), 255);
    CHECK_EQ(int(back.layers[4].mask->asset.image->at(2, 10)), 0);
    REQUIRE(back.layers[5].mask.has_value());
    CHECK(!back.layers[5].mask->enabled);
    CHECK_EQ(rt.summary.clipped, 2);
    CHECK_EQ(rt.summary.masks, 2);
    CHECK(rt.summary.warnings.empty());
    checkLooksTheSame(doc, *rt.imported);
}

TEST_CASE(psd_export_nested_folders_with_masks_and_an_empty_folder) {
    Document doc(40, 40);
    doc.layers.push_back(pixels("Ground", solid(40, 40, 30, 30, 30), {0, 0}));
    Layer outer = folder("Outer", doc);
    doc.layers.push_back(outer);
    Layer a = pixels("A", solid(20, 20, 255, 0, 0), {0, 0}); a.parentId = outer.id; doc.layers.push_back(a);
    Layer inner = folder("Inner", doc); inner.parentId = outer.id;
    auto folderMask = std::make_shared<GrayImage>(40, 40, 0);
    for (int y = 0; y < 40; y++) for (int x = 20; x < 40; x++) folderMask->at(x, y) = 255;
    LayerMask fm; fm.asset = MaskAsset::make(folderMask); inner.mask = fm;
    doc.layers.push_back(inner);
    Layer b = pixels("B", solid(30, 30, 0, 255, 0), {10, 10}); b.parentId = inner.id; b.blendMode = BlendMode::Screen; doc.layers.push_back(b);
    Layer empty = folder("Empty", doc); empty.parentId = outer.id; doc.layers.push_back(empty);
    Layer top = pixels("Top", solid(5, 5, 0, 0, 255), {35, 0}); doc.layers.push_back(top);
    auto rt = roundTrip(doc, "12_nested.psd");
    REQUIRE(rt.imported.has_value());
    const Document& back = rt.imported->document;
    REQUIRE(back.layers.size() == doc.layers.size());
    auto find = [&](const char* name) -> const Layer* { for (const Layer& l : back.layers) if (l.name == name) return &l; return nullptr; };
    const Layer* bOuter = find("Outer"); const Layer* bInner = find("Inner"); const Layer* bB = find("B"); const Layer* bEmpty = find("Empty");
    REQUIRE(bOuter && bInner && bB && bEmpty);
    CHECK(bOuter->isGroup && bInner->isGroup && bEmpty->isGroup);
    CHECK(bInner->parentId == std::optional<Uuid>(bOuter->id));
    CHECK(bB->parentId == std::optional<Uuid>(bInner->id));
    CHECK(bEmpty->parentId == std::optional<Uuid>(bOuter->id));
    REQUIRE(bInner->mask.has_value());
    CHECK_EQ(rt.summary.folders, 3);
    // Order survives: the same names bottom to top.
    for (size_t i = 0; i < doc.layers.size(); i++) CHECK_EQ(back.layers[i].name, doc.layers[i].name);
    checkLooksTheSame(doc, *rt.imported);
}

TEST_CASE(psd_export_keeps_soft_edges_clean) {
    // Premultiplied pixels must be written straight: a dark fringe on every soft edge otherwise.
    Document doc(64, 64);
    doc.layers.push_back(pixels("Glow", softDisc(64, 255, 220, 40), {0, 0}));
    auto rt = roundTrip(doc, "13_soft_edges.psd");
    REQUIRE(rt.imported.has_value());
    const Image& back = *rt.imported->document.layers[0].asset->image;
    const Image& original = *doc.layers[0].asset->image;
    CHECK(worstDifference(original, back) <= 1);
    checkLooksTheSame(doc, *rt.imported);
}

TEST_CASE(psd_export_empty_layer_and_adjustment_layers) {
    Document doc(32, 32);
    auto ramp = std::make_shared<Image>(32, 32);
    for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) { uint8_t* p = ramp->pixel(x, y); p[0] = uint8_t(x * 8); p[1] = uint8_t(y * 8); p[2] = 128; p[3] = 255; }
    doc.layers.push_back(pixels("Ramp", ramp, {0, 0}));
    doc.layers.push_back(Layer("Empty", doc.size()));
    AdjustmentSettings levels = AdjustmentSettings::defaults(AdjustmentKind::Levels);
    levels.levels.ranges[0] = LevelsRange{20, 1.3, 230, 0, 255};
    Layer lv("Levels", doc.size()); lv.adjustment = levels.toLayerAdjustment(); doc.layers.push_back(lv);
    AdjustmentSettings curves = AdjustmentSettings::defaults(AdjustmentKind::Curves);
    curves.curves.channels[0] = {{0, 0}, {128, 160}, {255, 255}};
    Layer cv("Curves", doc.size()); cv.adjustment = curves.toLayerAdjustment(); doc.layers.push_back(cv);
    AdjustmentSettings exposure = AdjustmentSettings::defaults(AdjustmentKind::Exposure);
    exposure.exposure = ExposureSettings{0.5, 0, 1.1};
    Layer ex("Exposure", doc.size()); ex.adjustment = exposure.toLayerAdjustment(); doc.layers.push_back(ex);
    AdjustmentSettings hue = AdjustmentSettings::defaults(AdjustmentKind::HueSaturation);
    hue.hsv.photoshopSaturation = true;
    hue.hsv.adjustments[0] = {20, -30, 5};
    Layer hs("Hue", doc.size()); hs.adjustment = hue.toLayerAdjustment(); doc.layers.push_back(hs);
    auto rt = roundTrip(doc, "14_adjustments.psd");
    REQUIRE(rt.imported.has_value());
    const Document& back = rt.imported->document;
    REQUIRE(back.layers.size() == 6u);
    CHECK(!back.layers[1].asset);   // the empty layer stays empty
    for (size_t i = 2; i < 6; i++) { REQUIRE(back.layers[i].adjustment.has_value()); CHECK(back.layers[i].adjustment->kind == doc.layers[i].adjustment->kind); }
    CHECK_EQ(rt.summary.adjustments, 4);
    CHECK(rt.summary.warnings.empty());
    // Levels stores gamma in hundredths: the look may move by a level or two.
    checkLooksTheSame(doc, *rt.imported, 2);
}

TEST_CASE(psd_export_bakes_what_photoshop_cannot_say) {
    Document doc(40, 40);
    doc.layers.push_back(pixels("Photo", solid(40, 40, 120, 90, 60), {0, 0}));
    // A scaled and rotated layer, resampled into place.
    Layer turned = pixels("Turned", solid(10, 10, 20, 200, 90), {15, 15});
    turned.transform.rotation = 30;
    turned.transform.size = Size(16, 12);
    doc.layers.push_back(turned);
    // Grain has no Photoshop counterpart: baked into a pixel layer.
    AdjustmentSettings grain = AdjustmentSettings::defaults(AdjustmentKind::Grain);
    Layer gr("Grain", doc.size()); gr.adjustment = grain.toLayerAdjustment(); doc.layers.push_back(gr);
    // Clipped to a layer that is not right beneath it: written unclipped, as it shows.
    Layer spacer = pixels("Spacer", solid(4, 4, 0, 0, 0), {0, 36});
    doc.layers.push_back(spacer);
    Layer clipped = pixels("Clipped far", solid(40, 40, 255, 255, 255), {0, 0});
    clipped.maskSourceId = doc.layers[1].id;
    clipped.opacity = 0.5;
    doc.layers.push_back(clipped);
    auto rt = roundTrip(doc, "16_baked.psd");
    REQUIRE(rt.imported.has_value());
    const Document& back = rt.imported->document;
    REQUIRE(back.layers.size() == doc.layers.size());
    CHECK(!back.layers[2].adjustment.has_value());   // the grain is pixels now
    CHECK(!back.layers[4].maskSourceId.has_value());
    CHECK_EQ(int(rt.summary.warnings.size()), 2);
    CHECK_EQ(int(rt.summary.notes.size()), 1);
    checkLooksTheSame(doc, *rt.imported, 2);
}

TEST_CASE(psd_export_refuses_documents_past_psd_limits) {
    Document doc(psdMaxSide + 1, 10);
    std::string error;
    PsdExportSummary summary;
    CHECK(encodePsd(doc, {}, &summary, &error).empty());
    CHECK(error.find("PSB") != std::string::npos);
}

TEST_CASE(psd_export_plan_matches_the_export) {
    Document doc(20, 20);
    doc.layers.push_back(pixels("A", solid(20, 20, 1, 2, 3), {0, 0}));
    Layer f = folder("F", doc); f.opacity = 0.5; doc.layers.push_back(f);
    Layer b = pixels("B", solid(5, 5, 9, 9, 9), {1, 1}); b.parentId = f.id; doc.layers.push_back(b);
    PsdExportSummary plan = planPsdExport(doc);
    auto rt = roundTrip(doc, "plan.psd");
    CHECK_EQ(plan.layers, rt.summary.layers);
    CHECK_EQ(plan.folders, rt.summary.folders);
    CHECK_EQ(plan.warnings.size(), rt.summary.warnings.size());
    CHECK_EQ(int(plan.warnings.size()), 1);   // the folder's opacity
}

TEST_MAIN()
