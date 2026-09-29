#include "compositor/toning.h"
#include "compositor/blur.h"
#include "compositor/parallel.h"
#include <algorithm>
#include <cmath>
#include <type_traits>

namespace compositor {

// Each range's curve bends the tones it names most and leaves black and white where they are (Dodge Shadows lifts
// black, as Photoshop's does; Burn Highlights pulls white down).
double dodgeTone(double v, ToneRange range) {
    v = std::clamp(v, 0.0, 1.0);
    switch (range) {
    case ToneRange::Shadows: return 0.5 + v * v / 2;             // black to middle grey, white stays
    case ToneRange::Midtones: return std::sqrt(v);               // gamma 0.5
    case ToneRange::Highlights: return v * (2 - v);               // the bright half pushed to white
    }
    return v;
}

double burnTone(double v, ToneRange range) {
    v = std::clamp(v, 0.0, 1.0);
    switch (range) {
    case ToneRange::Shadows: return v * v * (1.5 - v / 2);        // the dark half pushed to black, white stays
    case ToneRange::Midtones: return v * v;                      // gamma 2
    case ToneRange::Highlights: return v * (1 - v / 2);           // white to middle grey
    }
    return v;
}

namespace {

double luma(const double c[3]) { return 0.299 * c[0] + 0.587 * c[1] + 0.114 * c[2]; }

/// Calls `f` with each visible pixel's straight colour (0..1), writing back what it leaves there.
template <typename F>
void eachStraight(Image& image, F f) {
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < image.width(); x++) {
                uint8_t* p = image.pixel(x, y);
                const int a = p[3];
                if (!a) continue;
                double c[3];
                for (int i = 0; i < 3; i++) c[i] = std::min(1.0, p[i] / double(a));
                f(c);
                for (int i = 0; i < 3; i++) p[i] = uint8_t(std::lround(std::clamp(c[i], 0.0, 1.0) * a));
            }
    });
}

/// The same over a 16-bit image: straight colour from the 15-bit samples, written back rounded to 15 bits.
template <typename F>
void eachStraight(Image16& image, F f) {
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < image.width(); x++) {
                uint16_t* p = image.pixel(x, y);
                const int a = std::min<int>(p[3], 32768);
                if (!a) continue;
                double c[3];
                for (int i = 0; i < 3; i++) c[i] = std::min(1.0, p[i] / double(a));
                f(c);
                for (int i = 0; i < 3; i++) p[i] = uint16_t(std::lround(std::clamp(c[i], 0.0, 1.0) * a));
            }
    });
}

template <typename Img>
void toneAny(Img& image, const ToningSettings& s);

} // namespace

void toneImage(Image& image, const ToningSettings& s) { toneAny(image, s); }
void toneImage(Image16& image, const ToningSettings& s) { toneAny(image, s); }

namespace {

template <typename Img>
void toneAny(Img& image, const ToningSettings& s) {
    if (s.kind == ToningKind::Sponge) {
        const double k = s.saturate ? 2.0 : 0.0;   // twice the chroma, or none
        eachStraight(image, [&](double c[3]) {
            const double l = luma(c);
            double out[3];
            for (int i = 0; i < 3; i++) out[i] = l + (c[i] - l) * k;
            // Saturating keeps the lightness: scale the chroma back rather than clip a channel on its own.
            double over = 1;
            for (int i = 0; i < 3; i++) {
                const double d = out[i] - l;
                if (out[i] > 1 && d > 0) over = std::min(over, (1 - l) / d);
                if (out[i] < 0 && d < 0) over = std::min(over, -l / d);
            }
            for (int i = 0; i < 3; i++) c[i] = l + (out[i] - l) * over;
        });
        return;
    }
    auto curve = [&](double v) { return s.kind == ToningKind::Dodge ? dodgeTone(v, s.range) : burnTone(v, s.range); };
    if (!s.protectTones) {
        eachStraight(image, [&](double c[3]) { for (int i = 0; i < 3; i++) c[i] = curve(c[i]); });
        return;
    }
    // Protect Tones: the lightness follows the curve and the colour moves with it, its chroma scaled down as far as
    // it must be to stay inside the gamut, so hues do not shift and channels do not clip.
    eachStraight(image, [&](double c[3]) {
        const double l = luma(c), target = curve(l);
        double chroma[3];
        for (int i = 0; i < 3; i++) chroma[i] = c[i] - l;
        double scale = 1;
        for (int i = 0; i < 3; i++) {
            const double v = target + chroma[i];
            if (v > 1 && chroma[i] > 0) scale = std::min(scale, (1 - target) / chroma[i]);
            if (v < 0 && chroma[i] < 0) scale = std::min(scale, -target / chroma[i]);
        }
        for (int i = 0; i < 3; i++) c[i] = target + chroma[i] * std::max(0.0, scale);
    });
}

} // namespace

void sharpenImage(Image& image, double radius) {
    Image blurred = image;
    gaussianBlur(blurred, std::max(0.3, radius));
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < image.width(); x++) {
                uint8_t* p = image.pixel(x, y);
                const uint8_t* b = blurred.pixel(x, y);
                const int a = p[3];
                if (!a) continue;
                // On straight colour, so the soft edge of a shape does not darken; the alpha stays.
                const double ba = std::max(1, int(b[3]));
                for (int i = 0; i < 3; i++) {
                    const double c = p[i] / double(a), blur = b[i] / ba;
                    p[i] = uint8_t(std::lround(std::clamp(c + (c - blur), 0.0, 1.0) * a));
                }
            }
    });
}

void sharpenImage(Image16& image, double radius) {
    Image16 blurred = image;
    gaussianBlur(blurred, std::max(0.3, radius));
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < image.width(); x++) {
                uint16_t* p = image.pixel(x, y);
                const uint16_t* b = blurred.pixel(x, y);
                const int a = std::min<int>(p[3], 32768);
                if (!a) continue;
                const double ba = std::max(1, int(b[3]));
                for (int i = 0; i < 3; i++) {
                    const double c = p[i] / double(a), blur = b[i] / ba;
                    p[i] = uint16_t(std::lround(std::clamp(c + (c - blur), 0.0, 1.0) * a));
                }
            }
    });
}

void sharpenImage(ImageF& image, double radius) {
    ImageF blurred = image;
    gaussianBlur(blurred, std::max(0.3, radius));
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < image.width(); x++) {
                float* p = image.pixel(x, y);
                const float* b = blurred.pixel(x, y);
                const float a = std::min(p[3], 1.0f);
                if (!(a > 0)) continue;
                const float ba = std::max(b[3], 1.0f / 65536);
                for (int i = 0; i < 3; i++) {
                    const float c = p[i] / a, blur = b[i] / ba;
                    p[i] = std::max(0.0f, c + (c - blur)) * a;
                }
            }
    });
}

// ---- CMYK and Lab ------------------------------------------------------------------------------------------------

namespace {

/// Calls `f` with each visible pixel's straight samples (0..1, `n - 1` of them), written back rounded.
template <typename Img, typename F>
void eachStraightN(Img& image, int n, double one, F f) {
    parallelRows(0, image.height(), [&](int y0, int y1) {
        double c[4];
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < image.width(); x++) {
                auto* p = image.pixel(x, y);
                using Sample = std::remove_reference_t<decltype(p[0])>;
                const double a = std::min(double(p[n - 1]), one);
                if (!(a > 0)) continue;
                for (int i = 0; i < n - 1; i++) c[i] = std::min(1.0, p[i] / a);
                f(c);
                for (int i = 0; i < n - 1; i++) p[i] = Sample(std::lround(std::clamp(c[i], 0.0, 1.0) * a));
            }
    });
}

template <typename Img>
void toneNative(Img& image, int n, double one, const ToningSettings& s, ColorMode mode) {
    auto curve = [&](double v) { return s.kind == ToningKind::Dodge ? dodgeTone(v, s.range) : burnTone(v, s.range); };
    if (mode == ColorMode::Lab) {
        // a and b are stored offset by half the range (colormodes.h): 128 of 255, 16384 of 32768.
        const double mid = one == 255 ? 128.0 / 255.0 : 0.5;
        if (s.kind == ToningKind::Sponge) {
            const double k = s.saturate ? 2.0 : 0.0;
            eachStraightN(image, n, one, [&](double c[4]) {
                const double da = c[1] - mid, db = c[2] - mid;
                // Saturating keeps the hue: both axes scaled back together rather than one clipped.
                double scale = k;
                for (double d : {da, db}) {
                    if (d * k > 1 - mid) scale = std::min(scale, (1 - mid) / d);
                    if (d * k < -mid) scale = std::min(scale, -mid / d);
                }
                c[1] = mid + da * scale;
                c[2] = mid + db * scale;
            });
            return;
        }
        eachStraightN(image, n, one, [&](double c[4]) { c[0] = curve(c[0]); });
        return;
    }
    // CMYK: C, M, Y and K as stored are each plate's brightness (the ink inverted).
    if (s.kind == ToningKind::Sponge) {
        const double k = s.saturate ? 2.0 : 0.0;
        eachStraightN(image, n, one, [&](double c[4]) {
            const double l = luma(c);
            double out[3], over = 1;
            for (int i = 0; i < 3; i++) out[i] = l + (c[i] - l) * k;
            for (int i = 0; i < 3; i++) {
                const double d = out[i] - l;
                if (out[i] > 1 && d > 0) over = std::min(over, (1 - l) / d);
                if (out[i] < 0 && d < 0) over = std::min(over, -l / d);
            }
            for (int i = 0; i < 3; i++) c[i] = l + (out[i] - l) * over;
        });
        return;
    }
    eachStraightN(image, n, one, [&](double c[4]) { for (int i = 0; i < 4; i++) c[i] = curve(c[i]); });
}

/// A five-sample image blurred as two four-sample ones (C, M, Y, alpha and K, alpha) with the kernels of its depth:
/// premultiplied samples blur independently, so the split is exact.
template <typename Img, typename Four>
void blurSamples(Img& image, double sigma) {
    const int w = image.width(), h = image.height(), n = image.channels();
    if constexpr (std::is_same_v<Four, Img>)
        if (n == 4) { gaussianBlur(image, sigma); return; }
    Four inks(w, h), black(w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const auto* p = image.pixel(x, y);
            auto* a = inks.pixel(x, y);
            auto* b = black.pixel(x, y);
            a[0] = p[0]; a[1] = p[1]; a[2] = p[2]; a[3] = p[n - 1];
            b[0] = n == 5 ? p[3] : 0; b[1] = 0; b[2] = 0; b[3] = p[n - 1];
        }
    gaussianBlur(inks, sigma);
    if (n == 5) gaussianBlur(black, sigma);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            auto* p = image.pixel(x, y);
            const auto* a = inks.pixel(x, y);
            const auto* b = black.pixel(x, y);
            p[0] = a[0]; p[1] = a[1]; p[2] = a[2];
            if (n == 5) p[3] = b[0];
            p[n - 1] = a[3];
        }
}

template <typename Img>
void sharpenSamplesImpl(Img& image, double radius, double one) {
    Img blurred = image;
    gaussianBlurSamples(blurred, std::max(0.3, radius));
    const int n = image.channels();
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++)
            for (int x = 0; x < image.width(); x++) {
                auto* p = image.pixel(x, y);
                using Sample = std::remove_reference_t<decltype(p[0])>;
                const auto* b = blurred.pixel(x, y);
                const double a = std::min(double(p[n - 1]), one);
                if (!(a > 0)) continue;
                const double ba = std::max(1.0, double(b[n - 1]));
                for (int i = 0; i < n - 1; i++) {
                    const double c = p[i] / a, blur = b[i] / ba;
                    p[i] = Sample(std::lround(std::clamp(c + (c - blur), 0.0, 1.0) * a));
                }
            }
    });
}

} // namespace

void toneImage(Image& image, const ToningSettings& s, ColorMode mode) {
    if (mode == ColorMode::RGB) { toneImage(image, s); return; }
    toneNative(image, 4, 255.0, s, mode);
}
void toneImage(Image16& image, const ToningSettings& s, ColorMode mode) {
    if (image.channels() == 5) { toneNative(image, 5, 32768.0, s, ColorMode::CMYK); return; }
    if (mode == ColorMode::RGB) { toneImage(image, s); return; }
    toneNative(image, 4, 32768.0, s, mode);
}
void toneImage(ImageC8& image, const ToningSettings& s) { toneNative(image, image.channels(), 255.0, s, ColorMode::CMYK); }

void gaussianBlurSamples(ImageC8& image, double sigma) { blurSamples<ImageC8, Image>(image, sigma); }
void gaussianBlurSamples(Image16& image, double sigma) { blurSamples<Image16, Image16>(image, sigma); }
void sharpenSamples(ImageC8& image, double radius) { sharpenSamplesImpl(image, radius, 255.0); }
void sharpenSamples(Image16& image, double radius) { sharpenSamplesImpl(image, radius, 32768.0); }

} // namespace compositor
