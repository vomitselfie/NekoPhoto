// Vector shape layers made here (vectorlayer.h): the authored blocks read back through the parsers the renderer
// and the PSD round trip use, and the layer draws its shape.
#include "check.h"
#include "compositor/render.h"
#include "compositor/vectorlayer.h"
#include <cmath>

using namespace compositor;

namespace {
Document canvas(int w, int h) {
    Document doc(w, h);
    auto white = std::make_shared<Image>(w, h);
    white->fill(255, 255, 255, 255);
    doc.layers.push_back(Layer(Asset::make(white, "Background"), Point(0, 0)));
    return doc;
}
}

TEST_CASE(authored_paths_read_back) {
    const VectorPath path = ellipsePath(Rect(10.25, 20.5, 100, 60));
    auto bytes = authorVectorMask(path, 300, 200);
    auto back = parseVectorMask(bytes, 300, 200);
    REQUIRE(back.has_value());
    REQUIRE(back->subpaths.size() == 1);
    REQUIRE(back->subpaths[0].knots.size() == 4);
    for (size_t i = 0; i < 4; i++) {
        const auto& a = path.subpaths[0].knots[i];
        const auto& b = back->subpaths[0].knots[i];
        CHECK(std::abs(a.x - b.x) < 1e-3 && std::abs(a.y - b.y) < 1e-3 && std::abs(a.outX - b.outX) < 1e-3 && std::abs(a.inY - b.inY) < 1e-3);
        CHECK(knotIsSmooth(b));
    }
    CHECK(!knotIsSmooth(rectanglePath(Rect(0, 0, 10, 10)).subpaths[0].knots[0]));
}

TEST_CASE(a_shape_layer_draws_its_fill_and_stroke_and_reads_back) {
    Document doc = canvas(100, 100);
    VectorShape shape;
    shape.path = rectanglePath(Rect(20, 20, 60, 40));
    shape.r = 200; shape.g = 30; shape.b = 30;
    shape.stroke.enabled = true;
    shape.stroke.width = 4;
    shape.stroke.align = VectorStroke::Align::Outside;
    shape.stroke.r = 0; shape.stroke.g = 0; shape.stroke.b = 255;
    Layer layer(Asset::make(std::make_shared<Image>(1, 1), "Shape"), Point(0, 0));
    setVectorShape(layer, doc, shape);
    doc.layers.push_back(layer);
    REQUIRE(isVectorShapeLayer(doc.layers[1]));
    auto out = renderFlattened(doc);
    const uint8_t* inside = out->pixel(50, 40);
    CHECK_EQ(int(inside[0]), 200); CHECK_EQ(int(inside[2]), 30);
    const uint8_t* band = out->pixel(50, 18);   // 2 px outside the top edge: the stroke
    CHECK(band[2] > 200 && band[0] < 60);
    CHECK_EQ(int(out->pixel(50, 10)[0]), 255);   // beyond it, the background
    auto back = vectorShapeOf(doc.layers[1], doc);
    REQUIRE(back.has_value());
    CHECK(back->fill && back->stroke.enabled && std::abs(back->stroke.width - 4) < 1e-9 && back->stroke.align == VectorStroke::Align::Outside);
    CHECK_EQ(int(back->r), 200); CHECK_EQ(int(back->stroke.b), 255);
    // Editing it again (moved and recoloured) keeps it a shape.
    back->path = rectanglePath(Rect(5, 5, 20, 20));
    back->fill = false;
    setVectorShape(doc.layers[1], doc, *back);
    out = renderFlattened(doc);
    CHECK_EQ(int(out->pixel(50, 40)[0]), 255);                 // the old place is empty
    CHECK(out->pixel(15, 5)[2] > 200 && out->pixel(15, 15)[1] == 255);   // stroke only: the inside shows through
}

TEST_CASE(shape_builders_fit_their_box) {
    const Rect box(10, 10, 80, 40);
    for (const VectorPath& p : {rectanglePath(box, 8), ellipsePath(box), polygonPath(box, 6), polygonPath(box, 5, 0.5)}) {
        const Rect b = pathBounds(p);
        CHECK(b.x >= box.x - 1e-6 && b.maxX() <= box.maxX() + 1e-6 && b.y >= box.y - 1e-6 && b.maxY() <= box.maxY() + 1e-6);
    }
    for (const auto& name : customShapeNames()) CHECK(!customShapePath(name, box).subpaths.empty());
    const Rect line = pathBounds(linePath({0, 0}, {100, 0}, 6));
    CHECK(std::abs(line.height - 6) < 1e-9 && std::abs(line.width - 100) < 1e-9);
}

TEST_CASE(document_paths_are_kept_as_photoshop_keeps_them) {
    Document doc = canvas(200, 100);
    CHECK(documentPaths(doc).empty());
    const uint16_t work = setDocumentPath(doc, kWorkPathId, "", ellipsePath(Rect(10, 10, 50, 40)));
    CHECK_EQ(int(work), 1025);
    const uint16_t saved = setDocumentPath(doc, 0, "Outline", rectanglePath(Rect(100, 20, 60, 60)));
    CHECK_EQ(int(saved), 2000);
    CHECK_EQ(int(setDocumentPath(doc, 0, "Second", rectanglePath(Rect(0, 0, 5, 5)))), 2001);
    auto paths = documentPaths(doc);
    REQUIRE(paths.size() == 3);
    CHECK(paths[0].id == kWorkPathId && paths[0].name == "Work Path");
    CHECK(paths[1].name == "Outline");
    CHECK(std::abs(pathBounds(paths[1].path).x - 100) < 1e-3);
    renameDocumentPath(doc, saved, "Renamed");
    removeDocumentPath(doc, 2001);
    paths = documentPaths(doc);
    REQUIRE(paths.size() == 2);
    CHECK(paths[1].name == "Renamed");
}

TEST_CASE(a_moved_shape_is_redrawn_on_its_path_and_a_painted_one_is_pixels) {
    Document doc = canvas(100, 100);
    VectorShape shape;
    shape.path = rectanglePath(Rect(10, 10, 20, 20));
    Layer layer(Asset::make(std::make_shared<Image>(1, 1), "Shape"), Point(0, 0));
    setVectorShape(layer, doc, shape);
    doc.layers.push_back(layer);
    // Moved 30 to the right and scaled twice as wide, as the Move tool would: the path follows and is drawn anew.
    Layer& l = doc.layers[1];
    l.transform.origin.x += 30;
    l.transform.size.width *= 2;
    CHECK_EQ(refreshVectorShapes(doc), 1);
    REQUIRE(isVectorShapeLayer(l));
    const Rect b = pathBounds(vectorShapeOf(l, doc)->path);
    CHECK(std::abs(b.width - 40) < 0.5);
    CHECK_EQ(refreshVectorShapes(doc), 0);
    // Painted on: its pixels are no longer the fill, so it is not a shape any more.
    auto painted = std::make_shared<Image>(*l.asset->image);
    painted->pixel(0, 0)[0] = 1;
    l.asset = Asset::make(painted, "Shape");
    CHECK(!isVectorShapeLayer(l));
}

TEST_MAIN()
