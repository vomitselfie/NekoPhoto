// Smart objects: placement geometry, Photoshop's placed-layer and linked-file blocks, the project package and
// PSD export. The real-file checks (Photoshop's own placed PNG, a designer file with warped, filtered and linked
// instances) run over Patchy's fixtures through build/tests/psd_roundtrip (docs/smart-objects.md).
#include "check.h"
#include "compositor/png.h"
#include "compositor/project.h"
#include "compositor/psd.h"
#include "compositor/psd_writer.h"
#include "compositor/render.h"
#include "compositor/smartobject.h"
#include "compositor/smartobject_edit.h"
#include "compositor/smartfilter.h"
#include "psd/psd_descriptor.hpp"
#include <filesystem>

using namespace compositor;
namespace psd = patchy::psd;

namespace {

bool near(const std::array<double, 8>& a, const std::array<double, 8>& b) {
    for (size_t i = 0; i < 8; i++) if (std::abs(a[i] - b[i]) > 1e-6) return false;
    return true;
}

psd::DescriptorValue dbl(double v) { psd::DescriptorValue d; d.type = psd::DescriptorValue::Type::Double; d.double_value = v; return d; }
psd::DescriptorValue text(const std::string& v) { psd::DescriptorValue d; d.type = psd::DescriptorValue::Type::String; d.string_value = v; return d; }
psd::DescriptorValue quadList(const std::array<double, 8>& q) {
    psd::DescriptorValue d; d.type = psd::DescriptorValue::Type::List;
    for (double v : q) d.list_value.push_back(dbl(v));
    return d;
}

/// A 'SoLd' as Photoshop writes one: the source id, the instance id and the placement quad.
std::vector<uint8_t> sold(const std::string& source, const std::array<double, 8>& quad) {
    psd::DescriptorObject d;
    d.class_id = "null";
    auto add = [&](const std::string& k, psd::DescriptorValue v) { d.values[k] = v; d.key_order.push_back({k, k.size() != 4}); };
    add("Idnt", text(source));
    add("placed", text("11111111-2222-3333-4444-555555555555"));
    add("Trnf", quadList(quad));
    add("nonAffineTransform", quadList(quad));
    psd::BigEndianWriter w;
    for (char c : std::string("soLD")) w.write_u8(uint8_t(c));
    w.write_u32(4);
    w.write_u32(16);
    psd::write_descriptor(w, d);
    return w.bytes();
}

/// A 20 x 10 document placing a 4 x 2 source, scaled x2 at (6, 3).
Document placed() {
    Document doc(20, 10);
    auto source = std::make_shared<SmartObjectSource>();
    source->id = "5ad5e2a9-c713-1549-8660-e253d5db7729";
    source->fileName = "small.png";
    source->fileType = "png ";
    source->bytes = std::make_shared<const std::vector<uint8_t>>(std::vector<uint8_t>{1, 2, 3});
    auto image = std::make_shared<Image>(4, 2);
    image->fill(0, 128, 0, 255);
    source->image = image;
    source->width = 4; source->height = 2;
    doc.smartObjects[source->id] = source;
    const std::array<double, 8> quad{6, 3, 14, 3, 14, 7, 6, 7};
    Layer layer(Asset::make(image, "Placed"), Point(6, 3));
    layer.transform = *transformForQuad(quad, 4, 2);
    SmartObjectInstance instance;
    instance.sourceId = source->id;
    instance.quad = quad;
    instance.placedId = "11111111-2222-3333-4444-555555555555";
    instance.psdBlocks.push_back({"SoLd", sold(source->id, quad)});
    instance.placedTransform = layer.transform;
    instance.placedWidth = 4; instance.placedHeight = 2;
    layer.smartObject = instance;
    layer.smartImage = image;
    doc.layers.push_back(layer);
    return doc;
}

} // namespace

TEST_CASE(quads_and_transforms_agree) {
    // An upright quad, a rotated one and a vertically flipped one all come back to the same corners.
    for (const std::array<double, 8>& quad : {std::array<double, 8>{10, 20, 50, 20, 50, 40, 10, 40},
                                              std::array<double, 8>{10, 10, 30, 30, 20, 40, 0, 20},
                                              std::array<double, 8>{500, 2001, 837, 2001, 837, 1001, 500, 1001}}) {
        auto t = transformForQuad(quad, 100, 50);
        REQUIRE(t.has_value());
        std::array<double, 8> back{};
        const double corners[4][2] = {{0, 0}, {100, 0}, {100, 50}, {0, 50}};
        for (int i = 0; i < 4; i++) { const Point p = mapThroughTransform(*t, 100, 50, corners[i][0], corners[i][1]); back[size_t(i * 2)] = p.x; back[size_t(i * 2 + 1)] = p.y; }
        CHECK(near(back, quad));
    }
    CHECK(!transformForQuad({0, 0, 10, 0, 12, 10, 2, 10}, 10, 10).has_value());   // skewed
    CHECK(!transformForQuad({0, 0, 10, 0, 11, 12, 0, 10}, 10, 10).has_value());   // perspective
}

TEST_CASE(moving_a_layer_moves_its_quad) {
    const std::array<double, 8> quad{2, 2, 8, 2, 8, 6, 2, 6};
    LayerTransform before(Point(0, 0), Size(10, 10)), after(Point(5, 1), Size(20, 20));
    CHECK(near(moveQuad(quad, before, 10, 10, after, 10, 10), {9, 5, 21, 5, 21, 13, 9, 13}));
}

TEST_CASE(placement_blocks_parse_and_patch) {
    const std::array<double, 8> quad{6, 3, 14, 3, 14, 7, 6, 7};
    auto block = sold("abc", quad);
    auto p = parsePsdPlacement("SoLd", block);
    REQUIRE(p.has_value());
    CHECK(p->sourceId == "abc");
    CHECK(near(p->quad, quad));
    CHECK(!p->warped && !p->filtered && !p->nonAffine);
    const std::array<double, 8> moved{16, 3, 24, 3, 24, 7, 16, 7};
    auto patched = patchPsdPlacement("SoLd", block, moved, "99999999-2222-3333-4444-555555555555");
    REQUIRE(patched.has_value());
    auto again = parsePsdPlacement("SoLd", *patched);
    REQUIRE(again.has_value());
    CHECK(near(again->quad, moved));
    CHECK(again->placedId == "99999999-2222-3333-4444-555555555555");
    CHECK(!again->nonAffine);   // the perspective quad moved along with it
    // Unchanged, a patch writes the same bytes back.
    CHECK(*patchPsdPlacement("SoLd", block, quad) == block);
}

TEST_CASE(linked_file_blocks_parse) {
    psd::BigEndianWriter body;
    for (char c : std::string("liFD")) body.write_u8(uint8_t(c));
    body.write_u32(7);
    const std::string id = "5ad5e2a9-c713-1549-8660-e253d5db7729";
    body.write_u8(uint8_t(id.size()));
    for (char c : id) body.write_u8(uint8_t(c));
    psd::write_descriptor_unicode_string(body, "small.png");
    for (char c : std::string("png     ")) body.write_u8(uint8_t(c));
    body.write_u64(3);
    body.write_u8(0);
    for (uint8_t b : {1, 2, 3}) body.write_u8(b);
    psd::BigEndianWriter block;
    block.write_u64(body.bytes().size());
    block.write_bytes(body.bytes());
    while (block.bytes().size() % 4) block.write_u8(0);
    auto sources = parsePsdLinkBlock(block.bytes());
    REQUIRE(sources.size() == 1);
    CHECK(sources[0].id == id);
    CHECK(sources[0].fileName == "small.png");
    CHECK(sources[0].fileType == "png ");
    CHECK(*sources[0].bytes == (std::vector<uint8_t>{1, 2, 3}));
    CHECK(parsePsdLinkBlock({0, 0, 0, 0, 0, 0, 0, 9, 'x'}).empty());   // damaged: nothing
}

TEST_CASE(an_instance_draws_its_source_and_survives_a_project) {
    Document doc = placed();
    auto out = renderFlattened(doc);
    CHECK_EQ(int(out->pixel(10, 5)[1]), 128);   // inside the placed quad
    CHECK_EQ(int(out->pixel(3, 5)[3]), 0);      // outside
    const auto package = std::filesystem::temp_directory_path() / "nekophoto-smartobject.comp";
    ProjectError error;
    REQUIRE(saveProject(doc, std::nullopt, package.string(), error));
    auto loaded = loadProject(package.string(), error);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->layers[0].isLiveSmartObject());
    CHECK(loaded->layers[0].smartObject->quad == doc.layers[0].smartObject->quad);
    REQUIRE(loaded->smartObjects.size() == 1);
    const auto& source = *loaded->smartObjects.begin()->second;
    CHECK(source.fileName == "small.png");
    CHECK(*source.bytes == *doc.smartObjects.begin()->second->bytes);
    CHECK(source.image && source.image->width() == 4);
    std::filesystem::remove_all(package);
}

TEST_CASE(export_writes_the_placement_where_the_layer_is) {
    Document doc = placed();
    doc.layers[0].transform.origin.x += 3;   // moved right
    PsdExportSummary summary;
    std::string error;
    auto bytes = encodePsd(doc, {}, &summary, &error);
    REQUIRE(!bytes.empty());
    CHECK_EQ(summary.smartObjects, 1);
    const std::string file(bytes.begin(), bytes.end());
    const size_t at = file.find("8BIMSoLd");
    REQUIRE(at != std::string::npos);
    const size_t length = size_t(uint8_t(file[at + 8])) << 24 | size_t(uint8_t(file[at + 9])) << 16 | size_t(uint8_t(file[at + 10])) << 8 | uint8_t(file[at + 11]);
    auto p = parsePsdPlacement("SoLd", std::vector<uint8_t>(bytes.begin() + long(at + 12), bytes.begin() + long(at + 12 + length)));
    REQUIRE(p.has_value());
    CHECK(near(p->quad, {9, 3, 17, 3, 17, 7, 9, 7}));
    // Painted on, it is plain pixels: no placed-layer block.
    doc.layers[0].asset = Asset::make(std::make_shared<Image>(4, 2), "Placed");
    auto painted = encodePsd(doc, {}, &summary, &error);
    CHECK(std::string(painted.begin(), painted.end()).find("8BIMSoLd") == std::string::npos);
}

TEST_CASE(image_size_scales_the_placement_not_the_pixels) {
    Document doc = placed();
    REQUIRE(resizeDocument(doc, 40, 20, 144, Sampling::High));
    REQUIRE(doc.layers[0].isLiveSmartObject());
    CHECK(doc.layers[0].asset->image->width() == 4);   // still the source
    CHECK(std::abs(doc.layers[0].transform.origin.x - 12) < 1e-9 && std::abs(doc.layers[0].transform.size.width - 16) < 1e-9);
}

namespace {
std::shared_ptr<Image> filled(int w, int h, uint8_t r, uint8_t g, uint8_t b) { auto i = std::make_shared<Image>(w, h); i->fill(r, g, b, 255); return i; }
SmartObjectContents pngContents(int w, int h, uint8_t g, const char* name) {
    SmartObjectContents c;
    c.image = filled(w, h, 0, g, 0);
    encodePngImage(*c.image, c.bytes);
    c.fileName = name;
    return c;
}
std::optional<PsdImport> throughPsd(const Document& doc) {
    std::string error;
    auto bytes = encodePsd(doc, {}, nullptr, &error);
    return importPsdBytes(bytes, &error);
}
} // namespace

TEST_CASE(convert_to_smart_object_and_back_through_psd) {
    Document doc(30, 20);
    doc.layers.push_back(Layer(Asset::make(filled(30, 20, 255, 255, 255), "Back"), Point(0, 0)));
    doc.layers.push_back(Layer(Asset::make(filled(6, 4, 200, 0, 0), "A"), Point(5, 5)));
    doc.layers.push_back(Layer(Asset::make(filled(4, 4, 0, 0, 200), "B"), Point(12, 8)));
    auto before = renderFlattened(doc);
    std::string error;
    auto id = convertToSmartObject(doc, {doc.layers[1].id, doc.layers[2].id}, &error);
    REQUIRE(id.has_value());
    REQUIRE(doc.layers.size() == 2);
    const Layer& so = doc.layers[1];
    CHECK(so.id == *id && so.name == "B" && so.isLiveSmartObject() && !so.smartObject->locked());
    CHECK(near(so.smartObject->quad, {5, 5, 16, 5, 16, 12, 5, 12}));   // the members' bounds
    // It looks the same.
    auto after = renderFlattened(doc);
    int diff = 0;
    for (int y = 0; y < 20; y++) for (int x = 0; x < 30; x++) for (int k = 0; k < 4; k++) diff = std::max(diff, std::abs(int(before->pixel(x, y)[k]) - int(after->pixel(x, y)[k])));
    CHECK(diff <= 1);
    // Through PSD it is still a smart object, and its contents open with both layers.
    auto back = throughPsd(doc);
    REQUIRE(back.has_value());
    REQUIRE(back->document.layers.size() == 2);
    const Layer& again = back->document.layers[1];
    REQUIRE(again.isLiveSmartObject());
    CHECK(!again.smartObject->locked());
    auto contents = smartObjectContentsDocument(back->document, again.smartObject->sourceId);
    REQUIRE(contents.has_value());
    CHECK_EQ(int(contents->layers.size()), 2);
    CHECK(contents->width == 11 && contents->height == 7);
}

TEST_CASE(place_replace_and_rasterize) {
    Document doc(40, 20);
    auto source = makeSmartObjectSource(pngContents(10, 10, 200, "logo.png"));
    REQUIRE(source);
    CHECK(source->fileType == "png ");
    const Uuid id = placeSmartObject(doc, source, 0, std::nullopt);
    REQUIRE(doc.layers.size() == 1);
    CHECK(doc.layers[0].id == id && doc.layers[0].name == "logo");
    CHECK(near(doc.layers[0].smartObject->quad, {15, 5, 25, 5, 25, 15, 15, 15}));   // 1:1, centred
    // Too large: scaled down to fit.
    auto big = makeSmartObjectSource(pngContents(80, 20, 100, "wide.png"));
    placeSmartObject(doc, big, 1, std::nullopt);
    CHECK(near(doc.layers[1].smartObject->quad, {0, 5, 40, 5, 40, 15, 0, 15}));
    // Scaled x2, then replaced by a 20 x 10: the instance keeps its centre and its x2.
    doc.layers[0].transform = LayerTransform(Point(10, 0), Size(20, 20));
    auto replacement = makeSmartObjectSource(pngContents(20, 10, 50, "badge.png"));
    CHECK_EQ(replaceSmartObjectSource(doc, source->id, replacement), 1);
    CHECK(doc.layers[0].name == "badge");
    CHECK(near(doc.layers[0].smartObject->quad, {0, 0, 40, 0, 40, 20, 0, 20}));
    CHECK(!doc.smartObjects.count(source->id) && doc.smartObjects.count(replacement->id));
    // Through PSD: both sources embedded anew, both instances placed.
    auto back = throughPsd(doc);
    REQUIRE(back.has_value());
    CHECK(back->document.layers[0].isLiveSmartObject() && back->document.layers[1].isLiveSmartObject());
    CHECK(near(back->document.layers[0].smartObject->quad, {0, 0, 40, 0, 40, 20, 0, 20}));
    rasterizeSmartObject(doc.layers[0]);
    CHECK(!doc.layers[0].smartObject && doc.layers[0].asset);
}

TEST_CASE(edited_contents_commit_to_every_instance) {
    Document doc(40, 20);
    auto source = makeSmartObjectSource(pngContents(10, 10, 200, "logo.png"));
    placeSmartObject(doc, source, 0, std::nullopt);
    doc.layers.push_back(doc.layers[0]);   // a duplicate: the same source
    doc.layers[1].id = makeUuid();
    doc.layers[1].transform.origin.x += 12;
    CHECK(smartObjectContentsEditable(doc, source->id));
    auto contents = smartObjectContentsDocument(doc, source->id);
    REQUIRE(contents.has_value());
    contents->layers[0].asset = Asset::make(filled(10, 10, 9, 9, 9), "logo");   // edited
    SmartObjectContents edited;
    edited.bytes = encodeSmartObjectContents(*contents, *source);
    REQUIRE(!edited.bytes.empty());
    edited.fileName = source->fileName;
    edited.fileType = source->fileType;
    edited.image = renderFlattened(*contents);
    CHECK_EQ(replaceSmartObjectSource(doc, source->id, makeSmartObjectSource(std::move(edited))), 2);
    CHECK_EQ(int(renderFlattened(doc)->pixel(20, 10)[0]), 9);
    CHECK_EQ(int(renderFlattened(doc)->pixel(32, 10)[0]), 9);
    // A locked instance refuses new contents.
    doc.layers[1].smartObject->lock = SmartObjectInstance::Lock::Warp;
    std::string why;
    CHECK(!smartObjectContentsEditable(doc, doc.layers[1].smartObject->sourceId, &why));
    CHECK(why.find("warped") != std::string::npos);
}

TEST_CASE(warp_meshes_bake_like_photoshop) {
    // Flat: the patch is the rectangle.
    const WarpMesh flat = identityWarpMesh(0, 0, 40, 20, 4, 4);
    const Point mid = evaluateWarpMesh(flat, 0.25, 0.5);
    CHECK(std::abs(mid.x - 10) < 1e-9 && std::abs(mid.y - 10) < 1e-9);
    // A preset at zero bend is flat; an arch lifts the top edge's middle, not its ends.
    CHECK(*styleWarpMesh("warpArch", 0, false, 40, 20) == identityWarpMesh(0, 0, 40, 20, 4, 2));
    const WarpMesh arch = *styleWarpMesh("warpArch", 50, false, 40, 20);
    CHECK(evaluateWarpMesh(arch, 0.5, 0).y < -1);
    CHECK(std::abs(evaluateWarpMesh(arch, 0, 0).y) < 1e-9);
    CHECK(!styleWarpMesh("warpNone", 50, false, 40, 20));
    // Vertical distortion widens the bottom row about its middle.
    WarpMesh d = flat;
    distortWarpMesh(d, 0, 50);
    CHECK(std::abs(d.xs[12] + 10) < 1e-9 && std::abs(d.xs[15] - 50) < 1e-9);
}

TEST_CASE(a_warped_instance_draws_its_contents_through_the_mesh) {
    // A 40 x 20 source, red on the left and blue on the right, arched on a 60 x 60 canvas.
    Document doc(60, 60);
    auto image = std::make_shared<Image>(40, 20);
    for (int y = 0; y < 20; y++) for (int x = 0; x < 40; x++) { uint8_t* p = image->pixel(x, y); p[0] = x < 20 ? 255 : 0; p[2] = x < 20 ? 0 : 255; p[3] = 255; }
    SmartObjectContents contents;
    contents.image = image;
    encodePngImage(*image, contents.bytes);
    contents.fileName = "art.png";
    auto source = makeSmartObjectSource(contents);
    const WarpMesh arch = *styleWarpMesh("warpArch", 50, false, 40, 20);
    const auto [x0, x1] = std::minmax_element(arch.xs.begin(), arch.xs.end());
    const auto [y0, y1] = std::minmax_element(arch.ys.begin(), arch.ys.end());
    // The hull 1:1 at (10, 15): the contents' (0, 0) lands at (10 - x0, 15 - y0).
    const double ox = 10 - *x0, oy = 15 - *y0;
    const std::array<double, 8> quad{10, 15, 10 + *x1 - *x0, 15, 10 + *x1 - *x0, 15 + *y1 - *y0, 10, 15 + *y1 - *y0};
    Layer layer = smartObjectLayer(source, quad, "Art");
    layer.smartObject->psdBlocks[0].data = *warpPsdPlacement("SoLd", layer.smartObject->psdBlocks[0].data, arch, quad);
    doc.smartObjects[source->id] = source;
    doc.layers.push_back(layer);
    std::string error;
    auto back = importPsdBytes(encodePsd(doc, {}, nullptr, &error), &error);
    REQUIRE(back.has_value());
    const Layer& warped = back->document.layers[0];
    REQUIRE(warped.isLiveSmartObject());
    CHECK(!warped.smartObject->locked());
    REQUIRE(smartObjectWarp(*warped.smartObject).has_value());
    CHECK(near(warped.smartObject->quad, quad));
    auto out = renderFlattened(back->document);
    auto at = [&](double u, double v) { const Point p = evaluateWarpMesh(arch, u, v); return out->pixel(int(p.x + ox), int(p.y + oy)); };
    CHECK(at(0.25, 0.5)[0] > 200 && at(0.25, 0.5)[2] < 50);   // red where the left half went
    CHECK(at(0.75, 0.5)[2] > 200 && at(0.75, 0.5)[0] < 50);   // blue on the right
    CHECK(at(0.5, 0.05)[3] == 255);                              // the lifted top edge is drawn
    CHECK(out->pixel(int(ox) + 20, int(oy) + 19)[3] == 0);       // and the arch left the old bottom middle
    // Moved: the quad goes along, the mesh stays as it was.
    Document moved = back->document;
    moved.layers[0].transform.origin.x += 5;
    auto again = importPsdBytes(encodePsd(moved, {}, nullptr, &error), &error);
    REQUIRE(again.has_value());
    auto shifted = quad;
    for (size_t i = 0; i < 8; i += 2) shifted[i] += 5;
    CHECK(near(again->document.layers[0].smartObject->quad, shifted));
    CHECK(*smartObjectWarp(*again->document.layers[0].smartObject) == arch);
    // Scaled x2 about its origin: drawn again from the contents at twice the size, not resampled.
    Document scaled = back->document;
    Layer& big = scaled.layers[0];
    const LayerTransform was = big.transform;
    big.transform.size = Size(was.size.width * 2, was.size.height * 2);
    CHECK_EQ(refreshSmartObjectRasters(scaled), 1);
    CHECK(big.isLiveSmartObject());
    CHECK(std::abs(big.asset->image->width() - 2 * was.size.width) <= 2);
    CHECK(big.transform.size.width == big.asset->image->width());   // 1:1 again
    CHECK_EQ(refreshSmartObjectRasters(scaled), 0);                  // nothing left to redraw
    auto scaledBack = importPsdBytes(encodePsd(scaled, {}, nullptr, &error), &error);
    REQUIRE(scaledBack.has_value());
    const auto& q = scaledBack->document.layers[0].smartObject->quad;
    CHECK(std::abs((q[2] - q[0]) - 2 * (quad[2] - quad[0])) < 1e-6);   // the written quad is twice as wide
    // Replaced by contents twice the size: the same cage, the mesh scaled onto them.
    auto twice = std::make_shared<Image>(80, 40);
    twice->fill(0, 255, 0, 255);
    SmartObjectContents green;
    green.image = twice;
    encodePngImage(*twice, green.bytes);
    green.fileName = "green.png";
    auto replacement = makeSmartObjectSource(green);
    CHECK_EQ(replaceSmartObjectSource(moved, source->id, replacement), 1);
    CHECK(near(moved.layers[0].smartObject->quad, shifted));
    CHECK(std::abs(smartObjectWarp(*moved.layers[0].smartObject)->xs[1] - arch.xs[1] * 2) < 1e-9);   // (arm64 fuses the multiply-add)
    auto green2 = renderFlattened(moved);
    const Point p = evaluateWarpMesh(arch, 0.5, 0.5);
    CHECK(green2->pixel(int(p.x + ox + 5), int(p.y + oy))[1] > 200);
}

namespace {
/// The SoLd with a one-entry Smart Filter stack: Gaussian Blur at `radius`.
std::vector<uint8_t> withGaussianBlur(const std::vector<uint8_t>& sold, double radius) {
    using V = psd::DescriptorValue;
    psd::BigEndianReader r(sold);
    (void)r.read_bytes(4);
    const uint32_t version = r.read_u32(), dv = r.read_u32();
    psd::DescriptorObject d = psd::read_descriptor(r);
    auto object = [](const char* cls, bool longForm) { V v; v.type = V::Type::Object; v.object_value = std::make_shared<psd::DescriptorObject>(); v.object_value->class_id = cls; v.object_value->class_id_long_form = longForm; return v; };
    auto add = [](V& o, const std::string& k, V v) { o.object_value->key_order.push_back({k, k.size() != 4}); o.object_value->values[k] = std::move(v); };
    auto boolean = [](bool b) { V v; v.type = V::Type::Bool; v.bool_value = b; return v; };
    auto unit = [](const char* u, double x) { V v; v.type = V::Type::UnitFloat; v.unit = u; v.double_value = x; return v; };
    auto integer = [](int32_t x) { V v; v.type = V::Type::Integer; v.integer_value = x; return v; };
    V root = object("filterFXStyle", true);
    add(root, "enab", boolean(true));
    add(root, "validAtPosition", boolean(true));
    add(root, "filterMaskEnable", boolean(true));
    add(root, "filterMaskLinked", boolean(false));
    add(root, "filterMaskExtendWithWhite", boolean(true));
    V entry = object("filterFX", true);
    add(entry, "Nm  ", text("Gaussian Blur..."));
    V blend = object("blendOptions", true);
    add(blend, "Opct", unit("#Prc", 100));
    V mode; mode.type = V::Type::Enum; mode.enum_type = "BlnM"; mode.enum_value = "normal"; mode.enum_value_long_form = true;
    add(blend, "Md  ", mode);
    add(entry, "blendOptions", blend);
    add(entry, "enab", boolean(true));
    add(entry, "hasoptions", boolean(true));
    for (const char* k : {"FrgC", "BckC"}) {
        V c = object("RGBC", false);
        for (const char* ch : {"Rd  ", "Grn ", "Bl  "}) add(c, ch, dbl(0));
        add(entry, k, c);
    }
    V filter = object("GsnB", false);
    add(filter, "Rds ", unit("#Pxl", radius));
    add(entry, "Fltr", filter);
    add(entry, "filterID", integer(0x47736e42));
    V list; list.type = V::Type::List; list.list_value.push_back(entry);
    add(root, "filterFXList", list);
    d.key_order.push_back({"filterFX", true});
    d.values["filterFX"] = root;
    psd::BigEndianWriter w;
    for (char c : std::string("soLD")) w.write_u8(uint8_t(c));
    w.write_u32(version);
    w.write_u32(dv);
    psd::write_descriptor(w, d);
    return w.bytes();
}
}

TEST_CASE(smart_filters_draw_from_the_contents_and_their_cache_follows) {
    // A 10 x 10 white square placed at (10, 10) on 30 x 30, blurred.
    Document doc(30, 30);
    auto source = makeSmartObjectSource(pngContents(10, 10, 255, "square.png"));
    doc.smartObjects[source->id] = source;
    const std::array<double, 8> quad{10, 10, 20, 10, 20, 20, 10, 20};
    Layer layer = smartObjectLayer(source, quad, "Square");
    layer.smartObject->psdBlocks[0].data = withGaussianBlur(layer.smartObject->psdBlocks[0].data, 2);
    auto carry = std::make_shared<PsdDocumentCarry>();
    carry->width = 30; carry->height = 30;
    PlacedRaster unfiltered{std::make_shared<Image>(*source->image), 10, 10};
    std::vector<uint8_t> feid{0, 0, 0, 3};
    const auto record = authorSmartFilterRecord(layer.smartObject->placedId, PixelRect{0, 0, 30, 30}, unfiltered, nullptr, {}, 255);
    for (int i = 7; i >= 0; i--) feid.push_back(uint8_t(uint64_t(record.size()) >> (8 * i)));
    feid.insert(feid.end(), record.begin(), record.end());
    while (feid.size() % 4) feid.push_back(0);
    carry->globals.push_back({"FEid", feid});
    doc.psdCarry = carry;
    doc.layers.push_back(layer);
    auto cache = findSmartFilterCache(carry->globals, layer.smartObject->placedId);
    REQUIRE(cache.has_value());
    CHECK(cache->canvas == (PixelRect{0, 0, 30, 30}));

    auto back = throughPsd(doc);
    REQUIRE(back.has_value());
    const Layer& blurred = back->document.layers[0];
    REQUIRE(blurred.isLiveSmartObject());
    CHECK(!blurred.smartObject->locked());
    auto out = renderFlattened(back->document);
    CHECK(out->pixel(15, 15)[3] >= 250);                          // the middle stays solid
    CHECK(out->pixel(9, 15)[3] > 0 && out->pixel(9, 15)[3] < 255);   // the edge spreads out
    CHECK(out->pixel(3, 15)[3] == 0);

    // Moved by 5: exported, its cache record is rewritten there (and the file still reads, drawn, not locked).
    Document moved = back->document;
    moved.layers[0].transform.origin.x += 5;
    auto again = throughPsd(moved);
    REQUIRE(again.has_value());
    CHECK(!again->document.layers[0].smartObject->locked());
    auto shifted = quad;
    for (size_t i = 0; i < 8; i += 2) shifted[i] += 5;
    CHECK(near(again->document.layers[0].smartObject->quad, shifted));
    auto out2 = renderFlattened(again->document);
    CHECK(out2->pixel(14, 15)[3] > 0 && out2->pixel(14, 15)[3] < 255);
    CHECK(out2->pixel(20, 15)[3] >= 250);

    // New contents: drawn through the same filter.
    auto red = makeSmartObjectSource(pngContents(10, 10, 0, "other.png"));
    CHECK_EQ(replaceSmartObjectSource(moved, source->id, red), 1);
    CHECK(!moved.layers[0].smartObject->locked());
    auto out3 = renderFlattened(moved);
    CHECK(out3->pixel(14, 15)[3] > 0 && out3->pixel(14, 15)[3] < 255);
}

TEST_CASE(warp_a_smart_object_and_pixels) {
    Document doc(120, 80);
    auto source = makeSmartObjectSource(pngContents(40, 20, 200, "banner.png"));
    doc.smartObjects[source->id] = source;
    Layer so = smartObjectLayer(source, {20, 20, 60, 20, 60, 40, 20, 40}, "Banner");
    doc.layers.push_back(so);
    std::string error;
    CHECK(warpLayer(doc, doc.layers[0], TextWarp{"warpArc", 50, 0, 0, false}, &error));
    const Layer& warped = doc.layers[0];
    REQUIRE(warped.isLiveSmartObject());
    CHECK(!warped.smartObject->locked());
    REQUIRE(smartObjectWarp(*warped.smartObject).has_value());
    CHECK(warped.asset->image->height() > 20);   // the arc lifts it
    // Twice is refused (a warp over a warp is not modelled here).
    CHECK(!warpLayer(doc, doc.layers[0], TextWarp{"warpFlag", 30, 0, 0, false}, &error));
    // Through PSD: still a warped, editable smart object.
    auto back = throughPsd(doc);
    REQUIRE(back.has_value());
    CHECK(back->document.layers[0].isLiveSmartObject() && !back->document.layers[0].smartObject->locked());
    CHECK(smartObjectWarp(*back->document.layers[0].smartObject).has_value());

    // Pixels are bent for good; an unknown style is refused.
    Layer pixels(Asset::make(filled(30, 10, 0, 0, 255), "Stripe"), Point(10, 50));
    doc.layers.push_back(pixels);
    CHECK(!warpLayer(doc, doc.layers[1], TextWarp{"warpSpiral", 50, 0, 0, false}, &error));
    CHECK(warpLayer(doc, doc.layers[1], TextWarp{"warpBulge", 50, 0, 0, false}, &error));
    CHECK(doc.layers[1].asset->image->height() > 10);
    CHECK(!doc.layers[1].smartObject);
}

TEST_CASE(add_smart_filters_to_a_smart_object) {
    Document doc(60, 40);
    auto source = makeSmartObjectSource(pngContents(20, 10, 255, "chip.png"));
    doc.smartObjects[source->id] = source;
    doc.layers.push_back(smartObjectLayer(source, {20, 15, 40, 15, 40, 25, 20, 25}, "Chip"));
    std::string error;
    SmartFilterEntry blur;
    blur.parameters = smartfilter::GaussianBlur{2};
    REQUIRE(addSmartFilter(doc, doc.layers[0], blur, &error));
    const Layer& once = doc.layers[0];
    CHECK(once.isLiveSmartObject() && !once.smartObject->locked());
    CHECK(smartObjectFiltered(*once.smartObject));
    REQUIRE(doc.psdCarry);
    CHECK(findSmartFilterCache(doc.psdCarry->globals, once.smartObject->placedId).has_value());
    auto out = renderFlattened(doc);
    CHECK(out->pixel(18, 20)[3] > 0 && out->pixel(18, 20)[3] < 255);   // blurred past its edge
    // A second filter goes on top of the first.
    SmartFilterEntry mosaic;
    mosaic.parameters = smartfilter::Mosaic{4};
    REQUIRE(addSmartFilter(doc, doc.layers[0], mosaic, &error));
    std::optional<SmartFilterStack> stack;
    for (auto& b : doc.layers[0].smartObject->psdBlocks) if ((stack = parseSmartFilterStack(b.key, b.data))) break;
    REQUIRE(stack.has_value());
    CHECK(stack->supported && stack->entries.size() == 2 && stack->entries[0].name == "Gaussian Blur..." && stack->entries[1].name == "Mosaic...");
    // Through PSD: still filtered and drawn, not locked.
    auto back = throughPsd(doc);
    REQUIRE(back.has_value());
    CHECK(back->document.layers[0].isLiveSmartObject() && !back->document.layers[0].smartObject->locked());
    CHECK(smartObjectFiltered(*back->document.layers[0].smartObject));
    // Not on pixels.
    doc.layers.push_back(Layer(Asset::make(filled(5, 5, 0, 0, 0), "Plain"), Point(0, 0)));
    CHECK(!addSmartFilter(doc, doc.layers[1], blur, &error));
}

TEST_MAIN()
