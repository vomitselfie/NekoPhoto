// Unit tests for the portable core: geometry, transforms, blending, history,
// compositing semantics, brush strokes, PNG and the .comp round trip.
#include "check.h"
#include "compositor/adjustments.h"
#include <cstring>
#include "compositor/blend.h"
#include "compositor/filters.h"
#include "compositor/brush.h"
#include "compositor/document.h"
#include "compositor/history.h"
#include "compositor/png.h"
#include "compositor/project.h"
#include "compositor/render.h"
#include "compositor/selection.h"
#include "compositor/shape.h"
#include "compositor/matte.h"
#include "compositor/subject.h"
#include "compositor/scribble.h"
#include "compositor/warp.h"
#include "compositor/warpstroke.h"
#include "compositor/transform.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

using namespace compositor;
namespace fs = std::filesystem;

namespace {

std::shared_ptr<Image> solid(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    auto img = std::make_shared<Image>(w, h);
    // Premultiplied.
    img->fill(uint8_t(r * a / 255), uint8_t(g * a / 255), uint8_t(b * a / 255), a);
    return img;
}

Layer imageLayer(const std::string& name, std::shared_ptr<Image> image, Point origin) {
    return Layer(Asset::make(image, name), origin);
}

fs::path tempDir() {
    fs::path dir = fs::temp_directory_path() / ("compositor-core-tests-" + std::to_string(std::rand()));
    fs::create_directories(dir);
    return dir;
}

} // namespace

TEST_CASE(affine_matches_core_graphics_conventions) {
    Affine t = Affine::translation(10, 20).rotated(M_PI / 2).scaledBy(2, 3);
    // Apply order: scale, then rotate, then translate (CG: t.rotated(by:).scaledBy() applies inner ops first).
    Point p = t.apply({1, 1});
    CHECK_NEAR(p.x, 10 - 3, 1e-9);
    CHECK_NEAR(p.y, 20 + 2, 1e-9);
    Point back = t.inverted().apply(p);
    CHECK_NEAR(back.x, 1, 1e-9);
    CHECK_NEAR(back.y, 1, 1e-9);
    Affine ab = Affine::scaling(2, 2).concatenating(Affine::translation(5, 5));
    Point q = ab.apply({1, 1});
    CHECK_NEAR(q.x, 7, 1e-9);
    CHECK_NEAR(q.y, 7, 1e-9);
}

TEST_CASE(layer_transform_pixel_mapping_and_corners) {
    LayerTransform t(Point(100, 50), Size(200, 100));
    Affine m = t.pixelToDocument(20, 10);
    Point p = m.apply({0, 0});
    CHECK_NEAR(p.x, 100, 1e-9); CHECK_NEAR(p.y, 50, 1e-9);
    p = m.apply({20, 10});
    CHECK_NEAR(p.x, 300, 1e-9); CHECK_NEAR(p.y, 150, 1e-9);
    t.flipX = true;
    p = t.pixelToDocument(20, 10).apply({0, 0});
    CHECK_NEAR(p.x, 300, 1e-9);
    t.flipX = false;
    t.rotation = 90;
    auto c = t.corners();
    // Rotating 90 degrees clockwise about the center (200,100): the top-left corner goes to the top-right.
    CHECK_NEAR(c[0].x, 250, 1e-9); CHECK_NEAR(c[0].y, 0, 1e-9);
    CHECK(t.contains({200, 100}));
    CHECK(!t.contains({100, 50}));
    CHECK(t.isValid());
    LayerTransform moved = t.following(t, LayerTransform(Point(110, 60), Size(200, 100)));
    moved.rotation = 90;
    CHECK_NEAR(moved.origin.x, 110, 1e-9);
}

TEST_CASE(transform_placing_recovers_a_scaled_rotated_box) {
    LayerTransform t(Point(10, 20), Size(100, 50));
    t.rotation = 30;
    LayerTransform again = t.placing(t.unitToDocument());
    CHECK_NEAR(again.origin.x, t.origin.x, 1e-6);
    CHECK_NEAR(again.origin.y, t.origin.y, 1e-6);
    CHECK_NEAR(again.size.width, 100, 1e-6);
    CHECK_NEAR(again.size.height, 50, 1e-6);
    CHECK_NEAR(again.rotation, 30, 1e-6);
    CHECK(!again.flipY);
}

TEST_CASE(transform_drag_resize_keeps_opposite_corner) {
    LayerTransform t(Point(0, 0), Size(100, 100));
    TransformDrag drag{t, {100, 100}, TransformDrag::Mode::Resize, 4}; // bottom-right handle
    LayerTransform r = drag.updated({150, 120}, false, false);
    CHECK_NEAR(r.origin.x, 0, 1e-9);
    CHECK_NEAR(r.origin.y, 0, 1e-9);
    CHECK_NEAR(r.size.width, 150, 1e-9);
    CHECK_NEAR(r.size.height, 120, 1e-9);
    LayerTransform locked = drag.updated({150, 120}, true, false);
    CHECK_NEAR(locked.size.width, locked.size.height, 1e-9);
}

TEST_CASE(blend_modes_follow_the_pdf_definitions) {
    Rgb b{0.5f, 0.25f, 1.0f}, s{0.5f, 0.5f, 0.5f};
    Rgb m = blendColor(BlendMode::Multiply, b, s);
    CHECK_NEAR(m.r, 0.25, 1e-6); CHECK_NEAR(m.g, 0.125, 1e-6);
    Rgb sc = blendColor(BlendMode::Screen, b, s);
    CHECK_NEAR(sc.r, 0.75, 1e-6);
    Rgb dodge = blendColor(BlendMode::ColorDodge, {0.25f, 0.25f, 0.25f}, {0.5f, 0.5f, 0.5f});
    CHECK_NEAR(dodge.r, 0.5, 1e-6);
    Rgb burn = blendColor(BlendMode::ColorBurn, {0.75f, 0.75f, 0.75f}, {0.5f, 0.5f, 0.5f});
    CHECK_NEAR(burn.r, 0.5, 1e-6);
    Rgb lum = blendColor(BlendMode::Luminosity, {1, 0, 0}, {0.5f, 0.5f, 0.5f});
    CHECK_NEAR(0.3 * lum.r + 0.59 * lum.g + 0.11 * lum.b, 0.5, 1e-4);
    Rgb ov = blendColor(BlendMode::Overlay, {0.25f, 0.75f, 0.5f}, {0.5f, 0.5f, 0.5f});
    CHECK_NEAR(ov.r, 0.25, 1e-6); CHECK_NEAR(ov.g, 0.75, 1e-6);
}

TEST_CASE(composite_pixel_source_over_and_dodge_soft_edge) {
    uint8_t dst[4] = {0, 0, 255, 255};
    uint8_t src[4] = {128, 0, 0, 128}; // half-transparent red, premultiplied
    compositePixel(BlendMode::Normal, src, 1.0f, dst);
    CHECK_EQ(int(dst[3]), 255);
    CHECK(std::abs(int(dst[0]) - 128) <= 1);
    CHECK(std::abs(int(dst[2]) - 127) <= 1);
    // A translucent dodge must not produce a hard edge: half coverage gives half the effect.
    uint8_t backdrop[4] = {64, 64, 64, 255};
    uint8_t white[4] = {255, 255, 255, 255};
    uint8_t full[4]; std::copy(backdrop, backdrop + 4, full);
    compositePixel(BlendMode::ColorDodge, white, 1.0f, full);
    uint8_t half[4]; std::copy(backdrop, backdrop + 4, half);
    compositePixel(BlendMode::ColorDodge, white, 0.5f, half);
    CHECK_EQ(int(full[0]), 255);
    CHECK(std::abs(int(half[0]) - (64 + 255) / 2) <= 2);
}

TEST_CASE(history_records_only_real_changes_and_shares_images) {
    DocumentHistory history;
    Document doc(10, 10);
    auto image = solid(4, 4, 255, 0, 0);
    doc.layers.push_back(imageLayer("A", image, {0, 0}));
    history.begin("Nothing", doc, doc.layers[0].id);
    history.end(doc, doc.layers[0].id);
    CHECK(!history.canUndo());
    CHECK(!history.isModified());
    history.begin("Hide", doc, doc.layers[0].id);
    doc.layers[0].visible = false;
    history.end(doc, doc.layers[0].id);
    CHECK(history.canUndo());
    CHECK(history.isModified());
    CHECK_EQ(history.undoName(), std::string("Hide"));
    CHECK_EQ(history.retainedBytes(doc), size_t(0)); // the image is shared with the live document
    auto snapshot = history.undo();
    REQUIRE(snapshot.has_value());
    CHECK(snapshot->document->layers[0].visible);
    CHECK(!history.isModified());
    CHECK(history.canRedo());
    auto redo = history.redo();
    REQUIRE(redo.has_value());
    CHECK(!redo->document->layers[0].visible);
    // Nested edits collapse into one entry.
    history.begin("Outer", doc, doc.layers[0].id);
    history.begin("Inner", doc, doc.layers[0].id);
    doc.layers[0].opacity = 0.5;
    history.end(doc, doc.layers[0].id);
    CHECK(history.isEditing());
    doc.layers[0].name = "B";
    history.end(doc, doc.layers[0].id);
    CHECK_EQ(history.undoCount(), 2);
    CHECK_EQ(history.undoName(), std::string("Outer"));
}

TEST_CASE(history_keeps_the_region_an_edit_noted) {
    DocumentHistory history;
    Document doc(100, 100);
    doc.layers.push_back(imageLayer("A", solid(100, 100, 255, 0, 0), {0, 0}));
    history.begin("Paint", doc, doc.layers[0].id);
    history.noteRegion(Rect(10, 10, 5, 5));
    history.noteRegion(Rect(30, 40, 10, 10));
    doc.layers[0].opacity = 0.5;
    history.end(doc, doc.layers[0].id);
    history.begin("Hide", doc, doc.layers[0].id);
    doc.layers[0].visible = false;
    history.end(doc, doc.layers[0].id);
    REQUIRE(history.undo().has_value());
    CHECK(history.stepRegion().isEmpty()); // nothing noted: the caller works it out
    REQUIRE(history.undo().has_value());
    CHECK(history.stepRegion() == Rect(10, 10, 30, 40));
    REQUIRE(history.redo().has_value());
    CHECK(history.stepRegion() == Rect(10, 10, 30, 40));
}

TEST_CASE(history_squash_merges_the_steps_since_a_revision) {
    DocumentHistory history;
    Document doc(10, 10);
    doc.layers.push_back(imageLayer("A", solid(4, 4, 255, 0, 0), {0, 0}));
    history.begin("Before", doc, doc.layers[0].id); doc.layers[0].name = "B"; history.end(doc, doc.layers[0].id);
    const uint64_t since = history.revision();
    CHECK(history.revisionsSince(since)->empty());
    for (double opacity : {0.8, 0.6, 0.4}) {
        history.begin("Opacity", doc, doc.layers[0].id);
        doc.layers[0].opacity = opacity;
        history.end(doc, doc.layers[0].id);
    }
    CHECK_EQ(history.revisionsSince(since)->size(), size_t(3));
    CHECK_EQ(history.squash(since, "Agent"), 3);
    CHECK_EQ(history.undoCount(), 2);
    CHECK_EQ(history.undoName(), std::string("Agent"));
    auto undone = history.undo();
    REQUIRE(undone.has_value());
    CHECK_EQ(undone->document->layers[0].opacity, 1.0);   // all three steps at once
    CHECK_EQ(undone->document->layers[0].name, std::string("B"));
    auto redone = history.redo();
    REQUIRE(redone.has_value());
    CHECK_EQ(redone->document->layers[0].opacity, 0.4);
    // Undone past the mark: nothing to merge.
    history.undo(); history.undo();
    CHECK(!history.revisionsSince(since).has_value());
    CHECK_EQ(history.squash(since, "Agent"), 0);
}

TEST_CASE(changed_area_covers_only_what_changed) {
    Document before(100, 100);
    before.layers.push_back(imageLayer("base", solid(100, 100, 0, 0, 255), {0, 0}));
    before.layers.push_back(imageLayer("spot", solid(10, 10, 255, 0, 0), {20, 30}));
    Document after = before;
    CHECK(changedArea(before, after).isEmpty());
    after.layers[1].opacity = 0.5;
    CHECK(changedArea(before, after) == Rect(18, 28, 14, 14));
    // Moving a layer covers where it was and where it went.
    after = before;
    after.layers[1].transform.origin = {60, 30};
    CHECK(changedArea(before, after) == Rect(18, 28, 54, 14));
    // A change of stacking order, a folder or a new canvas size reaches the whole canvas.
    after = before;
    std::swap(after.layers[0], after.layers[1]);
    CHECK(changedArea(before, after) == before.rect());
    after = before;
    Layer group("Folder", before.size());
    group.isGroup = true;
    after.layers.push_back(group);
    CHECK(changedArea(before, after) == before.rect());
    // A new layer covers its own bounds.
    after = before;
    after.layers.push_back(imageLayer("new", solid(4, 4, 0, 255, 0), {90, 90}));
    CHECK(changedArea(before, after) == Rect(88, 88, 8, 8));
}

TEST_CASE(render_normal_layer_at_offset_and_opacity) {
    Document doc(8, 8);
    doc.layers.push_back(imageLayer("red", solid(4, 4, 255, 0, 0), {2, 2}));
    doc.layers.back().opacity = 0.5;
    auto out = renderFlattened(doc);
    const uint8_t* outside = out->pixel(0, 0);
    CHECK_EQ(int(outside[3]), 0);
    const uint8_t* inside = out->pixel(3, 3);
    CHECK(std::abs(int(inside[3]) - 128) <= 1);
    CHECK(std::abs(int(inside[0]) - 128) <= 1);
    CHECK_EQ(int(inside[1]), 0);
    // Edge pixel (2,2) is fully inside; (1,1) fully outside.
    CHECK(out->pixel(2, 2)[3] > 120);
    CHECK_EQ(int(out->pixel(1, 1)[3]), 0);
}

TEST_CASE(render_scaled_layer_and_region_at_zoom) {
    Document doc(16, 16);
    auto layer = imageLayer("blue", solid(2, 2, 0, 0, 255), {0, 0});
    layer.transform.size = {16, 16}; // scaled 8x
    doc.layers.push_back(layer);
    auto out = renderFlattened(doc);
    CHECK_EQ(int(out->pixel(8, 8)[2]), 255);
    CHECK_EQ(int(out->pixel(15, 15)[3]), 255);
    // A quarter of the document at 2x zoom gives a 16x16 output of the top-left corner.
    Image zoomed;
    RenderOptions options;
    options.region = {0, 0, 8, 8};
    options.scale = 2;
    render(doc, options, zoomed);
    CHECK_EQ(zoomed.width(), 16);
    CHECK_EQ(int(zoomed.pixel(3, 3)[2]), 255);
}

TEST_CASE(render_groups_visibility_masks_and_folder_masks) {
    Document doc(8, 8);
    Layer group("Folder", doc.size());
    group.isGroup = true;
    Layer child = imageLayer("green", solid(8, 8, 0, 255, 0), {0, 0});
    child.parentId = group.id;
    doc.layers = {group, child};
    auto out = renderFlattened(doc);
    CHECK_EQ(int(out->pixel(4, 4)[1]), 255);
    doc.layers[0].visible = false;
    out = renderFlattened(doc);
    CHECK_EQ(int(out->pixel(4, 4)[3]), 0);
    doc.layers[0].visible = true;
    // A layer mask hiding the left half.
    auto mask = std::make_shared<GrayImage>(8, 8, 255);
    for (int y = 0; y < 8; y++) for (int x = 0; x < 4; x++) mask->at(x, y) = 0;
    LayerMask m;
    m.asset = MaskAsset::make(mask);
    doc.layers[1].mask = m;
    out = renderFlattened(doc);
    CHECK_EQ(int(out->pixel(1, 4)[3]), 0);
    CHECK_EQ(int(out->pixel(6, 4)[3]), 255);
    doc.layers[1].mask->enabled = false;
    out = renderFlattened(doc);
    CHECK_EQ(int(out->pixel(1, 4)[3]), 255);
    doc.layers[1].mask.reset();
    // A folder mask hiding the top half clips the child.
    auto fmask = std::make_shared<GrayImage>(8, 8, 255);
    for (int y = 0; y < 4; y++) for (int x = 0; x < 8; x++) fmask->at(x, y) = 0;
    LayerMask fm;
    fm.asset = MaskAsset::make(fmask);
    doc.layers[0].mask = fm;
    out = renderFlattened(doc);
    CHECK_EQ(int(out->pixel(4, 1)[3]), 0);
    CHECK_EQ(int(out->pixel(4, 6)[3]), 255);
    // A uniform 1x1 mask stretches over the layer.
    doc.layers[0].mask.reset();
    LayerMask solidMask;
    solidMask.asset = MaskAsset::solid(false);
    doc.layers[1].mask = solidMask;
    out = renderFlattened(doc);
    CHECK_EQ(int(out->pixel(4, 4)[3]), 0);
}

TEST_CASE(render_clipping_mask_uses_base_alpha) {
    Document doc(8, 8);
    Layer base = imageLayer("base", solid(4, 4, 0, 0, 255), {0, 0});
    Layer clipped = imageLayer("clipped", solid(8, 8, 255, 0, 0), {0, 0});
    clipped.maskSourceId = base.id;
    doc.layers = {base, clipped};
    auto out = renderFlattened(doc);
    // Red shows only where the base has pixels.
    CHECK_EQ(int(out->pixel(2, 2)[0]), 255);
    CHECK_EQ(int(out->pixel(6, 6)[3]), 0);
    // Hiding the base leaves the clipped layer showing through the base's coverage, as on the Mac
    // (coverage ignores the source's visibility).
    doc.layers[0].visible = false;
    out = renderFlattened(doc);
    CHECK_EQ(int(out->pixel(2, 2)[0]), 255);
    CHECK_EQ(int(out->pixel(2, 2)[2]), 0);
    CHECK_EQ(int(out->pixel(6, 6)[3]), 0);
    // A clipped layer whose base is not directly below still clips by the base's coverage.
    doc.layers[0].visible = true;
    Layer between = imageLayer("between", solid(8, 8, 0, 255, 0), {0, 0});
    doc.layers = {doc.layers[0], between, doc.layers[1]};
    out = renderFlattened(doc);
    CHECK_EQ(int(out->pixel(2, 2)[0]), 255);
    CHECK_EQ(int(out->pixel(6, 6)[1]), 255);
    CHECK_EQ(int(out->pixel(6, 6)[0]), 0);
}

TEST_CASE(render_blend_mode_multiply_against_backdrop) {
    Document doc(4, 4);
    doc.layers.push_back(imageLayer("gray", solid(4, 4, 128, 128, 128), {0, 0}));
    Layer top = imageLayer("top", solid(4, 4, 128, 128, 128), {0, 0});
    top.blendMode = BlendMode::Multiply;
    doc.layers.push_back(top);
    auto out = renderFlattened(doc);
    CHECK(std::abs(int(out->pixel(1, 1)[0]) - 64) <= 1);
}

TEST_CASE(brush_paints_and_erases_within_opacity_cap) {
    Document doc(64, 64);
    Layer layer("Layer 1", doc.size());
    BrushSettings settings;
    settings.diameter = 20;
    settings.hardness = 1;
    settings.opacity = 0.5;
    settings.red = 1; settings.green = 0; settings.blue = 0;
    BrushStroke stroke(layer, false, settings, doc.size());
    REQUIRE(stroke.isValid());
    stroke.append({20, 32});
    stroke.append({40, 32});
    stroke.append({44, 32});
    stroke.flush();
    CHECK(stroke.touched());
    auto commit = stroke.commit();
    REQUIRE(commit.asset.has_value());
    const Image& img = *commit.asset->image;
    // Cropped to the painted pixels; the transform places them where they were painted.
    CHECK(img.width() < 64);
    CHECK_NEAR(commit.transform.origin.x, 10, 1.5);
    CHECK_NEAR(commit.transform.size.width, img.width(), 1e-9);
    // Overlapping dabs never exceed the stroke opacity.
    int cx = int(32 - commit.transform.origin.x), cy = int(32 - commit.transform.origin.y);
    const uint8_t* p = img.pixel(cx, cy);
    CHECK(std::abs(int(p[3]) - 128) <= 1);
    CHECK(std::abs(int(p[0]) - 128) <= 1);
    // Erase what was painted.
    Layer painted = layer;
    painted.asset = commit.asset;
    painted.transform = commit.transform;
    settings.erasing = true;
    settings.opacity = 1;
    BrushStroke eraser(painted, false, settings, doc.size());
    eraser.append({20, 32});
    eraser.append({44, 32});
    eraser.flush();
    auto erased = eraser.commit();
    REQUIRE(erased.asset.has_value());
    int ex = int(32 - erased.transform.origin.x), ey = int(32 - erased.transform.origin.y);
    CHECK_EQ(int(erased.asset->image->pixel(ex, ey)[3]), 0);
}

TEST_CASE(brush_heals_and_clones) {
    Document doc(64, 64);
    auto field = solid(64, 64, 120, 120, 120);
    for (int y = 28; y < 36; y++) for (int x = 28; x < 36; x++) { uint8_t* p = std::const_pointer_cast<Image>(std::static_pointer_cast<const Image>(field))->pixel(x, y); p[0] = p[1] = p[2] = 0; }
    Layer layer = imageLayer("field", field, {0, 0});
    BrushSettings settings;
    settings.diameter = 14;
    settings.healing = true;
    BrushStroke stroke(layer, false, settings, doc.size());
    REQUIRE(stroke.isValid());
    stroke.append({32, 32});
    stroke.flush();
    // While painting: a dark wash over the spot.
    CHECK(stroke.previewImage()->pixel(32, 32)[3] == 255);
    auto commit = stroke.commit();
    REQUIRE(commit.asset.has_value());
    int x = int(32 - commit.transform.origin.x), y = int(32 - commit.transform.origin.y);
    CHECK(commit.asset->image->pixel(x, y)[0] > 90); // the black spot is gone
    // Clone: copy from 20 px to the right, where the field is plain gray, onto the spot.
    auto sample = renderFlattened([&] { Document d(64, 64); d.layers.push_back(imageLayer("f", field, {0, 0})); return d; }());
    BrushSettings cloneSettings;
    cloneSettings.diameter = 14;
    BrushStroke cloner(layer, false, cloneSettings, doc.size());
    cloner.setClone({sample, {20, 0}});
    cloner.append({32, 32});
    cloner.flush();
    auto cloned = cloner.commit();
    REQUIRE(cloned.asset.has_value());
    int cx = int(32 - cloned.transform.origin.x), cy = int(32 - cloned.transform.origin.y);
    CHECK(std::abs(int(cloned.asset->image->pixel(cx, cy)[0]) - 120) <= 2);
}

TEST_CASE(brush_paints_mask_and_soft_tip_falls_off) {
    CHECK_NEAR(brushFalloff(0), 1, 1e-9);
    CHECK_NEAR(brushFalloff(1), 0, 1e-9);
    CHECK(brushFalloff(0.5) > 0.2 && brushFalloff(0.5) < 0.6);
    Document doc(32, 32);
    Layer layer = imageLayer("img", solid(32, 32, 0, 0, 0), {0, 0});
    LayerMask m;
    m.asset = MaskAsset::solid(true);
    layer.mask = m;
    BrushSettings settings;
    settings.diameter = 10;
    settings.hardness = 0;
    settings.maskValue = 0; // paint black: hide
    BrushStroke stroke(layer, true, settings, doc.size());
    REQUIRE(stroke.isValid());
    stroke.append({16, 16});
    stroke.flush();
    auto commit = stroke.commit();
    REQUIRE(commit.mask.has_value());
    CHECK_EQ(commit.mask->image->width(), 32);
    CHECK(commit.mask->image->at(16, 16) < 20); // a hardness-0 tip falls off from its very center
    CHECK_EQ(int(commit.mask->image->at(0, 0)), 255);
    CHECK(!commit.maskPlacement.has_value());
}

TEST_CASE(png_round_trip_keeps_pixels) {
    auto image = std::make_shared<Image>(5, 3);
    for (int y = 0; y < 3; y++) for (int x = 0; x < 5; x++) {
        uint8_t* p = image->pixel(x, y);
        uint8_t a = uint8_t(50 + x * 40);
        p[0] = uint8_t(x * 40 * a / 255); p[1] = uint8_t(y * 100 * a / 255); p[2] = uint8_t(a * 200 / 255); p[3] = a;
    }
    std::vector<uint8_t> bytes;
    std::string error;
    REQUIRE(encodePngImage(*image, bytes, 72, &error));
    auto back = decodePngImage(bytes.data(), bytes.size(), &error);
    REQUIRE(back != nullptr);
    CHECK_EQ(back->width(), 5);
    for (int y = 0; y < 3; y++) for (int x = 0; x < 5; x++)
        for (int c = 0; c < 4; c++) CHECK(std::abs(int(back->pixel(x, y)[c]) - int(image->pixel(x, y)[c])) <= 1);
    fs::path dir = tempDir();
    auto gray = std::make_shared<GrayImage>(3, 2, 7);
    gray->at(1, 1) = 200;
    REQUIRE(writePngGray((dir / "m.png").string(), *gray, &error));
    auto g = readPngGray((dir / "m.png").string(), &error);
    REQUIRE(g != nullptr);
    CHECK_EQ(int(g->at(1, 1)), 200);
    CHECK_EQ(int(g->at(0, 0)), 7);
    // A colour PNG is not a valid mask.
    REQUIRE(writePngImage((dir / "c.png").string(), *image, 0, &error));
    CHECK(readPngGray((dir / "c.png").string(), &error) == nullptr);
    fs::remove_all(dir);
}

TEST_CASE(png_written_in_parallel_strips_decodes_exactly) {
    // Big enough for several strips, each primed from the one before; odd widths catch filter edges.
    std::mt19937 rng(7);
    for (auto [w, h] : {std::pair{1537, 700}, std::pair{1, 1}, std::pair{3001, 1}, std::pair{1, 2500}}) {
        Image image(w, h);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            uint8_t* p = image.pixel(x, y);
            // Smooth ramps with noise on top, opaque on the left half and translucent on the right.
            unsigned a = x < w / 2 ? 255u : 40u + unsigned(rng() % 216);
            for (int c = 0; c < 3; c++) p[c] = uint8_t(((x * (c + 1) + y * 3 + int(rng() % 9)) & 255) * a / 255);
            p[3] = uint8_t(a);
        }
        std::vector<uint8_t> bytes;
        std::string error;
        REQUIRE(encodePngImage(image, bytes, 300, &error));
        auto back = decodePngImage(bytes.data(), bytes.size(), &error);
        REQUIRE(back != nullptr);
        CHECK_EQ(back->width(), w);
        CHECK_EQ(back->height(), h);
        int worst = 0;
        bool opaqueExact = true;
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) for (int c = 0; c < 4; c++) {
            int d = std::abs(int(back->pixel(x, y)[c]) - int(image.pixel(x, y)[c]));
            worst = std::max(worst, d);
            if (image.pixel(x, y)[3] == 255 && d) opaqueExact = false;
        }
        CHECK(worst <= 1);   // straight alpha in the file, premultiplied again on the way in
        CHECK(opaqueExact);
    }
    fs::path dir = tempDir();
    GrayImage gray(4099, 600);
    for (int y = 0; y < 600; y++) for (int x = 0; x < 4099; x++) gray.at(x, y) = uint8_t((x ^ y) + int(rng() % 3));
    std::string error;
    REQUIRE(writePngGray((dir / "g.png").string(), gray, &error));
    auto g = readPngGray((dir / "g.png").string(), &error);
    REQUIRE(g != nullptr);
    bool same = true;
    for (int y = 0; y < 600; y++) same = same && std::memcmp(g->row(y), gray.row(y), 4099) == 0;
    CHECK(same);
    fs::remove_all(dir);
}

TEST_CASE(project_round_trip_preserves_layers_masks_groups_and_unknown_fields) {
    Document doc(40, 30);
    doc.resolution = 300;
    Layer group("Folder 1", doc.size());
    group.isGroup = true;
    Layer a = imageLayer("Photo", solid(10, 8, 200, 100, 50, 255), {3, 4});
    a.parentId = group.id;
    a.opacity = 0.75;
    a.blendMode = BlendMode::ColorDodge;
    a.transform.rotation = 15;
    a.transform.flipX = true;
    a.transform.sampling = Sampling::Smooth;
    LayerMask mask;
    auto maskImage = std::make_shared<GrayImage>(10, 8, 255);
    maskImage->at(2, 2) = 0;
    mask.asset = MaskAsset::make(maskImage);
    mask.enabled = false;
    mask.linked = false;
    mask.placement = LayerTransform(Point(5, 5), Size(10, 8));
    a.mask = mask;
    Layer b("Blank", doc.size());
    b.maskSourceId = a.id;
    b.parentId = group.id;
    b.extraJson = "{\"futureField\":[1,2,3]}";
    Layer adj("Levels", doc.size());
    adj.adjustment = LayerAdjustment{AdjustmentKind::Levels, "{\"kind\":\"Levels\",\"levels\":{\"channel\":\"RGB\",\"ranges\":[]},\"custom\":true}"};
    Layer t = imageLayer("Text", solid(30, 10, 0, 0, 0), {2, 2});
    t.text = LayerText{"Hello\nWorld", "Sans", 24, true, false, 0.2, 0.4, 0.6, 1, 1.2, 0.5};
    t.textImage = t.asset->image;
    doc.layers = {group, a, b, adj, t};
    doc.extraJson = "{\"futureManifestField\":\"x\"}";

    fs::path dir = tempDir();
    fs::path package = dir / "Test.comp";
    ProjectError error;
    REQUIRE(saveProject(doc, a.id, package.string(), error));
    CHECK(fs::exists(package / "manifest.json"));
    CHECK(fs::exists(package / "images" / (a.id + ".png")));
    CHECK(fs::exists(package / "images" / (a.id + ".mask.png")));
    CHECK(!fs::exists(package / "images" / (b.id + ".png")));

    auto loaded = loadProject(package.string(), error);
    REQUIRE(loaded.has_value());
    CHECK_EQ(loaded->width, 40);
    CHECK_NEAR(loaded->resolution, 300, 1e-9);
    CHECK_EQ(loaded->id, doc.id);
    REQUIRE(loaded->layers.size() == 5u);
    CHECK(loaded->layers[0].isGroup);
    // A text layer comes back as pixels plus its content and style, still live.
    const Layer& lt = loaded->layers[4];
    REQUIRE(lt.text.has_value());
    CHECK(lt.isLiveText());
    CHECK_EQ(lt.text->text, std::string("Hello\nWorld"));
    CHECK_EQ(lt.text->fontFamily, std::string("Sans"));
    CHECK_NEAR(lt.text->fontSize, 24, 1e-9);
    CHECK(lt.text->bold);
    CHECK(!lt.text->italic);
    CHECK_NEAR(lt.text->green, 0.4, 1e-9);
    CHECK_EQ(lt.text->alignment, 1);
    CHECK_NEAR(lt.text->lineSpacing, 1.2, 1e-9);
    CHECK_NEAR(lt.text->letterSpacing, 0.5, 1e-9);
    const Layer& la = loaded->layers[1];
    CHECK_EQ(la.name, std::string("Photo"));
    CHECK(la.parentId == group.id);
    CHECK_NEAR(la.opacity, 0.75, 1e-9);
    CHECK(la.blendMode == BlendMode::ColorDodge);
    CHECK_NEAR(la.transform.rotation, 15, 1e-9);
    CHECK(la.transform.flipX);
    CHECK(la.transform.sampling == Sampling::Smooth);
    REQUIRE(la.asset.has_value());
    CHECK_EQ(la.asset->image->width(), 10);
    CHECK_EQ(int(la.asset->image->pixel(0, 0)[0]), 200);
    REQUIRE(la.mask.has_value());
    CHECK(!la.mask->enabled);
    CHECK(!la.mask->linked);
    REQUIRE(la.mask->placement.has_value());
    CHECK_NEAR(la.mask->placement->origin.x, 5, 1e-9);
    CHECK_EQ(int(la.mask->asset.image->at(2, 2)), 0);
    CHECK(loaded->layers[2].maskSourceId == a.id);
    CHECK(loaded->layers[2].extraJson.find("futureField") != std::string::npos);
    REQUIRE(loaded->layers[3].adjustment.has_value());
    CHECK(loaded->layers[3].adjustment->kind == AdjustmentKind::Levels);
    CHECK(loaded->layers[3].adjustment->json.find("custom") != std::string::npos);
    CHECK(loaded->extraJson.find("futureManifestField") != std::string::npos);
    CHECK(loadedActiveLayer(package.string()) == a.id);

    // Saving again over the existing package keeps the unknown fields.
    REQUIRE(saveProject(*loaded, std::nullopt, package.string(), error));
    std::ifstream in(package / "manifest.json");
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.find("futureField") != std::string::npos);
    CHECK(text.find("futureManifestField") != std::string::npos);
    CHECK(text.find("\"version\": 7") != std::string::npos);
    CHECK(text.find("\"origin\": [") != std::string::npos);
    CHECK(text.find("\"blendMode\": \"Color Dodge\"") != std::string::npos);
    CHECK(text.find("\"sampling\": \"Smooth\"") != std::string::npos);
    fs::remove_all(dir);
}

TEST_CASE(projects_past_the_macs_100_megapixels_save_and_load) {
    // Game texture stacks: layers that together pass Compositor for macOS's 100-megapixel project total.
    Document doc(6000, 6000);
    for (int i = 0; i < 3; i++) doc.layers.push_back(imageLayer("Texture " + std::to_string(i), solid(6000, 6000, 40 * i, 90, 200, 255), {0, 0}));
    CHECK(doc.layerPixels() == 108000000LL);
    CHECK(!doc.fitsMacBudget());
    fs::path dir = tempDir();
    std::string path = (dir / "Textures.comp").string();
    ProjectError error;
    REQUIRE(saveProject(doc, std::nullopt, path, error));
    auto back = loadProject(path, error);
    REQUIRE(back.has_value());
    CHECK_EQ(int(back->layers.size()), 3);
    CHECK(back->layerPixels() == 108000000LL);
    fs::remove_all(dir);
    // One layer still stops at 100 megapixels, as on the Mac.
    Document one(10, 10);
    one.layers.push_back(imageLayer("Too big", std::make_shared<Image>(10001, 10000), {0, 0}));
    CHECK(one.layers.back().asset->image->width() == 10001);
    dir = tempDir();
    CHECK(!saveProject(one, std::nullopt, (dir / "Big.comp").string(), error));
    CHECK(error.kind == ProjectError::TooLarge);
    fs::remove_all(dir);
}

TEST_CASE(project_rejects_bad_manifests_like_the_mac) {
    ProjectError error;
    CHECK(!parseManifest("{}", error));
    CHECK(error.kind == ProjectError::Invalid);
    std::string base = R"({"format":"com.compositor.project","version":%V,"colorSpace":"sRGB","documentID":"E621E1F8-C36C-495A-93FC-0C247A3E6E5F","width":10,"height":10,"activeLayerID":null,"layers":[%L]})";
    auto with = [&](const std::string& version, const std::string& layers) {
        std::string text = base;
        text.replace(text.find("%V"), 2, version);
        text.replace(text.find("%L"), 2, layers);
        return text;
    };
    CHECK(!parseManifest(with("8", ""), error));
    CHECK(error.kind == ProjectError::Version);
    CHECK_EQ(error.version, 8);
    CHECK(parseManifest(with("1", ""), error).has_value());
    std::string layer = R"({"id":"11111111-2222-3333-4444-555555555555","name":"L","isVisible":true,"transform":{"origin":[0,0],"size":[10,10],"rotation":0,"flipX":false,"flipY":false,"sampling":"High quality"}%X})";
    auto layerWith = [&](const std::string& extra) { std::string t = layer; t.replace(t.find("%X"), 2, extra); return t; };
    CHECK(parseManifest(with("7", layerWith("")), error).has_value());
    // Opacity needs version 3.
    CHECK(!parseManifest(with("2", layerWith(",\"opacity\":0.5")), error));
    CHECK(parseManifest(with("3", layerWith(",\"opacity\":0.5")), error).has_value());
    // Masks need version 4 and the right filename.
    CHECK(!parseManifest(with("3", layerWith(",\"maskFile\":\"11111111-2222-3333-4444-555555555555.mask.png\"")), error));
    CHECK(!parseManifest(with("4", layerWith(",\"maskFile\":\"wrong.png\"")), error));
    CHECK(parseManifest(with("4", layerWith(",\"maskFile\":\"11111111-2222-3333-4444-555555555555.mask.png\"")), error).has_value());
    // A missing parent is invalid; a lowercase uuid is accepted and uppercased.
    CHECK(!parseManifest(with("7", layerWith(",\"parentID\":\"99999999-2222-3333-4444-555555555555\"")), error));
    auto lower = parseManifest(with("7", layerWith(",\"imageFile\":\"11111111-2222-3333-4444-555555555555.png\"")), error);
    CHECK(lower.has_value());
    CHECK(!parseManifest(with("7", layerWith(",\"imageFile\":\"other.png\"")), error));
    // Deep nesting in an unknown field is refused, not recursed into on save (a crafted file's stack overflow).
    const std::string deep = std::string(100000, '[') + std::string(100000, ']');
    std::string nested = with("7", "");
    nested.insert(nested.size() - 1, ",\"extra\":" + deep);
    CHECK(!parseManifest(nested, error));
    CHECK(error.kind == ProjectError::Invalid);
    std::string shallow = with("7", "");
    shallow.insert(shallow.size() - 1, ",\"extra\":[[[1]]]");
    CHECK(parseManifest(shallow, error).has_value());
    // Self clipping is a cycle.
    CHECK(!parseManifest(with("7", layerWith(",\"maskSourceID\":\"11111111-2222-3333-4444-555555555555\"")), error));
    // Adjustment layers need version 7.
    CHECK(!parseManifest(with("6", layerWith(",\"adjustment\":{\"kind\":\"Levels\"}")), error));
    CHECK(parseManifest(with("7", layerWith(",\"adjustment\":{\"kind\":\"Levels\"}")), error).has_value());
    // Dimensions beyond 30000 are too large.
    std::string big = with("7", "");
    big.replace(big.find("\"width\":10"), 10, "\"width\":40000");
    CHECK(!parseManifest(big, error));
    CHECK(error.kind == ProjectError::TooLarge);
}

TEST_CASE(selection_rasterizes_and_combines) {
    auto rect = rasterizeRect({2, 2, 4, 4}, 10, 10, false);
    CHECK_EQ(int(rect->at(3, 3)), 255);
    CHECK_EQ(int(rect->at(1, 1)), 0);
    CHECK_EQ(int(rect->at(6, 6)), 0);
    auto half = rasterizeRect({2.5, 2, 4, 4}, 10, 10, true);
    CHECK(std::abs(int(half->at(2, 3)) - 128) <= 2);
    CHECK_EQ(int(half->at(4, 3)), 255);
    auto ellipse = rasterizeEllipse({0, 0, 10, 10}, 10, 10, true);
    CHECK_EQ(int(ellipse->at(5, 5)), 255);
    CHECK(ellipse->at(0, 0) < 30);
    auto sel = combineSelection(std::nullopt, *rect, SelectionMode::Replace, true);
    REQUIRE(sel.has_value());
    auto more = rasterizeRect({5, 5, 4, 4}, 10, 10, false);
    sel = combineSelection(sel, *more, SelectionMode::Add, true);
    CHECK_EQ(int(sel->coverage->at(7, 7)), 255);
    sel = combineSelection(sel, *rect, SelectionMode::Subtract, true);
    CHECK_EQ(int(sel->coverage->at(3, 3)), 0);
    CHECK_EQ(int(sel->coverage->at(7, 7)), 255);
    auto inverted = invertSelection(*sel, 10, 10);
    CHECK_EQ(int(inverted.coverage->at(3, 3)), 255);
    auto loops = selectionOutline(*rect);
    CHECK_EQ(loops.size(), size_t(1));
    CHECK_EQ(loops[0].size(), size_t(4));
}

TEST_CASE(hierarchy_entries_and_validation) {
    Document doc(10, 10);
    Layer g("Folder 1", doc.size());
    g.isGroup = true;
    Layer a("A", doc.size());
    a.parentId = g.id;
    Layer b("B", doc.size());
    doc.layers = {b, g, a};
    auto entries = hierarchyEntries(doc.layers, true);
    REQUIRE(entries.size() == 3u);
    CHECK_EQ(entries[0].layer->name, std::string("Folder 1"));
    CHECK_EQ(entries[1].layer->name, std::string("A"));
    CHECK_EQ(entries[1].depth, 1);
    CHECK_EQ(entries[2].layer->name, std::string("B"));
    CHECK(validateHierarchy(doc.layers));
    doc.layers[2].parentId = "00000000-0000-0000-0000-000000000000";
    CHECK(!validateHierarchy(doc.layers));
    doc.layers[2].parentId = g.id;
    doc.layers[1].parentId = a.id; // cycle through a non-group
    CHECK(!validateHierarchy(doc.layers));
    CHECK_EQ(nextLayerName(doc.layers, "Layer"), std::string("Layer 1"));
}

TEST_CASE(adjustment_settings_round_trip_matches_the_manifest_encoding) {
    AdjustmentSettings s = AdjustmentSettings::defaults(AdjustmentKind::HueSaturation);
    s.hsv.range = 1;
    s.hsv.adjustments[1] = {30, -20, 5};
    s.hsv.adjustments[0] = {0, 10, 0};
    s.hsv.bands[1] = HueBand{300, 340, 20, 60};
    s.levels.ranges[2] = {10, 1.2, 240, 5, 250};
    s.curves.channels[0] = {{0, 0}, {128, 150}, {255, 255}};
    s.exposure = {1.5, 0.1, 1.2};
    s.gradientMap.highlights = {0.2, 0.4, 0.6};
    s.gradientMap.reversed = true;
    s.grain = {40, 2, 60, 12345};
    s.extraJson = "{\"future\":1}";
    std::string json = s.toJson();
    // Enum-keyed dictionaries are flat key/value arrays, as Swift writes them.
    CHECK(json.find("\"adjustments\":[\"Master\"") != std::string::npos);
    CHECK(json.find("\"kind\":\"Hue/Saturation\"") != std::string::npos);
    CHECK(json.find("\"future\":1") != std::string::npos);
    AdjustmentSettings back;
    REQUIRE(AdjustmentSettings::parse(json, back));
    CHECK(back == s);
    // The pre-range form (top-level sliders only) still decodes.
    AdjustmentSettings old;
    REQUIRE(AdjustmentSettings::parse("{\"kind\":\"Hue/Saturation\",\"hue\":15,\"saturation\":-10,\"lightness\":0,\"colorize\":false}", old));
    CHECK_NEAR(old.hsv.adjustments[0].hue, 15, 1e-9);
    // An object-keyed dictionary is accepted too.
    AdjustmentSettings obj;
    REQUIRE(AdjustmentSettings::parse("{\"kind\":\"Levels\",\"hsvSettings\":{\"range\":\"Reds\",\"adjustments\":{\"Reds\":{\"hue\":5}}}}", obj));
    CHECK_NEAR(obj.hsv.adjustments[1].hue, 5, 1e-9);
    CHECK(!AdjustmentSettings::parse("{\"kind\":\"Nope\"}", obj));
    CHECK(!AdjustmentSettings::parse("{\"kind\":\"Levels\",\"levels\":{\"channel\":\"RGB\",\"ranges\":[]}}", obj));
}

TEST_CASE(adjustments_change_pixels_as_expected) {
    auto gray = solid(4, 4, 128, 128, 128);
    Image img = *gray;
    LevelsSettings levels;
    levels.ranges[0].black = 64; levels.ranges[0].white = 192;
    applyLevels(img, levels);
    CHECK_EQ(int(img.pixel(0, 0)[0]), 128); // midpoint stays put
    levels.ranges[0].gamma = 2;
    applyLevels(img = *gray, levels);
    CHECK(img.pixel(0, 0)[0] > 170);
    ExposureSettings exposure{1, 0, 1};
    applyExposure(img = *gray, exposure);
    CHECK(img.pixel(0, 0)[0] > 160);
    CurvesSettings curves;
    curves.channels[0] = {{0, 0}, {128, 200}, {255, 255}};
    applyCurves(img = *gray, curves);
    CHECK(std::abs(int(img.pixel(0, 0)[0]) - 200) <= 1);
    CHECK_NEAR(curves.value(0, 0), 0, 1e-9);
    CHECK_NEAR(curves.value(255, 0), 255, 1e-9);
    GradientMapSettings map;
    map.shadows = {1, 0, 0}; map.highlights = {0, 0, 1};
    applyGradientMap(img = *gray, map);
    CHECK(std::abs(int(img.pixel(0, 0)[0]) - 127) <= 1);
    CHECK(std::abs(int(img.pixel(0, 0)[2]) - 128) <= 1);
    HueSaturationSettings hsv;
    hsv.adjustments[0] = {180, 0, 0};
    auto red = solid(4, 4, 255, 0, 0);
    applyHueSaturation(img = *red, hsv);
    CHECK(img.pixel(1, 1)[0] < 30);
    CHECK(img.pixel(1, 1)[1] > 220);
    CHECK(img.pixel(1, 1)[2] > 220);
    hsv.adjustments[0] = {0, -100, 0};
    applyHueSaturation(img = *red, hsv);
    CHECK(std::abs(int(img.pixel(1, 1)[0]) - int(img.pixel(1, 1)[1])) <= 2);
    HueBand reds = HueBand::defaultBand(1);
    CHECK_NEAR(reds.weight(0), 1, 1e-9);
    CHECK_NEAR(reds.weight(30), 0.5, 1e-9);
    CHECK_NEAR(reds.weight(90), 0, 1e-9);
    applyInvert(img = *gray);
    CHECK_EQ(int(img.pixel(0, 0)[0]), 127);
    // A translucent pixel keeps its alpha when inverted.
    Image half = *solid(1, 1, 200, 100, 50, 128);
    applyInvert(half);
    CHECK_EQ(int(half.pixel(0, 0)[3]), 128);
    CHECK(half.pixel(0, 0)[0] <= 128);
    GrainSettings grain;
    grain.amount = 50;
    Image a = *gray, b = *gray;
    applyGrain(a, grain, {0, 0}, 1);
    applyGrain(b, grain, {0, 0}, 1);
    CHECK(a == b);
    CHECK(!(a == *gray));
    // Through the adjustment-layer path.
    LayerAdjustment adj{AdjustmentKind::GradientMap, AdjustmentSettings::defaults(AdjustmentKind::GradientMap).toJson()};
    img = *red;
    CHECK(applyAdjustment(adj, img, {0, 0, 4, 4}, 1));
    CHECK_EQ(int(img.pixel(0, 0)[0]), int(img.pixel(0, 0)[1])); // gray from black to white
}

TEST_CASE(filters_blur_noise_lens_and_growing) {
    Image dot(21, 21);
    dot.pixel(10, 10)[3] = 255; dot.pixel(10, 10)[0] = 255;
    Image blurred = dot;
    gaussianBlur(blurred, 2);
    CHECK(blurred.pixel(10, 10)[3] < 255);
    CHECK(blurred.pixel(12, 10)[3] > 0);
    CHECK_EQ(int(blurred.pixel(0, 0)[3]), 0);
    long total = 0;
    for (int y = 0; y < 21; y++) for (int x = 0; x < 21; x++) total += blurred.pixel(x, y)[3];
    CHECK(std::abs(total - 255) < 40); // mass is conserved
    Image wide = dot;
    gaussianBlur(wide, 8); // box approximation path
    CHECK(wide.pixel(10, 10)[3] < blurred.pixel(10, 10)[3]);
    Image streak = dot;
    motionBlur(streak, 9, 0);
    CHECK(streak.pixel(14, 10)[3] > 0);
    CHECK_EQ(int(streak.pixel(10, 14)[3]), 0);
    Image vertical = dot;
    motionBlur(vertical, 9, 90);
    CHECK(vertical.pixel(10, 14)[3] > 0);
    CHECK_EQ(int(vertical.pixel(14, 10)[3]), 0);
    FilterSettings settings;
    settings.amount = 30;
    Image noisy = *solid(16, 16, 100, 100, 100);
    applyFilter(FilterKind::AddNoise, noisy, settings, 1, 7);
    CHECK(!(noisy == *solid(16, 16, 100, 100, 100)));
    settings.distortion = -100;
    Image lens = *solid(32, 32, 100, 100, 100);
    applyFilter(FilterKind::LensCorrection, lens, settings);
    CHECK_EQ(int(lens.pixel(0, 0)[3]), 0);
    // Growing and trimming keep pixels in place on the document.
    LayerTransform t(Point(10, 20), Size(21, 21));
    LayerTransform grown;
    auto big = growImage(dot, t, 5, grown);
    REQUIRE(big != nullptr);
    CHECK_EQ(big->width(), 31);
    CHECK_NEAR(grown.origin.x, 5, 1e-9);
    CHECK_EQ(int(big->pixel(15, 15)[3]), 255);
    LayerTransform trimmed;
    auto small = trimToPixels(*big, grown, trimmed);
    CHECK_EQ(small->width(), 1);
    CHECK_NEAR(trimmed.origin.x, 20, 1e-9);
    CHECK_NEAR(trimmed.origin.y, 30, 1e-9);
    CHECK_NEAR(blurMargin(FilterKind::GaussianBlur, settings), 5, 1e-9);
}

TEST_CASE(warp_identity_and_perspective) {
    auto img = solid(20, 10, 255, 0, 0);
    LayerTransform t(Point(5, 7), Size(20, 10));
    Corners same = cornersOf(t);
    CHECK(cornersUsable(same));
    auto w = warpImage(*img, t, same);
    REQUIRE(w.has_value());
    CHECK_EQ(w->image->width(), 20);
    CHECK_NEAR(w->transform.origin.x, 5, 1e-9);
    CHECK_EQ(int(w->image->pixel(10, 5)[0]), 255);
    CHECK_EQ(int(w->image->pixel(10, 5)[3]), 255);
    // A trapezoid: the bottom edge narrowed. Pixels at the top corners stay, bottom corners empty.
    Corners trap = {Point{0, 0}, Point{40, 0}, Point{30, 20}, Point{10, 20}};
    auto p = warpImage(*img, t, trap);
    REQUIRE(p.has_value());
    CHECK_EQ(p->image->width(), 40);
    CHECK_EQ(int(p->image->pixel(20, 10)[3]), 255);
    CHECK_EQ(int(p->image->pixel(1, 18)[3]), 0);
    CHECK(p->image->pixel(20, 18)[3] == 255);
    Homography h = Homography::unitTo(trap);
    Point m = h.map({0.5, 1});
    CHECK_NEAR(m.x, 20, 1e-9); CHECK_NEAR(m.y, 20, 1e-9);
    Point back = h.inverted().map(m);
    CHECK_NEAR(back.x, 0.5, 1e-9); CHECK_NEAR(back.y, 1, 1e-9);
    Corners twisted = {Point{0, 0}, Point{40, 0}, Point{0, 20}, Point{40, 20}};
    CHECK(!cornersUsable(twisted));
    Rect crop;
    auto trimmed = warpImageTrimmed(*img, t, trap, &crop);
    REQUIRE(trimmed.has_value());
    CHECK(trimmed->image->width() <= 40);
    auto mask = std::make_shared<GrayImage>(20, 10, 255);
    auto wm = warpMask(*mask, t, trap, 0);
    REQUIRE(wm.has_value());
    CHECK_EQ(int(wm->image->at(20, 10)), 255);
    CHECK_EQ(int(wm->image->at(1, 18)), 0);
}

TEST_CASE(shape_rasters_and_gradients) {
    auto rect = shapeImage(ShapeKind::Rectangle, 10, 6, 1, 0, 0, 0);
    CHECK_EQ(int(rect->pixel(0, 0)[3]), 255);
    CHECK_EQ(int(rect->pixel(9, 5)[0]), 255);
    auto rounded = shapeImage(ShapeKind::Rectangle, 20, 20, 0, 1, 0, 8);
    CHECK_EQ(int(rounded->pixel(0, 0)[3]), 0);
    CHECK_EQ(int(rounded->pixel(10, 10)[1]), 255);
    CHECK_EQ(int(rounded->pixel(10, 0)[3]), 255);
    auto ellipse = shapeImage(ShapeKind::Ellipse, 20, 10, 0, 0, 1, 0);
    CHECK_EQ(int(ellipse->pixel(0, 0)[3]), 0);
    CHECK_EQ(int(ellipse->pixel(10, 5)[2]), 255);
    Image base(10, 1);
    Image out(10, 1);
    GradientStops stops{{1, 0, 0, 1}, {0, 0, 1, 1}};
    fillGradient(base, out, Affine::identity(), GradientShape::Linear, {0, 0}, {10, 0}, stops, 1, nullptr);
    CHECK(out.pixel(0, 0)[0] > 200);
    CHECK(out.pixel(9, 0)[2] > 200);
    CHECK(std::abs(int(out.pixel(5, 0)[0]) - int(out.pixel(5, 0)[2])) < 40);
    // Through a brush stroke on a blank layer, transparent at the far end.
    Document doc(16, 16);
    Layer blank("L", doc.size());
    BrushSettings s;
    BrushStroke stroke(blank, false, s, doc.size());
    float a[4] = {0, 1, 0, 1}, b[4] = {0, 1, 0, 0};
    stroke.fillGradientOver(0, {0, 8}, {16, 8}, a, b, 1);
    auto commit = stroke.commit();
    REQUIRE(commit.asset.has_value());
    CHECK(commit.asset->image->pixel(0, 8)[3] > 240);
    CHECK(commit.asset->image->pixel(commit.asset->image->width() - 1, 8)[3] < 40);
}

TEST_CASE(pixel_move_lifts_and_places) {
    Document doc(32, 32);
    auto img = solid(32, 32, 0, 0, 255);
    Layer layer = imageLayer("blue", img, {0, 0});
    auto sel = rasterizeRect({4, 4, 8, 8}, 32, 32, false);
    BrushSettings s;
    BrushStroke stroke(layer, false, s, doc.size(), sel.get());
    REQUIRE(stroke.liftSelection());
    stroke.moveLifted({10, 0}, false);
    auto out = stroke.previewImage();
    CHECK_EQ(int(out->pixel(5, 5)[3]), 0);      // the hole
    CHECK_EQ(int(out->pixel(15, 5)[2]), 255);   // the moved pixels
    stroke.moveLifted({10, 0}, true);
    CHECK_EQ(int(stroke.previewImage()->pixel(5, 5)[2]), 255); // duplicating leaves the source
    auto commit = stroke.commit();
    REQUIRE(commit.asset.has_value());
}

TEST_CASE(warp_stroke_moves_pixels) {
    auto img = std::make_shared<Image>(40, 40);
    for (int y = 0; y < 40; y++) for (int x = 0; x < 40; x++) { uint8_t* p = img->pixel(x, y); p[0] = x < 20 ? 255 : 0; p[2] = x < 20 ? 0 : 255; p[3] = 255; }
    WarpStroke liquify(img, WarpMode::Liquify, 16, 0.5, 1);
    liquify.append({20, 20});
    liquify.append({28, 20});
    CHECK(!liquify.points().empty());
    // Red was pushed to the right at the centre of the stroke.
    CHECK(liquify.image()->pixel(24, 20)[0] > 128);
    auto img2 = std::make_shared<Image>(*img);
    WarpStroke smudge(img2, WarpMode::Smudge, 16, 0.5, 0.8);
    smudge.append({18, 20});
    smudge.append({30, 20});
    CHECK(smudge.image()->pixel(26, 20)[0] > 40);
    // Pushing the edge away and back again through the displacement field brings back a sharp edge: the
    // result is always resampled from the original, never from itself.
    auto img3 = std::make_shared<Image>(40, 40);
    for (int y = 0; y < 40; y++) for (int x = 0; x < 40; x++) { uint8_t* p = img3->pixel(x, y); p[0] = x < 20 ? 255 : 0; p[2] = x < 20 ? 0 : 255; p[3] = 255; }
    WarpStroke there(img3, WarpMode::Liquify, 24, 0.5, 1);
    for (int i = 0; i <= 40; i++) there.append({20.0 + i * 0.25, 20});
    for (int i = 40; i >= 0; i--) there.append({20.0 + i * 0.25, 20});
    const Image& back = *there.image();
    int transition = 0;
    for (int x = 0; x < 40; x++) if (back.pixel(x, 20)[0] > 10 && back.pixel(x, 20)[0] < 245) transition++;
    CHECK(transition <= 3);
    CHECK(back.pixel(17, 20)[0] > 245);
    CHECK(back.pixel(23, 20)[0] < 10);
}

TEST_CASE(image_size_resamples_layers) {
    Document doc(40, 20);
    Layer a = imageLayer("a", solid(20, 10, 255, 0, 0), {10, 5});
    LayerMask m;
    auto mask = std::make_shared<GrayImage>(20, 10, 255);
    for (int y = 0; y < 10; y++) for (int x = 0; x < 10; x++) mask->at(x, y) = 0;
    m.asset = MaskAsset::make(mask);
    a.mask = m;
    doc.layers.push_back(a);
    REQUIRE(resizeDocument(doc, 80, 40, 150, Sampling::High));
    CHECK_EQ(doc.width, 80);
    CHECK_NEAR(doc.resolution, 150, 1e-9);
    const Layer& l = doc.layers[0];
    CHECK_EQ(l.asset->image->width(), 40);
    CHECK_EQ(l.asset->image->height(), 20);
    CHECK_NEAR(l.transform.origin.x, 20, 1e-9);
    CHECK_EQ(l.mask->asset.image->width(), 40);
    CHECK_EQ(int(l.mask->asset.image->at(5, 5)), 0);
    CHECK_EQ(int(l.mask->asset.image->at(35, 5)), 255);
    auto flat = renderFlattened(doc);
    CHECK_EQ(int(flat->pixel(50, 20)[0]), 255);
    CHECK_EQ(int(flat->pixel(25, 20)[3]), 0);
}

TEST_CASE(adjustment_layers_render_as_their_direct_application) {
    // A gradient base under Levels and Exposure adjustment layers.
    auto gradient = std::make_shared<Image>(64, 8);
    for (int y = 0; y < 8; y++) for (int x = 0; x < 64; x++) { uint8_t* p = gradient->pixel(x, y); unsigned a = y < 6 ? 255 : 128; p[0] = uint8_t((x * 4 * a + 127) / 255); p[1] = uint8_t((255 - x * 4) * a / 255); p[2] = uint8_t((x * 2 + 60) * a / 255); p[3] = uint8_t(a); }
    Document doc(64, 8);
    doc.layers.push_back(imageLayer("base", gradient, {0, 0}));
    AdjustmentSettings levels = AdjustmentSettings::defaults(AdjustmentKind::Levels);
    levels.levels.ranges[0].outputWhite = 254;   // darkens every full-white channel by exactly one level
    levels.levels.ranges[0].gamma = 1.4;
    Layer a("Levels", doc.size());
    a.adjustment = levels.toLayerAdjustment();
    AdjustmentSettings exposure = AdjustmentSettings::defaults(AdjustmentKind::Exposure);
    exposure.exposure.exposure = -0.7;
    Layer b("Exposure", doc.size());
    b.adjustment = exposure.toLayerAdjustment();
    doc.layers.push_back(a);
    // One layer at full opacity: the flattened result is the adjustment applied to the base, bit for bit.
    auto one = renderFlattened(doc);
    Image direct = *gradient;
    applyAdjustment(levels, direct, Rect(0, 0, 64, 8), 1);
    CHECK_EQ(std::memcmp(one->data(), direct.data(), one->byteCount()), 0);
    CHECK_EQ(int(one->pixel(63, 0)[1]), int(direct.pixel(63, 0)[1]));
    CHECK(int(one->pixel(1, 0)[1]) < 255);
    // Two in a row fuse into one table: within a level of applying them in turn.
    doc.layers.push_back(b);
    auto two = renderFlattened(doc);
    applyAdjustment(exposure, direct, Rect(0, 0, 64, 8), 1);
    int worst = 0;
    for (size_t i = 0; i < two->byteCount(); i++) worst = std::max(worst, std::abs(int(two->data()[i]) - int(direct.data()[i])));
    CHECK(worst <= 1);
    // A layer mask (all white) sends the layer through the blending path, which must give the direct result too.
    LayerMask m;
    m.asset = MaskAsset::make(std::make_shared<GrayImage>(64, 8, 255));
    doc.layers[1].mask = m;
    doc.layers.pop_back();
    auto masked = renderFlattened(doc);
    Image again = *gradient;
    applyAdjustment(levels, again, Rect(0, 0, 64, 8), 1);
    CHECK_EQ(std::memcmp(masked->data(), again.data(), masked->byteCount()), 0);
}

TEST_CASE(render_cache_matches_a_plain_render) {
    // Five layers, the third being edited through an override: with a cache the frame must match the plain
    // render, whether the layers above are plain (flattened once) or not (drawn live).
    Document doc(120, 90);
    for (int i = 0; i < 5; i++) {
        Layer l = imageLayer("L" + std::to_string(i), solid(60, 40, uint8_t(50 * i), uint8_t(255 - 40 * i), uint8_t(30 * i), uint8_t(i == 1 ? 160 : 255)), {10.0 * i, 8.0 * i});
        l.transform.rotation = i == 3 ? 20 : 0;
        l.opacity = i == 4 ? 0.7 : 1;
        doc.layers.push_back(l);
    }
    auto stroke = solid(60, 40, 255, 255, 0);
    Overrides overrides;
    overrides[doc.layers[2].id].image = stroke;
    RenderOptions o;
    o.region = Rect(5, 5, 100, 70);
    o.scale = 1.5;
    o.version = 7;
    Image plain, cached, again;
    render(doc, o, plain, &overrides);
    RenderCache cache;
    render(doc, o, cached, &overrides, &cache);
    REQUIRE(cache.backdrop != nullptr);
    CHECK(cache.aboveFlat);
    CHECK(cache.above != nullptr);
    CHECK_EQ(std::memcmp(plain.data(), cached.data(), plain.byteCount()), 0);
    render(doc, o, again, &overrides, &cache);   // from the cache
    CHECK_EQ(std::memcmp(plain.data(), again.data(), plain.byteCount()), 0);
    // A Multiply layer above: the layers above are drawn live, the backdrop still cached.
    doc.layers[4].blendMode = BlendMode::Multiply;
    o.version = 8;
    render(doc, o, plain, &overrides);
    render(doc, o, cached, &overrides, &cache);
    CHECK(!cache.aboveFlat);
    CHECK_EQ(std::memcmp(plain.data(), cached.data(), plain.byteCount()), 0);
    // A layer clipped to the edited one leaves the cache aside.
    doc.layers[3].maskSourceId = doc.layers[2].id;
    o.version = 9;
    render(doc, o, plain, &overrides);
    render(doc, o, cached, &overrides, &cache);
    CHECK_EQ(std::memcmp(plain.data(), cached.data(), plain.byteCount()), 0);
}

TEST_CASE(stamped_dabs_match_the_general_path) {
    // The same stroke with stamped dabs and with per-pixel dabs: the interior and the outside agree exactly,
    // the antialiased rim within the quarter-pixel phase the stamp snaps to.
    Document doc(200, 120);
    for (double hardness : {1.0, 0.5}) {
        std::shared_ptr<Image> results[2];
        for (int stamped = 0; stamped < 2; stamped++) {
            BrushSettings s;
            s.diameter = 41; s.hardness = hardness; s.stampedDabs = stamped == 1;
            Layer flat = imageLayer("flat", solid(200, 120, 0, 0, 0, 0), {0, 0});
            BrushStroke stroke(flat, false, s, doc.size());
            REQUIRE(stroke.isValid());
            stroke.append({40.3, 60.6});
            stroke.append({150.7, 60.2});
            stroke.append({160.1, 100.9});
            stroke.flush();
            results[stamped] = std::make_shared<Image>(*stroke.previewImage());
        }
        int worst = 0; long total = 0, count = 0;
        for (int y = 0; y < 120; y++)
            for (int x = 0; x < 200; x++) {
                int a = results[0]->pixel(x, y)[3], b = results[1]->pixel(x, y)[3];
                if ((a == 0) != (b == 0)) { CHECK(std::min(a, b) <= 40); }
                if (a == 255 || b == 255) CHECK(std::abs(a - b) <= 40);
                worst = std::max(worst, std::abs(a - b)); total += std::abs(a - b); count++;
            }
        CHECK(worst <= 48);
        CHECK(double(total) / double(count) < 0.6);
        CHECK_EQ(int(results[1]->pixel(90, 60)[3]), 255);
        CHECK_EQ(int(results[1]->pixel(90, 20)[3]), 0);
    }
}

TEST_CASE(matte_refinement_follows_the_guide) {
    // A hard vertical edge in the guide at x=20; a coarse mask edge at x=24 gets pulled onto the guide's edge.
    auto guide = std::make_shared<Image>(40, 40);
    for (int y = 0; y < 40; y++) for (int x = 0; x < 40; x++) { uint8_t v = x < 20 ? 240 : 20; uint8_t* p = guide->pixel(x, y); p[0] = p[1] = p[2] = v; p[3] = 255; }
    GrayImage mask(40, 40, 0);
    for (int y = 0; y < 40; y++) for (int x = 0; x < 24; x++) mask.at(x, y) = 255;
    auto refined = guidedRefine(mask, *guide, 6, 0);
    CHECK(refined->at(22, 20) < mask.at(22, 20));
    CHECK(refined->at(10, 20) > 200);
    CHECK(refined->at(35, 20) < 40);
    MatteSettings s{0, 100, 0};
    auto hard = refineMatte(mask, *guide, s, 0);
    CHECK_EQ(int(hard->at(10, 20)), 255);
    MatteSettings shrink{0, 0, -4};
    auto shrunk = refineMatte(mask, *guide, shrink, 0);
    CHECK(shrunk->at(23, 20) < 128);
    CHECK(shrunk->at(10, 20) > 128);
    MatteSettings grow{0, 0, 4};
    auto grown = refineMatte(mask, *guide, grow, 0);
    CHECK(grown->at(24, 20) > 128);
    CHECK(grown->at(28, 20) < 128);
}

TEST_CASE(matting_band_recovers_a_soft_edge) {
    // Red over blue with a 24-pixel linear blend between them; the coarse matte is the hard threshold. Matting
    // in a band around that edge solves the opacity back from the colours.
    const int w = 120, h = 60;
    auto guide = std::make_shared<Image>(w, h);
    auto truth = [](int x) { return std::clamp((72 - x) / 24.0, 0.0, 1.0); };   // 1 red left of 48, 0 right of 72
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        double a = truth(x);
        uint8_t* p = guide->pixel(x, y);
        p[0] = uint8_t(std::lround(220 * a + 30 * (1 - a))); p[1] = uint8_t(std::lround(40 * a + 60 * (1 - a))); p[2] = uint8_t(std::lround(30 * a + 200 * (1 - a))); p[3] = 255;
    }
    GrayImage coarse(w, h, 0);
    for (int y = 0; y < h; y++) for (int x = 0; x < 60; x++) coarse.at(x, y) = 255;
    auto matted = matteBand(coarse, *guide, 14, 0);
    double worst = 0;
    for (int y = 8; y < h - 8; y++) for (int x = 50; x < 70; x++) worst = std::max(worst, std::fabs(matted->at(x, y) / 255.0 - truth(x)));
    CHECK(worst <= 0.15);
    CHECK_EQ(int(matted->at(10, 30)), 255);
    CHECK_EQ(int(matted->at(110, 30)), 0);
    // Through the settings, and off by default.
    MatteSettings s{0, 0, 0, 14};
    auto viaSettings = refineMatte(coarse, *guide, s, 0);
    CHECK(std::fabs(viaSettings->at(60, 30) / 255.0 - truth(60)) <= 0.15);
    CHECK_NEAR(MatteSettings().normalized().matting, 0, 1e-9);
}

TEST_CASE(matting_band_recovers_thin_strands_far_from_the_body) {
    // Dark strands one to two pixels wide leave a body and run across a light, gently textured background;
    // the coarse matte is the hard threshold of the truth. Global sampling explains the strand pixels with
    // body colour from the sure region and background colour from around them, however far the strand runs.
    const int w = 160, h = 120;
    struct Strand { double y0, slope; };
    const Strand strands[5] = {{20, 0.05}, {40, -0.08}, {60, 0.0}, {80, 0.12}, {100, -0.03}};
    auto truth = [&](int x, int y) {
        if (x < 40) return 1.0;
        double a = 0;
        for (const Strand& st : strands) {
            double cy = st.y0 + (x - 40) * st.slope;
            a = std::max(a, std::clamp(1.4 - std::fabs(y - cy), 0.0, 1.0));
        }
        return a;
    };
    auto image = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        const double a = truth(x, y);
        const double br = 205 + 20 * std::sin(x / 7.0), bg = 195 + 15 * std::cos(y / 5.0), bb = 175 + 10 * std::sin((x + y) / 9.0);
        uint8_t* p = image->pixel(x, y);
        p[0] = uint8_t(std::lround(60 * a + br * (1 - a))); p[1] = uint8_t(std::lround(40 * a + bg * (1 - a))); p[2] = uint8_t(std::lround(30 * a + bb * (1 - a))); p[3] = 255;
    }
    GrayImage coarse(w, h, 0);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) coarse.at(x, y) = truth(x, y) > 0.5 ? 255 : 0;
    auto matted = matteBand(coarse, *image, 8, 0);
    double total = 0; int count = 0;
    for (int y = 4; y < h - 4; y++) for (int x = 44; x < 150; x++) { total += std::fabs(matted->at(x, y) / 255.0 - truth(x, y)); count++; }
    const double mad = total / count;
    std::printf("  thin strands: mean absolute error %.4f\n", mad);
    CHECK(mad < 0.05);
    // Far along the strands, on each one and at the nearest empty pixel below it.
    for (const Strand& st : strands) {
        const int x = 140, cy = int(std::lround(st.y0 + (x - 40) * st.slope));
        CHECK(matted->at(x, cy) > 170);
        int empty = cy + 3;
        while (empty < h - 1 && truth(x, empty) > 0) empty++;
        CHECK(matted->at(x, empty) < 50);
    }
}

TEST_CASE(detail_windows_cover_the_uncertain_pixels) {
    // Two uncertain blobs in a 500 x 400 map; 128-pixel windows from a half-window grid cover both with
    // the busiest first, stay inside the image, and stop when nothing worth a window is left.
    GrayImage uncertain(500, 400, 0);
    for (int y = 40; y < 100; y++) for (int x = 60; x < 200; x++) uncertain.at(x, y) = 255;
    for (int y = 300; y < 330; y++) for (int x = 420; x < 480; x++) uncertain.at(x, y) = 255;
    auto windows = detailWindows(uncertain, 128, 16, 50);
    CHECK(windows.size() >= 3);
    CHECK(windows.size() <= 6);
    for (const DetailWindow& win : windows) { CHECK(win.x >= 0); CHECK(win.y >= 0); CHECK(win.x + win.size <= 500); CHECK(win.y + win.size <= 400); CHECK_EQ(win.size, 128); }
    // The first window takes the biggest bite of the large blob.
    CHECK(windows[0].y <= 40 && windows[0].y + 128 >= 100);
    for (int y = 0; y < 400; y++) for (int x = 0; x < 500; x++) {
        if (!uncertain.at(x, y)) continue;
        bool covered = false;
        for (const DetailWindow& win : windows) covered = covered || (x >= win.x && x < win.x + win.size && y >= win.y && y < win.y + win.size);
        CHECK(covered);
    }
    CHECK(detailWindows(uncertain, 600, 4, 1).empty());
    CHECK(detailWindows(GrayImage(500, 400, 0), 128, 4, 1).empty());
}

TEST_CASE(fuse_detail_keeps_the_coarse_shape_and_takes_the_local_edge) {
    // A blurry coarse step against a sharp local step with a thin gap in it: inside the weighted band the
    // result has the sharp edge and the gap, outside it is the coarse mask untouched.
    const int w = 200, h = 100;
    GrayImage coarse(w, h), local(w, h), weight(w, h, 0);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        coarse.at(x, y) = uint8_t(std::lround(255 * std::clamp((110 - x) / 20.0, 0.0, 1.0)));
        local.at(x, y) = (x < 100 && !(y >= 48 && y < 52)) ? 255 : 0;
        weight.at(x, y) = (x >= 70 && x < 130) ? 255 : 0;
    }
    auto out = fuseDetail(coarse, local, weight, 6);
    CHECK_EQ(int(out->at(20, 20)), 255);
    CHECK_EQ(int(out->at(180, 20)), 0);
    CHECK_EQ(int(out->at(65, 20)), int(coarse.at(65, 20)));
    CHECK(out->at(96, 20) > 200);
    CHECK(out->at(104, 20) < 60);
    CHECK(out->at(90, 50) < 90);   // the gap came from the local mask
    CHECK(out->at(90, 40) > 200);
    // Sigma 0: the local mask where it is sure, and the coarse one where only it is; a window's soft floor
    // (0.4 outside the subject) yields to the coarse pass's certain background.
    local.at(120, 20) = 100;
    local.at(80, 60) = 140;   // unsure, over a sure coarse foreground
    auto direct = fuseDetail(coarse, local, weight, 0);
    CHECK_EQ(int(direct->at(90, 50)), 0);
    CHECK_EQ(int(direct->at(99, 20)), 255);
    CHECK_EQ(int(direct->at(100, 20)), 0);
    CHECK_EQ(int(direct->at(65, 20)), int(coarse.at(65, 20)));
    CHECK_EQ(int(direct->at(120, 20)), 0);
    CHECK_EQ(int(direct->at(80, 60)), 255);
}

TEST_CASE(scribble_selection_finds_the_disc) {
    if (!scribbleSelectionSupported()) { std::fprintf(stderr, "  (skipped: no OpenCV)\n"); return; }
    // A red disc on a blue field, a foreground stroke across the disc and a background stroke in a corner.
    const int w = 200, h = 160;
    auto image = std::make_shared<Image>(w, h);
    auto inside = [](int x, int y) { return (x - 100) * (x - 100) + (y - 80) * (y - 80) < 50 * 50; };
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        uint8_t* p = image->pixel(x, y);
        const bool in = inside(x, y);
        p[0] = in ? 210 : 40; p[1] = in ? 50 : 60; p[2] = in ? 40 : 200; p[3] = 255;
    }
    GrayImage labels(w, h, 0);
    for (int y = 76; y < 84; y++) for (int x = 80; x < 120; x++) labels.at(x, y) = 1;
    for (int y = 10; y < 16; y++) for (int x = 10; x < 60; x++) labels.at(x, y) = 2;
    std::string error;
    auto coverage = scribbleSelection(*image, labels, 120, 3, &error);
    CHECK(coverage != nullptr);
    if (!coverage) { std::fprintf(stderr, "  %s\n", error.c_str()); return; }
    int agree = 0, total = 0;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) { agree += (coverage->at(x, y) >= 128) == inside(x, y); total++; }
    std::printf("  scribble disc: %.1f%% of pixels right\n", 100.0 * agree / total);
    CHECK(agree > total * 97 / 100);
    GrayImage none(w, h, 0);
    CHECK(scribbleSelection(*image, none, 120, 3, &error) == nullptr);
}

TEST_CASE(matte_cleanup_removes_speckle_but_keeps_the_edge) {
    // A subject on the left with a soft ramp at its edge, a soft speck floating in the background and a soft
    // patch inside the subject: the ramp stays, the speck goes transparent, the patch opaque.
    const int w = 100, h = 60;
    GrayImage m(w, h, 0);
    auto ramp = [](int x) { return x < 40 ? 255 : x < 48 ? 255 - (x - 39) * 28 : 0; };
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) m.at(x, y) = uint8_t(ramp(x));
    for (int y = 20; y < 24; y++) for (int x = 70; x < 74; x++) m.at(x, y) = 120;
    for (int y = 30; y < 34; y++) for (int x = 10; x < 14; x++) m.at(x, y) = 100;
    GrayImage original = m;
    cleanMatte(m);
    CHECK_EQ(int(m.at(71, 21)), 0);
    CHECK_EQ(int(m.at(11, 31)), 255);
    for (int x = 38; x < 50; x++) CHECK_EQ(int(m.at(x, 10)), ramp(x));
    CHECK_EQ(int(m.at(20, 10)), 255);
    CHECK_EQ(int(m.at(90, 10)), 0);
    // Through the settings: on by default, and off leaves the speck alone.
    auto guide = std::make_shared<Image>(w, h);
    guide->fill(128, 128, 128, 255);
    MatteSettings on{0, 0, 0, 0};
    CHECK_EQ(int(refineMatte(original, *guide, on, 0)->at(71, 21)), 0);
    MatteSettings off{0, 0, 0, 0, false};
    CHECK_EQ(int(refineMatte(original, *guide, off, 0)->at(71, 21)), 120);
    MatteSettings wide{0, 0, 0, 300};
    CHECK_NEAR(wide.normalized().matting, 300, 1e-9);
}

TEST_CASE(foreground_estimation_removes_the_background_tint) {
    // Red over blue through a 24-pixel ramp, with the true opacity as the matte: the estimated colour in the
    // band is the red, not the mix; opaque pixels and pixels away from the edge are untouched.
    const int w = 120, h = 60;
    auto image = std::make_shared<Image>(w, h);
    auto truth = [](int x) { return std::clamp((72 - x) / 24.0, 0.0, 1.0); };
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        double a = truth(x);
        uint8_t* p = image->pixel(x, y);
        p[0] = uint8_t(std::lround(220 * a + 30 * (1 - a))); p[1] = uint8_t(std::lround(40 * a + 60 * (1 - a))); p[2] = uint8_t(std::lround(30 * a + 200 * (1 - a))); p[3] = 255;
    }
    GrayImage matte(w, h);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) matte.at(x, y) = uint8_t(std::lround(truth(x) * 255));
    auto out = estimateForeground(*image, matte);
    auto same = [&](int x, int y) { const uint8_t *a = out->pixel(x, y), *b = image->pixel(x, y); return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]; };
    CHECK(same(10, 30));
    CHECK(same(47, 30));
    CHECK(same(80, 30));
    for (int x = 52; x <= 70; x += 6) {
        const uint8_t* p = out->pixel(x, 30);
        CHECK(p[0] >= 190);
        CHECK(p[2] <= 60);
        CHECK_EQ(int(p[3]), 255);
    }
    // The first transparent pixel past the ramp is re-coloured too (a resampled mask blends it in).
    CHECK(!same(72, 30));
}

TEST_CASE(subject_mask_from_model_when_available) {
    const char* dir = std::getenv("COMPOSITOR_MODEL_DIR");
    if (!dir || !subjectModelSupported()) { std::fprintf(stderr, "  (skipped: set COMPOSITOR_MODEL_DIR with u2netp.onnx to run)\n"); return; }
    std::string path = std::string(dir) + "/u2netp.onnx";
    if (!fs::exists(path)) { std::fprintf(stderr, "  (skipped: %s not present)\n", path.c_str()); return; }
    // A bright disc on a dark field: the disc is the subject.
    auto img = std::make_shared<Image>(160, 160);
    for (int y = 0; y < 160; y++) for (int x = 0; x < 160; x++) { bool in = std::hypot(x - 80, y - 80) < 45; uint8_t* p = img->pixel(x, y); p[0] = in ? 230 : 30; p[1] = in ? 200 : 40; p[2] = in ? 120 : 60; p[3] = 255; }
    std::string error;
    auto mask = subjectMask(*img, path, &error);
    REQUIRE(mask != nullptr);
    CHECK(mask->at(80, 80) > mask->at(5, 5));
}

TEST_CASE(subject_masks_on_two_threads_match_the_masks_run_alone) {
    // Runs on two threads share one network. forward() into a list copies its outputs out of the network's
    // buffers, so what a run reads after the model lock is released is its own; this keeps it that way.
    const char* dir = std::getenv("COMPOSITOR_MODEL_DIR");
    if (!dir || !subjectModelSupported()) { std::fprintf(stderr, "  (skipped: set COMPOSITOR_MODEL_DIR with u2netp.onnx to run)\n"); return; }
    std::string path = std::string(dir) + "/u2netp.onnx";
    if (!fs::exists(path)) { std::fprintf(stderr, "  (skipped: %s not present)\n", path.c_str()); return; }
    auto disc = [](int cx, int cy) {
        auto img = std::make_shared<Image>(200, 160);
        for (int y = 0; y < 160; y++) for (int x = 0; x < 200; x++) { bool in = std::hypot(x - cx, y - cy) < 40; uint8_t* p = img->pixel(x, y); p[0] = in ? 230 : 30; p[1] = in ? 200 : 40; p[2] = in ? 120 : 60; p[3] = 255; }
        return img;
    };
    const std::shared_ptr<Image> images[2] = {disc(60, 60), disc(140, 100)};
    std::shared_ptr<GrayImage> alone[2];
    for (int i = 0; i < 2; i++) { alone[i] = subjectMask(*images[i], path, nullptr); REQUIRE(alone[i] != nullptr); }
    auto same = [](const GrayImage& a, const GrayImage& b) {
        for (int y = 0; y < a.height(); y++) if (std::memcmp(a.row(y), b.row(y), size_t(a.width()))) return false;
        return true;
    };
    std::atomic<int> mismatches{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 2; i++)
        threads.emplace_back([&, i] {
            for (int run = 0; run < 12; run++) {
                auto mask = subjectMask(*images[i], path, nullptr);
                if (!mask || !same(*mask, *alone[i])) mismatches++;
            }
        });
    for (auto& t : threads) t.join();
    CHECK_EQ(mismatches.load(), 0);

    // The same for click prompts, whose outputs are read for longer after the run.
    std::string prompt = std::string(dir) + "/efficientsam_ti_2025april.onnx";
    if (!fs::exists(prompt)) { std::fprintf(stderr, "  (prompts skipped: %s not present)\n", prompt.c_str()); return; }
    const std::vector<PointPrompt> clicks[2] = {{{60, 60, 1}}, {{140, 100, 1}}};
    for (int i = 0; i < 2; i++) { alone[i] = subjectFromPrompts(*images[i], prompt, clicks[i], nullptr); REQUIRE(alone[i] != nullptr); }
    threads.clear();
    for (int i = 0; i < 2; i++)
        threads.emplace_back([&, i] {
            for (int run = 0; run < 12; run++) {
                auto mask = subjectFromPrompts(*images[i], prompt, clicks[i], nullptr);
                if (!mask || !same(*mask, *alone[i])) mismatches++;
            }
        });
    for (auto& t : threads) t.join();
    CHECK_EQ(mismatches.load(), 0);
}

TEST_CASE(subject_from_prompts_when_the_model_is_available) {
    const char* dir = std::getenv("COMPOSITOR_MODEL_DIR");
    if (!dir || !subjectModelSupported()) { std::fprintf(stderr, "  (skipped: set COMPOSITOR_MODEL_DIR with efficientsam_ti_2025april.onnx to run)\n"); return; }
    std::string path = std::string(dir) + "/efficientsam_ti_2025april.onnx";
    if (!fs::exists(path)) { std::fprintf(stderr, "  (skipped: %s not present)\n", path.c_str()); return; }
    CHECK(promptModelPath(path));
    CHECK(!promptModelPath("isnet-general-use.onnx"));
    // Two discs on a dark field: a click on the left one selects it alone; a second click on the right one
    // with the first as a negative point selects the right one alone.
    auto img = std::make_shared<Image>(320, 200);
    for (int y = 0; y < 200; y++) for (int x = 0; x < 320; x++) {
        bool a = std::hypot(x - 90, y - 100) < 50, b = std::hypot(x - 230, y - 100) < 50;
        uint8_t* p = img->pixel(x, y);
        p[0] = a ? 230 : b ? 60 : 30; p[1] = a ? 200 : b ? 200 : 40; p[2] = a ? 120 : b ? 230 : 60; p[3] = 255;
    }
    std::string error;
    auto left = subjectFromPrompts(*img, path, {{90, 100, 1}}, &error);
    REQUIRE(left != nullptr);
    CHECK(left->at(90, 100) > 200);
    CHECK(left->at(230, 100) < 50);
    CHECK(left->at(10, 10) < 50);
    // A negative point is best-effort in this export (two flat discs read as one object to it), so only the
    // positive side is asserted.
    auto right = subjectFromPrompts(*img, path, {{230, 100, 1}, {90, 100, 0}}, &error);
    REQUIRE(right != nullptr);
    CHECK(right->at(230, 100) > 200);
    CHECK(right->at(10, 10) < 50);
    CHECK(subjectFromPrompts(*img, path, {}, &error) == nullptr);
}

TEST_CASE(image_sizes_beyond_the_buffer_limit_are_empty_rather_than_short) {
    // A width whose byte stride overflowed an int used to yield a four-byte buffer that claimed to be a
    // billion pixels wide; every write through it then ran off the heap.
    Image wrapping(0x40000001, 1);
    CHECK(wrapping.isEmpty());
    CHECK_EQ(int(wrapping.byteCount()), 0);
    Image justOver(maxImageSide + 1, 4);
    CHECK(justOver.isEmpty());
    Image negative(-5, 4);
    CHECK(negative.isEmpty());
    GrayImage grayWrapping(0x40000001, 1);
    CHECK(grayWrapping.isEmpty());
    // The limit itself still holds pixels, and the stride always matches the width it reports.
    Image atLimit(maxImageSide, 2);
    CHECK_EQ(atLimit.width(), maxImageSide);
    CHECK_EQ(atLimit.stride(), maxImageSide * 4);
    CHECK(atLimit.byteCount() == size_t(maxImageSide) * 4 * 2);
    GrayImage grayAtLimit(maxImageSide, 2);
    CHECK(grayAtLimit.byteCount() == size_t(maxImageSide) * 2);
}

TEST_MAIN()
