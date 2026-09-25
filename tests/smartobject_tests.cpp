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

TEST_MAIN()
