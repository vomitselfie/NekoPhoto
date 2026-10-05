// The adjustments on a CMYK or Lab document's own samples (modeedit.h, docs/color-modes.md "Adjustments and filters").
//
// What each kind changes, following Photoshop's per-mode menus (adjustmentOfferedInMode):
// - Levels and Curves: per channel, on the stored values as Photoshop's channel histograms show them (a CMYK plate
//   dark where the ink is); CMYK's composite goes on every ink after the ink's own, Lab has no composite (its first
//   slot goes with lightness, as older files hold it) and a and b take their own. Brightness/Contrast on every ink in
//   CMYK and on L in Lab; Invert and Posterize on every channel.
// - Exposure (Lab): on L, through relative luminance, as the RGB kernel works on linear light.
// - Threshold and Gradient Map: on the pixel's lightness (L in Lab, the CMYK colour's L* through the profile, read for
//   deciding only), to the settings' colours converted through the profile once (Threshold's black is the profile's
//   black, as the brush paints black).
// - Hue/Saturation and Color Balance (CMYK): the RGB kernels' maths on the stored cyan, magenta and yellow (the
//   complements of the inks) as red, green and blue; black is kept.
// - Photo Filter: the filter colour in the document's mode, laid over each ink (CMYK) as the RGB kernel lays it over
//   each channel, or moved onto a and b (Lab); Preserve Luminosity keeps the lightness.
// - Channel Mixer (CMYK): four ink rows over the four inks and a constant (ChannelMixerSettings::inks).
// - Selective Color (CMYK, its home): each colour range's cyan, magenta, yellow and black move those inks, black on
//   the black plate.
// These are the published formulas adapted to each mode; none is checked against Photoshop's own output yet.
#include "compositor/colormgmt.h"
#include "compositor/colormodes.h"
#include "compositor/depth.h"
#include "compositor/modeedit.h"
#include "compositor/parallel.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace compositor {

namespace {

using Table = std::array<float, 256>;

/// Straight 0..1 per stored level, read at `v` (a straight sample) from a 256-entry table (the renderer's lookup, so an
/// adjustment layer draws as it did before these kinds were ported).
template <SampleType S>
inline float lookup(const Table& table, uint32_t v) {
    if constexpr (S == SampleType::U8) return table[std::min<uint32_t>(v, 255)];
    else {
        const float x = float(std::min<uint32_t>(v, one16)) * 255.0f / float(one16);
        const int i = std::min(254, int(x));
        return table[size_t(i)] + (table[size_t(i) + 1] - table[size_t(i)]) * (x - float(i));
    }
}

template <class F>
Table tableOf(F&& f) {
    Table t;
    for (int i = 0; i < 256; i++) t[size_t(i)] = float(f(i));
    return t;
}

bool isIdentity(const Table& t) {
    for (int i = 0; i < 256; i++) if (std::fabs(t[size_t(i)] - i / 255.0f) > 1e-7f) return false;
    return true;
}

/// Per-channel tables over the colour samples; a null table leaves its channel alone.
template <SampleType S, class Img>
void applyTables(Img& image, int n, const std::array<const Table*, 4>& tables) {
    using T = SampleOf<S>;
    constexpr uint32_t one = SampleTraits<S>::one;
    const int c = n - 1;
    parallelRows(0, image.height(), [&](int ya, int yb) {
        for (int y = ya; y < yb; y++) {
            T* p = image.row(y);
            for (int x = 0; x < image.width(); x++, p += n) {
                const T a = p[c];
                if (a == 0) continue;
                const float unitA = float(a) / float(one);
                for (int k = 0; k < c; k++) {
                    if (!tables[size_t(k)]) continue;
                    const T straight = T(std::min<uint32_t>(one, (uint32_t(p[k]) * one + a / 2) / a));
                    const float v = lookup<S>(*tables[size_t(k)], straight);
                    p[k] = T(std::clamp(v * unitA * float(one) + 0.5f, 0.0f, float(a)));
                }
            }
        }
    });
}

/// Each pixel's straight colour samples (0..1, as stored: CMYK inverted, Lab offset) through `f(v, lightness)`,
/// premultiplied again; transparent pixels are left alone. With `lightness`, a CMYK row's L* (0..100) is read through
/// it first (a CMYKFloat to LabFloat transform); in Lab L* is the first sample.
template <SampleType S, class Img, class F>
void eachStraight(Img& image, int n, const ColorTransform* lightness, F&& f) {
    using T = SampleOf<S>;
    const int c = n - 1;
    parallelRows(0, image.height(), [&](int ya, int yb) {
        std::vector<float> inks, lab;
        if (lightness) { inks.resize(size_t(image.width()) * 4); lab.resize(size_t(image.width()) * 3); }
        double v[4] = {0, 0, 0, 0};
        for (int y = ya; y < yb; y++) {
            T* row = image.row(y);
            if (lightness) {
                for (int x = 0; x < image.width(); x++) {
                    const T* q = row + size_t(x) * size_t(n);
                    const uint32_t a = q[c];
                    for (int k = 0; k < 4; k++) inks[size_t(x) * 4 + size_t(k)] = a ? float(100.0 * (1.0 - std::min(1.0, double(q[k]) / a))) : 0.0f;
                }
                lightness->apply(inks.data(), lab.data(), size_t(image.width()));
            }
            for (int x = 0; x < image.width(); x++) {
                T* q = row + size_t(x) * size_t(n);
                const uint32_t a = q[c];
                if (!a) continue;
                const double inverse = 1.0 / a;
                for (int k = 0; k < c; k++) v[k] = std::min(1.0, q[k] * inverse);
                const double l = lightness ? double(lab[size_t(x) * 3]) : v[0] * 100;
                f(v, l);
                for (int k = 0; k < c; k++) q[k] = T(std::clamp(std::lround(std::clamp(v[k], 0.0, 1.0) * a), 0L, long(a)));
            }
        }
    });
}

/// An sRGB colour (straight 0..1) in the document's mode as straight stored samples (0..1): CMYK inverted ink through
/// the profile with its black generation, Lab offset L, a, b. False when the profile cannot be used.
template <SampleType S>
bool nativeColour(double r, double g, double b, ColorMode mode, const ColorProfile& profile, double out[4]) {
    const ColorTransformPtr t = transformBetween(ColorProfile(), profile, ConvertOptions(), PixelFormat::RGBFloat,
                                                 mode == ColorMode::CMYK ? PixelFormat::CMYKFloat : PixelFormat::LabFloat);
    if (!t) return false;
    const float in[3] = {float(std::clamp(r, 0.0, 1.0)), float(std::clamp(g, 0.0, 1.0)), float(std::clamp(b, 0.0, 1.0))};
    float v[4] = {0, 0, 0, 0};
    t->apply(in, v, 1);
    if (mode == ColorMode::CMYK) {
        for (int k = 0; k < 4; k++) out[k] = 1 - std::clamp(double(v[k]) / 100.0, 0.0, 1.0);
        return true;
    }
    const double one = double(SampleTraits<S>::one);
    out[0] = std::clamp(double(v[0]) / 100.0, 0.0, 1.0);
    for (int k = 1; k < 3; k++) out[k] = std::clamp((double(v[k]) * labScale<S>() + labOffset<S>()) / one, 0.0, 1.0);
    out[3] = 0;
    return true;
}

/// Signed a or b from a straight stored Lab sample (0..1), and back.
template <SampleType S> double labValue(double stored) { return (stored * double(SampleTraits<S>::one) - labOffset<S>()) / labScale<S>(); }
template <SampleType S> double labStored(double value) { return (value * labScale<S>() + labOffset<S>()) / double(SampleTraits<S>::one); }

// CIE L* and relative luminance.
double luminanceOf(double l) {
    const double fy = (l + 16) / 116, e = 6.0 / 29;
    return fy > e ? fy * fy * fy : 3 * e * e * (fy - 4.0 / 29);
}
double lightnessOf(double y) {
    const double e = 6.0 / 29;
    const double f = y > e * e * e ? std::cbrt(y) : y / (3 * e * e) + 4.0 / 29;
    return 116 * f - 16;
}

double luma(double r, double g, double b) { return 0.299 * r + 0.587 * g + 0.114 * b; }

template <SampleType S, ColorMode M, class Img>
bool applyInMode(const AdjustmentSettings& s, Img& image, const ColorProfile& profile) {
    constexpr int N = colorModeChannels(M), C = N - 1;
    constexpr bool cmyk = M == ColorMode::CMYK;
    if (!adjustmentAppliesInMode(s.kind, M)) return false;
    if constexpr (requires { image.channels(); }) if (image.channels() != N) return false;
    std::array<Table, 4> tables;
    std::array<const Table*, 4> use{nullptr, nullptr, nullptr, nullptr};
    auto useTable = [&](int k, const Table& t) { tables[size_t(k)] = t; if (!isIdentity(t)) use[size_t(k)] = &tables[size_t(k)]; };
    ColorTransformPtr lightness;
    if (cmyk && (s.kind == AdjustmentKind::Threshold || s.kind == AdjustmentKind::GradientMap)) {
        lightness = transformBetween(profile, ColorProfile(), ConvertOptions(), PixelFormat::CMYKFloat, PixelFormat::LabFloat);
        if (!lightness) return false;
    }
    switch (s.kind) {
    case AdjustmentKind::Levels:
        for (int k = 0; k < C; k++) {
            if (cmyk) useTable(k, tableOf([&](int i) { return s.levels.apply(i / 255.0, k + 1); }));
            else if (k == 0) useTable(k, tableOf([&](int i) { return s.levels.apply(i / 255.0, 1); }));
            else useTable(k, tableOf([&](int i) { return s.levels.ranges[size_t(k) + 1].apply(i / 255.0); }));
        }
        break;
    case AdjustmentKind::Curves:
        if (!s.curves.isValid()) return true;
        for (int k = 0; k < C; k++) {
            if (cmyk || k == 0) useTable(k, tableOf([&](int i) { return s.curves.value(s.curves.value(i, k + 1), 0) / 255; }));
            else useTable(k, tableOf([&](int i) { return s.curves.value(i, k + 1) / 255; }));
        }
        break;
    case AdjustmentKind::BrightnessContrast: {
        const Table t = brightnessContrastTransfer(s.brightnessContrast)[0];
        for (int k = 0; k < (cmyk ? C : 1); k++) useTable(k, t);
        break;
    }
    case AdjustmentKind::Invert: for (int k = 0; k < C; k++) useTable(k, invertTransfer()[0]); break;
    case AdjustmentKind::Posterize: {
        const Table t = posterizeTransfer(s.posterize)[0];
        for (int k = 0; k < C; k++) useTable(k, t);
        break;
    }
    case AdjustmentKind::Exposure: {
        // Lab: L through relative luminance, the RGB kernel's exposure, offset and gamma on linear light.
        const ExposureSettings e = s.exposure.normalized();
        const double gain = std::pow(2.0, e.exposure);
        useTable(0, tableOf([&](int i) {
            const double y = std::pow(std::max(0.0, luminanceOf(i / 255.0 * 100) * gain + e.offset), 1 / e.gamma);
            return std::clamp(lightnessOf(y) / 100, 0.0, 1.0);
        }));
        break;
    }
    case AdjustmentKind::Threshold: {
        // Photoshop's level on the lightness; black the profile's black (CMYK) or L 0 (Lab), white no ink or L 100.
        double black[4] = {0, 0, 0, 0}, white[4] = {1, 1, 1, 1};
        if (cmyk) { if (!nativeColour<S>(0, 0, 0, M, profile, black)) return false; }
        else { black[1] = black[2] = white[1] = white[2] = labStored<S>(0); }
        const double level = std::clamp(s.threshold.level, 1, 255) - 0.005;
        eachStraight<S>(image, N, lightness.get(), [&](double* v, double l) {
            const double* to = l * 2.55 >= level ? white : black;
            for (int k = 0; k < C; k++) v[k] = to[k];
        });
        return true;
    }
    case AdjustmentKind::GradientMap: {
        const GradientMapSettings& g = s.gradientMap;
        const AdjustmentColor dark = (g.reversed ? g.highlights : g.shadows).clamped(), light = (g.reversed ? g.shadows : g.highlights).clamped();
        double from[4], to[4];
        if (!nativeColour<S>(dark.red, dark.green, dark.blue, M, profile, from) || !nativeColour<S>(light.red, light.green, light.blue, M, profile, to)) return false;
        if (g.method != GradientMethod::Classic) {
            // Perceptual and Linear: the method's ramp at 257 even steps in the mode's own values, blended between them.
            constexpr int steps = 256;
            const GradientStops ramp = g.ramp();
            std::vector<std::array<double, 4>> native(steps + 1);
            for (int j = 0; j <= steps; j++) {
                float c[4];
                ramp.sample(float(j) / steps, c);
                if (!nativeColour<S>(c[0], c[1], c[2], M, profile, native[size_t(j)].data())) return false;
            }
            eachStraight<S>(image, N, lightness.get(), [&](double* v, double l) {
                const double at = std::clamp(l / 100, 0.0, 1.0) * steps;
                const int j = std::min(steps - 1, int(at));
                const double f = at - j;
                for (int k = 0; k < C; k++) v[k] = native[size_t(j)][size_t(k)] + (native[size_t(j + 1)][size_t(k)] - native[size_t(j)][size_t(k)]) * f;
            });
            return true;
        }
        eachStraight<S>(image, N, lightness.get(), [&](double* v, double l) {
            const double t = std::clamp(l / 100, 0.0, 1.0);
            for (int k = 0; k < C; k++) v[k] = from[k] + (to[k] - from[k]) * t;
        });
        return true;
    }
    case AdjustmentKind::HueSaturation: {
        if constexpr (!cmyk) return false;
        if (s.hsv.isIdentity()) return true;
        const auto adjust = s.hsv.adjuster();
        eachStraight<S>(image, N, nullptr, [&](double* v, double) { adjust(v[0], v[1], v[2]); });
        return true;
    }
    case AdjustmentKind::ColorBalance:
        if constexpr (cmyk) { applyColorBalanceStoredCmy(image, s.colorBalance); return true; }
        else return false;
    case AdjustmentKind::PhotoFilter: {
        const PhotoFilterSettings& f = s.photoFilter;
        const double d = std::clamp(f.density, 0.0, 100.0) / 100;
        const AdjustmentColor colour = f.color.clamped();
        double filter[4];
        if (!nativeColour<S>(colour.red, colour.green, colour.blue, M, profile, filter)) return false;
        if constexpr (cmyk) {
            // Each stored ink multiplied by the filter's, mixed in by the density (the RGB kernel's lens on each channel).
            eachStraight<S>(image, N, nullptr, [&](double* v, double) {
                const double before = luma(v[0], v[1], v[2]) * v[3];
                for (int k = 0; k < 4; k++) v[k] = std::min(1.0, v[k] * (1 - d) + v[k] * filter[k] * 2 * d);
                if (f.preserveLuminosity) {
                    const double after = luma(v[0], v[1], v[2]) * v[3];
                    if (after > 0.0001) for (int k = 0; k < 3; k++) v[k] = std::min(1.0, v[k] * before / after);
                }
            });
        } else {
            // The filter's a and b added in by the density; L multiplied as the RGB kernel multiplies (the filter's
            // lightness against mid-grey) unless Preserve Luminosity keeps it.
            const double fl = filter[0], fa = labValue<S>(filter[1]), fb = labValue<S>(filter[2]);
            eachStraight<S>(image, N, nullptr, [&](double* v, double) {
                if (!f.preserveLuminosity) v[0] = std::min(1.0, v[0] * (1 - d) + v[0] * fl * 2 * d);
                v[1] = labStored<S>(labValue<S>(v[1]) + fa * d);
                v[2] = labStored<S>(labValue<S>(v[2]) + fb * d);
            });
        }
        return true;
    }
    case AdjustmentKind::ChannelMixer: {
        if constexpr (!cmyk) return false;
        const ChannelMixerSettings& m = s.channelMixer;
        std::array<std::array<double, 5>, 4> w;
        for (size_t o = 0; o < 4; o++) for (size_t i = 0; i < 5; i++) w[o][i] = std::clamp(m.inks[o][i], -200.0, 200.0) / 100;
        eachStraight<S>(image, N, nullptr, [&](double* v, double) {
            const double ink[4] = {1 - v[0], 1 - v[1], 1 - v[2], 1 - v[3]};
            auto mix = [&](const std::array<double, 5>& r) { return r[0] * ink[0] + r[1] * ink[1] + r[2] * ink[2] + r[3] * ink[3] + r[4]; };
            if (m.monochrome) {
                const double k = mix(w[3]);
                v[0] = v[1] = v[2] = 1;
                v[3] = 1 - std::clamp(k, 0.0, 1.0);
                return;
            }
            double out[4];
            for (size_t o = 0; o < 4; o++) out[o] = mix(w[o]);
            for (int k = 0; k < 4; k++) v[k] = 1 - std::clamp(out[k], 0.0, 1.0);
        });
        return true;
    }
    case AdjustmentKind::SelectiveColor: {
        if constexpr (!cmyk) return false;
        // The RGB kernel's colour ranges, decided on the stored colour darkened by the black plate; each range's
        // cyan, magenta and yellow move those inks and its black the black plate.
        std::array<std::array<double, 4>, 9> adj;
        for (size_t i = 0; i < 9; i++) for (size_t k = 0; k < 4; k++) adj[i][k] = std::clamp(s.selectiveColor.ranges[i][k], -100.0, 100.0) / 100;
        const bool absolute = s.selectiveColor.absolute;
        eachStraight<S>(image, N, nullptr, [&](double* v, double) {
            const double r = v[0] * v[3], g = v[1] * v[3], b = v[2] * v[3];
            const double mx = std::max({r, g, b}), mn = std::min({r, g, b}), md = r + g + b - mx - mn;
            double wt[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
            const int primary = mx == r ? 0 : mx == g ? 2 : 4;
            int secondary;
            if (primary == 0) secondary = g >= b ? 1 : 5;
            else if (primary == 2) secondary = r >= b ? 1 : 3;
            else secondary = g >= r ? 3 : 5;
            wt[primary] = mx - md;
            wt[secondary] = md - mn;
            wt[6] = std::max(0.0, (mn - 0.5) * 2);
            wt[8] = std::max(0.0, (0.5 - mx) * 2);
            wt[7] = std::max(0.0, 1 - (std::fabs(mx - 0.5) + std::fabs(mn - 0.5)));
            double ink[4] = {1 - v[0], 1 - v[1], 1 - v[2], 1 - v[3]}, delta[4] = {0, 0, 0, 0};
            for (size_t range = 0; range < 9; range++) {
                if (wt[range] <= 0) continue;
                for (size_t k = 0; k < 4; k++) delta[k] += wt[range] * adj[range][k] * (absolute ? 1.0 : ink[k]);
            }
            for (int k = 0; k < 4; k++) v[k] = 1 - std::clamp(ink[k] + delta[k], 0.0, 1.0);
        });
        return true;
    }
    default: return false;
    }
    if (std::none_of(use.begin(), use.end(), [](const Table* t) { return t != nullptr; })) return true;
    applyTables<S>(image, N, use);
    return true;
}

} // namespace

bool adjustmentAppliesInMode(AdjustmentKind kind, ColorMode mode) {
    if (mode == ColorMode::RGB) return true;
    return adjustmentOfferedInMode(kind, mode) && kind != AdjustmentKind::ColorLookup;
}

bool applyAdjustmentMode(const AdjustmentSettings& settings, ImageC8& cmyk, const ColorProfile& profile) {
    return applyInMode<SampleType::U8, ColorMode::CMYK>(settings, cmyk, profile);
}

bool applyAdjustmentLab(const AdjustmentSettings& settings, Image& lab) {
    return applyInMode<SampleType::U8, ColorMode::Lab>(settings, lab, ColorProfile());
}

bool applyAdjustmentMode(const AdjustmentSettings& settings, Image16& image, ColorMode mode, const ColorProfile& profile) {
    if (mode == ColorMode::CMYK) return applyInMode<SampleType::U16, ColorMode::CMYK>(settings, image, profile);
    if (mode == ColorMode::Lab) return applyInMode<SampleType::U16, ColorMode::Lab>(settings, image, profile);
    return false;
}

std::array<std::vector<double>, 5> levelsHistogramInMode(const AnyImage& image, ColorMode mode, const AnyGray& coverage) {
    std::array<std::vector<double>, 5> bins;
    if (!image || mode == ColorMode::RGB || image.channels() != colorModeChannels(mode)) return bins;
    const int n = image.channels(), colours = n - 1;
    const bool cmyk = mode == ColorMode::CMYK;
    for (int c = 0; c < 5; c++) if (cmyk || (c >= 1 && c <= 3)) bins[size_t(c)].assign(256, 0.0);
    const bool covered = coverage && coverage.width() == image.width() && coverage.height() == image.height();
    auto count = [&](const auto& typed) {
        using T = std::remove_cvref_t<decltype(*typed.row(0))>;
        for (int y = 0; y < typed.height(); y++) {
            const T* p = typed.row(y);
            for (int x = 0; x < typed.width(); x++, p += n) {
                const double a = p[colours];
                if (a <= 0) continue;
                double weight = 1;
                if (covered && coverage.u8()) weight = coverage.u8()->at(x, y) / 255.0;
                else if (covered && coverage.u16()) weight = coverage.u16()->at(x, y) / double(one16);
                if (weight <= 0) continue;
                double sum = 0;
                for (int k = 0; k < colours; k++) {
                    const int level = std::clamp(int(std::lround(std::min(1.0, p[k] / a) * 255)), 0, 255);
                    bins[size_t(k) + 1][size_t(level)] += weight;
                    sum += level;
                }
                if (cmyk) bins[0][size_t(std::clamp(int(std::lround(sum / colours)), 0, 255))] += weight;
            }
        }
    };
    if (image.c8()) count(*image.c8());
    else if (image.u8()) count(*image.u8());
    else if (image.u16()) count(*image.u16());
    return bins;
}

AnyImage adjustedInMode(const AdjustmentSettings& settings, const AnyImage& image, ColorMode mode, const ColorProfile& profile) {
    if (!image || mode == ColorMode::RGB || image.channels() != colorModeChannels(mode)) return {};
    if (const auto& c8 = image.c8(); c8 && mode == ColorMode::CMYK) {
        auto out = std::make_shared<ImageC8>(*c8);
        return applyAdjustmentMode(settings, *out, profile) ? AnyImage(ImageC8Ptr(out)) : AnyImage();
    }
    if (const auto& u8 = image.u8(); u8 && mode == ColorMode::Lab) {
        auto out = std::make_shared<Image>(*u8);
        return applyAdjustmentLab(settings, *out) ? AnyImage(ImagePtr(out)) : AnyImage();
    }
    if (const auto& u16 = image.u16()) {
        auto out = std::make_shared<Image16>(*u16);
        return applyAdjustmentMode(settings, *out, mode, profile) ? AnyImage(Image16Ptr(out)) : AnyImage();
    }
    return {};
}

} // namespace compositor
