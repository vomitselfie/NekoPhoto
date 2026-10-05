// Artboards and slices: the Photoshop blocks both ways, the renderer's clip and background, and the round trip
// through PSD and the project package.
#include <algorithm>
#include "compositor/gif.h"
#include "compositor/depth.h"
#include "check.h"
#include "compositor/artboard.h"
#include "compositor/project.h"
#include "compositor/psd.h"
#include "compositor/psd_writer.h"
#include "compositor/render.h"
#include "compositor/animation.h"
#include "compositor/vectorlayer.h"
#include "compositor/vectormask.h"
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

TEST_CASE(artboard_background_stays_inside_in_every_blend_mode) {
    // Over a grey canvas, in every mode, isolated or Pass Through, the background paints inside the artboard's
    // rectangle only; inside, it goes through the folder with its layers, so the folder's mode and opacity apply
    // to it too (it used to be drawn beneath an isolated folder, at full strength in Normal).
    for (int m = 0; m < blendModeCount; m++) for (int passThrough = 0; passThrough < 2; passThrough++) for (double opacity : {1.0, 0.5}) {
        if (passThrough && m) continue;   // Pass Through has no mode of its own
        Document doc = artboardDocument();
        auto grey = std::make_shared<Image>(100, 60);
        grey->fill(128, 128, 128, 255);
        doc.layers.insert(doc.layers.begin(), Layer(Asset::make(grey, "Grey"), Point(0, 0)));
        for (Layer& l : doc.layers) if (l.artboard) { l.passThrough = passThrough; l.blendMode = BlendMode(m); l.opacity = opacity; }
        auto out = renderFlattened(doc);
        int outside = 0;
        for (int y = 0; y < 60; y++) for (int x = 0; x < 100; x++) {
            const bool inside = (x >= 10 && x < 50 && y >= 8 && y < 38) || (x >= 60 && x < 90 && y >= 10 && y < 30);
            const uint8_t* p = at(*out, x, y);
            if (!inside && (p[0] != 128 || p[1] != 128 || p[2] != 128 || p[3] != 255)) outside++;
        }
        if (outside) std::printf("  %s%s at %.1f: %d pixels outside changed\n", passThrough ? "Pass Through" : "", passThrough ? "" : blendModeName(BlendMode(m)), opacity, outside);
        CHECK_EQ(outside, 0);
        const uint8_t* bg = at(*out, 12, 10);   // red background over grey
        const BlendMode mode = BlendMode(m);
        if (mode == BlendMode::Normal || passThrough) {
            CHECK_NEAR(bg[0], opacity == 1 ? 255 : 191.5, 1.5);
            CHECK_NEAR(bg[1], opacity == 1 ? 0 : 64, 1.5);
            CHECK_NEAR(at(*out, 45, 25)[2], opacity == 1 ? 255 : 191.5, 1);   // the child, clipped at the edge
            CHECK_EQ(int(at(*out, 55, 25)[2]), 128);
        } else if (mode == BlendMode::Multiply) {
            CHECK_NEAR(bg[0], 128, 1.5);
            CHECK_NEAR(bg[1], opacity == 1 ? 0 : 64, 1.5);
        } else if (mode == BlendMode::Screen) {
            CHECK_NEAR(bg[0], opacity == 1 ? 255 : 191.5, 1.5);
            CHECK_NEAR(bg[1], 128, 1.5);
        }
    }
}

TEST_CASE(moving_an_artboard_carries_everything_inside) {
    // An artboard holding a folder with a mask of its own (following its transform) and a vector mask, and in it
    // a layer whose mask is placed and unlinked and one with a vector mask: moving the artboard with its contents
    // moves every one of them, so the picture inside moves rigidly.
    Document doc = artboardDocument();
    const Uuid boardId = doc.layers[1].id;
    Layer folder("Nested", doc.size());
    folder.isGroup = true;
    folder.parentId = boardId;
    auto hole = std::make_shared<GrayImage>(100, 60, 255);
    for (int y = 15; y < 25; y++) for (int x = 20; x < 30; x++) hole->row(y)[x] = 0;
    folder.mask = LayerMask{MaskAsset::make(hole), true, std::nullopt, true};
    auto green = std::make_shared<Image>(30, 20);
    green->fill(0, 255, 0, 255);
    Layer inner(Asset::make(green, "Green"), Point(15, 12));
    inner.parentId = folder.id;
    auto band = std::make_shared<GrayImage>(10, 30, 0);
    for (int y = 0; y < 30; y++) for (int x = 0; x < 5; x++) band->row(y)[x] = 255;
    inner.mask = LayerMask{MaskAsset::make(band), true, LayerTransform(Point(35, 10), Size(10, 30)), false};   // unlinked
    auto white = std::make_shared<Image>(6, 6);
    white->fill(255, 255, 255, 255);
    Layer shape(Asset::make(white, "Vector"), Point(12, 30));
    shape.parentId = folder.id;
    setLayerVectorMask(shape, doc, rectanglePath(Rect(13, 31, 3, 3)));
    setLayerVectorMask(folder, doc, rectanglePath(Rect(10, 8, 30, 25)));
    // Bottom to top: the blue child, the vector layer, the green layer, their folder, the artboard, the empty one.
    std::vector<Layer> layers = doc.layers;
    layers.insert(layers.begin() + 1, {shape, inner, folder});
    doc.layers = layers;
    std::string error;
    REQUIRE(validateHierarchy(doc.layers, &error));
    ensureAnimation(doc);
    duplicateFrame(doc, 0);
    auto before = renderFlattened(doc);
    const auto folderPath = layerVectorMask(*doc.find(folder.id), doc), shapePath = layerVectorMask(*doc.find(shape.id), doc);
    REQUIRE(folderPath && shapePath);

    std::set<Uuid> moving = descendantIds(doc.layers, boardId);
    moving.insert(boardId);
    CHECK_EQ(moving.size(), size_t(5));
    doc.find(boardId)->artboard->x += 7;
    doc.find(boardId)->artboard->y += 3;
    translateLayers(doc, moving, 7, 3);
    auto after = renderFlattened(doc);
    int differ = 0;
    for (int y = 8; y < 38; y++) for (int x = 10; x < 50; x++) if (std::memcmp(before->pixel(x, y), after->pixel(x + 7, y + 3), 4)) differ++;
    CHECK_EQ(differ, 0);
    CHECK(doc.find(folder.id)->transform.origin == Point(7, 3));
    CHECK(doc.find(inner.id)->mask->placement->origin == Point(42, 13));
    auto movedPath = layerVectorMask(*doc.find(folder.id), doc);
    REQUIRE(movedPath);
    CHECK_NEAR(movedPath->subpaths[0].knots[0].x, folderPath->subpaths[0].knots[0].x + 7, 1e-3);
    CHECK_NEAR(movedPath->subpaths[0].knots[0].y, folderPath->subpaths[0].knots[0].y + 3, 1e-3);
    auto movedShape = layerVectorMask(*doc.find(shape.id), doc);
    REQUIRE(movedShape);
    CHECK_NEAR(movedShape->subpaths[0].knots[0].x, shapePath->subpaths[0].knots[0].x + 7, 1e-3);
    // Every frame keeps the moved positions, so showing another frame does not pull the contents back.
    for (const AnimationFrame& f : doc.animation.frames) CHECK(f.layers.at(inner.id).position == Point(22, 15));
    CHECK(selectFrame(doc, 0));
    CHECK(doc.find(inner.id)->transform.origin == Point(22, 15));
    // The other artboard stays.
    CHECK(doc.layers.back().artboard->x == 60);
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
    { std::error_code cleanup_; std::filesystem::remove_all(dir, cleanup_); }
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
    in.close();   // Windows will not delete a file that is still open
    CHECK(text.str().find("\"version\": 8") != std::string::npos);
    CHECK(!doc.fitsMacBudget());
    // Without them the Mac's version stays.
    Document plain(10, 10);
    CHECK(manifestJson(plain, std::nullopt).find("\"version\": 7") != std::string::npos);
    { std::error_code cleanup_; std::filesystem::remove_all(dir, cleanup_); }
}

TEST_CASE(artboards_and_frames_at_sixteen_bits) {
    // The artboard document converted to 16 bits: backgrounds and clipping drawn by the 16-bit renderer, within a
    // level of the 8-bit render (they are exact: flat backgrounds and an opaque child).
    Document doc = artboardDocument();
    Document deep = doc;
    std::string error;
    REQUIRE(convertSampleType(deep, SampleType::U16, &error));
    auto eight = renderFlattened(doc);
    auto sixteen = narrowImage(*renderFlattened16(deep));
    int worst = 0;
    for (int y = 0; y < eight->height(); y++)
        for (int i = 0; i < eight->width() * 4; i++) worst = std::max(worst, std::abs(int(eight->row(y)[i]) - int(sixteen->row(y)[i])));
    CHECK_EQ(worst, 0);
    CHECK_EQ(int(at(*sixteen, 55, 25)[3]), 0);   // the child past the edge is clipped at 16 bits too
    // Two frames (the blue child shown, then hidden), rendered at 16 bits and written as a GIF dithered to 8 bits.
    ensureAnimation(deep);
    REQUIRE(duplicateFrame(deep, 0));
    const Uuid child = deep.layers[0].id;
    deep.animation.frames[1].layers[child].visible = false;
    const std::vector<uint8_t> gif = encodeDocumentGif(deep, &error);
    REQUIRE(!gif.empty());
    CHECK_EQ(gifFrameCount(gif), 2);
    auto imported = importGifBytes(gif, &error);
    REQUIRE(imported && imported->document.layers.size() == 2);
    const Image& shown = *imported->document.layers[0].asset->image.u8();
    const Image& hidden = *imported->document.layers[1].asset->image.u8();
    CHECK_EQ(int(shown.pixel(45, 25)[2]), 255);    // the child in the first frame
    CHECK_EQ(int(hidden.pixel(45, 25)[0]), 255);   // the red background in the second
}

TEST_MAIN()

TEST_CASE(guides_resource_and_round_trips) {
    // Resource 1032: version 1, the grid words, the count, then 1/32-pixel positions and a direction byte.
    std::vector<Guide> guides = {{Guide::Orientation::Vertical, 12.5}, {Guide::Orientation::Horizontal, -3.25}, {Guide::Orientation::Vertical, 99}};
    auto bytes = guidesResource(guides, std::pair<uint32_t, uint32_t>{640, 320});
    REQUIRE(bytes.size() == 16 + 3 * 5);
    CHECK_EQ(int(bytes[19]), 400 & 0xff);   // 12.5 * 32 = 400, big-endian
    CHECK_EQ(int(bytes[20]), 0);
    CHECK_EQ(int(bytes[25]), 1);
    std::vector<Guide> back;
    std::optional<std::pair<uint32_t, uint32_t>> grid;
    REQUIRE(parseGuidesResource(bytes, back, &grid));
    CHECK(back == guides);
    REQUIRE(grid);
    CHECK_EQ(grid->first, 640u);
    CHECK(!parseGuidesResource({0, 0, 0, 2}, back));
    CHECK_NEAR(guidePosition(1.0 / 7), 5.0 / 32, 0);

    // Through a PSD: the guides come back; unchanged, the file's resource (with its grid) is written as it was.
    Document doc(64, 48);
    auto red = std::make_shared<Image>(64, 48);
    red->fill(255, 0, 0, 255);
    doc.layers = {Layer(Asset::make(red, "Background"), Point(0, 0))};
    doc.guides = guides;
    std::string error;
    auto psdBytes = encodePsd(doc, {}, nullptr, &error);
    auto imported = importPsdBytes(psdBytes, &error);
    REQUIRE(imported);
    CHECK(imported->document.guides == guides);
    auto again = encodePsd(imported->document, {}, nullptr, &error);
    CHECK(again == encodePsd(imported->document, {}, nullptr, &error));
    CHECK_EQ(again.size(), psdBytes.size());
    Document moved = imported->document;
    moved.guides[0].position = 20;
    moved.guides.pop_back();
    auto third = importPsdBytes(encodePsd(moved, {}, nullptr, &error), &error);
    REQUIRE(third);
    CHECK(third->document.guides == moved.guides);

    // Through a project package.
    const auto dir = std::filesystem::temp_directory_path() / "nekophoto-guide-tests";
    { std::error_code cleanup_; std::filesystem::remove_all(dir, cleanup_); }
    std::filesystem::create_directories(dir);
    const std::string package = (dir / "Guides.comp").string();
    ProjectError projectError;
    REQUIRE(saveProject(doc, std::nullopt, package, projectError));
    auto loaded = loadProject(package, projectError);
    REQUIRE(loaded);
    CHECK(loaded->guides == guides);
    { std::error_code cleanup_; std::filesystem::remove_all(dir, cleanup_); }

    // The canvas changes carry them.
    std::vector<Guide> g = {{Guide::Orientation::Vertical, 10}, {Guide::Orientation::Horizontal, 4}};
    offsetGuides(g, -2, 3);
    CHECK_NEAR(g[0].position, 8, 0);
    CHECK_NEAR(g[1].position, 7, 0);
    scaleGuides(g, 2, 0.5);
    CHECK_NEAR(g[0].position, 16, 0);
    CHECK_NEAR(g[1].position, 3.5, 0);
    flipGuides(g, true, 64);
    CHECK_NEAR(g[0].position, 48, 0);
    CHECK_NEAR(g[1].position, 3.5, 0);
}
