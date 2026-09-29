// Adjustments and filters in CMYK and Lab documents (P7 E, modeedit.h): each kind on the document's own samples at 8
// and 16 bits. Levels and Curves per ink and per Lab channel, Invert on every ink, Selective Color moving the black
// plate, Threshold and Gradient Map on lightness to colours through the profile, Hue/Saturation and Channel Mixer on
// inks, Photo Filter and Exposure in Lab, the filters over five samples, the adjustment layer drawing what the pixel
// edit makes, and the settings' extra CMYK slots kept out of RGB manifests.
#include "check.h"
#include "compositor/colormgmt.h"
#include "compositor/colormodes.h"
#include "compositor/depth.h"
#include "compositor/modeedit.h"
#include "compositor/render.h"
#include <cmath>
#include <functional>

using namespace compositor;

namespace {

/// Straight inks (0..1) for a CMYK pixel, from percentages.
struct Inks { double c, m, y, k; };

/// A `w` x `h` opaque CMYK buffer at `S` whose pixel (x, y) holds `f(x, y)`'s inks.
template <SampleType S>
AnyImage cmykImage(int w, int h, const std::function<Inks(int, int)>& f) {
    constexpr double one = double(SampleTraits<S>::one);
    auto image = std::make_shared<ImageT<S>>(w, h, 5);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const Inks i = f(x, y);
            SampleOf<S>* p = image->pixel(x, y);
            const double inks[4] = {i.c, i.m, i.y, i.k};
            for (int k = 0; k < 4; k++) p[k] = SampleOf<S>(std::lround((1 - inks[k] / 100) * one));
            p[4] = SampleOf<S>(one);
        }
    if constexpr (S == SampleType::U8) return ImageC8Ptr(image);
    else return Image16Ptr(image);
}

/// A `w` x `h` opaque Lab buffer at `S` whose pixel (x, y) holds `f(x, y)` = {L, a, b}.
template <SampleType S>
AnyImage labImage(int w, int h, const std::function<std::array<double, 3>(int, int)>& f) {
    constexpr uint32_t one = SampleTraits<S>::one;
    auto fill = [&](auto& image) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const auto v = f(x, y);
                auto* p = image.pixel(x, y);
                p[0] = storedLabL<S>(v[0], SampleOf<S>(one));
                p[1] = storedLabAB<S>(v[1], SampleOf<S>(one));
                p[2] = storedLabAB<S>(v[2], SampleOf<S>(one));
                p[3] = SampleOf<S>(one);
            }
    };
    if constexpr (S == SampleType::U8) { auto image = std::make_shared<Image>(w, h); fill(*image); return ImagePtr(image); }
    else { auto image = std::make_shared<Image16>(w, h, 4); fill(*image); return Image16Ptr(image); }
}

/// Sample `k` of pixel (x, y) as stored, and its ink (CMYK, percent) or value (Lab: L, a, b).
double stored(const AnyImage& image, int x, int y, int k) {
    if (image.c8()) return image.c8()->pixel(x, y)[k];
    if (image.u8()) return image.u8()->pixel(x, y)[k];
    return image.u16()->pixel(x, y)[k];
}
double full(const AnyImage& image) { return image.sampleType() == SampleType::U16 ? double(one16) : 255.0; }
/// The straight ink of a premultiplied sample, percent.
double ink(const AnyImage& image, int x, int y, int k) {
    const double a = stored(image, x, y, 4);
    return a > 0 ? (1 - std::min(1.0, stored(image, x, y, k) / a)) * 100 : 0;
}
double labAt(const AnyImage& image, int x, int y, int k) {
    const double v = stored(image, x, y, k);
    if (k == 0) return v / full(image) * 100;
    return image.sampleType() == SampleType::U16 ? (v - 16384) / 128 : v - 128;
}

const ColorProfile& cmykProfile() { return defaultCmykProfile(); }

AdjustmentSettings settingsOf(AdjustmentKind kind) { return AdjustmentSettings::defaults(kind); }

template <SampleType S>
void levelsMoveInkAsPhotoshop() {
    // Composite input black at 64: the plates' darks (much ink) go darker, so every ink grows; white paper stays.
    const AnyImage before = cmykImage<S>(2, 1, [](int x, int) { return x == 0 ? Inks{20, 40, 60, 10} : Inks{0, 0, 0, 0}; });
    AdjustmentSettings s = settingsOf(AdjustmentKind::Levels);
    s.levels.ranges[0].black = 64;
    const AnyImage after = adjustedInMode(s, before, ColorMode::CMYK, cmykProfile());
    REQUIRE(after);
    for (int k = 0; k < 4; k++) {
        CHECK(ink(after, 0, 0, k) > ink(before, 0, 0, k) + 1);
        // The level maths on the stored plate: (v - 64) / (255 - 64).
        const double v = stored(before, 0, 0, k) / full(before);
        CHECK_NEAR(stored(after, 0, 0, k) / full(after), std::clamp((v * 255 - 64) / 191, 0.0, 1.0), 1.0 / 255 + 1e-6);
        CHECK_NEAR(ink(after, 1, 0, k), 0, 1e-9);
    }
    // Black's own slot moves the black plate alone.
    AdjustmentSettings black = settingsOf(AdjustmentKind::Levels);
    black.levels.ranges[4].white = 128;
    const AnyImage k = adjustedInMode(black, before, ColorMode::CMYK, cmykProfile());
    for (int c = 0; c < 3; c++) CHECK_EQ(stored(k, 0, 0, c), stored(before, 0, 0, c));
    CHECK(stored(k, 0, 0, 3) > stored(before, 0, 0, 3));
}

TEST_CASE(cmyk_levels_move_ink) {
    levelsMoveInkAsPhotoshop<SampleType::U8>();
    levelsMoveInkAsPhotoshop<SampleType::U16>();
}

template <SampleType S>
void curvesPerInk() {
    const AnyImage before = cmykImage<S>(1, 1, [](int, int) { return Inks{30, 30, 30, 30}; });
    AdjustmentSettings s = settingsOf(AdjustmentKind::Curves);
    s.curves.channels[4] = {{0, 0}, {128, 60}, {255, 255}};   // black plate only
    const AnyImage after = adjustedInMode(s, before, ColorMode::CMYK, cmykProfile());
    REQUIRE(after);
    for (int c = 0; c < 3; c++) CHECK_EQ(stored(after, 0, 0, c), stored(before, 0, 0, c));
    CHECK(ink(after, 0, 0, 3) > 30 + 5);
    // Cyan's slot moves cyan alone.
    AdjustmentSettings cyan = settingsOf(AdjustmentKind::Curves);
    cyan.curves.channels[1] = {{0, 0}, {255, 128}};
    const AnyImage c = adjustedInMode(cyan, before, ColorMode::CMYK, cmykProfile());
    CHECK(ink(c, 0, 0, 0) > 60);
    for (int k = 1; k < 4; k++) CHECK_EQ(stored(c, 0, 0, k), stored(before, 0, 0, k));
}

TEST_CASE(cmyk_curves_per_ink) {
    curvesPerInk<SampleType::U8>();
    curvesPerInk<SampleType::U16>();
}

template <SampleType S>
void labCurvesOnLightness() {
    const AnyImage before = labImage<S>(1, 1, [](int, int) { return std::array<double, 3>{40, 30, -20}; });
    AdjustmentSettings s = settingsOf(AdjustmentKind::Curves);
    s.curves.channel = 1;
    s.curves.channels[1] = {{0, 0}, {100, 160}, {255, 255}};   // Lightness
    const AnyImage after = adjustedInMode(s, before, ColorMode::Lab, ColorProfile());
    REQUIRE(after);
    CHECK(labAt(after, 0, 0, 0) > 50);
    CHECK_EQ(stored(after, 0, 0, 1), stored(before, 0, 0, 1));
    CHECK_EQ(stored(after, 0, 0, 2), stored(before, 0, 0, 2));
    // Levels on a alone: output white pulled in shrinks positive a, L and b stay.
    AdjustmentSettings a = settingsOf(AdjustmentKind::Levels);
    a.levels.ranges[2].outputWhite = 200;
    const AnyImage moved = adjustedInMode(a, before, ColorMode::Lab, ColorProfile());
    CHECK(labAt(moved, 0, 0, 1) < 30 - 5);
    CHECK_EQ(stored(moved, 0, 0, 0), stored(before, 0, 0, 0));
    CHECK_EQ(stored(moved, 0, 0, 2), stored(before, 0, 0, 2));
}

TEST_CASE(lab_curves_on_lightness_leave_a_and_b) {
    labCurvesOnLightness<SampleType::U8>();
    labCurvesOnLightness<SampleType::U16>();
}

template <SampleType S>
void invertInks() {
    const AnyImage before = cmykImage<S>(1, 1, [](int, int) { return Inks{10, 20, 70, 100}; });
    const AnyImage after = adjustedInMode(settingsOf(AdjustmentKind::Invert), before, ColorMode::CMYK, cmykProfile());
    REQUIRE(after);
    for (int k = 0; k < 4; k++) CHECK_NEAR(stored(after, 0, 0, k), full(before) - stored(before, 0, 0, k), 1);
    CHECK_EQ(stored(after, 0, 0, 4), full(before));
}

TEST_CASE(cmyk_invert_inverts_inks) {
    invertInks<SampleType::U8>();
    invertInks<SampleType::U16>();
}

template <SampleType S>
void selectiveColor() {
    // A red (magenta and yellow ink) and a blue (cyan and magenta): Reds' black +50% relative adds black ink to the red
    // only; absolute cyan -20% on Reds takes nothing where there is no cyan and never goes negative.
    const AnyImage before = cmykImage<S>(2, 1, [](int x, int) { return x == 0 ? Inks{0, 90, 90, 20} : Inks{90, 80, 0, 0}; });
    AdjustmentSettings s = settingsOf(AdjustmentKind::SelectiveColor);
    s.selectiveColor.ranges[0][3] = 50;
    const AnyImage after = adjustedInMode(s, before, ColorMode::CMYK, cmykProfile());
    REQUIRE(after);
    CHECK(ink(after, 0, 0, 3) > 20 + 5);
    for (int k = 0; k < 3; k++) CHECK_NEAR(ink(after, 0, 0, k), ink(before, 0, 0, k), 1e-9);
    for (int k = 0; k < 4; k++) CHECK_NEAR(ink(after, 1, 0, k), ink(before, 1, 0, k), 1e-9);
    AdjustmentSettings absolute = settingsOf(AdjustmentKind::SelectiveColor);
    absolute.selectiveColor.absolute = true;
    absolute.selectiveColor.ranges[0][2] = -30;   // Reds: less yellow
    const AnyImage less = adjustedInMode(absolute, before, ColorMode::CMYK, cmykProfile());
    CHECK(ink(less, 0, 0, 2) < 90 - 5);
    CHECK_NEAR(ink(less, 1, 0, 2), 0, 1e-9);
}

TEST_CASE(cmyk_selective_color) {
    selectiveColor<SampleType::U8>();
    selectiveColor<SampleType::U16>();
}

template <SampleType S>
void thresholdAndGradientMap() {
    // Threshold: a dark pixel becomes the profile's black (the brush's black, all four inks), a light one no ink.
    const AnyImage before = cmykImage<S>(2, 1, [](int x, int) { return x == 0 ? Inks{70, 60, 60, 60} : Inks{5, 5, 5, 0}; });
    const AnyImage after = adjustedInMode(settingsOf(AdjustmentKind::Threshold), before, ColorMode::CMYK, cmykProfile());
    REQUIRE(after);
    const ColorTransformPtr t = transformBetween(ColorProfile(), cmykProfile(), ConvertOptions(), PixelFormat::RGBFloat, PixelFormat::CMYKFloat);
    const float black[3] = {0, 0, 0};
    float inks[4] = {};
    t->apply(black, inks, 1);
    for (int k = 0; k < 4; k++) CHECK_NEAR(ink(after, 0, 0, k), inks[k], 0.5);
    for (int k = 0; k < 4; k++) CHECK_NEAR(ink(after, 1, 0, k), 0, 1e-9);
    // Gradient Map black to white in Lab: lightness kept, colour gone.
    const AnyImage lab = labImage<S>(3, 1, [](int x, int) { return std::array<double, 3>{20.0 + 30 * x, 40, -30}; });
    AdjustmentSettings g = settingsOf(AdjustmentKind::GradientMap);
    const AnyImage mapped = adjustedInMode(g, lab, ColorMode::Lab, ColorProfile());
    REQUIRE(mapped);
    for (int x = 0; x < 3; x++) {
        CHECK_NEAR(labAt(mapped, x, 0, 0), labAt(lab, x, 0, 0), 1);
        CHECK_NEAR(labAt(mapped, x, 0, 1), 0, 1);
        CHECK_NEAR(labAt(mapped, x, 0, 2), 0, 1);
    }
}

TEST_CASE(threshold_and_gradient_map_on_lightness) {
    thresholdAndGradientMap<SampleType::U8>();
    thresholdAndGradientMap<SampleType::U16>();
}

template <SampleType S>
void colourKinds() {
    const AnyImage before = cmykImage<S>(1, 1, [](int, int) { return Inks{10, 80, 60, 25}; });
    // Hue/Saturation -100: the stored cyan, magenta and yellow meet (a neutral), black untouched.
    AdjustmentSettings h = settingsOf(AdjustmentKind::HueSaturation);
    h.hsv.adjustments[0].saturation = -100;
    const AnyImage grey = adjustedInMode(h, before, ColorMode::CMYK, cmykProfile());
    REQUIRE(grey);
    CHECK_NEAR(stored(grey, 0, 0, 0), stored(grey, 0, 0, 1), 1);
    CHECK_NEAR(stored(grey, 0, 0, 1), stored(grey, 0, 0, 2), 1);
    CHECK_EQ(stored(grey, 0, 0, 3), stored(before, 0, 0, 3));
    // Channel Mixer: cyan made from magenta's ink.
    AdjustmentSettings m = settingsOf(AdjustmentKind::ChannelMixer);
    m.channelMixer.inks[0] = {0, 100, 0, 0, 0};
    const AnyImage mixed = adjustedInMode(m, before, ColorMode::CMYK, cmykProfile());
    REQUIRE(mixed);
    CHECK_NEAR(ink(mixed, 0, 0, 0), 80, 0.5);
    for (int k = 1; k < 4; k++) CHECK_EQ(stored(mixed, 0, 0, k), stored(before, 0, 0, k));
    // Color Balance towards red in the midtones takes cyan ink away and leaves black.
    AdjustmentSettings b = settingsOf(AdjustmentKind::ColorBalance);
    b.colorBalance.ranges[1] = {60, 0, 0};
    b.colorBalance.preserveLuminosity = false;
    const AnyImage balanced = adjustedInMode(b, before, ColorMode::CMYK, cmykProfile());
    REQUIRE(balanced);
    CHECK(ink(balanced, 0, 0, 0) < 10);
    CHECK_EQ(stored(balanced, 0, 0, 3), stored(before, 0, 0, 3));
    // Not offered in CMYK: Vibrance, Black & White, Exposure (null, pixels untouched).
    CHECK(!adjustedInMode(settingsOf(AdjustmentKind::Vibrance), before, ColorMode::CMYK, cmykProfile()));
    CHECK(!adjustedInMode(settingsOf(AdjustmentKind::Exposure), before, ColorMode::CMYK, cmykProfile()));
    CHECK(!adjustedInMode(settingsOf(AdjustmentKind::ColorLookup), before, ColorMode::CMYK, cmykProfile()));

    // Lab: Photo Filter (warming, Preserve Luminosity) warms a and b and keeps L; Exposure +1 lifts L alone.
    const AnyImage lab = labImage<S>(1, 1, [](int, int) { return std::array<double, 3>{50, 0, 0}; });
    const AnyImage warm = adjustedInMode(settingsOf(AdjustmentKind::PhotoFilter), lab, ColorMode::Lab, ColorProfile());
    REQUIRE(warm);
    CHECK_EQ(stored(warm, 0, 0, 0), stored(lab, 0, 0, 0));
    CHECK(labAt(warm, 0, 0, 2) > 5);
    AdjustmentSettings e = settingsOf(AdjustmentKind::Exposure);
    e.exposure.exposure = 1;
    const AnyImage bright = adjustedInMode(e, lab, ColorMode::Lab, ColorProfile());
    REQUIRE(bright);
    CHECK(labAt(bright, 0, 0, 0) > 60);
    CHECK_EQ(stored(bright, 0, 0, 1), stored(lab, 0, 0, 1));
    CHECK(!adjustedInMode(settingsOf(AdjustmentKind::HueSaturation), lab, ColorMode::Lab, ColorProfile()));
    CHECK(!adjustedInMode(settingsOf(AdjustmentKind::ChannelMixer), lab, ColorMode::Lab, ColorProfile()));
}

TEST_CASE(colour_kinds_in_cmyk_and_lab) {
    colourKinds<SampleType::U8>();
    colourKinds<SampleType::U16>();
}

template <SampleType S>
void filtersOverEverySample() {
    // Black ink on the left half only: a blur spreads the black plate across the edge and leaves the flat cyan flat.
    const AnyImage before = cmykImage<S>(16, 16, [](int x, int) { return Inks{40, 0, 0, x < 8 ? 100.0 : 0.0}; });
    FilterSettings f;
    f.radius = 2;
    const AnyImage blurred = filteredInMode(FilterKind::GaussianBlur, before, ColorMode::CMYK, f);
    REQUIRE(blurred);
    CHECK_EQ(blurred.channels(), 5);
    CHECK(ink(blurred, 7, 8, 3) < 99 && ink(blurred, 7, 8, 3) > 50);
    CHECK(ink(blurred, 8, 8, 3) > 1 && ink(blurred, 8, 8, 3) < 50);
    CHECK_NEAR(ink(blurred, 8, 8, 0), 40, 1);
    // Monochromatic noise in Lab changes L alone.
    const AnyImage lab = labImage<S>(8, 8, [](int, int) { return std::array<double, 3>{50, 20, -20}; });
    FilterSettings n;
    n.amount = 30;
    n.monochromatic = true;
    const AnyImage noisy = filteredInMode(FilterKind::AddNoise, lab, ColorMode::Lab, n, 1, 7);
    REQUIRE(noisy);
    bool lMoved = false;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            lMoved = lMoved || stored(noisy, x, y, 0) != stored(lab, x, y, 0);
            CHECK_EQ(stored(noisy, x, y, 1), stored(lab, x, y, 1));
            CHECK_EQ(stored(noisy, x, y, 2), stored(lab, x, y, 2));
        }
    CHECK(lMoved);
    // Monochromatic noise in CMYK moves every ink by the same amount.
    const AnyImage cmyk = cmykImage<S>(8, 8, [](int, int) { return Inks{50, 50, 50, 50}; });
    const AnyImage cmykNoisy = filteredInMode(FilterKind::AddNoise, cmyk, ColorMode::CMYK, n, 1, 7);
    for (int x = 0; x < 8; x++) for (int k = 1; k < 4; k++) CHECK_EQ(stored(cmykNoisy, x, 3, k), stored(cmykNoisy, x, 3, 0));
}

TEST_CASE(filters_over_every_sample) {
    filtersOverEverySample<SampleType::U8>();
    filtersOverEverySample<SampleType::U16>();
}

template <SampleType S>
void layerDrawsAsPixelEdit() {
    // A Selective Color adjustment layer over a CMYK layer draws what Image > Adjustments makes of the same pixels.
    Document doc(6, 1);
    doc.sampleType = S;
    doc.colorMode = ColorMode::CMYK;
    doc.profile = cmykProfile();
    const AnyImage pixels = cmykImage<S>(6, 1, [](int x, int) { return Inks{15.0 * x, 90 - 10.0 * x, 60, 5.0 * x}; });
    doc.layers = {Layer(Asset::makeAny(pixels, "base"), Point(0, 0))};
    AdjustmentSettings s = settingsOf(AdjustmentKind::SelectiveColor);
    s.selectiveColor.ranges[0] = {-20, 10, 0, 30};
    s.selectiveColor.ranges[7] = {5, 0, -10, 0};
    Layer adjustment("Selective Color", doc.size());
    adjustment.adjustment = s.toLayerAdjustment();
    doc.layers.push_back(adjustment);
    const AnyImage drawn = renderNative(doc);
    const AnyImage edited = adjustedInMode(s, pixels, ColorMode::CMYK, cmykProfile());
    REQUIRE(drawn && edited);
    for (int x = 0; x < 6; x++) for (int k = 0; k < 5; k++) CHECK_NEAR(stored(drawn, x, 0, k), stored(edited, x, 0, k), 1);
}

TEST_CASE(adjustment_layer_draws_as_the_pixel_edit) {
    layerDrawsAsPixelEdit<SampleType::U8>();
    layerDrawsAsPixelEdit<SampleType::U16>();
}

TEST_CASE(cmyk_slots_stay_out_of_rgb_settings) {
    // RGB settings keep their four Levels ranges and Curves channels, and no ink rows; CMYK's black slot and ink rows
    // are written when set and read back.
    const std::string levels = settingsOf(AdjustmentKind::Levels).toJson();
    CHECK(levels.find("\"Black\"") == std::string::npos);
    AdjustmentSettings s = settingsOf(AdjustmentKind::Levels);
    s.levels.ranges[4].black = 20;
    s.levels.channel = 4;
    AdjustmentSettings back;
    REQUIRE(AdjustmentSettings::parse(s.toJson(), back));
    CHECK(back == s);
    AdjustmentSettings c = settingsOf(AdjustmentKind::Curves);
    c.curves.channels[4] = {{0, 0}, {128, 100}, {255, 255}};
    REQUIRE(AdjustmentSettings::parse(c.toJson(), back));
    CHECK(back == c);
    const std::string mixer = settingsOf(AdjustmentKind::ChannelMixer).toJson();
    CHECK(mixer.find("cyan") == std::string::npos);
    AdjustmentSettings m = settingsOf(AdjustmentKind::ChannelMixer);
    m.channelMixer.inks[3] = {10, 20, 30, 40, 5};
    REQUIRE(AdjustmentSettings::parse(m.toJson(), back));
    CHECK(back == m);
    int channel = -1;
    CHECK(parseLevelsChannel("Lightness", channel) && channel == 1);
    CHECK(parseLevelsChannel("Cyan", channel) && channel == 1);
    CHECK(parseLevelsChannel("Black", channel) && channel == 4);
}

TEST_CASE(channel_mixer_sleeps_in_lab) {
    // Photoshop greys Channel Mixer in Lab: converting keeps the layer, hidden and marked.
    CHECK(!adjustmentOfferedInMode(AdjustmentKind::ChannelMixer, ColorMode::Lab));
    CHECK(adjustmentOfferedInMode(AdjustmentKind::ChannelMixer, ColorMode::CMYK));
    CHECK(!adjustmentAppliesInMode(AdjustmentKind::ColorLookup, ColorMode::Lab));
    CHECK(adjustmentAppliesInMode(AdjustmentKind::ColorLookup, ColorMode::RGB));
}

TEST_CASE(histograms_per_channel) {
    const AnyImage cmyk = cmykImage<SampleType::U8>(2, 1, [](int x, int) { return x == 0 ? Inks{100, 0, 0, 0} : Inks{0, 0, 0, 100}; });
    const auto bins = levelsHistogramInMode(cmyk, ColorMode::CMYK);
    CHECK_NEAR(bins[1][0], 1, 1e-9);     // cyan plate: one pixel at full ink (stored 0)
    CHECK_NEAR(bins[4][255], 1, 1e-9);   // black plate: one pixel without ink
    const AnyImage lab = labImage<SampleType::U16>(1, 1, [](int, int) { return std::array<double, 3>{100, 0, 0}; });
    const auto labBins = levelsHistogramInMode(lab, ColorMode::Lab);
    CHECK(labBins[0].empty());
    CHECK_NEAR(labBins[1][255], 1, 1e-9);
    CHECK_NEAR(labBins[2][128], 1, 1e-9);
}

} // namespace

TEST_MAIN()
