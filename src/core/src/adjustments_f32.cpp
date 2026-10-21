// The adjustments at 32 bits (adjustments.h, "32 bits"): Photoshop's 32-bit set on premultiplied linear float.
//
// The pixels stay linear. Photoshop runs most kinds on the linear values as stored, with the 8-bit settings
// (photocraft-corpus rgb32 and gray32, exact or within half a level of its merged images; docs/bit-depth.md):
//
// - Levels: the range formula unclipped, the gamma a power mirrored below the black point (PhotoCraft's levels_float).
// - Curves: the curve on the linear value; above 1 along its end tangent (NekoPhoto's stated choice).
// - Brightness/Contrast: legacy unclipped about 127 of 255; modern its curve within 0..1.
// - Exposure: the exact multiply by 2^exposure, the offset, the gamma as a power.
// - Hue/Saturation: the model on the linear values, unlimited (HueSaturationSettings::linearAdjuster).
// - Color Balance, Black & White and Channel Mixer: on the linear values (adjustments_more.cpp).
// - Photo Filter: its linear-light matrix, the luminance kept in it with Preserve Luminosity.
// - Vibrance and Color Lookup: on the colour encoded through `curve` (encodeExtended), a colour brighter than white
//   adjusted as the same colour at white and scaled back up. Invert: 1 - the encoded value. Gradient Map: the colour at
//   the encoded luminosity, cut at white.
//
// Posterize, Threshold, Selective Color and Grain are not drawn at 32 bits: they return false and are greyed ("Not
// available in 32-bit mode"). Photoshop 27 offers Selective Color there (photocraft-corpus rgb32), by a rule not known
// yet.
#include "nekophoto/adjustments.h"
#include "nekophoto/colormgmt.h"
#include "nekophoto/depth.h"
#include "nekophoto/parallel.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace nekophoto {

namespace {

// ---- Levels ----------------------------------------------------------------------------------------------------------

/// Photoshop's 32-bit Levels on a linear value: neither the input cut to the input range nor the result to the output
/// range, and the midtone gamma a plain power mirrored below the black point (PhotoCraft's `levels_float`,
/// crates/compose/src/adjust.rs at 04d0212, MIT OR Apache-2.0, used under MIT; photocraft-corpus rgb32 and gray32
/// levels.psd exact).
struct LevelsCurve {
    double black, span, inverseGamma, outBlack, outSpan;
    bool linear;
    explicit LevelsCurve(const LevelsRange& range) {
        const LevelsRange n = range.normalized();
        black = n.black / 255;
        span = std::max(1e-6, (n.white - n.black) / 255);
        inverseGamma = 1 / n.gamma;
        outBlack = n.outputBlack / 255;
        outSpan = (n.outputWhite - n.outputBlack) / 255;
        linear = n.gamma == 1;
    }
    double operator()(double x) const {
        const double t = (x - black) / span;
        return outBlack + (linear ? t : std::copysign(std::pow(std::fabs(t), inverseGamma), t)) * outSpan;
    }
};

// ---- Curves ----------------------------------------------------------------------------------------------------------

/// CurvesSettings::value for one channel with its spline solved once; above 255 continued along its end tangent.
struct CurveSpline {
    NaturalCurve curve;
    double endSlope = 0, endValue = 255;

    CurveSpline(const CurvesSettings& settings, int channel) : curve(settings.channels[size_t(std::clamp(channel, 0, 3))]) {
        endValue = curve(255);
        endSlope = curve.x.back() >= 255 ? curve.endSlope() : 0.0;
    }

    double operator()(double at) const { return at <= 255 ? curve(at) : std::max(0.0, endValue + (at - 255) * endSlope); }
};

// ---- the per-pixel frame -------------------------------------------------------------------------------------------

/// sRGB's curve both ways over 0..1 in 65536 steps, from its formulas in double. Its linear toe keeps linear
/// interpolation between the steps within 1e-7 of the formula everywhere (a pure power law would not be, near 0).
struct SrgbTables {
    static constexpr int steps = 65536;
    std::vector<float> encode, decode;
    SrgbTables() : encode(steps + 1), decode(steps + 1) {
        for (int i = 0; i <= steps; i++) {
            const double v = double(i) / steps;
            encode[size_t(i)] = float(v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1 / 2.4) - 0.055);
            decode[size_t(i)] = float(v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4));
        }
    }
    static float lookup(const std::vector<float>& table, float x) {
        const float s = x * float(steps);
        const int i = std::min(int(s), steps - 1);
        return table[size_t(i)] + (table[size_t(i) + 1] - table[size_t(i)]) * (s - float(i));
    }
};

const SrgbTables& srgbTables() {
    static const SrgbTables tables;
    return tables;
}

/// encodeExtended / decodeExtended, sRGB's through the tables within 0..1 (the common case: no power per sample).
struct Encoding {
    const TransferCurve& curve;
    const SrgbTables* srgb;
    explicit Encoding(const TransferCurve& c) : curve(c), srgb(c.kind() == TransferCurve::Kind::SRGB ? &srgbTables() : nullptr) {}
    float encode(float v) const {
        if (!srgb || !(v <= 1.0f)) return encodeExtended(curve, v);
        return v > 0 ? SrgbTables::lookup(srgb->encode, v) : 0.0f;
    }
    float decode(float e) const {
        if (!srgb || !(e <= 1.0f)) return decodeExtended(curve, e);
        return e > 0 ? SrgbTables::lookup(srgb->decode, e) : 0.0f;
    }
};

/// Each pixel's straight linear colour, per channel through `f` (linear in, linear out), premultiplied again.
template <class F>
void perLinearChannel(ImageF& image, F&& f) {
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            float* p = image.row(y);
            for (int x = 0; x < image.width(); x++, p += 4) {
                const float a = p[3];
                if (!(a > 0)) continue;
                for (int c = 0; c < 3; c++) p[c] = cleanColour(float(f(c, double(p[c]) / a) * a));
            }
        }
    });
}

/// Each pixel's straight linear colour, per channel through `f` (encoded in, encoded out), premultiplied again.
template <class F>
void perChannel(ImageF& image, const TransferCurve& curve, F&& f) {
    const Encoding encoding(curve);
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            float* p = image.row(y);
            for (int x = 0; x < image.width(); x++, p += 4) {
                const float a = p[3];
                if (!(a > 0)) continue;
                const float inverse = 1.0f / a;
                for (int c = 0; c < 3; c++) {
                    const double e = encoding.encode(p[c] * inverse);
                    p[c] = cleanColour(encoding.decode(float(f(c, e))) * a);
                }
            }
        }
    });
}

} // namespace

// Each pixel's straight colour, encoded and scaled into 0..1, through `f` (0..1 in, clamped to 0..1 out), and back: the
// colour kinds' frame at 32 bits (adjustments_more.cpp and colorlookup.cpp use it through their perPixel templates).
void forEachEncodedColour(ImageF& image, const TransferCurve& curve, const std::function<void(double&, double&, double&)>& f) {
    const Encoding encoding(curve);
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            float* p = image.row(y);
            for (int x = 0; x < image.width(); x++, p += 4) {
                const float a = p[3];
                if (!(a > 0)) continue;
                float lin[3];
                for (int c = 0; c < 3; c++) lin[c] = p[c] / a;
                const float bright = std::max({lin[0], lin[1], lin[2], 1.0f});
                double e[3];
                for (int c = 0; c < 3; c++) e[c] = encoding.encode(lin[c] / bright);
                f(e[0], e[1], e[2]);
                for (int c = 0; c < 3; c++) p[c] = cleanColour(encoding.decode(float(std::clamp(e[c], 0.0, 1.0))) * bright * a);
            }
        }
    });
}

namespace {

// ---- Hue/Saturation: Photoshop's model on each encoded colour ----------------------------------------------------

void applyHueSaturationF(ImageF& image, const HueSaturationSettings& settings, const TransferCurve&) {
    // Photoshop's 32-bit Hue/Saturation works on the linear values as stored (HueSaturationSettings::linearAdjuster).
    if (settings.isIdentity()) return;
    const auto adjust = settings.linearAdjuster();
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            float* p = image.row(y);
            for (int x = 0; x < image.width(); x++, p += 4) {
                const float a = p[3];
                if (!(a > 0)) continue;
                double c[3] = {p[0] / a, p[1] / a, p[2] / a};
                adjust(c[0], c[1], c[2]);
                for (int k = 0; k < 3; k++) p[k] = cleanColour(float(c[k] * a));
            }
        }
    });
}

void applyGradientMapF(ImageF& image, const GradientMapSettings& settings, const TransferCurve& curve) {
    const Encoding encoding(curve);
    // The ramp in the encoded values, at the encoded colour's luminosity.
    const GradientStops ramp = settings.ramp();
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            float* p = image.row(y);
            for (int x = 0; x < image.width(); x++, p += 4) {
                const float a = p[3];
                if (!(a > 0)) continue;
                double e[3];
                for (int c = 0; c < 3; c++) e[c] = encoding.encode(p[c] / a);
                const double t = std::clamp(GradientMapSettings::luminosity(e[0], e[1], e[2]), 0.0, 1.0);
                float col[4];
                ramp.sample(float(t), col);
                for (int c = 0; c < 3; c++) p[c] = cleanColour(encoding.decode(col[c]) * a);
            }
        }
    });
}

} // namespace

void applyPhotoFilter(ImageF& image, const PhotoFilterSettings& settings, const TransferCurve&) {
    // On the linear samples as stored, unclipped (photocraft-corpus rgb32 photo-filter.psd: white filters to red 1.19);
    // Preserve Luminosity keeps the luminance Y in the matrix itself.
    const PhotoFilterMatrix m = photoFilterMatrix(settings, settings.preserveLuminosity);
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            float* p = image.row(y);
            for (int x = 0; x < image.width(); x++, p += 4) {
                if (!(p[3] > 0)) continue;
                const double c[3] = {p[0], p[1], p[2]};   // premultiplied: the map is linear
                for (int k = 0; k < 3; k++) p[k] = cleanColour(float(m[size_t(k)][0] * c[0] + m[size_t(k)][1] * c[1] + m[size_t(k)][2] * c[2]));
            }
        }
    });
}

void applyInvert(ImageF& image, const TransferCurve& curve) {
    perChannel(image, curve, [](int, double e) { return std::max(0.0, 1 - e); });
}

void applyInvert(GrayF& mask) {
    float* d = mask.data();
    for (size_t i = 0, n = size_t(mask.width()) * size_t(mask.height()); i < n; i++) d[i] = 1.0f - cleanCoverage(d[i]);
}

bool adjustmentAt32(AdjustmentKind kind) {
    switch (kind) {
    case AdjustmentKind::Posterize: case AdjustmentKind::Threshold:
    case AdjustmentKind::SelectiveColor: case AdjustmentKind::Grain:
        return false;
    default: return true;
    }
}

bool applyAdjustment(const AdjustmentSettings& settings, ImageF& image, const Rect&, double, const TransferCurve& curve) {
    if (!adjustmentAt32(settings.kind)) return false;
    switch (settings.kind) {
    case AdjustmentKind::Levels: {
        if (settings.levels.isIdentity()) return true;
        const LevelsCurve composite(settings.levels.ranges[0]);
        const LevelsCurve own[3] = {LevelsCurve(settings.levels.ranges[1]), LevelsCurve(settings.levels.ranges[2]), LevelsCurve(settings.levels.ranges[3])};
        // On the linear values as stored, each channel's own range then the composite's.
        const bool identity[4] = {settings.levels.ranges[0] == LevelsRange(), settings.levels.ranges[1] == LevelsRange(),
                                  settings.levels.ranges[2] == LevelsRange(), settings.levels.ranges[3] == LevelsRange()};
        perLinearChannel(image, [&](int c, double v) {
            if (!identity[c + 1]) v = own[c](v);
            return identity[0] ? v : composite(v);
        });
        return true;
    }
    case AdjustmentKind::Curves: {
        // On the linear values as stored too (photocraft-corpus rgb32 and gray32 curves.psd and curves-per-channel.psd
        // exact); above 1 a curve goes on along its end tangent.
        if (!settings.curves.isValid() || settings.curves.isIdentity()) return true;
        const CurveSpline composite(settings.curves, 0);
        const CurveSpline own[3] = {CurveSpline(settings.curves, 1), CurveSpline(settings.curves, 2), CurveSpline(settings.curves, 3)};
        perLinearChannel(image, [&](int c, double v) { return composite(own[c](v * 255)) / 255; });
        return true;
    }
    case AdjustmentKind::Exposure: {
        const ExposureSettings s = settings.exposure.normalized();
        if (s.isIdentity()) return true;
        const float scale = float(std::pow(2.0, s.exposure)), offset = float(s.offset), inverseGamma = float(1 / s.gamma);
        const bool plain = s.gamma == 1;
        parallelRows(0, image.height(), [&](int y0, int y1) {
            for (int y = y0; y < y1; y++) {
                float* p = image.row(y);
                for (int x = 0; x < image.width(); x++, p += 4) {
                    const float a = p[3];
                    if (!(a > 0)) continue;
                    for (int c = 0; c < 3; c++) {
                        const float v = std::max(0.0f, p[c] / a * scale + offset);
                        p[c] = cleanColour((plain ? v : std::pow(v, inverseGamma)) * a);
                    }
                }
            }
        });
        return true;
    }
    case AdjustmentKind::BrightnessContrast: {
        // Photoshop 27 offers it at 32 bits, on the linear values as stored. Legacy: unclipped, the contrast pivoting
        // at 127 of 255, brightness added before a raised contrast and after a lowered one (PhotoCraft's
        // legacy_brightness_contrast_hdr, crates/compose/src/adjust.rs at 04d0212, MIT OR Apache-2.0, used under MIT;
        // photocraft-corpus rgb32 and gray32 brightness-contrast-legacy.psd exact). Modern: its curve, within 0..1.
        const BrightnessContrastSettings b = settings.brightnessContrast.normalized();
        if (b.brightness == 0 && b.contrast == 0) return true;
        if (b.legacy) {
            const double pivot = 127.0 / 255, c = std::min(b.contrast, 99), k = c >= 0 ? 1 / (1 - c / 100) : 1 + c / 100;
            const double pre = c >= 0 ? b.brightness / 255.0 : 0, post = c >= 0 ? 0 : b.brightness / 255.0;
            perLinearChannel(image, [&](int, double v) { return (v + pre - pivot) * k + pivot + post; });
        } else {
            perLinearChannel(image, [&](int, double v) { return brightnessContrastAt(b, v); });
        }
        return true;
    }
    case AdjustmentKind::HueSaturation: applyHueSaturationF(image, settings.hsv, curve); return true;
    case AdjustmentKind::GradientMap: applyGradientMapF(image, settings.gradientMap, curve); return true;
    case AdjustmentKind::Invert: applyInvert(image, curve); return true;
    case AdjustmentKind::BlackWhite: applyBlackWhite(image, settings.blackWhite, curve); return true;
    case AdjustmentKind::ColorBalance: applyColorBalance(image, settings.colorBalance, curve); return true;
    case AdjustmentKind::Vibrance: applyVibrance(image, settings.vibrance, curve); return true;
    case AdjustmentKind::PhotoFilter: applyPhotoFilter(image, settings.photoFilter, curve); return true;
    case AdjustmentKind::ChannelMixer: applyChannelMixer(image, settings.channelMixer, curve); return true;
    case AdjustmentKind::ColorLookup: applyColorLookup(image, settings.colorLookup, curve); return true;
    default: return false;
    }
}

bool applyAdjustment(const LayerAdjustment& adjustment, ImageF& image, const Rect& region, double scale, const TransferCurve& curve) {
    AdjustmentSettings settings;
    if (!AdjustmentSettings::parse(adjustment.json, settings)) return false;
    return applyAdjustment(settings, image, region, scale, curve);
}

std::array<std::vector<double>, 4> levelsHistogram(const ImageF& image, const GrayF* coverage, const TransferCurve& curve) {
    // 256 bins of the encoded colour at exposure 0, as at the other depths (light above 1 falls in the top bin).
    auto narrow = encodeImage8(image, curve);
    std::shared_ptr<GrayImage> cover = coverage ? narrowGrayF(*coverage) : nullptr;
    return levelsHistogram(*narrow, cover.get());
}

} // namespace nekophoto
