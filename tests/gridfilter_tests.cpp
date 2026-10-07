// The Filter menu's grid filters (filters.h, applyGridFilter): the Smart Filter kernels run destructively, and the
// filters ported from PhotoCraft. Integer-exact filters are compared by hash everywhere; the ones through sin, cos,
// atan2 and pow by hash on Linux only (MinGW's maths library rounds them differently) and by sampled values within a
// level everywhere.
#include "check.h"
#include "compositor/depth.h"
#include "compositor/filters.h"
#include "compositor/smartfilter.h"
#include "compositor/supports.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

using namespace compositor;

namespace {

/// A deterministic test card: gradients, a disc and a hard-edged square, partly transparent at the left.
std::shared_ptr<Image> card(int w = 64, int h = 48) {
    auto img = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t* p = img->pixel(x, y);
            const int a = x < 6 ? 128 : 255;
            int r = x * 255 / (w - 1), g = y * 255 / (h - 1), b = ((x / 8 + y / 8) % 2) * 200;
            const int dx = x - 40, dy = y - 20;
            if (dx * dx + dy * dy < 100) { r = 250; g = 30; b = 40; }
            if (x >= 10 && x < 22 && y >= 28 && y < 40) { r = 10; g = 10; b = 10; }
            p[0] = uint8_t(r * a / 255); p[1] = uint8_t(g * a / 255); p[2] = uint8_t(b * a / 255); p[3] = uint8_t(a);
        }
    return img;
}

uint64_t fnv(const void* data, size_t bytes, uint64_t h = 1469598103934665603ull) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

uint64_t hashOf(const AnyImage& image) {
    if (!image) return 0;
    uint64_t h = fnv(&image, 0);
    const int w = image.width(), hh = image.height();
    image.visit([&](const auto& p) {
        for (int y = 0; y < hh; y++) h = fnv(p->pixel(0, y), size_t(w) * size_t(image.channels()) * sizeof(*p->pixel(0, 0)), h);
    });
    return h;
}

/// COMPOSITOR_PRINT_FILTER_HASHES=1 prints the hashes to paste in.
void checkHash(const char* name, const AnyImage& image, uint64_t expected) {
    const uint64_t h = hashOf(image);
    if (std::getenv("COMPOSITOR_PRINT_FILTER_HASHES")) std::fprintf(stderr, "    %s 0x%016llxull\n", name, static_cast<unsigned long long>(h));
    if (h != expected) check::fail(__FILE__, __LINE__, std::string(name) + " hash changed");
}

AnyImage run(FilterKind kind, const AnyImage& image, FilterSettings s, GridFilterContext c = {}) {
    return applyGridFilter(kind, image, s, c);
}

FilterSettings defaults(FilterKind kind) { return FilterSettings::defaults(kind); }

} // namespace

TEST_CASE(every_kind_has_a_name_and_a_feature) {
    for (int i = 0; i < filterKindCount; i++) {
        const std::string name = filterKindName(FilterKind(i));
        CHECK(!name.empty());
        CHECK(supports("filter." + name, SampleType::U16));
    }
    CHECK(isGridFilter(FilterKind::Median));
    CHECK(!isGridFilter(FilterKind::LensCorrection));
    CHECK(filterRunsDirectly(FilterKind::Clouds));
    CHECK(!supports("filter.Median", SampleType::F32));
    CHECK(supports("filter.Twirl", SampleType::F32));
    CHECK(!supports("filter.Clouds", SampleType::U8, ColorMode::CMYK));
    CHECK(supports("filter.Unsharp Mask", SampleType::U16, ColorMode::Lab));
}

TEST_CASE(kernel_filters_draw_what_the_smart_filter_draws) {
    // The destructive filter is the one-entry Smart Filter stack over the layer, on its own grid.
    const auto img = card();
    struct Case { FilterKind kind; SmartFilterParameters p; };
    const Case cases[] = {
        {FilterKind::Median, smartfilter::Median{2}}, {FilterKind::HighPass, smartfilter::HighPass{4}},
        {FilterKind::Emboss, smartfilter::Emboss{135, 3, 100}}, {FilterKind::Mosaic, smartfilter::Mosaic{6}},
        {FilterKind::UnsharpMask, smartfilter::UnsharpMask{120, 1.5, 4}},
    };
    for (const Case& c : cases) {
        FilterSettings s = defaults(c.kind);
        s.radius = c.kind == FilterKind::Median ? 2 : c.kind == FilterKind::HighPass ? 4 : 1.5;
        s.angle = 135; s.height = 3; s.amount = c.kind == FilterKind::UnsharpMask ? 120 : 100; s.cellSize = 6; s.threshold = 4;
        const AnyImage out = run(c.kind, ImagePtr(img), s);
        REQUIRE(out.u8());
        CHECK_EQ(out.width(), img->width());
        CHECK_EQ(out.height(), img->height());
        SmartFilterStack stack;
        stack.supported = true;
        stack.entries.push_back({c.p, smartFilterName(c.p)});
        auto ref = renderSmartFilterStack(PlacedRaster{std::make_shared<Image>(*img), 0, 0}, PixelRect{0, 0, img->width(), img->height()}, stack);
        REQUIRE(ref && ref->image);
        int differing = 0;
        for (int y = 0; y < ref->image->height(); y++)
            for (int x = 0; x < ref->image->width(); x++)
                if (std::memcmp(ref->image->pixel(x, y), out.u8()->pixel(x + ref->x, y + ref->y), 4) != 0) differing++;
        CHECK_EQ(differing, 0);
    }
}

TEST_CASE(kernel_filters_at_sixteen_bits_and_in_cmyk_and_lab) {
    const auto img = card();
    const auto deep = widenImage(*img);
    for (FilterKind kind : {FilterKind::BoxBlur, FilterKind::RadialBlur, FilterKind::SurfaceBlur, FilterKind::DustAndScratches, FilterKind::Median,
                            FilterKind::UnsharpMask, FilterKind::HighPass, FilterKind::Emboss, FilterKind::Mosaic}) {
        FilterSettings s = defaults(kind);
        if (kind == FilterKind::BoxBlur) s.radius = 3;
        const AnyImage out = run(kind, Image16Ptr(deep), s);
        CHECK(out.u16());
        CHECK_EQ(out.width(), deep->width());
        // CMYK: five samples back.
        auto cmyk = std::make_shared<ImageC8>(img->width(), img->height(), 5);
        for (int y = 0; y < img->height(); y++)
            for (int x = 0; x < img->width(); x++) {
                const uint8_t* p = img->pixel(x, y);
                uint8_t* q = cmyk->pixel(x, y);
                q[0] = p[0]; q[1] = p[1]; q[2] = p[2]; q[3] = p[3]; q[4] = p[3];
            }
        GridFilterContext c;
        c.mode = ColorMode::CMYK;
        const AnyImage inks = run(kind, ImageC8Ptr(cmyk), s, c);
        CHECK(inks.c8());
        c.mode = ColorMode::Lab;
        CHECK(run(kind, ImagePtr(img), s, c).u8());
        // Not at 32 bits.
        CHECK(!run(kind, ImageFPtr(std::make_shared<ImageF>(4, 4, 4)), s));
    }
}

TEST_CASE(offset_moves_whole_pixels) {
    const auto img = card(16, 12);
    FilterSettings s = defaults(FilterKind::Offset);
    s.horizontal = 3; s.vertical = -2; s.undefinedAreas = 0;
    const AnyImage out = run(FilterKind::Offset, ImagePtr(img), s);
    REQUIRE(out.u8());
    for (int y = 0; y < 12; y++)
        for (int x = 0; x < 16; x++) CHECK(std::memcmp(out.u8()->pixel(x, y), img->pixel(((x - 3) % 16 + 16) % 16, ((y + 2) % 12 + 12) % 12), 4) == 0);
    s.undefinedAreas = 2;
    const AnyImage clear = run(FilterKind::Offset, ImagePtr(img), s);
    CHECK_EQ(int(clear.u8()->pixel(0, 0)[3]), 0);
    CHECK(std::memcmp(clear.u8()->pixel(5, 3), img->pixel(2, 5), 4) == 0);
    checkHash("offset/transparent", clear, 0x271d6b299354f65full);
}

TEST_CASE(maximum_and_minimum) {
    // A single bright pixel grows into a 3 x 3 square with Maximum 1 (squareness), or a plus with roundness.
    auto img = std::make_shared<Image>(9, 9);
    for (int y = 0; y < 9; y++) for (int x = 0; x < 9; x++) { uint8_t* p = img->pixel(x, y); p[0] = p[1] = p[2] = 0; p[3] = 255; }
    uint8_t* centre = img->pixel(4, 4);
    centre[0] = centre[1] = centre[2] = 255;
    FilterSettings s = defaults(FilterKind::Maximum);
    s.radius = 1;
    const AnyImage square = run(FilterKind::Maximum, ImagePtr(img), s);
    REQUIRE(square.u8());
    CHECK_EQ(int(square.u8()->pixel(3, 3)[0]), 255);
    CHECK_EQ(int(square.u8()->pixel(2, 4)[0]), 0);
    s.style = 1;
    const AnyImage round = run(FilterKind::Maximum, ImagePtr(img), s);
    CHECK_EQ(int(round.u8()->pixel(3, 4)[0]), 255);
    CHECK_EQ(int(round.u8()->pixel(3, 3)[0]), 0);
    // Minimum takes it away again.
    const AnyImage gone = run(FilterKind::Minimum, square, defaults(FilterKind::Minimum));
    CHECK(std::memcmp(gone.u8()->pixel(0, 0), img->pixel(0, 0), 4) == 0);
    CHECK_EQ(int(gone.u8()->pixel(4, 4)[0]), 255);
    CHECK_EQ(int(gone.u8()->pixel(3, 3)[0]), 0);
    FilterSettings big = defaults(FilterKind::Minimum);
    big.radius = 3; big.style = 1;
    checkHash("minimum/round3", run(FilterKind::Minimum, ImagePtr(card()), big), 0x5cf90fa77f92965cull);
}

TEST_CASE(find_edges_is_white_on_flat_colour) {
    auto img = std::make_shared<Image>(8, 8);
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) { uint8_t* p = img->pixel(x, y); p[0] = 90; p[1] = 120; p[2] = 30; p[3] = 255; }
    const AnyImage out = run(FilterKind::FindEdges, ImagePtr(img), {});
    REQUIRE(out.u8());
    CHECK_EQ(int(out.u8()->pixel(4, 4)[0]), 255);
    CHECK_EQ(int(out.u8()->pixel(4, 4)[3]), 255);
    checkHash("find_edges/card", run(FilterKind::FindEdges, ImagePtr(card()), {}), 0x8466f639dc2e1568ull);
    // Lab: lightness edges, a and b neutral.
    GridFilterContext lab;
    lab.mode = ColorMode::Lab;
    const AnyImage grey = run(FilterKind::FindEdges, ImagePtr(card()), {}, lab);
    CHECK_EQ(int(grey.u8()->pixel(30, 30)[1]), 128);
    CHECK_EQ(int(grey.u8()->pixel(30, 30)[2]), 128);
}

TEST_CASE(clouds_are_seeded_and_opaque) {
    const auto img = card();
    GridFilterContext c;
    c.seed = 7;
    c.documentSide = 64;
    c.foreground[0] = c.foreground[1] = c.foreground[2] = 0;
    const AnyImage a = run(FilterKind::Clouds, ImagePtr(img), {}, c);
    const AnyImage b = run(FilterKind::Clouds, ImagePtr(img), {}, c);
    REQUIRE(a.u8());
    CHECK_EQ(hashOf(a), hashOf(b));
    CHECK_EQ(int(a.u8()->pixel(1, 1)[3]), 255);
    c.seed = 8;
    CHECK(hashOf(run(FilterKind::Clouds, ImagePtr(img), {}, c)) != hashOf(a));
    c.seed = 7;
    checkHash("clouds/seed7", a, 0xa75eaae0facbfaa0ull);
    checkHash("difference_clouds/seed7", run(FilterKind::DifferenceClouds, ImagePtr(img), {}, c), 0xde9807f4dba62db3ull);
    // The pattern is fixed to the document: a grid one pixel further right reads one pixel further along.
    GridFilterContext moved = c;
    moved.originX = 1;
    const AnyImage shifted = run(FilterKind::Clouds, ImagePtr(img), {}, moved);
    CHECK(std::memcmp(shifted.u8()->pixel(10, 10), a.u8()->pixel(11, 10), 4) == 0);
}

TEST_CASE(distortions_stay_inside_their_bounds) {
    const auto img = card();
    GridFilterContext c;
    c.boundsX = 30; c.boundsY = 10; c.boundsWidth = 20; c.boundsHeight = 20;
    FilterSettings s = defaults(FilterKind::Twirl);
    s.angle = 200;
    const AnyImage out = run(FilterKind::Twirl, ImagePtr(img), s, c);
    REQUIRE(out.u8());
    int changedInside = 0, changedOutside = 0;
    for (int y = 0; y < img->height(); y++)
        for (int x = 0; x < img->width(); x++) {
            const bool inside = x >= 30 && x < 50 && y >= 10 && y < 30;
            if (std::memcmp(out.u8()->pixel(x, y), img->pixel(x, y), 4) != 0) (inside ? changedInside : changedOutside)++;
        }
    CHECK(changedInside > 0);
    CHECK_EQ(changedOutside, 0);
    // A Twirl of 0 changes nothing.
    s.angle = 0;
    CHECK_EQ(hashOf(run(FilterKind::Twirl, ImagePtr(img), s)), hashOf(ImagePtr(img)));
}

TEST_CASE(distortions_at_every_depth_and_layout) {
    const auto img = card();
    const auto deep = widenImage(*img);
    auto flt = std::make_shared<ImageF>(img->width(), img->height(), 4);
    for (int y = 0; y < img->height(); y++) for (int x = 0; x < img->width(); x++) for (int k = 0; k < 4; k++) flt->pixel(x, y)[k] = img->pixel(x, y)[k] / 255.0f;
    for (FilterKind kind : {FilterKind::Twirl, FilterKind::Pinch, FilterKind::Spherize, FilterKind::Wave, FilterKind::Ripple, FilterKind::PolarCoordinates,
                            FilterKind::ZigZag, FilterKind::Shear, FilterKind::Maximum, FilterKind::Minimum, FilterKind::Offset}) {
        FilterSettings s = defaults(kind);
        if (kind == FilterKind::Shear) s.amount = 40;
        if (kind == FilterKind::Offset) s.horizontal = 5;
        const AnyImage a = run(kind, ImagePtr(img), s), b = run(kind, Image16Ptr(deep), s), f = run(kind, ImageFPtr(flt), s);
        CHECK(a.u8());
        CHECK(b.u16());
        CHECK(f.f32());
        // The 16-bit result is the 8-bit one within a level.
        int worst = 0;
        for (int y = 0; y < img->height(); y++)
            for (int x = 0; x < img->width(); x++)
                for (int k = 0; k < 4; k++) worst = std::max(worst, std::abs(int(narrow16(b.u16()->pixel(x, y)[k])) - int(a.u8()->pixel(x, y)[k])));
        CHECK(worst <= 2);
        CHECK(hashOf(a) != hashOf(ImagePtr(img)));
    }
}

TEST_CASE(distortion_values) {
    // Sampled values within a level everywhere (MinGW's sin and cos may differ in the last bit), hashes on Linux.
    const auto img = card();
    FilterSettings twirl = defaults(FilterKind::Twirl);
    const AnyImage t = run(FilterKind::Twirl, ImagePtr(img), twirl);
    const AnyImage w = run(FilterKind::Wave, ImagePtr(img), defaults(FilterKind::Wave));
    const AnyImage p = run(FilterKind::PolarCoordinates, ImagePtr(img), defaults(FilterKind::PolarCoordinates));
    const AnyImage z = run(FilterKind::ZigZag, ImagePtr(img), defaults(FilterKind::ZigZag));
    const AnyImage s = run(FilterKind::Spherize, ImagePtr(img), defaults(FilterKind::Spherize));
    if (std::getenv("COMPOSITOR_PRINT_FILTER_HASHES"))
        for (const AnyImage* i : {&t, &w, &p, &z, &s}) {
            const uint8_t* q = i->u8()->pixel(20, 12);
            std::fprintf(stderr, "    sample %d %d %d %d\n", q[0], q[1], q[2], q[3]);
        }
    auto near = [](const AnyImage& i, int r, int g, int b, int a) {
        const uint8_t* q = i.u8()->pixel(20, 12);
        return std::abs(q[0] - r) <= 1 && std::abs(q[1] - g) <= 1 && std::abs(q[2] - b) <= 1 && std::abs(q[3] - a) <= 1;
    };
    CHECK(near(t, 96, 50, 71, 255));
    CHECK(near(w, 71, 28, 0, 255));
    CHECK(near(p, 224, 173, 100, 255));
    CHECK(near(z, 80, 65, 200, 255));
    CHECK(near(s, 95, 83, 101, 255));
#ifndef _WIN32
    checkHash("twirl/default", t, 0x96a3905a3a70a023ull);
    checkHash("wave/default", w, 0x71d8c35bc24232a8ull);
    checkHash("polar/default", p, 0x4e20a8cbd5b87500ull);
    checkHash("zigzag/default", z, 0x57732b278a61b8d9ull);
    checkHash("spherize/default", s, 0x4a46c3bca7023850ull);
#endif
}

TEST_CASE(radial_blur_zoom_at_every_depth_and_mode) {
    // Zoom is the Smart Filter kernel's Zoom (Photoshop's Blur Method), at 8 and 16 bits and on CMYK's inks and Lab.
    const auto img = card();
    FilterSettings s = defaults(FilterKind::RadialBlur);
    s.amount = 30;
    s.style = 1;   // Zoom
    const AnyImage zoom = run(FilterKind::RadialBlur, ImagePtr(img), s);
    REQUIRE(zoom.u8());
    SmartFilterStack stack;
    stack.supported = true;
    stack.entries.push_back({smartfilter::RadialBlur{30, 16, true}, "Radial Blur"});
    auto ref = renderSmartFilterStack(PlacedRaster{std::make_shared<Image>(*img), 0, 0}, PixelRect{0, 0, img->width(), img->height()}, stack);
    REQUIRE(ref && ref->image);
    int differing = 0;
    for (int y = 0; y < ref->image->height(); y++)
        for (int x = 0; x < ref->image->width(); x++)
            if (std::memcmp(ref->image->pixel(x, y), zoom.u8()->pixel(x + ref->x, y + ref->y), 4) != 0) differing++;
    CHECK_EQ(differing, 0);
    // Spin and Zoom differ.
    FilterSettings spin = s;
    spin.style = 0;
    const AnyImage spun = run(FilterKind::RadialBlur, ImagePtr(img), spin);
    CHECK(hashOf(spun) != hashOf(zoom));
    // 16 bits: the 8-bit result within two levels.
    const AnyImage deep = run(FilterKind::RadialBlur, Image16Ptr(widenImage(*img)), s);
    REQUIRE(deep.u16());
    int worst = 0;
    for (int y = 0; y < img->height(); y++)
        for (int x = 0; x < img->width(); x++)
            for (int k = 0; k < 4; k++) worst = std::max(worst, std::abs(int(narrow16(deep.u16()->pixel(x, y)[k])) - int(zoom.u8()->pixel(x, y)[k])));
    CHECK(worst <= 2);
    // CMYK (the first three inks carry the card's channels) and Lab.
    auto cmyk = std::make_shared<ImageC8>(img->width(), img->height(), 5);
    for (int y = 0; y < img->height(); y++)
        for (int x = 0; x < img->width(); x++) {
            const uint8_t* p = img->pixel(x, y);
            uint8_t* q = cmyk->pixel(x, y);
            q[0] = p[0]; q[1] = p[1]; q[2] = p[2]; q[3] = p[3]; q[4] = p[3];
        }
    GridFilterContext c;
    c.mode = ColorMode::CMYK;
    const AnyImage inks = run(FilterKind::RadialBlur, ImageC8Ptr(cmyk), s, c);
    REQUIRE(inks.c8());
    CHECK(hashOf(inks) != hashOf(ImageC8Ptr(cmyk)));
    c.mode = ColorMode::Lab;
    const AnyImage lab = run(FilterKind::RadialBlur, ImagePtr(img), s, c);
    REQUIRE(lab.u8());
    CHECK(hashOf(lab) != hashOf(ImagePtr(img)));
#ifndef _WIN32
    checkHash("radial/zoom", zoom, 0x62b2c3b6d47f8e00ull);
#endif
}

TEST_CASE(polar_coordinates_fills_the_corners_with_the_edge) {
    // Rectangular to Polar maps the image into the disc inscribed in the area; Photoshop fills what lies past it (the
    // corners) with the source's edge pixels repeated outwards, so an opaque layer stays opaque. Here the source's
    // green rises from 0 at the top to 255 at the bottom row and red follows x: each corner is the bottom row's green,
    // and its red the bottom row's red at the corner's angle.
    const int w = 64, h = 48;
    auto img = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t* p = img->pixel(x, y);
            p[0] = uint8_t(x * 255 / (w - 1)); p[1] = uint8_t(y * 255 / (h - 1)); p[2] = 60; p[3] = 255;
        }
    const AnyImage out = run(FilterKind::PolarCoordinates, ImagePtr(img), defaults(FilterKind::PolarCoordinates));
    REQUIRE(out.u8());
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) CHECK_EQ(int(out.u8()->pixel(x, y)[3]), 255);
    const double pi = 3.14159265358979323846;
    for (auto [x, y] : {std::pair{0, 0}, std::pair{w - 1, 0}, std::pair{0, h - 1}, std::pair{w - 1, h - 1}}) {
        const uint8_t* q = out.u8()->pixel(x, y);
        CHECK(q[1] >= 250);
        double theta = std::atan2(x + 0.5 - w / 2.0, -(y + 0.5 - h / 2.0));
        if (theta < 0) theta += 2 * pi;
        const int column = std::clamp(int(theta / (2 * pi) * w), 0, w - 1);
        CHECK(std::abs(int(q[0]) - int(img->pixel(column, h - 1)[0])) <= 10);
    }
}

TEST_MAIN()
