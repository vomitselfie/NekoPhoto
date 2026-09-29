// Transforms and resampling in CMYK and Lab documents (P7 E): Image Size, Distort, Warp and merging a transformed
// selection keep the inks (all five samples) and Lab's a and b; CMYK through two 4-sample passes agrees with the RGB
// samplers channel by channel; the flat exports are the native composite through the profile to sRGB.
#include "check.h"
#include "compositor/colormgmt.h"
#include "compositor/colormodes.h"
#include "compositor/depth.h"
#include "compositor/modetransform.h"
#include "compositor/render.h"
#include "compositor/smartobject_edit.h"
#include <cmath>

using namespace compositor;

namespace {

template <class Img, class S>
std::shared_ptr<Img> flat(int w, int h, int n, std::array<S, 5> v) {
    auto image = std::make_shared<Img>(w, h, n);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) for (int c = 0; c < n; c++) image->pixel(x, y)[c] = v[size_t(c)];
    return image;
}

Document docWith(ColorMode mode, SampleType type, const AnyImage& image) {
    Document d(image.width(), image.height());
    d.colorMode = mode;
    d.sampleType = type;
    d.layers.push_back(Layer(Asset::makeAny(image, "L"), Point(0, 0)));
    return d;
}

} // namespace

TEST_CASE(split_and_join_round_trip) {
    auto image = std::make_shared<ImageC8>(7, 5, 5);
    for (int y = 0; y < 5; y++) for (int x = 0; x < 7; x++) for (int c = 0; c < 5; c++) image->pixel(x, y)[c] = uint8_t(c == 4 ? 255 : (x * 31 + y * 17 + c * 50) % 256);
    auto [cmy, black] = splitFiveSample(*image);
    CHECK(*joinFiveSample(*cmy, *black) == *image);
    auto deep = widenImageC8(*image);
    auto [c16, k16] = splitFiveSample(*deep);
    CHECK(*joinFiveSample(*c16, *k16) == *deep);
}

TEST_CASE(cmyk_warp_matches_the_rgb_warp_per_channel) {
    // A gradient of inks: the CMYK warp's C, M, Y and alpha are the RGB warp's of (C, M, Y, alpha); K its first channel
    // of (K, K, K, alpha).
    auto image = std::make_shared<ImageC8>(40, 30, 5);
    for (int y = 0; y < 30; y++) for (int x = 0; x < 40; x++) { uint8_t* p = image->pixel(x, y); p[0] = uint8_t(x * 6); p[1] = uint8_t(y * 8); p[2] = 90; p[3] = uint8_t(255 - x * 5); p[4] = 255; }
    const LayerTransform t(Point(0, 0), Size(40, 30));
    const Corners corners{Point(3, 2), Point(50, 6), Point(44, 41), Point(1, 30)};
    auto warped = warpImageAny(ImageC8Ptr(image), t, corners);
    CHECK(warped.has_value());
    auto [cmy, black] = splitFiveSample(*image);
    auto a = warpImage(ImagePtr(cmy), t, corners);
    auto b = warpImage(ImagePtr(black), t, corners);
    const ImageC8& w = *warped->image.c8();
    CHECK_EQ(w.width(), a->image->width());
    bool same = true;
    for (int y = 0; y < w.height(); y++)
        for (int x = 0; x < w.width(); x++) {
            const uint8_t* p = w.pixel(x, y);
            const uint8_t* q = a->image->pixel(x, y);
            same = same && p[0] == q[0] && p[1] == q[1] && p[2] == q[2] && p[4] == q[3] && p[3] == std::min(b->image->pixel(x, y)[0], q[3]);
            same = same && b->image->pixel(x, y)[3] == q[3];   // both passes weigh the same alpha
        }
    CHECK(same);
}

TEST_CASE(image_size_of_a_cmyk_document_keeps_the_inks) {
    // Flat inks (stored inverted): after Image Size every interior pixel holds the same inks, at 8 and 16 bits and with
    // each sampling mode.
    for (Sampling s : {Sampling::Nearest, Sampling::Smooth, Sampling::High}) {
        auto image = flat<ImageC8, uint8_t>(20, 16, 5, {128, 200, 255, 50, 255});
        Document d = docWith(ColorMode::CMYK, SampleType::U8, ImageC8Ptr(image));
        CHECK(resizeDocument(d, 50, 40, 72, s));
        CHECK_EQ(d.width, 50);
        const ImageC8Ptr& out = d.layers[0].asset->image.c8();
        CHECK(out != nullptr);
        CHECK_EQ(out->width(), 50);
        const uint8_t* p = out->pixel(25, 20);
        CHECK_EQ(int(p[0]), 128); CHECK_EQ(int(p[1]), 200); CHECK_EQ(int(p[2]), 255); CHECK_EQ(int(p[3]), 50); CHECK_EQ(int(p[4]), 255);

        auto deep = flat<Image16, uint16_t>(20, 16, 5, {16000, 25000, 32768, 6000, 32768});
        Document d16 = docWith(ColorMode::CMYK, SampleType::U16, Image16Ptr(deep));
        CHECK(resizeDocument(d16, 10, 8, 72, s));
        const Image16Ptr& o16 = d16.layers[0].asset->image.u16();
        CHECK(o16 != nullptr && o16->channels() == 5);
        const uint16_t* q = o16->pixel(5, 4);
        CHECK_NEAR(q[0], 16000, 1); CHECK_NEAR(q[1], 25000, 1); CHECK_NEAR(q[2], 32768, 1); CHECK_NEAR(q[3], 6000, 1); CHECK_NEAR(q[4], 32768, 1);
    }
}

TEST_CASE(image_size_of_a_lab_document_keeps_a_and_b) {
    // Lab through the RGB samplers: a and b (offset) come out as they went in, a colour not a neutral.
    auto lab = std::make_shared<Image>(16, 16);
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) { uint8_t* p = lab->pixel(x, y); p[0] = 140; p[1] = 180; p[2] = 60; p[3] = 255; }
    Document d = docWith(ColorMode::Lab, SampleType::U8, ImagePtr(lab));
    CHECK(resizeDocument(d, 37, 29, 72, Sampling::High));
    const uint8_t* p = d.layers[0].asset->image.u8()->pixel(18, 14);
    CHECK_EQ(int(p[0]), 140); CHECK_EQ(int(p[1]), 180); CHECK_EQ(int(p[2]), 60);
    auto deep = flat<Image16, uint16_t>(16, 16, 4, {20000, 16384 + 40 * 128, 16384 - 30 * 128, 32768, 0});
    Document d16 = docWith(ColorMode::Lab, SampleType::U16, Image16Ptr(deep));
    CHECK(resizeDocument(d16, 9, 9, 72, Sampling::Smooth));
    const uint16_t* q = d16.layers[0].asset->image.u16()->pixel(4, 4);
    CHECK_NEAR(q[1], 16384 + 40 * 128, 1);
    CHECK_NEAR(q[2], 16384 - 30 * 128, 1);
}

TEST_CASE(warp_and_cage_bend_cmyk_layers_on_five_samples) {
    auto image = flat<ImageC8, uint8_t>(32, 32, 5, {10, 20, 30, 40, 255});
    Document d = docWith(ColorMode::CMYK, SampleType::U8, ImageC8Ptr(image));
    TextWarp warp;
    warp.style = "warpArc";
    warp.bend = 30;
    std::string why;
    CHECK(warpLayer(d, d.layers[0], warp, &why));
    const ImageC8Ptr& bent = d.layers[0].asset->image.c8();
    CHECK(bent != nullptr);
    // The middle of the bent layer is still the ink it was.
    const uint8_t* p = bent->pixel(bent->width() / 2, bent->height() / 2);
    CHECK_EQ(int(p[0]), 10); CHECK_EQ(int(p[3]), 40); CHECK_EQ(int(p[4]), 255);

    Document c = docWith(ColorMode::CMYK, SampleType::U8, ImageC8Ptr(image));
    auto cage = layerWarpCage(c, c.layers[0], &why);
    CHECK(cage.has_value());
    for (double& x : cage->xs) x *= 1.5;
    auto preview = previewWarpCageAny(c, c.layers[0], *cage, 16);
    CHECK(preview.has_value() && preview->image.c8());
    CHECK(warpLayerToCage(c, c.layers[0], *cage, &why));
    const ImageC8Ptr& caged = c.layers[0].asset->image.c8();
    CHECK(caged != nullptr && caged->width() > 40);
    CHECK_EQ(int(caged->pixel(caged->width() / 2, 16)[2]), 30);
}

TEST_CASE(composite_over_merges_a_floating_cmyk_selection) {
    auto base = flat<ImageC8, uint8_t>(4, 4, 5, {255, 255, 255, 255, 255});
    auto top = flat<ImageC8, uint8_t>(4, 4, 5, {0, 0, 0, 0, 0});
    top->pixel(1, 1)[0] = 0; top->pixel(1, 1)[3] = 0; top->pixel(1, 1)[4] = 255;   // opaque full ink at (1, 1)
    const AnyImage merged = compositeOverAny(ImageC8Ptr(top), ImageC8Ptr(base));
    CHECK_EQ(int(merged.c8()->pixel(1, 1)[0]), 0);
    CHECK_EQ(int(merged.c8()->pixel(0, 0)[0]), 255);
    const AnyImage placed = placeInAny(ImageC8Ptr(base), 6, 6, 1, 2);
    CHECK_EQ(int(placed.c8()->pixel(0, 0)[4]), 0);
    CHECK_EQ(int(placed.c8()->pixel(1, 2)[4]), 255);
}

TEST_CASE(cmyk_and_lab_exports_are_the_composite_through_the_profile) {
    // What PNG, JPEG and the other flat formats write: render()'s sRGB for CMYK and Lab, which is the native composite
    // converted through the document's profile to sRGB (the display's conversion without a monitor profile).
    auto image = std::make_shared<ImageC8>(8, 8, 5);
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) { uint8_t* p = image->pixel(x, y); p[0] = uint8_t(x * 30); p[1] = uint8_t(y * 30); p[2] = 200; p[3] = 230; p[4] = 255; }
    for (ColorMode mode : {ColorMode::CMYK, ColorMode::Lab}) {
        AnyImage pixels = ImageC8Ptr(image);
        if (mode == ColorMode::Lab) {
            auto lab = std::make_shared<Image>(8, 8);
            for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) { uint8_t* p = lab->pixel(x, y); p[0] = uint8_t(60 + x * 20); p[1] = uint8_t(100 + y * 10); p[2] = 150; p[3] = 255; }
            pixels = ImagePtr(lab);
        }
        Document d = docWith(mode, SampleType::U8, pixels);
        auto exported = renderFlattened(d);
        const AnyImage rgb = convertImage(renderNative(d), mode, d.profile, ColorMode::RGB, ColorProfile());
        CHECK(rgb.u8() != nullptr);
        int worst = 0;
        for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) for (int c = 0; c < 4; c++)
            worst = std::max(worst, std::abs(int(exported->pixel(x, y)[c]) - int(rgb.u8()->pixel(x, y)[c])));
        CHECK(worst <= 1);
    }
}

TEST_CASE(rgb_pixels_convert_into_cmyk_through_the_profile) {
    // Paste and Import into CMYK: sRGB red through the Working CMYK is Little CMS's inks for it, and back it is red again.
    auto red = std::make_shared<Image>(2, 2);
    for (int y = 0; y < 2; y++) for (int x = 0; x < 2; x++) { uint8_t* p = red->pixel(x, y); p[0] = 255; p[1] = 0; p[2] = 0; p[3] = 255; }
    const AnyImage cmyk = convertImage(ImagePtr(red), ColorMode::RGB, ColorProfile(), ColorMode::CMYK, ColorProfile());
    CHECK(cmyk.c8() != nullptr);
    const uint8_t* p = cmyk.c8()->pixel(0, 0);
    CHECK(p[0] > 200);   // hardly any cyan (stored inverted)
    CHECK(p[1] < 60 && p[2] < 60);   // magenta and yellow laid
    const AnyImage back = convertImage(cmyk, ColorMode::CMYK, ColorProfile(), ColorMode::RGB, ColorProfile());
    CHECK(back.u8()->pixel(0, 0)[0] > 200 && back.u8()->pixel(0, 0)[1] < 80);
}

TEST_MAIN()
