// Vector masks ('vmsk'): parsing Photoshop's path records, antialiased coverage, the combine rules, mask
// parameters, strokes, and the path following its layer on export. Photoshop's own renders are checked over
// Patchy's fixtures (docs/vector-masks.md), as are the feathers of pixel masks, vector masks and stroked shapes.
#include "check.h"
#include "compositor/psd.h"
#include "compositor/psd_carry.h"
#include "compositor/psd_writer.h"
#include "compositor/render.h"
#include "compositor/vectorlayer.h"
#include "compositor/vectormask.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

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

namespace {

/// A parameters-only mask section with a vector feather (and density).
std::vector<uint8_t> vectorParameters(double feather, std::optional<int> density = std::nullopt) {
    std::vector<uint8_t> s(18, 0);
    s[17] = 0x10;
    s.push_back(uint8_t(0x08 | (density ? 0x04 : 0)));
    if (density) s.push_back(uint8_t(*density));
    uint64_t u;
    std::memcpy(&u, &feather, 8);
    for (int i = 7; i >= 0; i--) s.push_back(uint8_t(u >> (8 * i)));
    return s;
}

/// A shape layer of a 20 x 20 square (red fill, a 4-pixel dark stroke inside when `stroked`) feathered by 3.
Document featheredSquare(bool stroked) {
    Document doc(40, 40);
    VectorShape shape;
    shape.path = rectanglePath(Rect(10, 10, 20, 20), 0);
    shape.r = 200;
    shape.stroke.enabled = stroked;
    shape.stroke.width = 4;
    shape.stroke.align = VectorStroke::Align::Inside;
    shape.stroke.r = shape.stroke.g = shape.stroke.b = 20;
    Layer layer("Shape", Size(1, 1));
    setVectorShape(layer, doc, shape);
    auto carry = std::make_shared<PsdLayerCarry>(*layer.psdCarry);
    carry->maskData = vectorParameters(3);
    layer.psdCarry = carry;
    doc.layers.push_back(layer);
    return doc;
}

std::string fixture(const char* name) {
    const char* dir = std::getenv("PATCHY_FIXTURES");
    return std::string(dir ? dir : PATCHY_FIXTURES) + "/" + name;
}

/// A 24-bit BMP as straight RGB rows; empty when it cannot be read.
std::vector<uint8_t> readBmp24(const std::string& path, int& w, int& h) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> f((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (f.size() < 54 || f[0] != 'B' || f[1] != 'M') return {};
    auto u32 = [&](size_t at) { return uint32_t(f[at]) | uint32_t(f[at + 1]) << 8 | uint32_t(f[at + 2]) << 16 | uint32_t(f[at + 3]) << 24; };
    const uint32_t offset = u32(10);
    w = int(int32_t(u32(18)));
    const int rawH = int(int32_t(u32(22)));
    h = std::abs(rawH);
    if (f[28] != 24 || w <= 0 || h <= 0) return {};
    const size_t stride = (size_t(w) * 3 + 3) / 4 * 4;
    if (f.size() < offset + stride * size_t(h)) return {};
    std::vector<uint8_t> rgb(size_t(w) * size_t(h) * 3);
    for (int y = 0; y < h; y++) {
        const uint8_t* row = f.data() + offset + stride * size_t(rawH > 0 ? h - 1 - y : y);
        for (int x = 0; x < w; x++) for (int k = 0; k < 3; k++) rgb[(size_t(y) * size_t(w) + size_t(x)) * 3 + size_t(k)] = row[x * 3 + 2 - k];
    }
    return rgb;
}

} // namespace

TEST_CASE(a_stroked_shape_feathers_as_one_drawing) {
    // Photoshop feathers the drawn shape, fill and stroke together: past the edge only one silhouette fades, so the
    // stroked square's fringe is exactly as opaque as the plain one's (it was about twice as opaque when the stroke
    // and the fill were feathered apart and laid one over the other).
    const auto plain = renderFlattened(featheredSquare(false));
    const auto stroked = renderFlattened(featheredSquare(true));
    for (int x : {5, 7, 9, 11}) CHECK_NEAR(int(stroked->pixel(x, 20)[3]), int(plain->pixel(x, 20)[3]), 1);
    CHECK(stroked->pixel(7, 20)[3] > 0 && stroked->pixel(7, 20)[3] < 128);
    // Inside, the stroke is dark and the middle red, softened.
    CHECK(stroked->pixel(13, 20)[0] < 120);
    CHECK(stroked->pixel(20, 20)[0] > 150);
    // At 16 bits, the same shape within a level.
    Document deep = featheredSquare(true);
    REQUIRE(convertSampleType(deep, SampleType::U16));
    const auto deep8 = renderFlattened(deep);
    for (int x = 0; x < 40; x++) for (int k = 0; k < 4; k++) CHECK_NEAR(int(deep8->pixel(x, 20)[k]), int(stroked->pixel(x, 20)[k]), 1);
}

TEST_CASE(feathered_masks_match_photoshops_renders) {
    // Patchy's fixtures, against Photoshop's flatten: a pixel mask larger than its layer feathered (Photoshop blurs the
    // mask's own plane, so the layer's sides stay unfaded) with density, vector masks feathered by 4 and 8, and shapes
    // feathered with a stroke (one drawing feathered) and with density.
    struct Case { const char* name; int maxError; double maxMean; };
    for (const Case& c : {Case{"photoshop-user-mask-params", 2, 0.35}, Case{"photoshop-vector-mask-feather", 3, 0.45}, Case{"photoshop-shape-feather", 3, 0.45}}) {
        std::string error;
        auto imported = importPsd(fixture((std::string(c.name) + ".psd").c_str()), &error);
        int w = 0, h = 0;
        const auto ps = readBmp24(fixture((std::string(c.name) + ".bmp").c_str()), w, h);
        if (!imported || ps.empty()) { std::printf("  skipped: Patchy's fixtures are not beside this checkout\n"); return; }
        const auto ours = renderFlattened(imported->document);
        REQUIRE(ours->width() == w && ours->height() == h);
        double sum = 0;
        int worst = 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                for (int k = 0; k < 3; k++) {
                    const int mine = int(ours->row(y)[x * 4 + k]) + 255 - int(ours->row(y)[x * 4 + 3]);
                    const int d = std::abs(mine - int(ps[(size_t(y) * size_t(w) + size_t(x)) * 3 + size_t(k)]));
                    sum += d;
                    worst = std::max(worst, d);
                }
        std::printf("  %s against Photoshop's render: mean %.3f, max %d levels\n", c.name, sum / (w * h * 3.0), worst);
        CHECK(worst <= c.maxError);
        CHECK(sum / (w * h * 3.0) < c.maxMean);
    }
}

TEST_MAIN()
