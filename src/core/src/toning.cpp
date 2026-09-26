#include "compositor/toning.h"
#include "compositor/blur.h"
#include "compositor/parallel.h"
#include <algorithm>
#include <cmath>

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

} // namespace

void toneImage(Image& image, const ToningSettings& s) {
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

} // namespace compositor
