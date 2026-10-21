// The adjustments on a CMYK or Lab document's own samples (modeedit.h, docs/color-modes.md "Adjustments and filters").
//
// What each kind changes, following Photoshop's per-mode menus (adjustmentOfferedInMode):
// - Levels and Curves: per channel, on the stored values as Photoshop's channel histograms show them (a CMYK plate
//   dark where the ink is); CMYK's composite goes on every ink after the ink's own, Lab has no composite (its first
//   slot goes with lightness, as older files hold it) and a and b take their own. Brightness/Contrast on every ink in
//   CMYK and on L in Lab; Invert and Posterize on every channel.
// - Exposure (Lab): on L, through relative luminance, as the RGB kernel works on linear light.
// - Threshold: on L in Lab; in CMYK on the Rec. 601 luma of the inks' plain complement, to paper or black ink alone.
// - Hue/Saturation and Color Balance (CMYK): the RGB kernels' maths on the stored cyan, magenta and yellow (the
//   complements of the inks) as red, green and blue; black is kept.
// - Photo Filter: Lab scales the colour's X, Y and Z by the filter's (Preserve Luminosity keeps L), as Photoshop does;
//   CMYK runs the RGB kernel on the stored cyan, magenta and yellow, black kept (an approximation).
// - Gradient Map: the stops in the document's own values, read on L (Lab) or the inks' plain luminosity (CMYK).
// - Channel Mixer (CMYK): four ink rows over the four inks and a constant (ChannelMixerSettings::inks).
// - Selective Color (CMYK, its home): each colour range's cyan, magenta, yellow and black move those inks, black on
//   the black plate.
// Gradient Map and Lab's Photo Filter are checked against photocraft-corpus; the others are the published formulas adapted
// to each mode.
#include "nekophoto/colormgmt.h"
#include "nekophoto/colormodes.h"
#include "nekophoto/depth.h"
#include "nekophoto/modeedit.h"
#include "nekophoto/parallel.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace nekophoto {

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

/// Each pixel's colour as straight sRGB (0..1) through `f`, converted from the document's mode through the profile and
/// back, a row at a time; transparent pixels are left alone. False when the profile cannot be used.
template <SampleType S, ColorMode M, class Img, class F>
bool eachViaRgb(Img& image, const ColorProfile& profile, F&& f) {
    using T = SampleOf<S>;
    constexpr int N = colorModeChannels(M), C = N - 1;
    constexpr bool cmyk = M == ColorMode::CMYK;
    const PixelFormat native = cmyk ? PixelFormat::CMYKFloat : PixelFormat::LabFloat;
    const ColorTransformPtr in = transformBetween(profile, ColorProfile(), ConvertOptions(), native, PixelFormat::RGBFloat);
    const ColorTransformPtr out = transformBetween(ColorProfile(), profile, ConvertOptions(), PixelFormat::RGBFloat, native);
    if (!in || !out) return false;
    parallelRows(0, image.height(), [&](int ya, int yb) {
        const size_t w = size_t(image.width());
        constexpr size_t stride = cmyk ? 4 : 3;   // CMYKFloat holds four floats, LabFloat three
        std::vector<float> values(w * stride), rgb(w * 3);
        for (int y = ya; y < yb; y++) {
            T* row = image.row(y);
            for (size_t x = 0; x < w; x++) {
                const T* q = row + x * size_t(N);
                const double a = q[C];
                float* v = values.data() + x * stride;
                double st[4] = {0, 0, 0, 0};
                for (int k = 0; k < C; k++) st[k] = a > 0 ? std::min(1.0, q[k] / a) : (cmyk ? 1.0 : 0.0);
                if (cmyk) for (int k = 0; k < 4; k++) v[k] = float(100 * (1 - st[k]));
                else { v[0] = float(st[0] * 100); v[1] = float(labValue<S>(st[1])); v[2] = float(labValue<S>(st[2])); }
            }
            in->apply(values.data(), rgb.data(), w);
            for (size_t x = 0; x < w; x++) {
                double c[3] = {rgb[x * 3], rgb[x * 3 + 1], rgb[x * 3 + 2]};
                f(c[0], c[1], c[2]);
                for (int k = 0; k < 3; k++) rgb[x * 3 + size_t(k)] = float(std::clamp(c[k], 0.0, 1.0));
            }
            out->apply(rgb.data(), values.data(), w);
            for (size_t x = 0; x < w; x++) {
                T* q = row + x * size_t(N);
                const double a = q[C];
                if (!(a > 0)) continue;
                const float* v = values.data() + x * stride;
                double st[4];
                if (cmyk) for (int k = 0; k < 4; k++) st[k] = 1 - std::clamp(double(v[k]) / 100, 0.0, 1.0);
                else { st[0] = std::clamp(double(v[0]) / 100, 0.0, 1.0); st[1] = std::clamp(labStored<S>(v[1]), 0.0, 1.0); st[2] = std::clamp(labStored<S>(v[2]), 0.0, 1.0); }
                for (int k = 0; k < C; k++) q[k] = T(std::clamp(std::lround(st[k] * a), 0L, long(a)));
            }
        }
    });
    return true;
}

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


/// A Gradient Map stop in the document's own straight samples (0..1, as stored).
struct NativeStop { double location, midpoint; double v[4]; };

/// The map's stops in the mode's samples, Reverse applied: each the file's own CMYK or Lab colour while the stop keeps
/// the sRGB colour read with it, else its sRGB colour converted through the profile. A Perceptual or Linear map is its
/// method's ramp sampled at 257 even stops first. Empty when the profile cannot be used.
template <SampleType S>
std::vector<NativeStop> nativeStops(const GradientMapSettings& g, ColorMode mode, const ColorProfile& profile) {
    std::vector<NativeStop> out;
    const double one = double(SampleTraits<S>::one);
    if (g.method == GradientMethod::Classic && g.stops.size() >= 2) {
        const GradientStops ramp = [&] { GradientMapSettings plain = g; plain.reversed = false; plain.smoothness = 0; return plain.ramp(); }();
        std::vector<size_t> order(g.stops.size());
        for (size_t i = 0; i < order.size(); i++) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return g.stops[a].location < g.stops[b].location; });
        const bool native = g.nativeMode == mode && g.native.size() == g.stops.size();
        for (size_t j = 0; j < order.size(); j++) {
            const GradientColorStop& read = g.stops[order[j]];
            const GradientColorStop& now = ramp.colors[j];
            NativeStop stop{double(now.location), double(now.midpoint), {0, 0, 0, 0}};
            const bool kept = native && read.rgb[0] == now.rgb[0] && read.rgb[1] == now.rgb[1] && read.rgb[2] == now.rgb[2];
            if (kept) {
                const std::array<double, 4>& n = g.native[order[j]];
                if (mode == ColorMode::CMYK) for (int k = 0; k < 4; k++) stop.v[k] = 1 - std::clamp(n[size_t(k)], 0.0, 1.0);
                else {
                    stop.v[0] = std::clamp(n[0] / 100, 0.0, 1.0);
                    for (int k = 1; k < 3; k++) stop.v[k] = std::clamp((n[size_t(k)] * labScale<S>() + labOffset<S>()) / one, 0.0, 1.0);
                }
            } else if (!nativeColour<S>(now.rgb[0], now.rgb[1], now.rgb[2], mode, profile, stop.v)) return {};
            out.push_back(stop);
        }
    } else {
        // Two ends, or a Perceptual or Linear ramp sampled at 257 even stops.
        const GradientStops ramp = [&] { GradientMapSettings plain = g; plain.reversed = false; if (g.method == GradientMethod::Classic) plain.smoothness = 0; return plain.ramp(); }();
        const int steps = g.method == GradientMethod::Classic ? 1 : 256;
        for (int j = 0; j <= steps; j++) {
            float c[4];
            ramp.sample(float(j) / steps, c);
            NativeStop stop{double(j) / steps, 0.5, {0, 0, 0, 0}};
            if (!nativeColour<S>(c[0], c[1], c[2], mode, profile, stop.v)) return {};
            out.push_back(stop);
        }
    }
    if (g.reversed) {
        // Mirrored end for end: a stop's midpoint belongs to the run ending at it (GradientStops::reverse).
        std::reverse(out.begin(), out.end());
        for (auto& stop : out) stop.location = 1 - stop.location;
        for (size_t j = out.size(); j-- > 1;) out[j].midpoint = 1 - out[j - 1].midpoint;
        if (!out.empty()) out.front().midpoint = 0.5;
    }
    return out;
}

/// The stops' samples at `t` (0..1): each run blended after its midpoint, the ends held beyond the outer stops. With
/// `smoothness` (a Classic map's), the blend eases towards the Catmull-Rom through the neighbours, kept between the run's
/// two stops (GradientMapSettings::ramp).
void sampleNative(const std::vector<NativeStop>& stops, double t, double smoothness, double out[4]) {
    size_t i = 1;
    while (i < stops.size() && t > stops[i].location) i++;
    if (t <= stops.front().location || i >= stops.size()) {
        const NativeStop& end = t <= stops.front().location ? stops.front() : stops.back();
        for (int k = 0; k < 4; k++) out[k] = end.v[k];
        return;
    }
    const NativeStop &l = stops[i - 1], &r = stops[i];
    const NativeStop& p = i > 1 ? stops[i - 2] : l;
    const NativeStop& n = i + 1 < stops.size() ? stops[i + 1] : r;
    const double span = r.location - l.location;
    double u = span > 1e-9 ? (t - l.location) / span : 1;
    const double m = std::clamp(r.midpoint, 0.001, 0.999);
    if (std::fabs(m - 0.5) > 1e-6) u = u <= m ? 0.5 * u / m : 0.5 + 0.5 * (u - m) / (1 - m);
    const double u2 = u * u, u3 = u2 * u;
    for (int k = 0; k < 4; k++) {
        const double lin = l.v[k] + (r.v[k] - l.v[k]) * u;
        if (smoothness <= 0) { out[k] = lin; continue; }
        const double cr = 0.5 * ((2 * l.v[k]) + (-p.v[k] + r.v[k]) * u + (2 * p.v[k] - 5 * l.v[k] + 4 * r.v[k] - n.v[k]) * u2 + (-p.v[k] + 3 * l.v[k] - 3 * r.v[k] + n.v[k]) * u3);
        out[k] = lin + (std::clamp(cr, std::min(l.v[k], r.v[k]), std::max(l.v[k], r.v[k])) - lin) * smoothness;
    }
}

template <SampleType S, ColorMode M, class Img>
bool applyInMode(const AdjustmentSettings& s, Img& image, const ColorProfile& profile) {
    constexpr int N = colorModeChannels(M), C = N - 1;
    constexpr bool cmyk = M == ColorMode::CMYK;
    if (!adjustmentAppliesInMode(s.kind, M)) return false;
    if constexpr (requires { image.channels(); }) if (image.channels() != N) return false;
    if (s.kind == AdjustmentKind::ColorLookup) {
        // An ICC abstract profile or a device link of the document's colour space, through Little CMS on the document's
        // own samples (colorLookupAppliesInMode); 3DLUT tables are RGB and draw nothing here, as Photoshop offers them in
        // RGB only.
        if (s.colorLookup.format != "icc") return false;
        const auto bytes = fromBase64(s.colorLookup.data);
        const ColorTransformPtr t = bytes ? lookupTransform(profile, *bytes, pixelFormatFor(S, M)) : nullptr;
        if (!t) return false;
        convertImage(image, t.get());
        return true;
    }
    std::array<Table, 4> tables;
    std::array<const Table*, 4> use{nullptr, nullptr, nullptr, nullptr};
    auto useTable = [&](int k, const Table& t) { tables[size_t(k)] = t; if (!isIdentity(t)) use[size_t(k)] = &tables[size_t(k)]; };
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
        // Lab: on L, black L 0 and white L 100, a and b neutral. CMYK: Photoshop compares the Rec. 601 luma of the inks'
        // plain complement (stored cyan times stored black, and so on), whatever the profile, rounded to a level, and
        // makes paper or 100% black ink alone (PhotoCraft's ink_luma, crates/compose/src/adjust.rs at 04d0212, MIT OR
        // Apache-2.0, used under MIT; photocraft-corpus cmyk8 and cmyk16 threshold-128.psd).
        double black[4] = {1, 1, 1, 0}, white[4] = {1, 1, 1, 1};
        if (!cmyk) { black[0] = 0; black[1] = black[2] = white[1] = white[2] = labStored<S>(0); }
        const int level = std::clamp(s.threshold.level, 1, 255);
        eachStraight<S>(image, N, nullptr, [&](double* v, double l) {
            const double luma = cmyk ? (0.299 * v[0] + 0.587 * v[1] + 0.114 * v[2]) * v[3] : l / 100;
            const double* to = std::lround(luma * 255) >= level ? white : black;
            for (int k = 0; k < C; k++) v[k] = to[k];
        });
        return true;
    }
    case AdjustmentKind::GradientMap: {
        const GradientMapSettings& g = s.gradientMap;
        const std::vector<NativeStop> stops = nativeStops<S>(g, M, profile);
        if (stops.empty()) return false;
        const double smooth = g.method == GradientMethod::Classic ? std::clamp(g.smoothness, 0.0, 1.0) : 0.0;
        eachStraight<S>(image, N, nullptr, [&](double* v, double l) {
            // Lab: L. CMYK: the luminosity of the inks' plain complement (stored cyan times stored black, and so on),
            // whatever the profile (photocraft-corpus cmyk8 and cmyk16 gradient-map.psd: within a level).
            const double t = cmyk ? GradientMapSettings::luminosity(v[0] * v[3], v[1] * v[3], v[2] * v[3]) : l / 100;
            double out[4];
            sampleNative(stops, std::clamp(t, 0.0, 1.0), smooth, out);
            for (int k = 0; k < C; k++) v[k] = out[k];
        });
        return true;
    }
    case AdjustmentKind::HueSaturation: {
        if (s.hsv.isIdentity()) return true;
        const auto adjust = s.hsv.adjuster();
        // CMYK: on the stored cyan, magenta and yellow as red, green and blue, black kept; Colorize, which Photoshop
        // separates again (black and all), and Lab go through RGB and back.
        if (cmyk && !s.hsv.colorize) {
            eachStraight<S>(image, N, nullptr, [&](double* v, double) { adjust(v[0], v[1], v[2]); });
            return true;
        }
        return eachViaRgb<S, M>(image, profile, adjust);
    }
    case AdjustmentKind::ColorBalance:
        if constexpr (cmyk) { applyColorBalanceStoredCmy(image, s.colorBalance); return true; }
        else return false;
    case AdjustmentKind::PhotoFilter: {
        const PhotoFilterSettings& f = s.photoFilter;
        if constexpr (cmyk) {
            // The RGB kernel on the stored cyan, magenta and yellow, black kept (as Photoshop keeps it).
            applyPhotoFilterStoredCmy(image, f);
        } else {
            // Lab: the colour's D50 XYZ multiplied by the filter's, relative to white and mixed in by the density (the
            // diagonal of photoFilterMatrix); Preserve Luminosity keeps L. photocraft-corpus lab8 and lab16
            // photo-filter.psd: a and b within a quarter of a unit.
            const std::array<double, 3> scale = photoFilterScale(f);
            eachStraight<S>(image, N, nullptr, [&](double* v, double) {
                const double L = v[0] * 100, A = labValue<S>(v[1]), B = labValue<S>(v[2]);
                const double fy = (L + 16) / 116, fx = fy + A / 500, fz = fy - B / 200;
                auto inverse = [](double t) { return t > 6.0 / 29 ? t * t * t : 3 * (6.0 / 29) * (6.0 / 29) * (t - 4.0 / 29); };
                double xyz[3] = {inverse(fx) * scale[0], inverse(fy) * scale[1], inverse(fz) * scale[2]};
                auto forward = [](double t) { return t > 216.0 / 24389 ? std::cbrt(t) : t * 24389.0 / 3132 + 4.0 / 29; };
                const double gx = forward(xyz[0]), gy = forward(xyz[1]), gz = forward(xyz[2]);
                if (!f.preserveLuminosity) v[0] = std::clamp((116 * gy - 16) / 100, 0.0, 1.0);
                v[1] = std::clamp(labStored<S>(500 * (gx - gy)), 0.0, 1.0);
                v[2] = std::clamp(labStored<S>(200 * (gy - gz)), 0.0, 1.0);
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
    return adjustmentOfferedInMode(kind, mode);
}

bool colorLookupAppliesInMode(const ColorLookupSettings& settings, ColorMode mode, const ColorProfile& profile) {
    if (mode == ColorMode::RGB) return colorLookupReadable(settings);
    if (settings.format != "icc") return false;
    const auto bytes = fromBase64(settings.data);
    return bytes && lookupTransform(profile, *bytes, pixelFormatFor(SampleType::U8, mode)) != nullptr;
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

} // namespace nekophoto
