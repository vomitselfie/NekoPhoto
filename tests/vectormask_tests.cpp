// Vector masks ('vmsk'): parsing Photoshop's path records, antialiased coverage, the combine rules, mask
// parameters, strokes, and the path following its layer on export. Photoshop's own renders are checked over
// Patchy's fixtures (docs/vector-masks.md).
#include "check.h"
#include "compositor/psd.h"
#include "compositor/psd_carry.h"
#include "compositor/psd_writer.h"
#include "compositor/render.h"
#include "compositor/vectormask.h"

using namespace compositor;

namespace {

void put16(std::vector<uint8_t>& b, size_t at, uint16_t v) { b[at] = uint8_t(v >> 8); b[at + 1] = uint8_t(v); }
void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) { for (int i = 0; i < 4; i++) b[at + size_t(i)] = uint8_t(v >> (24 - 8 * i)); }

/// A 'vmsk' of rectangles (x0, y0, x1, y1 in document pixels on a `w` x `h` canvas), each its own shape group.
std::vector<uint8_t> vmsk(const std::vector<std::array<double, 4>>& rects, int w, int h, uint16_t op = 1, uint32_t flags = 0) {
    std::vector<uint8_t> b(8, 0);
    put32(b, 0, 3);
    put32(b, 4, flags);
    auto record = [&]() { b.resize(b.size() + 26, 0); return b.size() - 26; };
    put16(b, record(), 6);   // fill rule
    int group = 0;
    for (auto& r : rects) {
        size_t at = record();
        put16(b, at, 0); put16(b, at + 2, 4); put16(b, at + 4, op); put16(b, at + 6, 1); put32(b, at + 12, uint32_t(group++));
        const double pts[4][2] = {{r[0], r[1]}, {r[2], r[1]}, {r[2], r[3]}, {r[0], r[3]}};
        for (auto& p : pts) {
            at = record();
            put16(b, at, 2);
            const uint32_t y = uint32_t(int32_t(std::lround(p[1] / h * 16777216.0))), x = uint32_t(int32_t(std::lround(p[0] / w * 16777216.0)));
            for (size_t k = 2; k < 26; k += 8) { put32(b, at + k, y); put32(b, at + k + 4, x); }
        }
    }
    return b;
}

Layer shapeLayer(const std::vector<uint8_t>& path, int w, int h) {
    auto fill = std::make_shared<Image>(w, h);
    fill->fill(200, 0, 0, 255);
    Layer layer(Asset::make(fill, "Shape"), Point(0, 0));
    auto carry = std::make_shared<PsdLayerCarry>();
    carry->blocks.push_back({"vmsk", path});
    carry->placement = layer.transform;
    carry->contentHash = psdContentHash(fill.get());
    layer.psdCarry = carry;
    return layer;
}

} // namespace

TEST_CASE(a_rectangle_parses_and_covers_its_pixels) {
    auto path = parseVectorMask(vmsk({{4, 4, 12, 10}}, 20, 20), 20, 20);
    REQUIRE(path.has_value());
    REQUIRE(path->subpaths.size() == 1);
    CHECK(std::abs(path->subpaths[0].knots[2].x - 12) < 1e-4 && std::abs(path->subpaths[0].knots[2].y - 10) < 1e-4);
    auto m = rasterizeVectorMask(*path, Rect(0, 0, 20, 20), 1, 20, 20);
    CHECK_EQ(int(m->row(6)[6]), 255);
    CHECK_EQ(int(m->row(2)[6]), 0);
    CHECK_EQ(int(m->row(6)[12]), 0);
    // Half a pixel in: half covered.
    auto half = rasterizeVectorMask(*parseVectorMask(vmsk({{4.5, 4, 12, 10}}, 20, 20), 20, 20), Rect(0, 0, 20, 20), 1, 20, 20);
    CHECK(std::abs(int(half->row(6)[4]) - 128) <= 1);
    CHECK(!parseVectorMask({0, 0, 0, 3, 0, 0}, 20, 20).has_value());
}

TEST_CASE(groups_combine_in_order) {
    // Two overlapping squares: added, subtracted, intersected, excluded.
    const std::vector<std::array<double, 4>> rects{{2, 2, 10, 10}, {6, 6, 14, 14}};
    auto at = [&](uint16_t op, int x, int y) {
        auto b = vmsk(rects, 20, 20, 1);
        put16(b, 8 + 26 * 6 + 4, op);   // the second group's length record
        return int(rasterizeVectorMask(*parseVectorMask(b, 20, 20), Rect(0, 0, 20, 20), 1, 20, 20)->row(y)[x]);
    };
    CHECK_EQ(at(1, 12, 12), 255); CHECK_EQ(at(1, 4, 4), 255);   // add
    CHECK_EQ(at(2, 8, 8), 0); CHECK_EQ(at(2, 4, 4), 255); CHECK_EQ(at(2, 12, 12), 0);   // subtract
    CHECK_EQ(at(3, 8, 8), 255); CHECK_EQ(at(3, 4, 4), 0);   // intersect
    CHECK_EQ(at(0, 8, 8), 0); CHECK_EQ(at(0, 12, 12), 255);   // exclude
    // Inverted: the outside.
    CHECK_EQ(int(rasterizeVectorMask(*parseVectorMask(vmsk({{2, 2, 10, 10}}, 20, 20, 1, 1), 20, 20), Rect(0, 0, 20, 20), 1, 20, 20)->row(1)[1]), 255);
}

TEST_CASE(a_shape_layer_draws_its_path_and_it_follows_the_layer) {
    Document doc(20, 20);
    doc.layers.push_back(shapeLayer(vmsk({{4, 4, 12, 10}}, 20, 20), 20, 20));
    auto out = renderFlattened(doc);
    CHECK_EQ(int(out->pixel(6, 6)[3]), 255);
    CHECK_EQ(int(out->pixel(14, 14)[3]), 0);
    // Moved by (3, 2): the shape comes along, drawn and exported.
    doc.layers[0].transform.origin = Point(3, 2);
    out = renderFlattened(doc);
    CHECK_EQ(int(out->pixel(14, 11)[3]), 255);
    CHECK_EQ(int(out->pixel(5, 5)[3]), 0);
    std::string error;
    auto back = importPsdBytes(encodePsd(doc, {}, nullptr, &error), &error);
    REQUIRE(back.has_value());
    auto moved = layerVectorMask(back->document.layers[0], back->document);
    REQUIRE(moved.has_value());
    CHECK(std::abs(moved->subpaths[0].knots[0].x - 7) < 1e-3 && std::abs(moved->subpaths[0].knots[0].y - 6) < 1e-3);
}

TEST_CASE(mask_parameters_feather_and_density) {
    // A parameters-only section: flags 0x10, then vector density 128 (bit 2).
    std::vector<uint8_t> section(20, 0);
    section[17] = 0x10;
    section[18] = 0x04;
    section[19] = 128;
    auto p = parseMaskParameters(section);
    REQUIRE(p.has_value());
    CHECK(p->vectorDensity == 128 && !p->vectorFeather && !p->userDensity);
    GrayImage g(4, 1, 0);
    applyMaskParameters(g, p->vectorDensity, std::nullopt, 1);
    CHECK_EQ(int(g.row(0)[0]), 127);   // hidden ground shows at (255 - 128) / 255
    GrayImage edge(10, 1, 255);
    applyMaskParameters(edge, std::nullopt, 2.0, 1, true);
    CHECK_EQ(int(edge.row(0)[0]), 255);   // clamped at the canvas: an edge stays solid
    GrayImage open(10, 1, 255);
    applyMaskParameters(open, std::nullopt, 2.0, 1, false);
    CHECK(int(open.row(0)[0]) < 255);     // unclamped (a vector mask's feather): it fades
}

TEST_CASE(an_open_path_strokes_with_butt_ends) {
    VectorPath path;
    VectorPath::Subpath s;
    s.closed = false;
    s.knots = {{4, 10, 4, 10, 4, 10}, {16, 10, 16, 10, 16, 10}};
    path.subpaths.push_back(s);
    VectorStroke stroke;
    stroke.enabled = true;
    stroke.width = 4;
    auto band = rasterizeVectorStroke(path, stroke, Rect(0, 0, 20, 20), 1, 20, 20);
    CHECK_EQ(int(band->row(10)[10]), 255);   // on the line
    CHECK_EQ(int(band->row(14)[10]), 0);     // beyond half the width
    CHECK_EQ(int(band->row(10)[2]), 0);      // past the end: square, not round
    CHECK_EQ(int(band->row(15)[10]), 0);     // and not closed back on itself
}

TEST_CASE(corners_join_as_asked_and_dashes_break_the_line) {
    // A square's outside stroke: mitred corners are square, round ones rounded, bevelled ones cut.
    VectorPath path;
    VectorPath::Subpath s;
    for (auto [x, y] : {std::pair{6.0, 6.0}, {14.0, 6.0}, {14.0, 14.0}, {6.0, 14.0}}) s.knots.push_back({x, y, x, y, x, y});
    path.subpaths.push_back(s);
    VectorStroke stroke;
    stroke.enabled = true;
    stroke.width = 3;
    stroke.align = VectorStroke::Align::Outside;
    auto corner = [&](VectorStroke::Join join) {
        stroke.join = join;
        return int(rasterizeVectorStroke(path, stroke, Rect(0, 0, 20, 20), 1, 20, 20)->row(3)[3]);   // the outer corner pixel
    };
    CHECK_EQ(corner(VectorStroke::Join::Miter), 255);
    CHECK(corner(VectorStroke::Join::Round) < 128);
    CHECK(corner(VectorStroke::Join::Bevel) < 64);
    // Dashes of one width on, one off along the top edge.
    stroke.join = VectorStroke::Join::Miter;
    stroke.align = VectorStroke::Align::Center;
    stroke.width = 2;
    stroke.dashes = {1, 1};
    auto dashed = rasterizeVectorStroke(path, stroke, Rect(0, 0, 20, 20), 1, 20, 20);
    CHECK_EQ(int(dashed->row(6)[6]), 255);   // on
    CHECK_EQ(int(dashed->row(6)[8]), 0);     // off
    CHECK_EQ(int(dashed->row(6)[10]), 255);  // on again
}

TEST_MAIN()
