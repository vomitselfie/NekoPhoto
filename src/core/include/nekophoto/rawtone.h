// The Camera Raw look: the default tone curve a RAW file develops through, and the responses of the Camera Raw panels
// (Exposure, the Basic panel, the Tone Curve, the colour mixer, Colour Grading, Calibration and the vignettes), fitted to
// Camera Raw 18.7's renders of RAWmakase's synthetic chart (tools/camera_raw_fit.py, tests/camera_raw_parity;
// docs/camera-raw.md#fitted-responses).
//
// A RAW file develops (raw.h) from LibRaw's linear ProPhoto RGB: the default curve takes the brightest and the darkest
// channel from scene light to display light and the middle one keeps its place between them, so a colour keeps its hue;
// then the colour goes to sRGB. The Camera Raw grade (cameraraw.h) undoes that on its input to apply Exposure in scene
// light, and runs the other tone controls on ProPhoto RGB with the sRGB transfer, the same way. The filter on an ordinary
// layer treats its pixels as such a develop, so a RAW opened with settings equals one opened plain and filtered with them.
#pragma once
#include <array>
#include <cstdint>
#include <vector>

namespace nekophoto {

struct CameraRawSettings;
struct CameraRawCurveSettings;

namespace rawtone {

using Rgb = std::array<double, 3>;

/// Linear sRGB to linear ProPhoto RGB (D65 to D50 adapted, LibRaw's own output matrix) and back.
Rgb proPhotoFromSrgb(const Rgb& linearSrgb);
Rgb srgbFromProPhoto(const Rgb& linearProPhoto);

/// The default tone curve: scene light (1 = the raw white point once white-balanced) to display light, 0..1.
double toneCurve(double scene);
/// Its inverse on 0..1.
double inverseToneCurve(double display);

/// What File > Open makes of LibRaw's linear ProPhoto RGB: encoded sRGB, 0..1.
Rgb develop(const Rgb& sceneProPhoto);
/// The scene light an encoded sRGB colour develops from (exact up to clipping).
Rgb undevelop(const Rgb& encodedSrgb);

/// The develop as lookup tables, for whole images: `display` maps LibRaw's 16-bit linear values, scaled by `scale` (a DNG's
/// BaselineExposure), to display light (the default curve); `encode` maps linear sRGB (indexed by its square root, 4096
/// steps) to encoded sRGB.
struct Developer {
    std::vector<float> display;
    std::vector<float> encode;
    explicit Developer(double scale = 1);
    /// One pixel of LibRaw's 16-bit ProPhoto output to encoded sRGB, 0..1.
    void develop(const uint16_t in[3], float out[3]) const;
    static const Developer& shared();
};

/// Scales the scene light a colour (linear ProPhoto RGB in display light, 0..1) develops from by `gain` and develops it
/// again, keeping its hue: Exposure for one pixel.
void rescaleScene(double proPhotoDisplay[3], double gain);

/// The post-crop vignette (Effects): the share of its strength at (`x`, `y`) of a `width` x `height` frame, from Midpoint,
/// Feather and Roundness (Camera Raw's ranges), fitted to the corpus's vignette cases.
double vignetteMask(double x, double y, double width, double height, double midpoint, double feather, double roundness);
/// The vignette on one encoded sRGB colour (0..1) at mask `mask`: Paint Overlay darkens or lightens the encoded value;
/// Highlight and Color Priority darken in scene light, sparing bright colours by Highlights (0..100), and lighten like
/// a paint.
void vignette(float rgb[3], double mask, double amount, int style, double highlights);

/// Lens vignetting (Optics' Vignetting Amount and Midpoint, Camera Raw's ranges): the gain in display light at (`x`, `y`).
double lensVignetteGain(double x, double y, double width, double height, double amount, double midpoint);

/// The Tone Curve's parametric curve alone (regions and splits) on encoded values: 4097 entries over 0..1.
std::vector<float> parametricTable(const CameraRawCurveSettings& curve);

/// The Camera Raw grade's tone and colour stage, made once per settings and applied per pixel to encoded sRGB (0..1,
/// unpremultiplied): the relative white balance gains, Camera Calibration, Exposure, the Basic panel's tone sliders (Whites, Blacks,
/// Highlights, Shadows, Dehaze's tone, Contrast), the parametric curve, the point curves with Refine Saturation,
/// Vibrance and Saturation, the colour mixer and Colour Grading.
class ToneStage {
public:
    explicit ToneStage(const CameraRawSettings& normalized);
    /// Whether the stage changes nothing.
    bool identity() const { return identity_; }
    void apply(float rgb[3]) const;

private:
    bool identity_ = true;
    std::array<double, 3> gains_{1, 1, 1};
    /// Camera Calibration: the primaries' matrix on scene light, and Shadow Tint (-1..1).
    bool calibrated_ = false;
    std::array<std::array<double, 3>, 3> calibration_{};
    double shadowTint_ = 0;
    double exposureScale_ = 1, shoulder_ = 0;
    bool gained_ = false, exposed_ = false, toned_ = false, curved_ = false, channelCurved_ = false;
    double refine_ = 0;            // -1..0: how much of the master curve's saturation change is undone
    double saturation_ = 0, vibrance_ = 0;
    std::vector<float> exposure_;  // display light (by square root) -> display light
    /// The Basic panel's sliders and the parametric curve, in order: encoded -> encoded tables, each applied to the
    /// brightest and darkest channel, or by `luminance` (0..1) to the colour's luminance instead, as the measured
    /// slider does.
    struct Step {
        std::vector<float> table;
        double luminance = 0;
    };
    std::vector<Step> steps_;
    std::vector<float> master_;    // encoded -> encoded, the RGB point curve
    std::array<std::vector<float>, 3> channel_;
    /// The colour mixer: each band's hue (degrees), saturation (factor) and lightness change at these settings.
    bool mixed_ = false;
    std::array<std::array<double, 8>, 3> mixer_{};
    /// Colour Grading: each wheel's push (the direction of its hue times its saturation) and luminance, with Blending and
    /// Balance.
    bool graded_ = false;
    struct Wheel {
        std::array<double, 3> push{};
        double saturation = 0, luminance = 0;
    };
    std::array<Wheel, 4> wheels_{};   // shadows, midtones, highlights, global
    double blending_ = 50, balance_ = 0;
};

} // namespace rawtone
} // namespace nekophoto
