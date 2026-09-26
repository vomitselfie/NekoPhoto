// The toning tools' full-strength images (toning.h): Dodge, Burn, Sponge and Sharpen.
#include "check.h"
#include "compositor/toning.h"
#include <cmath>

using namespace compositor;

namespace {

Image solid(int w, int h, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    Image image(w, h);
    image.fill(r, g, b, a);
    return image;
}

int luma(const uint8_t* p) { return int(std::lround(0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2])); }

} // namespace

TEST_CASE(dodge_and_burn_curves_keep_their_order_and_ends) {
    for (ToneRange range : {ToneRange::Shadows, ToneRange::Midtones, ToneRange::Highlights}) {
        double lastDodge = -1, lastBurn = -1;
        for (int i = 0; i <= 100; i++) {
            const double v = i / 100.0, d = dodgeTone(v, range), b = burnTone(v, range);
            CHECK(d >= v - 1e-12);
            CHECK(b <= v + 1e-12);
            CHECK(d >= lastDodge);
            CHECK(b >= lastBurn);
            lastDodge = d; lastBurn = b;
        }
        CHECK_EQ(dodgeTone(1, range), 1.0);
        CHECK_EQ(burnTone(0, range), 0.0);
    }
    // Each range works on its own tones most.
    CHECK(dodgeTone(0.1, ToneRange::Shadows) - 0.1 > dodgeTone(0.1, ToneRange::Highlights) - 0.1);
    CHECK(0.9 - burnTone(0.9, ToneRange::Highlights) > 0.9 - burnTone(0.9, ToneRange::Shadows));
}

TEST_CASE(protect_tones_moves_lightness_and_keeps_the_hue) {
    Image image = solid(2, 2, 180, 90, 40);
    const int before = luma(image.pixel(0, 0));
    toneImage(image, {ToningKind::Dodge, ToneRange::Midtones, true, false});
    const uint8_t* p = image.pixel(0, 0);
    CHECK(luma(p) > before + 20);
    CHECK(p[0] > p[1] && p[1] > p[2]);   // still orange: the channels keep their order
    Image burned = solid(2, 2, 180, 90, 40);
    toneImage(burned, {ToningKind::Burn, ToneRange::Midtones, true, false});
    CHECK(luma(burned.pixel(0, 0)) < before - 20);
}

TEST_CASE(sponge_desaturates_to_grey_and_saturates_inside_the_gamut) {
    Image grey = solid(1, 1, 200, 100, 50);
    toneImage(grey, {ToningKind::Sponge, ToneRange::Midtones, true, false});
    const uint8_t* g = grey.pixel(0, 0);
    CHECK(std::abs(int(g[0]) - int(g[1])) <= 1 && std::abs(int(g[1]) - int(g[2])) <= 1);
    Image vivid = solid(1, 1, 200, 100, 50);
    const int before = luma(vivid.pixel(0, 0));
    toneImage(vivid, {ToningKind::Sponge, ToneRange::Midtones, true, true});
    const uint8_t* v = vivid.pixel(0, 0);
    CHECK(int(v[0]) - int(v[2]) > 150);                  // further apart than 150 before
    CHECK(std::abs(luma(v) - before) <= 2);              // the lightness stays
}

TEST_CASE(toning_leaves_transparency_alone_and_works_on_straight_colour) {
    Image image(2, 1);
    image.fill(0, 0, 0, 0);
    uint8_t* half = image.pixel(1, 0);
    half[0] = 50; half[1] = 50; half[2] = 50; half[3] = 128;   // straight grey 100
    toneImage(image, {ToningKind::Burn, ToneRange::Midtones, false, false});
    CHECK_EQ(int(image.pixel(0, 0)[3]), 0);
    CHECK_EQ(int(image.pixel(1, 0)[3]), 128);
    // Straight 100/255 squared is about 39: premultiplied by 128, about 20.
    CHECK(std::abs(int(image.pixel(1, 0)[0]) - 20) <= 1);
}

TEST_CASE(sharpen_raises_edge_contrast_and_leaves_flat_areas) {
    Image image = solid(20, 10, 60, 60, 60);
    for (int y = 0; y < 10; y++) for (int x = 10; x < 20; x++) { uint8_t* p = image.pixel(x, y); p[0] = p[1] = p[2] = 180; }
    sharpenImage(image);
    CHECK(image.pixel(9, 5)[0] < 60);    // the dark side of the edge darker
    CHECK(image.pixel(10, 5)[0] > 180);  // the light side lighter
    CHECK_EQ(int(image.pixel(2, 5)[0]), 60);
    CHECK_EQ(int(image.pixel(17, 5)[0]), 180);
}

TEST_MAIN()
