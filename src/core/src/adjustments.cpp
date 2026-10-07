#include "compositor/adjustments.h"
#include "compositor/kernels.h"
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

} // namespace

// ---- Levels ------------------------------------------------------------------------

const char* levelsChannelName(int channel) {
    static const char* names[] = {"RGB", "Red", "Green", "Blue", "Black"};
    return names[size_t(clamp(channel, 0, levelsChannelCount - 1))];
}

const char* levelsChannelName(int channel, ColorMode mode) {
    static const char* cmyk[] = {"CMYK", "Cyan", "Magenta", "Yellow", "Black"};
    static const char* lab[] = {"", "Lightness", "a", "b", ""};
    if (channel < 0 || channel >= levelsChannelCount) return "";
    if (mode == ColorMode::CMYK) return cmyk[channel];
    if (mode == ColorMode::Lab) return lab[channel];
    return channel < 4 ? levelsChannelName(channel) : "";
}

bool parseLevelsChannel(const std::string& name, int& out) {
    for (int i = 0; i < levelsChannelCount; i++) if (name == levelsChannelName(i)) { out = i; return true; }
    for (ColorMode mode : {ColorMode::CMYK, ColorMode::Lab})
        for (int i = 0; i < levelsChannelCount; i++) if (*levelsChannelName(i, mode) && name == levelsChannelName(i, mode)) { out = i; return true; }
    if (name == "Lab") { out = 0; return true; }
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

// ---- Transfers ----------------------------------------------------------------------

Transfer identityTransfer() {
    Transfer t;
    for (auto& channel : t) for (int i = 0; i < 256; i++) channel[size_t(i)] = i / 255.0f;
    return t;
}

Transfer levelsTransfer(const LevelsSettings& settings) {
    Transfer t;
    for (int c = 0; c < 3; c++) for (int i = 0; i < 256; i++) t[size_t(c)][size_t(i)] = float(settings.apply(i / 255.0, c + 1));
    return t;
}

Transfer composeTransfer(const Transfer& first, const Transfer& second) {
    Transfer t;
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 256; i++) {
            float x = std::clamp(first[size_t(c)][size_t(i)], 0.0f, 1.0f) * 255;
            int lo = std::min(254, int(x));
            float frac = x - lo;
            t[size_t(c)][size_t(i)] = second[size_t(c)][size_t(lo)] * (1 - frac) + second[size_t(c)][size_t(lo + 1)] * frac;
        }
    return t;
}

void applyTransfer(Image& image, const Transfer& transfer) {
    // Quantised to bytes once: the kernel then needs no float work per pixel. Identical results for opaque
    // pixels, within a level at partial alpha.
    kernels::ChannelTables lut;
    bool identity = true;
    for (int c = 0; c < 3; c++)
        for (int i = 0; i < 256; i++) {
            lut.lut[c][i] = uint8_t(std::lround(std::clamp(transfer[size_t(c)][size_t(i)], 0.0f, 1.0f) * 255.0f));
            identity = identity && lut.lut[c][i] == i;
        }
    if (identity) return;
    kernels::applyChannelTables(image, lut);
}

void applyLevels(Image& image, const LevelsSettings& settings) {
    if (settings.isIdentity()) return;
    applyTransfer(image, levelsTransfer(settings));
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
        // The end points may move in from 0 and 255 (Photoshop's black and white input points): flat beyond them.
        if (points.size() < 2 || points.size() > 32) return false;
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
        if (points[0].x != 0 || points[1].x != 255 || points[0].y != 0 || points[1].y != 255) return false;
    }
    return true;
}

double CurvesSettings::value(double x, int channel) const {
    const auto& p = channels[size_t(clamp(channel, 0, levelsChannelCount - 1))];
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

Transfer curvesTransfer(const CurvesSettings& settings) {
    if (!settings.isValid()) return identityTransfer();
    Transfer t;
    for (int c = 1; c <= 3; c++) for (int i = 0; i < 256; i++) t[size_t(c - 1)][size_t(i)] = float(settings.value(settings.value(i, c), 0) / 255);
    return t;
}

void applyCurves(Image& image, const CurvesSettings& settings) {
    if (!settings.isValid() || settings.isIdentity()) return;
    applyTransfer(image, curvesTransfer(settings));
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

Transfer exposureTransfer(const ExposureSettings& settings) {
    std::array<float, 256> table = settings.table();
    return {table, table, table};
}

void applyExposure(Image& image, const ExposureSettings& settings) {
    if (settings.normalized().isIdentity()) return;
    applyTransfer(image, exposureTransfer(settings));
}

// ---- Gradient Map and Grain ----------------------------------------------------------

AdjustmentColor AdjustmentColor::clamped() const { return {clampFinite(red, 0, 1, 0), clampFinite(green, 0, 1, 0), clampFinite(blue, 0, 1, 0)}; }

GradientStops GradientMapSettings::ramp() const {
    const AdjustmentColor dark = (reversed ? highlights : shadows).clamped(), light = (reversed ? shadows : highlights).clamped();
    GradientStops stops;
    stops.start[0] = float(dark.red); stops.start[1] = float(dark.green); stops.start[2] = float(dark.blue); stops.start[3] = 1;
    stops.end[0] = float(light.red); stops.end[1] = float(light.green); stops.end[2] = float(light.blue); stops.end[3] = 1;
    stops.method = method;
    return stops.baked();
}

std::vector<uint8_t> GradientMapSettings::table() const {
    if (method != GradientMethod::Classic) {
        const GradientStops stops = ramp();
        std::vector<uint8_t> result(256 * 3);
        for (int i = 0; i < 256; i++) {
            float c[4];
            stops.sample(i / 255.0f, c);
            for (int k = 0; k < 3; k++) result[size_t(i * 3 + k)] = uint8_t(std::clamp(std::lround(c[k] * 255.0), 0L, 255L));
        }
        return result;
    }
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
    kernels::gradientMap(image, table.data());
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

// Photoshop's Hue/Saturation, ported from Patchy's calibration (src/core/adjustment_layer.cpp and
// docs/adjustments-calibration.md at the commit in src/third_party/patchy_psd/README.md; MIT, (c) 2026 Seth A. Robinson):
// the lightness slider blends each channel towards white or black first; the colour is then read as a lightness
// (max + min) / 2, a half-chroma and a position on a 1530-step hue wheel; saturation scales the half-chroma, the hue
// slider turns the wheel, and the triple is rebuilt. A range's lightness collapses the chroma towards the brightest
// (positive) or darkest (negative) channel before that, and the ranges are chosen by the colour's own hue. On bytes
// the arithmetic is Photoshop's integer one (within 2 levels of its output); on 16 and 32 bits the same model runs
// without the rounding.

// Per-degree hue interpolant (x / 255) within the 60-degree sector, measured for Colorize.
constexpr std::array<uint8_t, 360> colorizeHueInterp = {
      0,   0,   7,  14,  14,  21,  28,  28,  34,  40,  47,  47,  53,  59,  59,
     65,  71,  76,  76,  82,  88,  88,  93,  99, 104, 104, 110, 115, 115, 121,
    126, 132, 132, 137, 142, 142, 148, 153, 159, 159, 165, 170, 170, 176, 182,
    187, 187, 193, 199, 199, 205, 211, 211, 218, 224, 231, 231, 237, 244, 244,
    255, 255, 244, 244, 237, 231, 231, 224, 218, 211, 211, 205, 199, 199, 193,
    187, 182, 182, 176, 170, 170, 165, 159, 153, 153, 148, 142, 142, 137, 132,
    126, 126, 121, 115, 115, 110, 104, 104,  99,  93,  88,  88,  82,  76,  76,
     71,  65,  59,  59,  53,  47,  47,  40,  34,  28,  28,  21,  14,  14,   7,
      0,   0,   0,   7,  14,  14,  21,  28,  34,  34,  40,  47,  47,  53,  59,
     65,  65,  71,  76,  76,  82,  88,  88,  93,  99, 104, 104, 110, 115, 115,
    121, 126, 132, 132, 137, 142, 142, 148, 153, 159, 159, 165, 170, 170, 176,
    182, 187, 187, 193, 199, 199, 205, 211, 218, 218, 224, 231, 231, 237, 244,
    255, 255, 244, 237, 237, 231, 224, 224, 218, 211, 205, 205, 199, 193, 193,
    187, 182, 176, 176, 170, 165, 165, 159, 153, 148, 148, 142, 137, 137, 132,
    126, 121, 121, 115, 110, 110, 104,  99,  93,  93,  88,  82,  82,  76,  71,
     65,  65,  59,  53,  53,  47,  40,  40,  34,  28,  21,  21,  14,   7,   7,
      0,   0,   7,   7,  14,  21,  21,  28,  34,  40,  40,  47,  53,  53,  59,
     65,  71,  71,  76,  82,  82,  88,  93,  99,  99, 104, 110, 110, 115, 121,
    126, 126, 132, 137, 137, 142, 148, 148, 153, 159, 165, 165, 170, 176, 176,
    182, 187, 193, 193, 199, 205, 205, 211, 218, 224, 224, 231, 237, 237, 244,
    255, 255, 255, 244, 237, 237, 231, 224, 218, 218, 211, 205, 205, 199, 193,
    187, 187, 182, 176, 176, 170, 165, 165, 159, 153, 148, 148, 142, 137, 137,
    132, 126, 121, 121, 115, 110, 110, 104,  99,  93,  93,  88,  82,  82,  76,
     71,  65,  65,  59,  53,  53,  47,  40,  34,  34,  28,  21,  21,  14,   7,
};

// Colorize's saturation as a share of the half-chroma available, per percent.
constexpr std::array<double, 101> colorizeSaturationScale = {
    0.000000000, 0.007905262, 0.019710941, 0.027668416, 0.039421881, 0.047270696, 0.059073014, 0.066946710, 0.078843763,
    0.086640420, 0.098455023, 0.110265169, 0.118146027, 0.129960630, 0.137863155, 0.149803150, 0.157507281, 0.169323089,
    0.177190272, 0.189000384, 0.200803537, 0.208678535, 0.220530338, 0.228370759, 0.240176779, 0.249015748, 0.259921260,
    0.267786839, 0.279548726, 0.287450787, 0.299606299, 0.311067367, 0.318931578, 0.330738946, 0.338646177, 0.350410526,
    0.358300525, 0.370104305, 0.378000768, 0.389797144, 0.401607074, 0.409465789, 0.421278069, 0.429150262, 0.441060676,
    0.448841267, 0.460652039, 0.468626969, 0.480353559, 0.488212135, 0.501968504, 0.511857893, 0.519710941, 0.531513797,
    0.539421881, 0.551231577, 0.559073014, 0.571147357, 0.578843763, 0.590730136, 0.602385922, 0.610265169, 0.622070134,
    0.629960630, 0.641761664, 0.649803150, 0.661437828, 0.669323089, 0.681130891, 0.689000384, 0.700803537, 0.712621052,
    0.720530338, 0.732303348, 0.740176779, 0.751984252, 0.759921260, 0.771696337, 0.779548726, 0.791502625, 0.803170548,
    0.811067367, 0.822875656, 0.830738946, 0.842556139, 0.850410526, 0.862224811, 0.870104305, 0.881917104, 0.889797144,
    0.901607074, 0.913423683, 0.921278069, 0.933202100, 0.941060676, 0.952793047, 0.960652039, 0.972459005, 0.980353559,
    0.992156742, 1.003952500,
};

// The Saturation slider's multiplier of the half-chroma, indexed percent + 100 (no closed form reproduces it).
constexpr std::array<double, 201> masterSaturationScale = {
    0.000000000, 0.015503876, 0.027027027, 0.034482759, 0.047619048, 0.054545455, 0.066666667, 0.074074074, 0.085714286,
    0.097560976, 0.105263158, 0.117647059, 0.125000000, 0.136363636, 0.142857143, 0.155963303, 0.163934426, 0.176470588,
    0.187500000, 0.195121951, 0.206896552, 0.214285714, 0.226415094, 0.235294118, 0.247311828, 0.253333333, 0.266666667,
    0.277777778, 0.285714286, 0.296296296, 0.304347826, 0.315789474, 0.324324324, 0.333333333, 0.347826087, 0.355555556,
    0.368421053, 0.375000000, 0.387096774, 0.393939394, 0.406250000, 0.413793103, 0.425531915, 0.437500000, 0.444444444,
    0.457142857, 0.465116279, 0.476190476, 0.483870968, 0.496062992, 0.500000000, 0.515151515, 0.527272727, 0.534883721,
    0.545454545, 0.555555556, 0.566037736, 0.574468085, 0.586206897, 0.600000000, 0.606060606, 0.617021277, 0.625000000,
    0.636363636, 0.645161290, 0.656000000, 0.666666667, 0.675675676, 0.687500000, 0.695652174, 0.707317073, 0.714285714,
    0.727272727, 0.733333333, 0.747368421, 0.753246753, 0.764705882, 0.777777778, 0.785714286, 0.800000000, 0.804878049,
    0.816326531, 0.823529412, 0.836363636, 0.847457627, 0.857142857, 0.866666667, 0.875000000, 0.886792453, 0.894736842,
    0.905982906, 0.914285714, 0.925925926, 0.937500000, 0.945454545, 0.956521739, 0.965517241, 0.976744186, 0.984126984,
    1.000000000, 1.000000000, 1.011764706, 1.023255814, 1.031250000, 1.043478261, 1.050847458, 1.066666667, 1.074074074,
    1.086956522, 1.097560976, 1.111111111, 1.121212121, 1.137254902, 1.153846154, 1.160000000, 1.176470588, 1.187500000,
    1.205128205, 1.216216216, 1.235294118, 1.247311828, 1.266666667, 1.277777778, 1.297872340, 1.310344828, 1.333333333,
    1.352941176, 1.368421053, 1.388888889, 1.403508772, 1.428571429, 1.444444444, 1.470588235, 1.485714286, 1.514285714,
    1.529411765, 1.560000000, 1.575757576, 1.608695652, 1.640000000, 1.658536585, 1.692307692, 1.716981132, 1.750000000,
    1.777777778, 1.811594203, 1.838709677, 1.882352941, 1.909090909, 1.952380952, 2.000000000, 2.030769231, 2.076923077,
    2.111111111, 2.166666667, 2.200000000, 2.263157895, 2.307692308, 2.368421053, 2.411764706, 2.481481481, 2.533333333,
    2.609756098, 2.692307692, 2.750000000, 2.842105263, 2.904761905, 3.000000000, 3.081081081, 3.200000000, 3.275862069,
    3.411764706, 3.500000000, 3.666666667, 3.761904762, 3.933333333, 4.125000000, 4.263157895, 4.500000000, 4.666666667,
    4.923076923, 5.117647059, 5.444444444, 5.692307692, 6.090909091, 6.400000000, 6.909090909, 7.333333333, 8.000000000,
    8.818181818, 9.500000000, 10.666666667, 11.666666667, 13.500000000, 15.000000000, 18.250000000, 21.333333333,
    28.400000000, 36.500000000, 64.000000000, 128.000000000,
};

/// A table read at a fractional index, linearly between its entries (sliders are whole numbers in Photoshop).
template <size_t N>
double tableAt(const std::array<double, N>& table, double index) {
    index = std::clamp(index, 0.0, double(N - 1));
    const size_t i = std::min(N - 2, size_t(index));
    return table[i] + (table[i + 1] - table[i]) * (index - double(i));
}

/// The 1530-step wheel position (six sectors of 255) of a colour on the 0..255 scale.
double wheelPosition(const double c[3]) {
    const double r = c[0], g = c[1], b = c[2];
    const double high = std::max({r, g, b}), low = std::min({r, g, b}), span = high - low;
    if (span <= 0) return 0;
    auto ramp = [span](double middle, double bottom) { return 255 * (middle - bottom) / span; };
    if (r == high && b == low) return ramp(g, b);               // red -> yellow
    if (g == high && b == low) return 510 - ramp(r, b);         // yellow -> green
    if (g == high && r == low) return 510 + ramp(b, r);         // green -> cyan
    if (b == high && r == low) return 1020 - ramp(g, r);        // cyan -> blue
    if (b == high && g == low) return 1020 + ramp(r, g);        // blue -> magenta
    return 1530 - ramp(b, g);                                   // magenta -> red
}

void wheelSplit(double position, int& sector, double& interpolant) {
    position = std::fmod(position, 1530.0);
    if (position < 0) position += 1530;
    sector = std::min(5, int(position / 255));
    const double offset = position - sector * 255.0;
    interpolant = sector % 2 == 0 ? offset : 255 - offset;
}

/// The colour at a lightness and half-chroma in a sector. On bytes (`exact`) the brightest channel rounds and the
/// darkest truncates, which makes all-zero sliders an exact identity.
void hslRebuild(double light, double halfChroma, int sector, double interpolant, bool exact, double out[3]) {
    double q, p, m;
    if (exact) {
        q = std::min(255.0, light + std::floor(halfChroma + 0.5));
        p = std::max(0.0, light - std::floor(halfChroma));
        m = p + std::floor((q - p) * interpolant / 255 + 0.5);
    } else {
        q = std::min(255.0, light + halfChroma);
        p = std::max(0.0, light - halfChroma);
        m = p + (q - p) * interpolant / 255;
    }
    switch (sector) {
    case 0: out[0] = q; out[1] = m; out[2] = p; break;
    case 1: out[0] = m; out[1] = q; out[2] = p; break;
    case 2: out[0] = p; out[1] = q; out[2] = m; break;
    case 3: out[0] = p; out[1] = m; out[2] = q; break;
    case 4: out[0] = m; out[1] = p; out[2] = q; break;
    default: out[0] = q; out[1] = p; out[2] = m; break;
    }
}

/// The Lightness slider on one channel: Photoshop quantises the percent to a byte step first.
double lightnessValue(double value, double lightness, bool exact) {
    const double step = std::floor(std::fabs(lightness) * 255 / 100);
    double v = value;
    if (lightness > 0) v = value + (255 - value) * step / 255;
    else if (lightness < 0) v = value * (255 - step) / 255;
    return exact ? std::floor(v + 0.5) : v;
}

struct HueSatModel {
    HueSaturationSettings settings;
    bool colorize = false;
    // Colorize
    int colorizeHue = 0;
    double colorizeScale = 0, colorizeLightness = 0;
    // Master
    double lightness = 0, ratio = 1, rotation = 0;
    struct Range { int range; double hue, lightness, saturationOffset; };
    std::vector<Range> ranges;
    std::array<uint8_t, 256> ramp{};

    explicit HueSatModel(const HueSaturationSettings& s) : settings(s), colorize(s.colorize) {
        auto saturationRatio = [&](double percent) {
            percent = std::clamp(percent, -100.0, 100.0);
            return s.photoshopSaturation ? tableAt(masterSaturationScale, percent + 100) : std::max(0.0, 1 + percent / 100);
        };
        if (colorize) {
            const RangeAdjustment a = s.currentValue();
            colorizeHue = int(std::lround(wrap360(a.hue))) % 360;
            colorizeScale = tableAt(colorizeSaturationScale, std::clamp(a.saturation, 0.0, 100.0));
            colorizeLightness = std::clamp(a.lightness, -100.0, 100.0);
            return;
        }
        RangeAdjustment master;
        if (auto it = s.adjustments.find(0); it != s.adjustments.end()) master = it->second;
        lightness = std::clamp(master.lightness, -100.0, 100.0);
        ratio = saturationRatio(master.saturation);
        rotation = std::floor(std::clamp(master.hue, -180.0, 180.0) * 4.25 + 0.5);
        for (auto& [range, a] : s.adjustments) {
            if (range < 1 || range > 6 || a == RangeAdjustment{}) continue;
            ranges.push_back({range, std::clamp(a.hue, -180.0, 180.0), std::clamp(a.lightness, -100.0, 100.0),
                              a.saturation != 0 ? saturationRatio(a.saturation) - 1 : 0.0});
        }
        for (int v = 0; v < 256; v++) ramp[size_t(v)] = uint8_t(lightnessValue(v, lightness, true));
    }

    /// One colour on the 0..255 scale; whole numbers in and out when `exact`.
    void apply(double c[3], bool exact) const {
        if (colorize) {
            const double high = std::max({c[0], c[1], c[2]}), low = std::min({c[0], c[1], c[2]});
            const double light = lightnessValue(exact ? std::floor((high + low) / 2) : (high + low) / 2, colorizeLightness, exact);
            const double halfChroma = std::min(light, 255 - light) * colorizeScale;
            hslRebuild(light, halfChroma, colorizeHue / 60, colorizeHueInterp[size_t(colorizeHue)], exact, c);
            return;
        }
        // The ranges, chosen by the colour's own hue (a master rotation does not move them); their lightness first.
        double weights[7] = {0, 0, 0, 0, 0, 0, 0};
        if (!ranges.empty()) {
            const double wheel = wheelPosition(c);
            int sector;
            double interpolant;
            wheelSplit(wheel, sector, interpolant);
            double high = std::max({c[0], c[1], c[2]}), low = std::min({c[0], c[1], c[2]});
            double bandLightness = 0;
            if (high > low)
                for (const Range& r : ranges) {
                    weights[r.range] = settings.weight(r.range, wheel / 4.25);
                    bandLightness += weights[r.range] * r.lightness;
                }
            if (bandLightness > 0) low += (high - low) * std::min(bandLightness, 100.0) / 100;
            else if (bandLightness < 0) high += (low - high) * std::min(-bandLightness, 100.0) / 100;
            if (bandLightness != 0) {
                if (exact) { high = std::clamp(std::floor(high + 0.5), 0.0, 255.0); low = std::clamp(std::floor(low + 0.5), 0.0, 255.0); }
                const double light = exact ? std::floor((high + low) / 2) : (high + low) / 2;
                hslRebuild(light, (high - low) / 2, sector, interpolant, exact, c);
            }
        }
        double lit[3];
        for (int i = 0; i < 3; i++) lit[i] = exact ? ramp[size_t(std::clamp(int(c[i]), 0, 255))] : lightnessValue(c[i], lightness, false);
        const double high = std::max({lit[0], lit[1], lit[2]}), low = std::min({lit[0], lit[1], lit[2]});
        if (high == low) { c[0] = lit[0]; c[1] = lit[1]; c[2] = lit[2]; return; }   // neutrals are never tinted
        const double light = exact ? std::floor((high + low) / 2) : (high + low) / 2;
        const double half = (high - low) / 2;
        // The ranges' saturation offsets from 1 add up and multiply the master's; their rotations add, in whole steps.
        double offset = 0, turn = rotation;
        for (const Range& r : ranges) {
            const double w = weights[r.range];
            if (w <= 0) continue;
            offset += w * r.saturationOffset;
            if (r.hue != 0) turn += exact ? std::floor(w * r.hue * 4.25 + 0.5) : w * r.hue * 4.25;
        }
        const double limit = std::max(std::min(light, 255 - light), half);
        const double halfChroma = std::min(half * ratio * std::max(0.0, 1 + offset), limit);
        int sector;
        double interpolant;
        wheelSplit(wheelPosition(lit) + turn, sector, interpolant);
        hslRebuild(light, halfChroma, sector, interpolant, exact, c);
    }
};

} // namespace

void HueSaturationSettings::adjust(double& r, double& g, double& b) const { adjuster()(r, g, b); }

std::function<void(double&, double&, double&)> HueSaturationSettings::adjuster() const {
    auto model = std::make_shared<const HueSatModel>(*this);
    return [model](double& r, double& g, double& b) {
        double c[3] = {std::clamp(r, 0.0, 1.0) * 255, std::clamp(g, 0.0, 1.0) * 255, std::clamp(b, 0.0, 1.0) * 255};
        model->apply(c, false);
        r = c[0] / 255; g = c[1] / 255; b = c[2] / 255;
    };
}

double HueSaturationSettings::shiftedHue(double hue) const {
    double shift = 0;
    for (auto& [range, a] : adjustments) if (a.hue != 0) shift += a.hue * weight(range, hue);
    return wrap360(hue + shift);
}

void applyHueSaturation(Image& image, const HueSaturationSettings& settings) {
    if (settings.isIdentity()) return;
    // Photoshop's byte arithmetic on each straight colour.
    const HueSatModel model(settings);
    parallelRows(0, image.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            uint8_t* p = image.row(y);
            for (int x = 0; x < image.width(); x++, p += 4) {
                const int a = p[3];
                if (!a) continue;
                double c[3];
                for (int i = 0; i < 3; i++) c[i] = a == 255 ? p[i] : std::min(255, (p[i] * 255 + a / 2) / a);
                model.apply(c, true);
                for (int i = 0; i < 3; i++) {
                    const int v = std::clamp(int(c[i]), 0, 255);
                    p[i] = uint8_t(a == 255 ? v : (v * a + 127) / 255);
                }
            }
        }
    });
}

// ---- Invert ---------------------------------------------------------------------------

void applyInvert(Image& image) { kernels::invertColors(image); }

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
    // Four slots (RGB's), or five with CMYK's black.
    if (ranges == j.end() || !ranges->is_array() || ranges->size() < 4 || ranges->size() > size_t(levelsChannelCount)) return false;
    out.ranges[4] = LevelsRange();
    for (size_t i = 0; i < ranges->size(); i++) {
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
    // The fifth slot (CMYK's black) only when it is set, so RGB settings read as they always have.
    const size_t count = s.ranges[4] == LevelsRange() ? 4 : 5;
    for (size_t i = 0; i < count; i++) {
        const LevelsRange& r = s.ranges[i];
        j["ranges"].push_back({{"black", number(r.black)}, {"gamma", number(r.gamma)}, {"white", number(r.white)}, {"outputBlack", number(r.outputBlack)}, {"outputWhite", number(r.outputWhite)}});
    }
    return j;
}

bool parseCurves(const json& j, CurvesSettings& out) {
    if (!j.is_object()) return false;
    if (!parseLevelsChannel(j.value("channel", "RGB"), out.channel)) return false;
    auto channels = j.find("channels");
    if (channels == j.end() || !channels->is_array() || channels->size() < 4 || channels->size() > size_t(levelsChannelCount)) return false;
    out.channels[4] = {{0, 0}, {255, 255}};
    for (size_t c = 0; c < channels->size(); c++) {
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
    const bool black = !(s.channels[4] == std::vector<CurvePoint>{{0, 0}, {255, 255}});
    for (size_t c = 0; c < (black ? 5u : 4u); c++) {
        json arr = json::array();
        for (auto& p : s.channels[c]) arr.push_back({{"x", number(p.x)}, {"y", number(p.y)}});
        j["channels"].push_back(arr);
    }
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
    std::string curve = j.value("saturationCurve", "scale");
    if (curve != "scale" && curve != "photoshop") return false;
    out.photoshopSaturation = curve == "photoshop";
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
    if (s.photoshopSaturation) j["saturationCurve"] = "photoshop";
    json adjustments = json::array();
    for (auto& [range, a] : s.adjustments) { adjustments.push_back(colorRangeName(range)); adjustments.push_back({{"hue", number(a.hue)}, {"saturation", number(a.saturation)}, {"lightness", number(a.lightness)}}); }
    j["adjustments"] = adjustments;
    json bands = json::array();
    for (auto& [range, b] : s.bands) { bands.push_back(colorRangeName(range)); bands.push_back({{"falloffStart", number(b.falloffStart)}, {"rangeStart", number(b.rangeStart)}, {"rangeEnd", number(b.rangeEnd)}, {"falloffEnd", number(b.falloffEnd)}}); }
    j["bands"] = bands;
    return j;
}

const char* knownKeys[] = {"kind", "hue", "saturation", "lightness", "colorize", "hsvSettings", "levels", "curves", "exposureSettings", "gradientMapSettings", "grainSettings",
                           "brightnessContrastSettings", "posterizeSettings", "thresholdSettings", "blackWhiteSettings", "colorBalanceSettings", "vibranceSettings",
                           "photoFilterSettings", "channelMixerSettings", "selectiveColorSettings", "colorLookupSettings"};

json colourJson(const AdjustmentColor& c) { return {{"red", number(c.red)}, {"green", number(c.green)}, {"blue", number(c.blue)}}; }
bool colourFrom(const json& j, AdjustmentColor& c) {
    if (!j.is_object()) return false;
    c = {num(j, "red", 0), num(j, "green", 0), num(j, "blue", 0)};
    return c == c.clamped();
}
/// A JSON array of `n` numbers within [lo, hi].
template <size_t N>
bool numbersFrom(const json& j, std::array<double, N>& out, double lo, double hi) {
    if (!j.is_array() || j.size() != N) return false;
    for (size_t i = 0; i < N; i++) {
        if (!j[i].is_number()) return false;
        out[i] = j[i].get<double>();
        if (!(out[i] >= lo && out[i] <= hi)) return false;
    }
    return true;
}
template <size_t N> json numbersJson(const std::array<double, N>& v) { json a = json::array(); for (double x : v) a.push_back(number(x)); return a; }

// The settings of Photoshop's other adjustment layers (adjustments_more.cpp): false on a malformed or out-of-range value.
bool parseMore(const json& j, AdjustmentSettings& s) {
    if (auto it = j.find("brightnessContrastSettings"); it != j.end() && it->is_object()) {
        s.brightnessContrast = {int(num(*it, "brightness", 0)), int(num(*it, "contrast", 0)), boolean(*it, "legacy", false)};
        if (!(s.brightnessContrast == s.brightnessContrast.normalized())) return false;
    }
    if (auto it = j.find("posterizeSettings"); it != j.end() && it->is_object()) {
        s.posterize.levels = int(num(*it, "levels", 4));
        if (s.posterize.levels < 2 || s.posterize.levels > 255) return false;
    }
    if (auto it = j.find("thresholdSettings"); it != j.end() && it->is_object()) {
        s.threshold.level = int(num(*it, "level", 128));
        if (s.threshold.level < 1 || s.threshold.level > 255) return false;
    }
    if (auto it = j.find("blackWhiteSettings"); it != j.end() && it->is_object()) {
        if (auto w = it->find("weights"); w != it->end() && !numbersFrom(*w, s.blackWhite.weights, -200, 300)) return false;
        s.blackWhite.tint = boolean(*it, "tint", false);
        if (auto c = it->find("tintColor"); c != it->end() && !colourFrom(*c, s.blackWhite.tintColor)) return false;
    }
    if (auto it = j.find("colorBalanceSettings"); it != j.end() && it->is_object()) {
        const char* names[3] = {"shadows", "midtones", "highlights"};
        for (size_t r = 0; r < 3; r++)
            if (auto v = it->find(names[r]); v != it->end() && !numbersFrom(*v, s.colorBalance.ranges[r], -100, 100)) return false;
        s.colorBalance.preserveLuminosity = boolean(*it, "preserveLuminosity", true);
    }
    if (auto it = j.find("vibranceSettings"); it != j.end() && it->is_object()) {
        s.vibrance = {num(*it, "vibrance", 0), num(*it, "saturation", 0)};
        if (std::fabs(s.vibrance.vibrance) > 100 || std::fabs(s.vibrance.saturation) > 100) return false;
    }
    if (auto it = j.find("photoFilterSettings"); it != j.end() && it->is_object()) {
        if (auto c = it->find("color"); c != it->end() && !colourFrom(*c, s.photoFilter.color)) return false;
        s.photoFilter.density = num(*it, "density", 25);
        s.photoFilter.preserveLuminosity = boolean(*it, "preserveLuminosity", true);
        if (!(s.photoFilter.density >= 0 && s.photoFilter.density <= 100)) return false;
    }
    if (auto it = j.find("channelMixerSettings"); it != j.end() && it->is_object()) {
        s.channelMixer.monochrome = boolean(*it, "monochrome", false);
        const char* names[4] = {"red", "green", "blue", "gray"};
        for (size_t r = 0; r < 4; r++)
            if (auto v = it->find(names[r]); v != it->end() && !numbersFrom(*v, s.channelMixer.rows[r], -200, 200)) return false;
        const char* inks[4] = {"cyan", "magenta", "yellow", "black"};
        for (size_t r = 0; r < 4; r++)
            if (auto v = it->find(inks[r]); v != it->end() && !numbersFrom(*v, s.channelMixer.inks[r], -200, 200)) return false;
    }
    if (auto it = j.find("colorLookupSettings"); it != j.end() && it->is_object()) {
        auto text = [&](const char* k) { auto v = it->find(k); return v != it->end() && v->is_string() ? v->get<std::string>() : std::string(); };
        s.colorLookup = {text("name"), text("format"), text("data"), boolean(*it, "dither", false)};
        if (!s.colorLookup.format.empty() && s.colorLookup.format != "cube" && s.colorLookup.format != "3dl" && s.colorLookup.format != "icc") return false;
    }
    if (auto it = j.find("selectiveColorSettings"); it != j.end() && it->is_object()) {
        s.selectiveColor.absolute = boolean(*it, "absolute", false);
        const char* names[9] = {"reds", "yellows", "greens", "cyans", "blues", "magentas", "whites", "neutrals", "blacks"};
        for (size_t r = 0; r < 9; r++)
            if (auto v = it->find(names[r]); v != it->end() && !numbersFrom(*v, s.selectiveColor.ranges[r], -100, 100)) return false;
    }
    return true;
}

void moreJson(const AdjustmentSettings& s, json& j) {
    // Written for the kind that uses them only, so older manifests stay as they were.
    switch (s.kind) {
    case AdjustmentKind::BrightnessContrast:
        j["brightnessContrastSettings"] = {{"brightness", s.brightnessContrast.brightness}, {"contrast", s.brightnessContrast.contrast}, {"legacy", s.brightnessContrast.legacy}};
        break;
    case AdjustmentKind::Posterize: j["posterizeSettings"] = {{"levels", s.posterize.levels}}; break;
    case AdjustmentKind::Threshold: j["thresholdSettings"] = {{"level", s.threshold.level}}; break;
    case AdjustmentKind::BlackWhite:
        j["blackWhiteSettings"] = {{"weights", numbersJson(s.blackWhite.weights)}, {"tint", s.blackWhite.tint}, {"tintColor", colourJson(s.blackWhite.tintColor)}};
        break;
    case AdjustmentKind::ColorBalance:
        j["colorBalanceSettings"] = {{"shadows", numbersJson(s.colorBalance.ranges[0])}, {"midtones", numbersJson(s.colorBalance.ranges[1])},
                                     {"highlights", numbersJson(s.colorBalance.ranges[2])}, {"preserveLuminosity", s.colorBalance.preserveLuminosity}};
        break;
    case AdjustmentKind::Vibrance: j["vibranceSettings"] = {{"vibrance", number(s.vibrance.vibrance)}, {"saturation", number(s.vibrance.saturation)}}; break;
    case AdjustmentKind::PhotoFilter:
        j["photoFilterSettings"] = {{"color", colourJson(s.photoFilter.color)}, {"density", number(s.photoFilter.density)}, {"preserveLuminosity", s.photoFilter.preserveLuminosity}};
        break;
    case AdjustmentKind::ChannelMixer:
        j["channelMixerSettings"] = {{"monochrome", s.channelMixer.monochrome}, {"red", numbersJson(s.channelMixer.rows[0])}, {"green", numbersJson(s.channelMixer.rows[1])},
                                     {"blue", numbersJson(s.channelMixer.rows[2])}, {"gray", numbersJson(s.channelMixer.rows[3])}};
        // CMYK's rows only when set, so RGB settings read as they always have.
        if (s.channelMixer.inks != ChannelMixerSettings().inks) {
            const char* inks[4] = {"cyan", "magenta", "yellow", "black"};
            for (size_t r = 0; r < 4; r++) j["channelMixerSettings"][inks[r]] = numbersJson(s.channelMixer.inks[r]);
        }
        break;
    case AdjustmentKind::ColorLookup:
        j["colorLookupSettings"] = {{"name", s.colorLookup.name}, {"format", s.colorLookup.format}, {"data", s.colorLookup.data}, {"dither", s.colorLookup.dither}};
        break;
    case AdjustmentKind::SelectiveColor: {
        json r = {{"absolute", s.selectiveColor.absolute}};
        const char* names[9] = {"reds", "yellows", "greens", "cyans", "blues", "magentas", "whites", "neutrals", "blacks"};
        for (size_t i = 0; i < 9; i++) r[names[i]] = numbersJson(s.selectiveColor.ranges[i]);
        j["selectiveColorSettings"] = r;
        break;
    }
    default: break;
    }
}

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
        auto method = gradient->find("interpolation");
        if (method != gradient->end()) {
            if (!method->is_string() || !parseGradientMethod(method->get<std::string>(), s.gradientMap.method)) return false;
        }
    }
    auto grain = j.find("grainSettings");
    if (grain != j.end() && grain->is_object()) {
        s.grain = {num(*grain, "amount", 25), num(*grain, "size", 1.5), num(*grain, "roughness", 50), uint32_t(num(*grain, "seed", 0))};
        GrainSettings n = s.grain.normalized();
        if (n.amount != s.grain.amount || n.size != s.grain.size || n.roughness != s.grain.roughness) return false;
    }
    if (!parseMore(j, s)) return false;
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
                                {"reversed", gradientMap.reversed}, {"interpolation", gradientMethodKey(gradientMap.method)}};
    j["grainSettings"] = {{"amount", number(grain.amount)}, {"size", number(grain.size)}, {"roughness", number(grain.roughness)}, {"seed", grain.seed}};
    moreJson(*this, j);
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
    case AdjustmentKind::Invert: case AdjustmentKind::Posterize: case AdjustmentKind::Threshold:
    case AdjustmentKind::BlackWhite: case AdjustmentKind::PhotoFilter: return false;
    case AdjustmentKind::BrightnessContrast: return brightnessContrast.brightness == 0 && brightnessContrast.contrast == 0;
    case AdjustmentKind::ColorBalance: {
        for (auto& r : colorBalance.ranges) for (double v : r) if (v != 0) return false;
        return true;
    }
    case AdjustmentKind::Vibrance: return vibrance.vibrance == 0 && vibrance.saturation == 0;
    case AdjustmentKind::ChannelMixer: return !channelMixer.monochrome && channelMixer == ChannelMixerSettings{};
    case AdjustmentKind::SelectiveColor: {
        for (auto& r : selectiveColor.ranges) for (double v : r) if (v != 0) return false;
        return true;
    }
    case AdjustmentKind::ColorLookup: return !colorLookupReadable(colorLookup);
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
    case AdjustmentKind::Invert: applyTransfer(image, invertTransfer()); return true;
    case AdjustmentKind::BrightnessContrast: applyTransfer(image, brightnessContrastTransfer(settings.brightnessContrast)); return true;
    case AdjustmentKind::Posterize: applyTransfer(image, posterizeTransfer(settings.posterize)); return true;
    case AdjustmentKind::Threshold: applyThreshold(image, settings.threshold); return true;
    case AdjustmentKind::BlackWhite: applyBlackWhite(image, settings.blackWhite); return true;
    case AdjustmentKind::ColorBalance: applyColorBalance(image, settings.colorBalance); return true;
    case AdjustmentKind::Vibrance: applyVibrance(image, settings.vibrance); return true;
    case AdjustmentKind::PhotoFilter: applyPhotoFilter(image, settings.photoFilter); return true;
    case AdjustmentKind::ChannelMixer: applyChannelMixer(image, settings.channelMixer); return true;
    case AdjustmentKind::SelectiveColor: applySelectiveColor(image, settings.selectiveColor); return true;
    case AdjustmentKind::ColorLookup: applyColorLookup(image, settings.colorLookup); return true;
    }
    return false;
}

bool applyAdjustment(const LayerAdjustment& adjustment, Image& image, const Rect& region, double scale) {
    AdjustmentSettings settings;
    if (!AdjustmentSettings::parse(adjustment.json, settings)) return false;
    return applyAdjustment(settings, image, region, scale);
}

std::optional<Transfer> adjustmentTransfer(const AdjustmentSettings& settings) {
    switch (settings.kind) {
    case AdjustmentKind::Levels: return levelsTransfer(settings.levels);
    case AdjustmentKind::Curves: return curvesTransfer(settings.curves);
    case AdjustmentKind::Exposure: return exposureTransfer(settings.exposure);
    case AdjustmentKind::Invert: return invertTransfer();
    case AdjustmentKind::BrightnessContrast: return brightnessContrastTransfer(settings.brightnessContrast);
    case AdjustmentKind::Posterize: return posterizeTransfer(settings.posterize);
    default: return std::nullopt;
    }
}

std::string defaultAdjustmentJson(AdjustmentKind kind) { return AdjustmentSettings::defaults(kind).toJson(); }

} // namespace compositor
