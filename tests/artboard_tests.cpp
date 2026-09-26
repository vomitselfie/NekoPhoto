// Artboards and slices: the Photoshop blocks both ways, the renderer's clip and background, and the round trip
// through PSD and the project package.
#include "check.h"
#include "compositor/artboard.h"
#include "compositor/project.h"
#include "compositor/psd.h"
#include "compositor/psd_writer.h"
#include "compositor/render.h"
#include "psd/psd_descriptor.hpp"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

using namespace compositor;
namespace psd = patchy::psd;

namespace {

Artboard sampleArtboard() {
    Artboard a;
    a.x = 10; a.y = 8; a.width = 40; a.height = 30;
    a.background = Artboard::Other;
    a.red = 1; a.green = 0; a.blue = 0;
    a.presetName = "Custom";
    return a;
}

Document artboardDocument() {
    Document doc(100, 60);
    Layer board("Artboard 1", doc.size());
    board.isGroup = true;
    board.artboard = sampleArtboard();
    auto blue = std::make_shared<Image>(20, 20);
    blue->fill(0, 0, 255, 255);
    Layer child(Asset::make(blue, "Blue"), Point(40, 20));   // reaches past the artboard's right edge (50)
    child.parentId = board.id;
    Layer empty("Artboard 2", doc.size());
    empty.isGroup = true;
    Artboard b; b.x = 60; b.y = 10; b.width = 30; b.height = 20; b.background = Artboard::Black;
    empty.artboard = b;
    doc.layers = {child, board, empty};
    Slice s; s.id = 3; s.name = "hero"; s.x = 5; s.y = 6; s.width = 20; s.height = 10; s.url = "https://example.com"; s.altTag = "Hero";
    doc.slices = {s};
    return doc;
}

const uint8_t* at(const Image& image, int x, int y) { return image.pixel(x, y); }

} // namespace

TEST_CASE(artboard_block_round_trip) {
    const Artboard a = sampleArtboard();
    auto bytes = artboardBlock(a);
    auto back = parseArtboardBlock(bytes);
    REQUIRE(back);
    CHECK(*back == a);
    // Photoshop's shape: version 16, class "artboard", the rectangle as classFloatRect.
    psd::BigEndianReader r(bytes);
    CHECK_EQ(r.read_u32(), 16u);
    auto d = psd::read_descriptor(r);
    CHECK_EQ(d.class_id, std::string("artboard"));
    auto rect = psd::descriptor_object(d, "artboardRect");
    REQUIRE(rect);
    CHECK_EQ(rect->class_id, std::string("classFloatRect"));
    CHECK_NEAR(psd::descriptor_number(*rect, "Rght"), 50, 0);
    CHECK_NEAR(psd::descriptor_number(d, "artboardBackgroundType"), 4, 0);
    CHECK(!parseArtboardBlock({1, 2, 3}));
}

TEST_CASE(slices_resource_round_trip) {
    std::vector<Slice> slices(2);
    slices[0].id = 1; slices[0].name = "one"; slices[0].x = 1; slices[0].y = 2; slices[0].width = 3; slices[0].height = 4;
    slices[1].id = 7; slices[1].name = "zwei \xc3\xa4"; slices[1].x = 10; slices[1].y = 20; slices[1].width = 30; slices[1].height = 40;
    slices[1].url = "u"; slices[1].target = "_blank"; slices[1].message = "m"; slices[1].altTag = "a";
    auto bytes = slicesResource(slices, 100, 80, "doc");
    std::vector<Slice> back;
    std::string group;
    REQUIRE(parseSlicesResource(bytes, back, &group));
    CHECK(back == slices);
    CHECK_EQ(group, std::string("doc"));
    CHECK_EQ(nextSliceId(back), 8u);

    // The descriptor form (versions 7 and 8), as newer Photoshop writes it.
    auto num = [](int v) { psd::DescriptorValue d; d.type = psd::DescriptorValue::Type::Integer; d.integer_value = v; return d; };
    auto text = [](const std::string& s) { psd::DescriptorValue d; d.type = psd::DescriptorValue::Type::String; d.string_value = s; return d; };
    psd::DescriptorObject bounds; bounds.class_id = "Rct1";
    bounds.values["Top "] = num(5); bounds.values["Left"] = num(6); bounds.values["Btom"] = num(25); bounds.values["Rght"] = num(46);
    psd::DescriptorObject slice; slice.class_id = "slice";
    slice.values["sliceID"] = num(4);
    psd::DescriptorValue origin; origin.type = psd::DescriptorValue::Type::Enum; origin.enum_type = "ESliceOrigin"; origin.enum_value = "userGenerated";
    slice.values["origin"] = origin;
    slice.values["Nm  "] = text("desc");
    slice.values["url"] = text("x");
    psd::DescriptorValue b; b.type = psd::DescriptorValue::Type::Object; b.object_value = std::make_shared<psd::DescriptorObject>(bounds);
    slice.values["bounds"] = b;
    psd::DescriptorObject top; top.class_id = "null";
    top.values["baseName"] = text("base");
    psd::DescriptorValue list; list.type = psd::DescriptorValue::Type::List;
    psd::DescriptorValue item; item.type = psd::DescriptorValue::Type::Object; item.object_value = std::make_shared<psd::DescriptorObject>(slice);
    list.list_value.push_back(item);
    top.values["slices"] = list;
    psd::BigEndianWriter w;
    w.write_u32(7); w.write_u32(16);
    psd::write_descriptor(w, top);
    REQUIRE(parseSlicesResource(w.bytes(), back, &group));
    REQUIRE(back.size() == 1);
    CHECK_EQ(back[0].id, 4u);
    CHECK_EQ(back[0].name, std::string("desc"));
    CHECK_EQ(back[0].x, 6); CHECK_EQ(back[0].y, 5); CHECK_EQ(back[0].width, 40); CHECK_EQ(back[0].height, 20);
    CHECK_EQ(group, std::string("base"));
}

TEST_CASE(artboards_render_background_and_clip) {
    const Document doc = artboardDocument();
    auto out = renderFlattened(doc);
    CHECK_EQ(int(at(*out, 5, 5)[3]), 0);             // outside every artboard
    CHECK_EQ(int(at(*out, 12, 10)[0]), 255);          // the red background
    CHECK_EQ(int(at(*out, 12, 10)[3]), 255);
    CHECK_EQ(int(at(*out, 45, 25)[2]), 255);          // the child inside
    CHECK_EQ(int(at(*out, 45, 25)[0]), 0);
    CHECK_EQ(int(at(*out, 55, 25)[3]), 0);            // the child past the edge is clipped
    CHECK_EQ(int(at(*out, 70, 15)[3]), 255);          // the empty black artboard
    CHECK_EQ(int(at(*out, 70, 15)[0]), 0);
    // A hidden artboard draws nothing.
    Document hidden = doc;
    hidden.layers[2].visible = false;
    auto h = renderFlattened(hidden);
    CHECK_EQ(int(at(*h, 70, 15)[3]), 0);
}

TEST_CASE(artboards_slices_psd_round_trip) {
    const Document doc = artboardDocument();
    std::string error;
    auto bytes = encodePsd(doc, {}, nullptr, &error);
    REQUIRE(!bytes.empty());
    auto imported = importPsdBytes(bytes, &error);
    REQUIRE(imported);
    const Document& back = imported->document;
    int boards = 0;
    for (const Layer& l : back.layers) {
        if (!l.artboard) continue;
        boards++;
        CHECK(l.isGroup);
        if (l.name == "Artboard 1") CHECK(*l.artboard == sampleArtboard());
    }
    CHECK_EQ(boards, 2);
    CHECK(back.slices == doc.slices);
    // Written again unchanged, the file's own blocks go back as they were; changed, anew.
    auto again = encodePsd(back, {}, nullptr, &error);
    CHECK_EQ(again.size(), bytes.size());
    Document moved = back;
    for (Layer& l : moved.layers) if (l.artboard && l.name == "Artboard 1") l.artboard->x = 0;
    moved.slices[0].name = "renamed";
    auto third = importPsdBytes(encodePsd(moved, {}, nullptr, &error), &error);
    REQUIRE(third);
    bool found = false;
    for (const Layer& l : third->document.layers) if (l.artboard && l.name == "Artboard 1") { found = true; CHECK_EQ(l.artboard->x, 0); }
    CHECK(found);
    REQUIRE(third->document.slices.size() == 1);
    CHECK_EQ(third->document.slices[0].name, std::string("renamed"));
    // Rendered the same after the trip.
    auto a = renderFlattened(doc), b = renderFlattened(back);
    CHECK(std::memcmp(a->data(), b->data(), a->byteCount()) == 0);
}

TEST_CASE(artboards_slices_project_round_trip) {
    const Document doc = artboardDocument();
    const auto dir = std::filesystem::temp_directory_path() / "nekophoto-artboard-tests";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string package = (dir / "Boards.comp").string();
    ProjectError error;
    REQUIRE(saveProject(doc, std::nullopt, package, error));
    auto loaded = loadProject(package, error);
    REQUIRE(loaded);
    CHECK(loaded->slices == doc.slices);
    int boards = 0;
    for (const Layer& l : loaded->layers) if (l.artboard) { boards++; if (l.name == "Artboard 1") CHECK(*l.artboard == sampleArtboard()); }
    CHECK_EQ(boards, 2);
    // Version 8: the Mac app has no artboards.
    std::ifstream in(std::filesystem::path(package) / "manifest.json");
    std::stringstream text; text << in.rdbuf();
    CHECK(text.str().find("\"version\": 8") != std::string::npos);
    CHECK(!doc.fitsMacBudget());
    // Without them the Mac's version stays.
    Document plain(10, 10);
    CHECK(manifestJson(plain, std::nullopt).find("\"version\": 7") != std::string::npos);
    std::filesystem::remove_all(dir);
}

TEST_MAIN()
