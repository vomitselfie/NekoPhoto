// Filter > Camera Raw Filter on an already-rendered layer: white balance, tone, presence, curve, color
// mixer, color grading, detail, optics, geometry, effects and calibration, applied destructively.
// A port of upstream Compositor (MIT, see LICENSES/MIT-Compositor.txt): Compositor/Document/CameraRaw.swift,
// CameraRawColor.swift, CameraRawDetailOptics.swift and CameraRawGeometryCalibration.swift. The pixel
// kernels are src/pixels/CameraRawPixels.c. Camera RAW file decoding (RawImporter.swift) is not part of it.
#pragma once
#include "adjustments.h"
#include "image.h"
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace compositor {

enum class CameraRawWhiteBalance { Custom, Auto };
/// Diffusion and Bloom are tinted cool to warm by Warmth; Halation's fringe stays red.
enum class CameraRawGlowStyle { Diffusion, Bloom, Halation };
/// Highlight Priority is the style whose Highlights slider protects bright pixels.
enum class CameraRawVignetteStyle { HighlightPriority, ColorPriority, PaintOverlay };
enum class CameraRawUprightMode { Off, Guided };
enum class CameraRawProjection { Perspective, Rectilinear };
/// Preview-only view that replaces the grade (upstream shows it while Option is held on a Light slider).
enum class CameraRawClipping { None = 0, Highlights = 1, Shadows = 2 };

const char* cameraRawName(CameraRawWhiteBalance value);
const char* cameraRawName(CameraRawGlowStyle value);
const char* cameraRawName(CameraRawVignetteStyle value);
const char* cameraRawName(CameraRawUprightMode value);
const char* cameraRawName(CameraRawProjection value);

/// A curve point on 0…1 both axes.
struct CameraRawCurvePoint { double x = 0, y = 0; bool operator==(const CameraRawCurvePoint&) const = default; };

/// Parametric regions and point curves. Amounts are −100…100.
struct CameraRawCurveSettings {
    double shadows = 0, darks = 0, lights = 0, highlights = 0;
    /// Dividers, 0…100, kept in order: where each parametric slider hands off to the next.
    double shadowSplit = 25, darkSplit = 50, lightSplit = 75;
    std::vector<CameraRawCurvePoint> rgb = linear(), red = linear(), green = linear(), blue = linear();
    /// How much the composite curve also changes saturation, −100…100. 0 keeps it to brightness.
    double refineSaturation = 0;
    bool operator==(const CameraRawCurveSettings&) const = default;

    static std::vector<CameraRawCurvePoint> linear() { return {{0, 0}, {1, 1}}; }
    static std::vector<CameraRawCurvePoint> mediumContrast() { return {{0, 0}, {0.25, 0.18}, {0.75, 0.82}, {1, 1}}; }
    static std::vector<CameraRawCurvePoint> strongContrast() { return {{0, 0}, {0.25, 0.10}, {0.75, 0.90}, {1, 1}}; }
    static bool isLinear(const std::vector<CameraRawCurvePoint>& points);

    bool adjusts() const;
    /// Lifts or lowers the region a tone (0…1) belongs to.
    double parametric(double tone) const;
    /// 256 entries: the parametric curve followed by the RGB point curve.
    std::vector<float> lumaTable() const;
    std::vector<float> channelTable(const std::vector<CameraRawCurvePoint>& points) const;
    CameraRawCurveSettings normalized() const;
};

/// One picked color and how far its adjustment reaches.
struct CameraRawPointColor {
    double hue = 0;                 // degrees 0…360
    double saturation = 0, luminance = 0;   // 0…1
    double hueShift = 0, saturationShift = 0, luminanceShift = 0;   // −100…100
    double hueRange = 30;           // degrees 5…180
    double saturationRange = 0.4, luminanceRange = 0.4;             // 0.05…1
    bool operator==(const CameraRawPointColor&) const = default;
    CameraRawPointColor normalized() const;
};

/// Eight color families (Reds, Oranges, Yellows, Greens, Aquas, Blues, Purples, Magentas), each with hue,
/// saturation and luminance shifts of −100…100, and up to eight point colors.
struct CameraRawMixerSettings {
    static constexpr int familyCount = 8;
    static const std::array<const char*, 8> names;
    static const std::array<double, 8> centers;
    std::array<double, 8> hue{}, saturation{}, luminance{};
    std::vector<CameraRawPointColor> points;
    bool operator==(const CameraRawMixerSettings&) const = default;
    bool adjusts() const;
    /// How much each family shares a hue in degrees. Neighbours overlap.
    static std::array<double, 8> weights(double degrees);
    CameraRawMixerSettings normalized() const;
};

struct CameraRawGradeWheel {
    double hue = 0;         // 0…360
    double saturation = 0;  // 0…100
    double luminance = 0;   // −100…100
    bool operator==(const CameraRawGradeWheel&) const = default;
    CameraRawGradeWheel normalized() const;
};

/// Four color wheels plus how the three tonal wheels overlap and which end they favour.
struct CameraRawGradingSettings {
    CameraRawGradeWheel shadows, midtones, highlights, global;
    double blending = 50;   // 0…100: higher lets the tonal wheels overlap more
    double balance = 0;     // −100…100: negative favours shadows, positive highlights
    bool operator==(const CameraRawGradingSettings&) const = default;
    bool adjusts() const;
    CameraRawGradingSettings normalized() const;
};

/// Sharpening and manual noise reduction. Amount is 0…150; the rest 0…100.
struct CameraRawDetailSettings {
    double sharpenAmount = 0, sharpenRadius = 10, sharpenDetail = 25, sharpenMasking = 0;
    double noiseLuminance = 0, noiseLuminanceDetail = 50, noiseLuminanceContrast = 0;
    double noiseColor = 0, noiseColorDetail = 50, noiseColorSmoothness = 50;
    bool operator==(const CameraRawDetailSettings&) const = default;
    bool adjusts() const { return sharpenAmount != 0 || noiseLuminance != 0 || noiseColor != 0; }
    CameraRawDetailSettings normalized() const;
};

/// Lens profile toggles, manual distortion, defringe and lens-vignetting correction. A rendered layer has no
/// profile metadata, so the profile sliders only scale a generic correction.
struct CameraRawOpticsSettings {
    bool removeChromaticAberration = false, enableLensProfile = false;
    double profileDistortion = 100, profileVignetting = 100;   // 0…100
    double distortion = 0;                                     // −100…100, Lens Correction's sign
    double purpleAmount = 0, purpleHueLow = 270, purpleHueHigh = 310;
    double greenAmount = 0, greenHueLow = 60, greenHueHigh = 120;
    double vignetteAmount = 0, vignetteMidpoint = 50;
    bool operator==(const CameraRawOpticsSettings&) const = default;
    bool adjusts() const;
    CameraRawOpticsSettings normalized() const;
    /// The radial `k` for lens_distort, on the Lens Correction filter's scale.
    double distortionK(double profileStrength) const;
};

/// A guide line in normalized image coordinates, 0…1 measured from the lower-left of the pixel grid (as upstream).
struct CameraRawGeometryGuide {
    double startX = 0, startY = 0, endX = 0, endY = 0;
    bool operator==(const CameraRawGeometryGuide&) const = default;
};

struct CameraRawGeometrySettings {
    CameraRawUprightMode upright = CameraRawUprightMode::Off;
    CameraRawProjection projection = CameraRawProjection::Perspective;
    double vertical = 0, horizontal = 0;   // −100…100
    double rotate = 0;                     // −45…45 degrees
    double aspect = 0, scale = 0, offsetX = 0, offsetY = 0;   // −100…100
    bool constrainCrop = false;
    std::vector<CameraRawGeometryGuide> guides;
    bool operator==(const CameraRawGeometrySettings&) const = default;
    bool adjusts() const;
    CameraRawGeometrySettings normalized() const;
    /// Where the image's corners go, in pixel coordinates (y down), in the order top-left, top-right,
    /// bottom-right, bottom-left. Upstream hands the same corners (y up) to Core Image's perspective transform.
    std::array<std::array<double, 2>, 4> outputCorners(int width, int height) const;
    /// The perspective and affine warp on the pixel grid; the output keeps the input's size. Transparent past
    /// the warped edges unless Constrain Crop scales the covered part back up to fill the frame.
    Image apply(const Image& image) const;
};

struct CameraRawCalibrationSettings {
    int process = 6;   // process version 1…6; older versions apply the sliders more weakly
    double shadowTint = 0, redHue = 0, redSaturation = 0, greenHue = 0, greenSaturation = 0, blueHue = 0, blueSaturation = 0;
    bool operator==(const CameraRawCalibrationSettings&) const = default;
    bool adjusts() const;
    CameraRawCalibrationSettings normalized() const;
};

/// Which panels take part. A panel switched off contributes nothing (upstream's eye buttons).
struct CameraRawPanels {
    bool light = true, color = true, effects = true, curve = true, mixer = true, grading = true,
         detail = true, optics = true, geometry = true, calibration = true;
};

/// Camera Raw Filter settings. Defaults leave the image unchanged.
struct CameraRawSettings {
    /// Share of a full warm/cool swing applied to red and blue.
    static constexpr double temperatureGain = 0.35;
    /// Magenta/green swing shared by red and blue, and on green (opposite).
    static constexpr double tintRedBlue = 0.15, tintGreen = 0.30;

    CameraRawWhiteBalance whiteBalance = CameraRawWhiteBalance::Custom;
    double temperature = 0, tint = 0;   // −100…100, relative (not kelvin); positive is warmer / more magenta
    double exposure = 0;                // stops, −5…5
    double contrast = 0, highlights = 0, shadows = 0, whites = 0, blacks = 0;
    double vibrance = 0, saturation = 0;
    double texture = 0, clarity = 0, dehaze = 0;
    double glow = 0;   // 0…100; range, spread and warmth are idle at zero
    CameraRawGlowStyle glowStyle = CameraRawGlowStyle::Diffusion;
    double glowRange = 0, glowSpread = 0, glowWarmth = 0;
    double vignetteAmount = 0;
    CameraRawVignetteStyle vignetteStyle = CameraRawVignetteStyle::HighlightPriority;
    double vignetteMidpoint = 50, vignetteRoundness = 0, vignetteFeather = 50, vignetteHighlights = 0;
    double grainAmount = 0, grainSize = 25, grainRoughness = 50;
    CameraRawCurveSettings curve;
    CameraRawMixerSettings mixer;
    CameraRawGradingSettings grading;
    CameraRawDetailSettings detail;
    CameraRawOpticsSettings optics;
    CameraRawGeometrySettings geometry;
    CameraRawCalibrationSettings calibration;
    bool operator==(const CameraRawSettings&) const = default;

    bool adjustsLight() const { return exposure != 0 || contrast != 0 || highlights != 0 || shadows != 0 || whites != 0 || blacks != 0; }
    bool adjustsColor() const { return temperature != 0 || tint != 0 || vibrance != 0 || saturation != 0; }
    bool adjustsEffects() const { return texture != 0 || clarity != 0 || dehaze != 0 || glow != 0 || vignetteAmount != 0 || grainAmount != 0; }
    bool isIdentity() const;
    /// Every scalar finite and within its range.
    bool isValid() const;
    /// Clamped into range; a value that is not finite takes its default.
    CameraRawSettings normalized() const;
    /// The grade with the panels switched off reset to their defaults.
    CameraRawSettings applying(const CameraRawPanels& panels) const;
    /// Camera Raw's 0…100 grain size in adjust_grain's pixel scale.
    double grainKernelSize() const { return 0.5 + (grainSize / 100) * 19.5; }
    /// Channel multipliers for temperature and tint; neutral is 1, 1, 1.
    std::array<double, 3> gains() const;

    /// Temperature and tint that bring one linear-light colour to neutral with the gains above. Empty when a
    /// channel is missing or the cast cannot be expressed on those two axes.
    static std::optional<std::array<double, 2>> neutralize(double linearRed, double linearGreen, double linearBlue);
    static std::optional<std::array<double, 2>> neutralizeStraight(double red, double green, double blue);
    /// Gray-world balance of the covered pixels (White Balance > Auto). Empty for an image with no coverage.
    static std::optional<std::array<double, 2>> autoBalance(const Image& image);

    /// JSON with the same keys as the fields above (nested objects for curve, mixer, grading, detail, optics,
    /// geometry, calibration; enums by their names such as "Halation").
    std::string toJson() const;
    /// Reads keys over `out` (so a partial object patches it). Unknown keys and wrong types fail with `error`.
    static bool parse(const std::string& text, CameraRawSettings& out, std::string* error = nullptr);
};

/// What the preview may show instead of, or on top of, the grade. None of it is ever committed.
struct CameraRawPreview {
    CameraRawClipping clipping = CameraRawClipping::None;
    bool shadowClipIndicator = false, highlightClipIndicator = false;
    bool sharpenMask = false;
    int visualizePointColor = -1;
};

/// Applies the Camera Raw grade to premultiplied RGBA in place, in upstream's order: geometry, calibration,
/// light and color, curve / mixer / grading, effects and grain, optics, detail. `scale` is pixels in `image` per
/// layer pixel (a reduced preview scales the radii); `seed` fixes the grain. Returns false (image untouched) for
/// settings that do not validate once normalized.
bool applyCameraRaw(Image& image, const CameraRawSettings& settings, double scale = 1, uint32_t seed = 0,
                    const CameraRawPreview& preview = {});

} // namespace compositor
