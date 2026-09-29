// Retouching in CMYK and Lab documents (P7 E): the healers on CMYK's five samples, Blur and Sharpen on five samples,
// Smudge and Liquify carrying five, Dodge, Burn and Sponge on L, a, b and on the inks, and the Paint Bucket's choice on
// the native samples. Nothing here passes through RGB.
#include "check.h"
#include "compositor/bucket.h"
#include "compositor/colormodes.h"
#include "compositor/heal.h"
#include "compositor/toning.h"
#include "compositor/warpstroke.h"
#include <cmath>

using namespace compositor;

namespace {

/// A CMYK image filled with `stored` (5 samples, inverted inks then alpha), with a square of `spot` at (x0, y0).
template <class Img, class S>
std::shared_ptr<Img> cmyk(int w, int h, std::array<S, 5> stored, std::array<S, 5> spot, int x0, int y0, int side) {
    auto image = std::make_shared<Img>(w, h, 5);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const bool in = x >= x0 && x < x0 + side && y >= y0 && y < y0 + side;
            for (int c = 0; c < 5; c++) image->pixel(x, y)[c] = in ? spot[size_t(c)] : stored[size_t(c)];
        }
    return image;
}

} // namespace

TEST_CASE(cmyk8_spot_healing_heals_every_sample) {
    const std::array<uint8_t, 5> field{100, 230, 240, 200, 255}, blot{0, 0, 0, 0, 255};
    for (int mode : {0, 1, 2}) {
        auto image = cmyk<ImageC8, uint8_t>(96, 96, field, blot, 44, 44, 8);
        GrayImage spot(96, 96, 0);
        for (int y = 42; y < 54; y++) for (int x = 42; x < 54; x++) spot.at(x, y) = 255;
        spotHeal(*image, spot, 1.0f, mode, 7);
        for (int c = 0; c < 5; c++) CHECK_NEAR(image->pixel(48, 48)[c], field[size_t(c)], 3);
        CHECK_EQ(int(image->pixel(10, 10)[3]), 200);   // outside the spot untouched
    }
}

TEST_CASE(cmyk16_spot_healing_and_healing_brush_heal_every_sample) {
    const std::array<uint16_t, 5> field{12000, 29000, 30000, 25000, 32768}, blot{0, 0, 0, 0, 32768};
    auto image = cmyk<Image16, uint16_t>(96, 96, field, blot, 44, 44, 8);
    Gray16 spot(96, 96, 0);
    for (int y = 42; y < 54; y++) for (int x = 42; x < 54; x++) spot.at(x, y) = 32768;
    spotHeal(*image, spot, 1.0f, 2, 7);
    for (int c = 0; c < 5; c++) CHECK_NEAR(image->pixel(48, 48)[c], field[size_t(c)], 200);

    // The Healing Brush: a source of other inks is toned to meet the edge (every sample, K too).
    auto target = cmyk<Image16, uint16_t>(64, 64, field, blot, 28, 28, 8);
    auto source = cmyk<Image16, uint16_t>(64, 64, {20000, 20000, 20000, 10000, 32768}, {20000, 20000, 20000, 10000, 32768}, 0, 0, 0);
    Gray16 hole(64, 64, 0);
    for (int y = 26; y < 38; y++) for (int x = 26; x < 38; x++) hole.at(x, y) = 32768;
    healFrom(*target, *source, hole, 1.0f);
    for (int c = 0; c < 5; c++) CHECK_NEAR(target->pixel(32, 32)[c], field[size_t(c)], 2);
}

TEST_CASE(cmyk8_healing_brush_matches_the_edge) {
    const std::array<uint8_t, 5> field{100, 230, 240, 200, 255};
    auto target = cmyk<ImageC8, uint8_t>(40, 40, field, {0, 0, 0, 0, 255}, 16, 16, 8);
    auto source = cmyk<ImageC8, uint8_t>(40, 40, {60, 60, 60, 60, 255}, {60, 60, 60, 60, 255}, 0, 0, 0);
    GrayImage hole(40, 40, 0);
    for (int y = 14; y < 26; y++) for (int x = 14; x < 26; x++) hole.at(x, y) = 255;
    healFrom(*target, *source, hole, 1.0f);
    for (int c = 0; c < 5; c++) CHECK_NEAR(target->pixel(20, 20)[c], field[size_t(c)], 1);
}

TEST_CASE(cmyk_blur_and_sharpen_work_on_all_five_samples) {
    // A black-ink edge: K blurs across it, the other inks (flat) stay, alpha stays opaque.
    ImageC8 image(32, 32, 5);
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) { uint8_t* p = image.pixel(x, y); p[0] = 200; p[1] = 150; p[2] = 100; p[3] = x < 16 ? 0 : 255; p[4] = 255; }
    gaussianBlurSamples(image, 2);
    CHECK(image.pixel(15, 16)[3] > 60 && image.pixel(15, 16)[3] < 195);
    CHECK(image.pixel(14, 16)[3] < image.pixel(17, 16)[3]);
    CHECK_NEAR(image.pixel(15, 16)[0], 200, 1);
    CHECK_NEAR(image.pixel(15, 16)[2], 100, 1);
    CHECK_EQ(int(image.pixel(15, 16)[4]), 255);

    Image16 flat(16, 16, 5);
    for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) { uint16_t* p = flat.pixel(x, y); p[0] = 1000; p[1] = 2000; p[2] = 3000; p[3] = 4000; p[4] = 32768; }
    Image16 sharpened = flat;
    sharpenSamples(sharpened, 1.0);
    CHECK(sharpened.pixel(8, 8)[3] == 4000 && sharpened.pixel(8, 8)[0] == 1000);
}

TEST_CASE(lab_dodge_raises_lightness_and_keeps_a_b) {
    Image lab(4, 4);
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) { uint8_t* p = lab.pixel(x, y); p[0] = 100; p[1] = 160; p[2] = 90; p[3] = 255; }
    ToningSettings dodge;
    dodge.kind = ToningKind::Dodge;
    toneImage(lab, dodge, ColorMode::Lab);
    CHECK(lab.pixel(1, 1)[0] > 140);
    CHECK_EQ(int(lab.pixel(1, 1)[1]), 160);
    CHECK_EQ(int(lab.pixel(1, 1)[2]), 90);

    // Sponge (desaturate) at 16 bits: a and b go to neutral, L stays.
    Image16 lab16(4, 4);
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) { uint16_t* p = lab16.pixel(x, y); p[0] = 20000; p[1] = 16384 + 3000; p[2] = 16384 - 2000; p[3] = 32768; }
    ToningSettings sponge;
    sponge.kind = ToningKind::Sponge;
    toneImage(lab16, sponge, ColorMode::Lab);
    CHECK_EQ(int(lab16.pixel(2, 2)[0]), 20000);
    CHECK_NEAR(lab16.pixel(2, 2)[1], 16384, 1);
    CHECK_NEAR(lab16.pixel(2, 2)[2], 16384, 1);
    // Saturate doubles the chroma.
    Image16 more(1, 1);
    uint16_t* m = more.pixel(0, 0); m[0] = 20000; m[1] = 16384 + 3000; m[2] = 16384 - 2000; m[3] = 32768;
    sponge.saturate = true;
    toneImage(more, sponge, ColorMode::Lab);
    CHECK_NEAR(more.pixel(0, 0)[1], 16384 + 6000, 1);
    CHECK_NEAR(more.pixel(0, 0)[2], 16384 - 4000, 1);
}

TEST_CASE(cmyk_burn_adds_ink_and_dodge_removes_it) {
    ImageC8 image(2, 2, 5);
    for (int y = 0; y < 2; y++) for (int x = 0; x < 2; x++) { uint8_t* p = image.pixel(x, y); p[0] = 128; p[1] = 160; p[2] = 200; p[3] = 220; p[4] = 255; }
    ImageC8 burned = image, dodged = image;
    ToningSettings burn;
    burn.kind = ToningKind::Burn;
    toneImage(burned, burn);
    ToningSettings dodge;
    dodge.kind = ToningKind::Dodge;
    toneImage(dodged, dodge);
    for (int c = 0; c < 4; c++) {
        CHECK(burned.pixel(0, 0)[c] < image.pixel(0, 0)[c]);   // stored inverted: lower is more ink
        CHECK(dodged.pixel(0, 0)[c] > image.pixel(0, 0)[c]);
    }
    CHECK_EQ(int(burned.pixel(0, 0)[4]), 255);
    // Sponge leaves K alone and moves C, M, Y towards their mean.
    ImageC8 sponged = image;
    ToningSettings sponge;
    sponge.kind = ToningKind::Sponge;
    toneImage(sponged, sponge);
    CHECK_EQ(int(sponged.pixel(0, 0)[3]), 220);
    CHECK(std::abs(sponged.pixel(0, 0)[0] - sponged.pixel(0, 0)[2]) <= 1);
}

TEST_CASE(bucket_chooses_on_native_samples) {
    // Two areas that differ only in black ink: the fill from the left stops where K changes.
    auto image = std::make_shared<ImageC8>(20, 10, 5);
    for (int y = 0; y < 10; y++)
        for (int x = 0; x < 20; x++) { uint8_t* p = image->pixel(x, y); p[0] = p[1] = p[2] = 200; p[3] = x < 10 ? 255 : 100; p[4] = 255; }
    GrayImage mask(1, 1);
    CHECK_EQ(bucketMask(ImageC8Ptr(image), 2, 2, 32, true, mask), 100L);
    CHECK_EQ(int(mask.at(9, 5)), 255);
    CHECK_EQ(int(mask.at(10, 5)), 0);
    CHECK_EQ(bucketMask(ImageC8Ptr(image), 2, 2, 200, true, mask), 200L);
    // Lab at 16 bits: a within Tolerance, b not.
    auto lab = std::make_shared<Image16>(4, 1, 4);
    for (int x = 0; x < 4; x++) { uint16_t* p = lab->pixel(x, 0); p[0] = 16000; p[1] = uint16_t(16384 + x * 100); p[2] = x == 3 ? 20000 : 16384; p[3] = 32768; }
    CHECK_EQ(bucketMask(Image16Ptr(lab), 0, 0, 2, false, mask), 3L);
}

TEST_CASE(cmyk_smudge_carries_black_ink) {
    auto image = std::make_shared<ImageC8>(60, 20, 5);
    for (int y = 0; y < 20; y++)
        for (int x = 0; x < 60; x++) { uint8_t* p = image->pixel(x, y); p[0] = p[1] = p[2] = 255; p[3] = x < 20 ? 0 : 255; p[4] = 255; }
    WarpStroke smudge(image, WarpMode::Smudge, 10, 0.5, 0.9);
    for (int x = 10; x <= 40; x += 2) smudge.append({double(x), 10});
    CHECK(smudge.imageC8());
    CHECK(smudge.imageC8()->pixel(30, 10)[3] < 200);    // black ink dragged in
    CHECK_EQ(int(smudge.imageC8()->pixel(30, 10)[0]), 255);   // no cyan made up
    auto liquify = std::make_shared<ImageC8>(*image);
    WarpStroke push(liquify, WarpMode::Liquify, 16, 0.5, 1.0);
    for (int x = 14; x <= 30; x += 1) push.append({double(x), 10});
    CHECK(push.imageC8()->pixel(22, 10)[3] < 128);
}

TEST_MAIN()
