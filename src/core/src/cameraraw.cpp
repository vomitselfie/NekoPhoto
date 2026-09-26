// Camera Raw Filter. A port of upstream Compositor (MIT, see LICENSES/MIT-Compositor.txt):
// Compositor/Document/CameraRaw.swift (settings, gains, neutralize, auto balance, the apply order),
// CameraRawColor.swift (curve, mixer, grading), CameraRawDetailOptics.swift and
// CameraRawGeometryCalibration.swift. Upstream warps Geometry with Core Image's CIPerspectiveTransform;
// here the same corners drive a homography resampled bilinearly.
#include "compositor/cameraraw.h"
#include "compositor/parallel.h"
#include "compositor/resample.h"
#include "compositor/warp.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>

extern "C" {
#include "AdjustPixels.h"
#include "CameraRawPixels.h"
}

namespace compositor {

namespace {

using json = nlohmann::json;

/// Lens Correction's strength, shared with Filter > Lens Correction (upstream PixelFilter.lensStrength).
constexpr double lensStrength = 0.35;

double clampTo(double value, double lo, double hi, double fallback) {
    return std::isfinite(value) ? std::min(hi, std::max(lo, value)) : fallback;
}

bool within(double value, double lo, double hi) { return std::isfinite(value) && value >= lo && value <= hi; }

double decode(double encoded) { return encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4); }

std::vector<CameraRawCurvePoint> repair(const std::vector<CameraRawCurvePoint>& points) {
    std::vector<CameraRawCurvePoint> sorted;
    for (const auto& p : points) if (std::isfinite(p.x) && std::isfinite(p.y)) sorted.push_back(p);
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.x < b.x; });
    if (sorted.size() < 2) return CameraRawCurveSettings::linear();
    sorted.front() = {0, std::clamp(sorted.front().y, 0.0, 1.0)};
    sorted.back() = {1, std::clamp(sorted.back().y, 0.0, 1.0)};
    std::vector<CameraRawCurvePoint> kept{sorted.front()};
    for (size_t i = 1; i + 1 < sorted.size(); i++) {
        double x = std::clamp(sorted[i].x, 0.01, 0.99);
        if (!(x > kept.back().x + 0.01)) continue;
        kept.push_back({x, std::clamp(sorted[i].y, 0.0, 1.0)});
    }
    kept.push_back(sorted.back());
    return kept;
}

/// The point curve through CurvesSettings' shape-preserving interpolation, as upstream does (0…1 in and out).
double curveAt(double x, const std::vector<CameraRawCurvePoint>& points) {
    if (points.size() < 2) return x;
    CurvesSettings curve;
    curve.channels[0].clear();
    for (const auto& p : points) curve.channels[0].push_back({p.x * 255, p.y * 255});
    return curve.value(x * 255, 0) / 255;
}

/// Runs a per-pixel kernel over bands of rows in parallel.
void perRows(Image& image, const std::function<void(uint8_t*, size_t)>& body) {
    parallelRows(0, image.height(), [&](int y0, int y1) { body(image.row(y0), size_t(y1 - y0)); });
}

} // namespace

// ---- names ------------------------------------------------------------------------------------

const char* cameraRawName(CameraRawWhiteBalance v) { return v == CameraRawWhiteBalance::Auto ? "Auto" : "Custom"; }
const char* cameraRawName(CameraRawGlowStyle v) {
    switch (v) { case CameraRawGlowStyle::Bloom: return "Bloom"; case CameraRawGlowStyle::Halation: return "Halation"; default: return "Diffusion"; }
}
const char* cameraRawName(CameraRawVignetteStyle v) {
    switch (v) { case CameraRawVignetteStyle::ColorPriority: return "Color Priority"; case CameraRawVignetteStyle::PaintOverlay: return "Paint Overlay"; default: return "Highlight Priority"; }
}
const char* cameraRawName(CameraRawUprightMode v) { return v == CameraRawUprightMode::Guided ? "Guided" : "Off"; }
const char* cameraRawName(CameraRawProjection v) { return v == CameraRawProjection::Rectilinear ? "Rectilinear" : "Perspective"; }

// ---- Curve ------------------------------------------------------------------------------------

bool CameraRawCurveSettings::isLinear(const std::vector<CameraRawCurvePoint>& p) {
    return p.size() == 2 && p[0].x == 0 && p[0].y == 0 && p[1].x == 1 && p[1].y == 1;
}

bool CameraRawCurveSettings::adjusts() const {
    return shadows != 0 || darks != 0 || lights != 0 || highlights != 0 || refineSaturation != 0
        || !isLinear(rgb) || !isLinear(red) || !isLinear(green) || !isLinear(blue);
}

double CameraRawCurveSettings::parametric(double tone) const {
    const double shadow = shadowSplit / 100, dark = darkSplit / 100, light = lightSplit / 100;
    double amount, lo, hi;
    if (tone < shadow) { amount = shadows; lo = 0; hi = shadow; }
    else if (tone < dark) { amount = darks; lo = shadow; hi = dark; }
    else if (tone < light) { amount = lights; lo = dark; hi = light; }
    else { amount = highlights; lo = light; hi = 1; }
    const double span = std::max(0.02, hi - lo);
    const double weight = 1 - std::fabs(tone - (lo + hi) / 2) / (span / 2);
    return std::min(1.0, std::max(0.0, tone + (amount / 100) * std::max(0.0, weight) * 0.22));
}

std::vector<float> CameraRawCurveSettings::lumaTable() const {
    std::vector<float> table(256);
    for (int i = 0; i < 256; i++) table[size_t(i)] = float(curveAt(parametric(i / 255.0), rgb));
    return table;
}

std::vector<float> CameraRawCurveSettings::channelTable(const std::vector<CameraRawCurvePoint>& points) const {
    std::vector<float> table(256);
    for (int i = 0; i < 256; i++) table[size_t(i)] = float(curveAt(i / 255.0, points));
    return table;
}

CameraRawCurveSettings CameraRawCurveSettings::normalized() const {
    CameraRawCurveSettings r = *this;
    r.shadows = clampTo(shadows, -100, 100, 0);
    r.darks = clampTo(darks, -100, 100, 0);
    r.lights = clampTo(lights, -100, 100, 0);
    r.highlights = clampTo(highlights, -100, 100, 0);
    r.refineSaturation = clampTo(refineSaturation, -100, 100, 0);
    r.shadowSplit = clampTo(shadowSplit, 5, 90, 25);
    r.darkSplit = clampTo(darkSplit, r.shadowSplit + 2, 95, 50);
    r.lightSplit = clampTo(lightSplit, r.darkSplit + 2, 98, 75);
    r.rgb = repair(rgb);
    r.red = repair(red);
    r.green = repair(green);
    r.blue = repair(blue);
    return r;
}

// ---- Color Mixer and Grading --------------------------------------------------------------------

const std::array<const char*, 8> CameraRawMixerSettings::names{"Reds", "Oranges", "Yellows", "Greens", "Aquas", "Blues", "Purples", "Magentas"};
const std::array<double, 8> CameraRawMixerSettings::centers{0, 30, 60, 120, 180, 240, 270, 300};

bool CameraRawMixerSettings::adjusts() const {
    auto any = [](const std::array<double, 8>& a) { return std::any_of(a.begin(), a.end(), [](double v) { return v != 0; }); };
    return any(hue) || any(saturation) || any(luminance)
        || std::any_of(points.begin(), points.end(), [](const auto& p) { return p.hueShift != 0 || p.saturationShift != 0 || p.luminanceShift != 0; });
}

std::array<double, 8> CameraRawMixerSettings::weights(double degrees) {
    std::array<double, 8> out{};
    for (size_t i = 0; i < 8; i++) {
        double distance = std::fabs(degrees - centers[i]);
        if (distance > 180) distance = 360 - distance;
        out[i] = std::max(0.0, 1 - distance / 40);
    }
    return out;
}

CameraRawPointColor CameraRawPointColor::normalized() const {
    CameraRawPointColor r = *this;
    r.hue = clampTo(hue, 0, 360, 0);
    r.saturation = clampTo(saturation, 0, 1, 0);
    r.luminance = clampTo(luminance, 0, 1, 0);
    r.hueShift = clampTo(hueShift, -100, 100, 0);
    r.saturationShift = clampTo(saturationShift, -100, 100, 0);
    r.luminanceShift = clampTo(luminanceShift, -100, 100, 0);
    r.hueRange = clampTo(hueRange, 5, 180, 30);
    r.saturationRange = clampTo(saturationRange, 0.05, 1, 0.4);
    r.luminanceRange = clampTo(luminanceRange, 0.05, 1, 0.4);
    return r;
}

CameraRawMixerSettings CameraRawMixerSettings::normalized() const {
    CameraRawMixerSettings r = *this;
    for (size_t i = 0; i < 8; i++) {
        r.hue[i] = clampTo(hue[i], -100, 100, 0);
        r.saturation[i] = clampTo(saturation[i], -100, 100, 0);
        r.luminance[i] = clampTo(luminance[i], -100, 100, 0);
    }
    r.points.clear();
    for (size_t i = 0; i < std::min<size_t>(8, points.size()); i++) r.points.push_back(points[i].normalized());
    return r;
}

CameraRawGradeWheel CameraRawGradeWheel::normalized() const {
    return {clampTo(hue, 0, 360, 0), clampTo(saturation, 0, 100, 0), clampTo(luminance, -100, 100, 0)};
}

bool CameraRawGradingSettings::adjusts() const {
    for (const auto* w : {&shadows, &midtones, &highlights, &global}) if (w->saturation != 0 || w->luminance != 0) return true;
    return false;
}

CameraRawGradingSettings CameraRawGradingSettings::normalized() const {
    CameraRawGradingSettings r = *this;
    r.shadows = shadows.normalized();
    r.midtones = midtones.normalized();
    r.highlights = highlights.normalized();
    r.global = global.normalized();
    r.blending = clampTo(blending, 0, 100, 50);
    r.balance = clampTo(balance, -100, 100, 0);
    return r;
}

// ---- Detail and Optics --------------------------------------------------------------------------

CameraRawDetailSettings CameraRawDetailSettings::normalized() const {
    CameraRawDetailSettings r = *this;
    r.sharpenAmount = clampTo(sharpenAmount, 0, 150, 0);
    r.sharpenRadius = clampTo(sharpenRadius, 0, 100, 10);
    r.sharpenDetail = clampTo(sharpenDetail, 0, 100, 25);
    r.sharpenMasking = clampTo(sharpenMasking, 0, 100, 0);
    r.noiseLuminance = clampTo(noiseLuminance, 0, 100, 0);
    r.noiseLuminanceDetail = clampTo(noiseLuminanceDetail, 0, 100, 50);
    r.noiseLuminanceContrast = clampTo(noiseLuminanceContrast, 0, 100, 0);
    r.noiseColor = clampTo(noiseColor, 0, 100, 0);
    r.noiseColorDetail = clampTo(noiseColorDetail, 0, 100, 50);
    r.noiseColorSmoothness = clampTo(noiseColorSmoothness, 0, 100, 50);
    return r;
}

bool CameraRawOpticsSettings::adjusts() const {
    return removeChromaticAberration || enableLensProfile || distortion != 0 || purpleAmount != 0 || greenAmount != 0 || vignetteAmount != 0;
}

CameraRawOpticsSettings CameraRawOpticsSettings::normalized() const {
    CameraRawOpticsSettings r = *this;
    r.profileDistortion = clampTo(profileDistortion, 0, 100, 100);
    r.profileVignetting = clampTo(profileVignetting, 0, 100, 100);
    r.distortion = clampTo(distortion, -100, 100, 0);
    r.purpleAmount = clampTo(purpleAmount, 0, 100, 0);
    r.greenAmount = clampTo(greenAmount, 0, 100, 0);
    r.vignetteAmount = clampTo(vignetteAmount, -100, 100, 0);
    r.vignetteMidpoint = clampTo(vignetteMidpoint, 0, 100, 50);
    r.purpleHueLow = clampTo(purpleHueLow, 0, 360, 270);
    r.purpleHueHigh = clampTo(purpleHueHigh, 0, 360, 310);
    r.greenHueLow = clampTo(greenHueLow, 0, 360, 60);
    r.greenHueHigh = clampTo(greenHueHigh, 0, 360, 120);
    if (r.purpleHueLow > r.purpleHueHigh) std::swap(r.purpleHueLow, r.purpleHueHigh);
    if (r.greenHueLow > r.greenHueHigh) std::swap(r.greenHueLow, r.greenHueHigh);
    return r;
}

double CameraRawOpticsSettings::distortionK(double profileStrength) const {
    const double manual = distortion / 100 * profileStrength;
    const double profile = enableLensProfile ? profileDistortion / 100 * profileStrength : 0;
    return manual + profile;
}

// ---- Geometry and Calibration -------------------------------------------------------------------

namespace {
bool guideReadable(const CameraRawGeometryGuide& g) { return std::hypot(g.endX - g.startX, g.endY - g.startY) > 0.01; }
} // namespace

bool CameraRawGeometrySettings::adjusts() const {
    const bool usesGuides = upright == CameraRawUprightMode::Guided && std::any_of(guides.begin(), guides.end(), guideReadable);
    return usesGuides || vertical != 0 || horizontal != 0 || rotate != 0 || aspect != 0 || scale != 0 || offsetX != 0 || offsetY != 0;
}

CameraRawGeometrySettings CameraRawGeometrySettings::normalized() const {
    CameraRawGeometrySettings r = *this;
    r.vertical = clampTo(vertical, -100, 100, 0);
    r.horizontal = clampTo(horizontal, -100, 100, 0);
    r.rotate = clampTo(rotate, -45, 45, 0);
    r.aspect = clampTo(aspect, -100, 100, 0);
    r.scale = clampTo(scale, -100, 100, 0);
    r.offsetX = clampTo(offsetX, -100, 100, 0);
    r.offsetY = clampTo(offsetY, -100, 100, 0);
    r.guides.clear();
    for (const auto& g : guides) if (std::isfinite(g.startX + g.startY + g.endX + g.endY) && guideReadable(g)) r.guides.push_back(g);
    return r;
}

std::array<std::array<double, 2>, 4> CameraRawGeometrySettings::outputCorners(int width, int height) const {
    // Upright > Guided: the first line sets the rotation, a second one a fixed keystone.
    double vert = vertical, horiz = horizontal, rot = rotate;
    if (upright == CameraRawUprightMode::Guided && !guides.empty()) {
        const auto& first = guides.front();
        const double dx = first.endX - first.startX, dy = first.endY - first.startY;
        if (std::hypot(dx, dy) > 1e-4) {
            double guidedRotate = -std::atan2(dy, dx) * 180 / M_PI;
            if (guidedRotate > 45) guidedRotate -= 90; else if (guidedRotate < -45) guidedRotate += 90;
            double guidedVertical = 0, guidedHorizontal = 0;
            if (guides.size() > 1) {
                const auto& second = guides[1];
                const double sx = second.endX - second.startX, sy = second.endY - second.startY;
                if (std::hypot(sx, sy) > 1e-4) {
                    const double a2 = std::atan2(sy, sx) * 180 / M_PI;
                    guidedVertical = std::fabs(a2) > 45 ? (a2 > 0 ? 25 : -25) : 0;
                    guidedHorizontal = std::fabs(a2) <= 45 ? (a2 > 0 ? 25 : -25) : 0;
                }
            }
            vert += guidedVertical; horiz += guidedHorizontal; rot += guidedRotate;
        }
    }
    // Upstream's corners, in Core Image's y-up space.
    const double w = width, h = height;
    const double strength = projection == CameraRawProjection::Perspective ? 1.0 : 0.55;
    const double v = vert / 100 * w * 0.18 * strength;
    const double hz = horiz / 100 * h * 0.18 * strength;
    const double aspectScale = 1 + aspect / 200;
    const double zoom = 1 + scale / 100;
    const double shiftX = offsetX / 100 * w * 0.15, shiftY = offsetY / 100 * h * 0.15;
    std::array<std::array<double, 2>, 4> c{{{-v + shiftX, h + shiftY}, {w + v + shiftX, h + shiftY}, {w + hz + shiftX, -shiftY}, {-hz + shiftX, -shiftY}}};
    const double cx = w / 2 + shiftX, cy = h / 2 + shiftY;
    const double radians = rot * M_PI / 180, cosine = std::cos(radians), sine = std::sin(radians);
    for (auto& p : c) {
        const double dx = p[0] - cx, dy = p[1] - cy;
        p = {cx + dx * cosine - dy * sine, cy + dx * sine + dy * cosine};
        if (aspectScale != 1) p = {cx + (p[0] - cx) * aspectScale, cy + (p[1] - cy) / aspectScale};
        if (zoom != 1) p = {cx + (p[0] - cx) * zoom, cy + (p[1] - cy) * zoom};
        p[1] = h - p[1];   // to the pixel grid's y-down
    }
    return c;
}

Image CameraRawGeometrySettings::apply(const Image& image) const {
    const CameraRawGeometrySettings s = normalized();
    if (!s.adjusts() || image.isEmpty()) return image;
    const int width = image.width(), height = image.height();
    const auto c = s.outputCorners(width, height);
    Corners corners{Point(c[0][0], c[0][1]), Point(c[1][0], c[1][1]), Point(c[2][0], c[2][1]), Point(c[3][0], c[3][1])};
    const Homography inverse = Homography::unitTo(corners).inverted();
    Image out(width, height);
    parallelRows(0, height, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            uint8_t* row = out.row(y);
            for (int x = 0; x < width; x++) {
                const Point unit = inverse.map(Point(x + 0.5, y + 0.5));
                if (!(unit.x >= 0 && unit.x <= 1 && unit.y >= 0 && unit.y <= 1)) continue;
                sampleBilinear(image, unit.x * width, unit.y * height, row + x * 4);
            }
        }
    });
    if (!s.constrainCrop) return out;
    // Constrain Crop: the covered part scaled up, centred, to fill the frame again.
    const PixelBounds b = alphaBounds(out);
    const int cw = b.x1 - b.x0, ch = b.y1 - b.y0;
    if (b.isEmpty() || (cw >= width && ch >= height)) return out;
    const double zoom = std::min(double(width) / cw, double(height) / ch);
    const double left = (width - cw * zoom) / 2, top = (height - ch * zoom) / 2;
    Image fitted(width, height);
    parallelRows(0, height, [&](int y0, int y1) {
        for (int y = y0; y < y1; y++) {
            const double sy = (y + 0.5 - top) / zoom;
            if (sy < 0 || sy > ch) continue;
            uint8_t* row = fitted.row(y);
            for (int x = 0; x < width; x++) {
                const double sx = (x + 0.5 - left) / zoom;
                if (sx < 0 || sx > cw) continue;
                sampleBilinear(out, b.x0 + sx, b.y0 + sy, row + x * 4);
            }
        }
    });
    return fitted;
}

bool CameraRawCalibrationSettings::adjusts() const {
    return shadowTint != 0 || redHue != 0 || redSaturation != 0 || greenHue != 0 || greenSaturation != 0 || blueHue != 0 || blueSaturation != 0;
}

CameraRawCalibrationSettings CameraRawCalibrationSettings::normalized() const {
    CameraRawCalibrationSettings r = *this;
    r.process = std::clamp(process, 1, 6);
    r.shadowTint = clampTo(shadowTint, -100, 100, 0);
    r.redHue = clampTo(redHue, -100, 100, 0);
    r.redSaturation = clampTo(redSaturation, -100, 100, 0);
    r.greenHue = clampTo(greenHue, -100, 100, 0);
    r.greenSaturation = clampTo(greenSaturation, -100, 100, 0);
    r.blueHue = clampTo(blueHue, -100, 100, 0);
    r.blueSaturation = clampTo(blueSaturation, -100, 100, 0);
    return r;
}

// ---- Settings -----------------------------------------------------------------------------------

bool CameraRawSettings::isIdentity() const {
    return !adjustsLight() && !adjustsColor() && !adjustsEffects() && !curve.adjusts() && !mixer.adjusts() && !grading.adjusts()
        && !detail.adjusts() && !optics.adjusts() && !geometry.adjusts() && !calibration.adjusts();
}

bool CameraRawSettings::isValid() const {
    if (!within(exposure, -5, 5)) return false;
    for (double v : {contrast, highlights, shadows, whites, blacks, temperature, tint, vibrance, saturation, texture, clarity, dehaze,
                     glowRange, glowSpread, glowWarmth, vignetteAmount, vignetteRoundness})
        if (!within(v, -100, 100)) return false;
    for (double v : {glow, vignetteMidpoint, vignetteFeather, vignetteHighlights, grainAmount, grainSize, grainRoughness})
        if (!within(v, 0, 100)) return false;
    return true;
}

CameraRawSettings CameraRawSettings::normalized() const {
    CameraRawSettings r = *this;
    r.exposure = clampTo(exposure, -5, 5, 0);
    for (double* v : {&r.contrast, &r.highlights, &r.shadows, &r.whites, &r.blacks, &r.temperature, &r.tint, &r.vibrance, &r.saturation,
                      &r.texture, &r.clarity, &r.dehaze, &r.glowRange, &r.glowSpread, &r.glowWarmth, &r.vignetteAmount, &r.vignetteRoundness})
        *v = clampTo(*v, -100, 100, 0);
    r.glow = clampTo(glow, 0, 100, 0);
    r.vignetteMidpoint = clampTo(vignetteMidpoint, 0, 100, 50);
    r.vignetteFeather = clampTo(vignetteFeather, 0, 100, 50);
    r.vignetteHighlights = clampTo(vignetteHighlights, 0, 100, 0);
    r.grainAmount = clampTo(grainAmount, 0, 100, 0);
    r.grainSize = clampTo(grainSize, 0, 100, 25);
    r.grainRoughness = clampTo(grainRoughness, 0, 100, 50);
    r.curve = curve.normalized();
    r.mixer = mixer.normalized();
    r.grading = grading.normalized();
    r.detail = detail.normalized();
    r.optics = optics.normalized();
    r.geometry = geometry.normalized();
    r.calibration = calibration.normalized();
    return r;
}

CameraRawSettings CameraRawSettings::applying(const CameraRawPanels& panels) const {
    CameraRawSettings r = *this;
    if (!panels.light) r.exposure = r.contrast = r.highlights = r.shadows = r.whites = r.blacks = 0;
    if (!panels.color) r.temperature = r.tint = r.vibrance = r.saturation = 0;
    if (!panels.effects) r.texture = r.clarity = r.dehaze = r.glow = r.vignetteAmount = r.grainAmount = 0;
    if (!panels.curve) r.curve = {};
    if (!panels.mixer) r.mixer = {};
    if (!panels.grading) r.grading = {};
    if (!panels.detail) r.detail = {};
    if (!panels.optics) r.optics = {};
    if (!panels.geometry) r.geometry = {};
    if (!panels.calibration) r.calibration = {};
    return r;
}

std::array<double, 3> CameraRawSettings::gains() const {
    const double warm = temperature / 100, magenta = tint / 100;
    return {1 + temperatureGain * warm + tintRedBlue * magenta, 1 - tintGreen * magenta, 1 - temperatureGain * warm + tintRedBlue * magenta};
}

std::optional<std::array<double, 2>> CameraRawSettings::neutralize(double red, double green, double blue) {
    if (!(red > 1e-4 && green > 1e-4 && blue > 1e-4)) return std::nullopt;
    const double a1 = temperatureGain * red, b1 = tintRedBlue * red + tintGreen * green, c1 = green - red;
    const double a2 = -temperatureGain * blue, b2 = tintRedBlue * blue + tintGreen * green, c2 = green - blue;
    const double determinant = a1 * b2 - a2 * b1;
    if (!(std::fabs(determinant) > 1e-8)) return std::nullopt;
    const double warm = (c1 * b2 - c2 * b1) / determinant, magenta = (a1 * c2 - a2 * c1) / determinant;
    if (!std::isfinite(warm) || !std::isfinite(magenta)) return std::nullopt;
    return std::array<double, 2>{warm * 100, magenta * 100};
}

std::optional<std::array<double, 2>> CameraRawSettings::neutralizeStraight(double red, double green, double blue) {
    return neutralize(decode(red), decode(green), decode(blue));
}

std::optional<std::array<double, 2>> CameraRawSettings::autoBalance(const Image& image) {
    double red = 0, green = 0, blue = 0, count = 0;
    for (int y = 0; y < image.height(); y++) {
        const uint8_t* p = image.row(y);
        for (int x = 0; x < image.width(); x++, p += 4) {
            const double alpha = p[3];
            if (alpha == 0) continue;
            red += decode(std::min(1.0, p[0] / alpha));
            green += decode(std::min(1.0, p[1] / alpha));
            blue += decode(std::min(1.0, p[2] / alpha));
            count += 1;
        }
    }
    if (count <= 0) return std::nullopt;
    return neutralize(red / count, green / count, blue / count);
}

// ---- Apply --------------------------------------------------------------------------------------

bool applyCameraRaw(Image& image, const CameraRawSettings& raw, double scale, uint32_t seed, const CameraRawPreview& preview) {
    const CameraRawSettings s = raw.normalized();
    const bool clipping = preview.clipping != CameraRawClipping::None;
    const bool sharpenMask = preview.sharpenMask;
    const int visualize = preview.visualizePointColor;
    const bool indicators = preview.shadowClipIndicator || preview.highlightClipIndicator;
    if (s.isIdentity() && !clipping && visualize < 0 && !sharpenMask && !indicators) return true;
    if (!s.isValid()) return false;
    if (image.isEmpty()) return true;
    const double pixelScale = scale > 0 && std::isfinite(scale) ? scale : 1;
    const bool paintColor = !clipping && !sharpenMask && (s.curve.adjusts() || s.mixer.adjusts() || s.grading.adjusts() || visualize >= 0);
    const bool paintEffects = !clipping && !sharpenMask && s.adjustsEffects();
    const bool paintDetailOptics = !clipping && (s.detail.adjusts() || s.optics.adjusts() || sharpenMask);
    const size_t width = size_t(image.width()), stride = size_t(image.stride());

    if (!clipping && !sharpenMask && visualize < 0 && s.geometry.adjusts()) image = s.geometry.apply(image);

    if (!clipping && !sharpenMask && s.calibration.adjusts()) {
        const auto& c = s.calibration;
        perRows(image, [&](uint8_t* rows, size_t count) {
            adjust_camera_raw_calibration(rows, width, count, stride, c.shadowTint, c.redHue, c.redSaturation, c.greenHue, c.greenSaturation,
                                          c.blueHue, c.blueSaturation, c.process);
        });
    }
    if (s.adjustsLight() || s.adjustsColor() || clipping) {
        const auto gains = s.gains();
        perRows(image, [&](uint8_t* rows, size_t count) {
            adjust_camera_raw(rows, width, count, stride, gains[0], gains[1], gains[2], s.exposure, s.contrast, s.highlights, s.shadows,
                              s.whites, s.blacks, s.vibrance, s.saturation, int(preview.clipping));
        });
    }
    if (paintColor) {
        const auto luma = s.curve.lumaTable(), red = s.curve.channelTable(s.curve.red), green = s.curve.channelTable(s.curve.green),
                   blue = s.curve.channelTable(s.curve.blue);
        std::vector<float> mixer;
        for (const auto* family : {&s.mixer.hue, &s.mixer.saturation, &s.mixer.luminance}) for (double v : *family) mixer.push_back(float(v / 100));
        std::vector<float> points;
        for (const auto& p : s.mixer.points)
            for (double v : {p.hue / 360, p.saturation, p.luminance, p.hueShift / 100, p.saturationShift / 100, p.luminanceShift / 100,
                             p.hueRange / 360, p.saturationRange, p.luminanceRange})
                points.push_back(float(v));
        points.push_back(0);   // never an empty vector's null data
        std::vector<float> grade;
        for (const auto* w : {&s.grading.shadows, &s.grading.midtones, &s.grading.highlights, &s.grading.global}) {
            grade.push_back(float(w->hue / 360));
            grade.push_back(float(w->saturation / 100));
            grade.push_back(float(w->luminance / 100));
        }
        const int pointCount = int(s.mixer.points.size());
        perRows(image, [&](uint8_t* rows, size_t count) {
            adjust_camera_raw_curve_color(rows, width, count, stride, luma.data(), red.data(), green.data(), blue.data(),
                                          s.curve.refineSaturation / 100, mixer.data(), pointCount, points.data(), grade.data(),
                                          s.grading.blending / 100, s.grading.balance / 100, visualize);
        });
    }
    if (paintEffects) {
        if (s.texture != 0 || s.clarity != 0 || s.dehaze != 0 || s.glow != 0 || s.vignetteAmount != 0)
            adjust_camera_raw_effects(image.data(), width, size_t(image.height()), stride, s.texture, s.clarity, s.dehaze, s.glow,
                                      int(s.glowStyle), s.glowRange, s.glowSpread, s.glowWarmth, s.vignetteAmount, s.vignetteMidpoint,
                                      s.vignetteRoundness, s.vignetteFeather, s.vignetteHighlights, int(s.vignetteStyle), pixelScale);
        if (s.grainAmount > 0) {
            const double unitsPerPixel = 1 / pixelScale, size = s.grainKernelSize();
            parallelRows(0, image.height(), [&](int y0, int y1) {
                for (int y = y0; y < y1; y++)
                    adjust_grain(image.row(y), width, 1, stride, s.grainAmount, size, s.grainRoughness, seed, 0, y * unitsPerPixel, unitsPerPixel);
            });
        }
    }
    if (paintDetailOptics) {
        const auto& d = s.detail;
        const auto& o = s.optics;
        if (sharpenMask) {
            adjust_camera_raw_sharpen_mask_overlay(image.data(), width, size_t(image.height()), stride, d.sharpenRadius, d.sharpenDetail,
                                                   d.sharpenMasking, pixelScale);
            return true;
        }
        if (o.adjusts())
            adjust_camera_raw_optics(image.data(), width, size_t(image.height()), stride, o.removeChromaticAberration ? 1 : 0,
                                     o.enableLensProfile ? 1 : 0, o.profileDistortion, o.profileVignetting, o.distortionK(lensStrength),
                                     o.purpleAmount, o.purpleHueLow, o.purpleHueHigh, o.greenAmount, o.greenHueLow, o.greenHueHigh,
                                     o.vignetteAmount, o.vignetteMidpoint, pixelScale);
        if (d.adjusts())
            adjust_camera_raw_detail(image.data(), width, size_t(image.height()), stride, d.sharpenAmount, d.sharpenRadius, d.sharpenDetail,
                                     d.sharpenMasking, d.noiseLuminance, d.noiseLuminanceDetail, d.noiseLuminanceContrast, d.noiseColor,
                                     d.noiseColorDetail, d.noiseColorSmoothness, pixelScale);
    }
    if (indicators && !clipping && !sharpenMask) {
        perRows(image, [&](uint8_t* rows, size_t count) {
            adjust_camera_raw_clip_overlay(rows, width, count, stride, preview.shadowClipIndicator ? 1 : 0, preview.highlightClipIndicator ? 1 : 0);
        });
    }
    return true;
}

// ---- JSON ---------------------------------------------------------------------------------------

namespace {

/// A strict reader: every key must be known and of the right type.
struct Reader {
    std::string error;
    bool ok() const { return error.empty(); }
    void fail(const std::string& path, const std::string& what) { if (error.empty()) error = path + ": " + what; }

    void number(const json& v, const std::string& path, double& out) {
        if (!v.is_number()) return fail(path, "must be a number");
        out = v.get<double>();
    }
    void boolean(const json& v, const std::string& path, bool& out) {
        if (!v.is_boolean()) return fail(path, "must be true or false");
        out = v.get<bool>();
    }
    template <typename E, size_t N>
    void choice(const json& v, const std::string& path, E& out, const std::array<E, N>& values) {
        if (!v.is_string()) return fail(path, "must be a string");
        const std::string name = v.get<std::string>();
        std::string options;
        for (E e : values) {
            if (name == cameraRawName(e)) { out = e; return; }
            options += (options.empty() ? "" : ", ") + std::string(cameraRawName(e));
        }
        fail(path, "must be one of " + options);
    }
    /// Walks an object's keys through `fields`; an unknown key fails.
    void object(const json& v, const std::string& path, const std::map<std::string, std::function<void(const json&, const std::string&)>>& fields) {
        if (!v.is_object()) return fail(path.empty() ? "settings" : path, "must be an object");
        for (auto it = v.begin(); it != v.end() && ok(); ++it) {
            const std::string key = path.empty() ? it.key() : path + "." + it.key();
            auto field = fields.find(it.key());
            if (field == fields.end()) {
                std::string known;
                for (const auto& f : fields) known += (known.empty() ? "" : ", ") + f.first;
                return fail(key, "unknown key (known: " + known + ")");
            }
            field->second(it.value(), key);
        }
    }
    void curvePoints(const json& v, const std::string& path, std::vector<CameraRawCurvePoint>& out) {
        if (!v.is_array()) return fail(path, "must be an array of {x, y} or [x, y] points on 0..1");
        std::vector<CameraRawCurvePoint> points;
        for (const auto& p : v) {
            CameraRawCurvePoint point;
            if (p.is_array() && p.size() == 2 && p[0].is_number() && p[1].is_number()) point = {p[0].get<double>(), p[1].get<double>()};
            else if (p.is_object()) object(p, path + "[]", {{"x", [&](const json& x, const std::string& k) { number(x, k, point.x); }},
                                                         {"y", [&](const json& y, const std::string& k) { number(y, k, point.y); }}});
            else return fail(path, "points are {x, y} or [x, y]");
            points.push_back(point);
        }
        out = points;
    }
    void family(const json& v, const std::string& path, std::array<double, 8>& out) {
        if (v.is_array()) {
            if (v.size() != 8) return fail(path, "must have 8 numbers (Reds, Oranges, Yellows, Greens, Aquas, Blues, Purples, Magentas)");
            for (size_t i = 0; i < 8; i++) number(v[i], path, out[i]);
            return;
        }
        std::map<std::string, std::function<void(const json&, const std::string&)>> fields;
        for (size_t i = 0; i < 8; i++) {
            std::string name = CameraRawMixerSettings::names[i];
            name[0] = char(std::tolower(static_cast<unsigned char>(name[0])));
            fields[name] = [&, i](const json& x, const std::string& k) { number(x, k, out[i]); };
        }
        object(v, path, fields);
    }
};

#define NUM(obj, name) {#name, [&](const json& v, const std::string& k) { r.number(v, k, obj.name); }}
#define BOOL(obj, name) {#name, [&](const json& v, const std::string& k) { r.boolean(v, k, obj.name); }}

void readWheel(Reader& r, const json& v, const std::string& path, CameraRawGradeWheel& w) {
    r.object(v, path, {NUM(w, hue), NUM(w, saturation), NUM(w, luminance)});
}

json curvePointsJson(const std::vector<CameraRawCurvePoint>& points) {
    json a = json::array();
    for (const auto& p : points) a.push_back({p.x, p.y});
    return a;
}

json familyJson(const std::array<double, 8>& values) {
    json o = json::object();
    for (size_t i = 0; i < 8; i++) {
        std::string name = CameraRawMixerSettings::names[i];
        name[0] = char(std::tolower(static_cast<unsigned char>(name[0])));
        o[name] = values[i];
    }
    return o;
}

json wheelJson(const CameraRawGradeWheel& w) { return {{"hue", w.hue}, {"saturation", w.saturation}, {"luminance", w.luminance}}; }

} // namespace

std::string CameraRawSettings::toJson() const {
    const CameraRawSettings& s = *this;
    json points = json::array();
    for (const auto& p : s.mixer.points)
        points.push_back({{"hue", p.hue}, {"saturation", p.saturation}, {"luminance", p.luminance}, {"hueShift", p.hueShift},
                          {"saturationShift", p.saturationShift}, {"luminanceShift", p.luminanceShift}, {"hueRange", p.hueRange},
                          {"saturationRange", p.saturationRange}, {"luminanceRange", p.luminanceRange}});
    json guides = json::array();
    for (const auto& g : s.geometry.guides) guides.push_back({{"startX", g.startX}, {"startY", g.startY}, {"endX", g.endX}, {"endY", g.endY}});
    json j = {
        {"whiteBalance", cameraRawName(s.whiteBalance)}, {"temperature", s.temperature}, {"tint", s.tint},
        {"exposure", s.exposure}, {"contrast", s.contrast}, {"highlights", s.highlights}, {"shadows", s.shadows},
        {"whites", s.whites}, {"blacks", s.blacks}, {"vibrance", s.vibrance}, {"saturation", s.saturation},
        {"texture", s.texture}, {"clarity", s.clarity}, {"dehaze", s.dehaze},
        {"glow", s.glow}, {"glowStyle", cameraRawName(s.glowStyle)}, {"glowRange", s.glowRange}, {"glowSpread", s.glowSpread}, {"glowWarmth", s.glowWarmth},
        {"vignetteAmount", s.vignetteAmount}, {"vignetteStyle", cameraRawName(s.vignetteStyle)}, {"vignetteMidpoint", s.vignetteMidpoint},
        {"vignetteRoundness", s.vignetteRoundness}, {"vignetteFeather", s.vignetteFeather}, {"vignetteHighlights", s.vignetteHighlights},
        {"grainAmount", s.grainAmount}, {"grainSize", s.grainSize}, {"grainRoughness", s.grainRoughness},
        {"curve", {{"shadows", s.curve.shadows}, {"darks", s.curve.darks}, {"lights", s.curve.lights}, {"highlights", s.curve.highlights},
                   {"shadowSplit", s.curve.shadowSplit}, {"darkSplit", s.curve.darkSplit}, {"lightSplit", s.curve.lightSplit},
                   {"rgb", curvePointsJson(s.curve.rgb)}, {"red", curvePointsJson(s.curve.red)}, {"green", curvePointsJson(s.curve.green)},
                   {"blue", curvePointsJson(s.curve.blue)}, {"refineSaturation", s.curve.refineSaturation}}},
        {"mixer", {{"hue", familyJson(s.mixer.hue)}, {"saturation", familyJson(s.mixer.saturation)}, {"luminance", familyJson(s.mixer.luminance)},
                   {"points", points}}},
        {"grading", {{"shadows", wheelJson(s.grading.shadows)}, {"midtones", wheelJson(s.grading.midtones)}, {"highlights", wheelJson(s.grading.highlights)},
                     {"global", wheelJson(s.grading.global)}, {"blending", s.grading.blending}, {"balance", s.grading.balance}}},
        {"detail", {{"sharpenAmount", s.detail.sharpenAmount}, {"sharpenRadius", s.detail.sharpenRadius}, {"sharpenDetail", s.detail.sharpenDetail},
                    {"sharpenMasking", s.detail.sharpenMasking}, {"noiseLuminance", s.detail.noiseLuminance},
                    {"noiseLuminanceDetail", s.detail.noiseLuminanceDetail}, {"noiseLuminanceContrast", s.detail.noiseLuminanceContrast},
                    {"noiseColor", s.detail.noiseColor}, {"noiseColorDetail", s.detail.noiseColorDetail}, {"noiseColorSmoothness", s.detail.noiseColorSmoothness}}},
        {"optics", {{"removeChromaticAberration", s.optics.removeChromaticAberration}, {"enableLensProfile", s.optics.enableLensProfile},
                    {"profileDistortion", s.optics.profileDistortion}, {"profileVignetting", s.optics.profileVignetting}, {"distortion", s.optics.distortion},
                    {"purpleAmount", s.optics.purpleAmount}, {"purpleHueLow", s.optics.purpleHueLow}, {"purpleHueHigh", s.optics.purpleHueHigh},
                    {"greenAmount", s.optics.greenAmount}, {"greenHueLow", s.optics.greenHueLow}, {"greenHueHigh", s.optics.greenHueHigh},
                    {"vignetteAmount", s.optics.vignetteAmount}, {"vignetteMidpoint", s.optics.vignetteMidpoint}}},
        {"geometry", {{"upright", cameraRawName(s.geometry.upright)}, {"projection", cameraRawName(s.geometry.projection)},
                      {"vertical", s.geometry.vertical}, {"horizontal", s.geometry.horizontal}, {"rotate", s.geometry.rotate},
                      {"aspect", s.geometry.aspect}, {"scale", s.geometry.scale}, {"offsetX", s.geometry.offsetX}, {"offsetY", s.geometry.offsetY},
                      {"constrainCrop", s.geometry.constrainCrop}, {"guides", guides}}},
        {"calibration", {{"process", s.calibration.process}, {"shadowTint", s.calibration.shadowTint}, {"redHue", s.calibration.redHue},
                         {"redSaturation", s.calibration.redSaturation}, {"greenHue", s.calibration.greenHue},
                         {"greenSaturation", s.calibration.greenSaturation}, {"blueHue", s.calibration.blueHue},
                         {"blueSaturation", s.calibration.blueSaturation}}},
    };
    return j.dump();
}

bool CameraRawSettings::parse(const std::string& text, CameraRawSettings& out, std::string* error) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded()) { if (error) *error = "settings are not valid JSON"; return false; }
    CameraRawSettings s = out;
    Reader r;
    auto& c = s.curve;
    auto& m = s.mixer;
    auto& g = s.grading;
    auto& d = s.detail;
    auto& o = s.optics;
    auto& geo = s.geometry;
    auto& cal = s.calibration;
    r.object(j, "", {
        {"whiteBalance", [&](const json& v, const std::string& k) { r.choice(v, k, s.whiteBalance, std::array{CameraRawWhiteBalance::Custom, CameraRawWhiteBalance::Auto}); }},
        NUM(s, temperature), NUM(s, tint), NUM(s, exposure), NUM(s, contrast), NUM(s, highlights), NUM(s, shadows), NUM(s, whites), NUM(s, blacks),
        NUM(s, vibrance), NUM(s, saturation), NUM(s, texture), NUM(s, clarity), NUM(s, dehaze), NUM(s, glow),
        {"glowStyle", [&](const json& v, const std::string& k) { r.choice(v, k, s.glowStyle, std::array{CameraRawGlowStyle::Diffusion, CameraRawGlowStyle::Bloom, CameraRawGlowStyle::Halation}); }},
        NUM(s, glowRange), NUM(s, glowSpread), NUM(s, glowWarmth), NUM(s, vignetteAmount),
        {"vignetteStyle", [&](const json& v, const std::string& k) { r.choice(v, k, s.vignetteStyle, std::array{CameraRawVignetteStyle::HighlightPriority, CameraRawVignetteStyle::ColorPriority, CameraRawVignetteStyle::PaintOverlay}); }},
        NUM(s, vignetteMidpoint), NUM(s, vignetteRoundness), NUM(s, vignetteFeather), NUM(s, vignetteHighlights),
        NUM(s, grainAmount), NUM(s, grainSize), NUM(s, grainRoughness),
        {"curve", [&](const json& v, const std::string& k) {
            r.object(v, k, {NUM(c, shadows), NUM(c, darks), NUM(c, lights), NUM(c, highlights), NUM(c, shadowSplit), NUM(c, darkSplit),
                            NUM(c, lightSplit), NUM(c, refineSaturation),
                            {"rgb", [&](const json& x, const std::string& p) { r.curvePoints(x, p, c.rgb); }},
                            {"red", [&](const json& x, const std::string& p) { r.curvePoints(x, p, c.red); }},
                            {"green", [&](const json& x, const std::string& p) { r.curvePoints(x, p, c.green); }},
                            {"blue", [&](const json& x, const std::string& p) { r.curvePoints(x, p, c.blue); }}});
        }},
        {"mixer", [&](const json& v, const std::string& k) {
            r.object(v, k, {{"hue", [&](const json& x, const std::string& p) { r.family(x, p, m.hue); }},
                            {"saturation", [&](const json& x, const std::string& p) { r.family(x, p, m.saturation); }},
                            {"luminance", [&](const json& x, const std::string& p) { r.family(x, p, m.luminance); }},
                            {"points", [&](const json& x, const std::string& p) {
                                if (!x.is_array()) return r.fail(p, "must be an array of point colors");
                                if (x.size() > 8) return r.fail(p, "at most 8 point colors");
                                m.points.clear();
                                for (const auto& item : x) {
                                    CameraRawPointColor pc;
                                    r.object(item, p + "[]", {NUM(pc, hue), NUM(pc, saturation), NUM(pc, luminance), NUM(pc, hueShift),
                                                              NUM(pc, saturationShift), NUM(pc, luminanceShift), NUM(pc, hueRange),
                                                              NUM(pc, saturationRange), NUM(pc, luminanceRange)});
                                    m.points.push_back(pc);
                                }
                            }}});
        }},
        {"grading", [&](const json& v, const std::string& k) {
            r.object(v, k, {{"shadows", [&](const json& x, const std::string& p) { readWheel(r, x, p, g.shadows); }},
                            {"midtones", [&](const json& x, const std::string& p) { readWheel(r, x, p, g.midtones); }},
                            {"highlights", [&](const json& x, const std::string& p) { readWheel(r, x, p, g.highlights); }},
                            {"global", [&](const json& x, const std::string& p) { readWheel(r, x, p, g.global); }},
                            NUM(g, blending), NUM(g, balance)});
        }},
        {"detail", [&](const json& v, const std::string& k) {
            r.object(v, k, {NUM(d, sharpenAmount), NUM(d, sharpenRadius), NUM(d, sharpenDetail), NUM(d, sharpenMasking), NUM(d, noiseLuminance),
                            NUM(d, noiseLuminanceDetail), NUM(d, noiseLuminanceContrast), NUM(d, noiseColor), NUM(d, noiseColorDetail),
                            NUM(d, noiseColorSmoothness)});
        }},
        {"optics", [&](const json& v, const std::string& k) {
            r.object(v, k, {BOOL(o, removeChromaticAberration), BOOL(o, enableLensProfile), NUM(o, profileDistortion), NUM(o, profileVignetting),
                            NUM(o, distortion), NUM(o, purpleAmount), NUM(o, purpleHueLow), NUM(o, purpleHueHigh), NUM(o, greenAmount),
                            NUM(o, greenHueLow), NUM(o, greenHueHigh), NUM(o, vignetteAmount), NUM(o, vignetteMidpoint)});
        }},
        {"geometry", [&](const json& v, const std::string& k) {
            r.object(v, k, {{"upright", [&](const json& x, const std::string& p) { r.choice(x, p, geo.upright, std::array{CameraRawUprightMode::Off, CameraRawUprightMode::Guided}); }},
                            {"projection", [&](const json& x, const std::string& p) { r.choice(x, p, geo.projection, std::array{CameraRawProjection::Perspective, CameraRawProjection::Rectilinear}); }},
                            NUM(geo, vertical), NUM(geo, horizontal), NUM(geo, rotate), NUM(geo, aspect), NUM(geo, scale), NUM(geo, offsetX),
                            NUM(geo, offsetY), BOOL(geo, constrainCrop),
                            {"guides", [&](const json& x, const std::string& p) {
                                if (!x.is_array()) return r.fail(p, "must be an array of {startX, startY, endX, endY}");
                                geo.guides.clear();
                                for (const auto& item : x) {
                                    CameraRawGeometryGuide guide;
                                    r.object(item, p + "[]", {NUM(guide, startX), NUM(guide, startY), NUM(guide, endX), NUM(guide, endY)});
                                    geo.guides.push_back(guide);
                                }
                            }}});
        }},
        {"calibration", [&](const json& v, const std::string& k) {
            r.object(v, k, {{"process", [&](const json& x, const std::string& p) {
                                if (!x.is_number_integer() || x.get<int>() < 1 || x.get<int>() > 6) return r.fail(p, "must be a process version 1..6");
                                cal.process = x.get<int>();
                            }},
                            NUM(cal, shadowTint), NUM(cal, redHue), NUM(cal, redSaturation), NUM(cal, greenHue), NUM(cal, greenSaturation),
                            NUM(cal, blueHue), NUM(cal, blueSaturation)});
        }},
    });
    if (!r.ok()) { if (error) *error = r.error; return false; }
    out = s;
    return true;
}

#undef NUM
#undef BOOL

} // namespace compositor
