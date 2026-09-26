// Vector shape layers made here (vectorlayer.h): the authored blocks read back through the parsers the renderer
// and the PSD round trip use, and the layer draws its shape.
#include "check.h"
#include "compositor/presets.h"
#include "compositor/psd.h"
#include "compositor/psd_writer.h"
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

TEST_CASE(anchors_are_added_without_changing_the_outline_and_removed) {
    VectorPath path = ellipsePath(Rect(0, 0, 100, 60));
    Document doc = canvas(120, 80);
    auto before = rasterizeVectorMask(path, Rect(0, 0, 120, 80), 1, 120, 80);
    auto hit = nearestPathSegment(path, Point(85, 5));
    REQUIRE(hit.has_value());
    CHECK(hit->distance < 10);
    const int added = insertAnchor(path, hit->subpath, hit->segment, hit->t);
    CHECK_EQ(int(path.subpaths[0].knots.size()), 5);
    const auto& k = path.subpaths[0].knots[size_t(added)];
    CHECK(knotIsSmooth(k));
    auto after = rasterizeVectorMask(path, Rect(0, 0, 120, 80), 1, 120, 80);
    int worst = 0;
    for (int y = 0; y < 80; y++) for (int x = 0; x < 120; x++) worst = std::max(worst, std::abs(int(before->at(x, y)) - int(after->at(x, y))));
    // The same outline: flattening chords fall elsewhere once a curve is split (a quarter pixel at most), so edge
    // pixels move a little while the area does not.
    long total = 0;
    for (int y = 0; y < 80; y++) for (int x = 0; x < 120; x++) total += int(after->at(x, y)) - int(before->at(x, y));
    CHECK(worst <= 40);
    CHECK(std::abs(total) < 255 * 2);
    auto knot = nearestKnot(path, Point(k.x + 1, k.y), 4);
    REQUIRE(knot.has_value());
    CHECK(knot->second == added);
    removeAnchor(path, knot->first, knot->second);
    CHECK_EQ(int(path.subpaths[0].knots.size()), 4);
    CHECK(!nearestKnot(path, Point(-50, -50), 4));
}

TEST_CASE(the_speech_bubble_tail_hangs_off_its_bottom_edge) {
    // The bottom edge runs right to left; the tail's knots come in order along it, with no fold back.
    const VectorPath bubble = customShapePath("Speech Bubble", Rect(0, 0, 100, 100));
    const auto& k = bubble.subpaths[0].knots;
    REQUIRE(k.size() == 11);
    CHECK(k[5].x > k[6].x && k[6].x > k[8].x && k[8].x > k[9].x);   // 82 > 34 > 20 > 18
    CHECK(std::abs(k[7].y - 100) < 1e-9);                            // the tip
}

TEST_CASE(a_full_set_of_paths_refuses_a_new_one_and_reuses_gaps) {
    Document doc = canvas(10, 10);
    for (int i = 0; i < 998; i++) REQUIRE(setDocumentPath(doc, 0, "p", rectanglePath(Rect(0, 0, 5, 5))) == uint16_t(2000 + i));
    CHECK_EQ(int(setDocumentPath(doc, 0, "extra", rectanglePath(Rect(0, 0, 5, 5)))), 0);
    CHECK_EQ(int(documentPath(doc, 2997)->name == "p"), 1);   // the last one was not overwritten
    removeDocumentPath(doc, 2100);
    CHECK_EQ(int(setDocumentPath(doc, 0, "gap", rectanglePath(Rect(0, 0, 5, 5)))), 2100);
}


// ---- Path operations, fills, live shapes, vector masks ------------------------------------------------------------

namespace {
/// A document with a shape layer exported to PSD and read back: its layer 1.
std::optional<PsdImport> throughPsd(const Document& doc) {
    std::string error;
    auto bytes = encodePsd(doc, {}, nullptr, &error);
    return importPsdBytes(bytes, &error);
}
}

TEST_CASE(path_operations_combine_components_and_merge_into_add_only_geometry) {
    VectorPath path = rectanglePath(Rect(10, 10, 40, 40));
    // Subtract Front Shape: a new group, its op on every subpath of it.
    const int32_t group = addShapeComponent(path, rectanglePath(Rect(20, 20, 10, 10)), VectorPath::Op::Subtract);
    CHECK_EQ(group, 1);
    REQUIRE(path.subpaths.size() == 2);
    CHECK(path.subpaths[1].group == 1 && path.subpaths[1].op == VectorPath::Op::Subtract);
    auto at = [](const VectorPath& p, int x, int y) { return int(rasterizeVectorMask(p, Rect(0, 0, 60, 60), 1, 60, 60)->at(x, y)); };
    CHECK_EQ(at(path, 15, 15), 255);
    CHECK_EQ(at(path, 25, 25), 0);    // the hole
    // Intersect instead: only the overlap.
    setComponentOp(path, 1, VectorPath::Op::Intersect);
    CHECK(path.subpaths[1].op == VectorPath::Op::Intersect);
    CHECK_EQ(at(path, 15, 15), 0);
    CHECK_EQ(at(path, 25, 25), 255);
    // Exclude, with a third, overlapping component.
    setComponentOp(path, 1, VectorPath::Op::Xor);
    CHECK_EQ(at(path, 25, 25), 0);
    // Merge Shape Components: one add-only group, drawn as before.
    setComponentOp(path, 1, VectorPath::Op::Subtract);
    addShapeComponent(path, ellipsePath(Rect(40, 40, 15, 15)), VectorPath::Op::Add);
    const VectorPath merged = mergeShapeComponents(path);
    REQUIRE(!merged.subpaths.empty());
    for (const auto& s : merged.subpaths) CHECK(s.op == VectorPath::Op::Add && s.group == 0);
    auto before = rasterizeVectorMask(path, Rect(0, 0, 60, 60), 1, 60, 60);
    auto after = rasterizeVectorMask(merged, Rect(0, 0, 60, 60), 1, 60, 60);
    long difference = 0;
    for (int y = 0; y < 60; y++) for (int x = 0; x < 60; x++) difference += std::abs(int(before->at(x, y)) - int(after->at(x, y)));
    CHECK(difference < 255 * 8);   // the outline within a fraction of a pixel all round
    CHECK_EQ(int(after->at(25, 25)), 0);
    CHECK_EQ(int(after->at(47, 47)), 255);
    // The ops survive the blocks.
    auto back = parseVectorMask(authorVectorMask(path, 60, 60), 60, 60);
    REQUIRE(back.has_value() && back->subpaths.size() == 3);
    CHECK(back->subpaths[1].op == VectorPath::Op::Subtract && back->subpaths[2].group == 2);
}

TEST_CASE(an_empty_vector_mask_reveals_all_and_inverted_hides_all) {
    VectorPath none;
    CHECK_EQ(int(rasterizeVectorMask(none, Rect(0, 0, 4, 4), 1, 4, 4)->at(1, 1)), 255);
    none.inverted = true;
    CHECK_EQ(int(rasterizeVectorMask(none, Rect(0, 0, 4, 4), 1, 4, 4)->at(1, 1)), 0);
}

TEST_CASE(gradient_and_pattern_fills_and_strokes_round_trip_through_psd) {
    Document doc = canvas(80, 60);
    addDocumentPatterns(doc, {makePattern("pat-1", "Checks", 2, 2, {255, 0, 0, 255, 0, 0, 255, 255, 0, 0, 255, 255, 255, 0, 0, 255})});
    VectorShape shape;
    shape.path = rectanglePath(Rect(10, 10, 60, 40));
    shape.fillPaint.kind = VectorPaint::Kind::Gradient;
    shape.fillPaint.gradient.colors = {{0, {255, 0, 0}, 0.5f}, {1, {0, 0, 255}, 0.5f}};
    shape.fillPaint.gradient.alphas = {{0, 1, 0.5f}, {1, 1, 0.5f}};
    shape.fillPaint.gradient.angle = 0;   // left to right
    shape.fillPaint.gradient.fillLayer = true;
    shape.stroke.enabled = true;
    shape.stroke.width = 4;
    shape.stroke.paint.kind = VectorPaint::Kind::Pattern;
    shape.stroke.paint.pattern.id = "pat-1";
    Layer layer(Asset::make(std::make_shared<Image>(1, 1), "Gradient"), Point(0, 0));
    setVectorShape(layer, doc, shape);
    doc.layers.push_back(layer);
    REQUIRE(isVectorShapeLayer(doc.layers[1]));
    bool gdfl = false, soco = false;
    for (auto& b : doc.layers[1].psdCarry->blocks) { gdfl |= b.key == "GdFl"; soco |= b.key == "SoCo"; }
    CHECK(gdfl && !soco);
    auto out = renderFlattened(doc);
    CHECK(out->pixel(14, 30)[0] > 200 && out->pixel(14, 30)[2] < 60);   // red at the left
    CHECK(out->pixel(66, 30)[2] > 200 && out->pixel(66, 30)[0] < 60);   // blue at the right
    // The stroke's pattern (red and blue checks) along the top edge.
    int red = 0, blue = 0;
    for (int x = 20; x < 30; x++) { const uint8_t* p = out->pixel(x, 10); red += p[0] > 200 && p[2] < 60; blue += p[2] > 200 && p[0] < 60; }
    CHECK(red > 0 && blue > 0);
    auto read = vectorShapeOf(doc.layers[1], doc);
    REQUIRE(read.has_value());
    CHECK(read->fillPaint.kind == VectorPaint::Kind::Gradient && read->fillPaint.gradient.colors.size() == 2);
    CHECK(read->stroke.paint.kind == VectorPaint::Kind::Pattern && read->stroke.paint.pattern.id == "pat-1");
    // Through a PSD and back: the same fill and stroke contents.
    auto back = throughPsd(doc);
    REQUIRE(back.has_value());
    REQUIRE(back->document.layers.size() == 2);
    auto shapeBack = vectorShapeOf(back->document.layers[1], back->document);
    REQUIRE(shapeBack.has_value());
    CHECK(shapeBack->fillPaint.kind == VectorPaint::Kind::Gradient);
    CHECK_EQ(int(shapeBack->fillPaint.gradient.colors.back().color.b), 255);
    CHECK(shapeBack->stroke.paint.kind == VectorPaint::Kind::Pattern && shapeBack->stroke.paint.pattern.id == "pat-1");
    // A pattern fill, and back to a colour.
    shapeBack->fillPaint.kind = VectorPaint::Kind::Pattern;
    shapeBack->fillPaint.pattern.id = "pat-1";
    setVectorShape(back->document.layers[1], back->document, *shapeBack);
    auto again = throughPsd(back->document);
    REQUIRE(again.has_value());
    auto patterned = vectorShapeOf(again->document.layers[1], again->document);
    REQUIRE(patterned.has_value());
    CHECK(patterned->fillPaint.kind == VectorPaint::Kind::Pattern && patterned->fillPaint.pattern.id == "pat-1");
    patterned->fillPaint.kind = VectorPaint::Kind::Solid;
    patterned->r = 9;
    setVectorShape(again->document.layers[1], again->document, *patterned);
    auto solid = vectorShapeOf(again->document.layers[1], again->document);
    CHECK(solid && solid->fillPaint.kind == VectorPaint::Kind::Solid && solid->r == 9);
}

TEST_CASE(live_rectangles_and_ellipses_keep_their_properties_until_the_path_is_edited) {
    Document doc = canvas(100, 100);
    LiveShape rect;
    rect.box = Rect(10, 10, 50, 30);
    rect.radii = {0, 4, 8, 12};
    VectorShape shape;
    shape.path = livePath(rect);
    shape.live = {rect};
    LiveShape oval;
    oval.kind = LiveShape::Kind::Ellipse;
    oval.box = Rect(60, 50, 30, 20);
    oval.group = addShapeComponent(shape.path, livePath(oval), VectorPath::Op::Add);
    shape.live.push_back(oval);
    CHECK(liveShapeHolds(rect, shape.path) && liveShapeHolds(oval, shape.path));
    // The block reads back as written.
    auto parsed = parseVectorOrigination(authorVectorOrigination(shape.live));
    REQUIRE(parsed.has_value() && parsed->size() == 2);
    CHECK((*parsed)[0] == rect);
    CHECK((*parsed)[1] == oval);
    Layer layer(Asset::make(std::make_shared<Image>(1, 1), "Live"), Point(0, 0));
    setVectorShape(layer, doc, shape);
    doc.layers.push_back(layer);
    auto read = vectorShapeOf(doc.layers[1], doc);
    REQUIRE(read.has_value() && read->live.size() == 2);
    // Through a PSD.
    auto back = throughPsd(doc);
    REQUIRE(back.has_value());
    auto live = vectorShapeOf(back->document.layers[1], back->document);
    REQUIRE(live.has_value() && live->live.size() == 2);
    CHECK(std::abs(live->live[0].radii[3] - 12) < 1e-6 && std::abs(live->live[1].box.width - 30) < 1e-4);
    // Moved: the boxes go along.
    Layer& l = back->document.layers[1];
    l.transform.origin.x += 5;
    CHECK_EQ(refreshVectorShapes(back->document), 1);
    auto moved = vectorShapeOf(l, back->document);
    REQUIRE(moved.has_value() && moved->live.size() == 2);
    CHECK(std::abs(moved->live[0].box.x - 15) < 1e-3);
    // An anchor dragged: that group's properties end, the other's stay.
    moved->path.subpaths[0].knots[0].x += 3;
    setVectorShape(l, back->document, *moved);
    auto edited = vectorShapeOf(l, back->document);
    REQUIRE(edited.has_value());
    CHECK(edited->live.size() == 1 && edited->live[0].kind == LiveShape::Kind::Ellipse);
}

TEST_CASE(a_pixel_layer_takes_a_vector_mask_that_follows_it) {
    Document doc = canvas(40, 40);
    auto red = std::make_shared<Image>(40, 40);
    red->fill(255, 0, 0, 255);
    doc.layers.push_back(Layer(Asset::make(red, "Red"), Point(0, 0)));
    Layer& l = doc.layers[1];
    CHECK(!hasLayerVectorMask(l));
    setLayerVectorMask(l, doc, VectorPath{});   // Reveal All
    CHECK(hasLayerVectorMask(l));
    CHECK_EQ(int(renderFlattened(doc)->pixel(5, 5)[1]), 0);
    VectorPath hide;
    hide.inverted = true;                        // Hide All
    setLayerVectorMask(l, doc, hide);
    CHECK_EQ(int(renderFlattened(doc)->pixel(5, 5)[1]), 255);
    setLayerVectorMask(l, doc, rectanglePath(Rect(10, 10, 10, 10)));
    auto out = renderFlattened(doc);
    CHECK_EQ(int(out->pixel(15, 15)[1]), 0);
    CHECK_EQ(int(out->pixel(25, 25)[1]), 255);
    // Moved, then given a new mask where it now is: the mask is where it was put.
    l.transform.origin = Point(5, 0);
    setLayerVectorMask(l, doc, rectanglePath(Rect(30, 10, 5, 5)));
    auto mask = layerVectorMask(l, doc);
    REQUIRE(mask.has_value());
    CHECK(std::abs(pathBounds(*mask).x - 30) < 1e-3);
    bool vmsk = false;
    for (auto& b : l.psdCarry->blocks) vmsk |= b.key == "vmsk";
    CHECK(vmsk);
    auto back = throughPsd(doc);
    REQUIRE(back.has_value());
    auto kept = layerVectorMask(back->document.layers[1], back->document);
    REQUIRE(kept.has_value());
    CHECK(std::abs(pathBounds(*kept).x - 30) < 0.01);
    setLayerVectorMask(l, doc, std::nullopt);
    CHECK(!hasLayerVectorMask(l));
}

TEST_CASE(rectangles_take_a_radius_per_corner) {
    const VectorPath p = rectanglePath(Rect(0, 0, 40, 20), std::array<double, 4>{0, 5, 0, 5});
    CHECK_EQ(int(p.subpaths[0].knots.size()), 6);
    // The single-radius form is the same path it always was.
    const VectorPath a = rectanglePath(Rect(0, 0, 40, 20), 6), b = rectanglePath(Rect(0, 0, 40, 20), std::array<double, 4>{6, 6, 6, 6});
    REQUIRE(a.subpaths[0].knots.size() == 8);
    for (size_t i = 0; i < 8; i++) CHECK(std::abs(a.subpaths[0].knots[i].outX - b.subpaths[0].knots[i].outX) < 1e-12);
}
TEST_MAIN()
