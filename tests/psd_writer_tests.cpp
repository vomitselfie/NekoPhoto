// PSD export against our own importer: each document is written, read back, and compared, structure first
// (layers, names, order, bounds, opacity, visibility, blend modes, clipping, folders, masks) and then as
// rendered, which must agree to within one level. The fixtures follow docs/psd-export.md.
#include "check.h"
#include "compositor/adjustments.h"
#include "compositor/psd.h"
#include "compositor/project.h"
#include "compositor/psd_writer.h"
#include "compositor/render.h"
#include "psd/psd_descriptor.hpp"
#include <filesystem>
#include <set>

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
    CHECK_EQ(int(plan.warnings.size()), 0);   // folder opacity is drawn here as in Photoshop now
}

TEST_CASE(folders_isolate_and_fade_as_photoshop_does) {
    // Grey backdrop; a folder holding a Multiply layer of half grey. Pass Through: the child multiplies the
    // backdrop. Isolated (Normal): it multiplies nothing inside the folder, so the folder shows the child as is.
    Document doc(4, 4);
    doc.layers.push_back(pixels("Back", solid(4, 4, 200, 200, 200), {0, 0}));
    Layer f = folder("F", doc);
    doc.layers.push_back(f);
    Layer child = pixels("Child", solid(4, 4, 128, 128, 128), {0, 0});
    child.parentId = f.id;
    child.blendMode = BlendMode::Multiply;
    doc.layers.push_back(child);
    CHECK(std::abs(int(renderFlattened(doc)->pixel(1, 1)[0]) - 200 * 128 / 255) <= 1);
    doc.layers[1].passThrough = false;
    CHECK(std::abs(int(renderFlattened(doc)->pixel(1, 1)[0]) - 128) <= 1);
    // Half opacity: halfway back to the backdrop, isolated or not.
    doc.layers[1].opacity = 0.5;
    CHECK(std::abs(int(renderFlattened(doc)->pixel(1, 1)[0]) - (200 + 128) / 2) <= 1);
    doc.layers[1].passThrough = true;
    CHECK(std::abs(int(renderFlattened(doc)->pixel(1, 1)[0]) - (200 + 200 * 128 / 255) / 2) <= 1);
    // Saved as a project it needs version 8; the plain document stays at the Mac app's 7.
    CHECK(manifestJson(doc, std::nullopt).find("\"version\": 8") != std::string::npos);
    doc.layers[1].opacity = 1;
    CHECK(manifestJson(doc, std::nullopt).find("\"version\": 7") != std::string::npos);
    // Through PSD: Pass Through and isolation survive.
    doc.layers[1].passThrough = false;
    auto rt = roundTrip(doc, "isolated.psd");
    REQUIRE(rt.imported.has_value());
    CHECK(!rt.imported->document.layers[1].passThrough);
}

namespace {

/// A layer as if opened from a PSD: a style (kept always), text (kept while the pixels are), a vector mask
/// (kept while the layer stays put), Blend If ranges, Fill 50% under opacity 80%, and Photoshop's id.
Layer carried(const std::string& name, std::shared_ptr<Image> image, Point at, const std::string& blend = "norm", BlendMode as = BlendMode::Normal) {
    Layer l = pixels(name, image, at);
    l.blendMode = as;
    auto c = std::make_shared<PsdLayerCarry>();
    c->blocks = {{"lfx2", {0, 0, 0, 0, 0, 0, 0, 16, 1, 2}}, {"TySh", {7, 7, 7, 7}}, {"vmsk", {0, 0, 0, 3, 0, 0}}, {"lclr", {0, 4, 0, 0, 0, 0, 0, 0}}};
    c->blendingRanges = std::vector<uint8_t>(40, 0);
    c->blendingRanges[3] = 0xff;
    c->opacity = 204; c->fill = 128;
    l.opacity = 204 / 255.0 * (128 / 255.0);
    c->layerId = 42;
    c->blendKey = blend;
    c->blendAs = int(as);
    c->contentHash = psdContentHash(image.get());
    c->placement = l.transform;
    l.psdCarry = c;
    return l;
}

std::set<std::string> keys(const Layer& l) {
    std::set<std::string> out;
    if (l.psdCarry) for (auto& b : l.psdCarry->blocks) out.insert(b.key);
    return out;
}

} // namespace

TEST_CASE(psd_carry_comes_back_while_it_is_still_true) {
    Document doc(40, 40);
    doc.layers.push_back(carried("Untouched", solid(10, 10, 200, 0, 0), {2, 2}, "lbrn", BlendMode::ColorBurn));
    doc.layers.push_back(carried("Painted", solid(10, 10, 0, 200, 0), {14, 2}));
    doc.layers.push_back(carried("Moved", solid(10, 10, 0, 0, 200), {26, 2}));
    doc.layers.push_back(carried("Faded", solid(10, 10, 9, 9, 9), {2, 20}));
    doc.layers[1].asset = Asset::make(solid(10, 10, 0, 100, 0), "Painted");     // new pixels
    doc.layers[2].transform.origin = Point(27, 3);                              // moved
    doc.layers[3].opacity = 0.3;                                                // opacity changed
    auto rt = roundTrip(doc, "carry.psd");
    REQUIRE(rt.imported.has_value());
    const auto& back = rt.imported->document.layers;
    REQUIRE(back.size() == 4);
    CHECK(keys(back[0]) == (std::set<std::string>{"lfx2", "TySh", "vmsk", "lclr"}));
    CHECK(keys(back[1]) == (std::set<std::string>{"lfx2", "vmsk", "lclr"}));   // text no longer true
    CHECK(keys(back[2]) == (std::set<std::string>{"lfx2", "lclr"}));           // nor text, nor the vector mask
    CHECK(keys(back[3]) == (std::set<std::string>{"lfx2", "TySh", "vmsk", "lclr"}));
    REQUIRE(back[0].psdCarry);
    CHECK(back[0].psdCarry->blendKey == "lbrn");                               // not Color Burn
    CHECK(back[0].psdCarry->blendingRanges == doc.layers[0].psdCarry->blendingRanges);
    CHECK_EQ(int(back[0].psdCarry->opacity), 204);
    CHECK_EQ(int(back[0].psdCarry->fill), 128);
    CHECK_EQ(int(back[0].psdCarry->layerId), 42);
    CHECK(back[1].psdCarry->layerId != 42 && back[2].psdCarry->layerId != 42);   // ids stay unique
    CHECK_EQ(int(back[3].psdCarry->fill), 255);                                 // ours alone once changed
    CHECK(std::abs(back[3].opacity - 0.3) < 1.0 / 255);
    CHECK(std::abs(back[0].opacity - doc.layers[0].opacity) < 1.0 / 255);
    bool textNote = false, vectorWarning = false;
    for (auto& n : rt.summary.notes) textNote |= n.find("Painted") != std::string::npos && n.find("editable text") != std::string::npos;
    for (auto& w : rt.summary.warnings) vectorWarning |= w.find("Moved") != std::string::npos && w.find("vector mask") != std::string::npos;
    CHECK(textNote);
    CHECK(vectorWarning);
}

TEST_CASE(psd_carry_survives_a_project_save) {
    Document doc(20, 20);
    doc.layers.push_back(carried("A", solid(8, 8, 1, 2, 3, 128), {3, 3}));
    auto dc = std::make_shared<PsdDocumentCarry>();
    dc->resources = {{1032, "", {0, 0, 0, 1}}, {2000, "Path 1", {1, 2, 3}}};
    dc->globals = {{"Txt2", {1, 2, 3, 4, 5}}};
    dc->width = 20; dc->height = 20;
    doc.psdCarry = dc;
    const auto package = std::filesystem::temp_directory_path() / "nekophoto-psd-carry.comp";
    ProjectError error;
    REQUIRE(saveProject(doc, std::nullopt, package.string(), error));
    auto loaded = loadProject(package.string(), error);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->layers[0].psdCarry);
    CHECK(loaded->layers[0].psdCarry->blocks == doc.layers[0].psdCarry->blocks);
    CHECK_EQ(loaded->layers[0].psdCarry->contentHash, doc.layers[0].psdCarry->contentHash);
    // The pixels came back through PNG, and still match what the text describes.
    CHECK_EQ(psdContentHash(loaded->layers[0].asset->image.get()), doc.layers[0].psdCarry->contentHash);
    REQUIRE(loaded->psdCarry);
    CHECK_EQ(int(loaded->psdCarry->resources.size()), 2);
    CHECK(loaded->psdCarry->globals == dc->globals);
    std::filesystem::remove_all(package);

    // Exported: the resources and global block are in the file; after a canvas change, not the guides or paths.
    auto rt = roundTrip(*loaded, "carry-project.psd");
    REQUIRE(rt.imported && rt.imported->document.psdCarry);
    CHECK_EQ(int(rt.imported->document.psdCarry->resources.size()), 2);
    CHECK(rt.imported->document.psdCarry->globals == dc->globals);
    loaded->width = 30;
    auto resized = roundTrip(*loaded, "carry-resized.psd");
    REQUIRE(resized.imported && resized.imported->document.psdCarry);
    CHECK_EQ(int(resized.imported->document.psdCarry->resources.size()), 0);
}

TEST_CASE(psd_export_writes_text_as_photoshop_type_layers) {
    Document doc(300, 200);
    LayerText text;
    text.text = "Hello (World)\nsecond";
    text.fontSize = 30; text.red = 1; text.alignment = 1; text.letterSpacing = 3;
    auto raster = solid(200, 90, 255, 0, 0, 0);
    Layer l = pixels("Title", raster, {40, 30});
    l.text = text; l.textImage = raster;
    doc.layers.push_back(l);
    PsdExportOptions options;
    options.textMetrics = [](const LayerText&) {
        PsdTextMetrics m;
        m.postScriptName = "Example-Bold"; m.fontSize = 30; m.ascent = 28; m.lineHeight = 36;
        m.blockLeft = 4; m.blockTop = 4; m.blockWidth = 192; m.lines = 2;
        return std::optional<PsdTextMetrics>(m);
    };
    PsdExportSummary summary;
    std::string error;
    auto bytes = encodePsd(doc, options, &summary, &error);
    REQUIRE(!bytes.empty());
    CHECK_EQ(summary.texts, 1);
    CHECK(summary.notes.empty());
    const std::string file(bytes.begin(), bytes.end());
    const size_t at = file.find("8BIMTySh");
    REQUIRE(at != std::string::npos);
    const uint32_t length = uint32_t(uint8_t(file[at + 8])) << 24 | uint32_t(uint8_t(file[at + 9])) << 16 | uint32_t(uint8_t(file[at + 10])) << 8 | uint8_t(file[at + 11]);
    CHECK_EQ(length % 2, 0u);
    std::vector<uint8_t> block(bytes.begin() + long(at + 12), bytes.begin() + long(at + 12 + length));
    patchy::psd::BigEndianReader r(block);
    CHECK_EQ(int(r.read_u16()), 1);
    double m[6];
    for (double& v : m) v = patchy::psd::read_f64(r);
    // Centred: anchored at the block's middle, on the first baseline.
    CHECK(std::abs(m[4] - (40 + 4 + 96)) < 1e-9);
    CHECK(std::abs(m[5] - (30 + 4 + 28)) < 1e-9);
    CHECK_EQ(int(r.read_u16()), 50);
    CHECK_EQ(int(r.read_u32()), 16);
    auto descriptor = patchy::psd::read_descriptor(r);
    CHECK(descriptor.class_id == "TxLr");
    auto txt = patchy::psd::descriptor_value(descriptor, "Txt ");
    REQUIRE(txt);
    CHECK(txt->string_value == "Hello (World)\rsecond\r");
    auto bounds = patchy::psd::descriptor_object(descriptor, "bounds");
    REQUIRE(bounds);
    CHECK(std::abs(patchy::psd::descriptor_number(*bounds, "Left") + 96) < 1e-9);
    CHECK(std::abs(patchy::psd::descriptor_number(*bounds, "Top ") + 28) < 1e-9);
    auto engine = patchy::psd::descriptor_value(descriptor, "EngineData");
    REQUIRE(engine);
    const std::string e(engine->raw_value.begin(), engine->raw_value.end());
    CHECK(e.find("/Justification 2") != std::string::npos);
    CHECK(e.find("/Leading 36.0") != std::string::npos);
    CHECK(e.find("/AutoLeading false") != std::string::npos);
    CHECK(e.find("/Tracking 100") != std::string::npos);
    CHECK(e.find("/RunLengthArray [ 21 ]") != std::string::npos);
    CHECK_EQ(int(r.read_u16()), 1);   // warp
    CHECK_EQ(int(r.read_u32()), 16);
    auto warp = patchy::psd::read_descriptor(r);
    CHECK(warp.class_id == "warp");
    CHECK_EQ(r.remaining(), size_t(16));   // then the tail
    // Our reader sees a text layer.
    const auto dir = std::filesystem::temp_directory_path() / "nekophoto-psd-writer-tests";
    std::filesystem::create_directories(dir);
    REQUIRE(exportPsd(doc, (dir / "text.psd").string(), options, nullptr, &error));
    auto back = importPsd((dir / "text.psd").string(), &error);
    REQUIRE(back.has_value());
    CHECK(back->document.layers[0].extraJson.find("Hello (World)") != std::string::npos);
    // Flipped text cannot be Photoshop text.
    doc.layers[0].transform.flipX = true;
    auto flipped = encodePsd(doc, options, &summary, &error);
    CHECK(std::string(flipped.begin(), flipped.end()).find("8BIMTySh") == std::string::npos);
}

TEST_CASE(psb_export_reads_back_with_even_composite_rows) {
    Document doc(40, 30);
    doc.layers.push_back(pixels("A", softDisc(20, 200, 30, 30), {5, 5}));
    Layer masked = pixels("B", solid(10, 10, 0, 0, 255), {20, 10});
    auto mask = std::make_shared<GrayImage>(10, 10, 255);
    for (int y = 0; y < 10; y++) for (int x = 0; x < 5; x++) mask->at(x, y) = 0;
    masked.mask = LayerMask{};
    masked.mask->asset = MaskAsset::make(mask);
    doc.layers.push_back(masked);
    PsdExportOptions options;
    options.large = true;
    std::string error;
    auto bytes = encodePsd(doc, options, nullptr, &error);
    REQUIRE(!bytes.empty());
    CHECK(bytes[4] == 0 && bytes[5] == 2);   // version 2: PSB
    auto back = importPsdBytes(bytes, &error);
    REQUIRE(back.has_value());
    REQUIRE(back->document.layers.size() == 2);
    CHECK(back->document.layers[1].mask.has_value());
    checkLooksTheSame(doc, *back, 1);
    // Every row of the merged image is an even number of bytes (Photoshop's rule for embedded files).
    auto psd = encodePsd(doc, {}, nullptr, &error);
    const size_t h = 30, planes = 4;
    size_t at = psd.size();
    // Walk back from the end: the rows follow the u16 counts, which follow the compression u16.
    size_t rowsTotal = 0;
    std::vector<size_t> counts;
    for (size_t start = 0; start + 2 + h * planes * 2 < psd.size(); start++) {
        if (psd[start] != 0 || psd[start + 1] != 1) continue;
        size_t sum = 0;
        std::vector<size_t> c;
        for (size_t i = 0; i < h * planes; i++) { const size_t n = size_t(psd[start + 2 + i * 2]) << 8 | psd[start + 3 + i * 2]; c.push_back(n); sum += n; }
        if (start + 2 + h * planes * 2 + sum == at) { counts = c; rowsTotal = sum; break; }
    }
    REQUIRE(!counts.empty());
    CHECK(rowsTotal > 0);
    for (size_t n : counts) CHECK(n % 2 == 0);
}

TEST_MAIN()
