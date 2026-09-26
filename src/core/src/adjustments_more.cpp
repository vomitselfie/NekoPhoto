// Photoshop's other adjustment layers (adjustments.h): Brightness/Contrast, Posterize and Threshold from Patchy's
// calibration against Photoshop 2026 (src/core/adjustment_layer.cpp there, MIT; Posterize refitted to Photoshop's
// composite); Color Balance fitted here to Photoshop's composites; Black & White from upstream Compositor's
// AdjustPixels.c (MIT); Vibrance, Photo Filter, Channel Mixer and Selective Color from their
// published formulas, unchecked against Photoshop (docs/adjustment-layers.md).
#include "compositor/adjustments.h"
#include "compositor/parallel.h"
#include <algorithm>
#include <cmath>

namespace compositor {

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

double luma(double r, double g, double b) { return 0.299 * r + 0.587 * g + 0.114 * b; }

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

void applyBlackWhite(Image& image, const BlackWhiteSettings& s) {
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

void applyColorBalance(Image& image, const ColorBalanceSettings& s) {
    // Fitted to Photoshop's own composites of Patchy's two Color Balance files. Midtones are a gamma per channel,
    // v^(2^(-amount / 100)), exact on every sample; shadows and highlights move the channel's black and white points
    // (a shadow towards the colour lifts the output black, away from it clips the input black; highlights the same at
    // the white end), about 0.3% of the range a slider step, which leaves some 10 levels on the file that uses all three
    // ranges; Preserve Luminosity puts the original luminance back as Photoshop's Luminosity blend does.
    double gamma[3], inBlack[3], inWhite[3], outBlack[3], outWhite[3];
    for (int c = 0; c < 3; c++) {
        const double sh = std::clamp(s.ranges[0][size_t(c)], -100.0, 100.0), mid = std::clamp(s.ranges[1][size_t(c)], -100.0, 100.0);
        const double hi = std::clamp(s.ranges[2][size_t(c)], -100.0, 100.0);
        gamma[c] = std::pow(2.0, -mid / 100);
        inBlack[c] = sh < 0 ? -sh * 0.0035 : 0;
        outBlack[c] = sh > 0 ? sh * 0.0035 : 0;
        inWhite[c] = hi > 0 ? 1 - hi * 0.003 : 1;
        outWhite[c] = hi < 0 ? 1 + hi * 0.003 : 1;
    }
    auto lum = [](double r, double g, double b) { return 0.3 * r + 0.59 * g + 0.11 * b; };
    perPixel(image, [&](double& r, double& g, double& b) {
        const double before = lum(r, g, b);
        double c[3] = {r, g, b};
        for (int i = 0; i < 3; i++) {
            double x = std::clamp((c[i] - inBlack[i]) / std::max(1e-6, inWhite[i] - inBlack[i]), 0.0, 1.0);
            x = std::pow(x, gamma[i]);
            c[i] = outBlack[i] + (outWhite[i] - outBlack[i]) * x;
        }
        if (s.preserveLuminosity) {
            // Photoshop's SetLum and ClipColor (the Luminosity blend).
            const double d = before - lum(c[0], c[1], c[2]);
            for (double& v : c) v += d;
            const double l = lum(c[0], c[1], c[2]), lo = std::min({c[0], c[1], c[2]}), hi = std::max({c[0], c[1], c[2]});
            if (lo < 0 && l > lo) for (double& v : c) v = l + (v - l) * l / (l - lo);
            if (hi > 1 && hi > l) for (double& v : c) v = l + (v - l) * (1 - l) / (hi - l);
        }
        r = c[0]; g = c[1]; b = c[2];
    });
}

void applyVibrance(Image& image, const VibranceSettings& s) {
    // Saturation scales every colour's distance from its luminance; Vibrance does the same weighted towards the
    // muted ones (a saturated colour moves little), as Photoshop's does.
    const double sat = std::clamp(s.saturation, -100.0, 100.0) / 100, vib = std::clamp(s.vibrance, -100.0, 100.0) / 100;
    perPixel(image, [&](double& r, double& g, double& b) {
        const double l = luma(r, g, b);
        const double chroma = std::max({r, g, b}) - std::min({r, g, b});
        double k = 1 + sat;
        k *= 1 + vib * (vib > 0 ? (1 - chroma) : 1);
        r = l + (r - l) * k; g = l + (g - l) * k; b = l + (b - l) * k;
        // Held in gamut by moving towards the luminance.
        const double hi = std::max({r, g, b}), lo = std::min({r, g, b});
        double f = 1;
        if (hi > 1 && hi > l) f = std::min(f, (1 - l) / (hi - l));
        if (lo < 0 && lo < l) f = std::min(f, l / (l - lo));
        if (f < 1) { r = l + (r - l) * f; g = l + (g - l) * f; b = l + (b - l) * f; }
    });
}

void applyPhotoFilter(Image& image, const PhotoFilterSettings& s) {
    // The colour laid over the image as a lens filter would: each channel multiplied by the filter's, mixed in by the
    // density; Preserve Luminosity keeps each pixel's luminance.
    const double d = std::clamp(s.density, 0.0, 100.0) / 100;
    const AdjustmentColor f = s.color.clamped();
    perPixel(image, [&](double& r, double& g, double& b) {
        const double before = luma(r, g, b);
        r = r * (1 - d) + r * f.red * 2 * d;
        g = g * (1 - d) + g * f.green * 2 * d;
        b = b * (1 - d) + b * f.blue * 2 * d;
        r = std::min(r, 1.0); g = std::min(g, 1.0); b = std::min(b, 1.0);
        if (s.preserveLuminosity) {
            const double after = luma(r, g, b);
            if (after > 0.0001) { const double k = before / after; r *= k; g *= k; b *= k; }
        }
    });
}

void applyChannelMixer(Image& image, const ChannelMixerSettings& s) {
    std::array<std::array<double, 4>, 4> m;
    for (int row = 0; row < 4; row++) for (int k = 0; k < 4; k++) m[size_t(row)][size_t(k)] = std::clamp(s.rows[size_t(row)][size_t(k)], -200.0, 200.0) / 100;
    perPixel(image, [&](double& r, double& g, double& b) {
        auto mix = [&](const std::array<double, 4>& w) { return w[0] * r + w[1] * g + w[2] * b + w[3]; };
        if (s.monochrome) { r = g = b = mix(m[3]); return; }
        const double nr = mix(m[0]), ng = mix(m[1]), nb = mix(m[2]);
        r = nr; g = ng; b = nb;
    });
}

void applySelectiveColor(Image& image, const SelectiveColorSettings& s) {
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

} // namespace compositor
