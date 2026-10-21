// Photoshop's other adjustment layers (adjustments.h): Brightness/Contrast, Posterize and Threshold from Patchy's
// calibration against Photoshop 2026 (src/core/adjustment_layer.cpp there, MIT; Posterize refitted to Photoshop's
// composite); Color Balance fitted here to Photoshop's composites; Black & White from upstream Compositor's
// AdjustPixels.c (MIT); Photo Filter after PhotoCraft's Photoshop-fitted photo_filter_matrix (MIT OR Apache-2.0,
// THIRD-PARTY-NOTICES.md), Vibrance after its fit of Photoshop's; Channel Mixer and Selective Color from their
// published formulas (docs/adjustment-layers.md).
#include "nekophoto/adjustments.h"
#include "nekophoto/depth.h"
#include "nekophoto/imaget.h"
#include "nekophoto/modeedit.h"
#include "nekophoto/parallel.h"
#include <algorithm>
#include <cmath>

namespace nekophoto {

namespace {

// Each pixel's straight colour (0..1) through `f`, premultiplied again; transparent pixels are left alone.
template <class F>
void perPixel(Image& image, F&& f) {
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            uint8_t* row = image.row(y);
            for (int x = 0; x < image.width(); x++) {
                uint8_t* p = row + x * 4;
                const int a = p[3];
                if (!a) continue;
                double c[3];
                for (int i = 0; i < 3; i++) c[i] = std::min(255, p[i] * 255 / a) / 255.0;
                f(c[0], c[1], c[2]);
                for (int i = 0; i < 3; i++) p[i] = uint8_t(std::clamp(std::lround(std::clamp(c[i], 0.0, 1.0) * a), 0L, long(a)));
            }
        }
    });
}

// The same at 16 bits: the straight colour is exact (no rounding to a level before `f`).
template <class F>
void perPixel(Image16& image, F&& f) {
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            uint16_t* row = image.row(y);
            for (int x = 0; x < image.width(); x++) {
                uint16_t* p = row + x * 4;
                const int a = p[3];
                if (!a) continue;
                const double inverse = 1.0 / a;
                double c[3];
                for (int i = 0; i < 3; i++) c[i] = std::min(1.0, p[i] * inverse);
                f(c[0], c[1], c[2]);
                for (int i = 0; i < 3; i++) p[i] = uint16_t(std::clamp(std::lround(std::clamp(c[i], 0.0, 1.0) * a), 0L, long(a)));
            }
        }
    });
}

// The same at 32 bits: the colour encoded through the document's curve (forEachEncodedColour, adjustments_f32.cpp).
struct EncodedImageF { ImageF& image; const TransferCurve& curve; };
template <class F>
void perPixel(EncodedImageF& target, F&& f) { forEachEncodedColour(target.image, target.curve, f); }

// The same on a 32-bit image's straight linear values as stored, unclamped (Photoshop's 32-bit Color Balance and Channel
// Mixer, photocraft-corpus rgb32); the results are kept at 0 and up (cleanColour).
struct LinearImageF { ImageF& image; };
template <class F>
void perPixel(LinearImageF& target, F&& f) {
    ImageF& image = target.image;
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            float* p = image.row(y);
            for (int x = 0; x < image.width(); x++, p += 4) {
                const float a = p[3];
                if (!(a > 0)) continue;
                double c[3] = {p[0] / double(a), p[1] / double(a), p[2] / double(a)};
                f(c[0], c[1], c[2]);
                for (int i = 0; i < 3; i++) p[i] = cleanColour(float(c[i] * a));
            }
        }
    });
}

// A CMYK buffer (5 samples) with its stored cyan, magenta and yellow (the complements of the inks, which is how
// Photoshop's colour adjustments see a CMYK pixel's colour) taken as red, green and blue; black is left as it is.
template <SampleType S>
struct StoredCmy { ImageT<S>& image; };
template <SampleType S, class F>
void perPixel(StoredCmy<S>& target, F&& f) {
    ImageT<S>& image = target.image;
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            SampleOf<S>* row = image.row(y);
            for (int x = 0; x < image.width(); x++) {
                SampleOf<S>* p = row + x * 5;
                const int a = p[4];
                if (!a) continue;
                const double inverse = 1.0 / a;
                double c[3];
                for (int i = 0; i < 3; i++) c[i] = std::min(1.0, p[i] * inverse);
                f(c[0], c[1], c[2]);
                for (int i = 0; i < 3; i++) p[i] = SampleOf<S>(std::clamp(std::lround(std::clamp(c[i], 0.0, 1.0) * a), 0L, long(a)));
            }
        }
    });
}

double luma(double r, double g, double b) { return 0.299 * r + 0.587 * g + 0.114 * b; }

double srgbDecode(double v) { v = std::clamp(v, 0.0, 1.0); return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }
double srgbEncode(double v) { v = std::max(0.0, v); return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1 / 2.4) - 0.055; }

// ---- Brightness/Contrast (Patchy's closed forms; see adjustment_layer.cpp there for the capture record) ----

constexpr double kGainStep = 1.006321233202252;   // 2^(1/110)

double brightnessGain(int b) { double s = 1; for (int i = 0; i < b; i++) s *= kGainStep; return s; }

double brightnessCore(int b, double v) {
    const double sigma = brightnessGain(b), rayEnd = 0.5 / sigma;
    if (v <= rayEnd) return sigma * v;
    const double h = 1 - rayEnd, t = (v - rayEnd) / h;
    const double tau = std::max(0.1, 1 / (1 + 12 * (sigma - 1)));
    const double hermite = ((2 * t - 3) * t * t + 1) * 0.5 + ((t - 2) * t + 1) * t * h * sigma + (3 - 2 * t) * t * t + (t - 1) * t * t * h * tau;
    return std::min(1.0, hermite);
}
double brightnessPositive(int b, double v) { return b <= 100 ? brightnessCore(b, v) : brightnessCore(b - 100, brightnessCore(100, v)); }
double brightnessValue(int b, double v) {
    if (b == 0) return v;
    if (b > 0) return brightnessPositive(b, v);
    if (v <= 0) return 0;
    if (v >= 1) return 1;
    double lo = 0, hi = 1;   // negative: the exact inverse, by bisection
    for (int i = 0; i < 64; i++) { const double mid = 0.5 * (lo + hi); (brightnessPositive(-b, mid) < v ? lo : hi) = mid; }
    return 0.5 * (lo + hi);
}
double contrastValue(int c, double v) {
    const double beta = 1 - 0.0076 * c, alpha = 2 - 2 * beta;
    if (v <= 0.5) return (alpha * v + beta) * v;
    const double w = 1 - v;
    return 1 - (alpha * w + beta) * w;
}

Transfer tableFrom(auto&& byte) {
    Transfer t;
    for (int i = 0; i < 256; i++) { const float v = float(byte(uint8_t(i))) / 255.0f; t[0][size_t(i)] = t[1][size_t(i)] = t[2][size_t(i)] = v; }
    return t;
}

} // namespace

BrightnessContrastSettings BrightnessContrastSettings::normalized() const {
    BrightnessContrastSettings s = *this;
    if (legacy) { s.brightness = std::clamp(brightness, -100, 100); s.contrast = std::clamp(contrast, -100, 100); }
    else { s.brightness = std::clamp(brightness, -150, 150); s.contrast = std::clamp(contrast, -50, 100); }
    return s;
}

uint8_t BrightnessContrastSettings::apply(uint8_t value) const {
    const BrightnessContrastSettings s = normalized();
    if (!s.legacy) return uint8_t(std::clamp(std::lround(255 * contrastValue(s.contrast, brightnessValue(s.brightness, value / 255.0))), 0L, 255L));
    // Legacy: positive contrast folds brightness into the input and expands about 127.5; 100 is a hard threshold;
    // negative contrast compresses first and adds brightness to the output.
    const int b = s.brightness, c = s.contrast;
    if (c == 0) return uint8_t(std::clamp(int(value) + b, 0, 255));
    if (c >= 100) return int(value) + b >= 127 ? 255 : 0;
    if (c > 0) return uint8_t(std::clamp(std::lround((value + b - 127.5) * 100.0 / (100.0 - c) + 127.5), 0L, 255L));
    return uint8_t(std::clamp(std::lround((value - 127.5) * (100.0 + c) / 100.0 + 127.5) + b, 0L, 255L));
}

Transfer invertTransfer() { return tableFrom([](uint8_t v) { return 255 - v; }); }
Transfer brightnessContrastTransfer(const BrightnessContrastSettings& s) { return tableFrom([&](uint8_t v) { return s.apply(v); }); }
Transfer posterizeTransfer(const PosterizeSettings& s) {
    // Photoshop's buckets are equal slices of the 256 levels (floor(v * n / 256)), each shown at its step of 255 / (n - 1):
    // fitted to Photoshop's own composite of photoshop-posterize.psd, exact on every sample (rounding to the nearest
    // step, as Patchy has it, misses a third of them).
    const int n = std::clamp(s.levels, 2, 255);
    return tableFrom([&](uint8_t v) { return std::clamp(std::lround((v * n / 256) * 255.0 / (n - 1)), 0L, 255L); });
}

double posterizeAt16(const PosterizeSettings& s, double x) {
    // Photoshop's 16-bit Posterize bins the 16-bit sample, floor(v * n / 32768), and lands on 8-bit levels through a
    // 16-bit step: floor(bin * 32768 / (n - 1)) converted with floor((o * 255 + 16384) / 32768), so 127.5 comes out as
    // 128 but 42.5 as 42 (PhotoCraft's posterize_in, crates/compose/src/adjust.rs at 04d0212, MIT OR Apache-2.0, used
    // under MIT; exact on psd-tools posterize_16bits_rgb.psd and posterize_16bits_grayscale.psd).
    const double n = std::clamp(s.levels, 2, 255);
    const double v = std::round(std::clamp(std::isfinite(x) ? x : 0.0, 0.0, 1.0) * 32768);
    const double bin = std::min(n - 1, std::floor(v * n / 32768));
    const double step = std::floor(bin * 32768 / (n - 1));
    return std::floor((step * 255 + 16384) / 32768) / 255;
}

void applyThreshold(Image& image, const ThresholdSettings& s) {
    const int level = std::clamp(s.level, 1, 255);
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < image.width(); x++) {
                uint8_t* p = image.pixel(x, y);
                const int a = p[3];
                if (!a) continue;
                const int r = std::min(255, p[0] * 255 / a), g = std::min(255, p[1] * 255 / a), b = std::min(255, p[2] * 255 / a);
                // Photoshop's integer luminance, at or above the level white (Patchy).
                const uint8_t v = (r * 30 + g * 59 + b * 11) / 100 >= level ? uint8_t(a) : 0;
                p[0] = p[1] = p[2] = v;
            }
    });
}

template <class I>
void applyBlackWhiteImpl(I& image, const BlackWhiteSettings& s) {
    // A colour is min of grey, plus (mid - min) of the secondary between its two brightest channels, plus (max - mid)
    // of the primary of its brightest: each weighted by its slider (upstream's adjust_black_white).
    double w[6];
    for (int i = 0; i < 6; i++) w[i] = std::clamp(s.weights[size_t(i)], -200.0, 300.0) / 100;
    const double tr = s.tintColor.red, tg = s.tintColor.green, tb = s.tintColor.blue;
    const double tintLuma = luma(tr, tg, tb);
    perPixel(image, [&](double& r, double& g, double& b) {
        const double mx = std::max({r, g, b}), mn = std::min({r, g, b}), md = r + g + b - mx - mn;
        int primary, secondary;   // 0 red, 1 yellow, 2 green, 3 cyan, 4 blue, 5 magenta
        if (mx == r) { primary = 0; secondary = g >= b ? 1 : 5; }
        else if (mx == g) { primary = 2; secondary = r >= b ? 1 : 3; }
        else { primary = 4; secondary = g >= r ? 3 : 5; }
        const double grey = std::clamp(mn + (md - mn) * w[secondary] + (mx - md) * w[primary], 0.0, 1.0);
        if (!s.tint) { r = g = b = grey; return; }
        // Tinted: the tint's colour at this grey's lightness (its luminance moved to the grey's).
        const double shift = grey - tintLuma;
        r = tr + shift; g = tg + shift; b = tb + shift;
        const double l = luma(r, g, b);
        const double lo = std::min({r, g, b}), hi = std::max({r, g, b});
        if (lo < 0 && l > lo) { const double k = l / (l - lo); r = l + (r - l) * k; g = l + (g - l) * k; b = l + (b - l) * k; }
        if (hi > 1 && hi > l) { const double k = (1 - l) / (hi - l); r = l + (r - l) * k; g = l + (g - l) * k; b = l + (b - l) * k; }
    });
}

template <class I>
void applyColorBalanceImpl(I& image, const ColorBalanceSettings& s) {
    // Photoshop's Color Balance is a Levels per channel, out = ((v - black) / (white - black))^gamma, its points and
    // gamma set by the sliders (levels of 0..255, gammas as powers of two). The rules for one slider at a time follow
    // image-colormatcher's measurements (docs/CALIBRATION.md, MIT, (c) 2026 Ariloum); how several combine was fitted
    // here to the merged image of Patchy's photoshop-color-balance-full.psd (within a level on every sample):
    // - Preserve Luminosity on: the shadows raise the black point of each channel by how far its slider sits below the
    //   highest of the three, the highlights lower the white point by how far it sits above the lowest, and the
    //   midtones set the gamma 2^-((v - (max + min) / 2) / 100). One slider towards red thus darkens green and blue in
    //   the shadows rather than lifting red.
    // - Off: each slider moves only its own channel. Shadows: a negative value raises the black point by its size;
    //   highlights: a positive one lowers the white point; and every slider bends the gamma, the midtones by
    //   2^(-v / 100), the shadows and highlights by 2^(-v / 200).
    double black[3], white[3], gamma[3];
    std::array<std::array<double, 3>, 3> v;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) v[size_t(r)][size_t(c)] = std::clamp(s.ranges[size_t(r)][size_t(c)], -100.0, 100.0);
    const auto& shadows = v[0];
    const auto& midtones = v[1];
    const auto& highlights = v[2];
    for (int c = 0; c < 3; c++) {
        const size_t i = size_t(c);
        double log2Gamma;
        if (s.preserveLuminosity) {
            black[c] = *std::max_element(shadows.begin(), shadows.end()) - shadows[i];
            white[c] = 255 - (highlights[i] - *std::min_element(highlights.begin(), highlights.end()));
            const double centre = (*std::max_element(midtones.begin(), midtones.end()) + *std::min_element(midtones.begin(), midtones.end())) / 2;
            log2Gamma = -(midtones[i] - centre) / 100;
        } else {
            black[c] = std::max(0.0, -shadows[i]);
            white[c] = 255 - std::max(0.0, highlights[i]);
            log2Gamma = -midtones[i] / 100 - (shadows[i] + highlights[i]) / 200;
        }
        black[c] /= 255;
        white[c] /= 255;
        gamma[c] = std::pow(2.0, log2Gamma);
    }
    perPixel(image, [&](double& r, double& g, double& b) {
        double* channel[3] = {&r, &g, &b};
        for (int c = 0; c < 3; c++) {
            const double x = std::clamp((*channel[c] - black[c]) / std::max(1e-6, white[c] - black[c]), 0.0, 1.0);
            *channel[c] = gamma[c] == 1 ? x : std::pow(x, gamma[c]);
        }
    });
}

/// Photoshop's Vibrance on a linear-light colour (`v` the slider / 100), after PhotoCraft's black-box fit of Photoshop 25.4
/// (`vibrance_lin`, crates/compose/src/adjust.rs at 04d0212, MIT OR Apache-2.0, used under MIT). With t = 1 - min / max
/// (HSV saturation) and V = max: lowered, t' = t (1 - a/4) (1 - a + a t (1 + t) / 2) and V' = V - a t (1 - t)
/// (2 - t + t^2) V (1 - V) with a = -v; raised, V' = V + v/4 of the same shape and a saturation boost that grows as t and
/// V fall, damped on reds and skin (HSV hue 0 to 30 degrees, fading out by 45 and in from 300).
void vibranceLinear(double c[3], double v) {
    const double mx = std::max({c[0], c[1], c[2]}), mn = std::min({c[0], c[1], c[2]});
    if (mx <= 0 || mx - mn <= 1e-9) return;
    const double t = 1 - mn / mx;
    const double shape = t * (1 - t) * (2 - t + t * t) * mx * std::max(0.0, 1 - mx);
    double t2, v2;
    if (v < 0) {
        const double a = -v;
        t2 = t * (1 - a / 4) * (1 - a + a * t * (1 + t) / 2);
        v2 = mx - a * shape;
    } else {
        static constexpr double c0 = 0.5001, c1 = 0.0887, p = 0.4374, q = 0.7573, r0 = -0.8113, ra = 0.5154, rt = 2.8026;
        const double d = mx - mn;
        double h = mx == c[0] ? std::fmod((c[1] - c[2]) / d + 6, 6.0) : mx == c[1] ? (c[2] - c[0]) / d + 2 : (c[0] - c[1]) / d + 4;
        h *= 60;
        const double skin = h <= 30 ? 1 : h < 45 ? (45 - h) / 15 : h < 300 ? 0 : (h - 300) / 60;
        const double a = v * (1 - skin) + 0.64 * std::pow(v, 1.6) * skin;
        const double boost = (c0 * a + c1 * a * a) * std::pow(t, p) * std::pow(1 - t, q) * std::exp((r0 + ra * a + rt * (1 - t)) * std::max(0.0, 1 - mx));
        t2 = std::min(1.0, t * (1 + boost));
        v2 = mx + v / 4 * shape;
    }
    const double m2 = v2 * (1 - t2), k = (v2 - m2) / (mx - mn);
    for (int i = 0; i < 3; i++) c[i] = m2 + (c[i] - mn) * k;
}

template <class I>
void applyVibranceImpl(I& image, const VibranceSettings& s) {
    // Photoshop's Vibrance and Saturation work in linear light, Vibrance first (vibranceLinear); Saturation moves each
    // colour away from (or towards) the grey 0.288 R + 0.712 G (blue has no weight) by 1 + s, cutting each channel
    // (after PhotoCraft's vibrance_px, fitted on 7,400-colour charts at 8 and 16 bits).
    const double sat = std::clamp(s.saturation, -100.0, 100.0) / 100, vib = std::clamp(s.vibrance, -100.0, 100.0) / 100;
    perPixel(image, [&](double& r, double& g, double& b) {
        double l[3] = {srgbDecode(r), srgbDecode(g), srgbDecode(b)};
        if (vib != 0) vibranceLinear(l, vib);
        if (sat != 0) {
            const double grey = 0.288 * l[0] + 0.712 * l[1];
            for (double& x : l) x = std::clamp(grey + (1 + sat) * (x - grey), 0.0, 1.0);
        }
        r = srgbEncode(std::clamp(l[0], 0.0, 1.0)); g = srgbEncode(std::clamp(l[1], 0.0, 1.0)); b = srgbEncode(std::clamp(l[2], 0.0, 1.0));
    });
}

namespace {

/// Photoshop's luminosity (the blend modes' Lum) and its SetLum: `c` moved to luminosity `l`, then pulled towards its grey
/// until every channel is within 0..1 (ClipColor).
double lum(const double c[3]) { return 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2]; }
void setLum(double c[3], double l) {
    const double d = l - lum(c);
    for (int k = 0; k < 3; k++) c[k] += d;
    const double m = lum(c), lo = std::min({c[0], c[1], c[2]}), hi = std::max({c[0], c[1], c[2]});
    if (lo < 0) for (int k = 0; k < 3; k++) c[k] = m > 0 && m - lo > 0 ? m + (c[k] - m) * m / (m - lo) : m;
    if (hi > 1) for (int k = 0; k < 3; k++) c[k] = m < 1 && hi - m > 0 ? m + (c[k] - m) * (1 - m) / (hi - m) : m;
}

double srgbToLinear(double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }
double linearToSrgb(double v) { v = std::max(0.0, v); return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1 / 2.4) - 0.055; }

} // namespace

namespace {
// Linear sRGB to D50 XYZ (Bradford-adapted, as the ICC sRGB profile has it) and back.
constexpr double toXyz[3][3] = {{0.436074, 0.385064, 0.143080}, {0.222504, 0.716878, 0.060618}, {0.013932, 0.097104, 0.714173}};
constexpr double fromXyz[3][3] = {{3.133856, -1.616867, -0.490615}, {-0.978768, 1.916142, 0.033454}, {0.071945, -0.228991, 1.405243}};
} // namespace

std::array<double, 3> photoFilterScale(const PhotoFilterSettings& s) {
    static constexpr double d50[3] = {0.96422, 1.0, 0.82521};
    const double d = std::clamp(std::isfinite(s.density) ? s.density : 0.0, 0.0, 100.0) / 100;
    const AdjustmentColor f = s.color.clamped();
    const double lin[3] = {srgbToLinear(f.red), srgbToLinear(f.green), srgbToLinear(f.blue)};
    std::array<double, 3> scale{};
    for (int k = 0; k < 3; k++) scale[size_t(k)] = 1 - d + d * (toXyz[k][0] * lin[0] + toXyz[k][1] * lin[1] + toXyz[k][2] * lin[2]) / d50[k];
    return scale;
}

PhotoFilterMatrix photoFilterMatrix(const PhotoFilterSettings& s, bool keepLuminance) {
    std::array<double, 3> scale = photoFilterScale(s);
    if (keepLuminance && scale[1] > 1e-6) { const double y = scale[1]; for (double& v : scale) v /= y; }
    PhotoFilterMatrix m{};
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            for (int k = 0; k < 3; k++) m[size_t(r)][size_t(c)] += fromXyz[r][k] * scale[k] * toXyz[k][c];
    return m;
}


template <class I>
void applyPhotoFilterImpl(I& image, const PhotoFilterSettings& s) {
    // Photoshop's lens: the colour's linear light through photoFilterMatrix, encoded again; Preserve Luminosity then puts
    // back the encoded colour's luminosity as the Luminosity blend mode does (SetLum, ClipColor).
    const PhotoFilterMatrix m = photoFilterMatrix(s, false);
    perPixel(image, [&](double& r, double& g, double& b) {
        const double before[3] = {r, g, b};
        double lin[3];
        for (int k = 0; k < 3; k++) lin[k] = srgbToLinear(before[k]);
        double out[3];
        for (int k = 0; k < 3; k++) out[k] = linearToSrgb(m[size_t(k)][0] * lin[0] + m[size_t(k)][1] * lin[1] + m[size_t(k)][2] * lin[2]);
        if (s.preserveLuminosity) setLum(out, lum(before));
        r = std::clamp(out[0], 0.0, 1.0); g = std::clamp(out[1], 0.0, 1.0); b = std::clamp(out[2], 0.0, 1.0);
    });
}

template <class I>
void applyChannelMixerImpl(I& image, const ChannelMixerSettings& s) {
    std::array<std::array<double, 4>, 4> m;
    for (int row = 0; row < 4; row++) for (int k = 0; k < 4; k++) m[size_t(row)][size_t(k)] = std::clamp(s.rows[size_t(row)][size_t(k)], -200.0, 200.0) / 100;
    perPixel(image, [&](double& r, double& g, double& b) {
        auto mix = [&](const std::array<double, 4>& w) { return w[0] * r + w[1] * g + w[2] * b + w[3]; };
        if (s.monochrome) { r = g = b = mix(m[3]); return; }
        const double nr = mix(m[0]), ng = mix(m[1]), nb = mix(m[2]);
        r = nr; g = ng; b = nb;
    });
}

template <class I>
void applySelectiveColorImpl(I& image, const SelectiveColorSettings& s) {
    // Each colour belongs to its hue ranges by (max - mid) for the primaries' and (mid - min) for the secondaries'
    // ranges, to whites above mid-grey, blacks below, and neutrals by how far it is from both; each range moves the
    // ink (1 - channel) of cyan, magenta and yellow, and black moves all three.
    std::array<std::array<double, 4>, 9> adj;
    for (int i = 0; i < 9; i++) for (int k = 0; k < 4; k++) adj[size_t(i)][size_t(k)] = std::clamp(s.ranges[size_t(i)][size_t(k)], -100.0, 100.0) / 100;
    perPixel(image, [&](double& r, double& g, double& b) {
        const double mx = std::max({r, g, b}), mn = std::min({r, g, b}), md = r + g + b - mx - mn;
        double w[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
        // Reds, greens, blues: the brightest channel's lead; yellows, cyans, magentas: the second one's.
        const int primary = mx == r ? 0 : mx == g ? 2 : 4;
        int secondary;
        if (primary == 0) secondary = g >= b ? 1 : 5;
        else if (primary == 2) secondary = r >= b ? 1 : 3;
        else secondary = g >= r ? 3 : 5;
        w[primary] = mx - md;
        w[secondary] = md - mn;
        w[6] = std::max(0.0, (mn - 0.5) * 2);          // whites
        w[8] = std::max(0.0, (0.5 - mx) * 2);          // blacks
        w[7] = std::max(0.0, 1 - (std::fabs(mx - 0.5) + std::fabs(mn - 0.5)));   // neutrals
        double c[3] = {r, g, b};
        double delta[3] = {0, 0, 0};
        for (int range = 0; range < 9; range++) {
            if (w[range] <= 0) continue;
            for (int i = 0; i < 3; i++) {
                const double ink = 1 - c[i];
                // Relative scales by the ink there is; absolute adds it outright. Black adds to every ink.
                const double base = s.absolute ? 1.0 : ink;
                const double change = adj[size_t(range)][size_t(i)] * base + adj[size_t(range)][3] * base;
                delta[i] += w[range] * change;
            }
        }
        r = std::clamp(1 - ((1 - r) + delta[0]), 0.0, 1.0);
        g = std::clamp(1 - ((1 - g) + delta[1]), 0.0, 1.0);
        b = std::clamp(1 - ((1 - b) + delta[2]), 0.0, 1.0);
    });
}

void applyColorBalanceStoredCmy(ImageC8& image, const ColorBalanceSettings& s) {
    if (image.channels() != 5) return;
    StoredCmy<SampleType::U8> t{image};
    applyColorBalanceImpl(t, s);
}
void applyColorBalanceStoredCmy(Image16& image, const ColorBalanceSettings& s) {
    if (image.channels() != 5) return;
    StoredCmy<SampleType::U16> t{image};
    applyColorBalanceImpl(t, s);
}

void applyPhotoFilterStoredCmy(ImageC8& image, const PhotoFilterSettings& s) {
    if (image.channels() != 5) return;
    StoredCmy<SampleType::U8> t{image};
    applyPhotoFilterImpl(t, s);
}
void applyPhotoFilterStoredCmy(Image16& image, const PhotoFilterSettings& s) {
    if (image.channels() != 5) return;
    StoredCmy<SampleType::U16> t{image};
    applyPhotoFilterImpl(t, s);
}

void applyBlackWhite(Image& image, const BlackWhiteSettings& s) { applyBlackWhiteImpl(image, s); }
void applyBlackWhite(Image16& image, const BlackWhiteSettings& s) { applyBlackWhiteImpl(image, s); }
void applyColorBalance(Image& image, const ColorBalanceSettings& s) { applyColorBalanceImpl(image, s); }
void applyColorBalance(Image16& image, const ColorBalanceSettings& s) { applyColorBalanceImpl(image, s); }
void applyVibrance(Image& image, const VibranceSettings& s) { applyVibranceImpl(image, s); }
void applyVibrance(Image16& image, const VibranceSettings& s) { applyVibranceImpl(image, s); }
void applyPhotoFilter(Image& image, const PhotoFilterSettings& s) { applyPhotoFilterImpl(image, s); }
void applyPhotoFilter(Image16& image, const PhotoFilterSettings& s) { applyPhotoFilterImpl(image, s); }
void applyChannelMixer(Image& image, const ChannelMixerSettings& s) { applyChannelMixerImpl(image, s); }
void applyChannelMixer(Image16& image, const ChannelMixerSettings& s) { applyChannelMixerImpl(image, s); }
void applySelectiveColor(Image& image, const SelectiveColorSettings& s) { applySelectiveColorImpl(image, s); }
void applySelectiveColor(Image16& image, const SelectiveColorSettings& s) { applySelectiveColorImpl(image, s); }
void applyBlackWhite(ImageF& image, const BlackWhiteSettings& s, const TransferCurve&) {
    // Photoshop's 32-bit Black & White: the grey from the linear values as stored; the tint, its colour's values as
    // given, moved to that grey's luminosity as the Color blend mode does at 32 bits (only negatives pulled in, light
    // above 1 kept). After PhotoCraft's (crates/compose/src/adjust.rs at 04d0212, MIT OR Apache-2.0, used under MIT);
    // photocraft-corpus rgb32 black-white-tint.psd within half a level.
    BlackWhiteSettings plain = s;
    plain.tint = false;
    LinearImageF t{image};
    applyBlackWhiteImpl(t, plain);
    if (!s.tint) return;
    const double tint[3] = {s.tintColor.red, s.tintColor.green, s.tintColor.blue};
    perPixel(t, [&](double& r, double& g, double& b) {
        const double d = r - lum(tint);   // r, g and b are the grey
        double c[3] = {tint[0] + d, tint[1] + d, tint[2] + d};
        const double m = lum(c), lo = std::min({c[0], c[1], c[2]});
        if (lo < 0) for (double& v : c) v = m > 0 && m - lo > 0 ? m + (v - m) * m / (m - lo) : m;
        r = c[0]; g = c[1]; b = c[2];
    });
}
void applyColorBalance(ImageF& image, const ColorBalanceSettings& s, const TransferCurve&) { LinearImageF t{image}; applyColorBalanceImpl(t, s); }
void applyVibrance(ImageF& image, const VibranceSettings& s, const TransferCurve& curve) { EncodedImageF t{image, curve}; applyVibranceImpl(t, s); }
void applyChannelMixer(ImageF& image, const ChannelMixerSettings& s, const TransferCurve&) {
    // Photoshop's 32-bit Channel Mixer on the linear values, unclipped: each source percentage cut to 1/1024 towards
    // zero and the constant counting 255/512 of its value (PhotoCraft's, crates/compose/src/adjust.rs at 04d0212, MIT
    // OR Apache-2.0, used under MIT; photocraft-corpus rgb32 channel-mixer.psd exact).
    ChannelMixerSettings cut = s;
    for (auto& row : cut.rows) {
        for (int k = 0; k < 3; k++) row[size_t(k)] = std::trunc(std::clamp(row[size_t(k)], -200.0, 200.0) / 100 * 1024) / 1024 * 100;
        row[3] = std::clamp(row[3], -200.0, 200.0) * 255 / 512;
    }
    LinearImageF t{image};
    applyChannelMixerImpl(t, cut);
}

void applyThreshold(Image16& image, const ThresholdSettings& s) {
    // Photoshop's luminance weights on the exact straight colour. The half-unit allowance makes a 16-bit copy of an
    // 8-bit image split exactly where the 8-bit integer sum does (its colours are within 0.4 units of the bytes').
    const double level = std::clamp(s.level, 1, 255) * 100.0 - 0.5;
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < image.width(); x++) {
                uint16_t* p = image.pixel(x, y);
                const int a = p[3];
                if (!a) continue;
                const double k = 255.0 / a;
                const double sum = std::min(255.0, p[0] * k) * 30 + std::min(255.0, p[1] * k) * 59 + std::min(255.0, p[2] * k) * 11;
                p[0] = p[1] = p[2] = sum >= level ? uint16_t(a) : 0;
            }
    });
}

double brightnessContrastAt(const BrightnessContrastSettings& settings, double value) {
    const BrightnessContrastSettings s = settings.normalized();
    value = std::clamp(value, 0.0, 1.0);
    if (!s.legacy) return std::clamp(contrastValue(s.contrast, brightnessValue(s.brightness, value)), 0.0, 1.0);
    // The legacy formulas of apply() on the 0..255 scale, unrounded.
    const double v = value * 255;
    const int b = s.brightness, c = s.contrast;
    double out;
    if (c == 0) out = v + b;
    else if (c >= 100) out = v + b >= 126.5 ? 255 : 0;
    else if (c > 0) out = (v + b - 127.5) * 100.0 / (100.0 - c) + 127.5;
    else out = (v - 127.5) * (100.0 + c) / 100.0 + 127.5 + b;
    return std::clamp(out / 255, 0.0, 1.0);
}

} // namespace nekophoto
