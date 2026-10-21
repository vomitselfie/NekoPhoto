#include "rawmeasured.h"
#include "nekophoto/rawtone.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <utility>

namespace nekophoto::blobs {
extern const unsigned char rawmakaseColorMixerChart[];
extern const unsigned long long rawmakaseColorMixerChartSize;
extern const unsigned char rawmakaseVibranceChart[];
extern const unsigned long long rawmakaseVibranceChartSize;
extern const unsigned char rawmakaseParametric[];
extern const unsigned long long rawmakaseParametricSize;
extern const unsigned char rawmakaseColorGradeCurves[];
extern const unsigned long long rawmakaseColorGradeCurvesSize;
} // namespace nekophoto::blobs

namespace nekophoto::rawtone::measured {

namespace {

// ---- The tables (little-endian, as RAWmakase writes them) ---------------------------------------------------------------

float int16At(const unsigned char* bytes, size_t index, float scale) {
    const auto v = int16_t(uint16_t(bytes[2 * index]) | uint16_t(uint16_t(bytes[2 * index + 1]) << 8));
    return float(v) * scale;
}

float float32At(const unsigned char* bytes, size_t index) {
    const unsigned char* b = bytes + 4 * index;
    return std::bit_cast<float>(uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24);
}

// The colour mixer's grid: 36 hues x 6 saturations (spaced by the square root) x 6 values (spaced by v^0.45).
constexpr int kHues = 36, kSats = 6, kVals = 6;
constexpr size_t kCells = size_t(kHues * kSats * kVals);
constexpr size_t kTable = 3 * kCells;
// color_mixer_chart.bin: 8 bands x (hue, saturation, luminance) x (-, +), then Saturation -/+ and Vibrance -/+ (the ±50
// photo tables), at 1/8000 per step.
constexpr size_t kMixerTables = 52;
constexpr float kMixerScale = 1.0f / 8000;
constexpr size_t kSaturationTable = 48, kVibranceTable = 50;

float mixerValue(size_t table, size_t channel, size_t cell) {
    return int16At(blobs::rawmakaseColorMixerChart, table * kTable + channel * kCells + cell, kMixerScale);
}

// vibrance_chart.bin: a grid per measured Vibrance position.
constexpr std::array<double, 8> kVibranceKnots{-1, -0.75, -0.5, -0.25, 0.25, 0.5, 0.75, 1};

float vibranceValue(size_t knot, size_t channel, size_t cell) {
    return int16At(blobs::rawmakaseVibranceChart, knot * kTable + channel * kCells + cell, kMixerScale);
}

bool tablesPresent() {
    return blobs::rawmakaseColorMixerChartSize == kMixerTables * kTable * 2 && blobs::rawmakaseVibranceChartSize == kVibranceKnots.size() * kTable * 2;
}

/// Rec. 709 Y of the colour's linear sRGB: RAWmakase's luminance.
double srgbFromProPhotoLuminance(const double p[3]) {
    const Rgb s = srgbFromProPhoto({p[0], p[1], p[2]});
    return 0.2126 * s[0] + 0.7152 * s[1] + 0.0722 * s[2];
}

/// Trilinear lookup of the grid between cell centres; hue wraps.
std::array<double, 3> lookup(const Grid& grid, double h, double s, double v) {
    const double fh = (h - std::floor(h)) * kHues - 0.5;
    const double fs = std::clamp(std::sqrt(std::clamp(s, 0.0, 1.0)) * kSats - 0.5, 0.0, double(kSats - 1));
    const double fv = std::clamp(std::pow(std::clamp(v, 0.0, 1.0), 0.45) * kVals - 0.5, 0.0, double(kVals - 1));
    const double h0 = std::floor(fh);
    const double th = fh - h0, ts = fs - std::floor(fs), tv = fv - std::floor(fv);
    auto hueAt = [&](int d) { return ((int(h0) + d) % kHues + kHues) % kHues; };
    const int s0 = int(fs), v0 = int(fv);
    const int s1 = std::min(s0 + 1, kSats - 1), v1 = std::min(v0 + 1, kVals - 1);
    std::array<double, 3> out{0, 0, 0};
    for (const auto& [dh, wh] : {std::pair{0, 1 - th}, std::pair{1, th}})
        for (const auto& [si, ws] : {std::pair{s0, 1 - ts}, std::pair{s1, ts}})
            for (const auto& [vi, wv] : {std::pair{v0, 1 - tv}, std::pair{v1, tv}}) {
                const auto& d = grid[size_t((hueAt(dh) * kSats + si) * kVals + vi)];
                const double w = wh * ws * wv;
                for (size_t c = 0; c < 3; c++) out[c] += double(d[c]) * w;
            }
    // A grey has no hue: below the first saturation row's centre the change fades to none at saturation 0, so a nearly grey
    // colour does not take a hue's brightness change from noise in its channels (the ±50 Vibrance tables, measured on
    // photos, move their lowest row by up to 6% depending on the hue). RAWmakase holds the row's value down to 0.
    const double fade = std::min(1.0, std::sqrt(std::clamp(s, 0.0, 1.0)) * kSats * 2);
    for (double& v : out) v *= fade;
    return out;
}

/// How much of a band slider's measured change at ±100 applies at `s`: Luminance is not linear in the slider.
double strength(size_t kind, double s) {
    const double a = std::min(std::fabs(s), 1.0);
    return kind == 2 ? std::pow(a, s < 0 ? 1.6 : 0.79) : a;
}

/// The summed change of the tables `active` (index, strength) names. Negative saturation tables (even indices) scale
/// the saturation factor itself, which is how Camera Raw's -25 and -50 behave.
Grid sumGrid(const std::vector<std::pair<size_t, double>>& active) {
    Grid grid(kCells, {0, 0, 0});
    for (size_t cell = 0; cell < kCells; cell++)
        for (size_t c = 0; c < 3; c++) {
            double sum = 0;
            for (const auto& [t, w] : active) {
                const double v = mixerValue(t, c, cell);
                sum += c == 1 && t % 2 == 0 ? std::log2(std::max(1e-3, 1 + w * (std::exp2(v) - 1))) : v * w;
            }
            grid[cell][c] = float(sum);
        }
    return grid;
}

/// Vibrance `v` (beyond ±50) from the chart's grids, linear between the measured positions.
void addVibrance(Grid& grid, double v) {
    v = std::clamp(v, -1.0, 1.0);
    std::vector<std::pair<size_t, double>> knots;
    const size_t i = size_t(std::partition_point(kVibranceKnots.begin(), kVibranceKnots.end(), [&](double k) { return k < v; }) - kVibranceKnots.begin());
    if (i == 0) knots = {{0, 1}};
    else if (i == kVibranceKnots.size()) knots = {{i - 1, 1}};
    else if (kVibranceKnots[i - 1] < 0 && kVibranceKnots[i] > 0) knots = {v < 0 ? std::pair{i - 1, v / kVibranceKnots[i - 1]} : std::pair{i, v / kVibranceKnots[i]}};
    else {
        const double t = (v - kVibranceKnots[i - 1]) / (kVibranceKnots[i] - kVibranceKnots[i - 1]);
        knots = {{i - 1, 1 - t}, {i, t}};
    }
    for (const auto& [knot, w] : knots) {
        if (w == 0) continue;
        for (size_t cell = 0; cell < kCells; cell++)
            for (size_t c = 0; c < 3; c++) grid[cell][c] += float(w * vibranceValue(knot, c, cell));
    }
}

} // namespace

void applyGrid(const Grid& grid, double rgb[3]) {
    const double p[3] = {std::max(0.0, rgb[0]), std::max(0.0, rgb[1]), std::max(0.0, rgb[2])};
    const double hi = std::max({p[0], p[1], p[2]}), lo = std::min({p[0], p[1], p[2]});
    if (!(hi > 1e-6)) return;
    const double d = hi - lo;
    double h = 0;
    if (d >= 1e-9) {
        if (hi == p[0]) h = std::fmod((p[1] - p[2]) / d + 6, 6.0) / 6;
        else if (hi == p[1]) h = ((p[2] - p[0]) / d + 2) / 6;
        else h = ((p[0] - p[1]) / d + 4) / 6;
    }
    const double s = d / hi;
    const auto delta = lookup(grid, h, s, hi);
    double hue = h + delta[0];
    hue = (hue - std::floor(hue)) * 6;
    const double sat = std::clamp(s * std::exp2(delta[1]), 0.0, 1.0);
    const double v = hi * std::exp2(delta[2]);
    const double c = v * sat;
    const double x = c * (1 - std::fabs(std::fmod(hue, 2.0) - 1));
    double q[3];
    switch (int(hue)) {
    case 0: q[0] = c, q[1] = x, q[2] = 0; break;
    case 1: q[0] = x, q[1] = c, q[2] = 0; break;
    case 2: q[0] = 0, q[1] = c, q[2] = x; break;
    case 3: q[0] = 0, q[1] = x, q[2] = c; break;
    case 4: q[0] = x, q[1] = 0, q[2] = c; break;
    default: q[0] = c, q[1] = 0, q[2] = x; break;
    }
    for (int k = 0; k < 3; k++) rgb[k] = q[k] + v - c;
}

// ---- Colour mixer, Saturation and Vibrance ------------------------------------------------------------------------------

ColorMixer::ColorMixer(const std::array<std::array<double, 8>, 3>& bands, double saturation, double vibrance, bool useBands, bool useSaturation,
                       bool useVibrance) {
    if (!tablesPresent()) return;
    std::vector<std::pair<size_t, double>> active;
    if (useBands)
        for (size_t band = 0; band < 8; band++)
            for (size_t kind = 0; kind < 3; kind++)
                if (const double s = bands[kind][band]; s != 0) active.push_back({(band * 3 + kind) * 2 + (s > 0 ? 1 : 0), strength(kind, s)});
    // Saturation scales the measured ±50 tables down to -50, and fades to gray below.
    constexpr double grayFrom = -0.5;
    const double sat = useSaturation ? std::max(saturation, grayFrom) : 0;
    if (sat != 0) active.push_back({kSaturationTable + (sat > 0 ? 1 : 0), std::min(std::fabs(sat) * 2, 2.0)});
    // Up to ±50 Vibrance is the photo tables, scaled; beyond, the chart's grids.
    const bool chartVibrance = useVibrance && std::fabs(vibrance) > 0.5;
    if (useVibrance && !chartVibrance && vibrance != 0) active.push_back({kVibranceTable + (vibrance > 0 ? 1 : 0), std::min(std::fabs(vibrance) * 2, 2.0)});
    saturationGray_ = useSaturation ? std::clamp((grayFrom - saturation) / (1 + grayFrom), 0.0, 1.0) : 0;
    if (active.empty() && !chartVibrance) return;
    active_ = true;
    delta_ = sumGrid(active);
    if (chartVibrance) addVibrance(delta_, vibrance);
    if (saturationGray_ > 0) {
        // While colours fade to gray, the other controls still set its brightness.
        std::vector<std::pair<size_t, double>> others;
        for (const auto& item : active)
            if (item.first != kSaturationTable && item.first != kSaturationTable + 1) others.push_back(item);
        if (!others.empty() || chartVibrance) {
            graySource_ = sumGrid(others);
            if (chartVibrance) addVibrance(*graySource_, vibrance);
        }
    }
}

void ColorMixer::apply(double rgb[3]) const {
    if (!active_) return;
    const double source[3] = {rgb[0], rgb[1], rgb[2]};
    applyGrid(delta_, rgb);
    if (saturationGray_ == 0) return;
    // Camera Raw's -100 keeps the luminance the colour has without Saturation.
    double from[3] = {source[0], source[1], source[2]};
    if (graySource_) applyGrid(*graySource_, from);
    const double y = srgbFromProPhotoLuminance(from);
    for (int k = 0; k < 3; k++) rgb[k] += saturationGray_ * (y - rgb[k]);
}

// ---- Parametric curve ---------------------------------------------------------------------------------------------------

namespace {

// parametric.bin: changes to the identity at 65 points over 0..1 of gamma-2.2 encoding, f32: Shadows (over 0 to the
// midtone split, by amount and by the shadow split as a share of the midtone split), Highlights (likewise above the
// midtone split), Darks and Lights (by amount and midtone split), and Darks with Lights at the default splits.
constexpr size_t kCurveSamples = 65;
constexpr std::array<double, 8> kAmounts{-1, -0.75, -0.5, -0.25, 0.25, 0.5, 0.75, 1};
constexpr std::array<double, 5> kRatios{0.2, 0.36, 0.5, 0.64, 0.8};
constexpr std::array<double, 7> kMidtones{0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8};
constexpr std::array<double, 6> kJoint{-1, -0.5, -0.25, 0.25, 0.5, 1};
constexpr size_t kLow = 0, kHigh = kLow + kAmounts.size() * kRatios.size(), kDarks = kHigh + kAmounts.size() * kRatios.size(),
                 kLights = kDarks + kAmounts.size() * kMidtones.size(), kBoth = kLights + kAmounts.size() * kMidtones.size(),
                 kCurveTables = kBoth + kJoint.size() * kJoint.size();
constexpr size_t kDefaultMidtone = 3;
constexpr double kCurveGamma = 2.2;

using Curve = std::array<double, kCurveSamples>;

Curve curveTable(size_t i) {
    Curve t{};
    for (size_t k = 0; k < kCurveSamples; k++) t[k] = float32At(blobs::rawmakaseParametric, i * kCurveSamples + k);
    return t;
}

Curve blend(const Curve& a, double wa, const Curve& b, double wb) {
    Curve t{};
    for (size_t k = 0; k < kCurveSamples; k++) t[k] = a[k] * wa + b[k] * wb;
    return t;
}

/// The bracketing pair of `v` in `values` (clamped to their range) and the upper one's weight.
template <typename Values> std::pair<size_t, double> bracket(const Values& values, double v) {
    v = std::clamp(v, double(values.front()), double(values.back()));
    size_t j = values.size() - 2;
    for (size_t i = 0; i + 1 < values.size(); i++)
        if (v <= values[i + 1]) { j = i; break; }
    return {j, (v - values[j]) / (values[j + 1] - values[j])};
}

/// A region's table at `amount`, linear between the measured positions with no change at 0; `column` gives the table at
/// one measured amount (its index in kAmounts).
template <typename F> Curve atAmount(double amount, F&& column) {
    static constexpr std::array<double, 9> positions{-1, -0.75, -0.5, -0.25, 0, 0.25, 0.5, 0.75, 1};
    const auto [j, w] = bracket(positions, amount);
    auto get = [&](size_t p) { return p == 4 ? Curve{} : column(p < 4 ? p : p - 1); };
    return blend(get(j), 1 - w, get(j + 1), w);
}

template <typename Splits> Curve tableAt(size_t first, const Splits& splits, double amount, double split) {
    const auto [j, w] = bracket(splits, split);
    return atAmount(amount, [&](size_t a) {
        const size_t row = first + a * splits.size();
        return blend(curveTable(row + j), 1 - w, curveTable(row + j + 1), w);
    });
}

/// Darks and Lights together at the default splits: bilinear over the joint grid, whose axes are the single regions.
Curve joint(double darks, double lights) {
    static constexpr std::array<double, 7> axis{-1, -0.5, -0.25, 0, 0.25, 0.5, 1};
    auto node = [&](size_t d, size_t l) -> Curve {
        if (d == 3 && l == 3) return Curve{};
        if (d == 3) return tableAt(kLights, kMidtones, axis[l], kMidtones[kDefaultMidtone]);
        if (l == 3) return tableAt(kDarks, kMidtones, axis[d], kMidtones[kDefaultMidtone]);
        auto i = [](size_t v) { return v > 3 ? v - 1 : v; };
        return curveTable(kBoth + i(d) * kJoint.size() + i(l));
    };
    const auto [jd, wd] = bracket(axis, std::clamp(darks, -1.0, 1.0));
    const auto [jl, wl] = bracket(axis, std::clamp(lights, -1.0, 1.0));
    Curve t{};
    const Curve a = node(jd, jl), b = node(jd + 1, jl), c = node(jd, jl + 1), d = node(jd + 1, jl + 1);
    for (size_t k = 0; k < kCurveSamples; k++)
        t[k] = a[k] * (1 - wd) * (1 - wl) + b[k] * wd * (1 - wl) + c[k] * (1 - wd) * wl + d[k] * wd * wl;
    return t;
}

double curveLookup(const Curve& t, double x) {
    const double f = std::clamp(x, 0.0, 1.0) * double(kCurveSamples - 1);
    const size_t i = std::min(size_t(f), kCurveSamples - 2);
    return t[i] + (t[i + 1] - t[i]) * (f - double(i));
}

double srgbDecode(double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }
double srgbEncode(double l) { return l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1 / 2.4) - 0.055; }

} // namespace

std::vector<float> parametricCurve(const std::array<double, 4>& regions, const std::array<double, 3>& splits, int size) {
    if (regions == std::array<double, 4>{0, 0, 0, 0} || blobs::rawmakaseParametricSize != kCurveTables * kCurveSamples * 4 || size < 1) return {};
    const auto [shadows, darks, lights, highlights] = regions;
    const auto [low, midtone, high] = splits;
    std::optional<Curve> shadowTable, highlightTable, middle;
    if (shadows != 0) shadowTable = tableAt(kLow, kRatios, shadows, low / midtone);
    if (highlights != 0) highlightTable = tableAt(kHigh, kRatios, highlights, (high - midtone) / (1 - midtone));
    if (darks != 0 && lights != 0) {
        // Measured together at the default midtone split; a moved split changes each region as it does alone.
        const double standard = kMidtones[kDefaultMidtone];
        Curve t = joint(darks, lights);
        const Curve d = tableAt(kDarks, kMidtones, darks, midtone), d0 = tableAt(kDarks, kMidtones, darks, standard);
        const Curve l = tableAt(kLights, kMidtones, lights, midtone), l0 = tableAt(kLights, kMidtones, lights, standard);
        for (size_t k = 0; k < kCurveSamples; k++) t[k] += d[k] - d0[k] + l[k] - l0[k];
        middle = t;
    } else if (darks != 0) middle = tableAt(kDarks, kMidtones, darks, midtone);
    else if (lights != 0) middle = tableAt(kLights, kMidtones, lights, midtone);
    std::vector<float> out(size_t(size) + 1);
    float highest = 0;
    for (size_t i = 0; i < out.size(); i++) {
        const double w = std::pow(srgbDecode(double(i) / size), 1 / kCurveGamma);
        double y = w;
        if (shadowTable && w < midtone) y = midtone * (w / midtone + curveLookup(*shadowTable, w / midtone));
        else if (highlightTable && w >= midtone) {
            const double u = (w - midtone) / (1 - midtone);
            y = midtone + (1 - midtone) * (u + curveLookup(*highlightTable, u));
        }
        if (middle) {
            y = std::clamp(y, 0.0, 1.0);
            y += curveLookup(*middle, y);
        }
        // Measured tables carry small non-monotone noise; tone must never invert.
        highest = std::max(highest, float(srgbEncode(std::pow(std::clamp(y, 0.0, 1.0), kCurveGamma))));
        out[i] = highest;
    }
    return out;
}

// ---- Colour Grading -----------------------------------------------------------------------------------------------------

namespace {

// color_grade_curves.bin, f32, curves sampled at 64 points of x^(1/2.2): each tonal region's two profiles over Blending x
// Balance, Global's three, each region's per-channel coefficients over 12 hues x saturation 25..100, and each region's
// Luminance gains at -100..+100 (without 0).
constexpr size_t kGradeSamples = 64;
constexpr std::array<double, 5> kBlends{0, 0.25, 0.5, 0.75, 1};
constexpr std::array<double, 17> kBalances{-1, -0.88, -0.75, -0.62, -0.5, -0.38, -0.25, -0.12, 0, 0.12, 0.25, 0.38, 0.5, 0.62, 0.75, 0.88, 1};
constexpr size_t kGradeHues = 12;
constexpr std::array<double, 5> kSaturations{0, 0.25, 0.5, 0.75, 1};
constexpr std::array<double, 9> kLuminances{-1, -0.75, -0.5, -0.25, 0, 0.25, 0.5, 0.75, 1};
constexpr size_t kTerms = 3, kShapedTerms = 2;
constexpr size_t kGrid = 0, kGlobal = kGrid + 3 * kBlends.size() * kBalances.size() * kGradeSamples * kShapedTerms,
                 kCoef = kGlobal + kGradeSamples * kTerms, kLum = kCoef + 4 * kGradeHues * (kSaturations.size() - 1) * 3 * kTerms,
                 kGradeLength = kLum + 4 * (kLuminances.size() - 1) * kGradeSamples;
constexpr size_t kGlobalRegion = 3;

using Samples = std::array<double, kGradeSamples>;
using Profiles = std::array<std::array<double, kTerms>, kGradeSamples>;

double gradeValue(size_t i) { return float32At(blobs::rawmakaseColorGradeCurves, i); }

Profiles profiles(size_t region, double blending, double balance) {
    Profiles out{};
    if (region == kGlobalRegion) {
        for (size_t k = 0; k < kGradeSamples; k++)
            for (size_t t = 0; t < kTerms; t++) out[k][t] = gradeValue(kGlobal + k * kTerms + t);
        return out;
    }
    const auto [i, u] = bracket(kBlends, blending);
    const auto [j, v] = bracket(kBalances, balance);
    auto at = [&](size_t b, size_t a, size_t k, size_t t) {
        const size_t cell = ((region * kBlends.size() + b) * kBalances.size() + a) * kGradeSamples + k;
        return gradeValue(kGrid + cell * kShapedTerms + t);
    };
    for (size_t k = 0; k < kGradeSamples; k++)
        for (size_t t = 0; t < kShapedTerms; t++)
            out[k][t] = at(i, j, k, t) * (1 - u) * (1 - v) + at(i + 1, j, k, t) * u * (1 - v) + at(i, j + 1, k, t) * (1 - u) * v
                      + at(i + 1, j + 1, k, t) * u * v;
    return out;
}

std::array<std::array<double, kTerms>, 3> coefficients(size_t region, double hue, double saturation) {
    const double f = (hue - std::floor(hue)) * kGradeHues;
    const size_t h0 = size_t(f) % kGradeHues;
    const double w = f - std::floor(f);
    const auto [s, u] = bracket(kSaturations, saturation);
    auto at = [&](size_t h, size_t si, size_t c, size_t t) {
        if (si == 0) return 0.0;   // Saturation 0 has no tint; the table starts at 25
        return gradeValue(kCoef + (((region * kGradeHues + h) * (kSaturations.size() - 1) + si - 1) * 3 + c) * kTerms + t);
    };
    std::array<std::array<double, kTerms>, 3> out{};
    for (size_t c = 0; c < 3; c++)
        for (size_t t = 0; t < kTerms; t++) {
            auto hueAt = [&](size_t si) { return at(h0, si, c, t) * (1 - w) + at((h0 + 1) % kGradeHues, si, c, t) * w; };
            out[c][t] = hueAt(s) * (1 - u) + hueAt(s + 1) * u;
        }
    return out;
}

/// log2 of the measured Luminance gains at `amount`, Blending 50 and Balance 0.
Samples luminanceLog(size_t region, double amount) {
    const auto [j, u] = bracket(kLuminances, amount);
    auto table = [&](size_t i) {
        Samples out{};
        if (i == 4) return out;
        const size_t row = i < 4 ? i : i - 1;
        const size_t at = kLum + (region * (kLuminances.size() - 1) + row) * kGradeSamples;
        for (size_t k = 0; k < kGradeSamples; k++) out[k] = std::log2(gradeValue(at + k));
        return out;
    };
    const Samples a = table(j), b = table(j + 1);
    Samples out{};
    for (size_t k = 0; k < kGradeSamples; k++) out[k] = a[k] * (1 - u) + b[k] * u;
    return out;
}

/// The region's first profile scaled to peak at 1: where it sits along the channel.
Samples regionWeight(size_t region, double blending, double balance) {
    const Profiles p = profiles(region, blending, balance);
    double peak = 0;
    for (const auto& t : p)
        if (std::fabs(t[0]) > std::fabs(peak)) peak = t[0];
    Samples out{};
    for (size_t k = 0; k < kGradeSamples; k++) out[k] = peak == 0 ? 0 : p[k][0] / peak;
    return out;
}

size_t argmax(const Samples& v) {
    size_t m = 0;
    for (size_t i = 0; i < v.size(); i++)
        if (v[i] > v[m]) m = i;
    return m;
}

double sampleAt(const Samples& table, double at) {
    at = std::clamp(at, 0.0, double(kGradeSamples - 1));
    const size_t i = std::min(size_t(at), kGradeSamples - 2);
    const double t = at - double(i);
    return table[i] * (1 - t) + table[i + 1] * t;
}

double encodedAt(size_t k) { return double(k) / double(kGradeSamples - 1); }

/// The Luminance gains at `amount`, Blending and Balance: Luminance keeps the strength it has at the default where the
/// region's weight is the same, on the same side of the weight's peak. Darkening keeps its gain there, lifting its
/// encoded offset.
Samples luminanceGains(size_t region, double amount, double blending, double balance) {
    const Samples log = luminanceLog(region, amount);
    Samples out{};
    if (region == kGlobalRegion || (std::fabs(blending - 0.5) < 1e-4 && std::fabs(balance) < 1e-4)) {
        for (size_t k = 0; k < kGradeSamples; k++) out[k] = std::exp2(log[k]);
        return out;
    }
    const Samples w = regionWeight(region, blending, balance), w0 = regionWeight(region, 0.5, 0);
    const size_t peak = argmax(w), peak0 = argmax(w0);
    Samples at{};
    for (size_t k = 0; k < kGradeSamples; k++) {
        std::vector<size_t> seg;
        if (k <= peak) for (size_t i = 0; i <= peak0; i++) seg.push_back(i);
        else for (size_t i = peak0; i < kGradeSamples; i++) seg.push_back(i);
        size_t nearest = 0;
        for (size_t a = 1; a < seg.size(); a++)
            if (std::fabs(w0[seg[a]] - w[k]) < std::fabs(w0[seg[nearest]] - w[k])) nearest = a;
        at[k] = double(seg[nearest]);
        for (const auto& [a, b] : {std::pair{nearest - 1, nearest}, std::pair{nearest, nearest + 1}}) {
            if (a < seg.size() && b < seg.size()) {   // nearest - 1 wraps past the size at 0
                const double lo = w0[seg[a]], hi = w0[seg[b]];
                if ((lo - w[k]) * (hi - w[k]) <= 0 && lo != hi) {
                    const double t = (w[k] - lo) / (hi - lo);
                    at[k] = double(seg[a]) + t * (double(seg[b]) - double(seg[a]));
                    break;
                }
            }
        }
    }
    if (amount < 0) {
        for (size_t k = 0; k < kGradeSamples; k++) out[k] = std::exp2(sampleAt(log, at[k]));
        return out;
    }
    Samples offset{};
    for (size_t k = 0; k < kGradeSamples; k++) offset[k] = encodedAt(k) * (std::exp2(log[k] / 2.2) - 1);
    out[0] = std::exp2(log[0]);
    for (size_t k = 1; k < kGradeSamples; k++) out[k] = std::pow((encodedAt(k) + sampleAt(offset, at[k])) / encodedAt(k), 2.2);
    return out;
}

} // namespace

GradeCurves::GradeCurves(const std::array<std::array<double, 3>, 4>& wheels, double blending, double balance) {
    if (blobs::rawmakaseColorGradeCurvesSize != kGradeLength * 4) return;
    if (std::all_of(wheels.begin(), wheels.end(), [](const auto& w) { return w[1] == 0 && w[2] == 0; })) return;
    Samples lum;
    lum.fill(1);
    std::array<Samples, 3> tint;
    for (auto& t : tint) t.fill(1);
    for (size_t region = 0; region < 4; region++) {
        const auto [hue, saturation, amount] = wheels[region];
        if (amount != 0) {
            const Samples l = luminanceGains(region, amount, blending, balance);
            for (size_t k = 0; k < kGradeSamples; k++) lum[k] *= l[k];
        }
        if (saturation != 0) {
            const Profiles p = profiles(region, blending, balance);
            const auto c = coefficients(region, hue, saturation);
            for (size_t k = 0; k < kGradeSamples; k++)
                for (size_t ch = 0; ch < 3; ch++) {
                    double sum = 0;
                    for (size_t t = 0; t < kTerms; t++) sum += p[k][t] * c[ch][t];
                    tint[ch][k] *= 1 + sum;
                }
        }
    }
    // Luminance first, then the tint on its result.
    gain_.resize(kGradeSamples);
    for (size_t k = 0; k < kGradeSamples; k++) {
        const double after = std::pow(std::clamp(std::pow(encodedAt(k), 2.2) * lum[k], 0.0, 1.0), 1 / 2.2);
        for (size_t c = 0; c < 3; c++) gain_[k][c] = float(lum[k] * std::max(0.0, sampleAt(tint[c], after * double(kGradeSamples - 1))));
    }
    active_ = true;
}

void GradeCurves::apply(double rgb[3]) const {
    if (!active_) return;
    for (size_t c = 0; c < 3; c++) {
        const double v = rgb[c];
        const double at = std::pow(std::clamp(v, 0.0, 1.0), 1 / 2.2) * double(kGradeSamples - 1);
        const size_t i = std::min(size_t(at), kGradeSamples - 2);
        const double t = at - double(i);
        rgb[c] = v * (gain_[i][c] * (1 - t) + gain_[i + 1][c] * t);
    }
}

} // namespace nekophoto::rawtone::measured
