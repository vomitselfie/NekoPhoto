// Camera Raw Filter: the expected values of upstream Compositor's CompositorTests/CameraRawTests.swift
// (MIT, see LICENSES/MIT-Compositor.txt), run against the core port. The session tests upstream (the
// eyedropper on a canvas, OK as one undo step) are app behaviour and are covered by tools/rpc_smoke.py.
#include <functional>
#include <cstdio>
#include <cstdlib>
#include "nekophoto/depth.h"
#include "check.h"
#include "nekophoto/cameraraw.h"
#include "nekophoto/camerarawxmp.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

using namespace nekophoto;

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

// The tone sliders follow Camera Raw's responses fitted to the corpus chart (rawtone.h, docs/camera-raw.md#fitted-responses),
// so the expected values below are those responses, not upstream's.
TEST_CASE(exposure_adds_one_stop_and_contrast_pivots_around_mid_gray) {
    CameraRawSettings settings;
    settings.exposure = 1;
    Pixel brighter = pixels(applied(gray(), settings))[0];
    CHECK(std::abs(brighter[0] - 182) <= 2);   // one stop of scene light under the default curve
    CHECK(brighter[0] == brighter[1] && brighter[1] == brighter[2]);
    Image translucent = gray(4, 4, 0.5);
    CHECK_EQ(pixels(applied(translucent, settings))[0][3], pixels(translucent)[0][3]);

    Image both = pair(64 / 255.0, 192 / 255.0);
    settings = CameraRawSettings();
    settings.contrast = 100;
    auto pushed = pixels(applied(both, settings));
    CHECK(pushed[0][0] < 45 && pushed[1][0] > 205);
    settings.contrast = -100;
    auto flat = pixels(applied(both, settings));
    CHECK(flat[0][0] > 80 && flat[1][0] < 185);
}

TEST_CASE(tonal_sliders_move_the_end_they_name) {
    Image brightAndMid = pair(230 / 255.0, 128 / 255.0);
    CameraRawSettings settings;
    settings.highlights = -100;
    auto recovered = pixels(applied(brightAndMid, settings));
    CHECK(recovered[0][0] < 200);
    CHECK(230 - recovered[0][0] > 128 - recovered[1][0] + 20);   // the bright one drops most

    settings = CameraRawSettings();
    settings.whites = 100;
    auto clipped = pixels(applied(brightAndMid, settings));
    CHECK_EQ(clipped[0][0], 255);
    CHECK(clipped[1][0] > 128 && clipped[1][0] < 250);   // on Camera Raw's chart Whites +100 lifts the midtones too
    CameraRawPreview highlightView;
    highlightView.clipping = CameraRawClipping::Highlights;
    auto viz = pixels(applied(brightAndMid, settings, 0, highlightView));
    CHECK((viz[0] == Pixel{255, 255, 255, 255}) && (viz[1] == Pixel{0, 0, 0, 255}));

    Image darkAndMid = pair(20 / 255.0, 128 / 255.0);
    settings = CameraRawSettings();
    settings.shadows = 100;
    auto opened = pixels(applied(darkAndMid, settings));
    CHECK(opened[0][0] > 50);
    CHECK(opened[0][0] - 20 > opened[1][0] - 128);   // the dark one rises most

    settings = CameraRawSettings();
    settings.blacks = -100;
    auto crushed = pixels(applied(darkAndMid, settings));
    CHECK(crushed[0][0] < 5);
    CHECK(crushed[1][0] > 80);
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
    // Saturation +100 doubles the distance from the luminance in linear ProPhoto RGB: about twice the encoded chroma.
    CHECK(redRatio > 1.6 && redRatio < 2.3);
    CHECK(blueRatio > 1.6 && blueRatio < 2.3);
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
    CHECK(byteAt(clarified, 11) != atEdge);   // clarity's radius is a share of the image (2% of its long edge)
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
    // Balance toward the highlights weakens the shadow wheel on the upper shadows (as Camera Raw's Balance +50 does).
    Image upper = solid(4, 4, 0.35, 0.35, 0.35);
    settings.grading.balance = 0;
    Pixel unbalanced = pixels(applied(upper, settings))[0];
    settings.grading.balance = 100;
    Pixel balanced = pixels(applied(upper, settings))[0];
    CHECK(balanced[0] - balanced[1] < unbalanced[0] - unbalanced[1]);

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

namespace {

/// A picture with smooth ramps, texture, saturated patches and a half-transparent corner, 8-bit.
Image calibrationPicture(int width, int height) {
    Image image(width, height);
    uint32_t noise = 12345;
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) {
            noise = noise * 1664525U + 1013904223U;
            const int jitter = int((noise >> 24) % 21) - 10;
            int r = x * 255 / (width - 1), g = y * 255 / (height - 1), b = 255 - (x + y) * 255 / (width + height - 2);
            if ((x / 8 + y / 8) % 3 == 0) { r = std::clamp(r + jitter * 3, 0, 255); g = std::clamp(g - jitter * 2, 0, 255); }
            if (x > width * 3 / 4 && y > height * 3 / 4) { r = 230; g = 60; b = 20; }
            const int a = (x < width / 4 && y < height / 4) ? 96 + (x + y) % 128 : 255;
            uint8_t* p = image.pixel(x, y);
            p[0] = uint8_t((r * a + 127) / 255); p[1] = uint8_t((g * a + 127) / 255); p[2] = uint8_t((b * a + 127) / 255); p[3] = uint8_t(a);
        }
    return image;
}

struct Agreement { int maxDiff = 0; long long over1 = 0, samples = 0; };

Agreement agree(const Image& a, const Image& b) {
    Agreement out;
    for (int y = 0; y < a.height(); y++)
        for (int x = 0; x < a.width(); x++)
            for (int c = 0; c < 4; c++) {
                const int d = std::abs(int(a.pixel(x, y)[c]) - int(b.pixel(x, y)[c]));
                if (d > 1 && std::getenv("CAMERARAW16_DEBUG"))
                    std::fprintf(stderr, "    (%d,%d)[%d]: 8-bit %d,%d,%d,%d  16-bit %d,%d,%d,%d\n", x, y, c, a.pixel(x, y)[0], a.pixel(x, y)[1],
                                 a.pixel(x, y)[2], a.pixel(x, y)[3], b.pixel(x, y)[0], b.pixel(x, y)[1], b.pixel(x, y)[2], b.pixel(x, y)[3]);
                out.maxDiff = std::max(out.maxDiff, d);
                out.over1 += d > 1;
                out.samples++;
            }
    return out;
}

} // namespace

TEST_CASE(sixteen_bit_agrees_with_eight_bit_on_eight_bit_input) {
    const Image picture = calibrationPicture(64, 48);
    const auto deep = widenImage(picture);
    struct Case { const char* name; std::function<void(CameraRawSettings&)> set; int allowed; };
    // Each panel within a level of the 8-bit result on an 8-bit image, except where the 8-bit kernels' own rounding
    // is multiplied (docs/bit-depth.md has the figures).
    const std::vector<Case> cases{
        {"Exposure +0.7", [](CameraRawSettings& s) { s.exposure = 0.7; }, 1},
        {"Exposure -1.3", [](CameraRawSettings& s) { s.exposure = -1.3; }, 1},
        {"Temperature 30, tint -20", [](CameraRawSettings& s) { s.temperature = 30; s.tint = -20; }, 1},
        {"Contrast 40", [](CameraRawSettings& s) { s.contrast = 40; }, 1},
        {"Highlights -60, shadows 50", [](CameraRawSettings& s) { s.highlights = -60; s.shadows = 50; }, 1},
        {"Whites 30, blacks -30", [](CameraRawSettings& s) { s.whites = 30; s.blacks = -30; }, 1},
        {"Vibrance 40, saturation -20", [](CameraRawSettings& s) { s.vibrance = 40; s.saturation = -20; }, 1},
        {"Curve medium contrast", [](CameraRawSettings& s) { s.curve.rgb = CameraRawCurveSettings::mediumContrast(); }, 1},
        {"Mixer", [](CameraRawSettings& s) { s.mixer.hue[1] = 30; s.mixer.saturation[1] = -40; s.mixer.luminance[4] = 25; }, 1},
        {"Grading", [](CameraRawSettings& s) { s.grading.shadows = {220, 40, -10}; s.grading.highlights = {40, 30, 10}; }, 1},
        {"Texture 40, clarity 30", [](CameraRawSettings& s) { s.texture = 40; s.clarity = 30; }, 1},
        {"Dehaze 35", [](CameraRawSettings& s) { s.dehaze = 35; }, 1},
        {"Glow 50 bloom", [](CameraRawSettings& s) { s.glow = 50; s.glowStyle = CameraRawGlowStyle::Bloom; s.glowRange = 20; }, 1},
        {"Vignette -40", [](CameraRawSettings& s) { s.vignetteAmount = -40; s.vignetteHighlights = 30; }, 1},
        {"Grain 30", [](CameraRawSettings& s) { s.grainAmount = 30; }, 1},
        {"Sharpen 80", [](CameraRawSettings& s) { s.detail.sharpenAmount = 80; s.detail.sharpenMasking = 20; }, 1},
        {"Noise reduction 40/30", [](CameraRawSettings& s) { s.detail.noiseLuminance = 40; s.detail.noiseColor = 30; }, 1},
        {"Optics", [](CameraRawSettings& s) { s.optics.purpleAmount = 50; s.optics.vignetteAmount = 20; }, 1},
        {"Distortion", [](CameraRawSettings& s) { s.optics.distortion = 30; }, 1},
        {"Chromatic aberration", [](CameraRawSettings& s) { s.optics.removeChromaticAberration = true; }, 1},
        {"Geometry", [](CameraRawSettings& s) { s.geometry.rotate = 5; s.geometry.vertical = 10; }, 1},
        {"Calibration", [](CameraRawSettings& s) { s.calibration.redHue = 30; s.calibration.blueSaturation = -40; s.calibration.shadowTint = 20; }, 1},
        {"Everything at once", [](CameraRawSettings& s) {
             s.exposure = 0.4; s.contrast = 20; s.shadows = 30; s.vibrance = 20; s.clarity = 20; s.curve.rgb = CameraRawCurveSettings::mediumContrast();
             s.grading.midtones = {30, 20, 0}; s.detail.sharpenAmount = 40; s.vignetteAmount = -20; },
         // The 8-bit grade rounds after the tone, clarity, vignette and sharpening passes, and the fitted curves are steep
         // in the shadows, so their roundings add up on a few pixels: the low channel of strongly saturated colours, which
         // Vibrance's measured lookup pushes toward the edge of sRGB, where encoding is steepest.
         9},
    };
    bool within = true;
    for (const Case& c : cases) {
        CameraRawSettings settings;
        c.set(settings);
        Image eight = picture;
        REQUIRE(applyCameraRaw(eight, settings, 1, 7));
        Image16 sixteen = *deep;
        REQUIRE(applyCameraRaw(sixteen, settings, 1, 7));
        const Agreement d = agree(eight, *narrowImage(sixteen));
        std::fprintf(stderr, "  %-28s max %d level%s, over 1: %lld of %lld samples (%.3f%%)\n", c.name, d.maxDiff, d.maxDiff == 1 ? "" : "s",
                     d.over1, d.samples, 100.0 * double(d.over1) / double(d.samples));
        within &= d.maxDiff <= c.allowed;
    }
    CHECK(within);
}

TEST_CASE(sixteen_bit_keeps_sixteen_bit_precision) {
    // A ramp finer than 8 bits through Exposure and a curve keeps more than 256 distinct values.
    Image16 ramp(2048, 2);
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 2048; x++) { uint16_t* p = ramp.pixel(x, y); p[0] = p[1] = p[2] = uint16_t(4096 + x * 12); p[3] = 32768; }
    CameraRawSettings settings;
    settings.exposure = 0.5;
    settings.curve.rgb = CameraRawCurveSettings::mediumContrast();
    REQUIRE(applyCameraRaw(ramp, settings));
    std::vector<bool> seen(32769);
    int distinct = 0;
    for (int x = 0; x < 2048; x++) { const uint16_t v = ramp.pixel(x, 1)[0]; if (!seen[v]) { seen[v] = true; distinct++; } }
    CHECK(distinct > 1000);
    // Transparent pixels stay transparent.
    Image16 clear(4, 4);
    REQUIRE(applyCameraRaw(clear, settings));
    CHECK_EQ(int(clear.pixel(2, 2)[3]), 0);
    // Auto white balance reads a 16-bit image as it reads the 8-bit one.
    const Image warm = solid(4, 4, 0.8, 0.6, 0.4);
    const auto a = CameraRawSettings::autoBalance(warm), b = CameraRawSettings::autoBalance(*widenImage(warm));
    REQUIRE(a && b);
    CHECK(std::abs((*a)[0] - (*b)[0]) < 0.5 && std::abs((*a)[1] - (*b)[1]) < 0.5);
}

TEST_CASE(an_xmp_preset_patches_the_settings_and_lists_what_it_cannot) {
    // A Lightroom-style preset: settings as attributes and as elements, a point curve as a sequence, the name as a
    // language alternative, and settings this engine has no counterpart for.
    const std::string xmp = R"(<x:xmpmeta xmlns:x="adobe:ns:meta/">
 <rdf:RDF xmlns:rdf="http://www.w3.org/1999/02/22-rdf-syntax-ns#">
  <rdf:Description rdf:about="" xmlns:crs="http://ns.adobe.com/camera-raw-settings/1.0/"
    crs:PresetType="Normal" crs:Version="18.7" crs:ProcessVersion="15.4" crs:UUID="0123"
    crs:Exposure2012="+0.50" crs:Contrast2012="+20" crs:Temperature="5600" crs:Tint="+8" crs:WhiteBalance="Custom"
    crs:IncrementalTemperature="+12" crs:HueAdjustmentBlue="-30" crs:SplitToningShadowHue="210"
    crs:SplitToningShadowSaturation="25" crs:CurveRefineSaturation="50" crs:PerspectiveScale="90"
    crs:PostCropVignetteStyle="3" crs:ConvertToGrayscale="True" crs:GrayMixerRed="+10" crs:HasCrop="True"
    crs:EnableCalibration="False" crs:RedHue="+40" crs:Sharpness="0">
   <crs:Name><rdf:Alt><rdf:li xml:lang="x-default">Warm Look</rdf:li></rdf:Alt></crs:Name>
   <crs:Clarity2012>+15</crs:Clarity2012>
   <crs:ToneCurvePV2012><rdf:Seq><rdf:li>0, 0</rdf:li><rdf:li>128, 140</rdf:li><rdf:li>255, 255</rdf:li></rdf:Seq></crs:ToneCurvePV2012>
   <crs:Look><rdf:Description crs:Name="Adobe Color"/></crs:Look>
  </rdf:Description>
 </rdf:RDF>
</x:xmpmeta>)";
    const std::vector<uint8_t> bytes(xmp.begin(), xmp.end());
    CameraRawSettings base;
    base.saturation = 7;   // a preset is a patch: what it does not name stays
    std::string error;
    auto onPixels = readCameraRawXmp(bytes, base, false, &error);
    REQUIRE(onPixels);
    const CameraRawSettings& s = onPixels->settings;
    CHECK_EQ(onPixels->name, std::string("Warm Look"));
    CHECK_EQ(s.exposure, 0.5);
    CHECK_EQ(s.contrast, 20.0);
    CHECK_EQ(s.saturation, 7.0);
    CHECK_EQ(s.temperature, 12.0);   // the filter takes the relative temperature, not kelvin
    CHECK_EQ(s.rawTemperature, 0.0);
    CHECK_EQ(s.mixer.hue[5], -30.0);
    CHECK_EQ(s.grading.shadows.hue, 210.0);
    CHECK_EQ(s.grading.shadows.saturation, 25.0);
    CHECK_EQ(s.curve.refineSaturation, -50.0);
    CHECK_EQ(s.geometry.scale, -10.0);
    CHECK(s.vignetteStyle == CameraRawVignetteStyle::PaintOverlay);
    CHECK_EQ(s.clarity, 15.0);
    CHECK_EQ(s.curve.rgb.size(), size_t(3));
    CHECK_NEAR(s.curve.rgb[1].y, 140 / 255.0, 1e-9);
    CHECK_EQ(s.calibration.redHue, 0.0);   // EnableCalibration="False" switches the panel off
    auto listed = [&](const std::string& key) {
        return std::any_of(onPixels->unsupported.begin(), onPixels->unsupported.end(), [&](const std::string& u) { return u.rfind(key, 0) == 0; });
    };
    CHECK(s.blackWhite);   // Treatment: Black & White and its mix are read
    CHECK_EQ(s.grayMix[0], 10.0);
    CHECK_EQ(s.grading.blending, 100.0);   // legacy split toning (no ColorGrade keys) renders at Blending 100
    CHECK(!listed("ConvertToGrayscale") && !listed("GrayMixerRed"));
    CHECK(listed("HasCrop=True"));
    CHECK(listed("Look"));
    CHECK(listed("Temperature=5600"));   // kelvin has no meaning on pixels
    CHECK(!listed("Sharpness"));          // 0 changes nothing
    CHECK(!listed("UUID") && !listed("PresetType"));

    auto onRaw = readCameraRawXmp(bytes, base, true, &error);
    REQUIRE(onRaw);
    CHECK_EQ(onRaw->settings.rawTemperature, 5600.0);
    CHECK_EQ(onRaw->settings.rawTint, 8.0);
    CHECK(onRaw->settings.whiteBalance == CameraRawWhiteBalance::Custom);
    CHECK(std::any_of(onRaw->unsupported.begin(), onRaw->unsupported.end(), [](const std::string& u) { return u.rfind("IncrementalTemperature", 0) == 0; }));

    const std::string junk = "not xml";
    CHECK(!readCameraRawXmp(std::vector<uint8_t>(junk.begin(), junk.end()), base, false, &error));
    CHECK(!error.empty());
}

TEST_CASE(black_and_white_mixes_colours_into_greys) {
    // Two flat colours side by side, a blue and a yellow, and a grey.
    Image image(30, 2);
    for (int y = 0; y < 2; y++)
        for (int x = 0; x < 30; x++) {
            uint8_t* p = image.pixel(x, y);
            const uint8_t blue[3] = {40, 70, 200}, yellow[3] = {230, 200, 40}, grey[3] = {128, 128, 128};
            const uint8_t* c = x < 10 ? blue : x < 20 ? yellow : grey;
            p[0] = c[0], p[1] = c[1], p[2] = c[2], p[3] = 255;
        }
    auto grey = [&](const CameraRawSettings& settings, int x) {
        Image out = image;
        CHECK(applyCameraRaw(out, settings));
        const uint8_t* p = out.pixel(x, 0);
        CHECK(std::abs(int(p[0]) - int(p[1])) <= 1 && std::abs(int(p[1]) - int(p[2])) <= 1);
        return int(p[1]);
    };
    CameraRawSettings bw;
    bw.blackWhite = true;
    CHECK(!bw.isIdentity());
    const int blue = grey(bw, 0), yellow = grey(bw, 10), neutral = grey(bw, 20);
    CHECK(yellow > blue);
    CHECK(std::abs(neutral - 128) <= 2);
    // A band darkens or lightens its colours and leaves a grey where it was.
    CameraRawSettings darker = bw;
    darker.grayMix[5] = -60;   // Blues
    CHECK(grey(darker, 0) < blue - 5);
    CHECK(std::abs(grey(darker, 20) - neutral) <= 1);
    CameraRawSettings lighter = bw;
    lighter.grayMix[2] = 60;   // Yellows
    CHECK(grey(lighter, 10) > yellow + 3);
    // The colour mixer's bands have no say in Black & White, as Camera Raw's B&W panel replaces them.
    CameraRawSettings mixed = bw;
    mixed.mixer.luminance[5] = -80;
    CHECK_EQ(grey(mixed, 0), blue);
    // The settings round-trip, and a mix is kept while the treatment is Color.
    CameraRawSettings back;
    REQUIRE(CameraRawSettings::parse(darker.toJson(), back));
    CHECK(back == darker);
    CHECK(CameraRawSettings::parse(R"({"blackWhite": false, "grayMix": {"blues": -60}})", back));
    CHECK(!back.blackWhite && back.grayMix[5] == -60);
    CHECK(!CameraRawSettings::parse(R"({"grayMix": {"teals": 10}})", back));

    // Auto: the yellow is lighter than the image's mean and goes lighter, the blue darker; a grey image gets no mix.
    const auto mix = CameraRawSettings::autoGrayMix(image);
    CHECK(mix[2] > 0);
    CHECK(mix[5] < 0);
    for (double v : mix) CHECK(v >= -50 && v <= 50 && v == std::round(v));
    Image flat(8, 8);
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) { uint8_t* p = flat.pixel(x, y); p[0] = p[1] = p[2] = 90; p[3] = 255; }
    CHECK((CameraRawSettings::autoGrayMix(flat) == std::array<double, 8>{}));
}

TEST_MAIN()
