// The Histogram panel's counts and statistics, cache levels, and the Levels / Curves clipping display at 8, 16 and
// 32 bits and in CMYK and Lab.
#include "check.h"
#include "compositor/depth.h"
#include "compositor/histogram.h"

using namespace compositor;

namespace {

/// Left half black, right half white (opaque), 8 bits.
std::shared_ptr<Image> halves(int w = 8, int h = 4) {
    auto image = std::make_shared<Image>(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) { uint8_t v = x < w / 2 ? 0 : 255; uint8_t* p = image->pixel(x, y); p[0] = p[1] = p[2] = v; p[3] = 255; }
    return image;
}

TEST_CASE(rgb_counts_and_stats) {
    const auto h = imageHistogram(ImagePtr(halves()), ColorMode::RGB, TransferCurve::srgb());
    REQUIRE(!h.empty());
    CHECK_NEAR(h.channels[1][0], 16, 1e-9);
    CHECK_NEAR(h.channels[1][255], 16, 1e-9);
    CHECK_NEAR(h.luminosity[0], 16, 1e-9);
    CHECK_NEAR(h.luminosity[255], 16, 1e-9);
    const auto stats = histogramStats(h.channels[0]);
    CHECK_NEAR(stats.mean, 127.5, 1e-9);
    CHECK_NEAR(stats.stdDev, 127.5, 1e-9);
    CHECK_NEAR(stats.pixels, 32, 1e-9);
    CHECK_EQ(stats.median, 0);
    CHECK_NEAR(histogramPercentile(h.channels[1], 0), 50, 1e-9);
    CHECK_NEAR(histogramCount(h.channels[1], 1, 255), 16, 1e-9);
}

TEST_CASE(cache_level_counts_fewer_pixels) {
    const auto h = imageHistogram(ImagePtr(halves(64, 64)), ColorMode::RGB, TransferCurve::srgb(), 3);
    CHECK_EQ(h.cacheLevel, 3);
    CHECK_NEAR(histogramStats(h.channels[1]).pixels, 16 * 16, 1e-9);
    CHECK_EQ(histogramCacheLevel(4000, 3000, 1 << 20), 3);
    CHECK_EQ(histogramCacheLevel(100, 100, 1 << 20), 1);
}

TEST_CASE(depths_agree) {
    const auto eight = imageHistogram(ImagePtr(halves()), ColorMode::RGB, TransferCurve::srgb());
    auto deep = widenImage(*halves());
    const auto sixteen = imageHistogram(Image16Ptr(deep), ColorMode::RGB, TransferCurve::srgb());
    CHECK(eight.channels[2] == sixteen.channels[2]);
}

TEST_CASE(cmyk_and_lab_channels) {
    auto cmyk = std::make_shared<ImageC8>(4, 1, 5);
    for (int x = 0; x < 4; x++) { uint8_t* p = cmyk->pixel(x, 0); p[0] = 10; p[1] = 20; p[2] = 30; p[3] = 40; p[4] = 255; }
    const auto h = imageHistogram(ImageC8Ptr(cmyk), ColorMode::CMYK, TransferCurve::srgb());
    CHECK_NEAR(h.channels[4][40], 4, 1e-9);
    CHECK_NEAR(h.channels[0][10], 1, 1e-9);   // the four inks together, a quarter each
    CHECK(h.luminosity.empty());
    auto lab = std::make_shared<Image>(2, 1);
    for (int x = 0; x < 2; x++) { uint8_t* p = lab->pixel(x, 0); p[0] = 200; p[1] = 128; p[2] = 128; p[3] = 255; }
    const auto l = imageHistogram(ImagePtr(lab), ColorMode::Lab, TransferCurve::srgb());
    CHECK_NEAR(l.channels[1][200], 2, 1e-9);
    CHECK(l.channels[0].empty());
}

TEST_CASE(levels_white_point_clipping) {
    // A ramp of grays 0..255 and one pure red pixel; Input white at 200 clips the grays from 200 up.
    auto image = std::make_shared<Image>(257, 1);
    for (int x = 0; x < 256; x++) { uint8_t* p = image->pixel(x, 0); p[0] = p[1] = p[2] = uint8_t(x); p[3] = 255; }
    { uint8_t* p = image->pixel(256, 0); p[0] = 255; p[1] = 0; p[2] = 0; p[3] = 255; }
    AdjustmentSettings settings = AdjustmentSettings::defaults(AdjustmentKind::Levels);
    settings.levels.ranges[0].white = 200;
    settings.levels.ranges[0].outputWhite = 180;   // the display ignores the output range
    const auto adjusted = adjustedAny(clippingSettings(settings), ImagePtr(image), ColorMode::RGB, ColorProfile(), TransferCurve::srgb());
    const auto white = clippingDisplay(adjusted, ColorMode::RGB, true);
    REQUIRE(white);
    CHECK_EQ(int(white->pixel(100, 0)[0]), 0);
    CHECK_EQ(int(white->pixel(220, 0)[0]), 255);
    CHECK_EQ(int(white->pixel(220, 0)[2]), 255);
    CHECK_EQ(int(white->pixel(256, 0)[0]), 255);   // red clips red only
    CHECK_EQ(int(white->pixel(256, 0)[1]), 0);
    const auto black = clippingDisplay(adjusted, ColorMode::RGB, false);
    CHECK_EQ(int(black->pixel(0, 0)[0]), 0);       // black everywhere every channel clips
    CHECK_EQ(int(black->pixel(100, 0)[1]), 255);
    CHECK_EQ(int(black->pixel(256, 0)[1]), 0);     // green and blue at the bottom: red shows
    CHECK_EQ(int(black->pixel(256, 0)[0]), 255);
}

TEST_CASE(clipping_at_16_and_32_bits) {
    AdjustmentSettings settings = AdjustmentSettings::defaults(AdjustmentKind::Curves);
    settings.curves.channels[0] = {{0, 0}, {200, 255}};
    auto deep = widenImage(*halves());
    const auto a16 = adjustedAny(settings, Image16Ptr(deep), ColorMode::RGB, ColorProfile(), TransferCurve::srgb());
    const auto w16 = clippingDisplay(a16, ColorMode::RGB, true);
    CHECK_EQ(int(w16->pixel(0, 0)[0]), 0);
    CHECK_EQ(int(w16->pixel(7, 0)[0]), 255);
    auto f = std::make_shared<ImageF>(2, 1, 4);
    for (int x = 0; x < 2; x++) { float* p = f->pixel(x, 0); p[0] = p[1] = p[2] = x ? 2.0f : 0.25f; p[3] = 1; }
    const auto wf = clippingDisplay(ImageFPtr(f), ColorMode::RGB, true);
    CHECK_EQ(int(wf->pixel(0, 0)[0]), 0);
    CHECK_EQ(int(wf->pixel(1, 0)[0]), 255);
}

TEST_CASE(clipping_in_cmyk) {
    auto cmyk = std::make_shared<ImageC8>(1, 1, 5);
    uint8_t* p = cmyk->pixel(0, 0); p[0] = 255; p[1] = 10; p[2] = 10; p[3] = 10; p[4] = 255;
    const auto w = clippingDisplay(ImageC8Ptr(cmyk), ColorMode::CMYK, true);
    CHECK_EQ(int(w->pixel(0, 0)[0]), 255);
    CHECK_EQ(int(w->pixel(0, 0)[1]), 0);
}

TEST_CASE(curves_end_points_move_in) {
    // Photoshop's black and white input points: a curve whose ends sit at 40 and 200 is flat beyond them.
    CurvesSettings curves;
    curves.channels[0] = {{40, 0}, {200, 255}};
    CHECK(curves.isValid());
    CHECK(!curves.isIdentity());
    CHECK_NEAR(curves.value(10, 0), 0, 1e-9);
    CHECK_NEAR(curves.value(230, 0), 255, 1e-9);
    CHECK_NEAR(curves.value(120, 0), 127.5, 1e-6);
    CHECK(CurvesSettings().isIdentity());
}

} // namespace

TEST_MAIN()
