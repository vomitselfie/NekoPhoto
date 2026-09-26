// Camera Raw Filter pixel kernels, ported from upstream Compositor's Compositor/Rendering/AdjustPixels.h
// (MIT, see LICENSES/MIT-Compositor.txt). Premultiplied RGBA (4 bytes per pixel, `stride` bytes per row);
// alpha is kept and fully transparent pixels are left alone.
#ifndef CameraRawPixels_h
#define CameraRawPixels_h
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

// Camera Raw's Light and Color groups, in this order: white balance (the three channel gains), exposure in
// stops of linear light, contrast about mid gray, highlights, shadows, whites, blacks, vibrance, then
// saturation. Temperature and tint are relative, so the gains are computed by the caller. Amounts are Camera
// Raw's own ranges (exposure −5…5, the rest −100…100). `clipping` 0 renders the grade; 1 replaces it with a
// highlight-clip view (clipped channels lit on black); 2 with a shadow-clip view (clipped channels dark on white).
void adjust_camera_raw(uint8_t *rgba, size_t width, size_t height, size_t stride,
                       double redGain, double greenGain, double blueGain, double exposure, double contrast,
                       double highlights, double shadows, double whites, double blacks,
                       double vibrance, double saturation, int clipping);
// Blue over clipped shadows and red over clipped highlights, on top of the grade. Preview only.
void adjust_camera_raw_clip_overlay(uint8_t *rgba, size_t width, size_t height, size_t stride, int shadows, int highlights);
// Curve, Color Mixer, and Color Grading after the basic grade. `lumaLut` and the channel LUTs are 256
// entries. `mixer` is 24 floats: hue, saturation, luminance for eight families, −1…1. Each point color is
// 9 floats (hue, saturation, luminance, three shifts −1…1, three range half-widths). `grade` is four wheels
// of hue turns, saturation 0…1, and luminance −1…1. `visualize` darkens pixels outside that point color.
void adjust_camera_raw_curve_color(uint8_t *rgba, size_t width, size_t height, size_t stride,
                                   const float *lumaLut, const float *redLut, const float *greenLut, const float *blueLut,
                                   double refineSaturation, const float *mixer, int pointCount, const float *points,
                                   const float *grade, double blending, double balance, int visualize);
// Camera Raw Effects after Light and Color. Texture is a fine local contrast, Clarity a broader one.
// Dehaze raises contrast and saturation when positive and lifts the shadows when negative. Glow, its
// range, spread and warmth do nothing until `glow` is above zero: styles are 0 diffusion, 1 bloom,
// 2 halation. Vignette styles are 0 highlight priority, 1 color priority, 2 paint overlay; Highlights
// protects bright pixels only while the amount darkens. `scale` is preview pixels per layer pixel, so
// the radii match a full-size render. Grain is applied separately (adjust_grain).
void adjust_camera_raw_effects(uint8_t *rgba, size_t width, size_t height, size_t stride,
                               double texture, double clarity, double dehaze,
                               double glow, int glowStyle, double glowRange, double glowSpread, double glowWarmth,
                               double vignetteAmount, double vignetteMidpoint, double vignetteRoundness,
                               double vignetteFeather, double vignetteHighlights, int vignetteStyle,
                               double scale);
// Manual noise reduction, then sharpening. `scale` maps radius to preview pixels. Applied after the creative grade.
void adjust_camera_raw_detail(uint8_t *rgba, size_t width, size_t height, size_t stride,
                              double sharpenAmount, double sharpenRadius, double sharpenDetail, double sharpenMasking,
                              double noiseLuminance, double noiseLuminanceDetail, double noiseLuminanceContrast,
                              double noiseColor, double noiseColorDetail, double noiseColorSmoothness, double scale);
// Preview only: white where sharpening would land, black where masking protects. Uses the current sharpen sliders.
void adjust_camera_raw_sharpen_mask_overlay(uint8_t *rgba, size_t width, size_t height, size_t stride,
                                            double sharpenRadius, double sharpenDetail, double sharpenMasking, double scale);
// Chromatic aberration, lens distortion, defringe, and lens-vignetting correction. `distortionK` matches `lens_distort`.
// `profileDistortion` and `scale` are accepted for the upstream signature; the distortion is already folded into
// `distortionK` and no step here has a radius.
void adjust_camera_raw_optics(uint8_t *rgba, size_t width, size_t height, size_t stride,
                              int removeChromatic, int lensProfile, double profileDistortion, double profileVignetting,
                              double distortionK, double purpleAmount, double purpleHueLow, double purpleHueHigh,
                              double greenAmount, double greenHueLow, double greenHueHigh,
                              double vignetteAmount, double vignetteMidpoint, double scale);
// Camera calibration before the main grade. Primary hue and saturation shifts are −100…100; shadow tint is green/magenta.
void adjust_camera_raw_calibration(uint8_t *rgba, size_t width, size_t height, size_t stride,
                                   double shadowTint, double redHue, double redSaturation,
                                   double greenHue, double greenSaturation, double blueHue, double blueSaturation,
                                   int processVersion);

#ifdef __cplusplus
}
#endif
#endif
