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
    if (s.exposure != 0) {
        exposed_ = true;
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
        // run on the brightest and darkest channel compose into one table; negative Highlights works on the colour's
        // luminance, and Shadows halfway between the two, which is what fits Camera Raw's colours.
        Composer pending;
        bool used = false;
        auto flush = [&] {
            if (used) steps_.push_back({pending.table(), 0});
            pending = Composer();
            used = false;
        };
        auto slider = [&](ToneSlider k, double amount, double luminance) {
            if (amount == 0) return;
            if (luminance == 0) { pending.then(k, amount); used = true; return; }
            flush();
            Composer one;
            one.then(k, amount);
            steps_.push_back({one.table(), luminance});
        };
        slider(ToneSlider::Whites, s.whites, 0);
        slider(ToneSlider::Blacks, s.blacks, 0);
        slider(ToneSlider::Highlights, s.highlights, s.highlights < 0 ? 1 : 0);
        slider(ToneSlider::Shadows, s.shadows, 0.5);
        slider(ToneSlider::Dehaze, s.dehaze, 0);
        slider(ToneSlider::Contrast, s.contrast, 0);
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
    identity_ = !gained_ && !exposed_ && !toned_ && !curved_ && !channelCurved_ && saturation_ == 0 && vibrance_ == 0;
}

void ToneStage::apply(float rgb[3]) const {
    const auto& decode = decodeTable();
    const auto& encode = encodeTable();
    Rgb lin{};
    for (int k = 0; k < 3; k++) lin[size_t(k)] = std::clamp(linearIndexed(decode, rgb[k]) * gains_[size_t(k)], 0.0, 1.0);
    const Rgb p = proPhotoFromSrgb(lin);
    double c[3] = {std::max(0.0, p[0]), std::max(0.0, p[1]), std::max(0.0, p[2])};
    if (exposed_) hueKeeping(c, [&](double v) { return sqrtIndexed(exposure_, v); });
    if (toned_ || curved_ || channelCurved_) {
        double e[3];
        for (int k = 0; k < 3; k++) e[k] = sqrtIndexed(encode, c[k]);
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
        // Vibrance (fitted to the chart's ±50 and ±100): the colourfulness scaled more the duller the colour is. Raising it
        // keeps the brightest channel and spares skin hues by half; lowering it pulls the channels toward their mean.
        const double hi = std::max({c[0], c[1], c[2]}), lo = std::min({c[0], c[1], c[2]});
        const double dull = 1 - std::clamp(hi > 1e-9 ? (hi - lo) / hi : 0.0, 0.0, 1.0);
        double factor, pivot;
        if (vibrance_ > 0) {
            const Rgb s = srgbFromProPhoto({c[0], c[1], c[2]});
            double e[3];
            for (int k = 0; k < 3; k++) e[k] = srgbEncode(std::clamp(s[size_t(k)], 0.0, 1.0));
            const double eHi = std::max({e[0], e[1], e[2]}), eLo = std::min({e[0], e[1], e[2]}), chroma = eHi - eLo;
            double hue = 0;
            if (chroma > 1e-9) {
                if (eHi == e[0]) hue = std::fmod((e[1] - e[2]) / chroma + 6, 6.0) * 60;
                else if (eHi == e[1]) hue = ((e[2] - e[0]) / chroma + 2) * 60;
                else hue = ((e[0] - e[1]) / chroma + 4) * 60;
            }
            const double skin = std::max(0.0, 1 - std::fabs(std::remainder(hue - 15, 360.0)) / 45);
            factor = 1 + vibrance_ * 0.9 * std::pow(dull, 0.7) * (1 - 0.5 * skin);
            pivot = hi;
        } else {
            factor = 1 + vibrance_ * 0.9 * std::pow(dull, 0.2);
            pivot = (c[0] + c[1] + c[2]) / 3;
        }
        factor = std::max(0.0, factor);
        for (double& v : c) v = std::max(0.0, pivot + (v - pivot) * factor);
    }
    if (saturation_ != 0) {
        // Saturation: the colour's distance from its luminance in linear ProPhoto RGB, scaled by 1 + amount.
        const double y = kProPhotoY[0] * c[0] + kProPhotoY[1] * c[1] + kProPhotoY[2] * c[2];
        for (double& v : c) v = std::max(0.0, y + (v - y) * (1 + saturation_));
    }
    const auto& m = toSrgb().m;
    for (int r = 0; r < 3; r++) rgb[r] = float(sqrtIndexed(encode, m[r][0] * c[0] + m[r][1] * c[1] + m[r][2] * c[2]));
}

} // namespace compositor::rawtone
