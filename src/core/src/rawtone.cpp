#include "compositor/rawtone.h"
#include "compositor/cameraraw.h"
#include <algorithm>
#include <cmath>
#include <iterator>

namespace compositor::rawtone {

namespace {

enum class ToneSlider { Whites, Blacks, Highlights, Shadows, Dehaze, Contrast, ParametricShadows, ParametricDarks, ParametricLights, ParametricHighlights };

#include "rawtone_tables.inc"

constexpr int kCurveKnots = int(std::size(kCurveLog2));
constexpr int kGridKnots = int(std::size(kSliderGrid));

// LibRaw's linear sRGB -> ProPhoto table (its output matrix for ProPhoto RGB), and the inverse.
constexpr double kToProPhoto[3][3] = {{0.529317, 0.330092, 0.140588}, {0.098368, 0.873465, 0.028169}, {0.016879, 0.117663, 0.865457}};
struct Inverse {
    double m[3][3];
    Inverse() {
        const auto& a = kToProPhoto;
        const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
                         + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
        m[0][0] = (a[1][1] * a[2][2] - a[1][2] * a[2][1]) / det;
        m[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / det;
        m[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det;
        m[1][0] = (a[1][2] * a[2][0] - a[1][0] * a[2][2]) / det;
        m[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / det;
        m[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det;
        m[2][0] = (a[1][0] * a[2][1] - a[1][1] * a[2][0]) / det;
        m[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / det;
        m[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det;
    }
};
const Inverse& toSrgb() { static const Inverse inverse; return inverse; }

// ProPhoto RGB's luminance (its Y row, D50).
constexpr double kProPhotoY[3] = {0.2880402, 0.7118741, 0.0000857};

double srgbDecode(double v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }
double srgbEncode(double l) { return l <= 0.0031308 ? l * 12.92 : 1.055 * std::pow(l, 1 / 2.4) - 0.055; }

/// Fritsch-Carlson monotone cubic through the knots, as tools/camera_raw_fit.py evaluates it.
class Pchip {
public:
    Pchip(const double* x, const double* y, int n) : x_(x, x + n), y_(y, y + n), m_(size_t(n), 0) {
        std::vector<double> d(size_t(n - 1));
        for (int i = 0; i + 1 < n; i++) d[size_t(i)] = (y[i + 1] - y[i]) / (x[i + 1] - x[i]);
        for (int i = 1; i + 1 < n; i++)
            m_[size_t(i)] = d[size_t(i - 1)] * d[size_t(i)] > 0 ? 2 / (1 / d[size_t(i - 1)] + 1 / d[size_t(i)]) : 0;
        m_[0] = d[0];
        m_[size_t(n - 1)] = d[size_t(n - 2)];
    }
    double operator()(double v) const {
        const size_t n = x_.size();
        size_t i = size_t(std::upper_bound(x_.begin(), x_.end(), v) - x_.begin());
        i = std::clamp<size_t>(i, 1, n - 1) - 1;
        const double h = x_[i + 1] - x_[i];
        const double t = std::clamp((v - x_[i]) / h, 0.0, 1.0);
        const double t2 = t * t, t3 = t2 * t;
        return (2 * t3 - 3 * t2 + 1) * y_[i] + (t3 - 2 * t2 + t) * h * m_[i] + (-2 * t3 + 3 * t2) * y_[i + 1] + (t3 - t2) * h * m_[i + 1];
    }

private:
    std::vector<double> x_, y_, m_;
};

const Pchip& defaultCurve() {
    static const Pchip curve(kCurveLog2, kCurveEncoded, kCurveKnots);
    return curve;
}

/// The default curve's display light for scene light `s`: the fitted knots, proportional below the darkest one, and
/// held at the raw white point's value above 1.
double curveDisplay(double s) {
    if (!(s > 0)) return 0;
    const double floor = std::exp2(kCurveLog2[0]);
    if (s < floor) return srgbDecode(kCurveEncoded[0]) * s / floor;
    return srgbDecode(std::clamp(defaultCurve()(std::min(std::log2(s), 0.0)), 0.0, 1.0));
}

/// inverseToneCurve's table: scene light at display light (i / 4096)^2.
const std::vector<double>& inverseTable() {
    static const std::vector<double> table = [] {
        std::vector<double> t(4097);
        const double top = curveDisplay(1);
        for (size_t i = 0; i < t.size(); i++) {
            const double target = std::pow(double(i) / 4096, 2);
            if (target >= top) { t[i] = 1; continue; }
            double lo = 0, hi = 1;
            for (int k = 0; k < 60; k++) {
                const double mid = (lo + hi) / 2;
                (curveDisplay(mid) < target ? lo : hi) = mid;
            }
            t[i] = (lo + hi) / 2;
        }
        return t;
    }();
    return table;
}

double sqrtIndexed(const std::vector<float>& table, double v) {
    const double x = std::sqrt(std::clamp(v, 0.0, 1.0)) * double(table.size() - 1);
    const size_t i = std::min(size_t(x), table.size() - 2);
    const double t = x - double(i);
    return table[i] + (table[i + 1] - table[i]) * t;
}

double linearIndexed(const std::vector<float>& table, double v) {
    const double x = std::clamp(v, 0.0, 1.0) * double(table.size() - 1);
    const size_t i = std::min(size_t(x), table.size() - 2);
    const double t = x - double(i);
    return table[i] + (table[i + 1] - table[i]) * t;
}

const std::vector<float>& encodeTable() {
    static const std::vector<float> table = [] {
        std::vector<float> t(4097);
        for (size_t i = 0; i < t.size(); i++) t[i] = float(srgbEncode(std::pow(double(i) / 4096, 2)));
        return t;
    }();
    return table;
}

const std::vector<float>& decodeTable() {
    static const std::vector<float> table = [] {
        std::vector<float> t(4097);
        for (size_t i = 0; i < t.size(); i++) t[i] = float(srgbDecode(double(i) / 4096));
        return t;
    }();
    return table;
}

/// The brightest and darkest channel through `f`, the middle one kept at its place between them.
template <typename F> void hueKeeping(double c[3], F&& f) {
    const int hi = c[0] >= c[1] ? (c[0] >= c[2] ? 0 : 2) : (c[1] >= c[2] ? 1 : 2);
    const int lo = c[0] < c[1] ? (c[0] < c[2] ? 0 : 2) : (c[1] < c[2] ? 1 : 2);
    if (!(c[hi] > c[lo])) {   // grey
        const double v = f(c[0]);
        c[0] = c[1] = c[2] = v;
        return;
    }
    const int mid = 3 - hi - lo;
    const double t = (c[mid] - c[lo]) / (c[hi] - c[lo]);
    const double h = f(c[hi]), l = f(c[lo]);
    c[hi] = h;
    c[lo] = l;
    c[mid] = l + (h - l) * t;
}

/// The hue of an RGB colour in degrees (0 for a grey).
double hslHue(const double e[3]) {
    const double hi = std::max({e[0], e[1], e[2]}), chroma = hi - std::min({e[0], e[1], e[2]});
    if (!(chroma > 1e-9)) return 0;
    if (hi == e[0]) return std::fmod((e[1] - e[2]) / chroma + 6, 6.0) * 60;
    if (hi == e[1]) return ((e[2] - e[0]) / chroma + 2) * 60;
    return ((e[0] - e[1]) / chroma + 4) * 60;
}

void toHsl(const double e[3], double& h, double& s, double& l) {
    const double hi = std::max({e[0], e[1], e[2]}), lo = std::min({e[0], e[1], e[2]}), d = hi - lo;
    l = (hi + lo) / 2;
    s = d < 1e-9 ? 0 : d / std::max(1e-9, 1 - std::fabs(2 * l - 1));
    h = hslHue(e);
}

void fromHsl(double h, double s, double l, double out[3]) {
    const double c = (1 - std::fabs(2 * l - 1)) * s;
    const double hp = std::fmod(std::fmod(h, 360.0) + 360, 360.0) / 60;
    const double x = c * (1 - std::fabs(std::fmod(hp, 2.0) - 1));
    double r = 0, g = 0, b = 0;
    if (hp < 1) { r = c; g = x; }
    else if (hp < 2) { r = x; g = c; }
    else if (hp < 3) { g = c; b = x; }
    else if (hp < 4) { g = x; b = c; }
    else if (hp < 5) { r = x; b = c; }
    else { r = c; b = x; }
    const double m = l - c / 2;
    out[0] = r + m;
    out[1] = g + m;
    out[2] = b + m;
}

/// Each colour mixer band's share of a hue: linear between neighbouring band centres, so the shares add up to 1.
void mixerWeights(double h, double w[8]) {
    const auto& centres = CameraRawMixerSettings::centers;
    for (int b = 0; b < 8; b++) w[b] = 0;
    for (int b = 0; b < 8; b++) {
        const double from = centres[size_t(b)], to = b == 7 ? 360 : centres[size_t(b + 1)];
        const double offset = std::fmod(h - from + 360, 360.0);
        if (offset < to - from) {
            const double t = offset / (to - from);
            w[b] += 1 - t;
            w[(b + 1) % 8] += t;
            return;
        }
    }
}

/// A Colour Grading table at encoded luminance `luma` (linear between the ramp's steps, held past its ends).
double gradeAt(const double* table, double luma) {
    const int n = int(std::size(kGradeLuma));
    if (luma <= kGradeLuma[0]) return table[0];
    if (luma >= kGradeLuma[n - 1]) return table[n - 1];
    const int i = int(std::upper_bound(kGradeLuma, kGradeLuma + n, luma) - kGradeLuma) - 1;
    const double t = (luma - kGradeLuma[i]) / (kGradeLuma[i + 1] - kGradeLuma[i]);
    return table[i] + (table[i + 1] - table[i]) * t;
}

/// How much a wheel (0 shadows, 1 midtones, 2 highlights, 3 global) pushes per unit of saturation at `luma`. Blending moves
/// the shadow and highlight weights toward those measured at 100, Balance toward those at +50 (both linearly).
double gradeWeight(int region, double luma, double blending, double balance) {
    static const double* const base[4] = {kGradeShadows, kGradeMidtones, kGradeHighlights, kGradeGlobal};
    const double w = gradeAt(base[region], luma);
    if (region == 1 || region == 3) return w;
    const double* blended = region == 0 ? kGradeShadowsBlending100 : kGradeHighlightsBlending100;
    const double* balanced = region == 0 ? kGradeShadowsBalance50 : kGradeHighlightsBalance50;
    double out = w + (blending - 50) / 50 * (gradeAt(blended, luma) - w) + balance / 50 * (gradeAt(balanced, luma) - w);
    if (blending < 50) out = std::max(0.0, out);
    return out;
}

/// The grey a wheel's Luminance slider (-100..100) adds at `luma`.
double gradeLuminance(int region, double luma, double amount) {
    static const double* const darker[4] = {kGradeShadowsDarker, kGradeMidtonesDarker, kGradeHighlightsDarker, kGradeGlobalDarker};
    static const double* const brighter[4] = {kGradeShadowsBrighter, kGradeMidtonesBrighter, kGradeHighlightsBrighter, kGradeGlobalBrighter};
    return amount > 0 ? gradeAt(brighter[region], luma) * amount / 100 : gradeAt(darker[region], luma) * -amount / 100;
}

/// One slider's curve at `amount`: the measured curves on either side of it (0 is no change), mixed linearly.
std::vector<double> sliderCurve(ToneSlider slider, double amount) {
    std::vector<double> out(kSliderGrid, kSliderGrid + kGridKnots);
    if (amount == 0) return out;
    const SliderTable* below = nullptr;
    const SliderTable* above = nullptr;
    for (const SliderTable& table : kSliderTables) {
        if (table.slider != slider || (table.amount > 0) != (amount > 0)) continue;
        if (std::fabs(table.amount) <= std::fabs(amount) && (!below || std::fabs(table.amount) > std::fabs(below->amount))) below = &table;
        if (std::fabs(table.amount) >= std::fabs(amount) && (!above || std::fabs(table.amount) < std::fabs(above->amount))) above = &table;
    }
    if (!below && !above) return out;
    const double fromAmount = below ? below->amount : 0;
    if (!above) above = below;   // past the strongest measured: hold it
    const double span = above->amount - fromAmount;
    const double t = span == 0 ? 1 : (amount - fromAmount) / span;
    for (int k = 0; k < kGridKnots; k++) {
        const double from = below ? below->out[k] : kSliderGrid[k];
        out[size_t(k)] = from + (above->out[k] - from) * t;
    }
    return out;
}

/// A curve on encoded values as a table of 4097 entries (`index` 0..1), made from a slider's knots.
struct Composer {
    std::vector<double> values;
    Composer() : values(4097) { for (size_t i = 0; i < values.size(); i++) values[i] = double(i) / 4096; }
    void then(ToneSlider slider, double amount) {
        if (amount == 0) return;
        const std::vector<double> knots = sliderCurve(slider, amount);
        const Pchip curve(kSliderGrid, knots.data(), kGridKnots);
        for (double& v : values) v = std::clamp(curve(v), 0.0, 1.0);
    }
    /// A parametric region measured at the default splits (25, 50, 75), moved to `splits` by warping its input.
    void thenRegion(ToneSlider slider, double amount, const std::array<double, 3>& splits) {
        if (amount == 0) return;
        const std::vector<double> knots = sliderCurve(slider, amount);
        const Pchip curve(kSliderGrid, knots.data(), kGridKnots);
        const double from[5] = {0, splits[0], splits[1], splits[2], 1}, to[5] = {0, 0.25, 0.5, 0.75, 1};
        auto warp = [&](double v) {
            for (int k = 0; k < 4; k++)
                if (v <= from[k + 1] || k == 3) return to[k] + (to[k + 1] - to[k]) * (v - from[k]) / std::max(1e-6, from[k + 1] - from[k]);
            return v;
        };
        for (double& v : values) {
            const double w = std::clamp(warp(v), 0.0, 1.0);
            v = std::clamp(v + curve(w) - w, 0.0, 1.0);
        }
    }
    /// The parametric curve: Shadows then Darks, Highlights then Lights, which is how Camera Raw's regions compose.
    void thenParametric(const CameraRawCurveSettings& c) {
        const std::array<double, 3> splits{c.shadowSplit / 100, c.darkSplit / 100, c.lightSplit / 100};
        thenRegion(ToneSlider::ParametricShadows, c.shadows, splits);
        thenRegion(ToneSlider::ParametricHighlights, c.highlights, splits);
        thenRegion(ToneSlider::ParametricDarks, c.darks, splits);
        thenRegion(ToneSlider::ParametricLights, c.lights, splits);
    }
    std::vector<float> table() const { return std::vector<float>(values.begin(), values.end()); }
};

} // namespace

void rescaleScene(double c[3], double gain) {
    const auto& display = Developer::shared().display;
    hueKeeping(c, [&](double v) {
        const double scene = std::clamp(inverseToneCurve(v) * gain, 0.0, 1.0) * 65535;
        const size_t i = std::min(size_t(scene), size_t(65534));
        return display[i] + (display[i + 1] - display[i]) * (scene - double(i));
    });
}

double vignetteMask(double x, double y, double width, double height, double midpoint, double feather, double roundness) {
    // Distance from the middle: an ellipse with the frame's proportions (1 at the corners), turning into a circle in
    // pixels as Roundness goes to +100 and shrinking (a weaker vignette) as it goes below 0.
    const double nx = (x - width / 2) / (width / 2), ny = (y - height / 2) / (height / 2);
    const double ellipse = std::hypot(nx, ny) / std::sqrt(2.0);
    const double circle = std::hypot(x - width / 2, y - height / 2) / std::hypot(width / 2, height / 2);
    const double round = std::clamp(roundness, -100.0, 100.0) / 100;
    const double d = round >= 0 ? ellipse * (1 - round) + circle * round : ellipse * (1 + kVignetteShrink * round);
    const double start = kVignetteStart[0] + kVignetteStart[1] * midpoint / 100;
    const double span = std::max(0.02, kVignetteSpan[0] + kVignetteSpan[1] * feather / 100);
    const double t = std::clamp((d - start) / span, 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

void vignette(float rgb[3], double mask, double amount, int style, double highlights) {
    if (!(mask > 0) || amount == 0) return;
    const double a = amount / 100;
    if (a > 0) {
        const double k = style == 2 ? kVignettePaintLighten : kVignetteLighten;
        for (int i = 0; i < 3; i++) rgb[i] = float(rgb[i] + (1 - rgb[i]) * k * a * mask);
        return;
    }
    if (style == 2) {
        for (int i = 0; i < 3; i++) rgb[i] = float(rgb[i] * (1 + a * mask));
        return;
    }
    const auto& decode = decodeTable();
    const auto& encode = encodeTable();
    const Rgb p = proPhotoFromSrgb({linearIndexed(decode, rgb[0]), linearIndexed(decode, rgb[1]), linearIndexed(decode, rgb[2])});
    double c[3] = {std::max(0.0, p[0]), std::max(0.0, p[1]), std::max(0.0, p[2])};
    const double bright = std::clamp((std::max({c[0], c[1], c[2]}) - 0.2) / 0.8, 0.0, 1.0);
    rescaleScene(c, std::exp2(kVignetteStops * a * mask * (1 - kVignetteProtect * highlights / 100 * bright)));
    const auto& m = toSrgb().m;
    for (int r = 0; r < 3; r++) rgb[r] = float(sqrtIndexed(encode, m[r][0] * c[0] + m[r][1] * c[1] + m[r][2] * c[2]));
}

double lensVignetteGain(double x, double y, double width, double height, double amount, double midpoint) {
    if (amount == 0) return 1;
    const double r = std::hypot(x - width / 2, y - height / 2) / std::hypot(width / 2, height / 2);
    const double power = std::clamp(kLensVignettePower[0] + (midpoint - 20) / 30 * (kLensVignettePower[1] - kLensVignettePower[0]), 1.0, 12.0);
    return std::exp2(amount / 100 * kLensVignetteStrength[amount > 0 ? 1 : 0] * std::pow(r, power));
}

std::vector<float> parametricTable(const CameraRawCurveSettings& curve) {
    Composer composer;
    composer.thenParametric(curve.normalized());
    return composer.table();
}

Rgb proPhotoFromSrgb(const Rgb& c) {
    Rgb out{};
    for (int r = 0; r < 3; r++) out[size_t(r)] = kToProPhoto[r][0] * c[0] + kToProPhoto[r][1] * c[1] + kToProPhoto[r][2] * c[2];
    return out;
}

Rgb srgbFromProPhoto(const Rgb& c) {
    const auto& m = toSrgb().m;
    Rgb out{};
    for (int r = 0; r < 3; r++) out[size_t(r)] = m[r][0] * c[0] + m[r][1] * c[1] + m[r][2] * c[2];
    return out;
}

double toneCurve(double scene) { return curveDisplay(scene); }

double inverseToneCurve(double display) {
    const auto& t = inverseTable();
    const double x = std::sqrt(std::clamp(display, 0.0, 1.0)) * 4096;
    const size_t i = std::min(size_t(x), size_t(4095));
    return t[i] + (t[i + 1] - t[i]) * (x - double(i));
}

Rgb develop(const Rgb& scene) {
    double c[3] = {std::max(0.0, scene[0]), std::max(0.0, scene[1]), std::max(0.0, scene[2])};
    hueKeeping(c, [](double v) { return curveDisplay(v); });
    const Rgb s = srgbFromProPhoto({c[0], c[1], c[2]});
    return {srgbEncode(std::clamp(s[0], 0.0, 1.0)), srgbEncode(std::clamp(s[1], 0.0, 1.0)), srgbEncode(std::clamp(s[2], 0.0, 1.0))};
}

Rgb undevelop(const Rgb& encoded) {
    const Rgb p = proPhotoFromSrgb({srgbDecode(encoded[0]), srgbDecode(encoded[1]), srgbDecode(encoded[2])});
    double c[3] = {std::max(0.0, p[0]), std::max(0.0, p[1]), std::max(0.0, p[2])};
    hueKeeping(c, [](double v) { return inverseToneCurve(v); });
    return {c[0], c[1], c[2]};
}

Developer::Developer(double scale) : display(65536), encode(encodeTable()) {
    for (size_t i = 0; i < display.size(); i++) display[i] = float(curveDisplay(double(i) / 65535 * scale));
}

const Developer& Developer::shared() {
    static const Developer developer;
    return developer;
}

void Developer::develop(const uint16_t in[3], float out[3]) const {
    double c[3] = {double(in[0]), double(in[1]), double(in[2])};
    hueKeeping(c, [&](double v) { return double(display[size_t(v)]); });
    const auto& m = toSrgb().m;
    for (int r = 0; r < 3; r++) out[r] = float(sqrtIndexed(encode, m[r][0] * c[0] + m[r][1] * c[1] + m[r][2] * c[2]));
}

ToneStage::ToneStage(const CameraRawSettings& s) {
    if (s.temperature != 0 || s.tint != 0) {
        gains_ = s.gains();
        gained_ = true;
    }
    const auto& cal = s.calibration;
    if (cal.adjusts()) {
        // Older process versions apply the sliders more weakly, as the kernels always have.
        const double version = cal.process <= 1 ? 0.55 : cal.process == 2 ? 0.65 : cal.process == 3 ? 0.75 : cal.process == 4 ? 0.85
                             : cal.process == 5 ? 0.92 : 1.0;
        const double hue[3] = {cal.redHue, cal.greenHue, cal.blueHue}, sat[3] = {cal.redSaturation, cal.greenSaturation, cal.blueSaturation};
        // Each primary's column: Hue moves it into the two other channels in opposite directions, Saturation out of both.
        static constexpr int feeds[3][2] = {{1, 2}, {0, 2}, {0, 1}};
        std::array<std::array<double, 3>, 3> e{};
        for (int p = 0; p < 3; p++) {
            const double h = hue[p] / 100 * version * kCalibrationHue[p], st = sat[p] / 100 * version * kCalibrationSaturation[p];
            e[size_t(feeds[p][0])][size_t(p)] += h - st;
            e[size_t(feeds[p][1])][size_t(p)] += -h - st;
        }
        for (size_t r = 0; r < 3; r++) {
            double off = 0;
            for (size_t k = 0; k < 3; k++) if (k != r) off += e[r][k];
            for (size_t k = 0; k < 3; k++) calibration_[r][k] = (r == k ? 1 - off : e[r][k]);
        }
        calibrated_ = hue[0] != 0 || hue[1] != 0 || hue[2] != 0 || sat[0] != 0 || sat[1] != 0 || sat[2] != 0;
        shadowTint_ = cal.shadowTint / 100 * version;
    }
    if (s.exposure != 0) {
        exposed_ = true;
        exposureScale_ = std::exp2(s.exposure);
        shoulder_ = std::clamp(s.exposure, 0.0, 1.0);
        exposure_.resize(4097);
        const double scale = std::exp2(s.exposure), weight = std::clamp(s.exposure, 0.0, 1.0);
        for (size_t i = 0; i < exposure_.size(); i++) {
            double scene = inverseToneCurve(std::pow(double(i) / 4096, 2)) * scale;
            if (weight > 0) scene = (1 - weight) * scene + weight * scene / std::pow(1 + std::pow(scene, kShoulderPower), 1 / kShoulderPower);
            exposure_[i] = float(curveDisplay(scene));
        }
    }
    const auto& c = s.curve;
    if (s.whites != 0 || s.blacks != 0 || s.highlights != 0 || s.shadows != 0 || s.dehaze != 0 || s.contrast != 0 || c.shadows != 0
        || c.darks != 0 || c.lights != 0 || c.highlights != 0) {
        toned_ = true;
        // Whites, Blacks, Highlights, Shadows, Dehaze and Contrast in that order, then the parametric curve. The curves that
        // run on the brightest and darkest channel compose into one table; some sliders work partly or wholly on the
        // colour's luminance instead (kSliderLuminance), which is what fits Camera Raw's colours.
        Composer pending;
        bool used = false;
        auto flush = [&] {
            if (used) steps_.push_back({pending.table(), 0});
            pending = Composer();
            used = false;
        };
        auto slider = [&](ToneSlider k, double amount) {
            if (amount == 0) return;
            const double luminance = kSliderLuminance[int(k)][amount > 0 ? 1 : 0];
            if (luminance == 0) { pending.then(k, amount); used = true; return; }
            flush();
            Composer one;
            one.then(k, amount);
            steps_.push_back({one.table(), luminance});
        };
        slider(ToneSlider::Whites, s.whites);
        slider(ToneSlider::Blacks, s.blacks);
        slider(ToneSlider::Highlights, s.highlights);
        slider(ToneSlider::Shadows, s.shadows);
        slider(ToneSlider::Dehaze, s.dehaze);
        slider(ToneSlider::Contrast, s.contrast);
        if (c.shadows != 0 || c.darks != 0 || c.lights != 0 || c.highlights != 0) {
            pending.thenParametric(c);
            used = true;
        }
        flush();
    }
    if (!CameraRawCurveSettings::isLinear(c.rgb)) {
        curved_ = true;
        master_ = c.channelTable(c.rgb);
        refine_ = std::clamp(c.refineSaturation / 100, -1.0, 0.0);
    }
    if (!CameraRawCurveSettings::isLinear(c.red) || !CameraRawCurveSettings::isLinear(c.green) || !CameraRawCurveSettings::isLinear(c.blue)) {
        channelCurved_ = true;
        channel_ = {c.channelTable(c.red), c.channelTable(c.green), c.channelTable(c.blue)};
    }
    saturation_ = s.saturation / 100;
    vibrance_ = s.vibrance / 100;
    for (size_t b = 0; b < 8; b++) {
        auto at = [&](double amount, const double (&gain)[8][2]) { return amount / 100 * gain[b][amount > 0 ? 1 : 0]; };
        mixer_[0][b] = at(s.mixer.hue[b], kMixerHue);
        mixer_[1][b] = at(s.mixer.saturation[b], kMixerSaturation);
        mixer_[2][b] = at(s.mixer.luminance[b], kMixerLightness);
        for (const auto& kind : mixer_) mixed_ = mixed_ || kind[b] != 0;
    }
    const CameraRawGradeWheel* wheels[4] = {&s.grading.shadows, &s.grading.midtones, &s.grading.highlights, &s.grading.global};
    for (size_t i = 0; i < 4; i++) {
        Wheel& wheel = wheels_[i];
        wheel.saturation = wheels[i]->saturation / 100;
        wheel.luminance = wheels[i]->luminance;
        // The fully saturated colour of the wheel's hue, less its mean, at unit length.
        const double hp = std::fmod(wheels[i]->hue + 360, 360.0) / 60;
        double push[3] = {std::clamp(std::fabs(hp - 3) - 1, 0.0, 1.0), std::clamp(2 - std::fabs(hp - 2), 0.0, 1.0), std::clamp(2 - std::fabs(hp - 4), 0.0, 1.0)};
        const double mean = (push[0] + push[1] + push[2]) / 3;
        double length = 0;
        for (double& v : push) { v -= mean; length += v * v; }
        length = std::sqrt(length);
        for (size_t k = 0; k < 3; k++) wheel.push[k] = length > 0 ? push[k] / length : 0;
        graded_ = graded_ || wheel.saturation > 0 || wheel.luminance != 0;
    }
    blending_ = s.grading.blending;
    balance_ = s.grading.balance;
    identity_ = !gained_ && !calibrated_ && shadowTint_ == 0 && !exposed_ && !toned_ && !curved_ && !channelCurved_ && saturation_ == 0
             && vibrance_ == 0 && !mixed_ && !graded_;
}

void ToneStage::apply(float rgb[3]) const {
    const auto& decode = decodeTable();
    const auto& encode = encodeTable();
    Rgb lin{};
    for (int k = 0; k < 3; k++) lin[size_t(k)] = std::clamp(linearIndexed(decode, rgb[k]) * gains_[size_t(k)], 0.0, 1.0);
    const Rgb p = proPhotoFromSrgb(lin);
    double c[3] = {std::max(0.0, p[0]), std::max(0.0, p[1]), std::max(0.0, p[2])};
    if (calibrated_) {
        // Camera Calibration on scene light (with Exposure, which then follows it there), then developed again.
        hueKeeping(c, [](double v) { return inverseToneCurve(v); });
        double s[3];
        for (int r = 0; r < 3; r++) s[r] = std::max(0.0, calibration_[size_t(r)][0] * c[0] + calibration_[size_t(r)][1] * c[1] + calibration_[size_t(r)][2] * c[2]);
        const auto& display = Developer::shared().display;
        hueKeeping(s, [&](double v) {
            double scene = v * exposureScale_;
            if (shoulder_ > 0) scene = (1 - shoulder_) * scene + shoulder_ * scene / std::pow(1 + std::pow(scene, kShoulderPower), 1 / kShoulderPower);
            const double x = std::clamp(scene, 0.0, 1.0) * 65535;
            const size_t i = std::min(size_t(x), size_t(65534));
            return double(display[i]) + (display[i + 1] - display[i]) * (x - double(i));
        });
        for (int k = 0; k < 3; k++) c[k] = s[k];
    } else if (exposed_) {
        hueKeeping(c, [&](double v) { return sqrtIndexed(exposure_, v); });
    }
    if (toned_ || curved_ || channelCurved_ || shadowTint_ != 0) {
        double e[3];
        for (int k = 0; k < 3; k++) e[k] = sqrtIndexed(encode, c[k]);
        if (shadowTint_ != 0) {
            // Shadow Tint: a push along its measured green-magenta direction, by the colour's encoded luminance.
            const double luma = sqrtIndexed(encode, kProPhotoY[0] * c[0] + kProPhotoY[1] * c[1] + kProPhotoY[2] * c[2]);
            const double push = shadowTint_ * gradeAt(kShadowTint, luma);
            for (int k = 0; k < 3; k++) e[k] = std::clamp(e[k] + push * kShadowTintDirection[k], 0.0, 1.0);
        }
        for (const Step& step : steps_) {
            auto curve = [&](double v) { return linearIndexed(step.table, v); };
            if (step.luminance == 0) {
                hueKeeping(e, curve);
                continue;
            }
            double kept[3] = {e[0], e[1], e[2]};
            hueKeeping(kept, curve);
            double lin[3];
            for (int k = 0; k < 3; k++) lin[k] = linearIndexed(decode, e[k]);
            const double y = kProPhotoY[0] * lin[0] + kProPhotoY[1] * lin[1] + kProPhotoY[2] * lin[2];
            const double ratio = y > 1e-9 ? linearIndexed(decode, curve(sqrtIndexed(encode, y))) / y : 1;
            for (int k = 0; k < 3; k++) {
                const double scaled = sqrtIndexed(encode, lin[k] * ratio);
                e[k] = kept[k] + (scaled - kept[k]) * step.luminance;
            }
        }
        if (curved_) {
            const double before[3] = {e[0], e[1], e[2]};
            hueKeeping(e, [&](double v) {
                const double x = std::clamp(v, 0.0, 1.0) * 255;
                const size_t i = std::min(size_t(x), size_t(254));
                return master_[i] + (master_[i + 1] - master_[i]) * (x - double(i));
            });
            if (refine_ < 0) {
                // Refine Saturation below Lightroom's 100: the colour keeps its channel differences from before the curve and
                // takes its luma (Rec. 601) from the curved colour, scaled down where that would leave 0..1.
                auto luma = [](const double* v) { return 0.299 * v[0] + 0.587 * v[1] + 0.114 * v[2]; };
                const double target = luma(e), was = luma(before);
                double scale = 1;
                for (int k = 0; k < 3; k++) {
                    const double d = before[k] - was;
                    if (target + d > 1) scale = std::min(scale, (1 - target) / d);
                    if (target + d < 0) scale = std::min(scale, target / -d);
                }
                for (int k = 0; k < 3; k++) {
                    const double kept = target + (before[k] - was) * std::max(0.0, scale);
                    e[k] = e[k] + (kept - e[k]) * -refine_;
                }
            }
        }
        if (channelCurved_)
            for (int k = 0; k < 3; k++) {
                const auto& table = channel_[size_t(k)];
                const double x = std::clamp(e[k], 0.0, 1.0) * 255;
                const size_t i = std::min(size_t(x), size_t(254));
                e[k] = table[i] + (table[i + 1] - table[i]) * (x - double(i));
            }
        for (int k = 0; k < 3; k++) c[k] = linearIndexed(decode, e[k]);
    }
    if (vibrance_ != 0) {
        // Vibrance (fitted to the chart's ±50 and ±100, kVibrance): the colourfulness scaled more the duller the colour
        // is. Raising it keeps the brightest channel and spares skin hues; lowering it pulls the channels toward their mean.
        const double hi = std::max({c[0], c[1], c[2]}), lo = std::min({c[0], c[1], c[2]});
        const double dull = 1 - std::clamp(hi > 1e-9 ? (hi - lo) / hi : 0.0, 0.0, 1.0);
        const VibranceFit& fit = kVibrance[vibrance_ > 0 ? 1 : 0];
        double skin = 0;
        if (fit.skin > 0) {
            const Rgb s = srgbFromProPhoto({c[0], c[1], c[2]});
            double e[3];
            for (int k = 0; k < 3; k++) e[k] = srgbEncode(std::clamp(s[size_t(k)], 0.0, 1.0));
            const double hue = hslHue(e);
            skin = std::max(0.0, 1 - std::fabs(std::remainder(hue - 15, 360.0)) / 45);
        }
        const double factor = std::max(0.0, 1 + vibrance_ * fit.gain * std::pow(dull, fit.power) * (1 - fit.skin * skin));
        const double pivot = vibrance_ > 0 ? hi : (c[0] + c[1] + c[2]) / 3;
        for (double& v : c) v = std::max(0.0, pivot + (v - pivot) * factor);
    }
    if (saturation_ != 0) {
        // Saturation: the colour's distance from its luminance in linear ProPhoto RGB, scaled by 1 + amount.
        const double y = kProPhotoY[0] * c[0] + kProPhotoY[1] * c[1] + kProPhotoY[2] * c[2];
        for (double& v : c) v = std::max(0.0, y + (v - y) * (1 + saturation_));
    }
    if (mixed_ || graded_) {
        double e[3];
        for (int k = 0; k < 3; k++) e[k] = sqrtIndexed(encode, c[k]);
        if (mixed_) {
            // The colour mixer in the HSL of ProPhoto RGB with the sRGB transfer: each band's share of the hue is linear
            // between neighbouring band centres; lightness moves with the square root of the chroma, so greys stay.
            double h, s, l;
            toHsl(e, h, s, l);
            double w[8];
            mixerWeights(h, w);
            double dh = 0, ds = 0, dl = 0;
            for (int b = 0; b < 8; b++) {
                dh += w[b] * mixer_[0][size_t(b)];
                ds += w[b] * mixer_[1][size_t(b)];
                dl += w[b] * mixer_[2][size_t(b)];
            }
            const double chroma = std::max({e[0], e[1], e[2]}) - std::min({e[0], e[1], e[2]});
            fromHsl(h + dh, std::clamp(s * (1 + ds), 0.0, 1.0), std::clamp(l + dl * std::pow(std::max(0.0, chroma), kMixerLightnessChromaPower), 0.0, 1.0), e);
        }
        if (graded_) {
            // Colour Grading: each wheel pushes the colour along its hue by its saturation and the weight its region has at
            // the colour's luminance; the Luminance sliders add grey the same way.
            double lin[3];
            for (int k = 0; k < 3; k++) lin[k] = linearIndexed(decode, e[k]);
            const double luma = sqrtIndexed(encode, kProPhotoY[0] * lin[0] + kProPhotoY[1] * lin[1] + kProPhotoY[2] * lin[2]);
            double add[3] = {0, 0, 0};
            for (int region = 0; region < 4; region++) {
                const Wheel& wheel = wheels_[size_t(region)];
                if (wheel.saturation > 0) {
                    const double weight = gradeWeight(region, luma, blending_, balance_) * wheel.saturation;
                    for (int k = 0; k < 3; k++) add[k] += wheel.push[size_t(k)] * weight;
                }
                if (wheel.luminance != 0) {
                    const double grey = gradeLuminance(region, luma, wheel.luminance);
                    for (double& v : add) v += grey;
                }
            }
            for (int k = 0; k < 3; k++) e[k] = std::clamp(e[k] + add[k], 0.0, 1.0);
        }
        for (int k = 0; k < 3; k++) c[k] = linearIndexed(decode, e[k]);
    }
    const auto& m = toSrgb().m;
    for (int r = 0; r < 3; r++) rgb[r] = float(sqrtIndexed(encode, m[r][0] * c[0] + m[r][1] * c[1] + m[r][2] * c[2]));
}

} // namespace compositor::rawtone
