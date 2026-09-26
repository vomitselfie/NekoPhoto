// Smart Filter kernels and the stack renderer (smartfilter_render.cpp, a port of Patchy's smart_filter_renderer).
// The expected values are Patchy's own pins (tests/core/smart_filter_pixels_tests.cpp), which it measured against
// Photoshop 27.8, and the calibration facts in Patchy's docs/smart-filters-native.md.
#include "check.h"
#include "compositor/smartfilter.h"
#include "compositor/png.h"
#include "compositor/psd.h"
#include "compositor/psd_writer.h"
#include "compositor/render.h"
#include "compositor/smartobject_edit.h"

#include <array>
#include <cstdlib>
#include <vector>

using namespace compositor;

namespace {

/// Premultiplied pixels filled with one straight colour.
std::shared_ptr<Image> solid(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    auto image = std::make_shared<Image>(w, h);
    image->fill(uint8_t(r * a / 255), uint8_t(g * a / 255), uint8_t(b * a / 255), a);
    return image;
}

void put(Image& image, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    uint8_t* p = image.pixel(x, y);
    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}

PlacedRaster placed(std::shared_ptr<Image> image, int x, int y) { return PlacedRaster{std::move(image), x, y}; }

const uint8_t* at(const PlacedRaster& r, int documentX, int documentY) {
    const int lx = documentX - r.x, ly = documentY - r.y;
    if (!r.image || lx < 0 || ly < 0 || lx >= r.image->width() || ly >= r.image->height()) {
        static const uint8_t none[4] = {0, 0, 0, 0};
        return none;
    }
    return r.image->pixel(lx, ly);
}

SmartFilterStack stackOf(SmartFilterParameters parameters, double opacity = 1, BlendMode blend = BlendMode::Normal) {
    SmartFilterStack stack;
    stack.supported = true;
    SmartFilterEntry entry;
    entry.parameters = parameters;
    entry.opacity = opacity;
    entry.blend = blend;
    stack.entries.push_back(entry);
    return stack;
}

} // namespace

TEST_CASE(gaussian_single_pixel_radius_half_is_55_145_55) {
    // docs/smart-filters-native.md: radius 0.5 is bytes [55,145,55], not a true Gaussian.
    auto out = smartGaussianBlur(placed(solid(1, 1, 255, 255, 255, 255), 10, 10), PixelRect{0, 0, 20, 20}, 0.5);
    CHECK(out.bounds() == (PixelRect{9, 9, 3, 3}));
    CHECK_EQ(int(at(out, 10, 10)[3]), 82);   // separable: 145 x 145 / 255, byte-rounded after each pass
    // One row through the horizontal pass alone: a one-pixel-tall canvas repeats its edge vertically.
    auto row = smartGaussianBlur(placed(solid(1, 1, 255, 255, 255, 255), 10, 0), PixelRect{0, 0, 20, 1}, 0.5);
    CHECK(row.bounds() == (PixelRect{9, 0, 3, 1}));
    CHECK_EQ(int(at(row, 9, 0)[3]), 55);
    CHECK_EQ(int(at(row, 10, 0)[3]), 145);
    CHECK_EQ(int(at(row, 11, 0)[3]), 55);
    CHECK_EQ(int(at(row, 9, 0)[0]), 55);   // premultiplied white: colour equals alpha
}

TEST_CASE(gaussian_matches_photoshop_calibrated_kernels) {
    struct KernelCase { double radius; std::vector<uint8_t> alpha; int verticalSupport; };
    const std::vector<KernelCase> cases{
        {0.5, {55, 145, 55}, 1},
        {1.0, {2, 18, 60, 96, 60, 18, 2}, 3},
        {2.5, {1, 5, 11, 20, 31, 39, 42, 39, 31, 20, 11, 5, 1}, 5},
        {4.5, {1, 2, 3, 5, 7, 9, 12, 16, 19, 21, 22, 22, 22, 21, 19, 16, 12, 9, 7, 5, 3, 2, 1}, 9},
    };
    const int size = 65, lineX = 32, top = 16, bottom = 48;
    const PixelRect source{17, 29, size, size};
    for (const auto& c : cases) {
        auto image = solid(size, size, 0, 0, 0, 0);
        for (int y = top; y <= bottom; y++) put(*image, lineX, y, 255, 255, 255);
        auto out = renderSmartFilterStack(placed(image, source.x, source.y), source, stackOf(smartfilter::GaussianBlur{c.radius}));
        REQUIRE(out);
        const int support = int(c.alpha.size() / 2);
        CHECK(out->bounds() == (PixelRect{source.x + lineX - support, source.y + top - c.verticalSupport, int(c.alpha.size()),
                                           bottom - top + 1 + c.verticalSupport * 2}));
        const int sampleY = source.y + (top + bottom) / 2;
        for (size_t i = 0; i < c.alpha.size(); i++) {
            const uint8_t* p = at(*out, out->x + int(i), sampleY);
            CHECK_EQ(int(p[3]), int(c.alpha[i]));
            CHECK_EQ(int(p[0]), int(c.alpha[i]));   // white, premultiplied
        }
    }
}

TEST_CASE(gaussian_grows_inside_the_canvas_repeats_its_edges_and_trims) {
    // The canvas is the placed pixel itself: the edge repeats, so an opaque pixel stays opaque.
    auto edge = renderSmartFilterStack(placed(solid(1, 1, 255, 255, 255, 255), 8, 11), PixelRect{8, 11, 1, 1},
                                       stackOf(smartfilter::GaussianBlur{1.0}));
    REQUIRE(edge);
    CHECK(edge->bounds() == (PixelRect{8, 11, 1, 1}));
    CHECK_EQ(int(at(*edge, 8, 11)[3]), 255);
    // A document canvas: the pixel grows to radius 1's measured support and is trimmed to it.
    auto grown = renderSmartFilterStack(placed(solid(1, 1, 255, 255, 255, 255), 8, 11), PixelRect{0, 0, 20, 20},
                                        stackOf(smartfilter::GaussianBlur{1.0}));
    REQUIRE(grown);
    CHECK(grown->bounds() == (PixelRect{5, 8, 7, 7}));
    // Growth stops at the canvas: at its corner the blur keeps inside it.
    auto corner = renderSmartFilterStack(placed(solid(1, 1, 255, 255, 255, 255), 0, 0), PixelRect{0, 0, 20, 20},
                                         stackOf(smartfilter::GaussianBlur{1.0}));
    REQUIRE(corner);
    CHECK(corner->bounds() == (PixelRect{0, 0, 4, 4}));
    // A half plane at radius 4.5: the sub-byte tails reach alpha 1 eleven pixels out.
    auto half = solid(65, 65, 0, 0, 0, 0);
    for (int y = 0; y < 65; y++)
        for (int x = 0; x <= 31; x++) put(*half, x, y, 255, 255, 255);
    auto step = renderSmartFilterStack(placed(half, 0, 0), PixelRect{0, 0, 65, 65}, stackOf(smartfilter::GaussianBlur{4.5}));
    REQUIRE(step);
    CHECK(step->bounds() == (PixelRect{0, 0, 44, 65}));
    CHECK_EQ(int(at(*step, 32, 32)[3]), 116);
    CHECK_EQ(int(at(*step, 43, 32)[3]), 1);
    // Premultiplied blur: every visible halo pixel of a white point stays white.
    auto point = solid(9, 9, 0, 0, 0, 0);
    put(*point, 4, 4, 255, 255, 255);
    auto halo = renderSmartFilterStack(placed(point, 20, 30), PixelRect{20, 30, 9, 9}, stackOf(smartfilter::GaussianBlur{1.0}));
    REQUIRE(halo);
    CHECK(halo->bounds() == (PixelRect{21, 31, 7, 7}));
    bool sawTransparent = false;
    for (int y = 0; y < halo->image->height(); y++)
        for (int x = 0; x < halo->image->width(); x++) {
            const uint8_t* p = halo->image->pixel(x, y);
            if (p[3] == 0) sawTransparent = true;
            CHECK(p[0] == p[3] && p[1] == p[3] && p[2] == p[3]);
        }
    CHECK(sawTransparent);
}

TEST_CASE(gaussian_radius_one_and_a_half_impulse) {
    auto impulse = solid(9, 1, 0, 0, 0, 0);
    put(*impulse, 4, 0, 255, 255, 255);
    auto out = renderSmartFilterStack(placed(impulse, 0, 0), PixelRect{0, 0, 9, 1}, stackOf(smartfilter::GaussianBlur{1.5}));
    REQUIRE(out);
    CHECK(out->bounds() == (PixelRect{0, 0, 9, 1}));
    const std::array<int, 9> expected{2, 10, 28, 52, 72, 52, 28, 10, 2};
    for (int x = 0; x < 9; x++) CHECK_EQ(int(at(*out, x, 0)[3]), expected[size_t(x)]);
}

TEST_CASE(median_keeps_bounds_and_removes_an_outlier) {
    auto image = solid(5, 5, 40, 80, 120, 255);
    put(*image, 2, 2, 250, 0, 10);
    auto out = smartMedian(placed(image, 3, 4), 1.0);
    CHECK(out.bounds() == (PixelRect{3, 4, 5, 5}));
    for (int y = 0; y < 5; y++)
        for (int x = 0; x < 5; x++) {
            const uint8_t* p = out.image->pixel(x, y);
            CHECK(p[0] == 40 && p[1] == 80 && p[2] == 120 && p[3] == 255);
        }
    // A transparent surround keeps its bounds too (no growth, no trim).
    auto sparse = solid(7, 7, 0, 0, 0, 0);
    put(*sparse, 3, 3, 200, 100, 50);
    auto kept = smartMedian(placed(sparse, 0, 0), 2.7);
    CHECK(kept.bounds() == (PixelRect{0, 0, 7, 7}));
    CHECK_EQ(int(kept.image->pixel(3, 3)[3]), 0);   // alpha medians independently: one opaque pixel of 25 goes
}

TEST_CASE(dust_and_scratches_replaces_only_past_the_threshold) {
    auto image = solid(5, 5, 100, 100, 100, 255);
    put(*image, 2, 2, 130, 110, 100);   // largest channel difference from the median: 30
    auto replaced = smartDustAndScratches(placed(image, 0, 0), 1, 29);
    CHECK_EQ(int(replaced.image->pixel(2, 2)[0]), 100);
    CHECK_EQ(int(replaced.image->pixel(2, 2)[1]), 100);
    auto kept = smartDustAndScratches(placed(image, 0, 0), 1, 30);   // "exceeds": 30 is not > 30
    CHECK_EQ(int(kept.image->pixel(2, 2)[0]), 130);
    CHECK_EQ(int(kept.image->pixel(2, 2)[1]), 110);
    CHECK(kept.bounds() == (PixelRect{0, 0, 5, 5}));
    CHECK_EQ(int(kept.image->pixel(2, 2)[3]), 255);
}

TEST_CASE(mosaic_writes_block_averages_on_a_local_grid) {
    auto image = std::make_shared<Image>(5, 2);
    // Opaque values: block 0 (x 0..1) averages 10,20,30,41 -> 25.25 -> 25; block 1 (x 2..3) 100,100,200,201 -> 150.25 -> 150.
    const int values[2][5] = {{10, 20, 100, 100, 7}, {30, 41, 200, 201, 9}};
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 5; x++) put(*image, x, y, uint8_t(values[y][x]), 0, 0);
    auto out = smartMosaic(placed(image, 7, 3), 2);
    CHECK(out.bounds() == (PixelRect{7, 3, 5, 2}));
    for (int y = 0; y < 2; y++) {
        CHECK_EQ(int(out.image->pixel(0, y)[0]), 25);
        CHECK_EQ(int(out.image->pixel(1, y)[0]), 25);
        CHECK_EQ(int(out.image->pixel(2, y)[0]), 150);
        CHECK_EQ(int(out.image->pixel(3, y)[0]), 150);
        CHECK_EQ(int(out.image->pixel(4, y)[0]), 8);   // the partial last column: (7 + 9) / 2
    }
    // Alpha averages too, and colour is weighted by it.
    auto mixed = solid(2, 1, 0, 0, 0, 0);
    put(*mixed, 0, 0, 200, 0, 0, 255);
    auto half = smartMosaic(placed(mixed, 0, 0), 2);
    CHECK_EQ(int(half.image->pixel(1, 0)[3]), 128);
    CHECK_NEAR(double(half.image->pixel(1, 0)[0]), 200.0 * 128 / 255, 1.0);
}

TEST_CASE(unsharp_mask_matches_photoshop_impulse_and_threshold) {
    auto impulse = solid(65, 1, 128, 128, 128, 255);
    put(*impulse, 32, 0, 255, 255, 255);
    auto out = smartUnsharpMask(placed(impulse, 0, 0), 100, 2.5, 0);
    const std::array<int, 13> expected{127, 126, 122, 118, 113, 109, 255, 109, 113, 118, 122, 126, 127};
    for (int x = 0; x < 65; x++) {
        const int local = x - 26;
        const int want = local >= 0 && local < 13 ? expected[size_t(local)] : 128;
        CHECK_EQ(int(out.image->pixel(x, 0)[0]), want);
    }
    auto source = solid(65, 1, 128, 128, 128, 255);
    put(*source, 31, 0, 134, 134, 134);
    put(*source, 32, 0, 142, 142, 142);
    put(*source, 33, 0, 134, 134, 134);
    auto thresholded = renderSmartFilterStack(placed(source, 0, 0), PixelRect{0, 0, 65, 1}, stackOf(smartfilter::UnsharpMask{175, 2.5, 7}));
    REQUIRE(thresholded);
    CHECK(thresholded->bounds() == (PixelRect{0, 0, 65, 1}));
    for (int x = 0; x < 65; x++) {
        const int want = x == 31 || x == 33 ? 134 : x == 32 ? 152 : 128;
        CHECK_EQ(int(at(*thresholded, x, 0)[0]), want);
    }
}

TEST_CASE(motion_blur_axis_kernel_and_growth) {
    auto impulse = solid(65, 1, 0, 0, 0, 255);
    put(*impulse, 32, 0, 255, 255, 255);
    auto out = smartMotionBlur(placed(impulse, 0, 0), PixelRect{0, 0, 65, 1}, 0, 12);
    for (int x = 0; x < 65; x++) CHECK_EQ(int(at(out, x, 0)[0]), x >= 26 && x <= 38 ? 20 : 0);
    auto grown = smartMotionBlur(placed(solid(1, 1, 255, 255, 255, 255), 32, 0), PixelRect{0, 0, 65, 1}, 0, 12);
    CHECK(grown.bounds() == (PixelRect{26, 0, 13, 1}));
    for (int x = 26; x <= 38; x++) CHECK_EQ(int(at(grown, x, 0)[3]), 20);
}

TEST_CASE(add_noise_is_deterministic_and_keeps_alpha) {
    auto image = std::make_shared<Image>(9, 7);
    for (int y = 0; y < 7; y++)
        for (int x = 0; x < 9; x++) put(*image, x, y, uint8_t(20 + 23 * x), uint8_t(10 + 31 * y), uint8_t(200 - 11 * x));
    auto a = smartAddNoise(placed(image, 3, 4), 25.5, true, true, 77);
    auto b = smartAddNoise(placed(image, 3, 4), 25.5, true, true, 77);
    auto c = smartAddNoise(placed(image, 3, 4), 25.5, true, true, 78);
    CHECK(*a.image == *b.image);
    CHECK(!(*a.image == *c.image));
    CHECK(a.bounds() == (PixelRect{3, 4, 9, 7}));
    bool changed = false;
    for (int y = 0; y < 7; y++)
        for (int x = 0; x < 9; x++) {
            const uint8_t* before = image->pixel(x, y);
            const uint8_t* after = a.image->pixel(x, y);
            CHECK_EQ(int(after[3]), 255);
            const int dr = int(after[0]) - before[0], dg = int(after[1]) - before[1];
            if (after[0] != 0 && after[0] != 255 && after[1] != 0 && after[1] != 255) CHECK_EQ(dr, dg);
            changed = changed || dr != 0;
        }
    CHECK(changed);
}

TEST_CASE(radial_blur_keeps_a_uniform_field_and_its_centre) {
    auto image = solid(9, 7, 90, 60, 30, 255);
    for (int samples : {8, 16, 32}) {
        auto out = smartRadialBlur(placed(image, 0, 0), PixelRect{0, 0, 9, 7}, 42, samples);
        CHECK(out.bounds() == (PixelRect{0, 0, 9, 7}));
        CHECK(*out.image == *image);
    }
}

TEST_CASE(stack_opacity_disabled_entries_and_the_shared_mask) {
    // A white column on black; Gaussian radius 1 takes the column to 96 (docs: kernel 2,18,60,96,...).
    auto source = solid(5, 5, 0, 0, 0, 255);
    for (int y = 0; y < 5; y++) put(*source, 2, y, 255, 255, 255);
    const PixelRect bounds{10, 20, 5, 5};
    const auto in = placed(source, bounds.x, bounds.y);
    const auto stack = stackOf(smartfilter::GaussianBlur{1.0});
    auto unmasked = renderSmartFilterStack(in, bounds, stack);
    REQUIRE(unmasked);
    CHECK_EQ(int(at(*unmasked, 12, 22)[0]), 96);

    // 50% Normal: halfway between the unfiltered 255 and the filtered 96.
    auto half = renderSmartFilterStack(in, bounds, stackOf(smartfilter::GaussianBlur{1.0}, 0.5));
    REQUIRE(half);
    CHECK_NEAR(double(at(*half, 12, 22)[0]), 175.5, 1.0);
    CHECK_EQ(int(at(*half, 12, 22)[3]), 255);

    // All-white mask: unchanged from no mask.
    auto white = stack;
    white.mask = std::make_shared<GrayImage>(5, 5, 255);
    white.maskBounds = bounds;
    white.maskDefault = 255;
    auto whiteOut = renderSmartFilterStack(in, bounds, white);
    REQUIRE(whiteOut);
    CHECK(whiteOut->bounds() == unmasked->bounds() && *whiteOut->image == *unmasked->image);

    // Black mask, black past its bounds: the unfiltered pixels.
    auto black = stack;
    black.mask = std::make_shared<GrayImage>(5, 5, 0);
    black.maskBounds = bounds;
    black.maskDefault = 0;
    auto blackOut = renderSmartFilterStack(in, bounds, black);
    REQUIRE(blackOut);
    CHECK(blackOut->bounds() == bounds);
    CHECK(*blackOut->image == *source);

    // A 50% gray mask pixel mixes once, at the end: (255 * 127 + 96 * 128) / 255 = 175.
    auto gray = black;
    auto grayMask = std::make_shared<GrayImage>(5, 5, 0);
    grayMask->at(2, 2) = 128;
    gray.mask = grayMask;
    auto grayOut = renderSmartFilterStack(in, bounds, gray);
    REQUIRE(grayOut);
    CHECK_EQ(int(at(*grayOut, 12, 22)[0]), 175);
    CHECK_EQ(int(at(*grayOut, 12, 22)[3]), 255);
    CHECK_EQ(int(at(*grayOut, 12, 21)[0]), 255);

    // The mask's default applies past its bounds: a 1x1 black mask, white elsewhere.
    auto corner = stack;
    corner.mask = std::make_shared<GrayImage>(1, 1, 0);
    corner.maskBounds = PixelRect{12, 22, 1, 1};
    corner.maskDefault = 255;
    auto cornerOut = renderSmartFilterStack(in, bounds, corner);
    REQUIRE(cornerOut);
    CHECK_EQ(int(at(*cornerOut, 12, 22)[0]), 255);
    CHECK_EQ(int(at(*cornerOut, 12, 21)[0]), 96);

    // A disabled mask is no mask.
    auto disabledMask = black;
    disabledMask.maskEnabled = false;
    auto disabledMaskOut = renderSmartFilterStack(in, bounds, disabledMask);
    REQUIRE(disabledMaskOut);
    CHECK(*disabledMaskOut->image == *unmasked->image);

    // A disabled entry is skipped: the pixels come back as they were.
    auto disabledEntry = stack;
    disabledEntry.entries.front().enabled = false;
    auto entryOut = renderSmartFilterStack(in, bounds, disabledEntry);
    REQUIRE(entryOut);
    CHECK(entryOut->bounds() == bounds);
    CHECK(*entryOut->image == *source);

    // Two entries, the second disabled: only the first runs.
    auto twoEntries = stack;
    SmartFilterEntry off;
    off.parameters = smartfilter::Mosaic{4};
    off.enabled = false;
    twoEntries.entries.push_back(off);
    auto twoOut = renderSmartFilterStack(in, bounds, twoEntries);
    REQUIRE(twoOut);
    CHECK(*twoOut->image == *unmasked->image);

    // A disabled stack returns the placed pixels unchanged.
    auto disabledStack = stack;
    disabledStack.enabled = false;
    auto stackOut = renderSmartFilterStack(in, bounds, disabledStack);
    REQUIRE(stackOut);
    CHECK(stackOut->image == source);
}

TEST_CASE(stack_refuses_what_it_cannot_draw) {
    const auto in = placed(solid(3, 3, 1, 2, 3, 255), 0, 0);
    auto unsupported = stackOf(smartfilter::GaussianBlur{1.0});
    unsupported.supported = false;
    CHECK(!renderSmartFilterStack(in, PixelRect{0, 0, 3, 3}, unsupported));
    auto unknown = stackOf(smartfilter::GaussianBlur{1.0});
    unknown.entries.push_back(SmartFilterEntry{});   // monostate
    CHECK(!renderSmartFilterStack(in, PixelRect{0, 0, 3, 3}, unknown));
}

TEST_CASE(stack_blend_mode_uses_the_core_blend) {
    // Multiply of the blurred result over the unfiltered pixels, at full opacity: 255 * 96 / 255 on the column.
    auto source = solid(5, 5, 0, 0, 0, 255);
    for (int y = 0; y < 5; y++) put(*source, 2, y, 255, 255, 255);
    auto out = renderSmartFilterStack(placed(source, 0, 0), PixelRect{0, 0, 5, 5},
                                      stackOf(smartfilter::GaussianBlur{1.0}, 1.0, BlendMode::Multiply));
    REQUIRE(out);
    CHECK_NEAR(double(at(*out, 2, 2)[0]), 96.0, 1.0);
    CHECK_EQ(int(at(*out, 0, 2)[0]), 0);
}

namespace {

Document filteredDocument() {
    Document doc(60, 40);
    SmartObjectContents c;
    auto image = std::make_shared<Image>(20, 10);
    image->fill(0, 200, 0, 255);
    c.image = image;
    encodePngImage(*c.image, c.bytes);
    c.fileName = "chip.png";
    auto source = makeSmartObjectSource(std::move(c));
    doc.smartObjects[source->id] = source;
    doc.layers.push_back(smartObjectLayer(source, {20, 15, 40, 15, 40, 25, 20, 25}, "Chip"));
    return doc;
}

} // namespace

TEST_CASE(edit_reorder_and_mask_a_stack_then_round_trip) {
    Document doc = filteredDocument();
    std::string error;
    SmartFilterEntry blur, mosaic;
    blur.parameters = smartfilter::GaussianBlur{2};
    mosaic.parameters = smartfilter::Mosaic{4};
    REQUIRE(addSmartFilter(doc, doc.layers[0], blur, &error));
    REQUIRE(addSmartFilter(doc, doc.layers[0], mosaic, &error));
    auto stack = smartFilterStackOf(doc, doc.layers[0]);
    REQUIRE(stack && stack->supported && stack->entries.size() == 2);
    CHECK(!stack->mask || stack->mask->at(0, 0) == 255);
    // Reorder, edit, switch off, blend, a mask and the stack's switches.
    std::swap(stack->entries[0], stack->entries[1]);
    stack->entries[1].parameters = smartfilter::GaussianBlur{5};
    stack->entries[1].opacity = 0.5;
    stack->entries[1].blend = BlendMode::Multiply;
    stack->entries[0].enabled = false;
    auto mask = std::make_shared<GrayImage>(60, 40, 255);
    for (int y = 0; y < 40; y++) for (int x = 0; x < 30; x++) mask->at(x, y) = 0;
    stack->mask = mask;
    stack->maskBounds = {0, 0, 60, 40};
    stack->maskEnabled = false;
    REQUIRE(setSmartFilters(doc, doc.layers[0], *stack, &error));
    auto check = [&](const Document& d) {
        auto again = smartFilterStackOf(d, d.layers[0]);
        REQUIRE(again && again->supported && again->entries.size() == 2);
        CHECK(std::holds_alternative<smartfilter::Mosaic>(again->entries[0].parameters) && !again->entries[0].enabled);
        CHECK(again->entries[1].parameters == SmartFilterParameters(smartfilter::GaussianBlur{5}));
        CHECK(std::abs(again->entries[1].opacity - 0.5) < 1e-6 && again->entries[1].blend == BlendMode::Multiply);
        CHECK(!again->maskEnabled && again->enabled);
        REQUIRE(again->mask);
        const int lx = 10 - again->maskBounds.x, rx = 50 - again->maskBounds.x, ly = 20 - again->maskBounds.y;
        CHECK_EQ(int(again->mask->at(lx, ly)), 0);
        CHECK_EQ(int(again->mask->at(rx, ly)), 255);
    };
    check(doc);
    // Through PSD, the stack and its mask as they were.
    auto bytes = encodePsd(doc, {}, nullptr, &error);
    auto back = importPsdBytes(bytes, &error);
    REQUIRE(back.has_value());
    REQUIRE(back->document.layers[0].isLiveSmartObject() && !back->document.layers[0].smartObject->locked());
    check(back->document);
    // Out-of-range settings are held to what a file may carry.
    stack = smartFilterStackOf(doc, doc.layers[0]);
    stack->entries[1].parameters = smartfilter::GaussianBlur{5000};
    REQUIRE(setSmartFilters(doc, doc.layers[0], *stack, &error));
    CHECK(smartFilterStackOf(doc, doc.layers[0])->entries[1].parameters == SmartFilterParameters(smartfilter::GaussianBlur{1000}));
    // An entry not drawn here is refused.
    stack->entries[0].parameters = std::monostate{};
    CHECK(!setSmartFilters(doc, doc.layers[0], *stack, &error));
}

TEST_CASE(clearing_a_stack_removes_its_filterfx_and_cache_record) {
    Document doc = filteredDocument();
    std::string error;
    SmartFilterEntry blur;
    blur.parameters = smartfilter::GaussianBlur{2};
    REQUIRE(addSmartFilter(doc, doc.layers[0], blur, &error));
    const std::string placedId = doc.layers[0].smartObject->placedId;
    REQUIRE(setSmartFilters(doc, doc.layers[0], SmartFilterStack{}, &error));
    const Layer& layer = doc.layers[0];
    CHECK(layer.isLiveSmartObject() && !layer.smartObject->locked());
    CHECK(!smartObjectFiltered(*layer.smartObject));
    CHECK(!smartFilterStackOf(doc, layer).has_value());
    for (const PsdBlock& b : doc.psdCarry->globals) CHECK(b.key != "FEid" && b.key != "FXid");
    CHECK(!findSmartFilterCache(doc.psdCarry->globals, placedId).has_value());
    // Drawn unfiltered: nothing past its edge.
    auto out = renderFlattened(doc);
    CHECK_EQ(int(out->pixel(18, 20)[3]), 0);
    CHECK_EQ(int(out->pixel(25, 20)[3]), 255);
    auto bytes = encodePsd(doc, {}, nullptr, &error);
    auto back = importPsdBytes(bytes, &error);
    REQUIRE(back.has_value());
    CHECK(back->document.layers[0].isLiveSmartObject() && !smartObjectFiltered(*back->document.layers[0].smartObject));
    // And a filter can go on again.
    REQUIRE(addSmartFilter(doc, doc.layers[0], blur, &error));
    CHECK(smartFilterStackOf(doc, doc.layers[0])->supported);
}

TEST_MAIN()
