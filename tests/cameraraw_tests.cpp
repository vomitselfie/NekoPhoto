// Camera Raw Filter: the expected values of upstream Compositor's CompositorTests/CameraRawTests.swift
// (MIT, see LICENSES/MIT-Compositor.txt), run against the core port. The session tests upstream (the
// eyedropper on a canvas, OK as one undo step) are app behaviour and are covered by tools/rpc_smoke.py.
#include "check.h"
#include "compositor/cameraraw.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

using namespace compositor;

namespace {

using Pixel = std::array<int, 4>;   // straight RGBA

/// A `width` x `height` image of one straight sRGB colour (0..1) at `alpha`, premultiplied as Core Graphics fills it.
Image solid(int width, int height, double r, double g, double b, double alpha = 1) {
    Image image(width, height);
    const int a = int(std::lround(alpha * 255));
    auto pre = [a](double c) { return uint8_t(std::lround(c * a)); };
    image.fill(pre(r), pre(g), pre(b), uint8_t(a));
    return image;
}

Image gray(int width = 4, int height = 4, double alpha = 1) { return solid(width, height, 128 / 255.0, 128 / 255.0, 128 / 255.0, alpha); }

void set(Image& image, int x, int y, double r, double g, double b) {
    uint8_t* p = image.pixel(x, y);
    p[0] = uint8_t(std::lround(r * 255)); p[1] = uint8_t(std::lround(g * 255)); p[2] = uint8_t(std::lround(b * 255)); p[3] = 255;
}

/// Straight RGBA per pixel, top row first, unpremultiplied the way upstream's test reads them back.
std::vector<Pixel> pixels(const Image& image) {
    std::vector<Pixel> out;
    for (int y = 0; y < image.height(); y++)
        for (int x = 0; x < image.width(); x++) {
            const uint8_t* p = image.pixel(x, y);
            const int a = p[3];
            Pixel px{0, 0, 0, a};
            for (size_t c = 0; c < 3; c++) px[c] = a == 0 ? 0 : std::min(255, (p[c] * 255 + a / 2) / a);
            out.push_back(px);
        }
    return out;
}

int chroma(const Pixel& p) { return std::max({p[0], p[1], p[2]}) - std::min({p[0], p[1], p[2]}); }

Image applied(const Image& source, const CameraRawSettings& settings, uint32_t seed = 0, const CameraRawPreview& preview = {}) {
    Image out = source;
    applyCameraRaw(out, settings, 1, seed, preview);
    return out;
}

/// A checkerboard of 2x2 cells: a warp only shows in an image whose pixels differ.
Image checker(int width = 24, int height = 24) {
    Image image(width, height);
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) {
            const double v = ((x / 2) + (y / 2)) % 2 == 0 ? 0.9 : 0.1;
            set(image, x, y, v, v, v);
        }
    return image;
}

/// A wide step from 40 to 200, so a small blur and a wide blur reach different pixels.
Image step() {
    Image image(24, 4);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 24; x++) {
            const double v = (x < 12 ? 40 : 200) / 255.0;
            set(image, x, y, v, v, v);
        }
    return image;
}

int byteAt(const Image& image, int x) { return pixels(image)[size_t(x)][0]; }

Image pair(double left, double right) {
    Image image(2, 1);
    set(image, 0, 0, left, left, left);
    set(image, 1, 0, right, right, right);
    return image;
}

} // namespace

TEST_CASE(defaults_leave_pixels_and_alpha_alone) {
    Image input = gray(4, 4, 0.5);
    CHECK(pixels(applied(input, CameraRawSettings())) == pixels(input));
    CameraRawSettings broken;
    broken.exposure = NAN;
    broken.temperature = 400;
    CHECK(!broken.isValid());
    CHECK(broken.normalized().exposure == 0 && broken.normalized().temperature == 100);
    Image untouched = input;
    CHECK(applyCameraRaw(untouched, broken));   // normalized first, so it runs
}

TEST_CASE(exposure_adds_one_stop_and_contrast_pivots_around_mid_gray) {
    CameraRawSettings settings;
    settings.exposure = 1;
    Pixel brighter = pixels(applied(gray(), settings))[0];
    CHECK(std::abs(brighter[0] - 176) <= 2);
    CHECK(brighter[0] == brighter[1] && brighter[1] == brighter[2]);
    Image translucent = gray(4, 4, 0.5);
    CHECK_EQ(pixels(applied(translucent, settings))[0][3], pixels(translucent)[0][3]);

    Image both = pair(64 / 255.0, 192 / 255.0);
    settings = CameraRawSettings();
    settings.contrast = 100;
    auto pushed = pixels(applied(both, settings));
    CHECK(pushed[0][0] < 10 && pushed[1][0] > 250);
    settings.contrast = -100;
    auto flat = pixels(applied(both, settings));
    CHECK(std::abs(flat[0][0] - 128) <= 2 && std::abs(flat[1][0] - 128) <= 2);
}

TEST_CASE(tonal_sliders_move_the_end_they_name) {
    Image brightAndMid = pair(230 / 255.0, 128 / 255.0);
    CameraRawSettings settings;
    settings.highlights = -100;
    auto recovered = pixels(applied(brightAndMid, settings));
    CHECK(recovered[0][0] < 200);
    CHECK(std::abs(recovered[1][0] - 128) <= 2);

    settings = CameraRawSettings();
    settings.whites = 100;
    auto clipped = pixels(applied(brightAndMid, settings));
    CHECK_EQ(clipped[0][0], 255);
    CHECK(std::abs(clipped[1][0] - 128) <= 2);
    CameraRawPreview highlightView;
    highlightView.clipping = CameraRawClipping::Highlights;
    auto viz = pixels(applied(brightAndMid, settings, 0, highlightView));
    CHECK((viz[0] == Pixel{255, 255, 255, 255}) && (viz[1] == Pixel{0, 0, 0, 255}));

    Image darkAndMid = pair(20 / 255.0, 128 / 255.0);
    settings = CameraRawSettings();
    settings.shadows = 100;
    auto opened = pixels(applied(darkAndMid, settings));
    CHECK(opened[0][0] > 50);
    CHECK(std::abs(opened[1][0] - 128) <= 2);

    settings = CameraRawSettings();
    settings.blacks = -100;
    auto crushed = pixels(applied(darkAndMid, settings));
    CHECK(crushed[0][0] < 20);
    CHECK(std::abs(crushed[1][0] - 128) <= 2);
    CameraRawPreview shadowView;
    shadowView.clipping = CameraRawClipping::Shadows;
    auto shadowViz = pixels(applied(darkAndMid, settings, 0, shadowView));
    CHECK((shadowViz[0] == Pixel{0, 0, 0, 255}) && (shadowViz[1] == Pixel{255, 255, 255, 255}));
}

TEST_CASE(temperature_warms_and_tint_moves_toward_magenta) {
    CameraRawSettings settings;
    settings.temperature = 100;
    Pixel warm = pixels(applied(gray(), settings))[0];
    CHECK(warm[0] > 128 && warm[2] < 128 && warm[0] > warm[2]);
    settings = CameraRawSettings();
    settings.tint = 100;
    Pixel magenta = pixels(applied(gray(), settings))[0];
    CHECK(magenta[1] < 128 && magenta[1] < magenta[0] && magenta[1] < magenta[2]);
}

TEST_CASE(vibrance_favors_dull_colors_and_protects_skin_while_saturation_does_not) {
    Image dullGreen = solid(4, 4, 77 / 255.0, 153 / 255.0, 77 / 255.0);
    Image saturatedGreen = solid(4, 4, 20 / 255.0, 200 / 255.0, 20 / 255.0);
    Image skin = solid(4, 4, 153 / 255.0, 115 / 255.0, 77 / 255.0);
    CameraRawSettings settings;
    settings.vibrance = 100;
    const int dullBefore = chroma(pixels(dullGreen)[0]), saturatedBefore = chroma(pixels(saturatedGreen)[0]), skinBefore = chroma(pixels(skin)[0]);
    const int dullDelta = chroma(pixels(applied(dullGreen, settings))[0]) - dullBefore;
    const int saturatedDelta = chroma(pixels(applied(saturatedGreen, settings))[0]) - saturatedBefore;
    const int skinDelta = chroma(pixels(applied(skin, settings))[0]) - skinBefore;
    CHECK(dullDelta > saturatedDelta + 10);
    CHECK_EQ(dullBefore, skinBefore);
    CHECK(dullDelta > skinDelta + 10);

    Image red = solid(4, 4, 160 / 255.0, 120 / 255.0, 120 / 255.0);
    Image blue = solid(4, 4, 100 / 255.0, 100 / 255.0, 140 / 255.0);
    settings = CameraRawSettings();
    settings.saturation = 100;
    const double redRatio = double(chroma(pixels(applied(red, settings))[0])) / chroma(pixels(red)[0]);
    const double blueRatio = double(chroma(pixels(applied(blue, settings))[0])) / chroma(pixels(blue)[0]);
    CHECK_NEAR(redRatio, 2, 0.15);
    CHECK_NEAR(blueRatio, 2, 0.15);
}

TEST_CASE(eyedropper_and_auto_neutralize_a_warm_pixel) {
    const double r = 160 / 255.0, g = 140 / 255.0, b = 120 / 255.0;
    Image warm = solid(8, 8, r, g, b);
    const Pixel before = pixels(warm)[0];
    auto solved = CameraRawSettings::neutralizeStraight(r, g, b);
    REQUIRE(solved);
    CameraRawSettings settings;
    settings.temperature = (*solved)[0];
    settings.tint = (*solved)[1];
    CHECK(chroma(pixels(applied(warm, settings))[0]) < chroma(before) / 2);

    auto automatic = CameraRawSettings::autoBalance(warm);
    REQUIRE(automatic);
    settings.temperature = (*automatic)[0];
    settings.tint = (*automatic)[1];
    CHECK(chroma(pixels(applied(warm, settings))[0]) < chroma(before) / 2);

    CHECK(!CameraRawSettings::autoBalance(solid(8, 8, 1, 0, 0, 0)));   // no coverage, no answer
    CHECK(!CameraRawSettings::neutralize(0, 0.5, 0.5));
}

TEST_CASE(hidden_group_is_left_out) {
    Image input = gray(8, 8);
    CameraRawSettings settings;
    settings.exposure = 1;
    settings.temperature = 40;
    CameraRawPanels panels;
    panels.light = false;
    CameraRawSettings hiddenLight = settings.applying(panels);
    CHECK(pixels(applied(input, hiddenLight)) != pixels(input));
    CHECK(hiddenLight.exposure == 0 && hiddenLight.temperature == 40);
    panels.color = false;
    CameraRawSettings neither = settings.applying(panels);
    CHECK(neither.isIdentity());
    CHECK(pixels(applied(input, neither)) == pixels(input));
}

TEST_CASE(texture_and_clarity_sharpen_an_edge_and_leave_a_flat_field) {
    Image flat = gray();
    CameraRawSettings settings;
    settings.texture = 100;
    settings.clarity = 100;
    CHECK(pixels(applied(flat, settings)) == pixels(flat));

    Image edge = step();
    const int far = byteAt(edge, 0), near = byteAt(edge, 8), atEdge = byteAt(edge, 11);
    settings = CameraRawSettings();
    settings.texture = 100;
    Image textured = applied(edge, settings);
    CHECK_EQ(byteAt(textured, 0), far);
    CHECK_EQ(byteAt(textured, 8), near);   // texture's fine radius does not reach this far
    CHECK(byteAt(textured, 11) != atEdge);

    settings = CameraRawSettings();
    settings.clarity = 100;
    Image clarified = applied(edge, settings);
    CHECK_EQ(byteAt(clarified, 0), far);
    CHECK(byteAt(clarified, 8) != near);   // clarity's wider radius reaches further in
    settings.clarity = -100;
    Image softened = applied(edge, settings);
    const int hardGap = std::abs(byteAt(edge, 12) - byteAt(edge, 11));
    const int softGap = std::abs(byteAt(softened, 12) - byteAt(softened, 11));
    CHECK(softGap < hardGap);
}

TEST_CASE(dehaze_deepens_or_lifts_and_keeps_alpha) {
    Image dark = solid(4, 4, 30 / 255.0, 30 / 255.0, 30 / 255.0);
    Image pale = solid(4, 4, 180 / 255.0, 150 / 255.0, 150 / 255.0);
    CameraRawSettings settings;
    settings.dehaze = 100;
    CHECK(pixels(applied(dark, settings))[0][0] < 30);
    const Pixel paleBefore = pixels(pale)[0];
    CHECK(chroma(pixels(applied(pale, settings))[0]) > chroma(paleBefore));
    settings.dehaze = -100;
    CHECK(pixels(applied(dark, settings))[0][0] > 30);
    CHECK(chroma(pixels(applied(pale, settings))[0]) < chroma(paleBefore));
    Image translucent = solid(4, 4, 30 / 255.0, 30 / 255.0, 30 / 255.0, 0.5);
    settings.dehaze = 100;
    CHECK_EQ(pixels(applied(translucent, settings))[0][3], pixels(translucent)[0][3]);
}

TEST_CASE(glow_is_idle_at_zero_and_halation_fringe_is_redder_than_diffusion) {
    Image spot = solid(21, 21, 0, 0, 0);
    for (int y = 8; y < 13; y++) for (int x = 8; x < 13; x++) set(spot, x, y, 1, 1, 1);
    CameraRawSettings settings;
    settings.glowWarmth = 100;
    settings.glowRange = 100;
    CHECK(pixels(applied(spot, settings)) == pixels(spot));

    settings.glow = 100;
    settings.glowStyle = CameraRawGlowStyle::Diffusion;
    auto diffusion = pixels(applied(spot, settings));
    settings.glowStyle = CameraRawGlowStyle::Halation;
    auto halation = pixels(applied(spot, settings));
    const size_t fringe = 10 * 21 + 15, far = 0;
    CHECK(diffusion[fringe][0] > diffusion[far][0]);
    CHECK((halation[far] == Pixel{0, 0, 0, 255}));
    CHECK(halation[fringe][0] - halation[fringe][1] > diffusion[fringe][0] - diffusion[fringe][1]);
}

TEST_CASE(vignette_darkens_corners_and_highlights_only_while_darkening) {
    Image field = gray(9, 9);
    CameraRawSettings settings;
    settings.vignetteAmount = -100;
    auto darkened = pixels(applied(field, settings));
    const int center = darkened[4 * 9 + 4][0], corner = darkened[0][0];
    CHECK(std::abs(center - 128) <= 2);
    CHECK(corner < center - 40);

    Image white = solid(9, 9, 1, 1, 1);
    settings.vignetteHighlights = 100;
    settings.vignetteStyle = CameraRawVignetteStyle::HighlightPriority;
    const int protectedCorner = pixels(applied(white, settings))[0][0];
    settings.vignetteHighlights = 0;
    const int exposed = pixels(applied(white, settings))[0][0];
    CHECK(protectedCorner > exposed + 40);
    settings.vignetteHighlights = 100;
    settings.vignetteStyle = CameraRawVignetteStyle::PaintOverlay;
    CHECK(pixels(applied(white, settings))[0][0] < protectedCorner);

    settings = CameraRawSettings();
    settings.vignetteAmount = 100;
    auto plain = pixels(applied(field, settings));
    settings.vignetteHighlights = 100;
    CHECK(plain == pixels(applied(field, settings)));   // Highlights is idle while the vignette lightens
}

TEST_CASE(grain_is_stable_and_the_effects_panel_drops_the_whole_group) {
    Image field = gray(16, 16);
    CameraRawSettings settings;
    settings.grainSize = 40;
    settings.grainRoughness = 80;
    CHECK(pixels(applied(field, settings, 4)) == pixels(field));
    settings.grainAmount = 70;
    auto first = pixels(applied(field, settings, 4));
    CHECK(first == pixels(applied(field, settings, 4)));
    CHECK(first != pixels(field));
    CHECK(first[0][0] == first[0][1] && first[0][1] == first[0][2]);   // brightness only
    CHECK_EQ(pixels(applied(solid(4, 4, 0.5, 0.5, 0.5, 0), settings, 4))[0][3], 0);

    Image edge = step();
    settings = CameraRawSettings();
    settings.texture = 100;
    settings.grainAmount = 50;
    CameraRawPanels panels;
    panels.effects = false;
    CameraRawSettings hidden = settings.applying(panels);
    CHECK(hidden.isIdentity());
    CHECK(pixels(applied(edge, hidden, 2)) == pixels(edge));
    CHECK(pixels(applied(edge, settings, 2)) != pixels(edge));
}

TEST_CASE(clipping_indicators_paint_the_preview_only) {
    CameraRawPreview shadows;
    shadows.shadowClipIndicator = true;
    Pixel shadowed = pixels(applied(solid(4, 4, 0, 0, 0), CameraRawSettings(), 0, shadows))[0];
    CHECK(shadowed[2] > shadowed[0]);   // clipped shadows are painted blue
    CameraRawPreview highlights;
    highlights.highlightClipIndicator = true;
    Pixel highlighted = pixels(applied(solid(4, 4, 1, 1, 1), CameraRawSettings(), 0, highlights))[0];
    CHECK(highlighted[0] > highlighted[2]);   // clipped highlights are painted red
    Image black = solid(4, 4, 0, 0, 0);
    CHECK(pixels(applied(black, CameraRawSettings())) == pixels(black));
}

TEST_CASE(curve_mixer_and_grading_change_only_their_own_tones) {
    Image dark = solid(4, 4, 0.12, 0.12, 0.12);
    Image light = solid(4, 4, 0.62, 0.62, 0.62);
    CameraRawSettings settings;
    settings.curve.shadows = 100;
    const int darkGain = pixels(applied(dark, settings))[0][0] - int(std::lround(0.12 * 255));
    const int lightGain = pixels(applied(light, settings))[0][0] - int(std::lround(0.62 * 255));
    CHECK(darkGain > lightGain + 8);

    settings = CameraRawSettings();
    settings.curve.rgb = CameraRawCurveSettings::strongContrast();
    CHECK(pixels(applied(solid(4, 4, 0.25, 0.25, 0.25), settings))[0][0] < 55);

    settings = CameraRawSettings();
    settings.mixer.hue[0] = 100;
    Pixel shifted = pixels(applied(solid(4, 4, 1, 0, 0), settings))[0];
    CHECK(shifted[1] > shifted[2]);   // reds hue moves red toward orange

    settings = CameraRawSettings();
    settings.grading.shadows.saturation = 100;
    Pixel gradedDark = pixels(applied(dark, settings))[0];
    Pixel gradedLight = pixels(applied(solid(4, 4, 1, 1, 1), settings))[0];
    CHECK(gradedDark[0] > gradedDark[1] + 5);
    CHECK(std::abs(gradedLight[0] - gradedLight[1]) <= 2);
    settings.grading.balance = 100;
    Pixel balanced = pixels(applied(dark, settings))[0];
    CHECK(balanced[0] - balanced[1] < gradedDark[0] - gradedDark[1]);   // toward highlights weakens the shadow tint

    settings = CameraRawSettings();
    settings.curve.shadows = 100;
    CameraRawPanels panels;
    panels.curve = false;
    CHECK(pixels(applied(dark, settings.applying(panels))) == pixels(dark));
}

TEST_CASE(detail_sharpening_noise_and_masking_preview) {
    Image edge = step();
    CameraRawSettings settings;
    settings.detail.sharpenAmount = 150;
    settings.detail.sharpenRadius = 50;
    CHECK(pixels(applied(edge, settings)) != pixels(edge));

    settings = CameraRawSettings();
    settings.detail.noiseLuminance = 80;
    Image flat = gray(8, 8);
    CHECK(pixels(applied(flat, settings)) == pixels(flat));   // luminance NR leaves a flat field alone

    settings.detail.sharpenMasking = 50;
    CameraRawPreview mask;
    mask.sharpenMask = true;
    auto maskPixels = pixels(applied(edge, settings, 0, mask));
    CHECK(std::all_of(maskPixels.begin(), maskPixels.end(), [](const Pixel& p) { return p[0] == p[1] && p[1] == p[2]; }));
    CHECK(settings.detail.adjusts());
}

TEST_CASE(optics_distortion_defringe_and_detail_panel) {
    Image grid = checker();
    CameraRawSettings settings;
    settings.optics.distortion = 100;
    CHECK(pixels(applied(grid, settings)) != pixels(grid));

    Image purple = solid(4, 4, 0.8, 0.2, 0.9);
    settings = CameraRawSettings();
    settings.optics.purpleAmount = 100;
    settings.optics.purpleHueLow = 250;
    settings.optics.purpleHueHigh = 320;
    CHECK(chroma(pixels(applied(purple, settings))[0]) < chroma(pixels(purple)[0]));

    settings = CameraRawSettings();
    settings.optics.removeChromaticAberration = true;
    CHECK(settings.optics.adjusts());
    settings = CameraRawSettings();
    settings.detail.sharpenAmount = 40;
    CameraRawPanels panels;
    panels.detail = false;
    Image edge = step();
    CHECK(pixels(applied(edge, settings.applying(panels))) == pixels(edge));
}

TEST_CASE(geometry_warp_and_calibration_primaries) {
    Image grid = checker(12, 12);
    CameraRawSettings settings;
    settings.geometry.vertical = 40;
    CHECK(pixels(settings.geometry.apply(grid)) != pixels(grid));

    settings = CameraRawSettings();
    settings.calibration.redHue = 80;
    Image red = solid(4, 4, 1, 0, 0);
    const Pixel before = pixels(red)[0];
    CHECK(pixels(applied(red, settings))[0] != before);
    CameraRawPanels panels;
    panels.calibration = false;
    CHECK(pixels(applied(red, settings.applying(panels)))[0] == before);
}

TEST_CASE(guided_upright_follows_a_drawn_line_and_leaves_an_unguided_picture) {
    Image cool = solid(16, 16, 0.2, 0.45, 0.8);
    Image warm = solid(16, 16, 0.85, 0.25, 0.15);
    CameraRawSettings settings;
    settings.geometry.upright = CameraRawUprightMode::Guided;
    CHECK(pixels(applied(cool, settings)) == pixels(cool));
    CHECK(pixels(applied(warm, settings)) == pixels(warm));

    settings.geometry.guides = {CameraRawGeometryGuide{0.1, 0.15, 0.9, 0.8}};
    auto coolGuided = pixels(applied(cool, settings));
    auto warmGuided = pixels(applied(warm, settings));
    CHECK(coolGuided != pixels(cool));
    CHECK(warmGuided != pixels(warm));
    CHECK(coolGuided != warmGuided);
}

// ---- beyond upstream's tests: the port's own seams -----------------------------------------------

TEST_CASE(geometry_identity_keeps_pixels_and_zero_rotation_corners_are_the_frame) {
    CameraRawGeometrySettings geometry;
    auto corners = geometry.outputCorners(10, 6);
    CHECK_NEAR(corners[0][0], 0, 1e-9); CHECK_NEAR(corners[0][1], 0, 1e-9);
    CHECK_NEAR(corners[2][0], 10, 1e-9); CHECK_NEAR(corners[2][1], 6, 1e-9);
    // Offset Y is upward in upstream's y-up space: the picture moves up on the grid.
    geometry.offsetY = 100;
    corners = geometry.outputCorners(10, 6);
    CHECK(corners[0][1] < 0);
    Image grid = checker(12, 12);
    CHECK(pixels(CameraRawGeometrySettings().apply(grid)) == pixels(grid));
    // Constrain Crop scales the covered box back up to the frame (upstream crops to the alpha bounds, so a
    // rotation, whose box still spans the frame, keeps its empty corners; a zoom out does not).
    geometry = {};
    geometry.scale = -50;
    CHECK(geometry.apply(grid).pixel(0, 0)[3] == 0);
    geometry.constrainCrop = true;
    CHECK(geometry.apply(grid).pixel(0, 0)[3] > 0);
}

TEST_CASE(json_round_trips_and_refuses_unknown_keys) {
    CameraRawSettings settings;
    settings.exposure = 0.5;
    settings.glowStyle = CameraRawGlowStyle::Halation;
    settings.curve.rgb = CameraRawCurveSettings::mediumContrast();
    settings.mixer.hue[2] = -30;
    settings.mixer.points.push_back({200, 0.5, 0.4, 10, 0, 0, 30, 0.4, 0.4});
    settings.grading.global.luminance = 12;
    settings.geometry.guides.push_back({0.1, 0.1, 0.9, 0.2});
    settings.calibration.process = 3;
    CameraRawSettings back;
    CHECK(CameraRawSettings::parse(settings.toJson(), back));
    CHECK(back == settings);

    CameraRawSettings patched;
    CHECK(CameraRawSettings::parse(R"({"exposure": 1, "detail": {"sharpenAmount": 40}, "mixer": {"hue": {"reds": 20}}})", patched));
    CHECK(patched.exposure == 1 && patched.detail.sharpenAmount == 40 && patched.mixer.hue[0] == 20 && patched.detail.sharpenRadius == 10);
    std::string error;
    CameraRawSettings untouched;
    CHECK(!CameraRawSettings::parse(R"({"exposure": 1, "bogus": 2})", untouched, &error));
    CHECK(error.find("bogus") != std::string::npos);
    CHECK(untouched.exposure == 0);
    CHECK(!CameraRawSettings::parse(R"({"detail": {"sharpen": 2}})", untouched, &error));
    CHECK(!CameraRawSettings::parse(R"({"glowStyle": "Sparkle"})", untouched, &error));
    CHECK(!CameraRawSettings::parse(R"({"exposure": "high"})", untouched, &error));
}

TEST_CASE(a_reduced_preview_matches_upstream_scale_for_grain_positions) {
    // Grain at scale 1 and at scale 1 over a copy is the same pattern: it depends on position and seed only.
    CameraRawSettings settings;
    settings.grainAmount = 60;
    Image field = gray(32, 32);
    CHECK(pixels(applied(field, settings, 9)) == pixels(applied(field, settings, 9)));
    Image big = field, small = gray(16, 16);
    CHECK(applyCameraRaw(big, settings, 1, 9));
    CHECK(applyCameraRaw(small, settings, 0.5, 9));
    CHECK(pixels(small) != pixels(gray(16, 16)));
}

TEST_MAIN()
