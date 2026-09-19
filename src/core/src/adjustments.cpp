#include "compositor/adjustments.h"
#include "compositor/parallel.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>

extern "C" {
#include "AdjustPixels.h"
#include "LevelsPixels.h"
}

using json = nlohmann::json;

namespace compositor {

namespace {

double clampFinite(double v, double lo, double hi, double fallback) { return std::isfinite(v) ? std::min(hi, std::max(lo, v)) : fallback; }

void applyTables(Image& image, const std::vector<float>& tables) {
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) levels_apply(image.row(y), size_t(image.width()), tables.data());
    });
}

} // namespace

// ---- Levels ------------------------------------------------------------------------

const char* levelsChannelName(int channel) {
    static const char* names[] = {"RGB", "Red", "Green", "Blue"};
    return names[size_t(clamp(channel, 0, 3))];
}

bool parseLevelsChannel(const std::string& name, int& out) {
    for (int i = 0; i < 4; i++) if (name == levelsChannelName(i)) { out = i; return true; }
    return false;
}

LevelsRange LevelsRange::normalized() const {
    LevelsRange r = *this;
    r.black = clampFinite(black, 0, 254, 0);
    r.white = clampFinite(white, r.black + 1, 255, 255);
    r.gamma = clampFinite(gamma, 0.1, 9.99, 1);
    r.outputBlack = clampFinite(outputBlack, 0, 255, 0);
    r.outputWhite = clampFinite(outputWhite, 0, 255, 255);
    return r;
}

double LevelsRange::apply(double value) const {
    LevelsRange s = normalized();
    double input = std::min(1.0, std::max(0.0, (value * 255 - s.black) / (s.white - s.black)));
    return (s.outputBlack + std::pow(input, 1 / s.gamma) * (s.outputWhite - s.outputBlack)) / 255;
}

bool LevelsSettings::isIdentity() const {
    for (auto& r : ranges) if (!(r.normalized() == LevelsRange())) return false;
    return true;
}

void applyLevels(Image& image, const LevelsSettings& settings) {
    if (settings.isIdentity()) return;
    std::vector<float> tables(3 * 256);
    for (int c = 0; c < 3; c++) for (int i = 0; i < 256; i++) tables[size_t(c * 256 + i)] = float(settings.apply(i / 255.0, c + 1));
    applyTables(image, tables);
}

std::array<std::vector<double>, 4> levelsHistogram(const Image& image, const GrayImage* coverage) {
    std::vector<double> bins(1024, 0);
    std::vector<uint8_t> cov;
    if (coverage && coverage->width() == image.width() && coverage->height() == image.height()) cov.assign(coverage->data(), coverage->data() + coverage->byteCount());
    for (int y = 0; y < image.height(); y++)
        levels_histogram(image.row(y), cov.empty() ? nullptr : cov.data() + size_t(y) * image.width(), size_t(image.width()), bins.data());
    std::array<std::vector<double>, 4> result;
    for (int c = 0; c < 4; c++) result[size_t(c)].assign(bins.begin() + c * 256, bins.begin() + (c + 1) * 256);
    return result;
}

LevelsSettings autoLevels(LevelsAuto mode, const std::array<std::vector<double>, 4>& histogram) {
    LevelsSettings result;
    auto endpoints = [](const std::vector<double>& bins, double& low, double& high) {
        double total = 0;
        for (double b : bins) total += b;
        if (total <= 0) return false;
        double sum = 0;
        int lo = 0, hi = 255;
        for (int i = 0; i < 256; i++) { sum += bins[size_t(i)]; if (sum > total * 0.001) { lo = i; break; } }
        sum = 0;
        for (int i = 255; i >= 0; i--) { sum += bins[size_t(i)]; if (sum > total * 0.001) { hi = i; break; } }
        if (lo >= hi) return false;
        low = lo; high = hi;
        return true;
    };
    if (mode == LevelsAuto::Contrast) {
        double low = 256, high = -1;
        for (int c = 1; c <= 3; c++) { double lo, hi; if (endpoints(histogram[size_t(c)], lo, hi)) { low = std::min(low, lo); high = std::max(high, hi); } }
        if (low < high) { result.ranges[0].black = low; result.ranges[0].white = high; }
    } else {
        for (int c = 1; c <= 3; c++) {
            double low, high;
            if (!endpoints(histogram[size_t(c)], low, high)) continue;
            LevelsRange range;
            range.black = low; range.white = high;
            if (mode == LevelsAuto::Neutral) {
                double total = 0, mean = 0;
                for (int i = 0; i < 256; i++) { total += histogram[size_t(c)][size_t(i)]; mean += range.apply(i / 255.0) * histogram[size_t(c)][size_t(i)]; }
                mean /= total;
                if (mean > 0 && mean < 1) range.gamma = std::min(9.99, std::max(0.1, std::log(mean) / std::log(0.5)));
            }
            result.ranges[size_t(c)] = range;
        }
    }
    return result;
}

LevelsSettings sampleLevels(const LevelsSettings& settings, double r, double g, double b, LevelsSample mode) {
    LevelsSettings result = settings;
    result.ranges[0] = LevelsRange();
    double rgb[3] = {r, g, b};
    for (int c = 1; c <= 3; c++) {
        LevelsRange range = result.ranges[size_t(c)];
        double v = rgb[c - 1] * 255;
        switch (mode) {
        case LevelsSample::Black: range.black = std::min(range.white - 1, std::max(0.0, v)); break;
        case LevelsSample::White: range.white = std::max(range.black + 1, std::min(255.0, v)); break;
        case LevelsSample::Gray: {
            double fraction = (v - range.black) / (range.white - range.black);
            if (!(fraction > 0 && fraction < 1)) continue;
            range.gamma = std::log(fraction) / std::log(0.5);
            break;
        }
        }
        range.outputBlack = 0; range.outputWhite = 255;
        result.ranges[size_t(c)] = range.normalized();
    }
    return result;
}

// ---- Curves ------------------------------------------------------------------------

bool CurvesSettings::isValid() const {
    for (auto& points : channels) {
        if (points.size() < 2 || points.size() > 32 || points.front().x != 0 || points.back().x != 255) return false;
        for (size_t i = 0; i < points.size(); i++) {
            if (!std::isfinite(points[i].x) || !std::isfinite(points[i].y) || points[i].x < 0 || points[i].x > 255 || points[i].y < 0 || points[i].y > 255) return false;
            if (i > 0 && !(points[i - 1].x < points[i].x)) return false;
        }
    }
    return true;
}

bool CurvesSettings::isIdentity() const {
    for (auto& points : channels) {
        if (points.size() != 2) return false;
        if (points[0].y != 0 || points[1].y != 255) return false;
    }
    return true;
}

double CurvesSettings::value(double x, int channel) const {
    const auto& p = channels[size_t(clamp(channel, 0, 3))];
    size_t i = 0;
    for (size_t j = 0; j < p.size(); j++) if (p[j].x <= x) i = j;
    i = std::min(p.size() - 2, i);
    std::vector<double> d;
    for (size_t j = 0; j + 1 < p.size(); j++) d.push_back((p[j + 1].y - p[j].y) / (p[j + 1].x - p[j].x));
    auto slope = [&](size_t j) {
        if (j == 0) return d[0];
        if (j == p.size() - 1) return d.back();
        if (d[j - 1] * d[j] <= 0) return 0.0;
        return 2 / (1 / d[j - 1] + 1 / d[j]);
    };
    double h = p[i + 1].x - p[i].x, t = std::min(1.0, std::max(0.0, (x - p[i].x) / h));
    double y = (2 * t * t * t - 3 * t * t + 1) * p[i].y + (t * t * t - 2 * t * t + t) * h * slope(i)
        + (-2 * t * t * t + 3 * t * t) * p[i + 1].y + (t * t * t - t * t) * h * slope(i + 1);
    return std::min(255.0, std::max(0.0, y));
}

void applyCurves(Image& image, const CurvesSettings& settings) {
    if (!settings.isValid() || settings.isIdentity()) return;
    std::vector<float> tables(3 * 256);
    for (int c = 1; c <= 3; c++) for (int i = 0; i < 256; i++) tables[size_t((c - 1) * 256 + i)] = float(settings.value(settings.value(i, c), 0) / 255);
    applyTables(image, tables);
}

// ---- Exposure ----------------------------------------------------------------------

ExposureSettings ExposureSettings::normalized() const {
    return {clampFinite(exposure, -20, 20, 0), clampFinite(offset, -0.5, 0.5, 0), clampFinite(gamma, 0.01, 9.99, 1)};
}

std::array<float, 256> ExposureSettings::table() const {
    ExposureSettings s = normalized();
    std::array<float, 256> result{};
    double scale = std::pow(2.0, s.exposure);
    for (int i = 0; i < 256; i++) {
        double encoded = i / 255.0;
        double linear = encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4);
        linear = std::pow(std::max(0.0, linear * scale + s.offset), 1 / s.gamma);
        double output = linear <= 0.0031308 ? linear * 12.92 : 1.055 * std::pow(linear, 1 / 2.4) - 0.055;
        result[size_t(i)] = float(std::min(1.0, std::max(0.0, output)));
    }
    return result;
}

void applyExposure(Image& image, const ExposureSettings& settings) {
    if (settings.normalized().isIdentity()) return;
    auto t = settings.table();
    std::vector<float> tables(3 * 256);
    for (int c = 0; c < 3; c++) std::copy(t.begin(), t.end(), tables.begin() + c * 256);
    applyTables(image, tables);
}

// ---- Gradient Map and Grain ----------------------------------------------------------

AdjustmentColor AdjustmentColor::clamped() const { return {clampFinite(red, 0, 1, 0), clampFinite(green, 0, 1, 0), clampFinite(blue, 0, 1, 0)}; }

std::vector<uint8_t> GradientMapSettings::table() const {
    AdjustmentColor dark = (reversed ? highlights : shadows).clamped(), light = (reversed ? shadows : highlights).clamped();
    std::vector<uint8_t> result(256 * 3);
    for (int i = 0; i < 256; i++) {
        double t = i / 255.0;
        double rgb[3] = {dark.red + (light.red - dark.red) * t, dark.green + (light.green - dark.green) * t, dark.blue + (light.blue - dark.blue) * t};
        for (int c = 0; c < 3; c++) result[size_t(i * 3 + c)] = uint8_t(std::min(255.0, std::max(0.0, std::round(rgb[c] * 255))));
    }
    return result;
}

void applyGradientMap(Image& image, const GradientMapSettings& settings) {
    std::vector<uint8_t> table = settings.table();
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) adjust_gradient_map(image.row(y), size_t(image.width()), 1, size_t(image.stride()), table.data());
    });
}

GrainSettings GrainSettings::normalized() const {
    GrainSettings s = *this;
    s.amount = clampFinite(amount, 0, 100, 25);
    s.size = clampFinite(size, 0.5, 20, 1.5);
    s.roughness = clampFinite(roughness, 0, 100, 50);
    return s;
}

void applyGrain(Image& image, const GrainSettings& settings, Point origin, double unitsPerPixel) {
    GrainSettings s = settings.normalized();
    if (s.amount <= 0 || !(unitsPerPixel > 0) || !std::isfinite(unitsPerPixel)) return;
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++)
            adjust_grain(image.row(y), size_t(image.width()), 1, size_t(image.stride()), s.amount, s.size, s.roughness, s.seed, origin.x, origin.y + y * unitsPerPixel, unitsPerPixel);
    });
}

// ---- Hue/Saturation --------------------------------------------------------------------

const char* colorRangeName(int range) {
    static const char* names[] = {"Master", "Reds", "Yellows", "Greens", "Cyans", "Blues", "Magentas"};
    return names[size_t(clamp(range, 0, 6))];
}

bool parseColorRange(const std::string& name, int& out) {
    for (int i = 0; i < 7; i++) if (name == colorRangeName(i)) { out = i; return true; }
    return false;
}

HueBand HueBand::defaultBand(int range) {
    switch (range) {
    case 1: return {315, 345, 15, 45};
    case 2: return {15, 45, 75, 105};
    case 3: return {75, 105, 135, 165};
    case 4: return {135, 165, 195, 225};
    case 5: return {195, 225, 255, 285};
    case 6: return {255, 285, 315, 345};
    default: return {0, 0, 360, 360};
    }
}

double HueBand::forward(double from, double to) {
    double delta = std::fmod(to - from, 360.0);
    return delta < 0 ? delta + 360 : delta;
}

double HueBand::weight(double hue) const {
    double span = forward(falloffStart, falloffEnd);
    if (span <= 0) return 1;
    double position = forward(falloffStart, hue);
    if (position > span) return 0;
    double rampIn = forward(falloffStart, rangeStart), plateauEnd = forward(falloffStart, rangeEnd);
    if (position < rampIn) return rampIn > 0 ? position / rampIn : 1;
    if (position <= plateauEnd) return 1;
    double rampOut = span - plateauEnd;
    return rampOut > 0 ? (span - position) / rampOut : 1;
}

namespace { double wrap360(double v) { double r = std::fmod(v, 360.0); return r < 0 ? r + 360 : r; } }

HueBand HueBand::centered(double hue) const {
    double core = forward(rangeStart, rangeEnd), leading = forward(falloffStart, rangeStart), trailing = forward(rangeEnd, falloffEnd);
    double start = wrap360(hue - core / 2);
    return {wrap360(start - leading), start, wrap360(start + core), wrap360(start + core + trailing)};
}

void HueBand::include(double hue) {
    if (weight(hue) >= 1) return;
    double shoulderIn = forward(falloffStart, rangeStart), shoulderOut = forward(rangeEnd, falloffEnd);
    if (forward(hue, rangeStart) <= forward(rangeEnd, hue)) { rangeStart = hue; falloffStart = hue - shoulderIn; }
    else { rangeEnd = hue; falloffEnd = hue + shoulderOut; }
    normalize();
}

void HueBand::exclude(double hue) {
    if (weight(hue) <= 0) return;
    double shoulderIn = forward(falloffStart, rangeStart), shoulderOut = forward(rangeEnd, falloffEnd);
    if (forward(falloffStart, hue) <= forward(hue, falloffEnd)) { falloffStart = hue + 1; rangeStart = hue + 1 + shoulderIn; }
    else { falloffEnd = hue - 1; rangeEnd = hue - 1 - shoulderOut; }
    normalize();
}

void HueBand::normalize() {
    falloffStart = wrap360(falloffStart); rangeStart = wrap360(rangeStart); rangeEnd = wrap360(rangeEnd); falloffEnd = wrap360(falloffEnd);
    if (forward(falloffStart, falloffEnd) > 350) falloffEnd = wrap360(falloffStart + 350);
}

void HueBand::setHandle(int index, double degrees) {
    HueBand updated = *this;
    double value = wrap360(degrees);
    switch (index) { case 0: updated.falloffStart = value; break; case 1: updated.rangeStart = value; break; case 2: updated.rangeEnd = value; break; default: updated.falloffEnd = value; }
    double span = forward(updated.falloffStart, updated.falloffEnd), toStart = forward(updated.falloffStart, updated.rangeStart), toEnd = forward(updated.falloffStart, updated.rangeEnd);
    if (!(span > 1 && span <= 350 && toStart <= toEnd && toEnd <= span)) return;
    *this = updated;
}

HueSaturationSettings::HueSaturationSettings() { for (int i = 0; i < 7; i++) bands[i] = HueBand::defaultBand(i); }

HueSaturationSettings HueSaturationSettings::colorizeStart() {
    HueSaturationSettings s;
    s.colorize = true;
    s.adjustments[0] = {0, 25, 0};
    return s;
}

bool HueSaturationSettings::isIdentity() const {
    if (colorize) return false;
    for (auto& [range, a] : adjustments) if (!(a == RangeAdjustment{})) return false;
    return true;
}

double HueSaturationSettings::weight(int colorRange, double hue) const {
    if (colorRange == 0) return 1;
    auto it = bands.find(colorRange);
    double w = (it == bands.end() ? HueBand::defaultBand(colorRange) : it->second).weight(hue);
    return invertRange && colorRange == range ? 1 - w : w;
}

namespace {

void toHSL(double r, double g, double b, double& h, double& s, double& l) {
    double high = std::max({r, g, b}), low = std::min({r, g, b});
    l = (high + low) / 2;
    double delta = high - low;
    if (delta <= 0) { h = 0; s = 0; return; }
    s = std::min(1.0, delta / (1 - std::fabs(2 * l - 1)));
    if (high == r) h = (g - b) / delta; else if (high == g) h = (b - r) / delta + 2; else h = (r - g) / delta + 4;
    h *= 60;
    if (h < 0) h += 360;
}

void toRGB(double h, double s, double l, double& r, double& g, double& b) {
    if (s <= 0) { r = g = b = l; return; }
    double chroma = (1 - std::fabs(2 * l - 1)) * s;
    double sector = h / 60;
    double second = chroma * (1 - std::fabs(std::fmod(sector, 2.0) - 1));
    double base = l - chroma / 2;
    switch (int(sector)) {
    case 0: r = chroma; g = second; b = 0; break;
    case 1: r = second; g = chroma; b = 0; break;
    case 2: r = 0; g = chroma; b = second; break;
    case 3: r = 0; g = second; b = chroma; break;
    case 4: r = second; g = 0; b = chroma; break;
    default: r = chroma; g = 0; b = second; break;
    }
    r = std::min(1.0, std::max(0.0, r + base)); g = std::min(1.0, std::max(0.0, g + base)); b = std::min(1.0, std::max(0.0, b + base));
}

struct HueResponse { double shift = 0, saturation = 0, lightness = 0; };

std::vector<HueResponse> hueResponse(const HueSaturationSettings& settings) {
    std::vector<HueResponse> result(361);
    for (int degree = 0; degree <= 360; degree++) {
        for (auto& [range, a] : settings.adjustments) {
            if (a == RangeAdjustment{}) continue;
            double w = settings.weight(range, degree);
            if (w <= 0) continue;
            result[size_t(degree)].shift += a.hue * w;
            result[size_t(degree)].saturation += a.saturation * w;
            result[size_t(degree)].lightness += a.lightness * w;
        }
    }
    return result;
}

void adjustColor(const HueSaturationSettings& settings, const std::vector<HueResponse>& response, double& r, double& g, double& b) {
    double h, s, l;
    toHSL(r, g, b, h, s, l);
    double lightnessAmount;
    if (settings.colorize) {
        RangeAdjustment a = settings.currentValue();
        h = wrap360(a.hue);
        s = std::min(1.0, std::max(0.0, a.saturation / 100));
        lightnessAmount = a.lightness / 100;
    } else {
        const HueResponse& sampled = response[size_t(std::min(360, std::max(0, int(std::round(h)))))];
        lightnessAmount = sampled.lightness / 100;
        h = wrap360(h + sampled.shift);
        s = std::min(1.0, std::max(0.0, s * (1 + sampled.saturation / 100)));
    }
    double amount = std::min(1.0, std::max(-1.0, lightnessAmount));
    l = amount >= 0 ? l + (1 - l) * amount : l * (1 + amount);
    toRGB(h, s, std::min(1.0, std::max(0.0, l)), r, g, b);
}

} // namespace

void HueSaturationSettings::adjust(double& r, double& g, double& b) const { adjustColor(*this, hueResponse(*this), r, g, b); }

double HueSaturationSettings::shiftedHue(double hue) const {
    double shift = 0;
    for (auto& [range, a] : adjustments) if (a.hue != 0) shift += a.hue * weight(range, hue);
    return wrap360(hue + shift);
}

void applyHueSaturation(Image& image, const HueSaturationSettings& settings) {
    if (settings.isIdentity()) return;
    // A 33-point colour cube, as the Mac's CIColorCube, sampled trilinearly; fast enough for slider drags.
    constexpr int dim = 33;
    std::vector<float> cube(size_t(dim) * dim * dim * 3);
    std::vector<HueResponse> response = hueResponse(settings);
    parallelRows(0, dim, [&](int b0, int b1) {
        for (int bi = b0; bi < b1; bi++)
            for (int gi = 0; gi < dim; gi++)
                for (int ri = 0; ri < dim; ri++) {
                    double r = ri / double(dim - 1), g = gi / double(dim - 1), b = bi / double(dim - 1);
                    adjustColor(settings, response, r, g, b);
                    size_t index = (size_t(bi) * dim * dim + size_t(gi) * dim + size_t(ri)) * 3;
                    cube[index] = float(r); cube[index + 1] = float(g); cube[index + 2] = float(b);
                }
    }, 1);
    auto sample = [&](float r, float g, float b, float out[3]) {
        float fr = r * (dim - 1), fg = g * (dim - 1), fb = b * (dim - 1);
        int r0 = std::min(dim - 2, int(fr)), g0 = std::min(dim - 2, int(fg)), b0 = std::min(dim - 2, int(fb));
        float tr = fr - r0, tg = fg - g0, tb = fb - b0;
        out[0] = out[1] = out[2] = 0;
        for (int db = 0; db < 2; db++) for (int dg = 0; dg < 2; dg++) for (int dr = 0; dr < 2; dr++) {
            float w = (dr ? tr : 1 - tr) * (dg ? tg : 1 - tg) * (db ? tb : 1 - tb);
            if (w <= 0) continue;
            const float* c = &cube[(size_t(b0 + db) * dim * dim + size_t(g0 + dg) * dim + size_t(r0 + dr)) * 3];
            out[0] += c[0] * w; out[1] += c[1] * w; out[2] += c[2] * w;
        }
    };
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            uint8_t* p = image.row(y);
            for (int x = 0; x < image.width(); x++, p += 4) {
                unsigned a = p[3];
                if (!a) continue;
                float out[3];
                sample(std::min(1.0f, p[0] / float(a)), std::min(1.0f, p[1] / float(a)), std::min(1.0f, p[2] / float(a)), out);
                for (int c = 0; c < 3; c++) p[c] = uint8_t(std::min(float(a), std::max(0.0f, out[c] * a + 0.5f)));
            }
        }
    });
}

// ---- Invert ---------------------------------------------------------------------------

void applyInvert(Image& image) {
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            uint8_t* p = image.row(y);
            for (int x = 0; x < image.width(); x++, p += 4) for (int c = 0; c < 3; c++) p[c] = uint8_t(p[3] - p[c]);
        }
    });
}

void applyInvert(GrayImage& mask) {
    for (size_t i = 0; i < mask.byteCount(); i++) mask.data()[i] = uint8_t(255 - mask.data()[i]);
}

// ---- Settings JSON ---------------------------------------------------------------------

namespace {

json number(double v) {
    if (std::isfinite(v) && v == std::floor(v) && std::fabs(v) < 1e15) return json(static_cast<long long>(v));
    return json(v);
}

double num(const json& j, const char* key, double fallback) {
    auto it = j.find(key);
    return it != j.end() && it->is_number() ? it->get<double>() : fallback;
}

bool boolean(const json& j, const char* key, bool fallback) {
    auto it = j.find(key);
    return it != j.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

bool parseLevels(const json& j, LevelsSettings& out) {
    if (!j.is_object()) return false;
    std::string channel = j.value("channel", "RGB");
    if (!parseLevelsChannel(channel, out.channel)) return false;
    auto ranges = j.find("ranges");
    if (ranges == j.end() || !ranges->is_array() || ranges->size() != 4) return false;
    for (size_t i = 0; i < 4; i++) {
        const json& r = (*ranges)[i];
        if (!r.is_object()) return false;
        out.ranges[i] = {num(r, "black", 0), num(r, "gamma", 1), num(r, "white", 255), num(r, "outputBlack", 0), num(r, "outputWhite", 255)};
        if (!(out.ranges[i] == out.ranges[i].normalized())) return false;
    }
    return true;
}

json levelsJson(const LevelsSettings& s) {
    json j;
    j["channel"] = levelsChannelName(s.channel);
    j["ranges"] = json::array();
    for (auto& r : s.ranges) j["ranges"].push_back({{"black", number(r.black)}, {"gamma", number(r.gamma)}, {"white", number(r.white)}, {"outputBlack", number(r.outputBlack)}, {"outputWhite", number(r.outputWhite)}});
    return j;
}

bool parseCurves(const json& j, CurvesSettings& out) {
    if (!j.is_object()) return false;
    if (!parseLevelsChannel(j.value("channel", "RGB"), out.channel)) return false;
    auto channels = j.find("channels");
    if (channels == j.end() || !channels->is_array() || channels->size() != 4) return false;
    for (size_t c = 0; c < 4; c++) {
        out.channels[c].clear();
        if (!(*channels)[c].is_array()) return false;
        for (auto& p : (*channels)[c]) { if (!p.is_object()) return false; out.channels[c].push_back({num(p, "x", 0), num(p, "y", 0)}); }
    }
    return out.isValid();
}

json curvesJson(const CurvesSettings& s) {
    json j;
    j["channel"] = levelsChannelName(s.channel);
    j["channels"] = json::array();
    for (auto& points : s.channels) { json arr = json::array(); for (auto& p : points) arr.push_back({{"x", number(p.x)}, {"y", number(p.y)}}); j["channels"].push_back(arr); }
    return j;
}

// Swift encodes [ColorRange: T] as a flat array alternating key and value; accept an object too.
template <typename Value, typename Parse>
bool parseRangeMap(const json& j, std::map<int, Value>& out, Parse parse) {
    out.clear();
    if (j.is_object()) {
        for (auto& [key, value] : j.items()) { int range; Value v; if (!parseColorRange(key, range) || !parse(value, v)) return false; out[range] = v; }
        return true;
    }
    if (!j.is_array() || j.size() % 2 != 0) return false;
    for (size_t i = 0; i + 1 < j.size(); i += 2) {
        int range; Value v;
        if (!j[i].is_string() || !parseColorRange(j[i].get<std::string>(), range) || !parse(j[i + 1], v)) return false;
        out[range] = v;
    }
    return true;
}

bool parseHsv(const json& j, HueSaturationSettings& out) {
    if (!j.is_object()) return false;
    if (!parseColorRange(j.value("range", "Master"), out.range)) return false;
    out.colorize = boolean(j, "colorize", false);
    out.invertRange = boolean(j, "invertRange", false);
    auto adjustments = j.find("adjustments");
    if (adjustments != j.end() && !parseRangeMap(*adjustments, out.adjustments, [](const json& v, RangeAdjustment& a) {
            if (!v.is_object()) return false;
            a = {num(v, "hue", 0), num(v, "saturation", 0), num(v, "lightness", 0)};
            return std::isfinite(a.hue) && std::fabs(a.hue) <= 360 && std::fabs(a.saturation) <= 100 && std::fabs(a.lightness) <= 100;
        })) return false;
    auto bands = j.find("bands");
    if (bands != j.end() && !parseRangeMap(*bands, out.bands, [](const json& v, HueBand& b) {
            if (!v.is_object()) return false;
            b = {num(v, "falloffStart", 0), num(v, "rangeStart", 0), num(v, "rangeEnd", 360), num(v, "falloffEnd", 360)};
            return std::isfinite(b.falloffStart) && std::isfinite(b.rangeStart) && std::isfinite(b.rangeEnd) && std::isfinite(b.falloffEnd);
        })) return false;
    for (int i = 0; i < 7; i++) if (!out.bands.count(i)) out.bands[i] = HueBand::defaultBand(i);
    return true;
}

json hsvJson(const HueSaturationSettings& s) {
    json j;
    j["range"] = colorRangeName(s.range);
    j["colorize"] = s.colorize;
    j["invertRange"] = s.invertRange;
    json adjustments = json::array();
    for (auto& [range, a] : s.adjustments) { adjustments.push_back(colorRangeName(range)); adjustments.push_back({{"hue", number(a.hue)}, {"saturation", number(a.saturation)}, {"lightness", number(a.lightness)}}); }
    j["adjustments"] = adjustments;
    json bands = json::array();
    for (auto& [range, b] : s.bands) { bands.push_back(colorRangeName(range)); bands.push_back({{"falloffStart", number(b.falloffStart)}, {"rangeStart", number(b.rangeStart)}, {"rangeEnd", number(b.rangeEnd)}, {"falloffEnd", number(b.falloffEnd)}}); }
    j["bands"] = bands;
    return j;
}

const char* knownKeys[] = {"kind", "hue", "saturation", "lightness", "colorize", "hsvSettings", "levels", "curves", "exposureSettings", "gradientMapSettings", "grainSettings"};

} // namespace

AdjustmentSettings AdjustmentSettings::defaults(AdjustmentKind kind) {
    AdjustmentSettings s;
    s.kind = kind;
    return s;
}

bool AdjustmentSettings::parse(const std::string& text, AdjustmentSettings& out) {
    json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return false;
    AdjustmentSettings s;
    if (!parseAdjustmentKind(j.value("kind", ""), s.kind)) return false;
    auto hsv = j.find("hsvSettings");
    if (hsv != j.end() && !hsv->is_null()) { if (!parseHsv(*hsv, s.hsv)) return false; }
    else {
        // Projects saved before range-aware Hue/Saturation carry the three sliders at the top level.
        double hue = num(j, "hue", 0), saturation = num(j, "saturation", 0), lightness = num(j, "lightness", 0);
        if (std::fabs(hue) > 360 || std::fabs(saturation) > 100 || std::fabs(lightness) > 100) return false;
        s.hsv.colorize = boolean(j, "colorize", false);
        s.hsv.adjustments[0] = {hue, saturation, lightness};
    }
    auto levels = j.find("levels");
    if (levels != j.end() && !levels->is_null() && !parseLevels(*levels, s.levels)) return false;
    auto curves = j.find("curves");
    if (curves != j.end() && !curves->is_null() && !parseCurves(*curves, s.curves)) return false;
    auto exposure = j.find("exposureSettings");
    if (exposure != j.end() && exposure->is_object()) {
        s.exposure = {num(*exposure, "exposure", 0), num(*exposure, "offset", 0), num(*exposure, "gamma", 1)};
        if (!(s.exposure == s.exposure.normalized())) return false;
    }
    auto gradient = j.find("gradientMapSettings");
    if (gradient != j.end() && gradient->is_object()) {
        auto color = [&](const char* key, AdjustmentColor& c) {
            auto it = gradient->find(key);
            if (it == gradient->end() || !it->is_object()) return true;
            c = {num(*it, "red", 0), num(*it, "green", 0), num(*it, "blue", 0)};
            return c == c.clamped();
        };
        if (!color("shadows", s.gradientMap.shadows) || !color("highlights", s.gradientMap.highlights)) return false;
        s.gradientMap.reversed = boolean(*gradient, "reversed", false);
    }
    auto grain = j.find("grainSettings");
    if (grain != j.end() && grain->is_object()) {
        s.grain = {num(*grain, "amount", 25), num(*grain, "size", 1.5), num(*grain, "roughness", 50), uint32_t(num(*grain, "seed", 0))};
        GrainSettings n = s.grain.normalized();
        if (n.amount != s.grain.amount || n.size != s.grain.size || n.roughness != s.grain.roughness) return false;
    }
    json extra = json::object();
    for (auto& [key, value] : j.items()) {
        bool known = false;
        for (auto* k : knownKeys) if (key == k) known = true;
        if (!known) extra[key] = value;
    }
    if (!extra.empty()) s.extraJson = extra.dump();
    out = s;
    return true;
}

std::string AdjustmentSettings::toJson() const {
    json j = json::object();
    if (!extraJson.empty()) { auto extra = json::parse(extraJson, nullptr, false); if (extra.is_object()) j = extra; }
    j["kind"] = adjustmentKindName(kind);
    RangeAdjustment master = hsv.adjustments.count(0) ? hsv.adjustments.at(0) : RangeAdjustment{};
    j["hue"] = number(master.hue);
    j["saturation"] = number(master.saturation);
    j["lightness"] = number(master.lightness);
    j["colorize"] = hsv.colorize;
    j["hsvSettings"] = hsvJson(hsv);
    j["levels"] = levelsJson(levels);
    j["curves"] = curvesJson(curves);
    j["exposureSettings"] = {{"exposure", number(exposure.exposure)}, {"offset", number(exposure.offset)}, {"gamma", number(exposure.gamma)}};
    j["gradientMapSettings"] = {{"shadows", {{"red", number(gradientMap.shadows.red)}, {"green", number(gradientMap.shadows.green)}, {"blue", number(gradientMap.shadows.blue)}}},
                                {"highlights", {{"red", number(gradientMap.highlights.red)}, {"green", number(gradientMap.highlights.green)}, {"blue", number(gradientMap.highlights.blue)}}},
                                {"reversed", gradientMap.reversed}};
    j["grainSettings"] = {{"amount", number(grain.amount)}, {"size", number(grain.size)}, {"roughness", number(grain.roughness)}, {"seed", grain.seed}};
    return j.dump();
}

bool AdjustmentSettings::isValid() const {
    for (auto& [range, a] : hsv.adjustments) if (!(std::fabs(a.hue) <= 360 && std::fabs(a.saturation) <= 100 && std::fabs(a.lightness) <= 100)) return false;
    for (auto& r : levels.ranges) if (!(r == r.normalized())) return false;
    return curves.isValid() && exposure == exposure.normalized() && gradientMap.shadows == gradientMap.shadows.clamped()
        && gradientMap.highlights == gradientMap.highlights.clamped() && grain.normalized().amount == grain.amount;
}

bool AdjustmentSettings::isIdentity() const {
    switch (kind) {
    case AdjustmentKind::HueSaturation: return hsv.isIdentity();
    case AdjustmentKind::Levels: return levels.isIdentity();
    case AdjustmentKind::Curves: return curves.isIdentity();
    case AdjustmentKind::Exposure: return exposure.normalized().isIdentity();
    case AdjustmentKind::GradientMap: return false;
    case AdjustmentKind::Grain: return grain.normalized().amount <= 0;
    }
    return true;
}

bool applyAdjustment(const AdjustmentSettings& settings, Image& image, const Rect& region, double scale) {
    switch (settings.kind) {
    case AdjustmentKind::HueSaturation: applyHueSaturation(image, settings.hsv); return true;
    case AdjustmentKind::Levels: applyLevels(image, settings.levels); return true;
    case AdjustmentKind::Curves: applyCurves(image, settings.curves); return true;
    case AdjustmentKind::Exposure: applyExposure(image, settings.exposure); return true;
    case AdjustmentKind::GradientMap: applyGradientMap(image, settings.gradientMap); return true;
    case AdjustmentKind::Grain: {
        Rect r = region.isEmpty() ? Rect(0, 0, image.width(), image.height()) : region;
        applyGrain(image, settings.grain, r.origin(), scale > 0 ? 1 / scale : 1);
        return true;
    }
    }
    return false;
}

bool applyAdjustment(const LayerAdjustment& adjustment, Image& image, const Rect& region, double scale) {
    AdjustmentSettings settings;
    if (!AdjustmentSettings::parse(adjustment.json, settings)) return false;
    return applyAdjustment(settings, image, region, scale);
}

std::string defaultAdjustmentJson(AdjustmentKind kind) { return AdjustmentSettings::defaults(kind).toJson(); }

} // namespace compositor
